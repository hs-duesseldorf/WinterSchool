// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class GaussianSplatNiagaraShader : ModuleRules
	{
		public GaussianSplatNiagaraShader(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

			PrivateDependencyModuleNames.AddRange(
				new[]
				{
					"Core",
					"Engine",
					"RenderCore",
					"RHI"
				}
			);
		}
	}
}
