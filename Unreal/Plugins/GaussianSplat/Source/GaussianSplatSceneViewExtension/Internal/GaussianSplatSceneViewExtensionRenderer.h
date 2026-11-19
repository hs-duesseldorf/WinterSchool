// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianSplatRenderer.h"

class FGaussianSplatSceneViewExtensionRenderer : public IGaussianSplatRenderer
{
public:
	virtual bool IsSupportedOn(EShaderPlatform Platform) const override;

	virtual void OnComponentRegistered(UGaussianSplatComponent* Component) override;
	virtual void OnComponentUnregistered(UGaussianSplatComponent* Component) override;
};
