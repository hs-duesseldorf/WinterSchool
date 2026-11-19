// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GaussianSplatFormats.h"

#define UE_API GAUSSIANSPLAT_API

struct FGaussianSplatBuilder;

namespace GaussianSplat
{
#if WITH_EDITORONLY_DATA

	inline uint64 DilateBits(uint32 Value)
	{
		uint64 Result = Value & 0x1fffffull;
		Result = (Result | (Result << 32)) & 0x1f00000000ffffull;
		Result = (Result | (Result << 16)) & 0x1f0000ff0000ffull;
		Result = (Result | (Result <<  8)) & 0x100f00f00f00f00full;
		Result = (Result | (Result <<  4)) & 0x10c30c30c30c30c3ull;
		Result = (Result | (Result <<  2)) & 0x1249249249249249ull;
		return Result;
	}

	// 21 bits per axis
	inline uint32 QuantizeUnitCoord(float UnitCoord)
	{
		constexpr float MaxCoord = float((1u << 21) - 1u);
		const float Scaled = UnitCoord * MaxCoord;
		return Scaled > 0.0f ? uint32(Scaled < MaxCoord ? Scaled : MaxCoord) : 0u;
	}

	inline uint64 EncodeMorton3(const FVector3f& UnitPosition)
	{
		const uint32 X = QuantizeUnitCoord(UnitPosition.X);
		const uint32 Y = QuantizeUnitCoord(UnitPosition.Y);
		const uint32 Z = QuantizeUnitCoord(UnitPosition.Z);
		return DilateBits(X) | (DilateBits(Y) << 1) | (DilateBits(Z) << 2);
	}

	UE_API void BuildMortonOrder(
		TConstArrayView<FGaussianSplatFormatFloat::FBaseData> BaseData,
		TArray<uint32>& OutSplatIndex);

	UE_API void SortMortonOrder(
		TArrayView<FGaussianSplatFormatFloat::FBaseData> BaseData,
		TArrayView<FVector3f> SHData,
		int32 SHDimension);

	UE_API void SortMortonOrder(
		TArray<FGaussianSplatFormatFloat::FBaseData>& BaseData,
		TArray<FVector3f>& SHData,
		int32 SHDimension);

	UE_API void SortMortonOrder(FGaussianSplatBuilder& Builder);

#endif //WITH_EDITORONLY_DATA
}

#undef UE_API
