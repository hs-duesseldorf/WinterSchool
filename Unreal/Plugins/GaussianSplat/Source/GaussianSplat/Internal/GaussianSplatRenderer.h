// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RHIShaderPlatform.h"

class UGaussianSplatComponent;

#define UE_API GAUSSIANSPLAT_API

class IGaussianSplatRenderer
{
public:
	virtual ~IGaussianSplatRenderer() = default;

	virtual bool IsSupportedOn(EShaderPlatform Platform) const = 0;
	virtual void OnComponentRegistered(UGaussianSplatComponent* Component) = 0;
	virtual void OnComponentUnregistered(UGaussianSplatComponent* Component) = 0;
};

class FGaussianSplatRendererRegistry
{
public:
	UE_API static FGaussianSplatRendererRegistry& Get();

	UE_API void Register(IGaussianSplatRenderer* Renderer);
	UE_API void Unregister(IGaussianSplatRenderer* Renderer);

	UE_API IGaussianSplatRenderer* SelectRenderer(EShaderPlatform Platform) const;

private:
	FGaussianSplatRendererRegistry() = default;

	TArray<IGaussianSplatRenderer*> Renderers;
};

#undef UE_API
