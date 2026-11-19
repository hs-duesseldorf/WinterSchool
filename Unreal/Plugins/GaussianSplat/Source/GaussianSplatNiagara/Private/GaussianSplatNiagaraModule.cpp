// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraSplatRendererProperties.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

namespace GaussianSplatNiagaraLocal
{
	const TCHAR* ContentRootPath = TEXT("/GaussianSplatNiagara/");
}

class FGaussianSplatNiagaraModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GaussianSplat")))
		{
			ContentPath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Niagara"));
			FPackageName::RegisterMountPoint(GaussianSplatNiagaraLocal::ContentRootPath, ContentPath);
		}

		UNiagaraSplatRendererProperties::InitCDOPropertiesAfterModuleStartup();
	}

	virtual void ShutdownModule() override
	{
		if (!ContentPath.IsEmpty())
		{
			FPackageName::UnRegisterMountPoint(GaussianSplatNiagaraLocal::ContentRootPath, ContentPath);
		}
	}

private:
	FString ContentPath;
};

IMPLEMENT_MODULE(FGaussianSplatNiagaraModule, GaussianSplatNiagara)
