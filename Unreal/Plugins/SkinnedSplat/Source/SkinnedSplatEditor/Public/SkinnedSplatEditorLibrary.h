#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SkinnedSplatEditorLibrary.generated.h"

class USkinnedSplatData;

UCLASS()
class SKINNEDSPLATEDITOR_API USkinnedSplatEditorLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/**
	 * Imports the skeleton + clips of a skinned splat (a .glb/.gltf/.fbx, default: the file next to the splat's .ply with
	 * the same name) next to the splat, as SK_/SKEL_/PHYS_<splat name>, M_<material> and Anims/<clip>, and links the
	 * skeletal mesh and the animations to the splat. Assets already at those paths are kept (delete them to reimport).
	 */
	UFUNCTION(BlueprintCallable, Category = "Skinned Splat")
	static bool ImportRig(USkinnedSplatData* Splat, FString RigFile = TEXT(""));

	/** The rig file ImportRig would use when given none, or "" if there is none. */
	UFUNCTION(BlueprintCallable, Category = "Skinned Splat")
	static FString FindRigFile(const FString& PlyFile);
};
