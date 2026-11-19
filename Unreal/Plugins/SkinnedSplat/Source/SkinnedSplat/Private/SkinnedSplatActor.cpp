#include "SkinnedSplatActor.h"
#include "SkinnedSplatData.h"
#include "SkinnedSplatShader.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/BillboardComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialShared.h"
#include "ReferenceSkeleton.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "UObject/ConstructorHelpers.h"

#include "GaussianSplatRenderPipeline.h"
#include "GaussianSplatViewExtension.h"

DEFINE_LOG_CATEGORY_STATIC(LogSkinnedSplat, Log, All);

IMPLEMENT_GLOBAL_SHADER(FSkinnedSplatCS, "/Plugin/SkinnedSplat/Private/SkinnedSplat.usf", "MainCS", SF_Compute);
IMPLEMENT_MATERIAL_SHADER_TYPE(, FSkinnedSplatMaterialCS, TEXT("/Plugin/SkinnedSplat/Private/SkinnedSplatMaterial.usf"), TEXT("MainCS"), SF_Compute);

static constexpr uint32 SplatStride = 56; // FGaussianSplatShared::EFloat_Stride
static constexpr uint32 RestStride = 5;   // float4 per splat in the rest buffer

// ------------------------------------------------------------------------------------------------
// Render-thread side: owns the GPU buffers and the proxy handed to the splat renderer.
class FSkinnedSplatRenderState : public TSharedFromThis<FSkinnedSplatRenderState, ESPMode::ThreadSafe>
{
public:
	uint32 NumSplats = 0;
	TArray<FVector4f> RestCPU; // consumed by the first collect

	TRefCountPtr<FRDGPooledBuffer> RestBuffer;
	TRefCountPtr<FRDGPooledBuffer> SplatBuffer;
	TArray<FVector4f> BoneDQ;
	float Brightness = 1.0f;
	const FMaterialRenderProxy* MaterialProxy = nullptr;
	uint64 LastBuiltFrame = MAX_uint64;
	bool bHasPose = false;
	FBox3f SourceBounds = FBox3f(ForceInit); // actor-space bounds of the splats
	FGaussianSplatRenderProxy Proxy;

	void Collect(FRDGBuilder& GraphBuilder, const FSceneView& View, TArray<const FGaussianSplatRenderProxy*>& OutProxies)
	{
		if (!bHasPose || NumSplats == 0 || !Proxy.bVisible)
		{
			return;
		}
		// skin (and shade) once per frame; every view of the frame reuses the buffer
		if (LastBuiltFrame != GFrameCounterRenderThread)
		{
			LastBuiltFrame = GFrameCounterRenderThread;
			Build(GraphBuilder, View);
		}
		Proxy.NumSplats = NumSplats;
		Proxy.GpuBuffer = SplatBuffer;
		Proxy.GpuDataStride = SplatStride;
		Proxy.BufferFormat = 0;
		Proxy.SHDegree = 0;
		OutProxies.Add(&Proxy);
	}

private:
	void Build(FRDGBuilder& GraphBuilder, const FSceneView& View)
	{
		FRDGBufferRef Rest;
		if (!RestBuffer.IsValid())
		{
			Rest = CreateStructuredBuffer(GraphBuilder, TEXT("SkinnedSplat.Rest"), sizeof(FVector4f), RestCPU.Num(),
				RestCPU.GetData(), RestCPU.Num() * sizeof(FVector4f));
			RestBuffer = GraphBuilder.ConvertToExternalBuffer(Rest);
			RestCPU.Empty();
		}
		else
		{
			Rest = GraphBuilder.RegisterExternalBuffer(RestBuffer);
		}
		if (!SplatBuffer.IsValid())
		{
			SplatBuffer = AllocatePooledBuffer(FRDGBufferDesc::CreateByteAddressDesc(NumSplats * SplatStride), TEXT("SkinnedSplat.Splats"));
		}
		FRDGBufferRef Splats = GraphBuilder.RegisterExternalBuffer(SplatBuffer);
		FRDGBufferRef Bones = CreateStructuredBuffer(GraphBuilder, TEXT("SkinnedSplat.BoneDQ"), sizeof(FVector4f), BoneDQ.Num(),
			BoneDQ.GetData(), BoneDQ.Num() * sizeof(FVector4f));

		FSkinnedSplatCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSkinnedSplatCS::FParameters>();
		Parameters->RestSplats = GraphBuilder.CreateSRV(Rest);
		Parameters->BoneDQ = GraphBuilder.CreateSRV(Bones);
		Parameters->NumSplats = NumSplats;
		Parameters->SplatStride = SplatStride;
		Parameters->Brightness = Brightness;
		Parameters->RWSplatBuffer = GraphBuilder.CreateUAV(Splats);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SkinnedSplat(%u)", NumSplats), ERDGPassFlags::Compute,
			TShaderMapRef<FSkinnedSplatCS>(GetGlobalShaderMap(View.GetShaderPlatform())), Parameters,
			FComputeShaderUtils::GetGroupCountWrapped(NumSplats, FSkinnedSplatCS::ThreadGroupSize));

