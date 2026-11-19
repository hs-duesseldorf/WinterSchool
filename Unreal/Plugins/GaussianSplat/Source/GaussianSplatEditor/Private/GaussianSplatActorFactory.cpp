// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatActorFactory.h"

#include "GaussianSplat.h"
#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GaussianSplatActorFactory)

#define LOCTEXT_NAMESPACE "GaussianSplatActorFactory"

UGaussianSplatActorFactory::UGaussianSplatActorFactory()
{
	DisplayName = LOCTEXT("GaussianSplat", "Gaussian Splat");
	NewActorClass = AGaussianSplatActor::StaticClass();
	bUseSurfaceOrientation = true;
}

bool UGaussianSplatActorFactory::CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg)
{
	if (!AssetData.IsValid() || !AssetData.IsInstanceOf(UGaussianSplat::StaticClass()))
	{
		OutErrorMsg = LOCTEXT("NoGaussianAsset", "No valid gaussian asset selected");
		return false;
	}

	return true;
}

void UGaussianSplatActorFactory::PostSpawnActor(UObject* InAsset, AActor* InActor)
{
	Super::PostSpawnActor(InAsset, InActor);

	UGaussianSplat* Asset = CastChecked<UGaussianSplat>(InAsset);
	AGaussianSplatActor* Actor = CastChecked<AGaussianSplatActor>(InActor);

	if (UGaussianSplatComponent* Component = Actor->GaussianSplatComponent)
	{
		Component->Asset = Asset;
	}
}

UObject* UGaussianSplatActorFactory::GetAssetFromActorInstance(AActor* Instance)
{
	check(Instance->IsA<AGaussianSplatActor>());
	AGaussianSplatActor* Actor = CastChecked<AGaussianSplatActor>(Instance);

	if (Actor->GaussianSplatComponent)
	{
		return Actor->GaussianSplatComponent->Asset;
	}

	return nullptr;
}

#undef LOCTEXT_NAMESPACE
