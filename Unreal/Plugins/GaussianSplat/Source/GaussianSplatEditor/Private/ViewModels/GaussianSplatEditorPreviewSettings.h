// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UObject/Object.h"

#include "GaussianSplatEditorPreviewSettings.generated.h"

DECLARE_DELEGATE_OneParam(FOnGaussianSplatEditorPreviewSettingsChanged, const FPropertyChangedEvent&);

// Viewer-only settings shown in the Preview Settings tab. Nothing here is saved to the asset.
UCLASS(Transient)
class UGaussianSplatEditorPreviewSettings : public UObject
{
	GENERATED_BODY()

public:
	// For viewing splats imported with an awkward orientation
	UPROPERTY(EditAnywhere, Category = "Gaussian Splat")
	FRotator Rotation = FRotator::ZeroRotator;

	// Renders the splat as cooked for a platform with this Min LOD
	UPROPERTY(EditAnywhere, Category = "Gaussian Splat", meta = (DisplayName = "Min LOD", ClampMin = 0))
	int32 MinLOD = 0;

	// Renders the splat as cooked for a platform with this Max SH Degree
	UPROPERTY(EditAnywhere, Category = "Gaussian Splat", meta = (DisplayName = "Max SH Degree", ClampMin = 0, ClampMax = 3))
	int32 MaxSHDegree = 3;

	// Draws the statistics overlay in the viewport
	UPROPERTY(EditAnywhere, Category = "Gaussian Splat")
	bool bShowStats = true;

	FOnGaussianSplatEditorPreviewSettingsChanged OnChanged;

	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override
	{
		Super::PostEditChangeProperty(PropertyChangedEvent);
		OnChanged.ExecuteIfBound(PropertyChangedEvent);
	}
};
