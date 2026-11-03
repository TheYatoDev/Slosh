using UnrealBuildTool;

public class SloshDemoTarget : TargetRules
{
	public SloshDemoTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		ExtraModuleNames.Add("SloshDemo");
	}
}
