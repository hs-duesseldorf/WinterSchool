// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Math/Matrix.h"

class UGaussianSplatComponent;

namespace GaussianSplat
{
	/** Game-thread local-to-world matrix shared by proxy creation and per-frame updates. */
	GAUSSIANSPLATSCENEVIEWEXTENSION_API FMatrix44f GetComponentLocalToWorld(const UGaussianSplatComponent& Component);
}
