#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "MaterialShader.h"
#include "MaterialShaderType.h"
#include "ShaderParameterStruct.h"
#include "DataDrivenShaderPlatformInfo.h"

/** Dual-quaternion skinning of the capture-pose splats into GaussianSplat's float records. */
class FSkinnedSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSkinnedSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FSkinnedSplatCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, RestSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, BoneDQ)
		SHADER_PARAMETER(uint32, NumSplats)
		SHADER_PARAMETER(uint32, SplatStride)
		SHADER_PARAMETER(float, Brightness)
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

/**
 * Per-splat material evaluation. Compiled only for surface materials flagged "Used with Lidar Point Cloud" (the engine
 * has no generic usage flag; ASkinnedSplatActor sets it automatically when a material is assigned), so other materials
 * don't pay for an extra shader.
 */
class FSkinnedSplatMaterialCS : public FMaterialShader
{
	DECLARE_SHADER_TYPE(FSkinnedSplatMaterialCS, Material);
	SHADER_USE_PARAMETER_STRUCT_WITH_LEGACY_BASE(FSkinnedSplatMaterialCS, FMaterialShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(FMatrix44f, WorldToLocal)
		SHADER_PARAMETER(FVector3f, LocalBoundsOrigin)
		SHADER_PARAMETER(FVector3f, LocalBoundsExtent)
		SHADER_PARAMETER(uint32, NumSplats)
		SHADER_PARAMETER(uint32, SplatStride)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, RWSplatBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 64;

	static bool ShouldCompilePermutation(const FMaterialShaderPermutationParameters& Parameters)
	{
		return Parameters.MaterialParameters.MaterialDomain == MD_Surface
			&& Parameters.MaterialParameters.bIsUsedWithLidarPointCloud
			&& IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::ES3_1);
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), ThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("GET_PRIMITIVE_DATA_OVERRIDE"), 1);
		OutEnvironment.SetDefine(TEXT("USE_INSTANCE_CULLING"), 0);
		OutEnvironment.SetDefine(TEXT("SUBSTRATE_INLINE_SHADING"), 1);
	}
};
