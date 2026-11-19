// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Mirrored by FGaussianSplatCluster in GaussianSplatCommon.ush, keep the two in sync.
struct FGaussianSplatCluster
{
	FVector3f	BoundsMin;
	uint32		SplatOffset;
	FVector3f	BoundsMax;
	uint32		SplatCount;
	uint32		Level;

	float		Error;
	uint32		ParentIndex;
	uint32		Padding;
};

static_assert(sizeof(FGaussianSplatCluster) == 48, "FGaussianSplatCluster must stay three float4s");
