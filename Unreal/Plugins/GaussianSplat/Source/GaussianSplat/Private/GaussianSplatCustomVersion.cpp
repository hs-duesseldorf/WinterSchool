// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatCustomVersion.h"

#include "Serialization/CustomVersion.h"

const FGuid FGaussianSplatCustomVersion::GUID(0xEE6F23F5, 0x36CA48A6, 0x976D06B3, 0x73D3BBFD);

FCustomVersionRegistration GRegisterGaussianSplatCustomVersion(
	FGaussianSplatCustomVersion::GUID,
	FGaussianSplatCustomVersion::LatestVersion,
	TEXT("GaussianSplatVer"));
