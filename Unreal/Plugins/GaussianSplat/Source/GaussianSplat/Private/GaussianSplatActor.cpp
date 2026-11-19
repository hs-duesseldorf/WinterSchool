// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatActor.h"

#include "GaussianSplatComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GaussianSplatActor)

AGaussianSplatActor::AGaussianSplatActor()
{
	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	GaussianSplatComponent = CreateDefaultSubobject<UGaussianSplatComponent>(TEXT("GaussianSplatComponent"));
	GaussianSplatComponent->SetupAttachment(RootComponent);
}

#if WITH_EDITOR
bool AGaussianSplatActor::GetReferencedContentObjects(TArray<UObject*>& Objects) const
{
	Super::GetReferencedContentObjects(Objects);

	if (GaussianSplatComponent && GaussianSplatComponent->Asset)
	{
		Objects.Add(GaussianSplatComponent->Asset);
	}

	return true;
}
#endif
