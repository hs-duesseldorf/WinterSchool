// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ActorFactories/ActorFactory.h"

#include "GaussianSplatActorFactory.generated.h"

UCLASS(config = Editor, collapsecategories, hidecategories = Object)
class UGaussianSplatActorFactory : public UActorFactory
{
	GENERATED_BODY()

public:
	UGaussianSplatActorFactory();

	// BEGIN: UActorFactory Impl
	virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
	virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
	virtual UObject* GetAssetFromActorInstance(AActor* Instance) override;
	// END: UActorFactory Impl
};
