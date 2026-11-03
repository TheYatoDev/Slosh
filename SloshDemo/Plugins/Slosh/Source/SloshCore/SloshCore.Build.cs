// Copyright notice: see Plugins/Slosh/NOTICE.md.

using UnrealBuildTool;

public class SloshCore : ModuleRules
{
	public SloshCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"RenderCore",
			"RHI",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Projects",
		});
	}
}
