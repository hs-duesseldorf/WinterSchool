#include "SkinnedSplatActor.h"
#include "SkinnedSplatData.h"
#include "SkinnedSplatShader.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/World.h"
#include "ReferenceSkeleton.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

#include "GaussianSplatRenderPipeline.h"
#include "GaussianSplatViewExtension.h"

DEFINE_LOG_CATEGORY_STATIC(LogSkinnedSplat, Log, All);

IMPLEMENT_GLOBAL_SHADER(FSkinnedSplatCS, "/Plugin/SkinnedSplat/Private/SkinnedSplat.usf", "MainCS", SF_Compute);

static constexpr uint32 SplatStride = 56; // FGaussianSplatShared::EFloat_Stride
static constexpr uint32 RestStride = 5;   // float4 per splat in the rest buffer

// ------------------------------------------------------------------------------------------------
// Render-thread side: owns the GPU buffers and the proxy handed to the splat renderer.
class FSkinnedSplatRenderState : public TSharedFromThis<FSkinnedSplatRenderState, ESPMode::ThreadSafe>
{
public:
	uint32 NumSplats = 0;
	TArray<FVector4f> RestCPU;           // consumed by the first collect

	TRefCountPtr<FRDGPooledBuffer> RestBuffer;
	TRefCountPtr<FRDGPooledBuffer> SplatBuffer;
	TArray<FVector4f> BoneDQ;
	uint64 LastBuiltFrame = MAX_uint64;
	bool bHasPose = false;
	FGaussianSplatRenderProxy Proxy;

	void Collect(FRDGBuilder& GraphBuilder, TArray<const FGaussianSplatRenderProxy*>& OutProxies)
	{
		if (!bHasPose || NumSplats == 0 || !Proxy.bVisible)
		{
			return;
		}
		if (LastBuiltFrame != GFrameCounterRenderThread)
		{
			LastBuiltFrame = GFrameCounterRenderThread;
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
			FRDGBufferRef Bones = CreateStructuredBuffer(GraphBuilder, TEXT("SkinnedSplat.BoneDQ"), sizeof(FVector4f), BoneDQ.Num(),
				BoneDQ.GetData(), BoneDQ.Num() * sizeof(FVector4f));

			FSkinnedSplatCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSkinnedSplatCS::FParameters>();
			Parameters->RestSplats = GraphBuilder.CreateSRV(Rest);
			Parameters->BoneDQ = GraphBuilder.CreateSRV(Bones);
			Parameters->NumSplats = NumSplats;
			Parameters->SplatStride = SplatStride;
			Parameters->RWSplatBuffer = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(SplatBuffer));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SkinnedSplat(%u)", NumSplats), ERDGPassFlags::Compute,
				TShaderMapRef<FSkinnedSplatCS>(GetGlobalShaderMap(GMaxRHIShaderPlatform)), Parameters,
				FComputeShaderUtils::GetGroupCountWrapped(NumSplats, FSkinnedSplatCS::ThreadGroupSize));
		}
		Proxy.NumSplats = NumSplats;
		Proxy.GpuBuffer = SplatBuffer;
		Proxy.GpuDataStride = SplatStride;
		Proxy.BufferFormat = 0;
		Proxy.SHDegree = 0;
		OutProxies.Add(&Proxy);
	}
};

