// Copyright notice: see Plugins/Slosh/NOTICE.md.

using UnrealBuildTool;

public class Slosh : ModuleRules
{
	public Slosh(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"SloshCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"SloshRender",
			"PhysicsCore",
			"RenderCore",
			"RHI",
		});
	}
}
