// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class GaussianSplatShader : ModuleRules
{
    public GaussianSplatShader(ReadOnlyTargetRules Target) : base(Target)
    {
		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "../../Shaders/Shared"));

		PrivateDependencyModuleNames.AddRange(
            new string[] {
                "CoreUObject",
                "Engine",
                "Renderer",
            }
        );

        PublicDependencyModuleNames.AddRange(
            new string[] {
                "Core",
                "RenderCore",
                "RHI",
                "Projects",
            }
        );
    }
}