		AddMaterialPass(GraphBuilder, View, Splats);
	}

	void AddMaterialPass(FRDGBuilder& GraphBuilder, const FSceneView& View, FRDGBufferRef Splats)
	{
		if (!MaterialProxy)
		{
			return;
		}
		const ERHIFeatureLevel::Type FeatureLevel = View.GetFeatureLevel();
		const FMaterial* Material = MaterialProxy->GetMaterialNoFallback(FeatureLevel);
		if (!Material || Material->GetMaterialDomain() != MD_Surface || !Material->GetRenderingThreadShaderMap())
		{
			return;
		}
		FMaterialShaderTypes ShaderTypes;
		ShaderTypes.AddShaderType<FSkinnedSplatMaterialCS>();
		FMaterialShaders Shaders;
		TShaderRef<FSkinnedSplatMaterialCS> Shader;
		if (!Material->TryGetShaders(ShaderTypes, nullptr, Shaders) || !Shaders.TryGetComputeShader(Shader))
		{
			return; // still compiling, or the material lacks the usage flag: keep the splat colors
		}
		MaterialProxy->UpdateUniformExpressionCacheIfNeeded(FeatureLevel);

		FSkinnedSplatMaterialCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSkinnedSplatMaterialCS::FParameters>();
		Parameters->View = View.ViewUniformBuffer;
		Parameters->LocalToWorld = Proxy.LocalToWorld;
		Parameters->WorldToLocal = Proxy.WorldToLocal;
		Parameters->LocalBoundsOrigin = SourceBounds.GetCenter();
		Parameters->LocalBoundsExtent = SourceBounds.GetExtent();
		Parameters->NumSplats = NumSplats;
		Parameters->SplatStride = SplatStride;
		Parameters->RWSplatBuffer = GraphBuilder.CreateUAV(Splats);
		const FIntVector Groups = FComputeShaderUtils::GetGroupCountWrapped(NumSplats, FSkinnedSplatMaterialCS::ThreadGroupSize);
		const FMaterialRenderProxy* Proxy_ = MaterialProxy;
		GraphBuilder.AddPass(RDG_EVENT_NAME("SkinnedSplatMaterial(%u)", NumSplats), Parameters, ERDGPassFlags::Compute,
			[Parameters, Shader, Proxy_, Material, &View, Groups](FRHIComputeCommandList& RHICmdList)
			{
				FRHIComputeShader* ShaderRHI = Shader.GetComputeShader();
				SetComputePipelineState(RHICmdList, ShaderRHI);
				SetShaderParameters(RHICmdList, Shader, ShaderRHI, *Parameters);
				Shader->SetParameters(RHICmdList, ShaderRHI, Proxy_, *Material, View);
				RHICmdList.DispatchComputeShader(Groups.X, Groups.Y, Groups.Z);
				UnsetShaderUAVs(RHICmdList, Shader, ShaderRHI);
			});
	}
};

