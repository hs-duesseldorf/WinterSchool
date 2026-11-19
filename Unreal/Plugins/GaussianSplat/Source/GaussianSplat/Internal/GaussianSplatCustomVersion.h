// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#define UE_API GAUSSIANSPLAT_API

struct FGaussianSplatCustomVersion
{
	enum Type
	{
		BeforeCustomVersionWasAdded = 0,
		MortonOrder,
		RuntimeSHDegree,

		// -----<new versions can be added above this line>-------------------------------------------------
		VersionPlusOne,
		LatestVersion = VersionPlusOne - 1
	};

	UE_API static const FGuid GUID;

private:
	FGaussianSplatCustomVersion() {}
};

#undef UE_API
