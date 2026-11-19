#include "SkinnedSplatActorFactory.h"
#include "SkinnedSplatActor.h"
#include "SkinnedSplatData.h"
#include "AssetRegistry/AssetData.h"

#define LOCTEXT_NAMESPACE "SkinnedSplatActorFactory"

USkinnedSplatActorFactory::USkinnedSplatActorFactory()
{
	DisplayName = LOCTEXT("DisplayName", "Skinned Splat");
	NewActorClass = ASkinnedSplatActor::StaticClass();
	bUseSurfaceOrientation = false;
}

bool USkinnedSplatActorFactory::CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg)
{
	if (!AssetData.IsValid() || AssetData.IsInstanceOf(USkinnedSplatData::StaticClass()))
	{
		return true;
	}
	OutErrorMsg = LOCTEXT("NoSplat", "A Skinned Splat asset is required.");
	return false;
}

void USkinnedSplatActorFactory::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
	Super::PostSpawnActor(Asset, NewActor);
	ASkinnedSplatActor* Actor = CastChecked<ASkinnedSplatActor>(NewActor);
	if (USkinnedSplatData* Data = Cast<USkinnedSplatData>(Asset))
	{
		Actor->SplatData = Data;
		Actor->RerunConstructionScripts();
	}
}

UObject* USkinnedSplatActorFactory::GetAssetFromActorInstance(AActor* ActorInstance)
{
	const ASkinnedSplatActor* Actor = Cast<ASkinnedSplatActor>(ActorInstance);
	return Actor ? Actor->SplatData.Get() : nullptr;
}

#undef LOCTEXT_NAMESPACE
