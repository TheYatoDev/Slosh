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
			"GeometryCore",        // Slosh Paint: dynamic mesh + marching cubes
			"GeometryFramework",   // Slosh Paint: UDynamicMeshComponent
			"InputCore",           // painter actor key bindings
			"RenderCore",
			"RHI",
		});
	}
}
