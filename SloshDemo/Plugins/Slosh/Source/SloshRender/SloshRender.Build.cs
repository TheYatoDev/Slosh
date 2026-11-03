// Copyright notice: see Plugins/Slosh/NOTICE.md.

using UnrealBuildTool;

public class SloshRender : ModuleRules
{
	public SloshRender(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"SloshCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"RenderCore",
			"Renderer",
			"RHI",
		});
	}
}
