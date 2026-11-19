// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "NiagaraDataInterface.h"
#include "NiagaraDataInterfaceGaussianSplatReader.generated.h"

#define UE_API GAUSSIANSPLATNIAGARA_API

class UGaussianSplat;

UCLASS(EditInlineNew, meta = (DisplayName = "Gaussian Splat Reader", Experimental, Valkyrie_DevelopmentStatus = "Experimental", UEFN_DevelopmentStatus = "Experimental"), Blueprintable, BlueprintType, MinimalAPI)
class UNiagaraDataInterfaceGaussianSplatReader : public UNiagaraDataInterface
{
public:
	GENERATED_UCLASS_BODY()

	// UObject Interface
	UE_API virtual void PostInitProperties() override;
	// UObject Interface End

	// UNiagaraDataInterface Interface Begin
	virtual bool CanExecuteOnTarget(ENiagaraSimTarget Target) const override { return true; }

	UE_API virtual bool Equals(const UNiagaraDataInterface* Other) const override;
	UE_API virtual bool CopyToInternal(UNiagaraDataInterface* Destination) const override;

	UE_API virtual bool InitPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance) override;
	UE_API virtual void DestroyPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance) override;
	UE_API virtual int32 PerInstanceDataSize() const override;

protected:
#if WITH_EDITORONLY_DATA
	UE_API virtual void GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const override;
#endif
public:
	UE_API virtual void GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, void* InstanceData, FVMExternalFunction& OutFunc) override;
#if WITH_EDITORONLY_DATA
	UE_API virtual void GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, FString& OutHLSL) override;
	UE_API virtual bool GetFunctionHLSL(const FNiagaraDataInterfaceHlslGenerationContext& HlslGenContext, FString& OutHLSL) override;
	UE_API virtual bool AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const override;
#endif
	UE_API virtual void BuildShaderParameters(FNiagaraShaderParametersBuilder& ShaderParametersBuilder) const override;
	UE_API virtual void SetShaderParameters(const FNiagaraDataInterfaceSetShaderParametersContext& Context) const override;
	// UNiagaraDataInterface Interface End

	//-TODO: Temp until we have a user parameter binding (which requires per instance data)
	UE_API static void SetGaussianSplat(UNiagaraComponent* Component, const FName& ParameterName, UGaussianSplat* GaussianSplat);

	UE_API static TOptional<int32> GetGaussianSplatMaxSHDegree(UNiagaraComponent* Component, const FName& ParameterName);
	UE_API static void SetGaussianSplatMaxSHDegree(UNiagaraComponent* Component, const FName& ParameterName, int32 Value);

	/** Backing asset */
	UE_API UGaussianSplat* GetGaussianSplat() const;
	/** Spherical harmonic degree */
	UE_API int32 GetSHDegree() const;

protected:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Data")
	TObjectPtr<UGaussianSplat> GaussianSplat;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Data")
	TOptional<int32> MaxSHDegree;
};

#undef UE_API
