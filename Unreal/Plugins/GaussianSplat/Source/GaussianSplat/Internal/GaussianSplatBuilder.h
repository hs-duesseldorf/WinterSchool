// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GaussianSplat.h"

#define UE_API GAUSSIANSPLAT_API

struct FGaussianSplatBuilder
{
	// Returns false is we have no data / the bounds are invalid / data is invalid
	UE_API bool IsValid() const;

	// Gets the number of splats in the data set
	UE_API int32 GetNumSplats() const;

	// Sets the SH degree for the data set
	// Note that this can only ever remove SH data it can not add data
	UE_API void SetSHDegree(int32 InSHDegree);

	// Get the current dimension from the degree (i.e. the number of expected coefficients)
	UE_API int32 GetSHDimension() const;

	// Calculates the bounds from current data set
	UE_API void CalculateBounds();

	// Splat buffer budget assets are authored against: the D3D12 byte address view limit. RHIs with a smaller
	// limit fall back at resource creation instead of restricting import.
	UE_API static uint64 GetMaxBufferBytes();

	// Maximum number of runtime splats, across every LOD level, that fit GetMaxBufferBytes
	UE_API static int64 GetMaxSplatCount(int32 SHDimension, bool bQuantizedFormat);

	FBox3f												Bounds = FBox3f(EForceInit::ForceInit);
	int32												SHDegree = 0;
	EGaussianSplatCoordinateSpace				CoordinateSpace = EGaussianSplatCoordinateSpace::LH_FRU;
	TArray<FGaussianSplatFormatFloat::FBaseData>	BaseData;
	TArray<FVector3f>									SHData;

};

#undef UE_API
