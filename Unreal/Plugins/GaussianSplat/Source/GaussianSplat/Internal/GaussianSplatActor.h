// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GaussianSplatActor.generated.h"

#define UE_API GAUSSIANSPLAT_API

class UGaussianSplatComponent;

UCLASS(meta = (Valkyrie_DevelopmentStatus = "Experimental", UEFN_DevelopmentStatus = "Experimental"))
class AGaussianSplatActor : public AActor
{
	GENERATED_BODY()

public:
	AGaussianSplatActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UGaussianSplatComponent> GaussianSplatComponent;

#if WITH_EDITOR
	UE_API virtual bool GetReferencedContentObjects(TArray<UObject*>& Objects) const override;
#endif
};

#undef UE_API
