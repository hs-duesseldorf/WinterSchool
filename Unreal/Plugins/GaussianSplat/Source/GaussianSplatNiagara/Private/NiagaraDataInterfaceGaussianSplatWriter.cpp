// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraDataInterfaceGaussianSplatWriter.h"

#include "GaussianSplat.h"
#include "GaussianSplatCommon.h"
#include "GaussianSplatDefinitions.h"
#include "GaussianSplatRenderPipeline.h"
#include "GaussianSplatRenderResource.h"
#include "GaussianSplatShared.h"
#include "GaussianSplatViewExtension.h"

#include "NiagaraCompileHashVisitor.h"
#include "NiagaraComponent.h"
#include "NiagaraDataInterfaceGaussianSplatReader.h"
#include "NiagaraDataInterfaceUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraGpuComputeDispatchInterface.h"
#include "NiagaraScript.h"
#include "NiagaraShaderParametersBuilder.h"
#include "NiagaraSimStageData.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"

#include "ColorManagement/ColorSpace.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(NiagaraDataInterfaceGaussianSplatWriter)

#define LOCTEXT_NAMESPACE "NiagaraDataInterfaceGaussianSplatWriter"

DEFINE_LOG_CATEGORY_STATIC(LogNiagaraGaussianSplatWriter, Log, All);

// Unwritten splats are all zero in both formats: a float scale of zero, or the quantized scale floor a zero code decodes to, is
// rejected by the shading pass only while the covariance dilation keeps the projected radius under its sqrt(0.1) floor.
static_assert(GAUSSIAN_SPLAT_COVARIANCE_DILATION < 0.31622776f, "Zero scale splats are no longer rejected by the shading pass; the Gaussian Splat Writer can no longer clear its splats to zero.");

