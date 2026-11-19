// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectPtr.h"
#include "UObject/WeakObjectPtr.h"

class FGaussianSplatEditorToolkit;

class UGaussianSplat;
class UGaussianSplatComponent;
class UGaussianSplatEditorPreviewSettings;

class FGaussianSplatEditorViewModel : public FGCObject
{
public:
	virtual ~FGaussianSplatEditorViewModel() override;

	void Initialize(FGaussianSplatEditorToolkit* EditorToolkit, UGaussianSplat* Asset);
private:
	void SetupPreviewComponent();
	void UpdatePreviewAsset(bool bAssetChanged);
	void OnSplatDataChanged(UGaussianSplat* ChangedAsset);
	void OnPreviewSettingsChanged(const FPropertyChangedEvent& PropertyChangedEvent);

public:
	UGaussianSplat* GetAsset() const { return WeakAsset.Get(); }
	UGaussianSplatComponent* GetGaussianSplatComponent() const { return GaussianSplatComponent; }
	UGaussianSplatEditorPreviewSettings* GetPreviewSettings() const { return PreviewSettings; }

	TSharedPtr<FGaussianSplatEditorToolkit> GetEditorToolkit() const { return WeakEditorToolkit.Pin(); }

	uint64 GetAssetRawMemoryUsage() const;
	int32 GetNumberOfSplats() const;
	int32 GetSHDegree() const;
	FBox3f GetBounds() const;

	// Preview-only rotation, for viewing splats imported with an awkward orientation. Not saved to the asset.
	void SetPreviewRotation(const FRotator& Rotation);

	// Asset bounds with the preview transform applied
	FBox GetPreviewBounds() const;

	int32 GetPreviewMinLOD() const { return PreviewMinLOD; }
	int32 GetPreviewSHDegree() const;

	const UGaussianSplat* GetRenderedAsset() const { return PreviewAsset ? PreviewAsset.Get() : WeakAsset.Get(); }

	// BEGIN: FGCObject interface
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;
	// END: FGCObject interface

private:
	TWeakObjectPtr<UGaussianSplat>			WeakAsset;
	TObjectPtr<UGaussianSplatComponent> GaussianSplatComponent;
	TWeakPtr<FGaussianSplatEditorToolkit>			WeakEditorToolkit;

	// Preview variants
	TObjectPtr<UGaussianSplatEditorPreviewSettings> PreviewSettings;
	TObjectPtr<UGaussianSplat> PreviewAsset;
	int32 PreviewMinLOD = 0;
	int32 PreviewMaxSHDegree = 0;

	FDelegateHandle OnDataChangedHandle;
};
