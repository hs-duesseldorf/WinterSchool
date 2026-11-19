// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "Engine/EngineBaseTypes.h"

#if WITH_EDITOR
#include "EditorUndoClient.h"
#endif

#include "GaussianSplatSubsystem.generated.h"

class FGaussianSplatViewExtension;

UCLASS(MinimalAPI)
class UGaussianSplatSubsystem : public UWorldSubsystem
{
public:
	GENERATED_BODY()

public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	TSharedPtr<FGaussianSplatViewExtension> SceneViewExtension;
};