// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

using UnrealBuildTool;

public class GitSourceControl : ModuleRules
{
	public GitSourceControl(ReadOnlyTargetRules Target) : base(Target)
	{
        OptimizeCode = CodeOptimization.InShippingBuildsOnly;

        PrivateDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"CoreUObject",
				"ApplicationCore",
				"Engine",
				"Slate",
				"SlateCore",
				"InputCore",
				"DesktopWidgets",
				"EditorStyle",
				"UnrealEd",
				"SourceControl",
				"AssetRegistry",
				"AssetTools",
				"AssetDefinition",
				"ContentBrowser",
				"WorkspaceMenuStructure",
				"Projects",
				"UnsavedAssetsTracker"
			}
		);

		if (Target.Version.MajorVersion == 5)
		{
			PrivateDependencyModuleNames.Add("ToolMenus");
		}
	}
}