// ------------------------------------------------------------------------------------------------
// small column-vector 3x3 helpers (the calibration may contain a mirror: Blender is right-handed, UE left-handed)
namespace SkinnedSplatMath
{
	struct FM3 { double M[3][3] = {}; };
	static FM3 Mul(const FM3& A, const FM3& B)
	{
		FM3 R;
		for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) R.M[i][j] += A.M[i][k] * B.M[k][j];
		return R;
	}
	static FM3 Transpose(const FM3& A) { FM3 R; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) R.M[i][j] = A.M[j][i]; return R; }
	static double Det(const FM3& A)
	{
		return A.M[0][0] * (A.M[1][1] * A.M[2][2] - A.M[1][2] * A.M[2][1])
			 - A.M[0][1] * (A.M[1][0] * A.M[2][2] - A.M[1][2] * A.M[2][0])
			 + A.M[0][2] * (A.M[1][0] * A.M[2][1] - A.M[1][1] * A.M[2][0]);
	}
	static FM3 Inverse(const FM3& A)
	{
		const double D = Det(A); FM3 R;
		R.M[0][0] = (A.M[1][1] * A.M[2][2] - A.M[1][2] * A.M[2][1]) / D;
		R.M[0][1] = (A.M[0][2] * A.M[2][1] - A.M[0][1] * A.M[2][2]) / D;
		R.M[0][2] = (A.M[0][1] * A.M[1][2] - A.M[0][2] * A.M[1][1]) / D;
		R.M[1][0] = (A.M[1][2] * A.M[2][0] - A.M[1][0] * A.M[2][2]) / D;
		R.M[1][1] = (A.M[0][0] * A.M[2][2] - A.M[0][2] * A.M[2][0]) / D;
		R.M[1][2] = (A.M[0][2] * A.M[1][0] - A.M[0][0] * A.M[1][2]) / D;
		R.M[2][0] = (A.M[1][0] * A.M[2][1] - A.M[1][1] * A.M[2][0]) / D;
		R.M[2][1] = (A.M[0][1] * A.M[2][0] - A.M[0][0] * A.M[2][1]) / D;
		R.M[2][2] = (A.M[0][0] * A.M[1][1] - A.M[0][1] * A.M[1][0]) / D;
		return R;
	}
	static FVector3d Apply(const FM3& A, const FVector3d& V)
	{
		return FVector3d(A.M[0][0] * V.X + A.M[0][1] * V.Y + A.M[0][2] * V.Z,
						 A.M[1][0] * V.X + A.M[1][1] * V.Y + A.M[1][2] * V.Z,
						 A.M[2][0] * V.X + A.M[2][1] * V.Y + A.M[2][2] * V.Z);
	}
	// standard (column-vector) rotation matrix of a unit quaternion
	static FM3 FromQuat(double X, double Y, double Z, double W)
	{
		FM3 R;
		R.M[0][0] = 1 - 2 * (Y * Y + Z * Z); R.M[0][1] = 2 * (X * Y - W * Z);     R.M[0][2] = 2 * (X * Z + W * Y);
		R.M[1][0] = 2 * (X * Y + W * Z);     R.M[1][1] = 1 - 2 * (X * X + Z * Z); R.M[1][2] = 2 * (Y * Z - W * X);
		R.M[2][0] = 2 * (X * Z - W * Y);     R.M[2][1] = 2 * (Y * Z + W * X);     R.M[2][2] = 1 - 2 * (X * X + Y * Y);
		return R;
	}
	static FQuat4f ToQuat(const FM3& R)
	{
		const double T = R.M[0][0] + R.M[1][1] + R.M[2][2];
		double X, Y, Z, W;
		if (T > 0) { const double S = FMath::Sqrt(T + 1.0) * 2; W = 0.25 * S; X = (R.M[2][1] - R.M[1][2]) / S; Y = (R.M[0][2] - R.M[2][0]) / S; Z = (R.M[1][0] - R.M[0][1]) / S; }
		else if (R.M[0][0] > R.M[1][1] && R.M[0][0] > R.M[2][2]) { const double S = FMath::Sqrt(1.0 + R.M[0][0] - R.M[1][1] - R.M[2][2]) * 2; W = (R.M[2][1] - R.M[1][2]) / S; X = 0.25 * S; Y = (R.M[0][1] + R.M[1][0]) / S; Z = (R.M[0][2] + R.M[2][0]) / S; }
		else if (R.M[1][1] > R.M[2][2]) { const double S = FMath::Sqrt(1.0 + R.M[1][1] - R.M[0][0] - R.M[2][2]) * 2; W = (R.M[0][2] - R.M[2][0]) / S; X = (R.M[0][1] + R.M[1][0]) / S; Y = 0.25 * S; Z = (R.M[1][2] + R.M[2][1]) / S; }
		else { const double S = FMath::Sqrt(1.0 + R.M[2][2] - R.M[0][0] - R.M[1][1]) * 2; W = (R.M[1][0] - R.M[0][1]) / S; X = (R.M[0][2] + R.M[2][0]) / S; Y = (R.M[1][2] + R.M[2][1]) / S; Z = 0.25 * S; }
		FQuat4f Q((float)X, (float)Y, (float)Z, (float)W);
		Q.Normalize();
		return Q;
	}
}

// ------------------------------------------------------------------------------------------------
ASkinnedSplatActor::ASkinnedSplatActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork; // after the mesh has finished its pose
	Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
}

void ASkinnedSplatActor::BeginPlay()
{
	Super::BeginPlay();
	Mesh->SetHiddenInGame(!bShowBones);
	if (Animation)
	{
		Mesh->PlayAnimation(Animation, true);
		Mesh->SetPlayRate(PlayRate);
	}
	AddTickPrerequisiteComponent(Mesh);
	if (!Initialize())
	{
		SetActorTickEnabled(false);
	}
}