namespace NDIGaussianSplatWriterLocal
{
	BEGIN_SHADER_PARAMETER_STRUCT(FShaderParameters, )
		SHADER_PARAMETER(int32,									NumSplats)
		SHADER_PARAMETER(uint32,								OutputFormat)
		SHADER_PARAMETER(uint32,								OutputStride)
		SHADER_PARAMETER(uint32,								SHDimension)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer,	SplatData)

		SHADER_PARAMETER(FVector3f,								PositionScale)
		SHADER_PARAMETER(FVector3f,								PositionBias)
		SHADER_PARAMETER(FVector3f,								ScaleScale)
		SHADER_PARAMETER(FVector3f,								ScaleBias)
		SHADER_PARAMETER(FVector4f,								ColorScale)
		SHADER_PARAMETER(FVector4f,								ColorBias)
		SHADER_PARAMETER(FVector3f,								SHScale)
		SHADER_PARAMETER(FVector3f,								SHBias)

		SHADER_PARAMETER(FMatrix44f,							WorkingColorSpaceToSRGB)
	END_SHADER_PARAMETER_STRUCT()

	const TCHAR* TemplateShaderFilePath = TEXT("/Plugin/GaussianSplat/Private/NiagaraDataInterfaceGaussianSplatWriterTemplate.ush");

	static const FName NAME_GetNumSplats(TEXT("GetNumSplats"));
	static const FName NAME_GetSHDegree(TEXT("GetSHDegree"));
	static const FName NAME_SetSplat(TEXT("SetSplat"));
	static const FName NAME_SetBaseData(TEXT("SetBaseData"));
	static const FName NAME_SetColor(TEXT("SetColor"));
	static const FName NAME_SetOpacity(TEXT("SetOpacity"));
	static const FName NAME_SetSHCoefficient(TEXT("SetSHCoefficient"));
	static const FName NAME_Clear(TEXT("Clear"));

	static bool GEnabled = true;
	static FAutoConsoleVariableRef CVarEnabled(
		TEXT("fx.Niagara.GaussianSplatWriter.Enabled"),
		GEnabled,
		TEXT("If == 0, Gaussian Splat Writer data interfaces hand nothing to the splat pipeline."),
		ECVF_RenderThreadSafe
	);

	static int32 GMaxNumSplats = 4 * 1024 * 1024;
	static FAutoConsoleVariableRef CVarMaxNumSplats(
		TEXT("fx.Niagara.GaussianSplatWriter.MaxNumSplats"),
		GMaxNumSplats,
		TEXT("Upper bound on the splats a single Gaussian Splat Writer instance allocates (default = 4194304)."),
		ECVF_Default
	);

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	struct FInstanceConfig
	{
		int32 NumSplats = 0;
		int32 SHDegree = 0;
		EGaussianSplatCoordinateSpace SHCoordinateSpace = EGaussianSplatCoordinateSpace::LH_FRU;

		uint32 Format = 0;
		uint32 SHDimension = 1;
		uint32 Stride = FGaussianSplatShared::EFloat_Stride;

		FVector3f PositionScale = FVector3f::One();
		FVector3f PositionBias = FVector3f::Zero();
		FVector3f ScaleScale = FVector3f::One();
		FVector3f ScaleBias = FVector3f::Zero();
		FVector4f ColorScale = FVector4f::One();
		FVector4f ColorBias = FVector4f::Zero();
		FVector3f SHScale = FVector3f::One();
		FVector3f SHBias = FVector3f::Zero();

		float SplatScale = 1.0f;
		float OpacityScale = 1.0f;
		float InverseToneCurveStrength = 0.0f;
		bool bInvertExpandGamut = true;

		FMatrix44f WorkingColorSpaceToSRGB = FMatrix44f::Identity;
		FString DebugName;

		void SetLayout(bool bQuantized, int32 InSHDegree)
		{
			SHDegree = InSHDegree;
			Format = bQuantized ? 1u : 0u;
			SHDimension = uint32(GaussianSplatCommon::GetSHDimension(SHDegree));
			Stride = uint32(FGaussianSplatRenderResource::GetSplatStrideForFormat(int32(SHDimension), bQuantized));
		}

		void SetQuantizationRanges(const FVector3f& BoundsMin, const FVector3f& BoundsMax, float MaxScale, float MaxSHCoefficient)
		{
			PositionBias = BoundsMin;
			PositionScale = (BoundsMax - BoundsMin).ComponentMax(FVector3f::ZeroVector) / float((1 << 21) - 1);

			const float LogMin = FMath::Loge(FGaussianSplatShared::EQuantized_MinLinearScale);
			const float LogMax = FMath::Max(FMath::Loge(FMath::Max(MaxScale, FGaussianSplatShared::EQuantized_MinLinearScale)), LogMin);
			ScaleBias = FVector3f(LogMin);
			ScaleScale = FVector3f((LogMax - LogMin) / 2047.0f, (LogMax - LogMin) / 2047.0f, (LogMax - LogMin) / 1023.0f);

			const float CoefficientMax = 0.5f / FGaussianSplatSHConstants::SH_C0;
			ColorBias = FVector4f(-CoefficientMax, -CoefficientMax, -CoefficientMax, 0.0f);
			ColorScale = FVector4f(2.0f * CoefficientMax, 2.0f * CoefficientMax, 2.0f * CoefficientMax, 1.0f) / 255.0f;

			SHBias = FVector3f(-MaxSHCoefficient);
			SHScale = FVector3f(2.0f * MaxSHCoefficient / 255.0f);
		}

		uint64 GetSplatBufferBytes() const { return uint64(FMath::Max(NumSplats, 1)) * uint64(Stride); }
	};

	struct FInstanceData_GameThread
	{
		FInstanceConfig Config;

		// Whether the emitter that owns this writer simulates in local space.  Resolved once at init, like
		// UNiagaraDataInterfaceEmitterProperties; false for a writer only the system scripts reach.
		bool bLocalSpace = false;
		FMatrix44f LocalToWorld = FMatrix44f::Identity;
		FBox3f LocalBounds = FBox3f(ForceInit);
		bool bVisible = true;
		bool bTransformSent = false;
		bool bPendingClear = false;
	};

	struct FRegistration {};

	struct FInstanceData_RenderThread
	{
		FInstanceConfig Config;

		TSharedPtr<FRegistration> Registration;

		TRefCountPtr<FRDGPooledBuffer> SplatBuffer;
		bool bNeedsClear = true;

		FMatrix44f LocalToWorld = FMatrix44f::Identity;
		FBox3f LocalBounds = FBox3f(ForceInit);
		bool bVisible = true;
		FGaussianSplatRenderProxy Proxy;
	};

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	struct FNDIProxy : public FNiagaraDataInterfaceProxyRW
	{
		virtual void ConsumePerInstanceDataFromGameThread(void* PerInstanceData, const FNiagaraSystemInstanceID& Instance) override { checkNoEntry(); }
		virtual int32 PerInstanceDataPassedToRenderThreadSize() const override { return 0; }

		virtual void GetDispatchArgs(const FNDIGpuComputeDispatchArgsGenContext& Context) override
		{
			if (const FInstanceData_RenderThread* InstanceData = PerInstanceData_RenderThread.Find(Context.GetSystemInstanceID()))
			{
				Context.SetDirect(uint32(FMath::Max(InstanceData->Config.NumSplats, 0)));
			}
		}

		static FRDGBufferRef RegisterSplatBuffer(FRDGBuilder& GraphBuilder, FInstanceData_RenderThread& InstanceData, bool bAllowClear)
		{
			if (!InstanceData.SplatBuffer.IsValid())
			{
				InstanceData.SplatBuffer = AllocatePooledBuffer(FRDGBufferDesc::CreateByteAddressDesc(uint32(InstanceData.Config.GetSplatBufferBytes())), TEXT("NiagaraGaussianSplatWriter.Splats"));
				InstanceData.bNeedsClear = true;
			}

			FRDGBufferRef Splats = GraphBuilder.RegisterExternalBuffer(InstanceData.SplatBuffer);
			if (bAllowClear && InstanceData.bNeedsClear)
			{
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Splats), 0u);
				InstanceData.bNeedsClear = false;
			}
			return Splats;
		}

		virtual void PreStage(const FNDIGpuComputePreStageContext& Context) override
		{
			if (FInstanceData_RenderThread* InstanceData = PerInstanceData_RenderThread.Find(Context.GetSystemInstanceID()))
			{
				RegisterSplatBuffer(Context.GetGraphBuilder(), *InstanceData, Context.GetSimStageData().bFirstStage);
			}
		}

		// Converts InstanceData to render as FGaussianSplatRenderProxy
		void CollectSplatProxies_RenderThread(FRDGBuilder& GraphBuilder, FNiagaraSystemInstanceID InstanceID, TArray<const FGaussianSplatRenderProxy*>& OutProxies)
		{
			FInstanceData_RenderThread* InstanceData = PerInstanceData_RenderThread.Find(InstanceID);
			if (InstanceData == nullptr || !GEnabled || InstanceData->Config.NumSplats <= 0)
			{
				return;
			}

			RegisterSplatBuffer(GraphBuilder, *InstanceData, true);

			const FInstanceConfig& Config = InstanceData->Config;
			FGaussianSplatRenderProxy& Proxy = InstanceData->Proxy;
			Proxy.NumSplats = uint32(Config.NumSplats);
			Proxy.SHDegree = Config.SHDegree;
			Proxy.SHCoordinateSpace = Config.SHCoordinateSpace;
			Proxy.GpuBuffer = InstanceData->SplatBuffer;
			Proxy.GpuDataStride = Config.Stride;
			Proxy.BufferFormat = Config.Format;
			Proxy.ClusterBuffer = nullptr;
			Proxy.NumClusters = 0;
			Proxy.PositionScale = Config.PositionScale;
			Proxy.PositionBias = Config.PositionBias;
			Proxy.ScaleScale = Config.ScaleScale;
			Proxy.ScaleBias = Config.ScaleBias;
			Proxy.ColorScale = Config.ColorScale;
			Proxy.ColorBias = Config.ColorBias;
			Proxy.SHScale = Config.SHScale;
			Proxy.SHBias = Config.SHBias;
			Proxy.LocalToWorld = InstanceData->LocalToWorld;
			Proxy.WorldToLocal = Proxy.LocalToWorld.Inverse();
			Proxy.LocalBounds = InstanceData->LocalBounds;
			Proxy.PointScaleFactor = Config.SplatScale;
			Proxy.OpacityScale = Config.OpacityScale;
			Proxy.bVisible = InstanceData->bVisible;
			Proxy.InverseToneCurveStrength = Config.InverseToneCurveStrength;
			Proxy.bInvertExpandGamutLUT = Config.bInvertExpandGamut;
			OutProxies.Add(&Proxy);
		}

		TMap<FNiagaraSystemInstanceID, FInstanceData_RenderThread> PerInstanceData_RenderThread;
	};

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	void VMGetNumSplats(FVectorVMExternalFunctionContext& Context)
	{
		VectorVM::FUserPtrHandler<FInstanceData_GameThread> InstanceData(Context);
		FNDIOutputParam<int32> OutNumSplats(Context);
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutNumSplats.SetAndAdvance(InstanceData->Config.NumSplats);
		}
	}

	void VMGetSHDegree(FVectorVMExternalFunctionContext& Context)
	{
		VectorVM::FUserPtrHandler<FInstanceData_GameThread> InstanceData(Context);
		FNDIOutputParam<int32> OutDegree(Context);
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutDegree.SetAndAdvance(InstanceData->Config.SHDegree);
		}
	}

	void VMClear(FVectorVMExternalFunctionContext& Context)
	{
		VectorVM::FUserPtrHandler<FInstanceData_GameThread> InstanceData(Context);
		FNDIInputParam<bool> InExecute(Context);
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			InstanceData->bPendingClear |= InExecute.GetAndAdvance();
		}
	}

} //namespace NDIGaussianSplatWriterLocal

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

