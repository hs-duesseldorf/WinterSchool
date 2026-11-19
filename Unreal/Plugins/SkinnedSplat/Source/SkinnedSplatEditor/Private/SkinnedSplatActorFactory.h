#pragma once

#include "CoreMinimal.h"
#include "ActorFactories/ActorFactory.h"
#include "SkinnedSplatActorFactory.generated.h"

/** Dragging a Skinned Splat asset into a level places an ASkinnedSplatActor playing its default clip. */
UCLASS()
class USkinnedSplatActorFactory : public UActorFactory
{
	GENERATED_BODY()
public:
	USkinnedSplatActorFactory();
	virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
	virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
	virtual UObject* GetAssetFromActorInstance(AActor* ActorInstance) override;
};
