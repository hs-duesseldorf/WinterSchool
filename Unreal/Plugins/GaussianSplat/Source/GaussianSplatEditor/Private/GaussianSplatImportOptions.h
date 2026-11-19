// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GaussianSplatImportOptions.generated.h"

UENUM()
enum class EGaussianSplatImportCoordinateSpace : uint8
{
	// Do not apply any transformation to the incoming coordinates
	None,

	// Try to determine from the import file what the appropriate conversion should be
	// This is not always guaranteed to be correct as different file formats may have multiple spaces that can be used,
	// but one that is more common in which case we try to resolve against the most common space
	Automatic,

	// Converts from this space to Unreal (Left Handed X=Forward Y=Right Z=Up)
	// Generally from SPZ splat files
	RH_RUB UMETA(DisplayName = "Right Handed - X=Right Y=Up Z=Back"),

	// Converts from this space to Unreal (Left Handed X=Forward Y=Right Z=Up)
	// Generally from COLMAP PLY splat files
	RH_RDF UMETA(DisplayName = "Right Handed - X=Right Y=Down Z=Forward"),
};

UCLASS(hidecategories=Object, MinimalAPI)
class UGaussianSplatImportOptions : public UObject
{
public:
	GENERATED_BODY()

	// Coordinate space conversion (if any) to apply
	UPROPERTY(EditAnywhere, Category = "Import Settings")
	EGaussianSplatImportCoordinateSpace CoordinateSpace = EGaussianSplatImportCoordinateSpace::Automatic;

	// Scale to apply during the import
	// Defaults to 100.0 as most formats are 1 = 1m vs 1cm
	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (ClampMin = "0.01", ClampMax = "1000.0"))
	float ImportScale = 100.0f;

	// When set this is the maximum degree we will import
	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (ClampMin = "0", ClampMax = "3"))
	TOptional<int32> SHMaxDegree;
};
