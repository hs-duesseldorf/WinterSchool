// Copyright Epic Games, Inc. All Rights Reserved.

#include "ViewModels/GaussianSplatEditorViewModel.h"

#include "Toolkit/GaussianSplatEditorToolkit.h"
#include "ViewModels/GaussianSplatEditorPreviewSettings.h"
#include "GaussianSplat.h"
#include "GaussianSplatComponent.h"

#define LOCTEXT_NAMESPACE "GaussianSplatEditorViewModel"

FGaussianSplatEditorViewModel::~FGaussianSplatEditorViewModel()
{
	UGaussianSplat::GetOnDataChanged().Remove(OnDataChangedHandle);
	if (PreviewSettings)
	{
		PreviewSettings->OnChanged.Unbind();
	}
}

void FGaussianSplatEditorViewModel::Initialize(FGaussianSplatEditorToolkit* EditorToolkit, UGaussianSplat* Asset)
{
	WeakEditorToolkit = StaticCastWeakPtr<FGaussianSplatEditorToolkit>(EditorToolkit->AsWeak());
	WeakAsset = Asset;

	SetupPreviewComponent();
	PreviewSettings = NewObject<UGaussianSplatEditorPreviewSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	PreviewSettings->OnChanged.BindRaw(this, &FGaussianSplatEditorViewModel::OnPreviewSettingsChanged);
	PreviewMaxSHDegree = PreviewSettings->MaxSHDegree;
	OnDataChangedHandle = UGaussianSplat::GetOnDataChanged().AddRaw(this, &FGaussianSplatEditorViewModel::OnSplatDataChanged);
}

void FGaussianSplatEditorViewModel::SetupPreviewComponent()
{
	UGaussianSplat* Asset = WeakAsset.Get();
	if (Asset == nullptr)
	{
		return;
	}

	GaussianSplatComponent = NewObject<UGaussianSplatComponent>(GetTransientPackage(), NAME_None, RF_Transient);
	GaussianSplatComponent->Asset = Asset;
}

void FGaussianSplatEditorViewModel::UpdatePreviewAsset(bool bAssetChanged)
{
	UGaussianSplat* Asset = WeakAsset.Get();
	if (Asset == nullptr || GaussianSplatComponent == nullptr)
	{
		return;
	}

	PreviewSettings->MinLOD = FMath::Clamp(PreviewSettings->MinLOD, 0, FMath::Max(Asset->GetNumLODLevels() - 1, 0));
	PreviewSettings->MaxSHDegree = FMath::Clamp(PreviewSettings->MaxSHDegree, 0, GaussianSplatCommon::MaxSupportedSHDegree);
	if (!bAssetChanged && PreviewSettings->MinLOD == PreviewMinLOD && PreviewSettings->MaxSHDegree == PreviewMaxSHDegree)
	{
		return;
	}

	PreviewMinLOD = PreviewSettings->MinLOD;
	PreviewMaxSHDegree = PreviewSettings->MaxSHDegree;
	const int32 PreviewSHDegree = GetPreviewSHDegree();
	PreviewAsset = (PreviewMinLOD > 0 || PreviewSHDegree < Asset->GetSHDegree()) ? Asset->CreatePreview(PreviewMinLOD, PreviewSHDegree) : nullptr;
	GaussianSplatComponent->Asset = PreviewAsset ? PreviewAsset.Get() : Asset;
	GaussianSplatComponent->ReregisterComponent();
}

void FGaussianSplatEditorViewModel::OnSplatDataChanged(UGaussianSplat* ChangedAsset)
{
	if (ChangedAsset == WeakAsset.Get() && (PreviewAsset || GetPreviewSHDegree() < ChangedAsset->GetSHDegree()))
	{
		UpdatePreviewAsset(true);
	}
}

void FGaussianSplatEditorViewModel::OnPreviewSettingsChanged(const FPropertyChangedEvent& PropertyChangedEvent)
{
	SetPreviewRotation(PreviewSettings->Rotation);

	// Disables preview update when actively modifying values
	if (PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
	{
		UpdatePreviewAsset(false);
	}
}

uint64 FGaussianSplatEditorViewModel::GetAssetRawMemoryUsage() const
{
	UGaussianSplat* Asset = WeakAsset.Get();
	return Asset ? Asset->GetRawMemoryUsage() : 0;
}

int32 FGaussianSplatEditorViewModel::GetNumberOfSplats() const
{
	UGaussianSplat* Asset = WeakAsset.Get();
	return Asset ? Asset->GetNumSplats() : 0;
}

int32 FGaussianSplatEditorViewModel::GetSHDegree() const
{
	UGaussianSplat* Asset = WeakAsset.Get();
	return Asset ? Asset->GetSHDegree() : 0;
}

int32 FGaussianSplatEditorViewModel::GetPreviewSHDegree() const
{
	return FMath::Min(PreviewMaxSHDegree, GetSHDegree());
}

FBox3f FGaussianSplatEditorViewModel::GetBounds() const
{
	UGaussianSplat* Asset = WeakAsset.Get();
	return Asset ? Asset->GetBounds() : FBox3f(ForceInit);
}

void FGaussianSplatEditorViewModel::SetPreviewRotation(const FRotator& Rotation)
{
	if (GaussianSplatComponent)
	{
		GaussianSplatComponent->SetRelativeRotation(Rotation);
	}
}

FBox FGaussianSplatEditorViewModel::GetPreviewBounds() const
{
	const FBox3f Bounds = GetBounds();
	if (!Bounds.IsValid || !GaussianSplatComponent)
	{
		return FBox(ForceInit);
	}
	return FBox(Bounds).TransformBy(GaussianSplatComponent->GetComponentTransform());
}

void FGaussianSplatEditorViewModel::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(GaussianSplatComponent);
	Collector.AddReferencedObject(PreviewSettings);
	Collector.AddReferencedObject(PreviewAsset);
}

FString FGaussianSplatEditorViewModel::GetReferencerName() const
{
	return "FGaussianSplatEditorViewModel";
}

#undef LOCTEXT_NAMESPACE
