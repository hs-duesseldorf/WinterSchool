// Copyright Epic Games, Inc. All Rights Reserved.

#include "SGaussianSplatEditorViewport.h"

#include "Engine/World.h"
#include "Toolkit/GaussianSplatEditorToolkit.h"
#include "ViewModels/GaussianSplatEditorPreviewSettings.h"
#include "ViewModels/GaussianSplatEditorViewModel.h"
#include "GaussianSplat.h"
#include "GaussianSplatComponent.h"

#include "ViewportToolbar/UnrealEdViewportToolbar.h"
#include "AdvancedPreviewScene.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SGaussianSplatEditorViewport"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class FGaussianSplatEditorViewportClient : public FEditorViewportClient
{
public:
	FGaussianSplatEditorViewportClient(FAdvancedPreviewScene& InPreviewScene, const TSharedRef<SGaussianSplatEditorViewport>& InEditorViewport)
		: FEditorViewportClient(nullptr, &InPreviewScene, StaticCastSharedRef<SEditorViewport>(InEditorViewport))
	{
		// Setup defaults for the common draw helper.
		DrawHelper.bDrawPivot = false;
		DrawHelper.bDrawWorldBox = false;
		DrawHelper.bDrawKillZ = false;
		DrawHelper.bDrawGrid = true;
		DrawHelper.GridColorAxis = FColor(80, 80, 80);
		DrawHelper.GridColorMajor = FColor(72, 72, 72);
		DrawHelper.GridColorMinor = FColor(64, 64, 64);
		DrawHelper.PerspectiveGridSize = UE_OLD_HALF_WORLD_MAX1;
		ShowWidget(false);

		FEditorViewportClient::SetViewMode(VMI_Lit);

		EngineShowFlags.SetSnap(0);
		EngineShowFlags.SetEyeAdaptation(false);
		ExposureSettings.bFixed = true;
		ExposureSettings.FixedEV100 = 1.0f;

		OverrideNearClipPlane(1.0f);

		FGaussianSplatEditorViewportClient::SetIsSimulateInEditorViewport(true);
	}

	// FEditorViewportClient interface
	virtual FLinearColor GetBackgroundColor() const override
	{
		return FLinearColor::Black;
	}

	virtual void Tick(float DeltaSeconds) override
	{
		FEditorViewportClient::Tick(DeltaSeconds);
		PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);
	}

	virtual bool CanSetWidgetMode(UE::Widget::EWidgetMode NewMode) const override
	{
		return false;
	}

	virtual bool CanCycleWidgetMode() const override
	{
		return false;
	}
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void SGaussianSplatEditorViewport::Construct(FArguments InArgs, TSharedPtr<FGaussianSplatEditorViewModel> InViewModel)
{
	check(InViewModel.IsValid());

	ViewModel = InViewModel;

	AdvancedPreviewScene = MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
	AdvancedPreviewScene->SetFloorVisibility(false, true);
	AdvancedPreviewScene->SetEnvironmentVisibility(false, true);

	if (UGaussianSplatComponent* SplatComponent = ViewModel->GetGaussianSplatComponent())
	{
		AdvancedPreviewScene->AddComponent(SplatComponent, SplatComponent->GetRelativeTransform());
	}

	SEditorViewport::Construct(SEditorViewport::FArguments());

	Client->EngineShowFlags.SetGrid(false);

	// The viewport has no size until layout, so this frames for the client's default aspect ratio
	if (IsObjectSized())
	{
		FocusOnAsset();
	}
}

SGaussianSplatEditorViewport::~SGaussianSplatEditorViewport()
{
	if(ViewportClient.IsValid())
	{
		ViewportClient->Viewport = nullptr;
	}
}

void SGaussianSplatEditorViewport::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SEditorViewport::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
}

TSharedRef<FEditorViewportClient> SGaussianSplatEditorViewport::MakeEditorViewportClient()
{
	ViewportClient = MakeShareable( new FGaussianSplatEditorViewportClient(*AdvancedPreviewScene.Get(), SharedThis(this)));

	ViewportClient->SetViewLocation(FVector::ZeroVector);
	ViewportClient->SetViewRotation(FRotator::ZeroRotator);
	ViewportClient->bSetListenerPosition = false;

	ViewportClient->SetRealtime(true);
	ViewportClient->VisibilityDelegate.BindSP(this, &SGaussianSplatEditorViewport::IsVisible);

	return ViewportClient.ToSharedRef();
}

