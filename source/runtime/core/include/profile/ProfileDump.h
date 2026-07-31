#ifndef MOER_ENGINE_PROFILE_DUMP_H
#define MOER_ENGINE_PROFILE_DUMP_H

#include "API_Macro.h"
#include "profile/ProfileDumpCodec.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace Moer::ProfileDump {

enum class RuntimeState : std::uint8_t {
    Stopped = 0,
    Starting,
    Running,
    Draining,
    Faulted,
};

enum class StartResult : std::uint8_t {
    Started = 0,
    AlreadyRunning,
    Busy,
    InvalidConfig,
    OutputExists,
    FileOpenFailed,
    ThreadCreateFailed,
    ResourceExhausted,
};

enum class FlushResult : std::uint8_t {
    Completed = 0,
    NothingPending,
    Rejected,
    Faulted,
};

enum class CrashFlushResult : std::uint8_t {
    Completed = 0,
    NotRunning,
    Faulted,
    Busy,
    TimedOut,
};

enum class ShutdownResult : std::uint8_t {
    Completed = 0,
    AlreadyStopped,
    Faulted,
};

enum class SchemaStatus : std::uint8_t {
    Registered = 0,
    AlreadyRegistered,
    NotRunning,
    InvalidSchema,
    SchemaLimit,
    SchemaBytesLimit,
    HashCollision,
};

enum class EmitStatus : std::uint8_t {
    Accepted = 0,
    Disabled,
    InvalidHandle,
    ValueCountMismatch,
    ValueTypeMismatch,
    StringTooLarge,
    RecordTooLarge,
    QueueFull,
    SinkFault,
};

enum class RuntimeFault : std::uint8_t {
    None = 0,
    OpenTempFile,
    WritePacket,
    FlushFile,
    CloseFile,
    RenameFinal,
    WriterException,
};

struct RuntimeConfig {
    std::filesystem::path output_path{};
    CodecLimits           codec_limits{};

    std::size_t max_schemas{1024};
    std::size_t max_schema_bytes{1024 * 1024};
    std::size_t max_record_bytes{64 * 1024};

    std::size_t tls_publish_records{64};
    std::size_t tls_publish_bytes{16 * 1024};
    std::size_t tls_max_records{256};
    std::size_t tls_max_bytes{256 * 1024};

    std::size_t queue_max_chunks{128};
    std::size_t queue_max_records{65536};
    std::size_t queue_max_bytes{64 * 1024 * 1024};

    bool replace_existing{false};
};

struct SchemaHandle {
    std::uint64_t hash{0};
    std::uint64_t generation{0};

    [[nodiscard]] explicit operator bool() const noexcept {
        return hash != 0 && generation != 0;
    }

    friend bool operator==(const SchemaHandle&, const SchemaHandle&) = default;
};

struct SchemaRegistration {
    SchemaStatus status{SchemaStatus::NotRunning};
    SchemaHandle handle{};
};

struct RuntimeStats {
    RuntimeState  state{RuntimeState::Stopped};
    RuntimeFault  last_fault{RuntimeFault::None};
    std::uint64_t generation{0};

    std::uint64_t records_committed{0};
    std::uint64_t records_enqueued{0};
    std::uint64_t records_written{0};
    std::uint64_t records_dropped_stopped{0};
    std::uint64_t records_dropped_stale_generation{0};
    std::uint64_t records_dropped_oversized{0};
    std::uint64_t records_dropped_queue_full{0};
    std::uint64_t records_dropped_after_fault{0};

    std::uint64_t chunks_enqueued{0};
    std::uint64_t chunks_written{0};
    std::uint64_t chunks_dropped{0};

    std::uint64_t resident_chunks{0};
    std::uint64_t resident_records{0};
    std::uint64_t resident_bytes{0};
    std::uint64_t high_water_chunks{0};
    std::uint64_t high_water_records{0};
    std::uint64_t high_water_bytes{0};

    std::uint64_t file_bytes_written{0};
    std::uint64_t flush_completed{0};
    std::uint64_t flush_failed{0};
    std::uint64_t io_faults{0};
};

[[nodiscard]] CORE_API StartResult Start(const RuntimeConfig& _config) noexcept;

[[nodiscard]] CORE_API SchemaRegistration RegisterSchema(const SchemaDescriptor& _schema) noexcept;

[[nodiscard]] CORE_API EmitStatus
Emit(SchemaHandle _schema, std::span<const FieldValueView> _values) noexcept;

// Producer-thread operation. Publishes only the calling thread's TLS shard,
// then waits until the already-published writer interval has been flushed to
// the in-progress stream. This is not an fsync/power-loss durability
// guarantee.
[[nodiscard]] CORE_API FlushResult FlushThreadLocal() noexcept;

// Process-owner synchronization fence. Harvests every currently registered
// live TLS shard, then flushes that published writer interval. The harvest is
// a per-shard cut rather than one global instantaneous snapshot: an Emit that
// commits after its shard was visited can remain for the next interval.
[[nodiscard]] CORE_API FlushResult FlushAll() noexcept;

// Controlled-fatal boundary. This does not harvest TLS shards, allocate a
// fence, finalize the stream, or rename the in-progress file. It asks the
// existing writer to fflush only the prefix it has already written and waits
// no longer than _timeout_ms. The method is therefore best-effort and never
// promises a process-wide instantaneous snapshot.
[[nodiscard]] CORE_API CrashFlushResult
FlushCrashPublishedPrefix(std::uint32_t _timeout_ms) noexcept;

// Process-owner finalization. Closes admission, waits for already-accepted
// Emit calls, harvests all registered live TLS shards, drains the writer, and
// publishes .inprogress as the final file. Producers need not exit or flush
// individually, but must not begin a new capture Emit after Shutdown starts.
[[nodiscard]] CORE_API ShutdownResult Shutdown() noexcept;

[[nodiscard]] CORE_API RuntimeState GetRuntimeState() noexcept;
[[nodiscard]] CORE_API std::uint64_t GetRuntimeGeneration() noexcept;
[[nodiscard]] CORE_API RuntimeStats GetRuntimeStats() noexcept;

} // namespace Moer::ProfileDump

#endif // MOER_ENGINE_PROFILE_DUMP_H
