// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraRendererSplats.h"

#include "GaussianSplatCommon.h"
#include "GaussianSplatShared.h"
#include "GaussianSplatViewExtension.h"
#include "NiagaraGaussianSplatBuildSplats.h"
#include "NiagaraSplatRendererProperties.h"

#include "NiagaraDataSet.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraGpuComputeDispatchInterface.h"
#include "NiagaraSceneProxy.h"
#include "NiagaraSystemInstance.h"

#include "ColorManagement/ColorSpace.h"
#include "Engine/World.h"
#include "GlobalShader.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogNiagaraSplatRenderer, Log, All);

// The conversion pass is added from the proxy collection broadcast, which runs before the splat pipeline opens its own
// scope, so without a stat of its own the cost lands in the unaccounted remainder rather than against this renderer.
DECLARE_GPU_STAT(NiagaraSplatRenderer);

CSV_DEFINE_CATEGORY(NiagaraSplatRenderer, false);

namespace NiagaraRendererSplatsLocal
{
	static bool GRendererEnabled = true;
	static FAutoConsoleVariableRef CVarRendererEnabled(
		TEXT("fx.Niagara.SplatRenderer.Enabled"),
		GRendererEnabled,
		TEXT("If == 0, Niagara Splat Renderers are disabled."),
		ECVF_Default
	);

	static int32 GMaxSplatsPerEmitter = 4 * 1024 * 1024;
	static FAutoConsoleVariableRef CVarMaxSplatsPerEmitter(
		TEXT("fx.Niagara.SplatRenderer.MaxSplatsPerEmitter"),
		GMaxSplatsPerEmitter,
		TEXT("Upper bound on the splats a single Niagara Splat Renderer will emit in a frame (default = 4194304)."),
		ECVF_RenderThreadSafe
	);

	static int32 GLogSubmissions = 0;
	static FAutoConsoleVariableRef CVarLogSubmissions(
		TEXT("fx.Niagara.SplatRenderer.LogSubmissions"),
		GLogSubmissions,
		TEXT("Logs the next N submissions, one per Niagara Splat Renderer per view: how many splats were handed to the Gaussian Splat pipeline, or why none were."),
		ECVF_RenderThreadSafe
	);

	// Emitted splats carry a single spherical harmonic band.
	constexpr uint32 SplatStride = FGaussianSplatShared::EFloat_Stride;

	// Unused slots are written with a zero scale, which SG_DecomposeCovariance rejects only while the dilation term stays under its sqrt(0.1) radius floor.
	static_assert(GAUSSIAN_SPLAT_COVARIANCE_DILATION < 0.31622776f,
		"Zero scale splats are no longer rejected by the shading pass; WriteInactiveSplat in NiagaraGaussianSplatBuildSplats.usf needs a new way to mark unused slots.");

