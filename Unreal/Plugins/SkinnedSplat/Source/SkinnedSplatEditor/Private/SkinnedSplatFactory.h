#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "EditorReimportHandler.h"
#include "SkinnedSplatFactory.generated.h"

/**
 * Imports a skinned 3DGS .ply (one with bone_0..3 / weight_0..3 properties) as a USkinnedSplatData. Plain splat .ply
 * files are left to Epic's GaussianSplat importer. A .glb/.gltf/.fbx with the same name next to the .ply is imported
 * as the rig right after (skeletal mesh + clips) and linked to the splat.
 */
UCLASS()
class USkinnedSplatFactory : public UFactory, public FReimportHandler
{
	GENERATED_BODY()
public:
	USkinnedSplatFactory();

	virtual bool FactoryCanImport(const FString& Filename) override;
	virtual UObject* FactoryCreateFile(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, const FString& Filename,
		const TCHAR* Parms, FFeedbackContext* Warn, bool& bOutOperationCanceled) override;

	virtual bool CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
	virtual void SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
	virtual EReimportResult::Type Reimport(UObject* Obj) override;
	virtual int32 GetPriority() const override { return ImportPriority; }
};
