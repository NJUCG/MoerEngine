// Included after the fixture helpers; diagnostics are absent from ordinary benchmark builds.
namespace Diag = Moer::RdgDiagnostics;

class DiagnosticPmrResource final : public std::pmr::memory_resource {
    void* do_allocate(size_t bytes, size_t alignment) override {
        void* memory = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        Diag::Allocation(memory, bytes, Diag::AllocationKind::Pmr);
        return memory;
    }
    void do_deallocate(void* memory, size_t bytes, size_t alignment) override {
        Diag::Free(memory);
        std::pmr::new_delete_resource()->deallocate(memory, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};

void VerifyDiagnostics() {
    DiagnosticPmrResource memory;
    ScopedDefaultResource default_resource(memory);
    Diag::Begin(Diag::Mode::Allocations);
    {
        Diag::Scope phase("Coverage");
        void* ordinary = ::operator new(37);
        void* aligned = ::operator new(65, std::align_val_t{64});
        auto* resource = std::pmr::get_default_resource();
        void* pmr = resource->allocate(83, alignof(std::max_align_t));
        void* engine = Memory::Malloc(47);
        ::operator delete(ordinary);
        ::operator delete(aligned, std::align_val_t{64});
        resource->deallocate(pmr, 83, alignof(std::max_align_t));
        Memory::Free(engine);
    }
    const auto report = Diag::End();
    const auto& phase = report.phases[0];
    if (phase.allocations != 2 || phase.bytes != 102 || phase.pmr_allocations != 1 || phase.pmr_bytes != 83 || phase.engine_allocations != 1 ||
        phase.engine_bytes != 47 || phase.frees != 4 || report.dropped_sites != 0) {
        throw std::runtime_error("diagnostic allocation coverage self-test failed");
    }
    Diag::Begin(Diag::Mode::Operations);
    { Diag::Scope phase("Counters"); Diag::Count(Diag::Counter::CellTests, 7); }
    if (Diag::End().counters[size_t(Diag::Counter::CellTests)] != 7) {
        throw std::runtime_error("diagnostic counter self-test failed");
    }
    Diag::Begin(Diag::Mode::Timing);
    { Diag::Scope outer("Outer"); { Diag::Scope inner("Inner"); Sleep(1); } }
    const auto timing = Diag::End();
    if (timing.phases[0].inclusive_ns < timing.phases[1].inclusive_ns ||
        timing.phases[0].self_ns + timing.phases[1].inclusive_ns != timing.phases[0].inclusive_ns) {
        throw std::runtime_error("diagnostic nested timing self-test failed");
    }
    std::cout << "{\"diagnostic_contract\":\"passed\",\"ordinary_allocations\":2,\"pmr_allocations\":1,\"engine_allocations\":1}\n";
}

std::string DiagnosticSymbol(void* address) {
    alignas(SYMBOL_INFO) char bytes[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* info = reinterpret_cast<SYMBOL_INFO*>(bytes);
    info->SizeOfStruct = sizeof(SYMBOL_INFO);
    info->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(address), &displacement, info)) {
        return info->Name;
    }
    return std::to_string(reinterpret_cast<uintptr_t>(address));
}

void MeasureDiagnostics(const Options& options, const PhysicalResources& physical) {
    DiagnosticPmrResource memory;
    ScopedDefaultResource default_resource(memory);
    const auto mode = options.diagnose == "timing" ? Diag::Mode::Timing :
                      options.diagnose == "operations" ? Diag::Mode::Operations : Diag::Mode::Allocations;
    struct PhaseSamples { std::vector<double> inclusive, self; };
    std::map<std::string, PhaseSamples> phase_samples;
    auto last = std::make_unique<Diag::Report>();
    std::optional<std::array<uint64_t, size_t(Diag::Counter::Count)>> previous_counters;
    for (uint64_t iteration = 0; iteration < uint64_t{options.warmup} + options.samples; ++iteration) {
        uint32_t recorded = 0;
        Diag::Begin(mode);
        {
            Diag::Scope lifetime("Lifecycle");
            std::optional<RenderGraph> graph;
            std::optional<Moer::Render::CommandList> commands;
            {
                Diag::Scope scope("Build");
                graph.emplace("DiagnosticBenchmark"); commands.emplace();
                commands->SetResourceStateOwnership(Moer::Render::ERHIResourceStateOwnership::Compiled);
                BuildGraph(*graph, options, physical, &recorded);
            }
            {
                Diag::Scope scope("CompileEntry");
                if (!graph->Compile()) { throw std::runtime_error(graph->GetCompileError()); }
            }
            {
                Diag::Scope scope("RecordEntry");
                if (!graph->RecordAndMergeFrontendCommands(*commands, false)) {
                    throw std::runtime_error(graph->GetCompileError());
                }
            }
            { Diag::Scope scope("DestroyCommands"); commands.reset(); }
            { Diag::Scope scope("DestroyGraph"); graph.reset(); }
        }
        *last = Diag::End();
        if (recorded != options.passes) { throw std::runtime_error("diagnostic pass callback mismatch"); }
        if (iteration < options.warmup) { continue; }
        if (previous_counters && *previous_counters != last->counters) {
            throw std::runtime_error("operation counts vary across identical graphs");
        }
        previous_counters = last->counters;
        for (size_t index = 0; index < last->phase_count; ++index) {
            const auto& phase = last->phases[index];
            auto& samples = phase_samples[phase.name];
            samples.inclusive.push_back(phase.inclusive_ns / 1e6);
            samples.self.push_back(phase.self_ns / 1e6);
        }
    }
    nlohmann::json result{
        {"mode", options.diagnose}, {"passes", options.passes}, {"resources_per_kind", options.resources},
        {"accesses_per_kind_per_pass", options.accesses}, {"subranges", options.subranges},
        {"cpu", options.cpu}, {"warmup", options.warmup}, {"samples", options.samples},
        {"allocation_coverage", "new/delete in Render and benchmark, default PMR upstream and Memory API; excludes Core-internal new, direct C/allocator and other DLL allocations"}
    };
    for (size_t index = 0; index < last->phase_count; ++index) {
        const auto& phase = last->phases[index];
        auto& samples = phase_samples[phase.name];
        std::sort(samples.inclusive.begin(), samples.inclusive.end());
        std::sort(samples.self.begin(), samples.self.end());
        result["phases"].push_back({{"name", phase.name}, {"calls", phase.calls},
            {"inclusive_p50_ms", samples.inclusive[options.samples / 2]},
            {"inclusive_mean_ms", std::accumulate(samples.inclusive.begin(), samples.inclusive.end(), 0.0) / options.samples},
            {"self_p50_ms", samples.self[options.samples / 2]},
            {"self_mean_ms", std::accumulate(samples.self.begin(), samples.self.end(), 0.0) / options.samples},
            {"ordinary_allocations", phase.allocations}, {"ordinary_bytes", phase.bytes},
            {"pmr_allocations", phase.pmr_allocations}, {"pmr_bytes", phase.pmr_bytes},
            {"engine_allocations", phase.engine_allocations}, {"engine_bytes", phase.engine_bytes}, {"frees", phase.frees}});
    }
    for (size_t index = 0; index < last->counters.size(); ++index) {
        result["operations"][Diag::counter_names[index]] = last->counters[index];
    }
    if (mode == Diag::Mode::Allocations) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
        if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE)) { throw std::runtime_error("cannot resolve allocation symbols"); }
        result["dropped_allocation_sites"] = last->dropped_sites;
        for (size_t index = 0; index < last->site_count; ++index) {
            const auto& site = last->sites[index];
            nlohmann::json entry{{"phase", last->phases[site.phase].name},
                                 {"kind", site.kind == Diag::AllocationKind::Engine ? "engine" : site.kind == Diag::AllocationKind::Pmr ? "pmr" : "ordinary"},
                                 {"calls", site.calls}, {"bytes", site.bytes}};
            for (const auto frame : site.stack) { if (frame) { entry["stack"].push_back(DiagnosticSymbol(frame)); } }
            result["allocation_sites"].push_back(std::move(entry));
        }
        SymCleanup(GetCurrentProcess());
    }
    RenderGraph verify("CompilerBenchmark");
    BuildGraph(verify, options, physical);
    RenderGraphLowering::LoweredPlan lowered;
    std::string error;
    if (!verify.Compile() || !RenderGraphLowering::Lower(verify, lowered, error)) { throw std::runtime_error(error); }
    result["dump_fnv1a"] = std::to_string(Digest(verify.Dump()));
    result["lowered_dump_fnv1a"] = std::to_string(Digest(lowered.Dump()));
    const auto& plan = verify.GetCompiledPlan();
    result["work"] = {{"accesses", plan.accesses.size()}, {"edges", plan.edges.size()}, {"barriers", plan.barriers.size()},
                       {"queue_batches", plan.queue_batches.size()}, {"queue_syncs", plan.queue_syncs.size()}};
    size_t reasons = 0, sources = 0;
    for (const auto& edge : plan.edges) { reasons += edge.reasons.size(); }
    for (const auto& barrier : plan.barriers) { sources += barrier.sources.size(); }
    result["work"]["edge_reasons"] = reasons;
    result["work"]["barrier_sources"] = sources;
    std::cout << result.dump(2) << '\n';
}
