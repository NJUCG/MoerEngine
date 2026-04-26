#include "profile/ProfileDump.h"

#include "config/CVarSystem.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "network/Socket.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>

namespace Moer::ProfileDump {

namespace {

struct PendingRecord {
    uint32_t      schema_id = 0;
    uint64_t      sequence = 0;
    Array<uint8_t> payload;
};

struct RegisteredSchema {
    SchemaRuntimeDesc desc{};
    bool              emitted_to_file = false;
    bool              emitted_to_tcp = false;
};

struct HubState {
    std::mutex             mutex;
    DEQueue<RegisteredSchema> schemas;
    Array<PendingRecord>   published_records;
    std::atomic<uint64_t>  next_sequence{1};
    uint32_t               next_schema_id = 1;
    bool                   runtime_config_initialized = false;
    RuntimeConfig          runtime_config{};
    bool                   has_testing_override = false;
    RuntimeConfig          testing_override{};
    std::ofstream          file_stream;
    std::string            active_file_path;

    std::string            active_tcp_host;
    int                    active_tcp_port = 0;
    Network::TcpSocket     tcp_socket{};
};

HubState& GetState() {
    static HubState state;
    return state;
}

struct ThreadLocalShard {
    Array<PendingRecord> records;
    size_t               total_bytes = 0;