UNiagaraDataInterfaceGaussianSplatWriter::UNiagaraDataInterfaceGaussianSplatWriter(FObjectInitializer const& ObjectInitializer)
	: Super(ObjectInitializer)
{
	using namespace NDIGaussianSplatWriterLocal;

	Proxy.Reset(new FNDIProxy());
}

void UNiagaraDataInterfaceGaussianSplatWriter::PostInitProperties()
{
	Super::PostInitProperties();

	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		ENiagaraTypeRegistryFlags Flags = ENiagaraTypeRegistryFlags::AllowAnyVariable | ENiagaraTypeRegistryFlags::AllowParameter;
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(GetClass()), Flags);
	}
}

bool UNiagaraDataInterfaceGaussianSplatWriter::Equals(const UNiagaraDataInterface* Other) const
{
	if (!Super::Equals(Other))
	{
		return false;
	}

	const UNiagaraDataInterfaceGaussianSplatWriter* OtherTyped = CastChecked<const UNiagaraDataInterfaceGaussianSplatWriter>(Other);
	return
		OtherTyped->NumSplats == NumSplats &&
		OtherTyped->SHDegree == SHDegree &&
		OtherTyped->SHCoordinateSpace == SHCoordinateSpace &&
		OtherTyped->PositionBounds == PositionBounds &&
		OtherTyped->MaxScale == MaxScale &&
		OtherTyped->MaxSHCoefficient == MaxSHCoefficient &&
		OtherTyped->SplatScale == SplatScale &&
		OtherTyped->OpacityScale == OpacityScale &&
		OtherTyped->bInvertToneCurve == bInvertToneCurve &&
		OtherTyped->InverseToneCurveStrength == InverseToneCurveStrength &&
		OtherTyped->bInvertExpandGamut == bInvertExpandGamut;
}

