// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraSplatRendererProperties.h"

#include "Modules/ModuleManager.h"
#include "NiagaraEditorModule.h"

#define LOCTEXT_NAMESPACE "GaussianSplatNiagaraEditorModule"

class FGaussianSplatNiagaraEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Adds Splat Renderer to Niagara's renderers in the emitter stack
		FNiagaraEditorModule& NiagaraEditorModule = FModuleManager::LoadModuleChecked<FNiagaraEditorModule>(TEXT("NiagaraEditor"));
		NiagaraEditorModule.RegisterRendererCreationInfo(FNiagaraRendererCreationInfo(
			UNiagaraSplatRendererProperties::StaticClass()->GetDisplayNameText(),
			LOCTEXT("SplatRendererDescription", "Emits each particle as a splat, rendered by the Gaussian Splat plug-in"),
			UNiagaraSplatRendererProperties::StaticClass()->GetClassPathName(),
			FNiagaraRendererCreationInfo::FRendererFactory::CreateLambda(
				[](UObject* OuterEmitter)
				{
					return NewObject<UNiagaraSplatRendererProperties>(OuterEmitter, NAME_None, RF_Transactional);
				})));
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGaussianSplatNiagaraEditorModule, GaussianSplatNiagaraEditor)
