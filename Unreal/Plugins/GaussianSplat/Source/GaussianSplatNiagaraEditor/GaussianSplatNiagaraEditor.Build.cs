// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class GaussianSplatNiagaraEditor : ModuleRules
	{
		public GaussianSplatNiagaraEditor(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

			PrivateDependencyModuleNames.AddRange(
				new[]
				{
					"Core",
					"CoreUObject",
					"Engine",
					"GaussianSplatNiagara",
					"Niagara",
					"NiagaraEditor"
				}
			);
		}
	}
}