	/** Uploads a CPU simulation attributes as an SRV. */
	FRHIShaderResourceView* UploadParticleBuffer(FRDGBuilder& GraphBuilder, TConstArrayView<uint8> Data, uint32 BytesPerElement, EPixelFormat Format, FRHIShaderResourceView* Fallback)
	{
		if (Data.Num() == 0)
		{
			return Fallback;
		}

		FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(BytesPerElement, Data.Num() / BytesPerElement), TEXT("NiagaraSplatRenderer.CpuParticles"));
		GraphBuilder.QueueBufferUpload(Buffer, Data.GetData(), Data.Num());
		const TRefCountPtr<FRDGPooledBuffer>& PooledBuffer = GraphBuilder.ConvertToExternalBuffer(Buffer);
		GraphBuilder.UseExternalAccessMode(Buffer, ERHIAccess::SRVCompute);
		return PooledBuffer->GetOrCreateSRV(GraphBuilder.RHICmdList, FRHIBufferSRVCreateInfo(Format));
	}

	/** Encodes a binding as NiagaraParticleAccess.ush expects it: the component offset into the float, half (top bit set) or int buffer. */
	int32 AttributeRegister(const FNiagaraDataSet& ParticleData, const FNiagaraVariableAttributeBinding& Binding)
	{
		const FNiagaraVariableLayoutInfo* Layout = Binding.IsParticleBinding() ? ParticleData.GetVariableLayout(Binding.GetDataSetBindableVariable()) : nullptr;
		if (Layout == nullptr)
		{
			return INDEX_NONE;
		}
		if (Layout->GetNumHalfComponents() > 0)
		{
			return int32(Layout->GetHalfComponentStart() | 0x80000000u);
		}
		return Layout->GetNumFloatComponents() > 0 ? Layout->GetFloatComponentStart() : Layout->GetInt32ComponentStart();
	}

	/** Logs one submission and consumes one of the LogSubmissions budget, or does nothing once it is spent. */
	void LogSubmission(const FString& DebugName, const FString& Message)
	{
		if (GLogSubmissions > 0)
		{
			--GLogSubmissions;
			UE_LOGF(LogNiagaraSplatRenderer, Display, "%ls %ls", *DebugName, *Message);
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////

FNiagaraRendererSplats::FNiagaraRendererSplats(ERHIFeatureLevel::Type FeatureLevel, const UNiagaraRendererProperties* InProps, const FNiagaraEmitterInstance* Emitter)
	: FNiagaraRenderer(FeatureLevel, InProps, Emitter)
{
	using namespace NiagaraRendererSplatsLocal;

	const UNiagaraSplatRendererProperties* Properties = CastChecked<const UNiagaraSplatRendererProperties>(InProps);
	const FNiagaraDataSet& ParticleData = Emitter->GetParticleData();

	PositionRegister = AttributeRegister(ParticleData, Properties->PositionBinding);
	ScaleRegister = AttributeRegister(ParticleData, Properties->ScaleBinding);
	RotationRegister = AttributeRegister(ParticleData, Properties->RotationBinding);
	ColorRegister = AttributeRegister(ParticleData, Properties->ColorBinding);
	VisibilityTagRegister = AttributeRegister(ParticleData, Properties->RendererVisibilityTagBinding);
	RendererVisibility = Properties->RendererVisibility;

	FNiagaraSystemInstance* SystemInstance = Emitter->GetParentSystemInstance();
	DebugName = FString::Printf(TEXT("%s.%s"), SystemInstance && SystemInstance->GetSystem() ? *SystemInstance->GetSystem()->GetName() : TEXT("?"), *Emitter->GetEmitterHandle().GetName().ToString());

	if (SystemInstance)
	{
		GaussianSplat::RegisterExternalProxySource(SystemInstance->GetWorld(), FOnCollectGaussianSplatProxies::FDelegate::CreateSPLambda(Registration,
			[this](FRDGBuilder& GraphBuilder, const FSceneView&, TArray<const FGaussianSplatRenderProxy*>& OutProxies)
			{
				CollectSplatProxies_RenderThread(GraphBuilder, OutProxies);
			}));
	}
}

FNiagaraRendererSplats::~FNiagaraRendererSplats()
{
	check(IsInRenderingThread() || IsInParallelRenderingThread());
}

void FNiagaraRendererSplats::ReleaseRenderThreadResources()
{
	FNiagaraRenderer::ReleaseRenderThreadResources();
	SplatBuffer = nullptr;
	SplatBufferCapacity = 0;
}

FPrimitiveViewRelevance FNiagaraRendererSplats::GetViewRelevance(const FSceneView* View, const FNiagaraSceneProxy* SceneProxy) const
{
	// Draw relevance is claimed so the primitive takes part in normal visibility bookkeeping.
	// NOTE: There are no mesh batches
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = HasDynamicData() && SceneProxy && SceneProxy->IsShown(View);
	Result.bDynamicRelevance = true;
	return Result;
}

FNiagaraDynamicDataBase* FNiagaraRendererSplats::GenerateDynamicData(const FNiagaraSceneProxy* Proxy, const UNiagaraRendererProperties* InProperties, const FNiagaraEmitterInstance* Emitter) const
{
	using namespace NiagaraRendererSplatsLocal;

	const UNiagaraSplatRendererProperties* Properties = CastChecked<const UNiagaraSplatRendererProperties>(InProperties);
	FNiagaraSystemInstance* SystemInstance = Emitter->GetParentSystemInstance();
	USceneComponent* OwnerComponent = SystemInstance ? SystemInstance->GetAttachComponent() : nullptr;
	const UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(OwnerComponent);

	// A hidden component does not have a proxy
	if (!GRendererEnabled || OwnerComponent == nullptr || Emitter->GetParticleData().GetCurrentData() == nullptr
		|| !IsRendererEnabled(InProperties, Emitter) || (PrimitiveComponent && !PrimitiveComponent->ShouldRender()))
	{
		return nullptr;
	}

	FNiagaraDynamicDataSplats* DynamicData = new FNiagaraDynamicDataSplats(Emitter);
	DynamicData->ComputeDispatchInterface = Proxy ? Proxy->GetComputeDispatchInterface() : nullptr;

	// Splat positions stay in simulation space.  The pipeline applies LocalToWorld.
	const FTransform SimToWorld = UseLocalSpace(Proxy) ? OwnerComponent->GetComponentTransform() : SystemInstance->GetLWCSimToWorld(false);
	FGaussianSplatRenderProxy& SplatProxy = DynamicData->Proxy;
	SplatProxy.LocalToWorld = FMatrix44f(SimToWorld.ToMatrixWithScale());
	SplatProxy.WorldToLocal = SplatProxy.LocalToWorld.Inverse();
	const FBox EmitterBounds = Emitter->GetBounds();
	SplatProxy.LocalBounds = FBox3f(UseLocalSpace(Proxy) ? EmitterBounds : EmitterBounds.TransformBy(SystemInstance->GetWorldTransform()));
	SplatProxy.PointScaleFactor = Properties->SplatScale;
	SplatProxy.OpacityScale = Properties->OpacityScale;
	SplatProxy.InverseToneCurveStrength = Properties->bInvertToneCurve ? Properties->InverseToneCurveStrength : 0.0f;
	SplatProxy.bInvertExpandGamutLUT = Properties->bInvertExpandGamut;

	const FNiagaraParameterStore& Parameters = Emitter->GetRendererBoundVariables();
	DynamicData->DefaultPosition = Parameters.GetParameterValueOrDefault(Properties->PositionBinding.GetParamMapBindableVariable(), FNiagaraPosition(FVector3f::ZeroVector));
	DynamicData->DefaultScale = Parameters.GetParameterValueOrDefault(Properties->ScaleBinding.GetParamMapBindableVariable(), FVector3f::OneVector);
	DynamicData->DefaultRotation = Parameters.GetParameterValueOrDefault(Properties->RotationBinding.GetParamMapBindableVariable(), FQuat4f::Identity);
	DynamicData->DefaultColor = Parameters.GetParameterValueOrDefault(Properties->ColorBinding.GetParamMapBindableVariable(), FLinearColor::White);
	DynamicData->DefaultVisibilityTag = Parameters.GetParameterValueOrDefault(Properties->RendererVisibilityTagBinding.GetParamMapBindableVariable(), Properties->RendererVisibility);
	return DynamicData;
}

void FNiagaraRendererSplats::AddBuildSplatsPass(FRDGBuilder& GraphBuilder, const FNiagaraDynamicDataSplats& DynamicData, FNiagaraDataBuffer& ParticleData, uint32 NumSplats)
{
	using namespace NiagaraRendererSplatsLocal;

	// Grow in chunks and never shrink.
	const uint32 RequiredCapacity = Align(NumSplats, 1024u);
	if (!SplatBuffer.IsValid() || SplatBufferCapacity < RequiredCapacity)
	{
		SplatBuffer = AllocatePooledBuffer(FRDGBufferDesc::CreateByteAddressDesc(RequiredCapacity * SplatStride), TEXT("NiagaraSplatRenderer.Splats"));
		SplatBufferCapacity = RequiredCapacity;
	}

	FNiagaraBuildGaussianSplatsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FNiagaraBuildGaussianSplatsCS::FParameters>();
	Parameters->ParticleCountBuffer = GetDummyUIntBuffer();
	Parameters->ParticleCountOffset = INDEX_NONE;
	Parameters->ParticleCount = ParticleData.GetNumInstances();

	if (GetSimTarget() == ENiagaraSimTarget::GPUComputeSim)
	{
		// The live count only exists on the GPU, in Niagara's shared instance count buffer.
		Parameters->NiagaraParticleDataFloat = GetSrvOrDefaultFloat(ParticleData.GetGPUBufferFloat());
		Parameters->NiagaraParticleDataHalf = GetSrvOrDefaultHalf(ParticleData.GetGPUBufferHalf());
		Parameters->NiagaraParticleDataInt = GetSrvOrDefaultInt(ParticleData.GetGPUBufferInt());
		if (DynamicData.ComputeDispatchInterface && ParticleData.GetGPUInstanceCountBufferOffset() != INDEX_NONE)
		{
			Parameters->ParticleCountBuffer = GetSrvOrDefaultUInt(DynamicData.ComputeDispatchInterface->GetGPUInstanceCounterManager().GetInstanceCountBuffer());
			Parameters->ParticleCountOffset = ParticleData.GetGPUInstanceCountBufferOffset();
		}
	}
	else
	{
		Parameters->NiagaraParticleDataFloat = UploadParticleBuffer(GraphBuilder, ParticleData.GetFloatBuffer(), sizeof(float), PF_R32_FLOAT, GetDummyFloatBuffer());
		Parameters->NiagaraParticleDataHalf = UploadParticleBuffer(GraphBuilder, ParticleData.GetHalfBuffer(), sizeof(FFloat16), PF_R16F, GetDummyHalfBuffer());
		Parameters->NiagaraParticleDataInt = UploadParticleBuffer(GraphBuilder, ParticleData.GetInt32Buffer(), sizeof(int32), PF_R32_SINT, GetDummyIntBuffer());
	}
	Parameters->NiagaraFloatDataStride = ParticleData.GetFloatStride() / sizeof(float);
	Parameters->NiagaraIntDataStride = ParticleData.GetInt32Stride() / sizeof(int32);

	Parameters->PositionRegister = PositionRegister;
	Parameters->ScaleRegister = ScaleRegister;
	Parameters->RotationRegister = RotationRegister;
	Parameters->ColorRegister = ColorRegister;
	Parameters->VisibilityTagRegister = VisibilityTagRegister;
	Parameters->DefaultPosition = DynamicData.DefaultPosition;
	Parameters->DefaultScale = DynamicData.DefaultScale;
	Parameters->DefaultRotation = FVector4f(DynamicData.DefaultRotation.X, DynamicData.DefaultRotation.Y, DynamicData.DefaultRotation.Z, DynamicData.DefaultRotation.W);
	Parameters->DefaultColor = FVector4f(DynamicData.DefaultColor);
	Parameters->DefaultVisibilityTag = DynamicData.DefaultVisibilityTag;
	Parameters->RendererVisibility = RendererVisibility;

	Parameters->MaxSplats = NumSplats;
	Parameters->SplatStride = SplatStride;
	Parameters->WorkingColorSpaceToSRGB = FMatrix44f(UE::Color::FColorSpaceTransform::GetWorkingColorSpaceToSRGB());
	Parameters->RWSplatBuffer = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(SplatBuffer));

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("NiagaraSplatRenderer.BuildSplats(%u)", NumSplats),
		ERDGPassFlags::Compute,
		TShaderMapRef<FNiagaraBuildGaussianSplatsCS>(GetGlobalShaderMap(GMaxRHIShaderPlatform)),
		Parameters,
		FComputeShaderUtils::GetGroupCountWrapped(NumSplats, FNiagaraBuildGaussianSplatsCS::ThreadGroupSize));
}

