#pragma once

// Opt-in benchmark instrumentation. Ordinary builds contain no probes or allocation overrides.
#if MOER_RDG_DIAGNOSTICS
#include "API_Macro.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace Moer::RdgDiagnostics {
enum class Mode { Timing, Operations, Allocations };
enum class AllocationKind { Standard, Engine, Pmr };
enum class Counter : size_t {
    CellTests, MergedAccesses, AddEdgeCalls, ReasonAppends, ReasonGrowths,
    BarrierSources, KeepaliveBindings, InstructionGrowths, BoundaryTests, MapScopeCalls, Count
};
inline constexpr std::array counter_names{
    "cell_tests", "merged_accesses", "add_edge_calls", "reason_appends", "reason_growths",
    "barrier_sources", "keepalive_bindings", "instruction_growths", "boundary_tests", "map_scope_calls"
};
struct Phase {
    const char* name = nullptr;
    uint64_t calls = 0, inclusive_ns = 0, self_ns = 0;
    uint64_t allocations = 0, bytes = 0, frees = 0, engine_allocations = 0, engine_bytes = 0;
    uint64_t pmr_allocations = 0, pmr_bytes = 0;
};
struct AllocationSite {
    std::array<void*, 10> stack{};
    uint64_t calls = 0, bytes = 0;
    size_t phase = 0;
    AllocationKind kind = AllocationKind::Standard;
};
struct Report {
    std::array<Phase, 48> phases{};
    std::array<uint64_t, size_t(Counter::Count)> counters{};
    std::array<AllocationSite, 1024> sites{};
    size_t phase_count = 0, site_count = 0;
    uint64_t dropped_sites = 0;
};
CORE_API void Begin(Mode mode);
CORE_API Report End();
CORE_API void Count(Counter counter, uint64_t amount = 1) noexcept;
CORE_API void Allocation(const void* pointer, size_t bytes, AllocationKind kind = AllocationKind::Standard) noexcept;
CORE_API void Free(const void* pointer) noexcept;

class CORE_API Scope final {
public:
    explicit Scope(const char* name) noexcept;
    ~Scope();
    void Next(const char* name) noexcept;
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    size_t index = 0;
    uint64_t begin_ns = 0, child_ns = 0;
    Scope* parent = nullptr;
private:
    void Finish() noexcept;
    bool active = false;
};
}
#define MOER_RDG_DIAG_JOIN_(a,b) a##b
#define MOER_RDG_DIAG_JOIN(a,b) MOER_RDG_DIAG_JOIN_(a,b)
#define MOER_RDG_PHASE(name) ::Moer::RdgDiagnostics::Scope MOER_RDG_DIAG_JOIN(_rdg_scope_, __LINE__)(name)
#define MOER_RDG_COUNT(name, amount) ::Moer::RdgDiagnostics::Count(::Moer::RdgDiagnostics::Counter::name, amount)
#define MOER_RDG_PHASE_SEQUENCE(name) ::Moer::RdgDiagnostics::Scope _rdg_phase(name)
#define MOER_RDG_PHASE_NEXT(name) _rdg_phase.Next(name)
#define MOER_RDG_ENGINE_ALLOC(pointer, bytes) ::Moer::RdgDiagnostics::Allocation(pointer, bytes, ::Moer::RdgDiagnostics::AllocationKind::Engine)
#define MOER_RDG_FREE(pointer) ::Moer::RdgDiagnostics::Free(pointer)
#else
#define MOER_RDG_PHASE(name)
#define MOER_RDG_COUNT(name, amount)
#define MOER_RDG_PHASE_SEQUENCE(name)
#define MOER_RDG_PHASE_NEXT(name)
#define MOER_RDG_ENGINE_ALLOC(pointer, bytes)
#define MOER_RDG_FREE(pointer)
#endif
