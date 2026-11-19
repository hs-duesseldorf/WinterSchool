// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatSceneViewExtensionModule.h"

#include "GaussianSplatInverseToneCurveLUTCache.h"
#include "GaussianSplatRenderStats.h"
#include "GaussianSplatRenderer.h"
#include "GaussianSplatSceneViewExtensionRenderer.h"
#include "GaussianSplatViewExtension.h"
#include "RendererInterface.h"
#include "EngineModule.h"
#include "Misc/CoreDelegates.h"

static TUniquePtr<FGaussianSplatSceneViewExtensionRenderer> GSceneViewExtensionRenderer;

FGaussianSplatSceneViewExtensionModule::~FGaussianSplatSceneViewExtensionModule() = default;

void FGaussianSplatSceneViewExtensionModule::StartupModule()
{
	InverseToneCurveLUTCache = MakeShared<GaussianSplat::FInverseToneCurveLUTCache, ESPMode::ThreadSafe>();
	RenderStats = MakeUnique<GaussianSplat::FRenderStats>();

	GSceneViewExtensionRenderer = MakeUnique<FGaussianSplatSceneViewExtensionRenderer>();
	FGaussianSplatRendererRegistry::Get().Register(GSceneViewExtensionRenderer.Get());

	// Mobile deferred GBuffer path (FGaussianSplatViewExtension::PostOpaqueRender_RenderThread). The renderer module loads
	// after this one (PostConfigInit), so bind once the engine is up, before any scene renders.
	if (FModuleManager::Get().IsModuleLoaded(TEXT("Renderer")))
	{
		RegisterPostOpaqueRenderDelegate();
	}
	else
	{
		PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FGaussianSplatSceneViewExtensionModule::RegisterPostOpaqueRenderDelegate);
	}
}

void FGaussianSplatSceneViewExtensionModule::RegisterPostOpaqueRenderDelegate()
{
	if (!PostOpaqueRenderHandle.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("Renderer")))
	{
		PostOpaqueRenderHandle = GetRendererModule().RegisterPostOpaqueRenderDelegate(
			FPostOpaqueRenderDelegate::CreateStatic(&FGaussianSplatViewExtension::PostOpaqueRender_RenderThread));
	}
}

void FGaussianSplatSceneViewExtensionModule::ShutdownModule()
{
	FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
	if (PostOpaqueRenderHandle.IsValid())
	{
		if (IRendererModule* RendererModule = FModuleManager::GetModulePtr<IRendererModule>(TEXT("Renderer")))
		{
			RendererModule->RemovePostOpaqueRenderDelegate(PostOpaqueRenderHandle);
		}
		PostOpaqueRenderHandle.Reset();
	}

	if (GSceneViewExtensionRenderer.IsValid())
	{
		FGaussianSplatRendererRegistry::Get().Unregister(GSceneViewExtensionRenderer.Get());
		GSceneViewExtensionRenderer.Reset();
	}

	InverseToneCurveLUTCache.Reset();

	RenderStats.Reset();
}

FRHITexture* FGaussianSplatSceneViewExtensionModule::GetInverseToneCurveLUTRHI_RenderThread(bool bInvertExpandGamut)
{
	return InverseToneCurveLUTCache ? InverseToneCurveLUTCache->GetRHI_RenderThread(bInvertExpandGamut) : nullptr;
}

IMPLEMENT_MODULE(FGaussianSplatSceneViewExtensionModule, GaussianSplatSceneViewExtension)