    ~ThreadLocalShard() {
        if (records.empty()) {
            return;
        }

        HubState& state = GetState();
        std::lock_guard lock(state.mutex);
        for (PendingRecord& record : records) {
            state.published_records.emplace_back(std::move(record));
        }
        records.clear();
        total_bytes = 0;
    }
};

thread_local ThreadLocalShard g_tls_shard{};

bool ClampPositive(int& value, int fallback) {
    if (value > 0) {
        return false;
    }
    value = fallback;
    return true;
}

RuntimeConfig BuildConfigFromCVars() {
    static bool        file_enabled = true;
    static bool        tcp_enabled = false;
    static int         tls_max_records = 256;
    static int         tls_max_bytes = 32768;
    static int         auto_publish_records = 64;
    static int         auto_publish_bytes = 8192;
    static std::string file_path;
    static std::string tcp_host = "127.0.0.1";
    static int         tcp_port = 19090;

    static CVar::TCVar<bool> cvar_file_enabled(
        "Profile.Dump.FileEnabled",
        file_enabled,
        "Enable binary profile dump file sink.",
        "Binary profile dump file sink enabled.",
        "Binary profile dump file sink disabled.",
        CVar::EFlags::StartupConfigReadOnly
    );
    static CVar::TCVar<bool> cvar_tcp_enabled(
        "Profile.Dump.TcpEnabled",
        tcp_enabled,
        "Enable binary profile dump TCP sink.",
        "Binary profile dump TCP sink enabled.",
        "Binary profile dump TCP sink disabled.",
        CVar::EFlags::StartupConfigReadOnly
    );
    static CVar::TCVar<int> cvar_tls_max_records(
        "Profile.Dump.TlsMaxRecords",
        tls_max_records,
        "Maximum record count kept inside one TLS shard before forced publish.",
        "TLS shard record cap set.",
        "TLS shard record cap set.",
        CVar::EFlags::StartupConfigReadOnly
    );
    static CVar::TCVar<int> cvar_tls_max_bytes(
        "Profile.Dump.TlsMaxBytes",
        tls_max_bytes,
        "Maximum byte count kept inside one TLS shard before forced publish.",
        "TLS shard byte cap set.",
        "TLS shard byte cap set.",
        CVar::EFlags::StartupConfigReadOnly
    );
    static CVar::TCVar<int> cvar_auto_publish_records(
        "Profile.Dump.AutoPublishRecords",
        auto_publish_records,
        "Publish TLS shard when record count reaches this value.",
        "TLS auto publish record threshold set.",
        "TLS auto publish record threshold set.",
        CVar::EFlags::StartupConfigReadOnly
    );
    static CVar::TCVar<int> cvar_auto_publish_bytes(
        "Profile.Dump.AutoPublishBytes",
        auto_publish_bytes,
        "Publish TLS shard when byte count reaches this value.",
        "TLS auto publish byte threshold set.",
        "TLS auto publish byte threshold set.",
        CVar::EFlags::StartupConfigReadOnly
    );
    static CVar::TCVar<std::string> cvar_file_path(
        "Profile.Dump.FilePath",
        file_path,
        "Optional output file path for binary profile dumps.",
        "Binary profile dump file path set.",
        "Binary profile dump file path set.",
        CVar::EFlags::StartupConfigReadOnly
    );
    static CVar::TCVar<std::string> cvar_tcp_host(
        "Profile.Dump.TcpHost",
        tcp_host,
        "Target host for binary profile dump TCP sink.",
        "Binary profile dump TCP host set.",
        "Binary profile dump TCP host set.",
        CVar::EFlags::StartupConfigReadOnly
    );
    static CVar::TCVar<int> cvar_tcp_port(
        "Profile.Dump.TcpPort",
        tcp_port,
        "Target port for binary profile dump TCP sink.",
        "Binary profile dump TCP port set.",
        "Binary profile dump TCP port set.",
        CVar::EFlags::StartupConfigReadOnly
    );

    RuntimeConfig config{};
    config.enable_file = file_enabled;
    config.enable_tcp = tcp_enabled;
    config.tls_max_records = tls_max_records;
    config.tls_max_bytes = tls_max_bytes;
    config.auto_publish_records = auto_publish_records;
    config.auto_publish_bytes = auto_publish_bytes;
    config.file_path = file_path;
    config.tcp_host = tcp_host;
    config.tcp_port = tcp_port;

    if (ClampPositive(config.tls_max_records, 256)) {
        LOG_WARNING(MOER_TEXT("Profile.Dump.TlsMaxRecords must be positive. Fallback to 256."));
    }
    if (ClampPositive(config.tls_max_bytes, 32768)) {
        LOG_WARNING(MOER_TEXT("Profile.Dump.TlsMaxBytes must be positive. Fallback to 32768."));
    }
    if (ClampPositive(config.auto_publish_records, 64)) {
        LOG_WARNING(MOER_TEXT("Profile.Dump.AutoPublishRecords must be positive. Fallback to 64."));
    }
    if (ClampPositive(config.auto_publish_bytes, 8192)) {
        LOG_WARNING(MOER_TEXT("Profile.Dump.AutoPublishBytes must be positive. Fallback to 8192."));
    }
    if (ClampPositive(config.tcp_port, 19090)) {
        LOG_WARNING(MOER_TEXT("Profile.Dump.TcpPort must be positive. Fallback to 19090."));
    }

    if (config.file_path.empty()) {
        std::filesystem::path output_root = ConfigManager::GetInstance().GetWorkspacePath();
        if (output_root.empty()) {
            output_root = std::filesystem::current_path();
        }
        config.file_path = (output_root / "logs" / "profile_dump_stream.mpd").generic_string();
    }
    if (config.tcp_host.empty()) {
        config.tcp_host = "127.0.0.1";
    }

    return config;
}

RuntimeConfig GetConfigLocked(HubState& state) {
    if (!state.runtime_config_initialized) {
        state.runtime_config = state.has_testing_override ? state.testing_override : BuildConfigFromCVars();
        state.runtime_config_initialized = true;
    }
    return state.runtime_config;
}

void ResetFileSinkLocked(HubState& state) {
    if (state.file_stream.is_open()) {
        state.file_stream.flush();
        state.file_stream.close();
    }
    state.active_file_path.clear();
    for (RegisteredSchema& schema : state.schemas) {
        schema.emitted_to_file = false;
    }
}

void ResetTcpSinkLocked(HubState& state) {
    state.tcp_socket.Close();
    state.active_tcp_host.clear();
    state.active_tcp_port = 0;
    for (RegisteredSchema& schema : state.schemas) {
        schema.emitted_to_tcp = false;
    }
}

template<typename Writer>
void WriteStringPayload(Writer&& writer, std::string_view text) {
    const uint32_t length = static_cast<uint32_t>(text.size());
    writer(reinterpret_cast<const uint8_t*>(&length), sizeof(length));
    if (!text.empty()) {
        writer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }
}

template<typename Writer>
void WritePacket(Writer&& writer, EPacketType type, const Array<uint8_t>& payload) {
    const PacketHeader header{
        .magic = packet_magic,
        .version = packet_version,
        .type = type,
        .payload_size = static_cast<uint32_t>(payload.size()),
    };
    writer(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
    if (!payload.empty()) {
        writer(payload.data(), payload.size());
    }
}

Array<uint8_t> BuildSchemaPacketPayload(const RegisteredSchema& schema) {
    Array<uint8_t> payload;
    payload.reserve(256);

    Detail::AppendScalar(payload, schema.desc.schema_id);
    const uint8_t kind = static_cast<uint8_t>(schema.desc.kind);
    const uint8_t channel = static_cast<uint8_t>(schema.desc.channel);
    Detail::AppendScalar(payload, kind);
    Detail::AppendScalar(payload, channel);
    Detail::AppendScalar(payload, schema.desc.version);

    const uint32_t field_count = static_cast<uint32_t>(schema.desc.fields.size());
    Detail::AppendScalar(payload, field_count);

    auto append_string = [&](std::string_view text) {
        const uint32_t length = static_cast<uint32_t>(text.size());
        Detail::AppendScalar(payload, length);
        payload.insert(payload.end(), text.begin(), text.end());
    };

    append_string(schema.desc.name);
    append_string(schema.desc.event_type);
    for (const SchemaFieldDesc& field : schema.desc.fields) {
        const uint8_t field_type = static_cast<uint8_t>(field.type);
        Detail::AppendScalar(payload, field_type);
        append_string(field.name);
    }

    return payload;
}

Array<uint8_t> BuildRecordPacketPayload(const PendingRecord& record) {
    Array<uint8_t> payload;
    payload.reserve(record.payload.size() + 32);
    Detail::AppendScalar(payload, record.schema_id);
    Detail::AppendScalar(payload, record.sequence);
    const uint32_t value_bytes = static_cast<uint32_t>(record.payload.size());
    Detail::AppendScalar(payload, value_bytes);
    payload.insert(payload.end(), record.payload.begin(), record.payload.end());
    return payload;
}

bool EnsureFileSinkLocked(HubState& state, const RuntimeConfig& config) {
    if (!config.enable_file) {
        ResetFileSinkLocked(state);
        return false;
    }

    if (state.file_stream.is_open() && state.active_file_path == config.file_path) {
        return true;
    }

    ResetFileSinkLocked(state);
    std::filesystem::path path(config.file_path);
    std::filesystem::create_directories(path.parent_path());

    state.file_stream.open(path, std::ios::binary | std::ios::trunc);
    if (!state.file_stream.is_open()) {
        LOG_WARNING(MOER_TEXT("ProfileDump failed to open file sink `{}`."), config.file_path);
        return false;
    }

    state.active_file_path = config.file_path;
    return true;
}

bool EnsureTcpSinkLocked(HubState& state, const RuntimeConfig& config) {
    if (!config.enable_tcp) {
        ResetTcpSinkLocked(state);
        return false;
    }
    if (state.tcp_socket.IsOpen() &&
        state.active_tcp_host == config.tcp_host &&
        state.active_tcp_port == config.tcp_port) {
        return true;
    }

    ResetTcpSinkLocked(state);
    if (state.tcp_socket.Connect(Utf8StringView(config.tcp_host.data(), config.tcp_host.size()), static_cast<uint16_t>(config.tcp_port)) !=
        Network::ESocketStatus::Success) {
        LOG_WARNING(MOER_TEXT("ProfileDump failed to connect TCP sink {}:{}."), config.tcp_host, config.tcp_port);
        ResetTcpSinkLocked(state);
        return false;
    }

    state.active_tcp_host = config.tcp_host;
    state.active_tcp_port = config.tcp_port;
    return true;
}

void PublishThreadLocalShard(bool flush_after_publish) {
    HubState& state = GetState();
    if (!g_tls_shard.records.empty()) {
        std::lock_guard lock(state.mutex);
        for (PendingRecord& record : g_tls_shard.records) {
            state.published_records.emplace_back(std::move(record));
        }
        g_tls_shard.records.clear();
        g_tls_shard.total_bytes = 0;
    }

    if (flush_after_publish) {
        FlushAll();
    }
}

void EmitPendingToSinksLocked(HubState& state) {
    RuntimeConfig config = GetConfigLocked(state);

    if (state.published_records.empty()) {
        return;
    }

    std::sort(state.published_records.begin(), state.published_records.end(), [](const PendingRecord& lhs, const PendingRecord& rhs) {
        return lhs.sequence < rhs.sequence;
    });

    const bool can_write_file = EnsureFileSinkLocked(state, config);
    const bool can_write_tcp = EnsureTcpSinkLocked(state, config);

    if (!can_write_file && !can_write_tcp) {
        state.published_records.clear();
        return;
    }

    auto is_schema_referenced = [&](uint32_t schema_id) {
        return std::any_of(state.published_records.begin(), state.published_records.end(), [&](const PendingRecord& record) {
            return record.schema_id == schema_id;
        });
    };

    auto file_writer = [&](const uint8_t* data, size_t size) {
        state.file_stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    };

    auto tcp_writer = [&](const uint8_t* data, size_t size) {
        return state.tcp_socket.SendAll(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size)) ==
               Network::ESocketStatus::Success;
    };

    for (RegisteredSchema& schema : state.schemas) {
        if (!is_schema_referenced(schema.desc.schema_id)) {
            continue;
        }
        if (can_write_file && !schema.emitted_to_file) {
            const Array<uint8_t> payload = BuildSchemaPacketPayload(schema);
            WritePacket(file_writer, EPacketType::Schema, payload);
            schema.emitted_to_file = true;
        }
        if (can_write_tcp && !schema.emitted_to_tcp) {
            const Array<uint8_t> payload = BuildSchemaPacketPayload(schema);
            PacketHeader header{
                .magic = packet_magic,
                .version = packet_version,
                .type = EPacketType::Schema,
                .payload_size = static_cast<uint32_t>(payload.size()),
            };
            if (!tcp_writer(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) ||
                !tcp_writer(payload.data(), payload.size())) {
                LOG_WARNING(MOER_TEXT("ProfileDump lost TCP connection while sending schema packet."));
                ResetTcpSinkLocked(state);
                break;
            }
            schema.emitted_to_tcp = true;
        }
    }

    for (const PendingRecord& record : state.published_records) {
        const Array<uint8_t> payload = BuildRecordPacketPayload(record);
        if (can_write_file) {
            WritePacket(file_writer, EPacketType::Record, payload);
        }
        if (can_write_tcp) {
            PacketHeader header{
                .magic = packet_magic,
                .version = packet_version,
                .type = EPacketType::Record,
                .payload_size = static_cast<uint32_t>(payload.size()),
            };
            if (!tcp_writer(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) ||
                !tcp_writer(payload.data(), payload.size())) {
                LOG_WARNING(MOER_TEXT("ProfileDump lost TCP connection while sending record packet."));
                ResetTcpSinkLocked(state);
                break;
            }
        }
    }

    if (state.file_stream.is_open()) {
        state.file_stream.flush();
    }
    state.published_records.clear();
}

} // namespace

namespace Detail {

uint64_t ReserveSequence() {
    return GetState().next_sequence.fetch_add(1, std::memory_order_relaxed);
}

const SchemaRuntimeDesc& RegisterSchemaRuntime(
    std::string_view                 name,
    std::string_view                 event_type,
    EKind                            kind,
    EChannel                         channel,
    uint32_t                         version,
    std::span<const SchemaFieldDesc> fields
) {
    HubState& state = GetState();
    std::lock_guard lock(state.mutex);

    RegisteredSchema schema{};
    schema.desc.schema_id = state.next_schema_id++;
    schema.desc.name = name;
    schema.desc.event_type = event_type;
    schema.desc.kind = kind;
    schema.desc.channel = channel;
    schema.desc.version = version;
    schema.desc.fields = fields;
    state.schemas.emplace_back(schema);
    return state.schemas.back().desc;
}

void CommitSerializedRecord(uint32_t schema_id, uint64_t sequence, Array<uint8_t>&& payload) {
    HubState& state = GetState();
    const RuntimeConfig config = [&]() {
        std::lock_guard lock(state.mutex);
        return GetConfigLocked(state);
    }();

    g_tls_shard.total_bytes += payload.size();
    g_tls_shard.records.emplace_back(PendingRecord{
        .schema_id = schema_id,
        .sequence = sequence,
        .payload = std::move(payload),
    });

    const bool over_record_cap = static_cast<int>(g_tls_shard.records.size()) >= config.auto_publish_records;
    const bool over_byte_cap = static_cast<int>(g_tls_shard.total_bytes) >= config.auto_publish_bytes;
    const bool over_tls_record_cap = static_cast<int>(g_tls_shard.records.size()) >= config.tls_max_records;
    const bool over_tls_byte_cap = static_cast<int>(g_tls_shard.total_bytes) >= config.tls_max_bytes;
    if (over_record_cap || over_byte_cap || over_tls_record_cap || over_tls_byte_cap) {
        PublishThreadLocalShard(false);
    }
}

} // namespace Detail

void FlushThreadLocal() {
    PublishThreadLocalShard(false);
    FlushAll();
}

void FlushAll() {
    HubState& state = GetState();
    std::lock_guard lock(state.mutex);
    EmitPendingToSinksLocked(state);
}

void Shutdown() {
    FlushThreadLocal();
    HubState& state = GetState();
    std::lock_guard lock(state.mutex);
    ResetFileSinkLocked(state);
    ResetTcpSinkLocked(state);
}

void OverrideOutputFileForCurrentSession(const std::filesystem::path& path) {
    HubState& state = GetState();
    std::lock_guard lock(state.mutex);
    RuntimeConfig config = GetConfigLocked(state);
    config.enable_file = true;
    config.file_path = path.generic_string();
    state.runtime_config = std::move(config);
    state.runtime_config_initialized = true;
    ResetFileSinkLocked(state);
}

void OverrideConfigForTesting(const RuntimeConfig& config) {
    HubState& state = GetState();
    std::lock_guard lock(state.mutex);
    state.testing_override = config;
    state.has_testing_override = true;
    state.runtime_config = config;
    state.runtime_config_initialized = true;
    ResetFileSinkLocked(state);
    ResetTcpSinkLocked(state);
}

void ClearTestingConfigOverride() {
    HubState& state = GetState();
    std::lock_guard lock(state.mutex);
    state.has_testing_override = false;
    state.runtime_config = RuntimeConfig{};
    state.runtime_config_initialized = false;
    ResetFileSinkLocked(state);
    ResetTcpSinkLocked(state);
}

RuntimeConfig GetRuntimeConfigSnapshot() {
    HubState& state = GetState();
    std::lock_guard lock(state.mutex);
    return GetConfigLocked(state);
}

} // namespace Moer::ProfileDump