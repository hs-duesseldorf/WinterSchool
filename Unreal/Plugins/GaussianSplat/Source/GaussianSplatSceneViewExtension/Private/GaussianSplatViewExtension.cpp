// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatViewExtension.h"
#include "GaussianSplatRenderResource.h"
#include "GaussianSplatRenderPipeline.h"
#include "GaussianSplatTransform.h"

#include "SceneView.h"
#include "SceneInterface.h"
#include "PrimitiveSceneInfo.h"

#include "ScreenPass.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatInverseToneCurveLUT.h"
#include "GaussianSplatRenderStats.h"
#include "GaussianSplatSceneViewExtensionModule.h"
#include "GaussianSplatSubsystem.h"
#include "Engine/World.h"
#include "Containers/SortedMap.h"
#include "SceneRendering.h"
#include "SceneTextures.h"
#include "RendererInterface.h"
#include "Misc/ScopeRWLock.h"

#define INVALID_ID 0xFFFFFFFF

static TAutoConsoleVariable<int32> CVarGaussianSplatNumSplats(
	TEXT("r.GaussianSplat.NumSplats"),
	-1,
	TEXT("The maximum number of splats to support (default = -1 is unbounded)"),
	ECVF_RenderThreadSafe);

namespace
{
	static uint32 GetUniqueID(const UGaussianSplatComponent* Component)
	{
		return IsValid(Component->Asset) ? Component->GetUniqueID() : INVALID_ID;
	}
}

namespace
{
	// Live splat extensions, so the renderer-wide post-opaque delegate can find the one of a view family among its extensions
	FRWLock GLiveExtensionsLock;
	TSet<const ISceneViewExtension*> GLiveExtensions;
}

FGaussianSplatViewExtension::FGaussianSplatViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
{
	FWriteScopeLock Lock(GLiveExtensionsLock);
	GLiveExtensions.Add(this);
}

FGaussianSplatViewExtension::~FGaussianSplatViewExtension()
{
	{
		FWriteScopeLock Lock(GLiveExtensionsLock);
		GLiveExtensions.Remove(this);
	}
	RenderProxy_RenderThread.Reset();
}

FMatrix44f GaussianSplat::GetComponentLocalToWorld(const UGaussianSplatComponent& Component)
{
	return FMatrix44f(Component.GetComponentTransform().ToMatrixWithScale());
}

bool GaussianSplat::ShouldCastShadow(const UGaussianSplatComponent& Component)
{
	return Component.CastShadow && (Component.ShouldRender() || Component.bCastHiddenShadow);
}

FGaussianSplatRenderProxy* CreateRenderProxy(const UGaussianSplatComponent* Component)
{
	FGaussianSplatRenderProxy* Proxy = new FGaussianSplatRenderProxy();

	Proxy->SplatID = Component->GetUniqueID();
	UGaussianSplat* GaussianSplat = Component->Asset;
	TSharedPtr<FGaussianSplatRenderResource> Resource = GaussianSplat->GetResource();

	Proxy->SHDegree = Resource->GetSHDegree();
	Proxy->SHCoordinateSpace = GaussianSplat->GetSHCoordinateSpace();

	// Extract compression values
	const FSharedStruct& RuntimeData = Resource->GetRuntimeData();
	const FGaussianSplatFormatQuantized* QuantizedFormat = RuntimeData.GetPtr<FGaussianSplatFormatQuantized>();
	Proxy->BufferFormat = QuantizedFormat != nullptr ? 1u : 0u;
	if (QuantizedFormat)
	{
		Proxy->PositionScale = QuantizedFormat->PositionScaleBias[0];
		Proxy->PositionBias  = QuantizedFormat->PositionScaleBias[1];
		Proxy->ScaleScale    = QuantizedFormat->ScaleScaleBias[0];
		Proxy->ScaleBias     = QuantizedFormat->ScaleScaleBias[1];
		Proxy->ColorScale    = QuantizedFormat->ColorScaleBias[0];
		Proxy->ColorBias     = QuantizedFormat->ColorScaleBias[1];
		Proxy->SHScale       = QuantizedFormat->SHScaleBias[0];
		Proxy->SHBias        = QuantizedFormat->SHScaleBias[1];
	}

	Proxy->LocalToWorld = GaussianSplat::GetComponentLocalToWorld(*Component);
	Proxy->WorldToLocal = Proxy->LocalToWorld.Inverse();

	Proxy->PointScaleFactor = Component->PointScaleFactor;
	Proxy->OpacityScale     = Component->OpacityScale;
	Proxy->MaxSHDegree      = Component->MaxSHDegree;

	Proxy->InverseToneCurveStrength = Component->bInvertToneCurve ? Component->InverseToneCurveStrength : 0.0f;
	Proxy->bInvertExpandGamutLUT = Component->bInvertExpandGamut;
	Proxy->LightingStrength = Component->LightingStrength;
	Proxy->ShadowStrength = Component->ShadowStrength;
	Proxy->bVisible = Component->ShouldRender();
	Proxy->bCastShadow = GaussianSplat::ShouldCastShadow(*Component);

	return Proxy;
}

void FGaussianSplatViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
	ENQUEUE_RENDER_COMMAND(FUpdateGaussianSplatStatsRegistration)([](FRHICommandList&)
	{
		FModuleManager::GetModuleChecked<FGaussianSplatSceneViewExtensionModule>("GaussianSplatSceneViewExtension").GetRenderStats_RenderThread().UpdateRegistration();
	});

	// Update render-thread transform data
	for (const UGaussianSplatComponent* Component : RegisteredComponents)
	{
		const uint32 UniqueID = GetUniqueID(Component);
		if (UniqueID != INVALID_ID)
		{
			FMatrix44f LocalToWorld = GaussianSplat::GetComponentLocalToWorld(*Component);
			FMatrix44f WorldToLocal = FMatrix44f(Component->GetComponentTransform().ToMatrixWithScale().Inverse());
			bool bVisible = Component->ShouldRender();
			const float PointScaleFactor = Component->PointScaleFactor;
			const float OpacityScale = Component->OpacityScale;
			const int32 MaxSHDegree = Component->MaxSHDegree;
			float InverseToneCurveStrength = Component->bInvertToneCurve ? Component->InverseToneCurveStrength : 0.0f;
			bool bInvertExpandGamutLUT = Component->bInvertExpandGamut;
			const float LightingStrength = Component->LightingStrength;
			const float ShadowStrength = Component->ShadowStrength;
			const bool bCastShadow = GaussianSplat::ShouldCastShadow(*Component);

			ENQUEUE_RENDER_COMMAND(FCreateGaussianSplatRenderProxy)(
				[
					WeakThis = AsWeak(),
					UniqueID,
					LocalToWorld,
					WorldToLocal,
					bVisible,
					PointScaleFactor,
					OpacityScale,
					MaxSHDegree,
					InverseToneCurveStrength,
					bInvertExpandGamutLUT,
					LightingStrength,
					ShadowStrength,
					bCastShadow
				](FRHICommandList& RHICmdList)
				{
					TSharedPtr<FGaussianSplatViewExtension> This = StaticCastWeakPtr<FGaussianSplatViewExtension>(WeakThis).Pin();
					if (This.IsValid())
					{
						if (TUniquePtr<FGaussianSplatRenderProxy>* Proxy = This->RenderProxy_RenderThread.Find(UniqueID))
						{
							(*Proxy)->LocalToWorld = LocalToWorld;
							(*Proxy)->WorldToLocal = WorldToLocal;
							(*Proxy)->bVisible = bVisible;
							(*Proxy)->PointScaleFactor = PointScaleFactor;
							(*Proxy)->OpacityScale = OpacityScale;
							(*Proxy)->MaxSHDegree = MaxSHDegree;
							(*Proxy)->InverseToneCurveStrength = InverseToneCurveStrength;
							(*Proxy)->bInvertExpandGamutLUT = bInvertExpandGamutLUT;
							(*Proxy)->LightingStrength = LightingStrength;
							(*Proxy)->ShadowStrength = ShadowStrength;
							(*Proxy)->bCastShadow = bCastShadow;
						}
					}
				}
			);
		}
	}
}

void FGaussianSplatViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	//if (PassId == EPostProcessingPass::VisualizeDepthOfField)
	if (PassId == EPostProcessingPass::BeforeDOF)
	{
		if (GaussianSplat::IsEnabled() && View.Family && View.Family->Scene)
		{
			InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FGaussianSplatViewExtension::PostProcessPass_RenderThread));
		}
	}
}

bool FGaussianSplatViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	if (!FWorldSceneViewExtension::IsActiveThisFrame_Internal(Context))
	{
		return false;
	}

	return true;
}

