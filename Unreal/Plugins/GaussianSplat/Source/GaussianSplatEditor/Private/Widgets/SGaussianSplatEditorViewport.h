// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once
#include "AdvancedPreviewScene.h"
#include "SEditorViewport.h"

class FGaussianSplatEditorViewModel;

class SGaussianSplatEditorViewport : public SEditorViewport
{
	SLATE_BEGIN_ARGS(SGaussianSplatEditorViewport) {}
	SLATE_END_ARGS()
	
public:
	void Construct(FArguments InArgs, TSharedPtr<FGaussianSplatEditorViewModel> ViewModel);
	virtual ~SGaussianSplatEditorViewport() override;

	// BEGIN: SEditorViewport interface
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;
	virtual TSharedPtr<SWidget> BuildViewportToolbar() override;
	virtual void PopulateViewportOverlays(TSharedRef<SOverlay> Overlay) override;
	virtual void OnFocusViewportToSelection() override;
	// END: SEditorViewport interface

private:
	// Splats smaller than this (in their largest dimension) are treated as an object and framed on open; larger
	// ones are treated as an environment, which is captured from the inside, so the camera starts at the origin.
	static constexpr float ObjectMaxSize = 1000.0f;

	bool IsObjectSized() const;
	void FocusOnAsset();
	FText GetStatsText() const;

	TSharedPtr<FAdvancedPreviewScene>						AdvancedPreviewScene;
	TSharedPtr<class FGaussianSplatEditorViewportClient>	ViewportClient;
	TSharedPtr<FGaussianSplatEditorViewModel>				ViewModel;
};
