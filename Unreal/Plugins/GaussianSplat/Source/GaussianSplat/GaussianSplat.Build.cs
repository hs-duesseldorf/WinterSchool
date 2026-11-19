// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class GaussianSplat : ModuleRules
	{
		public GaussianSplat(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

			PublicDependencyModuleNames.AddRange(new[] { "Core", "GaussianSplatShader" });

			PrivateDependencyModuleNames.AddRange(
				new[]
				{
					"CoreUObject",
					"Engine",
					"Projects",
					"RenderCore",
					"RHI"
				}
			);

			if (Target.bBuildEditor == true)
			{
				PrivateDependencyModuleNames.AddRange(
					new[]
					{
						"DerivedDataCache",
						"TargetPlatform",
						"UnrealEd"
					}
				);

				// Necessary for JacobiSVD::EigenSolver3
				PrivateIncludePathModuleNames.Add("NaniteUtilities");
			}
		}
	}
}
