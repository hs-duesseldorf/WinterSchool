using UnrealBuildTool;

public class SkinnedSplatEditor : ModuleRules
{
	public SkinnedSplatEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "SkinnedSplat" });
		PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "AssetTools", "AssetRegistry", "Slate", "SlateCore" });
	}
}
