using UnrealBuildTool;

public class SkinnedSplat : ModuleRules
{
	public SkinnedSplat(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// GaussianSplatRenderPipeline.h / GaussianSplatViewExtension.h live in Internal/ of another plugin.
		bTreatAsEngineModule = true;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Projects", "RenderCore", "RHI", "Renderer",
			"GaussianSplat", "GaussianSplatShader", "GaussianSplatSceneViewExtension"
		});
	}
}