void FNiagaraRendererSplats::CollectSplatProxies_RenderThread(FRDGBuilder& GraphBuilder, TArray<const FGaussianSplatRenderProxy*>& OutProxies)
{
	using namespace NiagaraRendererSplatsLocal;

	FNiagaraDynamicDataSplats* DynamicData = static_cast<FNiagaraDynamicDataSplats*>(GetDynamicData());
	FNiagaraDataBuffer* ParticleData = DynamicData ? DynamicData->GetParticleDataToRender(GraphBuilder.RHICmdList, DynamicData->IsGpuLowLatencyTranslucencyEnabled()) : nullptr;
	if (ParticleData == nullptr)
	{
		LogSubmission(DebugName, TEXT("submitted no splats: nothing to render this frame."));
		return;
	}

	// A GPU simulation count only exists on the GPU, so the size of the allocation is used instead. The shader will trim it later.
	const bool bGpuSim = GetSimTarget() == ENiagaraSimTarget::GPUComputeSim;
	const uint32 NumSplats = FMath::Min(bGpuSim ? ParticleData->GetNumInstancesAllocated() : ParticleData->GetNumInstances(), uint32(FMath::Max(GMaxSplatsPerEmitter, 0)));
	if (NumSplats == 0)
	{
		LogSubmission(DebugName, TEXT("submitted no splats: the emitter has no particles."));
		return;
	}

	if (LastBuiltFrameCounter != GFrameCounterRenderThread)
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, NiagaraSplatRenderer, "NiagaraSplatRenderer");
		AddBuildSplatsPass(GraphBuilder, *DynamicData, *ParticleData, NumSplats);
		LastBuiltFrameCounter = GFrameCounterRenderThread;
	}

	FGaussianSplatRenderProxy& SplatProxy = DynamicData->Proxy;
	SplatProxy.NumSplats = NumSplats;
	SplatProxy.GpuBuffer = SplatBuffer;
	SplatProxy.GpuDataStride = SplatStride;
	OutProxies.Add(&SplatProxy);

	CSV_CUSTOM_STAT(NiagaraSplatRenderer, SplatsSubmitted, int32(NumSplats), ECsvCustomStatOp::Accumulate);
	CSV_CUSTOM_STAT(NiagaraSplatRenderer, Submissions, 1, ECsvCustomStatOp::Accumulate);

	LogSubmission(DebugName, FString::Printf(TEXT("submitted %u splats (%ls sim)."), NumSplats, bGpuSim ? TEXT("GPU") : TEXT("CPU")));
}
