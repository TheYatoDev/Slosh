// Copyright notice: see Plugins/Slosh/NOTICE.md.

using UnrealBuildTool;

public class SloshEditor : ModuleRules
{
	public SloshEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slosh",
			"Slate",
			"SlateCore",
			"InputCore",
			"EditorFramework",
			"UnrealEd",
			"InteractiveToolsFramework",
			"EditorInteractiveToolsFramework",
		});
	}
}
