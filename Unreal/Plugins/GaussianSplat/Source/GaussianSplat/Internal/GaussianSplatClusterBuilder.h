// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GaussianSplatCluster.h"
#include "GaussianSplatFormats.h"

#define UE_API GAUSSIANSPLAT_API

namespace GaussianSplat
{
#if WITH_EDITORONLY_DATA

	// Builds the level 0 cluster table over the first NumSplats splats of Source.
	UE_API void BuildClusters(
		const FGaussianSplatFormatFloat& Source,
		int32 NumSplats,
		int32 MaxSplatsPerCluster,
		EGaussianSplatClusterGrouping GroupingMode,
		TArray<FGaussianSplatCluster>& OutClusters
	);

	// Recomputes a cluster's bounds from the splats in its range.
	UE_API void ComputeClusterBounds(
		const FGaussianSplatFormatFloat& Source,
		FGaussianSplatCluster& Cluster
	);

#endif //WITH_EDITORONLY_DATA
}

#undef UE_API
