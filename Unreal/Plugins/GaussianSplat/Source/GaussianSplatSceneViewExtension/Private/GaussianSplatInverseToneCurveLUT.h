// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

// Log category for the inverse-tone-curve feature.
DECLARE_LOG_CATEGORY_EXTERN(LogGaussianSplat, Log, All);

namespace GaussianSplat
{
	// Selects which baked inverse-tone-curve LUT to sample; cast from bInvertExpandGamut for now.
	enum class EInverseToneCurveLUTVariant : uint8
	{
		/** Inverse filmic tone curve only (asset UE_InverseFilmicTC). */
		Filmic = 0,
		/** Inverse filmic tone curve plus gamut expansion (asset UE_InverseFilmicTC_Full). */
		FilmicExpandGamut = 1,
	};
}
