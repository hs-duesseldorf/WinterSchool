// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Templates/RefCounting.h"
#include "GaussianSplatCommon.h"
#include "RenderGraphFwd.h"
#include "RHIShaderPlatform.h"

class FRDGBuilder;
class FSceneView;
class FRHITexture;
class FRDGPooledBuffer;
struct FScreenPassRenderTarget;

class FGaussianSplatRenderProxy
{
public:
	FGaussianSplatRenderProxy() = default;

	int32 SplatID = INDEX_NONE;
	uint32 NumSplats = 0;
	uint32 NumLOD0Splats = 0;
	int32  SHDegree = 0;     // of the data
	int32  MaxSHDegree = 3;  // shaded at most (component setting)
	EGaussianSplatCoordinateSpace SHCoordinateSpace = EGaussianSplatCoordinateSpace::LH_FRU;

	TRefCountPtr<FRDGPooledBuffer> GpuBuffer; // set on render thread in RegisterComponent
	uint32 GpuDataStride = 0;
	uint32 BufferFormat = 0;

	// Cluster table, null with a count of zero when the asset was built without clustering
	TRefCountPtr<FRDGPooledBuffer> ClusterBuffer; // set on render thread in RegisterComponent
	uint32 NumClusters = 0;

	FVector3f PositionScale = FVector3f::One();
	FVector3f PositionBias  = FVector3f::Zero();
	FVector3f ScaleScale	= FVector3f::One();
	FVector3f ScaleBias		= FVector3f::Zero();
	FVector3f SHScale		= FVector3f::One();
	FVector3f SHBias		= FVector3f::Zero();
	FVector4f ColorScale	= FVector4f::One();
	FVector4f ColorBias		= FVector4f::Zero();

	FMatrix44f LocalToWorld = FMatrix44f::Identity;
	FMatrix44f WorldToLocal = FMatrix44f::Identity;

	FBox3f LocalBounds = FBox3f(EForceInit::ForceInit);

	float PointScaleFactor = 1.0f;
	float OpacityScale = 1.0f;

	bool bVisible = true;
	// Drawn into the splat shadow map (WinterSchool fork); can be set while bVisible is false (hidden shadow)
	bool bCastShadow = true;
	float InverseToneCurveStrength = 0.0f;
	bool bInvertExpandGamutLUT = true;
	// How much scene lighting affects the splat, see UGaussianSplatComponent::LightingStrength (WinterSchool fork)
	float LightingStrength = 1.0f;
	// Share of the captured color a full directional light shadow takes away, see UGaussianSplatComponent::ShadowStrength
	float ShadowStrength = 0.75f;

	bool HasClusters() const { return NumClusters > 0 && ClusterBuffer.IsValid(); }
	uint32 GetNumLOD0Splats() const { return NumLOD0Splats > 0 ? FMath::Min(NumLOD0Splats, NumSplats) : NumSplats; }
};

struct FGaussianSplatRenderItem
{
	const FGaussianSplatRenderProxy* Proxy = nullptr;
	FRHITexture* InverseToneCurveLUTRHI = nullptr;
};

/** The base pass outputs lit splats are written into (WinterSchool fork), see GaussianSplat::RenderDeferred. */
struct FGaussianSplatDeferredTargets
{
	FRDGTextureRef SceneColor = nullptr;
	FRDGTextureRef GBufferA = nullptr;
	FRDGTextureRef GBufferB = nullptr;
	FRDGTextureRef GBufferC = nullptr;
	FRDGTextureRef SceneDepth = nullptr;
	FRDGTextureRef Velocity = nullptr; // optional (desktop): zeroed where splats are written
};

class FViewInfo;
class UGaussianSplatComponent;

namespace GaussianSplat
{
	class FRenderStats;

	bool IsHardwareRasterization();
	bool IsHardwareRasterization(EShaderPlatform ShaderPlatform);
	bool IsGPUDrivenPipeline();

	void Render(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		TConstArrayView<FGaussianSplatRenderItem> RenderItems,
		FRenderStats& RenderStats,
		const FScreenPassRenderTarget& Output,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneVelocityTexture
	);

	/**
	 * True if View draws its splats into the GBuffer right after the base pass (r.GaussianSplat.Lighting), so the deferred
	 * lights, their shadows, the sky light, fog, AO and TSR treat them like opaque surfaces. Covers the desktop deferred
	 * renderer and mobile deferred (the web's tier for desktop-class GPUs). False for forward shading (mobile forward on
	 * phones), unlit views and configurations the GBuffer path does not cover; those keep the post-process composite.
	 */
	bool UsesDeferredLighting(const FSceneView& View);

	/**
	 * Renders all items into the GBuffer: one global front-to-back sort and draw into scratch targets, then a resolve that
	 * writes depth, normal, albedo and the emissive remainder of each covered pixel. Called after the base pass.
	 */
	void RenderDeferred(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		TConstArrayView<FGaussianSplatRenderItem> RenderItems,
		FRenderStats& RenderStats,
		const FGaussianSplatDeferredTargets& Targets
	);

	/** True if the platform runs the GBuffer path (and the splat shadow map). */
	bool SupportsDeferredSplats(EShaderPlatform ShaderPlatform);

	/** Falloff of the splat kernel (r.GaussianSplat.Falloff), see GaussianSplatFalloff in GaussianSplatCommon.ush. */
	FVector3f GetSplatFalloffParameters();

	/** The component's splats go into the splat shadow map: Cast Shadow, and visible or Hidden Shadow. */
	bool ShouldCastShadow(const UGaussianSplatComponent& Component);

	/**
	 * Draws Casters into the splat shadow map of the sun, for View's family (WinterSchool fork, see GaussianSplatShadow.cpp).
	 * Called before the lighting passes; does nothing without a directional light using the splat shadow light function.
	 */
	void RenderShadows(FRDGBuilder& GraphBuilder, const FViewInfo& View, TConstArrayView<const FGaussianSplatRenderProxy*> Casters);
}
