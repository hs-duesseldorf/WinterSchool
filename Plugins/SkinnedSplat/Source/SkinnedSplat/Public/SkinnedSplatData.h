#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SkinnedSplatData.generated.h"

/** Raw .skinsplat file (rest-pose splats + top-4 bone weights + rest joints), cooked as a normal asset. */
UCLASS(BlueprintType)
class SKINNEDSPLAT_API USkinnedSplatData : public UDataAsset
{
	GENERATED_BODY()
public:
	/** Reads a .skinsplat file into this asset (editor import step). */
	UFUNCTION(BlueprintCallable, Category = "Skinned Splat")
	bool LoadFromFile(const FString& Path);

	UFUNCTION(BlueprintPure, Category = "Skinned Splat")
	int32 GetNumBytes() const { return Raw.Num(); }

	virtual void Serialize(FArchive& Ar) override;

	TArray<uint8> Raw;
};