bool UNiagaraDataInterfaceGaussianSplatWriter::CopyToInternal(UNiagaraDataInterface* Destination) const
{
	if (!Super::CopyToInternal(Destination))
	{
		return false;
	}

	UNiagaraDataInterfaceGaussianSplatWriter* DestinationTyped = CastChecked<UNiagaraDataInterfaceGaussianSplatWriter>(Destination);
	DestinationTyped->NumSplats = NumSplats;
	DestinationTyped->SHDegree = SHDegree;
	DestinationTyped->SHCoordinateSpace = SHCoordinateSpace;
	DestinationTyped->PositionBounds = PositionBounds;
	DestinationTyped->MaxScale = MaxScale;
	DestinationTyped->MaxSHCoefficient = MaxSHCoefficient;
	DestinationTyped->SplatScale = SplatScale;
	DestinationTyped->OpacityScale = OpacityScale;
	DestinationTyped->bInvertToneCurve = bInvertToneCurve;
	DestinationTyped->InverseToneCurveStrength = InverseToneCurveStrength;
	DestinationTyped->bInvertExpandGamut = bInvertExpandGamut;
	return true;
}

int32 UNiagaraDataInterfaceGaussianSplatWriter::PerInstanceDataSize() const
{
	using namespace NDIGaussianSplatWriterLocal;
	return sizeof(FInstanceData_GameThread);
}

struct FEmitterBindings
{
	const UNiagaraDataInterfaceGaussianSplatReader* Reader = nullptr;
	bool bLocalSpace = false;
};

static FEmitterBindings FindEmitterBindings(const UNiagaraDataInterface* DataInterface, const FNiagaraSystemInstance* SystemInstance)
{
	const UNiagaraEmitter* Emitter = nullptr;
	TArray<TPair<const UObject*, const UNiagaraDataInterfaceGaussianSplatReader*>> ReaderOwners;
	FNiagaraDataInterfaceUtilities::ForEachDataInterface(
		SystemInstance,
		[DataInterface, &Emitter, &ReaderOwners](const FNiagaraDataInterfaceUtilities::FDataInterfaceUsageContext& UsageContext)
		{
			if (UsageContext.DataInterface == DataInterface)
			{
				if (const UNiagaraEmitter* Owner = Cast<UNiagaraEmitter>(UsageContext.OwnerObject))
				{
					Emitter = Owner;
				}
			}
			else if (const UNiagaraDataInterfaceGaussianSplatReader* Reader = Cast<UNiagaraDataInterfaceGaussianSplatReader>(UsageContext.DataInterface))
			{
				ReaderOwners.Emplace(UsageContext.OwnerObject, Reader);
			}
			return true;
		}
	);

	FEmitterBindings Bindings;
	if (Emitter)
	{
		// Return the first reader in the emitter
		for (const TPair<const UObject*, const UNiagaraDataInterfaceGaussianSplatReader*>& ReaderOwner : ReaderOwners)
		{
			if (ReaderOwner.Key == Emitter)
			{
				Bindings.Reader = ReaderOwner.Value;
				break;
			}
		}

		for (const FNiagaraEmitterInstanceRef& EmitterInstance : SystemInstance->GetEmitters())
		{
			if (EmitterInstance->GetEmitter() == Emitter)
			{
				Bindings.bLocalSpace = EmitterInstance->IsLocalSpace();
				break;
			}
		}
	}

	return Bindings;
}

static const UGaussianSplat* GetReaderAsset(const UNiagaraDataInterfaceGaussianSplatReader* Reader)
{
	const UGaussianSplat* Asset = Reader ? Reader->GetGaussianSplat() : nullptr;
	if (Asset == nullptr || Asset->GetNumSplats() <= 0 || Asset->GetRuntimeDataType() == nullptr)
	{
		return nullptr;
	}

	return Asset;
}

