using UnrealBuildTool;

public class SloshDemoEditorTarget : TargetRules
{
	public SloshDemoEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		ExtraModuleNames.Add("SloshDemo");
	}
}
