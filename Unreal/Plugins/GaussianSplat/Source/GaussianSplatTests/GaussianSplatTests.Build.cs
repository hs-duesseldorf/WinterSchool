// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GaussianSplatTests : ModuleRules
{
	public GaussianSplatTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"CQTest",
				"Projects",
				"UnrealEd",
				"RenderCore",
				"RHI",
				"GaussianSplat",
				"GaussianSplatEditor",
				"GaussianSplatSceneViewExtension"
			}
		);
	}
}