TSharedPtr<SWidget> SGaussianSplatEditorViewport::BuildViewportToolbar()
{
	// Register the viewport toolbar if another viewport hasn't already (it's shared).
	const FName ViewportToolbarName = "GaussianSplatEditor.ViewportToolbar";

	if (!UToolMenus::Get()->IsMenuRegistered(ViewportToolbarName))
	{
		UToolMenu* const ViewportToolbarMenu = UToolMenus::Get()->RegisterMenu(ViewportToolbarName, NAME_None /* parent */, EMultiBoxType::SlimHorizontalToolBar);

		ViewportToolbarMenu->StyleName = "ViewportToolbar";

		// Add the right-aligned part of the viewport toolbar.
		{
			FToolMenuSection& RightSection = ViewportToolbarMenu->AddSection("Right");
			RightSection.Alignment = EToolMenuSectionAlign::Last;

			// Add the "Camera" submenu.
			{
				RightSection.AddEntry(UE::UnrealEd::CreateCameraSubmenu(UE::UnrealEd::FViewportCameraMenuOptions().ShowLensControls()));
			}

			// Add the "View Modes" sub menu.
			{
				RightSection.AddEntry(UE::UnrealEd::CreateViewModesSubmenu());
			}

			RightSection.AddEntry(UE::UnrealEd::CreatePerformanceAndScalabilitySubmenu());
		}
	}

	FToolMenuContext ViewportToolbarContext;
	{
		ViewportToolbarContext.AppendCommandList(AdvancedPreviewScene->GetCommandList());
		ViewportToolbarContext.AppendCommandList(GetCommandList());

		// Add the UnrealEd viewport toolbar context.
		{
			UUnrealEdViewportToolbarContext* const ContextObject = UE::UnrealEd::CreateViewportToolbarDefaultContext(SharedThis(this));
			ContextObject->AssetEditorToolkit = ViewModel->GetEditorToolkit();

			ViewportToolbarContext.AddObject(ContextObject);
		}
	}

	return UToolMenus::Get()->GenerateWidget(ViewportToolbarName, ViewportToolbarContext);
}

void SGaussianSplatEditorViewport::PopulateViewportOverlays(TSharedRef<SOverlay> Overlay)
{
	SEditorViewport::PopulateViewportOverlays(Overlay);

	Overlay->AddSlot()
	.VAlign(VAlign_Top)
	.HAlign(HAlign_Left)
	.Padding(6.0f)
	[
		SNew(SBorder)
		.Visibility_Lambda([this]() { return ViewModel->GetPreviewSettings()->bShowStats ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
		.BorderImage(FAppStyle::Get().GetBrush("FloatingBorder"))
		.Padding(4.0f)
		[
			SNew(STextBlock)
			.Text(this, &SGaussianSplatEditorViewport::GetStatsText)
		]
	];
}

void SGaussianSplatEditorViewport::OnFocusViewportToSelection()
{
	FocusOnAsset();
}

bool SGaussianSplatEditorViewport::IsObjectSized() const
{
	const FBox3f Bounds = ViewModel->GetBounds();
	return Bounds.IsValid && Bounds.GetSize().GetMax() < ObjectMaxSize;
}

void SGaussianSplatEditorViewport::FocusOnAsset()
{
	const FBox Bounds = ViewModel->GetPreviewBounds();
	if (Bounds.IsValid)
	{
		ViewportClient->FocusViewportOnBox(Bounds, true);
	}
}

FText SGaussianSplatEditorViewport::GetStatsText() const
{
	FNumberFormattingOptions MiBOptions;
	MiBOptions.MinimumFractionalDigits = 2;
	MiBOptions.MaximumFractionalDigits = 2;
	auto AsMiB = [&MiBOptions](uint64 Bytes) { return FText::AsNumber(double(Bytes) / (1024.0 * 1024.0), &MiBOptions); };

	FTextBuilder Text;
	Text.AppendLine(FText::Format(LOCTEXT("StatsSplats", "Splats: {0}"), FText::AsNumber(ViewModel->GetNumberOfSplats())));
	Text.AppendLine(FText::Format(LOCTEXT("StatsSHDegree", "SH Degree: {0}"), FText::AsNumber(ViewModel->GetSHDegree())));

	const FBox3f Bounds = ViewModel->GetBounds();
	if (Bounds.IsValid)
	{
		const FVector3f Size = Bounds.GetSize();
		Text.AppendLine(FText::Format(LOCTEXT("StatsDimensions", "Dimensions: {0} x {1} x {2}"), FMath::CeilToInt(Size.X), FMath::CeilToInt(Size.Y), FMath::CeilToInt(Size.Z)));
	}

	Text.AppendLine(FText::Format(LOCTEXT("StatsRawMemory", "Raw Memory: {0} MiB"), AsMiB(ViewModel->GetAssetRawMemoryUsage())));

	if (const UGaussianSplat* Rendered = ViewModel->GetRenderedAsset())
	{
		const FGaussianSplatFormat* RuntimeFormat = Rendered->GetRuntimeData<FGaussianSplatFormat>();
		Text.AppendLine(FText::Format(LOCTEXT("StatsRuntimeSplats", "Runtime Splats (all LODs): {0}"), FText::AsNumber(RuntimeFormat ? RuntimeFormat->GetNumSplats() : 0)));
		Text.AppendLine(FText::Format(LOCTEXT("StatsRuntimeMemory", "Runtime Memory: {0} MiB"), AsMiB(Rendered->GetRuntimeMemoryUsage())));
	}

	if (const int32 PreviewMinLOD = ViewModel->GetPreviewMinLOD(); PreviewMinLOD > 0)
	{
		Text.AppendLine(FText::Format(LOCTEXT("StatsPreviewMinLOD", "Previewing Min LOD {0} (not saved)"), PreviewMinLOD));
	}

	if (const int32 PreviewSHDegree = ViewModel->GetPreviewSHDegree(); PreviewSHDegree < ViewModel->GetSHDegree())
	{
		Text.AppendLine(FText::Format(LOCTEXT("StatsPreviewSHDegree", "Previewing Max SH Degree {0} (not saved)"), PreviewSHDegree));
	}

	return Text.ToText();
}

#undef LOCTEXT_NAMESPACE
