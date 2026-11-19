#include "SkinnedSplatEditorLibrary.h"
#include "SkinnedSplatData.h"

#include "Animation/AnimSequence.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Animation/Skeleton.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogSkinnedSplatEditor, Log, All);

FString USkinnedSplatEditorLibrary::FindRigFile(const FString& PlyFile)
{
	for (const TCHAR* Ext : { TEXT(".glb"), TEXT(".gltf"), TEXT(".fbx") })
	{
		const FString Candidate = FPaths::ChangeExtension(PlyFile, Ext);
		if (IFileManager::Get().FileExists(*Candidate)) return Candidate;
	}
	return FString();
}

bool USkinnedSplatEditorLibrary::ImportRig(USkinnedSplatData* Splat, FString RigFile)
{
	if (!Splat) return false;
	if (RigFile.IsEmpty() && Splat->AssetImportData)
	{
		RigFile = FindRigFile(Splat->AssetImportData->GetFirstFilename());
	}
	if (RigFile.IsEmpty() || !IFileManager::Get().FileExists(*RigFile))
	{
		UE_LOG(LogSkinnedSplatEditor, Warning, TEXT("%s: no rig file found (put a .glb with the same name next to the .ply)"), *Splat->GetName());
		return false;
	}
	// The importer makes its own folder tree (<file>/SkeletalMeshes, Materials...): import into a staging folder, then
	// move everything next to the splat
	const FString Folder = FPaths::GetPath(Splat->GetPathName());
	const FString Staging = Folder / TEXT("RigImport");
	IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = RigFile;
	Task->DestinationPath = Staging;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bSave = false;
	AssetTools.ImportAssetTasks({ Task });

	TArray<FAssetData> Imported;
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get().GetAssetsByPath(FName(*Staging), Imported, true);
	const FString FilePrefix = FPaths::GetBaseFilename(RigFile); // clips come as <file name><clip name>
	USkeletalMesh* Mesh = nullptr;
	TArray<UAnimSequence*> Clips;
	TArray<FAssetRenameData> Moves;
	for (const FAssetData& Asset : Imported)
	{
		UObject* Obj = Asset.GetAsset();
		FString Path = Folder, Name;
		if (USkeletalMesh* M = Cast<USkeletalMesh>(Obj)) { Mesh = M; Name = TEXT("SK_") + Splat->GetName(); }
		else if (Cast<USkeleton>(Obj)) Name = TEXT("SKEL_") + Splat->GetName();
		else if (Cast<UPhysicsAsset>(Obj)) Name = TEXT("PHYS_") + Splat->GetName();
		else if (Cast<UMaterialInterface>(Obj)) Name = TEXT("M_") + Obj->GetName();
		else if (UAnimSequence* A = Cast<UAnimSequence>(Obj))
		{
			Clips.Add(A);
			Path = Folder / TEXT("Anims");
			Name = Obj->GetName().StartsWith(FilePrefix) && Obj->GetName().Len() > FilePrefix.Len() ? Obj->GetName().RightChop(FilePrefix.Len()) : Obj->GetName();
		}
		else continue;
		if (FPackageName::DoesPackageExist(Path / Name) || FindObject<UObject>(nullptr, *(Path / Name + TEXT(".") + Name)))
		{
			UE_LOG(LogSkinnedSplatEditor, Warning, TEXT("%s: %s exists, %s stays in %s"), *Splat->GetName(), *(Path / Name), *Obj->GetName(), *Staging);
			continue;
		}
		Moves.Emplace(Obj, Path, Name);
	}
	AssetTools.RenameAssets(Moves);
	if (!Mesh)
	{
		UE_LOG(LogSkinnedSplatEditor, Error, TEXT("%s: %s has no skeletal mesh"), *Splat->GetName(), *RigFile);
		return false;
	}
	Clips.Sort([](const UAnimSequence& A, const UAnimSequence& B) { return A.GetName() < B.GetName(); });
	Splat->Modify();
	Splat->Mesh = Mesh;
	Splat->Animations.Reset();
	Splat->DefaultAnimation = nullptr;
	for (UAnimSequence* A : Clips)
	{
		Splat->Animations.Add(A);
		if (!Splat->DefaultAnimation && A->GetName().Contains(TEXT("idle"))) Splat->DefaultAnimation = A;
	}
	if (!Splat->DefaultAnimation && Clips.Num()) Splat->DefaultAnimation = Clips[0];
	Splat->MarkPackageDirty();
	UE_LOG(LogSkinnedSplatEditor, Display, TEXT("%s: rig %s, %d clips (default %s)"), *Splat->GetName(), *Mesh->GetPathName(), Clips.Num(), *GetNameSafe(Splat->DefaultAnimation));
	return true;
}
