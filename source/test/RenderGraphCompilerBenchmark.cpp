#include "rendergraph/RenderGraph.h"
#include "rendergraph/RenderGraphLowering.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIImpl.h"

#include "misc/Types.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {

using Moer::Render::RenderGraph;
using Moer::Render::RenderGraphLowering;

struct Options {
    Moer::uint32 passes    = 64;
    Moer::uint32 resources = 64;
    Moer::uint32 accesses  = 16;
    Moer::uint32 warmup    = 5;
    Moer::uint32 samples   = 80;
    bool subranges = false;
    bool lower = false;
    bool record = false;
    bool lifecycle = false;
};

Options ParseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--lifecycle") {
            options.lifecycle = true;
            continue;
        }
        if (option == "--record") {
            options.record = true;
            continue;
        }
        if (option == "--lower") {
            options.lower = true;
            continue;
        }
        if (option == "--subranges") {
            options.subranges = true;
            continue;
        }
        if (index + 1 == argc) {
            throw std::runtime_error("numeric options require an unsigned integer value");
        }
        const std::string_view text = argv[++index];
        Moer::uint32           value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size()) {
            throw std::runtime_error("invalid unsigned integer: " + std::string(text));
        }
        if (option == "--passes") {
            options.passes = value;
        } else if (option == "--resources") {
            options.resources = value;
        } else if (option == "--accesses") {
            options.accesses = value;
        } else if (option == "--warmup") {
            options.warmup = value;
        } else if (option == "--samples") {
            options.samples = value;
        } else {
            throw std::runtime_error("unknown option: " + std::string(option));
        }
    }
    if (int(options.lower) + int(options.record) + int(options.lifecycle) > 1) {
        throw std::runtime_error("choose one of --lower, --record or --lifecycle");
    }
    if (options.passes == 0 || options.resources == 0 || options.accesses == 0 ||
        options.accesses > options.resources || options.samples == 0) {
        throw std::runtime_error("passes, resources, accesses and samples must be positive; accesses <= resources");
    }
    return options;
}

class BenchmarkTexture final : public Moer::Render::Texture {
public:
    BenchmarkTexture() : Texture(MakeInfo()) {}

    Moer::uint32 GetMipByteSize(Moer::uint32) const override {
        return 4;
    }
    void SetName(std::string_view) override {}

private:
    static Moer::Render::TextureInfo MakeInfo() {
        Moer::Render::TextureInfo info{};
        info.usage = ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS;
        info.extent = {64, 64};
        info.num_mips = 4;
        info.array_size = 2;
        info.aspect_flags = ETextureAspectFlags::COLOR;
        return info;
    }
};

class BenchmarkBuffer final : public Moer::Render::Buffer {
public:
    BenchmarkBuffer() : Buffer(Moer::Render::BufferInfo{4096, 1, EBufferUsageFlags::UNORDERED_ACCESS}) {}
    void SetName(std::string_view) override {}
};

struct PhysicalResources {
    std::vector<Moer::Render::TextureRef> textures;
    std::vector<Moer::Render::BufferRef> buffers;

    explicit PhysicalResources(Moer::uint32 count) {
        textures.reserve(count);
        buffers.reserve(count);
        for (Moer::uint32 index = 0; index < count; ++index) {
            textures.emplace_back(MoerNew(BenchmarkTexture)());
            buffers.emplace_back(MoerNew(BenchmarkBuffer)());
        }
    }
};

