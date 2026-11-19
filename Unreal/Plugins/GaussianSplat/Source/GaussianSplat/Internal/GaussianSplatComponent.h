// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/PrimitiveComponent.h"
#include "GaussianSplat.h"

#include "GaussianSplatComponent.generated.h"

#define UE_API GAUSSIANSPLAT_API

class IGaussianSplatRenderer;

/**
 * Draws a Gaussian splat asset. A primitive component (WinterSchool fork) so it has Unreal's usual rendering settings, bounds and
 * selection: Cast Shadow / Hidden Shadow switch its splats in the splat shadow map (see GaussianSplatShadow.cpp). It has no
 * scene proxy, the splat renderer draws it.
 */
UCLASS(BlueprintType, PrioritizeCategories = "GaussianSplat", meta = (Valkyrie_DevelopmentStatus = "Experimental", UEFN_DevelopmentStatus = "Experimental"))
class UGaussianSplatComponent : public UPrimitiveComponent
{
	GENERATED_UCLASS_BODY()

public:

	UPROPERTY(EditAnywhere, Category = "GaussianSplat")
	TObjectPtr<UGaussianSplat> Asset;

	UPROPERTY(EditAnywhere, Category = "GaussianSplat", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PointScaleFactor;

	UPROPERTY(EditAnywhere, Category = "GaussianSplat", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OpacityScale;

	/** Highest spherical harmonics degree shaded (0 = view independent base color, up to the asset's degree, max 3). Lower is faster. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GaussianSplat", meta = (DisplayName = "Max SH Degree", ClampMin = "0", ClampMax = "3", UIMin = "0", UIMax = "3"))
	int32 MaxSHDegree;

	/**
	 * Option to preserve original SDR splats colors by inverting UE's tonemapper filmic tone curve.
	 * 
	 * The filmic curve inversion (with default blue correction) is approximated with a pre-baked LUT.
	 * Note that the more accurate standard ACES 2.0 inversion is not yet supported.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GaussianSplat", meta = (DisplayName = "Invert UE Tone Curve"))
	bool bInvertToneCurve;

	// 0 leaves the splat color untouched; 1 applies the full LUT inverse.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GaussianSplat", AdvancedDisplay, meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", EditCondition = "bInvertToneCurve", EditConditionHides))
	float InverseToneCurveStrength;

	// When enabled, the tone-curve inversion also undoes UE's default expand-gamut step.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GaussianSplat", AdvancedDisplay, meta = (DisplayName = "Invert Default Expand Gamut", EditCondition = "bInvertToneCurve", EditConditionHides))
	bool bInvertExpandGamut;

	/**
	 * How much the scene's lights (and the shadows other objects cast in them) affect this splat (WinterSchool fork).
	 * Lit splats are written into the deferred GBuffer, so every Unreal light, shadow method, sky light and fog applies to them.
	 * The captured colors are kept where lighting is unchanged: each pixel is split into a diffuse albedo that reproduces the
	 * capture under the scene's directional and sky light, plus an emissive remainder. Occluding that light (e.g. a character's
	 * shadow) darkens the splat, other lights add to it.
	 * 0 = unlit (captured colors only, composited after the opaque pass), 1 = fully lit. Needs a deferred renderer
	 * (desktop / WebGPU SM5); mobile renderers always show splats unlit. See r.GaussianSplat.Lighting.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GaussianSplat|Lighting", meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float LightingStrength;

	/**
	 * How dark the directional light's shadows on this splat are (WinterSchool fork): the share of the captured color that
	 * comes from the sun and that a full shadow takes away. 0 = no shadow, 0.75 = a shadow keeps a quarter of the color,
	 * 1 = black. Independent of the light's intensity, which sets how bright the capture's sun is taken to be, i.e. how
	 * strongly other lights brighten the splat. The shadow's hue comes from the light colours (sky light vs directional light,
	 * like a real shadow lit by the sky alone). Also scaled by r.GaussianSplat.Lighting.ShadowStrength.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GaussianSplat|Lighting", meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float ShadowStrength;

public:

	UE_API bool HasValidSplatData() const;

	//~ Begin UActorComponent interface
	UE_API virtual void OnRegister() override;
	UE_API virtual void OnUnregister() override;
	//~ End UActorComponent interface

	//~ Begin USceneComponent interface
	UE_API virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	//~ End USceneComponent interface

private:
	/** The renderer chosen at OnRegister; cleared on OnUnregister. Lifetime owned by the renderer's module. */
	IGaussianSplatRenderer* ActiveRenderer = nullptr;
};

#undef UE_API
