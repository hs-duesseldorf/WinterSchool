#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "DataDrivenShaderPlatformInfo.h"

class FSkinnedSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSkinnedSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FSkinnedSplatCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, RestSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, BoneDQ)
		SHADER_PARAMETER(uint32, NumSplats)
		SHADER_PARAMETER(uint32, SplatStride)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, RWSplatBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 64;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::ES3_1);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), ThreadGroupSize);
	}
};