void BuildGraph(RenderGraph& graph, const Options& options, const PhysicalResources& physical,
                Moer::uint32* recorded_passes = nullptr) {
    std::vector<RenderGraph::TextureHandle> textures(options.resources);
    std::vector<RenderGraph::BufferHandle> buffers(options.resources);
    for (Moer::uint32 index = 0; index < options.resources; ++index) {
        textures[index] = graph.ImportTexture(
            "Texture" + std::to_string(index), physical.textures[index], {.mip_count = 4, .layer_count = 2});
        graph.SetInitialState(textures[index], RenderGraph::TextureAccessType::Sampled,
            RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
        graph.Export(textures[index], RenderGraph::TextureAccessType::Sampled,
            RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
        buffers[index] = graph.ImportBuffer(
            "Buffer" + std::to_string(index), physical.buffers[index], {.byte_size = 4096});
        graph.SetInitialState(buffers[index], RenderGraph::BufferAccessType::Storage,
            RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
        graph.Export(buffers[index], RenderGraph::BufferAccessType::Storage,
            RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
    }
    for (Moer::uint32 pass = 0; pass < options.passes; ++pass) {
        graph.AddRecordPass("Pass" + std::to_string(pass), [&](RenderGraph::PassBuilder& builder) {
            for (Moer::uint32 access = 0; access < options.accesses; ++access) {
                const auto index         = (Moer::uint64{pass} * 3 + access) % options.resources;
                auto texture_range = RenderGraph::TextureRange::Whole();
                auto buffer_range = RenderGraph::BufferRange::Whole();
                if (options.subranges) {
                    // Each touched resource has internal boundaries. Select from its
                    // eight mip/layer pairs and equal-sized buffer ranges without
                    // changing the number of accesses or the read/write pattern.
                    const auto slice = static_cast<Moer::uint32>((Moer::uint64{pass} + access) % 8);
                    texture_range = {.mip_first = slice % 4, .mip_count = 1,
                                     .layer_first = slice / 4, .layer_count = 1};
                    buffer_range     = {.offset = Moer::uint64{slice} * 512, .size = 512};
                }
                if (pass % 4 == 0) {
                    builder.Write(textures[index], RenderGraph::TextureAccessType::Storage, texture_range);
                    builder.Write(buffers[index], RenderGraph::BufferAccessType::Storage, buffer_range);
                } else {
                    builder.Read(textures[index], RenderGraph::TextureAccessType::Sampled, texture_range);
                    builder.Read(buffers[index], RenderGraph::BufferAccessType::Storage, buffer_range);
                }
            }
        },
                            [recorded_passes](Moer::Render::CommandList&) {
                                if (recorded_passes) {
                                    ++*recorded_passes;
                                }
                            },
                            RenderGraph::PassRecordPolicy::ParallelAllowed);
    }
}

Moer::uint64 Digest(std::string_view text) {
    Moer::uint64 digest = 14695981039346656037ull;
    for (unsigned char value : text) {
        digest = (digest ^ value) * 1099511628211ull;
    }
    return digest;
}

struct MemoryCounts {
    Moer::uint64 allocations   = 0;
    Moer::uint64 deallocations = 0;
    Moer::uint64 total_bytes   = 0;
    Moer::uint64 live_bytes    = 0;
    Moer::uint64 peak_bytes    = 0;
};

class CountingResource final : public std::pmr::memory_resource {
public:
    MemoryCounts counts;

private:
    void* do_allocate(size_t bytes, size_t alignment) override {
        void* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++counts.allocations;
        counts.total_bytes += bytes;
        counts.live_bytes += bytes;
        counts.peak_bytes = std::max(counts.peak_bytes, counts.live_bytes);
        return result;
    }

    void do_deallocate(void* memory, size_t bytes, size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(memory, bytes, alignment);
        ++counts.deallocations;
        counts.live_bytes -= bytes;
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

class ScopedDefaultResource {
public:
    explicit ScopedDefaultResource(std::pmr::memory_resource& resource) :
        previous(std::pmr::set_default_resource(&resource)) {}
    ~ScopedDefaultResource() { std::pmr::set_default_resource(previous); }
    ScopedDefaultResource(const ScopedDefaultResource&) = delete;
    ScopedDefaultResource& operator=(const ScopedDefaultResource&) = delete;

private:
    std::pmr::memory_resource* previous;
};

struct CrtCounts {
    Moer::uint64 allocation_requests          = 0;
    Moer::uint64 free_requests                = 0;
    Moer::uint64 reallocation_requests        = 0;
    Moer::uint64 allocation_requested_bytes   = 0;
    Moer::uint64 reallocation_requested_bytes = 0;
};

#if defined(_MSC_VER) && defined(_DEBUG)
// The benchmark creates no TaskGraph or worker threads. The hook must not allocate,
// print or inspect heap blocks; it counts requests, including CompiledPlan storage.
class ScopedCrtAllocHook {
public:
    explicit ScopedCrtAllocHook(CrtCounts& counts) {
        active_counts = &counts;
        previous_hook = _CrtSetAllocHook(CountAllocation);
    }
    ~ScopedCrtAllocHook() {
        _CrtSetAllocHook(previous_hook);
        active_counts = nullptr;
        previous_hook = nullptr;
    }
    ScopedCrtAllocHook(const ScopedCrtAllocHook&) = delete;
    ScopedCrtAllocHook& operator=(const ScopedCrtAllocHook&) = delete;

private:
    static int __cdecl CountAllocation(
        int type, void* data, size_t size, int block_type, long request,
        const unsigned char* file, int line
    ) {
        if (active_counts && _BLOCK_TYPE(block_type) != _CRT_BLOCK) {
            switch (type) {
                case _HOOK_ALLOC:
                    ++active_counts->allocation_requests;
                    active_counts->allocation_requested_bytes += size;
                    break;
                case _HOOK_FREE:
                    ++active_counts->free_requests;
                    break;
                case _HOOK_REALLOC:
                    ++active_counts->reallocation_requests;
                    active_counts->reallocation_requested_bytes += size;
                    break;
            }
        }
        return previous_hook ? previous_hook(type, data, size, block_type, request, file, line) : 1;
    }

    static inline CrtCounts* active_counts = nullptr;
    static inline _CRT_ALLOC_HOOK previous_hook = nullptr;
};
#endif

struct TimingResult {
    double p50_ms;
    double p95_ms;
    Moer::uint64 dump_digest;
};

void MeasureLifecycle(const Options& options, const PhysicalResources& physical) {
    using Clock = std::chrono::steady_clock;
    const auto milliseconds = [](auto duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    };
    std::vector<double> build_times, execute_times, total_times;
    build_times.reserve(options.samples);
    execute_times.reserve(options.samples);
    total_times.reserve(options.samples);
    for (Moer::uint64 iteration = 0; iteration < Moer::uint64{options.warmup} + options.samples; ++iteration) {
        Moer::uint32 recorded_passes = 0;
        const auto begin = Clock::now();
        Clock::time_point built, executed;
        {
            RenderGraph graph("LifecycleBenchmark");
            Moer::Render::CommandList commands;
            commands.SetResourceStateOwnership(Moer::Render::ERHIResourceStateOwnership::Compiled);
            BuildGraph(graph, options, physical, &recorded_passes);
            built = Clock::now();
            if (!graph.Compile() || !graph.RecordAndMergeFrontendCommands(commands, false)) {
                throw std::runtime_error(graph.GetCompileError());
            }
            executed = Clock::now();
        }
        const auto end = Clock::now();
        if (recorded_passes != options.passes) { throw std::runtime_error("not all passes were recorded"); }
        if (iteration >= options.warmup) {
            build_times.push_back(milliseconds(built - begin));
            execute_times.push_back(milliseconds(executed - built));
            total_times.push_back(milliseconds(end - begin));
        }
    }
    std::sort(build_times.begin(), build_times.end());
    std::sort(execute_times.begin(), execute_times.end());
    std::sort(total_times.begin(), total_times.end());
    std::cout << std::fixed << std::setprecision(6)
        << "{\"engine\":\"Moer\",\"backend\":\"FakeResources\",\"passes\":" << options.passes
        << ",\"resources_per_kind\":" << options.resources
        << ",\"accesses_per_kind_per_pass\":" << options.accesses
        << ",\"subranges\":" << std::boolalpha << options.subranges
        << ",\"warmup\":" << options.warmup << ",\"samples\":" << options.samples
        << ",\"build_p50_ms\":" << build_times[build_times.size() / 2]
        << ",\"execute_p50_ms\":" << execute_times[execute_times.size() / 2]
        << ",\"lifecycle_p50_ms\":" << total_times[total_times.size() / 2]
        << ",\"lifecycle_p95_ms\":" << total_times[total_times.size() * 95 / 100] << "}\n";
}

bool RunPhase(const Options& options, RenderGraph& graph,
              RenderGraphLowering::LoweredPlan& lowered, Moer::Render::CommandList& commands,
              std::string& error) {
    if (options.record) { return graph.RecordAndMergeFrontendCommands(commands, false); }
    if (options.lower) { return RenderGraphLowering::Lower(graph, lowered, error); }
    return graph.Compile();
}

TimingResult MeasurePhase(const Options& options, const PhysicalResources& physical) {
    using Clock = std::chrono::steady_clock;
    std::vector<double> samples;
    samples.reserve(options.samples);
    Moer::uint64       digest     = 0;
    const Moer::uint64 iterations = Moer::uint64{options.warmup} + options.samples;
    for (Moer::uint64 iteration = 0; iteration < iterations; ++iteration) {
        // A fresh graph per iteration matches a frame, without timing graph setup.
        RenderGraph graph("CompilerBenchmark");
        BuildGraph(graph, options, physical);
        if ((options.lower || options.record) && !graph.Compile()) {
            throw std::runtime_error(graph.GetCompileError());
        }
        Moer::Render::CommandList commands;
        commands.SetResourceStateOwnership(Moer::Render::ERHIResourceStateOwnership::Compiled);
        RenderGraphLowering::LoweredPlan lowered;
        std::string error;
        const auto begin = Clock::now();
        const bool compiled = RunPhase(options, graph, lowered, commands, error);
        const auto end = Clock::now();
        // Both phases return after their scratch storage dies; output destruction is excluded.
        if (!compiled) {
            throw std::runtime_error(options.lower ? error : graph.GetCompileError());
        }
        if (iteration >= options.warmup) {
            samples.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
        }
        if (iteration + 1 == iterations) {
            digest = Digest(graph.Dump());
        }
    }
    std::sort(samples.begin(), samples.end());
    // Same order-statistic convention as the earlier 80-sample benchmark.
    return {samples[samples.size() / 2], samples[samples.size() * 95 / 100], digest};
}

struct AllocationResult {
    MemoryCounts pmr;
    CrtCounts crt;
    size_t accesses;
    size_t edges;
    size_t barriers;
    Moer::uint64 lowered_digest;
};

AllocationResult MeasureAllocations(
    const Options&           options,
    const PhysicalResources& physical,
    Moer::uint64             expected_digest
) {
    CountingResource memory;
    CrtCounts crt;
    RenderGraph graph("CompilerBenchmark");
    BuildGraph(graph, options, physical);
    if ((options.lower || options.record) && !graph.Compile()) {
        throw std::runtime_error(graph.GetCompileError());
    }
    Moer::Render::CommandList commands;
    commands.SetResourceStateOwnership(Moer::Render::ERHIResourceStateOwnership::Compiled);
    RenderGraphLowering::LoweredPlan lowered;
    std::string error;
    bool compiled = false;
    {
        ScopedDefaultResource resource_scope(memory);
#if defined(_MSC_VER) && defined(_DEBUG)
        ScopedCrtAllocHook crt_scope(crt);
#endif
        compiled = RunPhase(options, graph, lowered, commands, error);
    }
    if (!compiled) {
        throw std::runtime_error(options.lower ? error : graph.GetCompileError());
    }
    if (memory.counts.live_bytes != 0 || memory.counts.allocations != memory.counts.deallocations) {
        throw std::runtime_error("Measured phase retained storage from the temporary PMR resource");
    }
    const auto dump = graph.Dump();
    if (Digest(dump) != expected_digest) {
        throw std::runtime_error("allocation instrumentation changed the compiled plan");
    }
    if (!options.lower && !RenderGraphLowering::Lower(graph, lowered, error)) {
        throw std::runtime_error(error);
    }
    const auto lowered_dump = lowered.Dump();
    if (lowered_dump.empty() || graph.Dump() != dump) {
        throw std::runtime_error("compiled output is not stable after scratch destruction and lowering");
    }
    const auto& plan = graph.GetCompiledPlan();
    return {memory.counts, crt, plan.accesses.size(), plan.edges.size(), plan.barriers.size(), Digest(lowered_dump)};
}

void PrintResult(const Options& options, const TimingResult& timing, const AllocationResult& memory) {
#if defined(_MSC_VER) && defined(_DEBUG)
    constexpr bool crt_available = true;
#else
    constexpr bool crt_available = false;
#endif
    std::cout << std::setprecision(9) << std::boolalpha
        << "{\"passes\":" << options.passes
        << ",\"textures\":" << options.resources << ",\"buffers\":" << options.resources
        << ",\"accesses_per_kind_per_pass\":" << options.accesses
        << ",\"subranges\":" << options.subranges
        << ",\"warmup\":" << options.warmup << ",\"samples\":" << options.samples
        << (options.record ? ",\"record_p50_ms\":" : options.lower ? ",\"lower_p50_ms\":" : ",\"compile_p50_ms\":") << timing.p50_ms
        << (options.record ? ",\"record_p95_ms\":" : options.lower ? ",\"lower_p95_ms\":" : ",\"compile_p95_ms\":") << timing.p95_ms
        << ",\"dump_fnv1a\":\"" << timing.dump_digest << "\",\"lowered_dump_fnv1a\":\"" << memory.lowered_digest
        << "\",\"plan_accesses\":" << memory.accesses << ",\"plan_edges\":" << memory.edges
        << ",\"plan_barriers\":" << memory.barriers
        << ",\"pmr_default_resource\":{\"allocations\":" << memory.pmr.allocations
        << ",\"deallocations\":" << memory.pmr.deallocations << ",\"total_bytes\":" << memory.pmr.total_bytes
        << ",\"peak_live_bytes\":" << memory.pmr.peak_bytes << (options.record ? ",\"live_bytes_after_record\":" : options.lower ? ",\"live_bytes_after_lower\":" : ",\"live_bytes_after_compile\":") << memory.pmr.live_bytes
        << "},\"crt\":{\"available\":" << crt_available
        << ",\"allocation_requests\":" << memory.crt.allocation_requests
        << ",\"free_requests\":" << memory.crt.free_requests
        << ",\"reallocation_requests\":" << memory.crt.reallocation_requests
        << ",\"allocation_requested_bytes\":" << memory.crt.allocation_requested_bytes
        << ",\"reallocation_requested_bytes\":" << memory.crt.reallocation_requested_bytes << "}}\n";
}

} // namespace

int main(int argc, char** argv) {
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: TestRenderGraphCompilerBenchmark [--passes 64] [--resources 64] "
                     "[--accesses 16] [--warmup 5] [--samples 80] [--subranges] [--lower | --record | --lifecycle]\n"
                     "--lifecycle includes build, Compile, compiled serial recording and destruction; no allocation probe.\n"
                     "Outputs JSON. Rebuilds the graph for every Compile; excludes build, Dump and graph destruction. --lower or --record measures Lower or compiled serial recording after untimed Compile.\n"
                     "--subranges selects single mip/layer pairs and 512-byte buffer ranges instead of whole resources.\n"
                     "Timing includes phase scratch destruction and has no allocation hooks.\n"
                     "A separate run of the selected phase counts PMR default-resource requests (including library proxies),\n"
                     "and MSVC Debug CRT requests excluding CRT internal blocks. CRT counts include the selected phase output,\n"
                     "exclude allocators bypassing the CRT, and do not measure live/peak heap bytes.\n";
        return 0;
    }
    try {
        const auto options = ParseOptions(argc, argv);
        const PhysicalResources physical(options.resources);
        if (options.lifecycle) {
            MeasureLifecycle(options, physical);
            return 0;
        }
        const auto timing = MeasurePhase(options, physical);
        const auto memory = MeasureAllocations(options, physical, timing.dump_digest);
        PrintResult(options, timing, memory);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TestRenderGraphCompilerBenchmark: " << error.what() << '\n';
        return 1;
    }
}
