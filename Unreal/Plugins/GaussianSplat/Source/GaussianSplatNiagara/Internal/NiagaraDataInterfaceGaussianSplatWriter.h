// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GaussianSplatCommon.h"
#include "NiagaraDataInterfaceRW.h"
#include "NiagaraDataInterfaceGaussianSplatWriter.generated.h"

#define UE_API GAUSSIANSPLATNIAGARA_API

USTRUCT()
struct FNiagaraGaussianSplatWriterBounds
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Bounds")
	FVector Min = FVector(-1000.0);

	UPROPERTY(EditAnywhere, Category = "Bounds")
	FVector Max = FVector(1000.0);

	bool operator==(const FNiagaraGaussianSplatWriterBounds& Other) const { return Min == Other.Min && Max == Other.Max; }
};

UCLASS(EditInlineNew, meta = (DisplayName = "Gaussian Splat Writer", Experimental, Valkyrie_DevelopmentStatus = "Experimental", UEFN_DevelopmentStatus = "Experimental"), Blueprintable, BlueprintType, MinimalAPI)
class UNiagaraDataInterfaceGaussianSplatWriter : public UNiagaraDataInterfaceRWBase
{
public:
	GENERATED_UCLASS_BODY()

	//~ Begin UObject interface
	UE_API virtual void PostInitProperties() override;
	//~ End UObject interface

	//~ Begin UNiagaraDataInterface interface
	virtual bool CanExecuteOnTarget(ENiagaraSimTarget Target) const override { return true; }
	UE_API virtual bool Equals(const UNiagaraDataInterface* Other) const override;
	UE_API virtual bool CopyToInternal(UNiagaraDataInterface* Destination) const override;

	UE_API virtual int32 PerInstanceDataSize() const override;
	UE_API virtual bool InitPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance) override;
	UE_API virtual void DestroyPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance) override;
	virtual bool HasPreSimulateTick() const override { return true; }
	virtual bool HasPostSimulateTick() const override { return true; }
	UE_API virtual bool PerInstanceTick(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance, float DeltaSeconds) override;
	UE_API virtual bool PerInstanceTickPostSimulate(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance, float DeltaSeconds) override;
#if WITH_EDITOR
	UE_API virtual void GetFeedback(UNiagaraSystem* InAsset, UNiagaraComponent* InComponent, TArray<FNiagaraDataInterfaceError>& OutErrors, TArray<FNiagaraDataInterfaceFeedback>& OutWarnings, TArray<FNiagaraDataInterfaceFeedback>& OutInfo) override;
#endif

protected:
#if WITH_EDITORONLY_DATA
	UE_API virtual void GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const override;
#endif
public:
	UE_API virtual void GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, void* InstanceData, FVMExternalFunction& OutFunc) override;
#if WITH_EDITORONLY_DATA
	UE_API virtual bool AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const override;
	UE_API virtual void GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, FString& OutHLSL) override;
	UE_API virtual bool GetFunctionHLSL(const FNiagaraDataInterfaceHlslGenerationContext& HlslGenContext, FString& OutHLSL) override;
#endif
	UE_API virtual void BuildShaderParameters(FNiagaraShaderParametersBuilder& ShaderParametersBuilder) const override;
	UE_API virtual void SetShaderParameters(const FNiagaraDataInterfaceSetShaderParametersContext& Context) const override;
	//~ End UNiagaraDataInterface interface

	UPROPERTY(EditAnywhere, Category = "Output", meta = (ClampMin = "0"))
	TOptional<int32> NumSplats;

	/** Spherical harmonic degree */
	UPROPERTY(EditAnywhere, Category = "Output", meta = (ClampMin = "0", ClampMax = "3"))
	TOptional<int32> SHDegree;

	/** Coordinate space for spherical harmonic evaluation */
	UPROPERTY()
	TOptional<EGaussianSplatCoordinateSpace> SHCoordinateSpace;

	/** Quantization bounds for position */
	UPROPERTY(EditAnywhere, Category = "Quantization")
	TOptional<FNiagaraGaussianSplatWriterBounds> PositionBounds;

	/** Quantization bounds for scale */
	UPROPERTY(EditAnywhere, Category = "Quantization", meta = (ClampMin = "0.0001", DefaultSome = "100.0"))
	TOptional<float> MaxScale;

	/** Quantization max for spherical harmonic coefficients */
	UPROPERTY(EditAnywhere, Category = "Quantization", meta = (ClampMin = "0.01"))
	TOptional<float> MaxSHCoefficient;

	/** Uniform scale multiplier */
	UPROPERTY(EditAnywhere, Category = "Splat Rendering", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "100.0"))
	float SplatScale = 1.0f;

	/** Uniform opacity multiplier */
	UPROPERTY(EditAnywhere, Category = "Splat Rendering", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "1.0"))
	float OpacityScale = 1.0f;

	/** Inverts Unreal's tone curve. Matches the Gaussian Splat component. */
	UPROPERTY(EditAnywhere, Category = "Splat Rendering", meta = (DisplayName = "Invert UE Tone Curve"))
	bool bInvertToneCurve = false;

	/** Modifier for tone curve. Matches the Gaussian Splat component. */
	UPROPERTY(EditAnywhere, Category = "Splat Rendering", AdvancedDisplay, meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bInvertToneCurve", EditConditionHides))
	float InverseToneCurveStrength = 1.0f;

	/** Undoes default expand gamut step. Matches the Gaussian Splat component. */
	UPROPERTY(EditAnywhere, Category = "Splat Rendering", AdvancedDisplay, meta = (DisplayName = "Invert Expand Gamut", EditCondition = "bInvertToneCurve", EditConditionHides))
	bool bInvertExpandGamut = true;
};

#undef UE_API