// ------------------------------------------------------------------------------------------------
namespace SkinnedSplatMath
{
	/** Least-squares similarity Y ~= S * R * X + T (Umeyama). Returns false if the fit is degenerate or mirrored. */
	static bool FitSimilarity(const TArray<FVector3d>& X, const TArray<FVector3d>& Y, FTransform& Out, double& OutRms)
	{
		const int32 N = X.Num();
		FVector3d MX(0), MY(0);
		for (int32 i = 0; i < N; ++i) { MX += X[i]; MY += Y[i]; }
		MX /= N; MY /= N;
		// cross covariance -> rotation via the quaternion eigen method (Horn), robust and mirror-free
		double S[3][3] = {}; double VarX = 0;
		for (int32 i = 0; i < N; ++i)
		{
			const FVector3d A = X[i] - MX, B = Y[i] - MY;
			for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) S[r][c] += A[r] * B[c];
			VarX += A.SizeSquared();
		}
		const double Sxx = S[0][0], Sxy = S[0][1], Sxz = S[0][2], Syx = S[1][0], Syy = S[1][1], Syz = S[1][2], Szx = S[2][0], Szy = S[2][1], Szz = S[2][2];
		const double K[4][4] = {
			{ Sxx + Syy + Szz, Syz - Szy, Szx - Sxz, Sxy - Syx },
			{ Syz - Szy, Sxx - Syy - Szz, Sxy + Syx, Szx + Sxz },
			{ Szx - Sxz, Sxy + Syx, -Sxx + Syy - Szz, Syz + Szy },
			{ Sxy - Syx, Szx + Sxz, Syz + Szy, -Sxx - Syy + Szz } };
		// power iteration on K + Shift * I for the largest eigenvector (generic start, not orthogonal to the answer)
		double Q[4] = { 0.73, 0.38, 0.46, 0.33 };
		const double Shift = 4.0 * (FMath::Abs(Sxx) + FMath::Abs(Syy) + FMath::Abs(Szz) + FMath::Abs(Sxy) + FMath::Abs(Sxz) + FMath::Abs(Syx) + FMath::Abs(Syz) + FMath::Abs(Szx) + FMath::Abs(Szy)) + 1e-9;
		for (int32 It = 0; It < 500; ++It)
		{
			double R[4];
			for (int r = 0; r < 4; ++r) { R[r] = Shift * Q[r]; for (int c = 0; c < 4; ++c) R[r] += K[r][c] * Q[c]; }
			const double L = FMath::Sqrt(R[0] * R[0] + R[1] * R[1] + R[2] * R[2] + R[3] * R[3]);
			for (int r = 0; r < 4; ++r) Q[r] = R[r] / L;
		}
		const FQuat Rot = FQuat(Q[1], Q[2], Q[3], Q[0]).GetNormalized();
		double Dot = 0;
		for (int32 i = 0; i < N; ++i) Dot += FVector3d::DotProduct(Rot.RotateVector(X[i] - MX), Y[i] - MY);
		const double Scale = VarX > 0 ? Dot / VarX : 1.0;
		const FVector3d T = MY - Rot.RotateVector(MX) * Scale;
		Out = FTransform(Rot, T, FVector3d(Scale));
		double Err = 0;
		for (int32 i = 0; i < N; ++i) Err += FVector3d::DistSquared(Out.TransformPosition(X[i]), Y[i]);
		OutRms = FMath::Sqrt(Err / N);
		return Scale > 0;
	}
}

// ------------------------------------------------------------------------------------------------
ASkinnedSplatActor::ASkinnedSplatActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork; // after the mesh has finished its pose

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Root);
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetVisibility(false);
	Mesh->bIsEditorOnly = false;
	// Only drives the splats, which cast the shadow themselves (GaussianSplat's splat shadow map, see bCastShadow)
	Mesh->SetCastShadow(false);
	Mesh->bAffectDistanceFieldLighting = false;
	Mesh->bAffectDynamicIndirectLighting = false;
	Mesh->bVisibleInRayTracing = false;

	BoundsBox = CreateDefaultSubobject<UBoxComponent>(TEXT("Bounds"));
	BoundsBox->SetupAttachment(Root);
	BoundsBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BoundsBox->SetHiddenInGame(true);
	BoundsBox->bDrawOnlyIfSelected = true;
	BoundsBox->SetCanEverAffectNavigation(false);
	BoundsBox->ShapeColor = FColor(90, 180, 255);
	BoundsBox->InitBoxExtent(FVector(40, 40, 90));

