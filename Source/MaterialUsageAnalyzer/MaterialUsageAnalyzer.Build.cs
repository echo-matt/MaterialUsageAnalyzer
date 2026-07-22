// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class MaterialUsageAnalyzer : ModuleRules
{
	public MaterialUsageAnalyzer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"RHI",
				"RenderCore",
				"Slate",
				"SlateCore",
				"SlateIM",
				"UnrealEd",
				"Projects",
				"AssetRegistry",
				"ContentBrowser",
				"DesktopPlatform",
				"Niagara",
				"MaterialEditor",
			}
		);
	}
}
