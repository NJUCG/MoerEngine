using UnrealBuildTool;

public class RdgCpuBenchmarkEditorTarget : TargetRules
{
    public RdgCpuBenchmarkEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V6;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
        ExtraModuleNames.Add("RdgCpuBenchmark");
    }
}
