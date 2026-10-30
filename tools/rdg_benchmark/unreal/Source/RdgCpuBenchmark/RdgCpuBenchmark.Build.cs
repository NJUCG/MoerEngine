using UnrealBuildTool;

public class RdgCpuBenchmark : ModuleRules
{
    public RdgCpuBenchmark(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "RenderCore", "RHI" });
    }
}
