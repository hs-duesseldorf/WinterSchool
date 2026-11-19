// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatRenderer.h"

FGaussianSplatRendererRegistry& FGaussianSplatRendererRegistry::Get()
{
	static FGaussianSplatRendererRegistry Instance;
	return Instance;
}

void FGaussianSplatRendererRegistry::Register(IGaussianSplatRenderer* Renderer)
{
	check(IsInGameThread());
	check(Renderer != nullptr);

	if (Renderers.Contains(Renderer))
	{
		return;
	}

	Renderers.Add(Renderer);
}

void FGaussianSplatRendererRegistry::Unregister(IGaussianSplatRenderer* Renderer)
{
	check(IsInGameThread());
	check(Renderer != nullptr);
	Renderers.Remove(Renderer);
}

IGaussianSplatRenderer* FGaussianSplatRendererRegistry::SelectRenderer(EShaderPlatform Platform) const
{
	for (IGaussianSplatRenderer* Renderer : Renderers)
	{
		if (Renderer->IsSupportedOn(Platform))
		{
			return Renderer;
		}
	}
	return nullptr;
}
