#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SkinnedSplatData.generated.h"

class USkeletalMesh;
class UAnimSequence;
class UAssetImportData;

/**
 * A rigged Gaussian splat, imported from a standard 3DGS .ply (COLMAP space, metres, log scales, logit opacity, SH DC)
 * that carries extra per-splat skinning properties bone_0..3 / weight_0..3 and one
 * "comment bone <index> <name> <parent> <x> <y> <z>" header line per bone (capture-pose joint in PLY space).
 * The splats are stored in the capture pose and in the same space Epic's GaussianSplat importer uses (cm, X forward).
 * The skeleton and its clips come from a regular Skeletal Mesh import (e.g. the .glb next to the .ply).
 */
UCLASS(BlueprintType)
class SKINNEDSPLAT_API USkinnedSplatData : public UObject
{
	GENERATED_BODY()
public:
	/** Proxy skeletal mesh whose bones drive the splats (bones are matched by name). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat")
	TObjectPtr<USkeletalMesh> Mesh;

	/** Clips that ship with this splat (for the actor's drop-down and Blueprints). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat")
	TArray<TObjectPtr<UAnimSequence>> Animations;

	/** Played by a new actor unless it picks another one. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat")
	TObjectPtr<UAnimSequence> DefaultAnimation;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Info")
	int32 NumSplats = 0;

	UPROPERTY(VisibleAnywhere, Category = "Info")
	TArray<FName> BoneNames;

	UPROPERTY()
	TArray<int32> BoneParents;

	/** Capture-pose joint positions, same space as the splats. */
	UPROPERTY()
	TArray<FVector3f> BoneRefPositions;

	UPROPERTY(VisibleAnywhere, Category = "Info")
	FBox Bounds = FBox(ForceInit);

#if WITH_EDITORONLY_DATA
	UPROPERTY(VisibleAnywhere, Instanced, Category = "Import")
	TObjectPtr<UAssetImportData> AssetImportData;
#endif

	/** GPU layout, 5 float4 per splat (see SkinnedSplat.usf). Serialized as bulk data. */
	TArray<FVector4f> Rest;

	/** Changes whenever the splat data changes; actors rebuild their GPU buffers when it does. */
	FGuid DataGuid;

	/** Parses a skinned .ply into this asset. ImportScale converts file units (m) to cm. */
	bool LoadFromPLY(const FString& Path, FString& OutError, float ImportScale = 100.0f);

	/** True if the file is a .ply with the bone_0 property (skinned splat), false for a plain splat. */
	static bool IsSkinnedPLY(const FString& Path);

	virtual void Serialize(FArchive& Ar) override;
	virtual void PostInitProperties() override;
#if WITH_EDITOR
	virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;
#endif
};
