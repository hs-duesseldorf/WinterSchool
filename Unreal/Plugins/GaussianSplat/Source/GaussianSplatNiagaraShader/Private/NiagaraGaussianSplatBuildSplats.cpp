// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraGaussianSplatBuildSplats.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "Modules/ModuleManager.h"

static bool GNiagaraGaussianSplatShaderEnabled = false;

class FGaussianSplatNiagaraShaderModule : public IModuleInterface
{
public:
	virtual void StartupModule() override { GNiagaraGaussianSplatShaderEnabled = true; }
};

bool FNiagaraBuildGaussianSplatsCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return GNiagaraGaussianSplatShaderEnabled && FDataDrivenShaderPlatformInfo::GetSupportsComputeFramework(Parameters.Platform);
}

void FNiagaraBuildGaussianSplatsCS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), ThreadGroupSize);
}

IMPLEMENT_GLOBAL_SHADER(FNiagaraBuildGaussianSplatsCS, "/Plugin/GaussianSplat/Private/NiagaraGaussianSplatBuildSplats.usf", "MainCS", SF_Compute);

IMPLEMENT_MODULE(FGaussianSplatNiagaraShaderModule, GaussianSplatNiagaraShader)