#if WITH_EDITORONLY_DATA
	Sprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
	if (Sprite)
	{
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> Icon(TEXT("/Engine/EditorResources/S_Actor"));
		Sprite->Sprite = Icon.Get();
		Sprite->SetupAttachment(Root);
		Sprite->SetRelativeLocation(FVector(0, 0, 100));
		Sprite->bIsScreenSizeScaled = true;
	}
#endif
}

void ASkinnedSplatActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	SetupMesh();
	BuildRenderState();
}

void ASkinnedSplatActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	SetupMesh();
	BuildRenderState();
}

void ASkinnedSplatActor::PostUnregisterAllComponents()
{
	ReleaseRenderState();
	Super::PostUnregisterAllComponents();
}

void ASkinnedSplatActor::BeginPlay()
{
	Super::BeginPlay();
	AddTickPrerequisiteComponent(Mesh);
	SetupMesh();
	BuildRenderState();
}

#if WITH_EDITOR
void ASkinnedSplatActor::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	if (Event.GetPropertyName() == GET_MEMBER_NAME_CHECKED(ASkinnedSplatActor, Material))
	{
		EnsureMaterialUsage(Material);
	}
	SetupMesh();
	BuildRenderState();
}
#endif

void ASkinnedSplatActor::EnsureMaterialUsage(UMaterialInterface* InMaterial)
{
	// the material compute shader is only compiled for materials with this usage (see FSkinnedSplatMaterialCS)
	if (InMaterial && !InMaterial->CheckMaterialUsage_Concurrent(MATUSAGE_LidarPointCloud))
	{
		UE_LOG(LogSkinnedSplat, Warning, TEXT("%s: enable 'Used with Lidar Point Cloud' on material %s to use it on splats"),
			*GetNameSafe(InMaterial), *GetNameSafe(InMaterial->GetMaterial()));
	}
}

void ASkinnedSplatActor::SetMaterial(UMaterialInterface* NewMaterial)
{
	Material = NewMaterial;
	EnsureMaterialUsage(Material);
}

void ASkinnedSplatActor::PlayClip(UAnimSequence* Clip, bool bLoop)
{
	if (Clip && Mesh)
	{
		Mesh->PlayAnimation(Clip, bLoop);
		Mesh->SetPlayRate(PlayRate);
	}
}

void ASkinnedSplatActor::SetupMesh()
{
	if (!Mesh || !SplatData)
	{
		return;
	}
	if (SplatData->Mesh && Mesh->GetSkeletalMeshAsset() != SplatData->Mesh)
	{
		Mesh->SetSkeletalMeshAsset(SplatData->Mesh);
	}
	Mesh->SetVisibility(bShowBones);
	Mesh->SetHiddenInGame(!bShowBones);
	UAnimSequence* Clip = Animation ? Animation.Get() : SplatData->DefaultAnimation.Get();
	const UWorld* World = GetWorld();
	const bool bGame = World && World->IsGameWorld();
	if (Mesh->GetAnimationMode() == EAnimationMode::AnimationBlueprint && Mesh->GetAnimClass())
	{
		return; // user drives the mesh with an anim blueprint
	}
	Mesh->AnimationData.AnimToPlay = Clip;
	Mesh->AnimationData.bSavedLooping = true;
	Mesh->AnimationData.bSavedPlaying = bPlayAnimation;
	Mesh->AnimationData.SavedPlayRate = PlayRate;
#if WITH_EDITOR
	Mesh->SetUpdateAnimationInEditor(!bGame && bAnimateInEditor && bPlayAnimation);
#endif
	if (Clip && Mesh->GetSkeletalMeshAsset() && (bGame || bAnimateInEditor))
	{
		if (Mesh->GetAnimationMode() != EAnimationMode::AnimationSingleNode || Mesh->GetSingleNodeInstance() == nullptr
			|| Mesh->GetSingleNodeInstance()->GetAnimationAsset() != Clip)
		{
			Mesh->PlayAnimation(Clip, true);
		}
		Mesh->SetPlayRate(PlayRate);
		if (!bPlayAnimation) Mesh->Stop();
	}
}