// Assigns writer properties with this general priority:
// 1. hand-edited properties
// 2. inherited properties from the reader
// 3. default initialized values
static void InitInstanceConfig(const UNiagaraDataInterfaceGaussianSplatWriter& Writer, const UNiagaraDataInterfaceGaussianSplatReader* Reader, NDIGaussianSplatWriterLocal::FInstanceConfig& Config)
{
	using namespace NDIGaussianSplatWriterLocal;

	const UGaussianSplat* Asset = GetReaderAsset(Reader);

	// Propagate the asset storage format if it exists, otherwise use quantized format
	const FGaussianSplatFormatQuantized* FormatQuantized = Asset ? Asset->GetRuntimeData<FGaussianSplatFormatQuantized>() : nullptr;
	const bool bQuantized = (Asset == nullptr) || (FormatQuantized != nullptr);
	const int32 SHDegree = Writer.SHDegree.Get(Asset ? Reader->GetSHDegree() : 0);
	Config.SetLayout(bQuantized, FMath::Clamp(SHDegree, 0, GaussianSplatCommon::MaxSupportedSHDegree));
	Config.SHCoordinateSpace = Writer.SHCoordinateSpace.Get(Asset ? Asset->GetSHCoordinateSpace() : EGaussianSplatCoordinateSpace::LH_FRU);
	Config.NumSplats = FMath::Clamp(Writer.NumSplats.Get(Asset ? Asset->GetNumSplats() : 0), 0, FMath::Max(GMaxNumSplats, 0));

	if (bQuantized)
	{
		const bool bQuantizedBoundsIsSet = Writer.PositionBounds.IsSet() || Writer.MaxScale.IsSet() || Writer.MaxSHCoefficient.IsSet();
		if (FormatQuantized && !bQuantizedBoundsIsSet)
		{
			Config.PositionScale = FormatQuantized->PositionScaleBias[0];
			Config.PositionBias = FormatQuantized->PositionScaleBias[1];
			Config.ScaleScale = FormatQuantized->ScaleScaleBias[0];
			Config.ScaleBias = FormatQuantized->ScaleScaleBias[1];
			Config.ColorScale = FormatQuantized->ColorScaleBias[0];
			Config.ColorBias = FormatQuantized->ColorScaleBias[1];
			Config.SHScale = FormatQuantized->SHScaleBias[0];
			Config.SHBias = FormatQuantized->SHScaleBias[1];
			return;
		}

		FNiagaraGaussianSplatWriterBounds Bounds;
		float MaxScale = 100.0f;
		float MaxSHCoefficient = 2.0f;
		if (FormatQuantized)
		{
			// Inverts FInstanceConfig::SetQuantizationRanges.
			const FVector3f BoundsMin = FormatQuantized->PositionScaleBias[1];
			const FVector3f BoundsMax = BoundsMin + FormatQuantized->PositionScaleBias[0] * float((1 << 21) - 1);
			Bounds.Min = FVector(BoundsMin);
			Bounds.Max = FVector(BoundsMax);

			const FVector3f LogMax = FormatQuantized->ScaleScaleBias[1] + FormatQuantized->ScaleScaleBias[0] * FVector3f(2047.0f, 2047.0f, 1023.0f);
			MaxScale = FMath::Exp(LogMax.GetMax());

			const FVector3f SHMin = FormatQuantized->SHScaleBias[1];
			const FVector3f SHMax = SHMin + FormatQuantized->SHScaleBias[0] * 255.0f;
			MaxSHCoefficient = FMath::Max(SHMin.GetAbsMax(), SHMax.GetAbsMax());
		}
		Bounds = Writer.PositionBounds.Get(Bounds);
		Config.SetQuantizationRanges(FVector3f(Bounds.Min), FVector3f(Bounds.Max), Writer.MaxScale.Get(MaxScale), Writer.MaxSHCoefficient.Get(MaxSHCoefficient));
	}
}

