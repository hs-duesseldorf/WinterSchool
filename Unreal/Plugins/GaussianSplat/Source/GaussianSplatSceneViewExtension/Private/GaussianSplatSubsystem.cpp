// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatSubsystem.h"

#include "GaussianSplatViewExtension.h"
//#include "GaussianSplatsModule.h"
//#include "GaussianSplatsActor.h"

#include "Engine/World.h"
#include "EngineUtils.h"


void UGaussianSplatSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	SceneViewExtension = FSceneViewExtensions::NewExtension<FGaussianSplatViewExtension>(GetWorld());
}

void UGaussianSplatSubsystem::Deinitialize()
{
	// Due to a flaw in SVE APIs, it may be kept alive unless we explicitely deactivate it.
	//SceneViewExtension->bIsActive = false;
	SceneViewExtension.Reset();
}

bool UGaussianSplatSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (Super::ShouldCreateSubsystem(Outer))
	{
		return true;
	}

	if (const UWorld* World = Cast<UWorld>(Outer))
	{
		return World->WorldType == EWorldType::EditorPreview;
	}

	return false;
}