namespace
{
	void GetSceneDepthAndVelocityTextures(
		const FPostProcessMaterialInputs& Inputs,
		FRDGTextureRef& OutSceneDepthTexture,
		FRDGTextureRef& OutSceneVelocityTexture)
	{
		OutSceneDepthTexture = nullptr;
		OutSceneVelocityTexture = nullptr;

		if (Inputs.SceneTextures.SceneTextures)
		{
			const FSceneTextureUniformParameters* Contents = Inputs.SceneTextures.SceneTextures->GetContents();
			OutSceneDepthTexture = Contents->SceneDepthTexture;
			OutSceneVelocityTexture = Contents->GBufferVelocityTexture;
		}
		else if (Inputs.SceneTextures.MobileSceneTextures)
		{
			const FMobileSceneTextureUniformParameters* Contents = Inputs.SceneTextures.MobileSceneTextures->GetContents();
			OutSceneDepthTexture = Contents->SceneDepthTexture;
			OutSceneVelocityTexture = Contents->SceneVelocityTexture;
		}
	}
}

FScreenPassTexture FGaussianSplatViewExtension::PostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& InOutInputs)
{
	FScreenPassTextureSlice SceneColorSlice = InOutInputs.GetInput(EPostProcessMaterialInput::SceneColor);
	check(SceneColorSlice.IsValid());

	FScreenPassTexture SceneColor(SceneColorSlice);

	FScreenPassRenderTarget Output = InOutInputs.OverrideOutput;

	if (WasRenderedDeferred(View))
	{
		// The splats are already part of the lit scene (GBuffer path); pass the scene color through
		if (!Output.IsValid())
		{
			return SceneColor;
		}
		AddDrawTexturePass(GraphBuilder, View, SceneColorSlice, Output);
		return MoveTemp(Output);
	}

	if (GaussianSplat::IsHardwareRasterization(View.GetShaderPlatform()))
	{
		if (!Output.IsValid())
		{
			Output = FScreenPassRenderTarget(SceneColor.Texture, SceneColor.ViewRect, View.GetOverwriteLoadAction());
		}
		else
		{
			AddDrawTexturePass(GraphBuilder, View, SceneColorSlice, Output);
		}

		FRDGTextureRef SceneDepthTexture = nullptr;
		FRDGTextureRef SceneVelocityTexture = nullptr;
		GetSceneDepthAndVelocityTextures(InOutInputs, SceneDepthTexture, SceneVelocityTexture);
		Render(GraphBuilder, View, Output, SceneDepthTexture, SceneVelocityTexture);
	}
	else
	{
		FRDGTextureDesc ColorDesc = FRDGTextureDesc::Create2D(
			SceneColor.Texture->Desc.Extent,
			SceneColor.Texture->Desc.Format,
			//FClearValueBinding::Black,
			FClearValueBinding::Transparent,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
		FRDGTextureRef ColorTexture = GraphBuilder.CreateTexture(ColorDesc, TEXT("GaussianSplat.Color"));
		// Copy SceneColor into the intermediate so splats composite on top of the scene.
		AddCopyTexturePass(GraphBuilder, SceneColor.Texture, ColorTexture);

		FScreenPassRenderTarget ColorRT(ColorTexture, SceneColor.ViewRect, ERenderTargetLoadAction::ELoad);

		FRDGTextureRef SceneDepthTexture = nullptr;
		FRDGTextureRef SceneVelocityTexture = nullptr;
		GetSceneDepthAndVelocityTextures(InOutInputs, SceneDepthTexture, SceneVelocityTexture);
		Render(GraphBuilder, View, ColorRT, SceneDepthTexture, SceneVelocityTexture);

		// Write the composited result to the engine's expected output.
		if (Output.IsValid())
		{
			AddDrawTexturePass(GraphBuilder, View, FScreenPassTextureSlice::CreateFromScreenPassTexture(GraphBuilder, FScreenPassTexture(ColorTexture, SceneColor.ViewRect)), Output);
		}
		else
		{
			Output = ColorRT;
		}
	}

	return MoveTemp(Output);
}

void FGaussianSplatViewExtension::Render(FRDGBuilder& GraphBuilder, const FSceneView& View, const FScreenPassRenderTarget& Output, FRDGTexture* SceneDepthTexture, FRDGTexture* SceneVelocityTexture)
{
	TArray<FGaussianSplatRenderItem> RenderItems;
	GatherRenderItems(GraphBuilder, View, RenderItems);

	FGaussianSplatSceneViewExtensionModule& Module = FModuleManager::GetModuleChecked<FGaussianSplatSceneViewExtensionModule>("GaussianSplatSceneViewExtension");
	GaussianSplat::FRenderStats& RenderStats = Module.GetRenderStats_RenderThread();
	GaussianSplat::Render(GraphBuilder, View, RenderItems, RenderStats, Output, SceneDepthTexture, SceneVelocityTexture);
}

void FGaussianSplatViewExtension::PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView, const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextureUniformBuffer)
{
	// Desktop deferred renderer; mobile deferred goes through PostOpaqueRender_RenderThread
	if (!GaussianSplat::IsEnabled() || !InView.bIsViewInfo || InView.Family == nullptr || InView.Family->Scene == nullptr
		|| IsMobilePlatform(InView.GetShaderPlatform())
		|| !GaussianSplat::UsesDeferredLighting(InView))
	{
		return;
	}

	const FViewInfo& View = static_cast<const FViewInfo&>(InView);
	const FSceneTextures& SceneTextures = View.GetSceneTextures();

	FGaussianSplatDeferredTargets Targets;
	Targets.SceneColor = SceneTextures.Color.Target;
	Targets.GBufferA = SceneTextures.GBufferA;
	Targets.GBufferB = SceneTextures.GBufferB;
	Targets.GBufferC = SceneTextures.GBufferC;
	Targets.SceneDepth = SceneTextures.Depth.Target;
	Targets.Velocity = SceneTextures.Velocity;
	if (!Targets.SceneColor || !Targets.GBufferA || !Targets.GBufferB || !Targets.GBufferC || !Targets.SceneDepth)
	{
		return;
	}

	RenderDeferred_RenderThread(GraphBuilder, View, Targets);
}

