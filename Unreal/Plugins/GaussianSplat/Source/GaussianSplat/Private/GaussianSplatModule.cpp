// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatModule.h"
#include "GaussianSplat.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatRenderResource.h"

#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "FGaussianSplatModule"

static TAutoConsoleVariable<int32> CVarGaussianSplat(
	TEXT("r.GaussianSplat"),
	1,
	TEXT("Enables Gaussian Splats (default = 1)"),
	ECVF_RenderThreadSafe);

namespace GaussianSplat
{
	bool IsEnabled()
	{
		return CVarGaussianSplat.GetValueOnAnyThread() != 0;
	}
}

void FGaussianSplatModule::StartupModule()
{
#if WITH_EDITORONLY_DATA
	OnSplatDataChangedHandle = UGaussianSplat::GetOnDataChanged().AddLambda([](UGaussianSplat* ChangedAsset)
	{
		for (TObjectIterator<UGaussianSplatComponent> It; It; ++It)
		{
			if (UGaussianSplatComponent* C = *It; IsValid(C) && C->Asset == ChangedAsset && C->IsRegistered())
			{
				C->ReregisterComponent();
			}
		}
	});
#endif

	// Module loaded here (potentially before Niagara), so global shaders can be registered
	if (IPluginManager::Get().FindEnabledPlugin(TEXT("Niagara")))
	{
		FModuleManager::Get().LoadModule(TEXT("GaussianSplatNiagaraShader"));
	}

	if (FModuleManager::Get().IsModuleLoaded(TEXT("Niagara")))
	{
		FModuleManager::Get().LoadModule(TEXT("GaussianSplatNiagara"));
	}
	else
	{
		ModulesChangedHandle = FModuleManager::Get().OnModulesChanged().AddLambda(
			[](FName ModuleName, EModuleChangeReason ChangeReason)
			{
				if (ModuleName == TEXT("Niagara") && ChangeReason == EModuleChangeReason::ModuleLoaded)
				{
					FModuleManager::Get().LoadModule(TEXT("GaussianSplatNiagara"));
				}
			}
		);
	}
}

void FGaussianSplatModule::ShutdownModule()
{
#if WITH_EDITORONLY_DATA
	if (OnSplatDataChangedHandle.IsValid())
	{
		UGaussianSplat::GetOnDataChanged().Remove(OnSplatDataChangedHandle);
		OnSplatDataChangedHandle.Reset();
	}
#endif

	if (ModulesChangedHandle.IsValid())
	{
		FModuleManager::Get().OnModulesChanged().Remove(ModulesChangedHandle);
		ModulesChangedHandle.Reset();
	}

	FGaussianSplatRenderResource::ReleaseEmptyResource();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGaussianSplatModule, GaussianSplat)
