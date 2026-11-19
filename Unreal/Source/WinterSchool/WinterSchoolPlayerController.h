// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "WinterSchoolPlayerController.generated.h"

class UInputMappingContext;

/** PlayerController that adds the input mapping contexts. */
UCLASS(abstract)
class AWinterSchoolPlayerController : public APlayerController
{
	GENERATED_BODY()

protected:

	/** Input Mapping Contexts */
	UPROPERTY(EditAnywhere, Category = "Input|Input Mappings")
	TArray<UInputMappingContext*> DefaultMappingContexts;

	/** Extra Input Mapping Contexts (mouse look) */
	UPROPERTY(EditAnywhere, Category = "Input|Input Mappings")
	TArray<UInputMappingContext*> MobileExcludedMappingContexts;

	virtual void SetupInputComponent() override;
};