void FGaussianSplatViewExtension::PostOpaqueRender_RenderThread(FPostOpaqueRenderParameters& Parameters)
{
	const FViewInfo* View = Parameters.View;
	if (Parameters.GraphBuilder == nullptr || View == nullptr || View->Family == nullptr || View->Family->Scene == nullptr
		|| !IsMobilePlatform(View->GetShaderPlatform())
		|| !GaussianSplat::IsEnabled()
		|| !GaussianSplat::UsesDeferredLighting(*View))
	{
		return;
	}

	// The delegate is renderer-wide: draw only for families this world's splat extension takes part in. The family holds a
	// reference to its active extensions, which keeps the one found alive for this call.
	FGaussianSplatViewExtension* Extension = nullptr;
	{
		FReadScopeLock Lock(GLiveExtensionsLock);
		for (const TSharedRef<ISceneViewExtension, ESPMode::ThreadSafe>& FamilyExtension : View->Family->ViewExtensions)
		{
			if (GLiveExtensions.Contains(&FamilyExtension.Get()))
			{
				Extension = static_cast<FGaussianSplatViewExtension*>(&FamilyExtension.Get());
				break;
			}
		}
	}
	if (Extension == nullptr)
	{
		return;
	}

	const FSceneTextures& SceneTextures = View->GetSceneTextures();
	FGaussianSplatDeferredTargets Targets;
	Targets.SceneColor = SceneTextures.Color.Target;
	Targets.GBufferA = SceneTextures.GBufferA;
	Targets.GBufferB = SceneTextures.GBufferB;
	Targets.GBufferC = SceneTextures.GBufferC;
	Targets.SceneDepth = SceneTextures.Depth.Target;

	// Multipass keeps the GBuffer in memory for the lighting passes; memoryless or multisampled targets mean a setup this
	// path does not cover (the post-process composite draws the splats then)
	for (FRDGTextureRef Texture : { Targets.SceneColor, Targets.GBufferA, Targets.GBufferB, Targets.GBufferC, Targets.SceneDepth })
	{
		if (Texture == nullptr || Texture->Desc.NumSamples > 1 || EnumHasAnyFlags(Texture->Desc.Flags, TexCreate_Memoryless))
		{
			return;
		}
	}

	Extension->RenderDeferred_RenderThread(*Parameters.GraphBuilder, *View, Targets);
}

