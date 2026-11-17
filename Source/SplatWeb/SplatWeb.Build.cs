// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class SplatWeb : ModuleRules
{
	public SplatWeb(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		PublicIncludePaths.AddRange(new string[] {
			"SplatWeb",
			"SplatWeb/Variant_Platforming",
			"SplatWeb/Variant_Platforming/Animation",
			"SplatWeb/Variant_Combat",
			"SplatWeb/Variant_Combat/AI",
			"SplatWeb/Variant_Combat/Animation",
			"SplatWeb/Variant_Combat/Gameplay",
			"SplatWeb/Variant_Combat/Interfaces",
			"SplatWeb/Variant_Combat/UI",
			"SplatWeb/Variant_SideScrolling",
			"SplatWeb/Variant_SideScrolling/AI",
			"SplatWeb/Variant_SideScrolling/Gameplay",
			"SplatWeb/Variant_SideScrolling/Interfaces",
			"SplatWeb/Variant_SideScrolling/UI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
