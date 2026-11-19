// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

class FNiagaraBuildGaussianSplatsCS : public FGlobalShader
{
	DECLARE_EXPORTED_GLOBAL_SHADER(FNiagaraBuildGaussianSplatsCS, GAUSSIANSPLATNIAGARASHADER_API);
	SHADER_USE_PARAMETER_STRUCT(FNiagaraBuildGaussianSplatsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Buffer<float>, NiagaraParticleDataFloat)
		SHADER_PARAMETER_SRV(Buffer<float>, NiagaraParticleDataHalf)
		SHADER_PARAMETER_SRV(Buffer<int>, NiagaraParticleDataInt)
		SHADER_PARAMETER(uint32, NiagaraFloatDataStride)
		SHADER_PARAMETER(uint32, NiagaraIntDataStride)

		SHADER_PARAMETER(int32, PositionRegister)
		SHADER_PARAMETER(int32, ScaleRegister)
		SHADER_PARAMETER(int32, RotationRegister)
		SHADER_PARAMETER(int32, ColorRegister)
		SHADER_PARAMETER(int32, VisibilityTagRegister)
		SHADER_PARAMETER(FVector3f, DefaultPosition)
		SHADER_PARAMETER(FVector3f, DefaultScale)
		SHADER_PARAMETER(FVector4f, DefaultRotation)
		SHADER_PARAMETER(FVector4f, DefaultColor)
		SHADER_PARAMETER(int32, DefaultVisibilityTag)
		SHADER_PARAMETER(int32, RendererVisibility)

		SHADER_PARAMETER_SRV(Buffer<uint>, ParticleCountBuffer)
		SHADER_PARAMETER(uint32, ParticleCountOffset)
		SHADER_PARAMETER(uint32, ParticleCount)

		SHADER_PARAMETER(uint32, MaxSplats)
		SHADER_PARAMETER(uint32, SplatStride)
		SHADER_PARAMETER(FMatrix44f, WorkingColorSpaceToSRGB)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, RWSplatBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 64;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};
