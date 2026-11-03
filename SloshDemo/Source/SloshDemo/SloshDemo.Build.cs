using UnrealBuildTool;

public class SloshDemo : ModuleRules
{
	public SloshDemo(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "RHI", "Slosh" });
	}
}