bool UNiagaraDataInterfaceGaussianSplatWriter::InitPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance)
{
	using namespace NDIGaussianSplatWriterLocal;

	FInstanceData_GameThread* InstanceData = new(PerInstanceData) FInstanceData_GameThread();
	FInstanceConfig& Config = InstanceData->Config;

	Config.DebugName = FString::Printf(TEXT("%s.%s"), SystemInstance->GetSystem() ? *SystemInstance->GetSystem()->GetName() : TEXT("?"), *GetFName().ToString());

	const FEmitterBindings Bindings = FindEmitterBindings(this, SystemInstance);
	InstanceData->bLocalSpace = Bindings.bLocalSpace;
	InitInstanceConfig(*this, Bindings.Reader, Config);
	Config.SplatScale = SplatScale;
	Config.OpacityScale = OpacityScale;
	Config.InverseToneCurveStrength = bInvertToneCurve ? InverseToneCurveStrength : 0.0f;
	Config.bInvertExpandGamut = bInvertExpandGamut;
	Config.WorkingColorSpaceToSRGB = FMatrix44f(UE::Color::FColorSpaceTransform::GetWorkingColorSpaceToSRGB());

	if (Config.GetSplatBufferBytes() > FGaussianSplatRenderResource::GetResourceSizeLimitInBytes())
	{
		UE_LOGF(LogNiagaraGaussianSplatWriter, Error, "%ls: %d splats (%u bytes) exceed the splat resource size limit", *Config.DebugName, Config.NumSplats, Config.Stride);
		Config.NumSplats = 0;
	}

	// Initialize on the render thread
	TSharedRef<FRegistration> Registration = MakeShared<FRegistration>();
	FNDIProxy* NDIProxy = GetProxyAs<FNDIProxy>();
	const FNiagaraSystemInstanceID InstanceID = SystemInstance->GetId();
	ENQUEUE_RENDER_COMMAND(FNDIGaussianSplatWriter_InitInstance)
	(
		[NDIProxy, InstanceID, RT_Config = Config, Registration](FRHICommandListImmediate&)
		{
			FInstanceData_RenderThread& InstanceData_RT = NDIProxy->PerInstanceData_RenderThread.FindOrAdd(InstanceID);
			InstanceData_RT = FInstanceData_RenderThread();
			InstanceData_RT.Config = RT_Config;
			InstanceData_RT.Registration = Registration;
		}
	);

	// Register FGaussianSplatRenderProxy creation with the Gaussian Splat renderer
	GaussianSplat::RegisterExternalProxySource(SystemInstance->GetWorld(), FOnCollectGaussianSplatProxies::FDelegate::CreateSPLambda(Registration,
		[NDIProxy, InstanceID](FRDGBuilder& GraphBuilder, const FSceneView&, TArray<const FGaussianSplatRenderProxy*>& OutProxies)
		{
			NDIProxy->CollectSplatProxies_RenderThread(GraphBuilder, InstanceID, OutProxies);
		}));

	return true;
}

void UNiagaraDataInterfaceGaussianSplatWriter::DestroyPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance)
{
	using namespace NDIGaussianSplatWriterLocal;

	static_cast<FInstanceData_GameThread*>(PerInstanceData)->~FInstanceData_GameThread();

	ENQUEUE_RENDER_COMMAND(FNDIGaussianSplatWriter_RemoveInstance)
	(
		[NDIProxy = GetProxyAs<FNDIProxy>(), InstanceID = SystemInstance->GetId()](FRHICommandListImmediate&)
		{
			NDIProxy->PerInstanceData_RenderThread.Remove(InstanceID);
		}
	);
}

bool UNiagaraDataInterfaceGaussianSplatWriter::PerInstanceTick(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance, float DeltaSeconds)
{
	using namespace NDIGaussianSplatWriterLocal;

	FInstanceData_GameThread* InstanceData = static_cast<FInstanceData_GameThread*>(PerInstanceData);

	USceneComponent* AttachComponent = SystemInstance->GetAttachComponent();
	// Splat positions stay in the space the emitter simulates in.  The pipeline applies LocalToWorld.
	const FTransform SimToWorld = SystemInstance->GetLWCSimToWorld(InstanceData->bLocalSpace);
	const UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(AttachComponent);
	const FMatrix44f LocalToWorld = FMatrix44f(SimToWorld.ToMatrixWithScale());
	const bool bVisible = PrimitiveComponent ? PrimitiveComponent->ShouldRender() : true;
	const FBox SystemBounds = SystemInstance->GetLocalBounds();
	const FBox3f LocalBounds = FBox3f(InstanceData->bLocalSpace ? SystemBounds : SystemBounds.TransformBy(SystemInstance->GetWorldTransform()));

	if (!InstanceData->bTransformSent || bVisible != InstanceData->bVisible || !LocalToWorld.Equals(InstanceData->LocalToWorld, 0.0f) || LocalBounds != InstanceData->LocalBounds)
	{
		InstanceData->bTransformSent = true;
		InstanceData->bVisible = bVisible;
		InstanceData->LocalToWorld = LocalToWorld;
		InstanceData->LocalBounds = LocalBounds;

		ENQUEUE_RENDER_COMMAND(FNDIGaussianSplatWriter_UpdateTransform)
		(
			[NDIProxy = GetProxyAs<FNDIProxy>(), InstanceID = SystemInstance->GetId(), LocalToWorld, LocalBounds, bVisible](FRHICommandListImmediate&)
			{
				if (FInstanceData_RenderThread* InstanceData_RT = NDIProxy->PerInstanceData_RenderThread.Find(InstanceID))
				{
					InstanceData_RT->LocalToWorld = LocalToWorld;
					InstanceData_RT->LocalBounds = LocalBounds;
					InstanceData_RT->bVisible = bVisible;
				}
			}
		);
	}
	return false;
}

