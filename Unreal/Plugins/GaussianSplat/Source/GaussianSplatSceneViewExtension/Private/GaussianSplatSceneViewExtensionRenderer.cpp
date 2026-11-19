// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatSceneViewExtensionRenderer.h"

#include "GaussianSplatComponent.h"
#include "GaussianSplatViewExtension.h"

#include "DataDrivenShaderPlatformInfo.h"

bool FGaussianSplatSceneViewExtensionRenderer::IsSupportedOn(EShaderPlatform Platform) const
{
	return GetMaxSupportedFeatureLevel(Platform) >= ERHIFeatureLevel::ES3_1;
}

void FGaussianSplatSceneViewExtensionRenderer::OnComponentRegistered(UGaussianSplatComponent* Component)
{
	FGaussianSplatViewExtension* Extension = IsValid(Component) ? GaussianSplat::FindViewExtension(Component->GetWorld()) : nullptr;
	if (Extension)
	{
		Extension->RegisterComponent(Component);
	}
}

void FGaussianSplatSceneViewExtensionRenderer::OnComponentUnregistered(UGaussianSplatComponent* Component)
{
	FGaussianSplatViewExtension* Extension = IsValid(Component) ? GaussianSplat::FindViewExtension(Component->GetWorld()) : nullptr;
	if (Extension)
	{
		Extension->UnregisterComponent(Component);
	}
}
