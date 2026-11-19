// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatEditorToolkit.h"
#include "GaussianSplat.h"

#include "ViewModels/GaussianSplatEditorPreviewSettings.h"
#include "ViewModels/GaussianSplatEditorViewModel.h"
#include "Widgets/SGaussianSplatEditorViewport.h"

#include "IDetailsView.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "GaussianSplatEditorToolkit"

namespace GaussianSplatEditorToolkitPrivate
{
	const FName GaussianSplatEditorAppName = FName("GaussianSplatEditorApp");
	const FName ViewportTabId = FName("GaussianSplatEditor_Viewport");
	const FName DetailsTabId = FName("GaussianSplatEditor_Details");
	const FName PreviewSettingsTabId = FName("GaussianSplatEditor_PreviewSettings");

	TSharedRef<SDockTab> MakeDetailsTab(UObject* Object)
	{
		FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		FDetailsViewArgs DetailsViewArgs;
		DetailsViewArgs.bHideSelectionTip = true;
		DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
		TSharedRef<IDetailsView> DetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
		DetailsView->SetObject(Object);

		return
			SNew(SDockTab)
			[
				DetailsView
			];
	}
}

void FGaussianSplatEditorToolkit::RegisterTabSpawners(const TSharedRef<class FTabManager>& InTabManager)
{
	using namespace GaussianSplatEditorToolkitPrivate;

	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenu_GaussianSplatEditor", "Gaussian Splat Editor"));
	
	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	InTabManager->RegisterTabSpawner(ViewportTabId, FOnSpawnTab::CreateSP(this, &FGaussianSplatEditorToolkit::SpawnTab_Viewport))
		.SetDisplayName(LOCTEXT("ViewportTab", "Viewport"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());

	InTabManager->RegisterTabSpawner(DetailsTabId, FOnSpawnTab::CreateSP(this, &FGaussianSplatEditorToolkit::SpawnTab_Details))
		.SetDisplayName(LOCTEXT("DetailsTab", "Details"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());

	InTabManager->RegisterTabSpawner(PreviewSettingsTabId, FOnSpawnTab::CreateSP(this, &FGaussianSplatEditorToolkit::SpawnTab_PreviewSettings))
		.SetDisplayName(LOCTEXT("PreviewSettingsTab", "Preview Settings"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());
}

void FGaussianSplatEditorToolkit::UnregisterTabSpawners(const TSharedRef<class FTabManager>& InTabManager)
{
	using namespace GaussianSplatEditorToolkitPrivate;

	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(ViewportTabId);
	InTabManager->UnregisterTabSpawner(DetailsTabId);
	InTabManager->UnregisterTabSpawner(PreviewSettingsTabId);
}

void FGaussianSplatEditorToolkit::Initialize(const EToolkitMode::Type Mode, const TSharedPtr< class IToolkitHost >& InitToolkitHost, UGaussianSplat* Asset)
{
	using namespace GaussianSplatEditorToolkitPrivate;

	if (!::IsValid(Asset))
	{
		return;
	}

	ViewModel = MakeShared<FGaussianSplatEditorViewModel>();
	ViewModel->Initialize(this, Asset);

	const TSharedRef<FTabManager::FLayout> StandaloneDefaultLayout = FTabManager::NewLayout("Standalone_GaussianSplatEditor_Layout_V5")
		->AddArea
		(
			FTabManager::NewPrimaryArea()->SetOrientation(Orient_Horizontal)
			->Split(
				FTabManager::NewStack()
				->SetSizeCoefficient(0.8f)
				->AddTab(ViewportTabId, ETabState::OpenedTab)
			)
			->Split
			(
				FTabManager::NewSplitter()->SetOrientation(Orient_Vertical)
				->SetSizeCoefficient(0.2f)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.7f)
					->AddTab(DetailsTabId, ETabState::OpenedTab)
				)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.3f)
					->AddTab(PreviewSettingsTabId, ETabState::OpenedTab)
				)
			)
		);

	constexpr bool bCreateDefaultStandaloneMenu = true;
	constexpr bool bCreateDefaultToolbar = true;
	InitAssetEditor(Mode, InitToolkitHost, GaussianSplatEditorAppName, StandaloneDefaultLayout, bCreateDefaultStandaloneMenu, bCreateDefaultToolbar, Asset);
}

FName FGaussianSplatEditorToolkit::GetToolkitFName() const
{
	return FName("GaussianSplatEditor");
}

FText FGaussianSplatEditorToolkit::GetBaseToolkitName() const
{
	return LOCTEXT("AppLabel", "Gaussian Splat Editor");
}

FString FGaussianSplatEditorToolkit::GetWorldCentricTabPrefix() const
{
	return LOCTEXT("WorldCentricTabPrefix", "Gaussian Splat ").ToString();
}

FLinearColor FGaussianSplatEditorToolkit::GetWorldCentricTabColorScale() const
{
	return FLinearColor::White;
}

TSharedRef<SDockTab> FGaussianSplatEditorToolkit::SpawnTab_Viewport(const FSpawnTabArgs& Args)
{
	using namespace GaussianSplatEditorToolkitPrivate;

	check(Args.GetTabId().TabType == ViewportTabId);

	return
		SNew(SDockTab)
		[
			SNew(SGaussianSplatEditorViewport, ViewModel)
		];
}

TSharedRef<SDockTab> FGaussianSplatEditorToolkit::SpawnTab_Details(const FSpawnTabArgs& Args)
{
	using namespace GaussianSplatEditorToolkitPrivate;

	check(Args.GetTabId().TabType == DetailsTabId);

	return MakeDetailsTab(ViewModel->GetAsset());
}

TSharedRef<SDockTab> FGaussianSplatEditorToolkit::SpawnTab_PreviewSettings(const FSpawnTabArgs& Args)
{
	using namespace GaussianSplatEditorToolkitPrivate;

	check(Args.GetTabId().TabType == PreviewSettingsTabId);

	return MakeDetailsTab(ViewModel->GetPreviewSettings());
}

#undef LOCTEXT_NAMESPACE
