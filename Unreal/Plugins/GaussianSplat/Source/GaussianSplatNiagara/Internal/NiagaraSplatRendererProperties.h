// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NiagaraRendererProperties.h"

#include "NiagaraSplatRendererProperties.generated.h"

#define UE_API GAUSSIANSPLATNIAGARA_API

/**
 * Emits each particle as a 3D Gaussian, rendered by the Gaussian Splat plugin. Each particle contributes a flat color.
 */
UCLASS(editinlinenew, MinimalAPI, meta = (DisplayName = "Splat Renderer", Experimental, Valkyrie_DevelopmentStatus = "Experimental", UEFN_DevelopmentStatus = "Experimental"))
class UNiagaraSplatRendererProperties : public UNiagaraRendererProperties
{
public:
	GENERATED_BODY()

	UE_API UNiagaraSplatRendererProperties();

	//~ Begin UObject interface
	UE_API virtual void PostLoad() override;
	UE_API virtual void PostInitProperties() override;
	//~ End UObject interface

	/** Bindings cannot be set up until the Niagara module has registered its attributes. */
	static UE_API void InitCDOPropertiesAfterModuleStartup();

	//~ Begin UNiagaraRendererProperties interface
	UE_API virtual FNiagaraRenderer* CreateEmitterRenderer(ERHIFeatureLevel::Type FeatureLevel, const FNiagaraEmitterInstance* Emitter, const FNiagaraSystemInstanceController& InController) override;
	UE_API virtual FNiagaraBoundsCalculator* CreateBoundsCalculator() override;
	virtual void GetUsedMaterials(const FNiagaraEmitterInstance* InEmitter, TArray<UMaterialInterface*>& OutMaterials) const override {}
	virtual bool IsSimTargetSupported(ENiagaraSimTarget InSimTarget) const override { return true; }
	UE_API virtual void CacheFromCompiledData(const FNiagaraDataSetCompiledData* CompiledData) override;
	UE_API virtual bool PopulateRequiredBindings(FNiagaraParameterStore& InParameterStore) override;
#if WITH_EDITORONLY_DATA
	UE_API virtual const TArray<FNiagaraVariable>& GetOptionalAttributes() override;
	UE_API virtual const FSlateBrush* GetStackIcon() const override;
	UE_API virtual void GetRendererTooltipWidgets(const FNiagaraEmitterInstance* InEmitter, TArray<TSharedPtr<SWidget>>& OutWidgets, TSharedPtr<FAssetThumbnailPool> InThumbnailPool) const override;
	UE_API virtual void GetRendererFeedback(const FVersionedNiagaraEmitter& InEmitter, TArray<FNiagaraRendererFeedback>& OutErrors, TArray<FNiagaraRendererFeedback>& OutWarnings, TArray<FNiagaraRendererFeedback>& OutInfo) const override;
#endif
	//~ End UNiagaraRendererProperties interface

	/** Uniform multiplier on every splat */
	UPROPERTY(EditAnywhere, Category = "Splat Rendering", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "100.0"))
	float SplatScale = 1.0f;

	/** Uniform multiplier on every splat's opacity, applied before the pipeline's opacity threshold. */
	UPROPERTY(EditAnywhere, Category = "Splat Rendering", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "1.0"))
	float OpacityScale = 1.0f;

	/** Inverts Unreal's filmic tone curve before compositing. Matches Gaussian Splat component. */
	UPROPERTY(EditAnywhere, Category = "Splat Rendering", meta = (DisplayName = "Invert UE Tone Curve"))
	bool bInvertToneCurve = false;

	/** 0 leaves the splat color untouched; 1 applies the full inverse tone curve. */
	UPROPERTY(EditAnywhere, Category = "Splat Rendering", AdvancedDisplay, meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bInvertToneCurve", EditConditionHides))
	float InverseToneCurveStrength = 1.0f;

	/** When enabled the tone curve inversion also undoes default expand gamut step. */
	UPROPERTY(EditAnywhere, Category = "Splat Rendering", AdvancedDisplay, meta = (DisplayName = "Invert Expand Gamut", EditCondition = "bInvertToneCurve", EditConditionHides))
	bool bInvertExpandGamut = true;

	/** If a render visibility tag is present, particles whose tag matches this value are emitted. */
	UPROPERTY(EditAnywhere, Category = "Visibility")
	int32 RendererVisibility = 0;

	/** Center of the Gaussian. */
	UPROPERTY(EditAnywhere, Category = "Bindings")
	FNiagaraVariableAttributeBinding PositionBinding;

	/** Per axis standard deviation of the Gaussian, in simulation units, before Splat Scale. */
	UPROPERTY(EditAnywhere, Category = "Bindings")
	FNiagaraVariableAttributeBinding ScaleBinding;

	/** Orientation of the Gaussian's axes. Unnormalized or zero quaternions fall back to identity. */
	UPROPERTY(EditAnywhere, Category = "Bindings")
	FNiagaraVariableAttributeBinding RotationBinding;

	/** RGB becomes the splat's flat color, alpha its opacity. */
	UPROPERTY(EditAnywhere, Category = "Bindings")
	FNiagaraVariableAttributeBinding ColorBinding;

	/** Compared against Renderer Visibility to decide whether a particle is emitted. */
	UPROPERTY(EditAnywhere, Category = "Bindings")
	FNiagaraVariableAttributeBinding RendererVisibilityTagBinding;
};

#undef UE_API
