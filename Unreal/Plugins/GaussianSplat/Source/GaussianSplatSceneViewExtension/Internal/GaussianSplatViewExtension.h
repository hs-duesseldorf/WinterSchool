// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "Containers/SortedMap.h"
#include "Containers/SortedSet.h"

class UGaussianSplatComponent;
class FGaussianSplatRenderProxy;
struct FGaussianSplatRenderItem;
class FGaussianSplatViewExtension;
class FPostOpaqueRenderParameters;
class FViewInfo;
struct FScreenPassRenderTarget;
struct FGaussianSplatDeferredTargets;

#define UE_API GAUSSIANSPLATSCENEVIEWEXTENSION_API

/**
 * Source of splats with no UGaussianSplatComponent behind them, e.g. a Niagara emitter converting its particles.
 * Broadcast on the render thread once per rendered view from inside the splat post process pass, after the frame's
 * simulation work has been submitted, so a source can add passes to the graph before appending the proxies to draw.
 *
 * Appended proxies are read until the splat pass for the view has been set up, on the same call stack, so sources
 * should own them persistently. Bind with CreateSP: the binding expires with the shared object and that is the only
 * deregistration, so everything the bound function reads must be destroyed on the render thread or be kept alive
 * by that object.
 */
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnCollectGaussianSplatProxies, FRDGBuilder&, const FSceneView&, TArray<const FGaussianSplatRenderProxy*>&);

namespace GaussianSplat
{
	/** Binds a source to the splat renderer of World. Callable from the game thread or a game thread task; no-op for worlds without one. */
	UE_API void RegisterExternalProxySource(UWorld* World, FOnCollectGaussianSplatProxies::FDelegate&& Source);

	/** The splat renderer of World, or null if it has none. Not exported; callers in other modules use the entry points above. */
	FGaussianSplatViewExtension* FindViewExtension(UWorld* World);
}

class FGaussianSplatViewExtension : public FWorldSceneViewExtension
{
public:
	FGaussianSplatViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld);
	~FGaussianSplatViewExtension();

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;
	virtual void PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView, const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;

	void RegisterComponent(const UGaussianSplatComponent* InComponent);
	void UnregisterComponent(const UGaussianSplatComponent* InComponent);
	void AddExternalProxySource(FOnCollectGaussianSplatProxies::FDelegate&& InSource);

	/**
	 * Post-opaque render delegate of the renderer module, bound once by the module. Draws the splats into the GBuffer on
	 * mobile deferred (multipass, e.g. the WebGPU deferred tier), which has no PostRenderBasePassDeferred_RenderThread:
	 * there the post-opaque extensions run after the base pass and before decals, AO and the lighting passes.
	 */
	static void PostOpaqueRender_RenderThread(FPostOpaqueRenderParameters& Parameters);

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

	FScreenPassTexture PostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& InOutInputs);
	void Render(FRDGBuilder& GraphBuilder, const FSceneView& View, const FScreenPassRenderTarget& Output, FRDGTexture* SceneDepthTexture, FRDGTexture* SceneVelocityTexture);
	/**
	 * Visible proxies (registered + external sources) of this frame, with their LUTs, ordered back to front by bounds centre.
	 * OutShadowCasters, if given, receives the proxies that cast a shadow, visible or not.
	 */
	void GatherRenderItems(FRDGBuilder& GraphBuilder, const FSceneView& View, TArray<FGaussianSplatRenderItem>& OutRenderItems, TArray<const FGaussianSplatRenderProxy*>* OutShadowCasters = nullptr);
	/** Draws View's splats into Targets and remembers the view for WasRenderedDeferred. */
	void RenderDeferred_RenderThread(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FGaussianSplatDeferredTargets& Targets);
	/** True if this frame's base pass already drew View's splats into the GBuffer. */
	bool WasRenderedDeferred(const FSceneView& View) const;

private:
	TArray<const UGaussianSplatComponent*> RegisteredComponents;
	TSortedSet<uint32> Components_GameThread;
	TSortedMap<uint32, TUniquePtr<FGaussianSplatRenderProxy>> RenderProxy_RenderThread;
	FOnCollectGaussianSplatProxies ExternalProxySources_RenderThread;

	// Views whose splats went into the GBuffer this frame (render thread); they skip the post-process composite
	TArray<const FSceneView*, TInlineAllocator<4>> DeferredViews_RenderThread;
	uint32 DeferredViewsFrame_RenderThread = MAX_uint32;
};

#undef UE_API
