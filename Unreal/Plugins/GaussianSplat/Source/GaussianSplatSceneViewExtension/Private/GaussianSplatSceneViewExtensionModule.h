// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Templates/SharedPointer.h"

class FRHITexture;
namespace GaussianSplat
{
    class FInverseToneCurveLUTCache;
    class FRenderStats;
}

class FGaussianSplatSceneViewExtensionModule : public IModuleInterface
{
public:
	// Out-of-line so the InverseToneCurveLUTCache TSharedPtr can hold a forward-declared type.
	virtual ~FGaussianSplatSceneViewExtensionModule() override;

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	// Returns the requested inverse-tone-curve LUT's RHI once resident, or nullptr while its async load is
	// still in flight (or the cache is unavailable). Render-thread only; gate the post-blend pass on non-null.
	FRHITexture* GetInverseToneCurveLUTRHI_RenderThread(bool bInvertExpandGamut);

	GaussianSplat::FRenderStats& GetRenderStats_RenderThread() { return *RenderStats; }

private:
	void RegisterPostOpaqueRenderDelegate();

	FDelegateHandle PostEngineInitHandle;
	FDelegateHandle PostOpaqueRenderHandle;
	TSharedPtr<GaussianSplat::FInverseToneCurveLUTCache, ESPMode::ThreadSafe> InverseToneCurveLUTCache;
	TUniquePtr<GaussianSplat::FRenderStats> RenderStats;
};