void ASkinnedSplatActor::BuildRenderState()
{
	using namespace SkinnedSplatMath;
	UWorld* World = GetWorld();
	if (!World || World->IsNetMode(NM_DedicatedServer) || !SplatData || SplatData->NumSplats <= 0 || SplatData->Rest.Num() != SplatData->NumSplats * int32(RestStride))
	{
		ReleaseRenderState();
		return;
	}
	const USkinnedAsset* Skinned = Mesh ? Mesh->GetSkinnedAsset() : nullptr;
	if (RenderState.IsValid() && BuiltGuid == SplatData->DataGuid && BuiltMesh.Get() == Skinned)
	{
		return;
	}
	ReleaseRenderState();

	// bones: match by name, fit splat space -> mesh component space from the capture-pose joints
	const int32 NumBones = SplatData->BoneNames.Num();
	MeshBoneIndex.Init(INDEX_NONE, NumBones);
	RefInverse.Init(FTransform::Identity, NumBones);
	SplatToMesh = FTransform::Identity;
	if (Skinned)
	{
		const FReferenceSkeleton& RefSkel = Skinned->GetRefSkeleton();
		const TArray<FTransform>& Local = RefSkel.GetRefBonePose();
		TArray<FTransform> RefCS; RefCS.SetNum(Local.Num());
		for (int32 i = 0; i < Local.Num(); ++i)
		{
			const int32 P = RefSkel.GetParentIndex(i);
			RefCS[i] = P == INDEX_NONE ? Local[i] : Local[i] * RefCS[P];
		}
		TArray<FVector3d> X, Y;
		for (int32 b = 0; b < NumBones; ++b)
		{
			const int32 M = RefSkel.FindBoneIndex(SplatData->BoneNames[b]);
			MeshBoneIndex[b] = M;
			if (M == INDEX_NONE) continue;
			RefInverse[b] = RefCS[M].Inverse();
			X.Add(FVector3d(SplatData->BoneRefPositions[b])); Y.Add(RefCS[M].GetLocation());
		}
		double Rms = 0;
		if (X.Num() >= 3 && FitSimilarity(X, Y, SplatToMesh, Rms))
		{
			UE_LOG(LogSkinnedSplat, Display, TEXT("%s: %d/%d bones matched, splat->mesh %s scale %.3f, joint rms %.3f cm"),
				*GetName(), X.Num(), NumBones, *SplatToMesh.GetRotation().Rotator().ToString(), SplatToMesh.GetScale3D().X, Rms);
			if (Rms > 1.0) UE_LOG(LogSkinnedSplat, Warning, TEXT("%s: skeleton and splat joints disagree (rms %.1f cm), wrong mesh?"), *GetName(), Rms);
		}
		else
		{
			UE_LOG(LogSkinnedSplat, Warning, TEXT("%s: only %d bones match the mesh, splats will not deform"), *GetName(), X.Num());
		}
		Mesh->SetRelativeTransform(SplatToMesh.Inverse()); // line the proxy skeleton up with the splats
	}
	BoneDQ.SetNumZeroed(NumBones * 2);
	for (int32 b = 0; b < NumBones; ++b) BoneDQ[b * 2] = FVector4f(0, 0, 0, 1);

	const FBox3f DataBounds(SplatData->Bounds);
	LocalBounds = DataBounds.ExpandBy(DataBounds.GetSize().GetMax() * 0.35f); // room for the animation
	if (BoundsBox)
	{
		BoundsBox->SetRelativeLocation(FVector(DataBounds.GetCenter()));
		BoundsBox->SetBoxExtent(FVector(DataBounds.GetExtent()));
	}

	TSharedPtr<FSkinnedSplatRenderState, ESPMode::ThreadSafe> State = MakeShared<FSkinnedSplatRenderState, ESPMode::ThreadSafe>();
	State->NumSplats = SplatData->NumSplats;
	State->RestCPU = SplatData->Rest;
	RenderState = State;
	BuiltGuid = SplatData->DataGuid;
	BuiltMesh = const_cast<USkinnedAsset*>(Skinned);
	GaussianSplat::RegisterExternalProxySource(World, FOnCollectGaussianSplatProxies::FDelegate::CreateSP(State.ToSharedRef(), &FSkinnedSplatRenderState::Collect));
	if (Material) EnsureMaterialUsage(Material);
}

