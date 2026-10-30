#include "profile/ProfileScope.h"
#include "misc/Types.h"
#include "platform/Platform.h"
#include "profile/ProfileDumpTemplates.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>

namespace Moer::ProfileDump {

namespace {

std::atomic<uint64>        g_active_schema_hash{0};
std::atomic<uint64>        g_active_generation{0};
std::mutex                 g_lifecycle_mutex{};

struct ThreadScopeState {
    uint64 generation{0};
    uint32 depth{0};
};

thread_local ThreadScopeState g_thread_scope_state{};

[[nodiscard]] uint64 CurrentSteadyNanoseconds() noexcept {
    const auto count =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        )
            .count();
    return count > 0 ? static_cast<uint64>(count) : 0;
}

void DisableIfCurrent(uint64 _generation) noexcept {
    try {
        std::lock_guard lock(g_lifecycle_mutex);
        if (g_active_generation.load(std::memory_order_acquire) == _generation) {
            g_active_generation.store(0, std::memory_order_release);
            g_active_schema_hash.store(0, std::memory_order_relaxed);
        }
    } catch (...) {
        // A producer failure must never escape a profiling scope destructor.
        // The generation is the authoritative active flag, so clearing it is
        // sufficient even if taking the cold-path lifecycle lock failed.
        uint64 expected = _generation;
        static_cast<void>(g_active_generation.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel, std::memory_order_acquire
        ));
    }
}

} // namespace

namespace CpuScopeProducer {

CpuScopeActivationResult Activate(SchemaHandle _schema) noexcept {
    try {
        if (!_schema) {
            return CpuScopeActivationResult::InvalidHandle;
        }

        if (GetRuntimeState() != RuntimeState::Running) {
            return CpuScopeActivationResult::RuntimeNotRunning;
        }

        if (_schema.generation != GetRuntimeGeneration()) {
            return CpuScopeActivationResult::StaleGeneration;
        }

        const uint64 expected_hash = ComputeSchemaHash(Templates::CpuScope());
        if (_schema.hash != expected_hash) {
            return CpuScopeActivationResult::WrongSchema;
        }

        std::lock_guard lock(g_lifecycle_mutex);
        // Revalidate after serializing producer lifecycle publication. The
        // ProfileDump runtime owns a separate lifecycle lock and can have
        // stopped or restarted between the checks above and this cold path.
        if (GetRuntimeState() != RuntimeState::Running) {
            return CpuScopeActivationResult::RuntimeNotRunning;
        }
        if (_schema.generation != GetRuntimeGeneration()) {
            return CpuScopeActivationResult::StaleGeneration;
        }

        const uint64 active_generation =
            g_active_generation.load(std::memory_order_acquire);
        const uint64 active_hash =
            g_active_schema_hash.load(std::memory_order_relaxed);
        if (active_generation == _schema.generation && active_hash == _schema.hash) {
            return CpuScopeActivationResult::AlreadyActive;
        }

        // A runtime owner is expected to deactivate before shutdown, but
        // replacing a publication from an older generation keeps the public
        // producer API recoverable if that cleanup was missed.
        g_active_schema_hash.store(_schema.hash, std::memory_order_relaxed);
        g_active_generation.store(_schema.generation, std::memory_order_release);
        return CpuScopeActivationResult::Activated;
    } catch (...) {
        return CpuScopeActivationResult::RuntimeNotRunning;
    }
}

void Deactivate() noexcept {
    try {
        std::lock_guard lock(g_lifecycle_mutex);
        g_active_generation.store(0, std::memory_order_release);
        g_active_schema_hash.store(0, std::memory_order_relaxed);
    } catch (...) {
        g_active_generation.store(0, std::memory_order_release);
    }
}

bool IsActive() noexcept {
    const uint64        generation = g_active_generation.load(std::memory_order_acquire);
    const RuntimeState  state      = GetRuntimeState();
    if (generation == 0 ||
        (state != RuntimeState::Running && state != RuntimeState::Faulted)) {
        return false;
    }
    return GetRuntimeGeneration() == generation &&
           g_active_generation.load(std::memory_order_acquire) == generation;
}

} // namespace CpuScopeProducer

ScopedCpuProfile::ScopedCpuProfile(std::string_view _name) noexcept {
    Begin({}, _name, false);
}

ScopedCpuProfile::ScopedCpuProfile(std::string_view _prefix, std::string_view _name) noexcept {
    Begin(_prefix, _name, true);
}

void ScopedCpuProfile::Begin(
    std::string_view _prefix, std::string_view _name, bool _join
) noexcept {
    const uint64 generation = g_active_generation.load(std::memory_order_acquire);
    if (generation == 0) {
        return;
    }
    const uint64 schema_hash = g_active_schema_hash.load(std::memory_order_relaxed);
    if (schema_hash == 0) {
        return;
    }

    std::size_t joined_bytes = _name.size();
    if (_name.empty()) {
        return;
    }
    if (_join) {
        if (_prefix.empty() || _prefix.size() >= kMaxNameBytes ||
            _name.size() > kMaxNameBytes - _prefix.size() - 1) {
            return;
        }
        joined_bytes = _prefix.size() + 1 + _name.size();
    } else if (_name.size() > kMaxNameBytes) {
        return;
    }

    if (g_thread_scope_state.generation != generation) {
        g_thread_scope_state.generation = generation;
        g_thread_scope_state.depth      = 0;
    }
    if (g_thread_scope_state.depth == std::numeric_limits<uint32>::max()) {
        return;
    }

    if (_join) {
        std::memcpy(name_.data(), _prefix.data(), _prefix.size());
        name_[_prefix.size()] = '.';
        std::memcpy(name_.data() + _prefix.size() + 1, _name.data(), _name.size());
    } else {
        std::memcpy(name_.data(), _name.data(), _name.size());
    }

    schema_      = {.hash = schema_hash, .generation = generation};
    thread_id_   = static_cast<uint64>(Platform::GetCurrentThreadID());
    begin_ns_    = CurrentSteadyNanoseconds();
    depth_       = g_thread_scope_state.depth;
    name_bytes_  = static_cast<uint16>(joined_bytes);
    ++g_thread_scope_state.depth;
    active_ = true;
}

ScopedCpuProfile::~ScopedCpuProfile() noexcept {
    if (!active_) {
        return;
    }
    active_ = false;

    if (g_thread_scope_state.generation == schema_.generation && g_thread_scope_state.depth != 0) {
        --g_thread_scope_state.depth;
    }

    // A scope may outlive the session in which it began. Do not route that
    // stale handle through a restarted runtime: even a rejected Emit would
    // charge the new session's dropped-record diagnostics.
    if (g_active_generation.load(std::memory_order_acquire) != schema_.generation ||
        GetRuntimeGeneration() != schema_.generation) {
        return;
    }

    try {
        const uint64                        end_ns = std::max(CurrentSteadyNanoseconds(), begin_ns_);
        const std::array<FieldValueView, 5> values = {
            thread_id_,
            std::string_view(name_.data(), name_bytes_),
            begin_ns_,
            end_ns,
            depth_,
        };
        const EmitStatus status = Emit(schema_, values);
        if (status == EmitStatus::Disabled || status == EmitStatus::InvalidHandle ||
            status == EmitStatus::SinkFault) {
            DisableIfCurrent(schema_.generation);
        }
    } catch (...) {
        DisableIfCurrent(schema_.generation);
    }
}

} // namespace Moer::ProfileDump