bool ASkinnedSplatActor::Initialize()
{
	using namespace SkinnedSplatMath;
	if (!SplatData || SplatData->Raw.Num() < 20 || !Mesh->GetSkinnedAsset())
	{
		UE_LOG(LogSkinnedSplat, Error, TEXT("%s: missing SplatData or skeletal mesh"), *GetName());
		return false;
	}
	const uint8* Ptr = SplatData->Raw.GetData();
	const uint8* End = Ptr + SplatData->Raw.Num();
	auto Read = [&](void* Out, int64 Bytes) { check(Ptr + Bytes <= End); FMemory::Memcpy(Out, Ptr, Bytes); Ptr += Bytes; };
	char Magic[4]; uint32 Version, NumSplats, NumBones, NamesLen;
	Read(Magic, 4); Read(&Version, 4); Read(&NumSplats, 4); Read(&NumBones, 4); Read(&NamesLen, 4);
	if (FMemory::Memcmp(Magic, "SKSP", 4) != 0 || Version != 1 || NumBones > 255)
	{
		UE_LOG(LogSkinnedSplat, Error, TEXT("%s: bad .skinsplat header"), *GetName());
		return false;
	}
	TArray<FString> Names;
	{
		check(Ptr + NamesLen <= End);
		const ANSICHAR* C = reinterpret_cast<const ANSICHAR*>(Ptr);
		for (uint32 i = 0; i < NamesLen;) { const int32 L = FCStringAnsi::Strlen(C + i); if (L) Names.Add(FString(L, C + i)); i += L + 1; }
	}
	Ptr += NamesLen;
	TArray<int32> Parents; Parents.SetNumUninitialized(NumBones); Read(Parents.GetData(), NumBones * 4);
	TArray<FVector3f> Joints; Joints.SetNumUninitialized(NumBones); Read(Joints.GetData(), NumBones * 12);
	const int64 RecordBytes = 14 * 4 + 4 + 16;
	if (Names.Num() != (int32)NumBones || Ptr + RecordBytes * NumSplats != End)
	{
		UE_LOG(LogSkinnedSplat, Error, TEXT("%s: .skinsplat size mismatch (%d names, %u bones)"), *GetName(), Names.Num(), NumBones);
		return false;
	}

	// bone mapping and reference pose in component space
	const FReferenceSkeleton& RefSkel = Mesh->GetSkinnedAsset()->GetRefSkeleton();
	const TArray<FTransform>& Local = RefSkel.GetRefBonePose();
	TArray<FTransform> RefCS; RefCS.SetNum(Local.Num());
	for (int32 i = 0; i < Local.Num(); ++i)
	{
		const int32 P = RefSkel.GetParentIndex(i);
		RefCS[i] = P == INDEX_NONE ? Local[i] : Local[i] * RefCS[P];
	}
	MeshBoneIndex.SetNum(NumBones); RefComponentSpace.SetNum(NumBones);
	TArray<FVector3d> X, Y;
	for (uint32 b = 0; b < NumBones; ++b)
	{
		MeshBoneIndex[b] = RefSkel.FindBoneIndex(FName(*Names[b]));
		if (MeshBoneIndex[b] == INDEX_NONE)
		{
			UE_LOG(LogSkinnedSplat, Warning, TEXT("bone %s not in skeleton, its splats stay at rest"), *Names[b]);
			RefComponentSpace[b] = FTransform::Identity;
			continue;
		}
		RefComponentSpace[b] = RefCS[MeshBoneIndex[b]];
		X.Add(FVector3d(Joints[b])); Y.Add(RefComponentSpace[b].GetLocation());
	}
	if (X.Num() < 4) { UE_LOG(LogSkinnedSplat, Error, TEXT("too few matching bones (%d)"), X.Num()); return false; }

	// calibration: data space (Blender, meters) -> mesh component space, least-squares affine then nearest similarity
	FVector3d MX(0), MY(0);
	for (int32 i = 0; i < X.Num(); ++i) { MX += X[i]; MY += Y[i]; }
	MX /= X.Num(); MY /= X.Num();
	FM3 SYX, SXX;
	for (int32 i = 0; i < X.Num(); ++i)
	{
		const FVector3d A = X[i] - MX, B = Y[i] - MY;
		for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) { SYX.M[r][c] += B[r] * A[c]; SXX.M[r][c] += A[r] * A[c]; }
	}
	const FM3 Affine = Mul(SYX, Inverse(SXX));
	const double DetA = Det(Affine);
	const double Scale = FMath::Pow(FMath::Abs(DetA), 1.0 / 3.0);
	FM3 Rot = Affine;
	for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) Rot.M[r][c] /= Scale;
	for (int It = 0; It < 20; ++It) // Higham polar iteration -> nearest orthogonal (keeps a mirror if there is one)
	{
		const FM3 InvT = Transpose(Inverse(Rot));
		for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) Rot.M[r][c] = 0.5 * (Rot.M[r][c] + InvT.M[r][c]);
	}
	const bool bMirror = Det(Rot) < 0;
	const FVector3d Offset = MY - Apply(Rot, MX) * Scale;
	double Err = 0, MaxErr = 0;
	for (int32 i = 0; i < X.Num(); ++i) { const double E = FVector3d::Dist(Apply(Rot, X[i]) * Scale + Offset, Y[i]); Err += E * E; MaxErr = FMath::Max(MaxErr, E); }
	UE_LOG(LogSkinnedSplat, Display, TEXT("%s: %u splats, %u bones (%d matched), calibration scale %.3f mirror %d, joint fit rms %.3f max %.3f"),
		*GetName(), NumSplats, NumBones, X.Num(), Scale, bMirror ? 1 : 0, FMath::Sqrt(Err / X.Num()), MaxErr);

	// rest splats -> component space, packed for the GPU
	TSharedPtr<FSkinnedSplatRenderState, ESPMode::ThreadSafe> State = MakeShared<FSkinnedSplatRenderState, ESPMode::ThreadSafe>();
	State->NumSplats = NumSplats;
	State->RestCPU.SetNumUninitialized(NumSplats * RestStride);
	RestBounds = FBox3f(ForceInit);
	constexpr double SH_C0 = 0.28209479177387814;
	constexpr double ColorScale = 0.75; // splats are unlit; the lit, auto-exposed scene made them look too bright
	for (uint32 i = 0; i < NumSplats; ++i)
	{
		float F[14]; uint8 Idx[4]; float W[4];
		Read(F, 56); Read(Idx, 4); Read(W, 16);
		const FVector3f P = FVector3f(Apply(Rot, FVector3d(F[0], F[1], F[2])) * Scale + Offset);
		FM3 R = Mul(Rot, FromQuat(F[4], F[5], F[6], F[3])); // file stores wxyz
		if (bMirror) { for (int r = 0; r < 3; ++r) R.M[r][0] = -R.M[r][0]; } // flip one local axis: same covariance, proper rotation
		const FQuat4f Q = ToQuat(R);
		uint32 Packed = 0;
		for (int k = 0; k < 4; ++k) Packed |= uint32(FMath::Min<uint32>(Idx[k], NumBones - 1)) << (8 * k);
		FVector4f* O = &State->RestCPU[i * RestStride];
		O[0] = FVector4f(P, F[10]);
		O[1] = FVector4f(Q.X, Q.Y, Q.Z, Q.W);
		O[2] = FVector4f(FMath::Abs(F[7]) * Scale, FMath::Abs(F[8]) * Scale, FMath::Abs(F[9]) * Scale, *reinterpret_cast<const float*>(&Packed));
		O[3] = FVector4f((F[11] * ColorScale - 0.5) / SH_C0, (F[12] * ColorScale - 0.5) / SH_C0, (F[13] * ColorScale - 0.5) / SH_C0, 0.0f);
		O[4] = FVector4f(W[0], W[1], W[2], W[3]);
		RestBounds += P;
	}
	// Upright: the FBX import can leave the mesh component space tilted, and the splats follow the bones. Turn the actor so
	// the data's up (+Z) is world up and its front (-Y) faces where the actor's +Y pointed (setup spawns +Y towards the
	// player), then put the lowest splats (0.5 %, ignores floaters) on the floor below.
	{
		const FVector UpCS = FVector(Apply(Rot, FVector3d(0, 0, 1))).GetSafeNormal();
		const FVector FrontCS = FVector(Apply(Rot, FVector3d(0, -1, 0))).GetSafeNormal();
		FVector Facing = GetActorRightVector(); Facing.Z = 0; Facing = Facing.GetSafeNormal(UE_SMALL_NUMBER, FVector::XAxisVector);
		const FQuat NewRot = FQuat(FRotationMatrix::MakeFromXZ(Facing, FVector::UpVector)) * FQuat(FRotationMatrix::MakeFromXZ(FrontCS, UpCS)).Inverse();
		const FVector MeshScale = Mesh->GetRelativeScale3D();
		TArray<float> Zs; Zs.Reserve(NumSplats);
		for (uint32 i = 0; i < NumSplats; ++i) Zs.Add(NewRot.RotateVector(FVector(State->RestCPU[i * RestStride]) * MeshScale).Z);
		Zs.Sort();
		const float FeetZ = Zs[FMath::Min<int32>(Zs.Num() - 1, Zs.Num() / 200)];
		FVector Loc = GetActorLocation();
		FHitResult Hit; FCollisionQueryParams QP(NAME_None, false, this);
		if (GetWorld()->LineTraceSingleByChannel(Hit, Loc + FVector(0, 0, 200), Loc - FVector(0, 0, 1000), ECC_Visibility, QP)) Loc.Z = Hit.ImpactPoint.Z;
		Loc.Z -= FeetZ;
		Mesh->SetMobility(EComponentMobility::Movable);
		SetActorLocationAndRotation(Loc, NewRot);
		UE_LOG(LogSkinnedSplat, Display, TEXT("%s: upright to %s at %s (floor hit %d)"), *GetName(), *NewRot.Rotator().ToString(), *Loc.ToString(), Hit.bBlockingHit ? 1 : 0);
	}
	RestBounds = RestBounds.ExpandBy(RestBounds.GetSize().GetMax() * 0.25f);
	BoneDQ.SetNumZeroed(NumBones * 2);
	for (uint32 b = 0; b < NumBones; ++b) BoneDQ[b * 2] = FVector4f(0, 0, 0, 1);

	RenderState = State;
	GaussianSplat::RegisterExternalProxySource(GetWorld(), FOnCollectGaussianSplatProxies::FDelegate::CreateSPLambda(State.ToSharedRef(),
		[S = State.Get()](FRDGBuilder& GraphBuilder, const FSceneView&, TArray<const FGaussianSplatRenderProxy*>& OutProxies)
		{
			S->Collect(GraphBuilder, OutProxies);
		}));
	return true;
}

void ASkinnedSplatActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!RenderState.IsValid())
	{
		return;
	}
	const TArray<FTransform>& CS = Mesh->GetComponentSpaceTransforms();
	for (int32 b = 0; b < MeshBoneIndex.Num(); ++b)
	{
		const int32 M = MeshBoneIndex[b];
		if (M == INDEX_NONE || !CS.IsValidIndex(M)) continue;
		// rest -> animated in component space: undo the reference pose, then apply the current pose
		const FTransform K = RefComponentSpace[b].Inverse() * CS[M];
		FQuat Q = K.GetRotation(); Q.Normalize();
		const FVector T = K.GetTranslation();
		const FQuat Dual = FQuat(T.X, T.Y, T.Z, 0.0) * Q * 0.5;
		BoneDQ[b * 2] = FVector4f(Q.X, Q.Y, Q.Z, Q.W);
		BoneDQ[b * 2 + 1] = FVector4f(Dual.X, Dual.Y, Dual.Z, Dual.W);
	}
	const FMatrix44f LocalToWorld(Mesh->GetComponentTransform().ToMatrixWithScale());
	ENQUEUE_RENDER_COMMAND(SkinnedSplatUpdate)(
		[State = RenderState, DQ = BoneDQ, LocalToWorld, Bounds = RestBounds, Point = PointScaleFactor, Opacity = OpacityScale,
		 bVisible = !IsHidden()](FRHICommandListImmediate&)
		{
			State->BoneDQ = DQ;
			State->Proxy.LocalToWorld = LocalToWorld;
			State->Proxy.WorldToLocal = LocalToWorld.Inverse();
			State->Proxy.LocalBounds = Bounds;
			State->Proxy.PointScaleFactor = Point;
			State->Proxy.OpacityScale = Opacity;
			State->Proxy.bVisible = bVisible;
			State->bHasPose = true;
		});
}

void ASkinnedSplatActor::EndPlay(const EEndPlayReason::Type Reason)
{
	if (RenderState.IsValid())
	{
		// the delegate only holds a weak reference; let the last strong one die on the render thread
		ENQUEUE_RENDER_COMMAND(SkinnedSplatRelease)([State = MoveTemp(RenderState)](FRHICommandListImmediate&) mutable { State.Reset(); });
	}
	Super::EndPlay(Reason);
}
