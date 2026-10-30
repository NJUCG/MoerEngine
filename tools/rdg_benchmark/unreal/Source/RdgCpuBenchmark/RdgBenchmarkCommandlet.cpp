#include "RdgBenchmarkCommandlet.h"
#include "RdgPrivateStats.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, RdgCpuBenchmark)

namespace
{
BEGIN_SHADER_PARAMETER_STRUCT(FBenchmarkParameters, )
    RDG_TEXTURE_ACCESS_ARRAY(Textures)
    RDG_BUFFER_ACCESS_ARRAY(Buffers)
END_SHADER_PARAMETER_STRUCT()

struct FOptions
{
    int32 Passes = 64;
    int32 Resources = 64;
    int32 Accesses = 16;
    int32 Warmup = 20;
    int32 Samples = 200;
    int32 Cpu = -1;
    bool bWorkStats = false;
};

double Percentile(TArray<double> Values, double Fraction)
{
    Values.Sort();
    return Values[FMath::Min(int32(Values.Num() * Fraction), Values.Num() - 1)];
}

FString Measure(FRHICommandListImmediate& Commands, const FOptions& Options)
{
    FPrivateRdgStats Stats(Options.bWorkStats);
    // Physical allocation is outside graph timing, matching Moer's imported fake resources.
    TArray<TRefCountPtr<IPooledRenderTarget>> Textures;
    TArray<TRefCountPtr<FRDGPooledBuffer>> Buffers;
    for (int32 Index = 0; Index < Options.Resources; ++Index)
    {
        const auto TextureDesc = FRHITextureCreateDesc::Create2DArray(TEXT("BenchmarkTexture"), 64, 64, 2, PF_R8G8B8A8)
            .SetNumMips(4).SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV)
            .SetInitialState(ERHIAccess::SRVCompute);
        Textures.Add(CreateRenderTarget(Commands.CreateTexture(TextureDesc), TEXT("BenchmarkTexture")));
        const auto BufferDesc = FRDGBufferDesc::CreateStructuredDesc(4, 1024);
        const auto CreateDesc = FRHIBufferCreateDesc::Create(TEXT("BenchmarkBuffer"), 4096, 4, BufferDesc.Usage)
            .SetInitialState(ERHIAccess::SRVCompute);
        Buffers.Add(new FRDGPooledBuffer(Commands, Commands.CreateBuffer(CreateDesc), BufferDesc, 1024, TEXT("BenchmarkBuffer")));
    }
    TArray<double> BuildTimes, ExecuteTimes, TotalTimes, DestroyTimes, DrainTimes, DrainedTotalTimes;
    for (auto* Times : { &BuildTimes, &ExecuteTimes, &TotalTimes, &DestroyTimes, &DrainTimes, &DrainedTotalTimes })
    {
        Times->Reserve(Options.Samples);
    }
    const auto StatsBefore = Stats.Read();
    for (int32 Iteration = 0; Iteration < Options.Warmup + Options.Samples; ++Iteration)
    {
        int32 RecordedPasses = 0;
        const double Begin = FPlatformTime::Seconds();
        double Built = 0, Executed = 0;
        {
            FRDGBuilder Graph(Commands, RDG_EVENT_NAME("CpuBenchmark"), ERDGBuilderFlags::None);
            TArray<FRDGTextureRef> GraphTextures;
            TArray<FRDGBufferRef> GraphBuffers;
            for (int32 Index = 0; Index < Options.Resources; ++Index)
            {
                GraphTextures.Add(Graph.RegisterExternalTexture(Textures[Index]));
                GraphBuffers.Add(Graph.RegisterExternalBuffer(Buffers[Index]));
                Graph.SetTextureAccessFinal(GraphTextures.Last(), ERHIAccess::SRVCompute);
                Graph.SetBufferAccessFinal(GraphBuffers.Last(), ERHIAccess::SRVCompute);
            }
            for (int32 Pass = 0; Pass < Options.Passes; ++Pass)
            {
                auto* Parameters = Graph.AllocParameters<FBenchmarkParameters>();
                const auto Access = Pass % 4 == 0 ? ERHIAccess::UAVCompute : ERHIAccess::SRVCompute;
                for (int32 Slot = 0; Slot < Options.Accesses; ++Slot)
                {
                    const int32 Index = (Pass * 3 + Slot) % Options.Resources;
                    Parameters->Textures.Emplace(GraphTextures[Index], Access);
                    Parameters->Buffers.Emplace(GraphBuffers[Index], Access);
                }
                Graph.AddPass(RDG_EVENT_NAME("Pass%d", Pass), Parameters,
                    ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
                    [&RecordedPasses](FRHICommandList&) { ++RecordedPasses; });
            }
            Built = FPlatformTime::Seconds();
            Graph.Execute();
            Executed = FPlatformTime::Seconds();
        }
        const double End = FPlatformTime::Seconds();
        // Report this separately as well as including it in an additional total.
        Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
        const double Drained = FPlatformTime::Seconds();
        checkf(RecordedPasses == Options.Passes, TEXT("Benchmark passes must not be culled"));
        if (Iteration >= Options.Warmup)
        {
            BuildTimes.Add((Built - Begin) * 1000);
            ExecuteTimes.Add((Executed - Built) * 1000);
            TotalTimes.Add((End - Begin) * 1000);
            DestroyTimes.Add((End - Executed) * 1000);
            DrainTimes.Add((Drained - End) * 1000);
            DrainedTotalTimes.Add((Drained - Begin) * 1000);
        }
    }
    const auto StatsAfter = Stats.Read();
    return FString::Printf(TEXT("{\"engine\":\"UE5.7.1\",\"backend\":\"NullRHI\",\"passes\":%d,\"resources_per_kind\":%d,\"accesses_per_kind_per_pass\":%d,\"warmup\":%d,\"samples\":%d,\"cpu\":%d,\"build_p50_ms\":%.6f,\"execute_p50_ms\":%.6f,\"lifecycle_p50_ms\":%.6f,\"lifecycle_p95_ms\":%.6f,\"destruction_p50_ms\":%.6f,\"drain_p50_ms\":%.6f,\"lifecycle_with_drain_p50_ms\":%.6f,\"work_stats\":%s}\n"),
        Options.Passes, Options.Resources, Options.Accesses, Options.Warmup, Options.Samples, Options.Cpu,
        Percentile(BuildTimes, .5), Percentile(ExecuteTimes, .5), Percentile(TotalTimes, .5), Percentile(TotalTimes, .95),
        Percentile(DestroyTimes, .5), Percentile(DrainTimes, .5), Percentile(DrainedTotalTimes, .5), *Stats.Json(StatsBefore, StatsAfter));
}
}

