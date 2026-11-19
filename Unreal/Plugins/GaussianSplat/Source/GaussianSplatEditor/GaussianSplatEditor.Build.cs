// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class GaussianSplatEditor : ModuleRules
	{
		public GaussianSplatEditor(ReadOnlyTargetRules Target) : base(Target)
		{
			AddEngineThirdPartyPrivateStaticDependencies(
				Target,
				"zlib"
			);

			// We don't have zstd available on all platforms
			bool bHasZstd = (Target.Platform == UnrealTargetPlatform.Win64 && Target.Architecture == UnrealArch.X64) 
				|| Target.Platform == UnrealTargetPlatform.Linux
				|| Target.Platform == UnrealTargetPlatform.LinuxArm64;
			if (bHasZstd)
			{
				AddEngineThirdPartyPrivateStaticDependencies(Target, "zstd");
			}
			PrivateDefinitions.Add("GAUSSIANSPLAT_WITH_ZSTD=" + (bHasZstd ? "1" : "0"));

			PublicDependencyModuleNames.AddRange(
				new string[] {
					"Core",
					"CoreUObject",
					"Engine",
					"Slate",
					"SlateCore",
					"RenderCore",
					"RHI",
					"AssetDefinition",
					"EditorFramework",
					"UnrealEd",
					"AssetTools",
					"PropertyEditor",
					"Projects",
					"GaussianSplat"
				}
			);

			PrivateDependencyModuleNames.AddRange(
				new string[] {
					"AdvancedPreviewScene",
					"InputCore",
					"ToolMenus"
				}
			);
		}
	}
}
