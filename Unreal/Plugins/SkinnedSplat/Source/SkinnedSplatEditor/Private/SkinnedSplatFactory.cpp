#include "SkinnedSplatFactory.h"
#include "SkinnedSplatData.h"
#include "SkinnedSplatEditorLibrary.h"

#include "Editor.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/FeedbackContext.h"
#include "Misc/Paths.h"
#include "Subsystems/ImportSubsystem.h"
#include "TimerManager.h"

USkinnedSplatFactory::USkinnedSplatFactory()
{
	SupportedClass = USkinnedSplatData::StaticClass();
	bCreateNew = false;
	bEditorImport = true;
	bText = false;
	Formats.Add(TEXT("ply;Skinned Gaussian Splat (3DGS PLY with bone weights)"));
	ImportPriority = DefaultImportPriority + 100; // ahead of Epic's splat factory, which also takes .ply
}

bool USkinnedSplatFactory::FactoryCanImport(const FString& Filename)
{
	return FPaths::GetExtension(Filename).Equals(TEXT("ply"), ESearchCase::IgnoreCase) && USkinnedSplatData::IsSkinnedPLY(Filename);
}

UObject* USkinnedSplatFactory::FactoryCreateFile(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, const FString& Filename,
	const TCHAR* Parms, FFeedbackContext* Warn, bool& bOutOperationCanceled)
{
	GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetPreImport(this, InClass, InParent, InName, TEXT("ply"));
	// keep the links (mesh, clips) of an existing asset when importing over it
	USkinnedSplatData* Data = FindObject<USkinnedSplatData>(InParent, *InName.ToString());
	if (!Data)
	{
		Data = NewObject<USkinnedSplatData>(InParent, InClass, InName, Flags | RF_Transactional);
	}
	FString Error;
	if (!Data->LoadFromPLY(Filename, Error))
	{
		Warn->Logf(ELogVerbosity::Error, TEXT("Skinned splat import failed for %s: %s"), *Filename, *Error);
		return nullptr;
	}
	Data->MarkPackageDirty();
	GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetPostImport(this, Data);

	// rig: imported on the next tick, so it runs outside of this import
	if (!Data->Mesh && !USkinnedSplatEditorLibrary::FindRigFile(Filename).IsEmpty())
	{
		TWeakObjectPtr<USkinnedSplatData> Weak(Data);
		GEditor->GetTimerManager()->SetTimerForNextTick([Weak]()
		{
			if (USkinnedSplatData* D = Weak.Get(); D && !D->Mesh) USkinnedSplatEditorLibrary::ImportRig(D);
		});
	}
	return Data;
}

bool USkinnedSplatFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
	const USkinnedSplatData* Data = Cast<USkinnedSplatData>(Obj);
	if (Data && Data->AssetImportData)
	{
		Data->AssetImportData->ExtractFilenames(OutFilenames);
		return true;
	}
	return false;
}

void USkinnedSplatFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
	USkinnedSplatData* Data = Cast<USkinnedSplatData>(Obj);
	if (Data && Data->AssetImportData && NewReimportPaths.Num() == 1)
	{
		Data->AssetImportData->UpdateFilenameOnly(NewReimportPaths[0]);
	}
}

EReimportResult::Type USkinnedSplatFactory::Reimport(UObject* Obj)
{
	USkinnedSplatData* Data = Cast<USkinnedSplatData>(Obj);
	if (!Data || !Data->AssetImportData) return EReimportResult::Failed;
	const FString File = Data->AssetImportData->GetFirstFilename();
	FString Error;
	Data->Modify();
	if (!Data->LoadFromPLY(File, Error))
	{
		UE_LOG(LogTemp, Error, TEXT("Skinned splat reimport failed for %s: %s"), *File, *Error);
		return EReimportResult::Failed;
	}
	Data->MarkPackageDirty();
	return EReimportResult::Succeeded;
}