URdgBenchmarkCommandlet::URdgBenchmarkCommandlet()
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = true;
}

int32 URdgBenchmarkCommandlet::Main(const FString& Params)
{
    FOptions Options;
    FParse::Value(*Params, TEXT("Passes="), Options.Passes);
    FParse::Value(*Params, TEXT("Resources="), Options.Resources);
    FParse::Value(*Params, TEXT("Accesses="), Options.Accesses);
    FParse::Value(*Params, TEXT("Warmup="), Options.Warmup);
    FParse::Value(*Params, TEXT("Samples="), Options.Samples);
    FParse::Value(*Params, TEXT("Cpu="), Options.Cpu);
    Options.bWorkStats = FParse::Param(*Params, TEXT("WorkStats"));
    FString Output;
    FParse::Value(*Params, TEXT("Output="), Output);
    if (Options.Passes <= 0 || Options.Resources <= 0 || Options.Accesses <= 0 ||
        Options.Accesses > Options.Resources || Options.Warmup < 0 || Options.Samples <= 0 ||
        Options.Cpu < -1 || Options.Cpu >= 64 || Output.IsEmpty())
    {
        return 1;
    }
    for (const TCHAR* Name : { TEXT("r.RDG.ParallelSetup"), TEXT("r.RDG.ParallelCompile"),
        TEXT("r.RDG.AsyncSetupQueue"), TEXT("r.RDG.ParallelExecute"), TEXT("r.RDG.ParallelDestruction") })
    {
        if (auto* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
        {
            Variable->Set(0, ECVF_SetByCode);
        }
    }
    FString Result;
    ENQUEUE_RENDER_COMMAND(RdgCpuBenchmark)([&](FRHICommandListImmediate& Commands)
    {
        if (Options.Cpu >= 0) { FPlatformProcess::SetThreadAffinityMask(uint64{1} << Options.Cpu); }
        if (FParse::Param(*Params, TEXT("Matrix")))
        {
            Result = TEXT("[\n");
            for (int32 Run = 0; Run < 3; ++Run)
            {
                for (const auto& Size : { FIntVector(16, 16, 4), FIntVector(64, 64, 16), FIntVector(128, 128, 16) })
                {
                    auto Case = Options;
                    Case.Passes = Size.X;
                    Case.Resources = Size.Y;
                    Case.Accesses = Size.Z;
                    if (Run != 0 || Size.X != 16) { Result += TEXT(",\n"); }
                    Result += Measure(Commands, Case).TrimEnd();
                }
            }
            Result += TEXT("\n]\n");
        }
        else
        {
            Result = Measure(Commands, Options);
        }
    });
    FlushRenderingCommands();
    if (!FFileHelper::SaveStringToFile(Result, *Output, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to save RDG benchmark result to %s"), *Output);
        return 1;
    }
    return 0;
}