void ASkinnedSplatActor::ReleaseRenderState()
{
	if (RenderState.IsValid())
	{
		// the delegate only holds a weak reference; let the last strong one die on the render thread
		ENQUEUE_RENDER_COMMAND(SkinnedSplatRelease)([State = MoveTemp(RenderState)](FRHICommandListImmediate&) mutable { State.Reset(); });
	}
	RenderState.Reset();
	BuiltGuid.Invalidate();
	BuiltMesh.Reset();
}

void ASkinnedSplatActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!RenderState.IsValid())
	{
		return;
	}
	const TArray<FTransform>& CS = Mesh->GetComponentSpaceTransforms();
	const FTransform MeshToSplat = SplatToMesh.Inverse();
	for (int32 b = 0; b < MeshBoneIndex.Num(); ++b)
	{
		const int32 M = MeshBoneIndex[b];
		if (M == INDEX_NONE || !CS.IsValidIndex(M)) continue;
		// capture pose -> current pose, in splat space: into mesh space, undo the ref pose, apply the current one, back
		const FTransform K = SplatToMesh * RefInverse[b] * CS[M] * MeshToSplat;
		FQuat Q = K.GetRotation(); Q.Normalize();
		const FVector T = K.GetTranslation();
		const FQuat Dual = FQuat(T.X, T.Y, T.Z, 0.0) * Q * 0.5;
		BoneDQ[b * 2] = FVector4f(Q.X, Q.Y, Q.Z, Q.W);
		BoneDQ[b * 2 + 1] = FVector4f(Dual.X, Dual.Y, Dual.Z, Dual.W);
	}

	bool bVisible = !IsHidden();
#if WITH_EDITOR
	if (!GetWorld()->IsGameWorld()) bVisible = !IsHiddenEd() && !IsTemporarilyHiddenInEditor();
#endif
	const FMatrix44f LocalToWorld(Root->GetComponentTransform().ToMatrixWithScale());
	const FMaterialRenderProxy* MaterialProxy = Material ? Material->GetRenderProxy() : nullptr;
	ENQUEUE_RENDER_COMMAND(SkinnedSplatUpdate)(
		[State = RenderState, DQ = BoneDQ, LocalToWorld, Bounds = LocalBounds, Point = PointScaleFactor, Opacity = OpacityScale,
		 Bright = Brightness, MaterialProxy, ToneCurve = bInvertToneCurve ? InverseToneCurveStrength : 0.0f, bVisible, bShadow = bCastShadow, Shadow = ShadowStrength](FRHICommandListImmediate&)
		{
			State->BoneDQ = DQ;
			State->Brightness = Bright;
			State->MaterialProxy = MaterialProxy;
			State->Proxy.LocalToWorld = LocalToWorld;
			State->Proxy.WorldToLocal = LocalToWorld.Inverse();
			State->SourceBounds = Bounds;
			State->Proxy.LocalBounds = Bounds;
			State->Proxy.PointScaleFactor = Point;
			State->Proxy.OpacityScale = Opacity;
			State->Proxy.InverseToneCurveStrength = ToneCurve;
			State->Proxy.bVisible = bVisible;
			State->Proxy.bCastShadow = bShadow;
			State->Proxy.ShadowStrength = Shadow;
			State->bHasPose = true;
		});
}