void FGaussianSplatViewExtension::RenderDeferred_RenderThread(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FGaussianSplatDeferredTargets& Targets)
{
	TArray<FGaussianSplatRenderItem> RenderItems;
	TArray<const FGaussianSplatRenderProxy*> ShadowCasters;
	GatherRenderItems(GraphBuilder, View, RenderItems, &ShadowCasters);

	// Before the lighting passes, which read the shadow map through the sun's light function
	GaussianSplat::RenderShadows(GraphBuilder, View, ShadowCasters);

	FGaussianSplatSceneViewExtensionModule& Module = FModuleManager::GetModuleChecked<FGaussianSplatSceneViewExtensionModule>("GaussianSplatSceneViewExtension");
	GaussianSplat::RenderDeferred(GraphBuilder, View, RenderItems, Module.GetRenderStats_RenderThread(), Targets);

	// Remember the view even without items, so the post-process pass does not run the sources a second time
	const uint32 Frame = View.Family->FrameCounter;
	if (DeferredViewsFrame_RenderThread != Frame)
	{
		DeferredViews_RenderThread.Reset();
		DeferredViewsFrame_RenderThread = Frame;
	}
	DeferredViews_RenderThread.AddUnique(&View);
}

bool FGaussianSplatViewExtension::WasRenderedDeferred(const FSceneView& View) const
{
	return View.Family != nullptr
		&& DeferredViewsFrame_RenderThread == View.Family->FrameCounter
		&& DeferredViews_RenderThread.Contains(&View);
}

void FGaussianSplatViewExtension::GatherRenderItems(FRDGBuilder& GraphBuilder, const FSceneView& View, TArray<FGaussianSplatRenderItem>& RenderItems, TArray<const FGaussianSplatRenderProxy*>* OutShadowCasters)
{
	FGaussianSplatSceneViewExtensionModule& Module = FModuleManager::GetModuleChecked<FGaussianSplatSceneViewExtensionModule>("GaussianSplatSceneViewExtension");

	// Registered components first, then whatever external sources build into this frame's graph.
	TArray<const FGaussianSplatRenderProxy*> Proxies;
	Proxies.Reserve(RenderProxy_RenderThread.Num());
	for (auto Itr = RenderProxy_RenderThread.CreateIterator(); Itr; ++Itr)
	{
		Proxies.Add(Itr.Value().Get());
	}
	ExternalProxySources_RenderThread.Broadcast(GraphBuilder, View, Proxies);

	TSortedMap<GaussianSplat::EInverseToneCurveLUTVariant, FRHITexture*> LUTRHI;
	RenderItems.Reset(Proxies.Num());
	for (const FGaussianSplatRenderProxy* RenderProxy : Proxies)
	{
		if (OutShadowCasters && RenderProxy->bCastShadow && RenderProxy->GpuBuffer.IsValid())
		{
			OutShadowCasters->Add(RenderProxy);
		}
		if (!RenderProxy->bVisible || !RenderProxy->GpuBuffer.IsValid())
		{
			continue;
		}

		FRHITexture* ProxyLUTRHI = nullptr;
		if (RenderProxy->InverseToneCurveStrength > 0.0f)
		{
			const GaussianSplat::EInverseToneCurveLUTVariant Variant = static_cast<GaussianSplat::EInverseToneCurveLUTVariant>(RenderProxy->bInvertExpandGamutLUT);
			FRHITexture*& CachedLUTRHI = LUTRHI.FindOrAdd(Variant);
			if (CachedLUTRHI == nullptr)
			{
				CachedLUTRHI = Module.GetInverseToneCurveLUTRHI_RenderThread(RenderProxy->bInvertExpandGamutLUT);
			}
			ProxyLUTRHI = CachedLUTRHI;
		}

		RenderItems.Add({ RenderProxy, ProxyLUTRHI });
	}

	// Sort by bounding box centroid
	const FVector ViewOrigin = View.ViewMatrices.GetViewOrigin();
	const FVector ViewDirection = View.GetViewDirection();
	auto GetSortDistance = [&ViewOrigin, &ViewDirection](const FGaussianSplatRenderProxy& Proxy)
	{
		// Proxies without bounds are drawn last
		if (!Proxy.LocalBounds.IsValid)
		{
			return TNumericLimits<double>::Lowest();
		}

		const FVector Center = FVector(FVector3f(Proxy.LocalToWorld.TransformPosition(Proxy.LocalBounds.GetCenter())));
		const FVector Distance = Center - ViewOrigin;
		const double SortDistance = FVector::DotProduct(Distance, ViewDirection);

		// A NaN distance is treated similarly to a proxy without bounds
		return FMath::IsFinite(SortDistance) ? SortDistance : TNumericLimits<double>::Lowest();
	};
	RenderItems.StableSort([&GetSortDistance](const FGaussianSplatRenderItem& A, const FGaussianSplatRenderItem& B)
	{
		return GetSortDistance(*A.Proxy) > GetSortDistance(*B.Proxy);
	});
}