bool UNiagaraDataInterfaceGaussianSplatWriter::PerInstanceTickPostSimulate(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance, float DeltaSeconds)
{
	using namespace NDIGaussianSplatWriterLocal;

	FInstanceData_GameThread* InstanceData = static_cast<FInstanceData_GameThread*>(PerInstanceData);
	if (InstanceData->bPendingClear)
	{
		InstanceData->bPendingClear = false;
		ENQUEUE_RENDER_COMMAND(FNDIGaussianSplatWriter_Clear)
		(
			[NDIProxy = GetProxyAs<FNDIProxy>(), InstanceID = SystemInstance->GetId()](FRHICommandListImmediate&)
			{
				if (FInstanceData_RenderThread* InstanceData_RT = NDIProxy->PerInstanceData_RenderThread.Find(InstanceID))
				{
					InstanceData_RT->bNeedsClear = true;
				}
			}
		);
	}
	return false;
}

#if WITH_EDITOR
void UNiagaraDataInterfaceGaussianSplatWriter::GetFeedback(UNiagaraSystem* InAsset, UNiagaraComponent* InComponent, TArray<FNiagaraDataInterfaceError>& OutErrors, TArray<FNiagaraDataInterfaceFeedback>& OutWarnings, TArray<FNiagaraDataInterfaceFeedback>& OutInfo)
{
	Super::GetFeedback(InAsset, InComponent, OutErrors, OutWarnings, OutInfo);
}
#endif // WITH_EDITOR

#if WITH_EDITORONLY_DATA
void UNiagaraDataInterfaceGaussianSplatWriter::GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const
{
	using namespace NDIGaussianSplatWriterLocal;

	FNiagaraFunctionSignature ReadSig;
	ReadSig.bMemberFunction = true;
	ReadSig.bRequiresContext = false;
	ReadSig.Inputs.Emplace(FNiagaraTypeDefinition(GetClass()), TEXT("GaussianSplatWriter"));

	FNiagaraFunctionSignature WriteSig = ReadSig;
	WriteSig.bRequiresExecPin = true;
	WriteSig.bWriteFunction = true;
	WriteSig.bSupportsCPU = false;
	WriteSig.Inputs.Emplace_GetRef(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Execute")).SetValue(true);

	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Emplace_GetRef(ReadSig);
		Sig.Name = NAME_GetNumSplats;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("NumSplats"));
		Sig.Description = LOCTEXT("GetNumSplatsDesc", "Number of splats the writer holds; the iteration count of a stage that iterates over it.");
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Emplace_GetRef(ReadSig);
		Sig.Name = NAME_GetSHDegree;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("SHDegree"));
		Sig.Description = LOCTEXT("GetSHDegreeDesc", "Spherical harmonic degree each splat carries; the coefficient count is (degree + 1) squared.");
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Emplace_GetRef(WriteSig);
		Sig.Name = NAME_SetSplat;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetPositionDef(), TEXT("Position"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetQuatDef(), TEXT("Rotation"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Scale"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetColorDef(), TEXT("Color"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Success"));
		Sig.Description = LOCTEXT("SetSplatDesc", "Writes a complete flat colored splat. RGB is the linear color, alpha the opacity, scale the per axis standard deviation. Higher harmonics are zeroed.");
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Emplace_GetRef(WriteSig);
		Sig.Name = NAME_SetBaseData;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetPositionDef(), TEXT("Position"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetQuatDef(), TEXT("Rotation"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Scale"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Success"));
		Sig.Description = LOCTEXT("SetBaseDataDesc", "Writes the position, rotation and scale of a splat and leaves its color, opacity and harmonics alone. A zero scale makes the splat one the pipeline does not draw.");
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Emplace_GetRef(WriteSig);
		Sig.Name = NAME_SetColor;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetColorDef(), TEXT("Color"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Success"));
		Sig.Description = LOCTEXT("SetColorDesc", "Writes the flat color (RGB, linear) and opacity (alpha) of a splat.");
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Emplace_GetRef(WriteSig);
		Sig.Name = NAME_SetOpacity;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Opacity"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Success"));
		Sig.Description = LOCTEXT("SetOpacityDesc", "Writes the opacity of a splat.");
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Emplace_GetRef(WriteSig);
		Sig.Name = NAME_SetSHCoefficient;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Coefficient"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Value"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Success"));
		Sig.Description = LOCTEXT("SetSHCoefficientDesc", "Writes one raw spherical harmonic coefficient as a Gaussian Splat Reader hands it out: 0 is the color coefficient, 1 and up the higher bands.");
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Emplace_GetRef(ReadSig);
		Sig.Name = NAME_Clear;
		Sig.ModuleUsageBitmask = ENiagaraScriptUsageMask::System | ENiagaraScriptUsageMask::Emitter;
		Sig.bRequiresExecPin = true;
		Sig.bWriteFunction = true;
		Sig.bSupportsGPU = false;
		Sig.Inputs.Emplace_GetRef(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Execute")).SetValue(true);
		Sig.Description = LOCTEXT("ClearDesc", "Zeroes every splat so none is drawn, before this tick's stages run. Splats otherwise persist from tick to tick.");
	}
}
#endif // WITH_EDITORONLY_DATA

