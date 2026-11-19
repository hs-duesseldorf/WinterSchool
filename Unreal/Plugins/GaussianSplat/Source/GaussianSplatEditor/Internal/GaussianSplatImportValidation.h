// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "UObject/NameTypes.h"

class FFeedbackContext;

struct FGaussianSplatImportSize
{
	int32 SourceSplatCount = 0;
	int32 SHDegree = 0;
	bool bQuantizedFormat = true;
};

namespace GaussianSplat
{
	/** Checks the factory's LOD-inclusive buffer budget and reports an actionable error on rejection. */
	GAUSSIANSPLATEDITOR_API bool ValidateImportSize(const FGaussianSplatImportSize& ImportSize, FName AssetName, FFeedbackContext& FeedbackContext);
}
