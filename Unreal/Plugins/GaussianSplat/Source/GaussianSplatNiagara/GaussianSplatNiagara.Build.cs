// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class GaussianSplatNiagara : ModuleRules
	{
		public GaussianSplatNiagara(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

			PublicDependencyModuleNames.AddRange(
				new[]
				{
					"Core",
					"GaussianSplat",
					"Niagara",
					"NiagaraCore"
				}
			);

			PrivateDependencyModuleNames.AddRange(
				new[]
				{
					"CoreUObject",
					"Engine",
					"Projects",
					"Renderer",
					"RenderCore",
					"RHI",
					"Slate",
					"SlateCore",
					"VectorVM",
					"GaussianSplatShader",
					"GaussianSplatSceneViewExtension",
					"GaussianSplatNiagaraShader"
				}
			);
		}
	}
}
