// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatShaderModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

IMPLEMENT_MODULE(FGaussianSplatShaderModule, GaussianSplatShader);

void FGaussianSplatShaderModule::StartupModule()
{
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("GaussianSplat"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/GaussianSplat"), PluginShaderDir);
	AddShaderSourceSharedVirtualDirectory(TEXT("/Plugin/GaussianSplat/Shared/"));
}

void FGaussianSplatShaderModule::ShutdownModule()
{
}
