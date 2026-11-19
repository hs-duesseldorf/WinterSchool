// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GaussianSplatCluster.h"
#include "GaussianSplatFormats.h"

#define UE_API GAUSSIANSPLAT_API

namespace GaussianSplat
{
#if WITH_EDITORONLY_DATA

	UE_API void BuildLevelsOfDetail(FGaussianSplatFormatFloat& Splats, EGaussianSplatAggregation Mode);
	UE_API int64 BoundLevelOfDetailSplatCount(int32 NumSplats, int32 MaxSplatsPerCluster);

#endif //WITH_EDITORONLY_DATA
}

#undef UE_API
