#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

class FSkinnedSplatModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const FString ShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("SkinnedSplat"))->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/SkinnedSplat"), ShaderDir);
	}
};

IMPLEMENT_MODULE(FSkinnedSplatModule, SkinnedSplat)
