// Copyright (c) 2026

using UnrealBuildTool;
using System.IO;

public class GitSourceControlTests : ModuleRules
{
	public GitSourceControlTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PrecompileForTargets = PrecompileTargetsType.Any;
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "GitSourceControl", "Private"));
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetTools",
				"Core",
				"CoreUObject",
				"DataValidation",
				"Engine",
				"GitSourceControl",
				"SourceControl",
				"UnrealEd",
				"Projects"
			});
	}
}
