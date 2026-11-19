// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class GaussianSplatSceneViewExtension : ModuleRules
	{
		public GaussianSplatSceneViewExtension(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

			PublicDependencyModuleNames.AddRange(new[] { "Core" });

			PrivateDependencyModuleNames.AddRange(
				new[]
				{
					"CoreUObject",
					"Engine",
					"Renderer",
					"RenderCore",
					"RHI",
					"Projects",
					"GaussianSplat",
					"GaussianSplatShader"
				}
			);

			PrivateIncludePaths.Add(System.IO.Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private"));
			// WinterSchool fork: as a project plugin, Internal headers of engine modules are not visible by default
			PrivateIncludePaths.Add(System.IO.Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Internal"));
		}
	}
}
