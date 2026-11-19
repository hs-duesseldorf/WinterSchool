// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetDefinition_GaussianSplat.h"
#include "Toolkit/GaussianSplatEditorToolkit.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AssetDefinition_GaussianSplat)

EAssetCommandResult UAssetDefinition_GaussianSplat::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
	for (UGaussianSplat* Asset : OpenArgs.LoadObjects<UGaussianSplat>())
	{
		const TSharedRef<FGaussianSplatEditorToolkit> Toolkit = MakeShared<FGaussianSplatEditorToolkit>();
		Toolkit->Initialize(OpenArgs.GetToolkitMode(), OpenArgs.ToolkitHost, Asset);
	}
	return EAssetCommandResult::Handled;
}
