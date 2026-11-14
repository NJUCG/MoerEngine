#include "profile/RdgDiagnostics.h"
#include <Windows.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <exception>

namespace Moer::RdgDiagnostics {
namespace {
thread_local Report report;
thread_local Scope* current = nullptr;
thread_local bool enabled = false;
thread_local bool allocating = false;
thread_local Mode mode = Mode::Timing;
uint64_t Now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

void Begin(Mode requested) {
    assert(!enabled && current == nullptr);
    report = {};
    mode = requested;
    enabled = true;
}
Report End() {
    assert(enabled && current == nullptr);
    enabled = false;
    return report;
}
Scope::Scope(const char* name) noexcept : active(enabled) {
    if (!active) { return; }
    parent = current;
    current = this;
    Next(name);
}
void Scope::Finish() noexcept {
    if (!active || begin_ns == 0) { return; }
    const uint64_t elapsed = Now() - begin_ns;
    auto& phase = report.phases[index];
    phase.inclusive_ns += elapsed;
    phase.self_ns += elapsed - child_ns;
    if (parent) { parent->child_ns += elapsed; }
    begin_ns = 0;
}
void Scope::Next(const char* name) noexcept {
    if (!active) { return; }
    Finish();
    index = 0;
    while (index < report.phase_count && std::strcmp(report.phases[index].name, name) != 0) { ++index; }
    if (index >= report.phases.size()) { std::terminate(); }
    if (index == report.phase_count) { report.phases[report.phase_count++].name = name; }
    ++report.phases[index].calls;
    child_ns = 0;
    begin_ns = mode == Mode::Timing ? Now() : 0;
}
Scope::~Scope() {
    if (!active) { return; }
    Finish();
    current = parent;
}
void Count(Counter counter, uint64_t amount) noexcept {
    if (enabled && mode == Mode::Operations) { report.counters[size_t(counter)] += amount; }
}
void Allocation(const void* pointer, size_t bytes, AllocationKind kind) noexcept {
    if (!pointer || !enabled || !current || allocating || mode != Mode::Allocations) { return; }
    allocating = true;
    auto& phase = report.phases[current->index];
    if (kind == AllocationKind::Engine) { ++phase.engine_allocations; phase.engine_bytes += bytes; }
    else if (kind == AllocationKind::Pmr) { ++phase.pmr_allocations; phase.pmr_bytes += bytes; }
    else { ++phase.allocations; phase.bytes += bytes; }
    std::array<void*, 10> stack{};
    CaptureStackBackTrace(1, DWORD(stack.size()), stack.data(), nullptr);
    size_t index = 0;
    for (; index < report.site_count; ++index) {
        const auto& site = report.sites[index];
        if (site.phase == current->index && site.kind == kind && site.stack == stack) { break; }
    }
    if (index < report.sites.size()) {
        auto& site = report.sites[index];
        if (index == report.site_count) {
            site.phase = current->index; site.kind = kind; site.stack = stack; ++report.site_count;
        }
        ++site.calls; site.bytes += bytes;
    } else { ++report.dropped_sites; }
    allocating = false;
}
void Free(const void* pointer) noexcept {
    if (pointer && enabled && current && !allocating && mode == Mode::Allocations) {
        ++report.phases[current->index].frees;
    }
}
}
