// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatEditorModule.h"

#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "GaussianSplatEditorModule"

void FGaussianSplatEditorModule::StartupModule()
{
	if (IPluginManager::Get().FindEnabledPlugin(TEXT("Niagara")))
	{
		FModuleManager::Get().LoadModule(TEXT("GaussianSplatNiagaraEditor"));
	}
}

void FGaussianSplatEditorModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGaussianSplatEditorModule, GaussianSplatEditor)