void FGaussianSplatViewExtension::RegisterComponent(const UGaussianSplatComponent* Component)
{
	if (!Component->HasValidSplatData() || RegisteredComponents.Contains(Component))
	{
		return;
	}
	RegisteredComponents.Add(Component);

	// Create render proxy on render thread
	const uint32 UniqueID = GetUniqueID(Component);
	if (UniqueID != INVALID_ID)
	{
		TUniquePtr<FGaussianSplatRenderProxy> RenderProxy(CreateRenderProxy(Component));
		TSharedPtr<FGaussianSplatRenderResource> Resource = Component->Asset->GetResource();
		const int32 NumSplatsCVar = CVarGaussianSplatNumSplats.GetValueOnGameThread();

		ENQUEUE_RENDER_COMMAND(FAddGaussianSplatRenderProxy)(
			[
				WeakThis = AsWeak(),
				UniqueID,
				RenderProxy = MoveTemp(RenderProxy),
				Resource,
				NumSplatsCVar
			](FRHICommandList& RHICmdList) mutable
			{
				RenderProxy->NumSplats = uint32(Resource->GetNumSplats());
				RenderProxy->NumLOD0Splats = uint32(Resource->GetNumLOD0Splats());
				RenderProxy->LocalBounds = Resource->GetBounds();

				bool bDebugDisableClustering = false;
				if (NumSplatsCVar > 0 && uint32(NumSplatsCVar) < RenderProxy->NumSplats)
				{
					RenderProxy->NumSplats = uint32(NumSplatsCVar);
					bDebugDisableClustering = true;
				}

				RenderProxy->GpuBuffer = Resource->GetSplatBuffer();
				RenderProxy->GpuDataStride = Resource->GetSplatStride();
				if (!bDebugDisableClustering)
				{
					RenderProxy->ClusterBuffer = Resource->GetClusterBuffer();
					RenderProxy->NumClusters = Resource->GetNumClusters();
				}

				TSharedPtr<FGaussianSplatViewExtension> This = StaticCastWeakPtr<FGaussianSplatViewExtension>(WeakThis).Pin();
				if (This.IsValid())
				{
					This->RenderProxy_RenderThread.Add(UniqueID, MoveTemp(RenderProxy));
				}
			}
		);
	}
}

void FGaussianSplatViewExtension::UnregisterComponent(const UGaussianSplatComponent* Component)
{
	RegisteredComponents.Remove(Component);

	// Remove render proxy on render thread
	const uint32 UniqueID = Component->GetUniqueID(); // bypass asset check
	ENQUEUE_RENDER_COMMAND(FRemoveGaussianSplatRenderProxy)(
		[
			WeakThis = AsWeak(),
			UniqueID
		](FRHICommandList& RHICmdList)
		{
			TSharedPtr<FGaussianSplatViewExtension> This = StaticCastWeakPtr<FGaussianSplatViewExtension>(WeakThis).Pin();
			if (This.IsValid())
			{
				This->RenderProxy_RenderThread.Remove(UniqueID); // auto-deletes via TUniquePtr
			}
		}
	);
}

void FGaussianSplatViewExtension::AddExternalProxySource(FOnCollectGaussianSplatProxies::FDelegate&& InSource)
{
	ENQUEUE_RENDER_COMMAND(FAddGaussianSplatExternalProxySource)(
		[WeakThis = AsWeak(), Source = MoveTemp(InSource)](FRHICommandList&) mutable
		{
			if (TSharedPtr<FGaussianSplatViewExtension> This = StaticCastWeakPtr<FGaussianSplatViewExtension>(WeakThis).Pin())
			{
				This->ExternalProxySources_RenderThread.Add(MoveTemp(Source));
			}
		}
	);
}

FGaussianSplatViewExtension* GaussianSplat::FindViewExtension(UWorld* World)
{
	UGaussianSplatSubsystem* Subsystem = UWorld::GetSubsystem<UGaussianSplatSubsystem>(World);
	return (IsValid(Subsystem) && Subsystem->SceneViewExtension.IsValid()) ? Subsystem->SceneViewExtension.Get() : nullptr;
}

void GaussianSplat::RegisterExternalProxySource(UWorld* World, FOnCollectGaussianSplatProxies::FDelegate&& Source)
{
	if (FGaussianSplatViewExtension* Extension = FindViewExtension(World))
	{
		Extension->AddExternalProxySource(MoveTemp(Source));
	}
}