void UNiagaraDataInterfaceGaussianSplatWriter::GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, void* InstanceData, FVMExternalFunction& OutFunc)
{
	using namespace NDIGaussianSplatWriterLocal;

	if (BindingInfo.Name == NAME_GetNumSplats)
	{
		OutFunc = FVMExternalFunction::CreateStatic(VMGetNumSplats);
	}
	else if (BindingInfo.Name == NAME_GetSHDegree)
	{
		OutFunc = FVMExternalFunction::CreateStatic(VMGetSHDegree);
	}
	else if (BindingInfo.Name == NAME_Clear)
	{
		OutFunc = FVMExternalFunction::CreateStatic(VMClear);
	}
}

#if WITH_EDITORONLY_DATA
bool UNiagaraDataInterfaceGaussianSplatWriter::AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const
{
	using namespace NDIGaussianSplatWriterLocal;

	bool bSuccess = Super::AppendCompileHash(InVisitor);
	bSuccess &= InVisitor->UpdateShaderFile(TemplateShaderFilePath);
	bSuccess &= InVisitor->UpdateShaderParameters<FShaderParameters>();
	return bSuccess;
}

void UNiagaraDataInterfaceGaussianSplatWriter::GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, FString& OutHLSL)
{
	using namespace NDIGaussianSplatWriterLocal;

	const TMap<FString, FStringFormatArg> TemplateArgs =
	{
		{TEXT("ParameterName"),	ParamInfo.DataInterfaceHLSLSymbol},
	};
	AppendTemplateHLSL(OutHLSL, TemplateShaderFilePath, TemplateArgs);
}

bool UNiagaraDataInterfaceGaussianSplatWriter::GetFunctionHLSL(const FNiagaraDataInterfaceHlslGenerationContext& HlslGenContext, FString& OutHLSL)
{
	using namespace NDIGaussianSplatWriterLocal;

	static const TSet<FName> ValidGpuFunctions =
	{
		NAME_GetNumSplats,
		NAME_GetSHDegree,
		NAME_SetSplat,
		NAME_SetBaseData,
		NAME_SetColor,
		NAME_SetOpacity,
		NAME_SetSHCoefficient,
	};
	return ValidGpuFunctions.Contains(HlslGenContext.GetFunctionInfo().DefinitionName);
}
#endif // WITH_EDITORONLY_DATA

void UNiagaraDataInterfaceGaussianSplatWriter::BuildShaderParameters(FNiagaraShaderParametersBuilder& ShaderParametersBuilder) const
{
	using namespace NDIGaussianSplatWriterLocal;

	ShaderParametersBuilder.AddNestedStruct<FShaderParameters>();
}

void UNiagaraDataInterfaceGaussianSplatWriter::SetShaderParameters(const FNiagaraDataInterfaceSetShaderParametersContext& Context) const
{
	using namespace NDIGaussianSplatWriterLocal;

	FNDIProxy& NDIProxy = Context.GetProxy<FNDIProxy>();
	FInstanceData_RenderThread& InstanceData = NDIProxy.PerInstanceData_RenderThread.FindChecked(Context.GetSystemInstanceID());
	FRDGBuilder& GraphBuilder = Context.GetGraphBuilder();

	const FInstanceConfig& Config = InstanceData.Config;
	FShaderParameters* ShaderParameters = Context.GetParameterNestedStruct<FShaderParameters>();
	ShaderParameters->NumSplats				= Config.NumSplats;
	ShaderParameters->OutputFormat			= Config.Format;
	ShaderParameters->OutputStride			= Config.Stride;
	ShaderParameters->SHDimension			= Config.SHDimension;
	ShaderParameters->SplatData				= GraphBuilder.CreateUAV(FNDIProxy::RegisterSplatBuffer(GraphBuilder, InstanceData, false));
	ShaderParameters->PositionScale			= Config.PositionScale;
	ShaderParameters->PositionBias			= Config.PositionBias;
	ShaderParameters->ScaleScale			= Config.ScaleScale;
	ShaderParameters->ScaleBias				= Config.ScaleBias;
	ShaderParameters->ColorScale			= Config.ColorScale;
	ShaderParameters->ColorBias				= Config.ColorBias;
	ShaderParameters->SHScale				= Config.SHScale;
	ShaderParameters->SHBias				= Config.SHBias;
	ShaderParameters->WorkingColorSpaceToSRGB = Config.WorkingColorSpaceToSRGB;
}

#undef LOCTEXT_NAMESPACE
