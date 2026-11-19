// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianSplat.h"
#include "AssetDefinitionDefault.h"

#include "AssetDefinition_GaussianSplat.generated.h"

UCLASS()
class UAssetDefinition_GaussianSplat : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	// UAssetDefinition Begin
	virtual FText GetAssetDisplayName() const override { return NSLOCTEXT("AssetTypeActions", "AssetTypeActions_GaussianSplat", "Gaussian Splat"); }
	virtual FLinearColor GetAssetColor() const override { return FLinearColor(FColor(128, 96, 192)); }
	virtual TSoftClassPtr<UObject> GetAssetClass() const override { return UGaussianSplat::StaticClass(); }
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override
	{
		static const auto Categories = { EAssetCategoryPaths::FX / NSLOCTEXT("GaussianSplat", "GaussianSplatSubMenu_Advanced", "Advanced") };
		return Categories;
	}

	virtual bool CanImport() const override { return true; }
	virtual EAssetCommandResult OpenAssets(const FAssetOpenArgs& OpenArgs) const override;
	// UAssetDefinition End
};
