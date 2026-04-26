#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include "Core.h"
#include "config/CVarSystem.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "ProfileSession.h"
#include "profile/ProfileDump.h"
#include "profile/ProfileDumpTemplates.h"
#include "render/rhi/RHIImpl.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/GPUEventStream.h"
#include "rhi/vulkan/VulkanDescriptor.h"
#include "rhi/vulkan/VulkanDevice.h"
#include "rhi/vulkan/VulkanSwapChain.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/TaskSystem.h"
#include "window/WindowContext.h"

#if defined(MOER_TEST_WITH_PROFILE)
#include "profile.h"
#endif

namespace {

using namespace Moer;
using namespace Moer::Render;

struct BindlessReadbackArgs {
    uint32_t src_handle;
    uint32_t xor_mask;
    uint32_t element_count;
};

struct BindlessTextureReadbackArgs {
    uint32_t handle0;
    uint32_t handle1;
    uint32_t output_offset;
    uint32_t sample_count;
    float    uv0_x;
    float    uv0_y;
    float    uv1_x;
    float    uv1_y;
    float    mip0;
    float    mip1;
};

class BindlessBufferReadbackPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(BindlessBufferReadbackPipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(BindlessReadbackArgs, args);
    DEFINE_SHADER_BUFFER(output_buffer);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(args, output_buffer, bdls);
};

class BindlessTextureReadbackPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(BindlessTextureReadbackPipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(BindlessTextureReadbackArgs, args);
    DEFINE_SHADER_BUFFER(output_buffer);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(args, output_buffer, bdls);
};

void ShutdownRHIForTest() {
    RHIExecutor::ShutDown();
    RenderDevice::Dispose();
}

constexpr uint32_t kElementCount      = 256;
constexpr uint32_t kIterations        = 64;
constexpr uint32_t kPresentIterations = 8;

GPUEvent MakeResolvedGpuEvent(
    GPUEvent::EType type,
    std::string_view name,
    uint32 depth,
    uint64 timestamp_ns
) {
    return GPUEvent{
        .type = type,
        .name = std::string(name),
        .query = {},
        .depth = depth,
        .timestamp_ns = timestamp_ns,
    };
}
constexpr uint32_t kCopyScopeIterations = 8;

BEGIN_INSTANT_EVENT_TEMPLATE(
    ProfileDumpRuntimeTemplate,
    "test.runtime_instant",
    Moer::ProfileDump::EChannel::CPUThread,
    1
)
    PROFILE_DUMP_FIELD(uint64_t, record_id)
    PROFILE_DUMP_FIELD(std::string_view, label)
    PROFILE_DUMP_FIELD(uint32_t, value)
END_INSTANT_EVENT_TEMPLATE()

static_assert(std::is_same_v<
    decltype(DUMP_STREAM(ProfileDumpRuntimeTemplate) << uint64_t{1} << "runtime" << uint32_t{7}),
    Moer::ProfileDump::StreamCommitToken>);

template<typename Fn>
int RunNamedTestCase(const char* name, Fn&& fn) {
    LOG_INFO(MOER_TEXT("[TESTCASE][BEGIN] {}"), name);
    const int ret = fn();
    if (ret == 0) {
        LOG_INFO(MOER_TEXT("[TESTCASE][PASS] {}"), name);
    } else {
        LOG_ERROR(MOER_TEXT("[TESTCASE][FAIL] {} :: exit={}"), name, ret);
    }
    return ret;
}

struct IndicePair {
    uint32_t src;
    uint32_t dst;
};

template<typename T>
std::span<Moer::byte> ToByteSpan(std::vector<T>& values) {
    return std::span<Moer::byte>(reinterpret_cast<Moer::byte*>(values.data()), values.size() * sizeof(T));
}

std::vector<uint8_t> MakeSolidRgba8(uint32_t width, uint32_t height, uint8_t red) {
    std::vector<uint8_t> bytes(size_t(width) * size_t(height) * 4u, 0u);
    for (size_t i = 0; i < size_t(width) * size_t(height); ++i) {
        bytes[i * 4u + 0u] = red;
        bytes[i * 4u + 1u] = 0u;
        bytes[i * 4u + 2u] = 0u;
        bytes[i * 4u + 3u] = 255u;
    }
    return bytes;
}

std::filesystem::path MakeProfileDumpTestPath(std::string_view file_name) {
    std::filesystem::path path = Moer::ConfigManager::GetInstance().GetWorkspacePath() / "logs" / std::string(file_name);
    std::filesystem::create_directories(path.parent_path());
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

struct ParsedProfileDumpCapture {
    Array<Moer::ProfileDump::DecodedSchema> schemas;
    Array<Moer::ProfileDump::DecodedRecord> records;
    Array<Moer::ProfileDump::EPacketType>   packet_order;
};

const Moer::ProfileDump::DecodedSchema*
FindProfileDumpSchemaByEventType(const ParsedProfileDumpCapture& capture, std::string_view event_type) {
    for (const Moer::ProfileDump::DecodedSchema& schema : capture.schemas) {
        if (schema.event_type == event_type) {
            return &schema;
        }
    }
    return nullptr;
}

bool ParseProfileDumpCapture(std::span<const uint8_t> bytes, ParsedProfileDumpCapture& capture) {
    capture.schemas.clear();
    capture.records.clear();
    capture.packet_order.clear();

    size_t offset = 0;
    while (offset < bytes.size()) {
        Moer::ProfileDump::PacketHeader   header{};
        std::span<const uint8_t>          payload{};
        if (!Moer::ProfileDump::ReadNextPacket(bytes, offset, header, payload)) {
            return false;
        }
        capture.packet_order.emplace_back(header.type);

        if (header.type == Moer::ProfileDump::EPacketType::Schema) {
            Moer::ProfileDump::DecodedSchema schema{};
            if (!Moer::ProfileDump::DeserializeSchemaPacket(header, payload, schema)) {
                return false;
            }
            if (std::any_of(capture.schemas.begin(), capture.schemas.end(), [&](const Moer::ProfileDump::DecodedSchema& existing) {
                    return existing.schema_id == schema.schema_id;
                })) {
                return false;
            }
            capture.schemas.emplace_back(std::move(schema));
            continue;
        }

        if (header.type == Moer::ProfileDump::EPacketType::Record) {
            if (payload.size() < sizeof(uint32_t)) {
                return false;
            }

            uint32_t schema_id = 0;
            std::memcpy(&schema_id, payload.data(), sizeof(schema_id));
            const auto schema_iter = std::find_if(
                capture.schemas.begin(),
                capture.schemas.end(),
                [&](const Moer::ProfileDump::DecodedSchema& schema) {
                    return schema.schema_id == schema_id;
                }
            );
            if (schema_iter == capture.schemas.end()) {
                return false;
            }

            Moer::ProfileDump::DecodedRecord record{};
            if (!Moer::ProfileDump::DeserializeRecordPacket(header, payload, *schema_iter, record)) {
                return false;
            }
            capture.records.emplace_back(std::move(record));
            continue;
        }

        return false;
    }

    return true;
}

bool AssertRuntimeProfileRecord(
    const Moer::ProfileDump::DecodedRecord& record,
    uint64_t                                expected_record_id,
    std::string_view                        expected_label,
    uint32_t                                expected_value
) {
    if (record.fields.size() != 3) {
        LOG_ERROR(MOER_TEXT("Runtime profile dump record expected 3 fields, got {}."), record.fields.size());
        return false;
    }

    const auto& record_id = record.fields[0];
    const auto& label = record.fields[1];
    const auto& value = record.fields[2];
    if (record_id.type != Moer::ProfileDump::EFieldType::UInt64 ||
        label.type != Moer::ProfileDump::EFieldType::String ||
        value.type != Moer::ProfileDump::EFieldType::UInt32) {
        LOG_ERROR(MOER_TEXT("Runtime profile dump record has unexpected field types."));
        return false;
    }
    if (record_id.uint64_value != expected_record_id ||
        label.string_value != expected_label ||
        value.uint32_value != expected_value) {
        LOG_ERROR(
            MOER_TEXT("Runtime profile dump record mismatch: id={} label={} value={}."),
            record_id.uint64_value,
            label.string_value,
            value.uint32_value
        );
        return false;
    }
    return true;
}

const Moer::Profiler::ProfileEvent* FindProfileConsumerEvent(
    const Moer::Profiler::ProfileStore& store,
    std::string_view                    event_name
) {
    for (const Moer::Profiler::ProfileEvent& event : store.events) {
        if (event.name == event_name) {
            return &event;
        }
    }
    return nullptr;
}

bool AssertProfileConsumerNormalizedStore(const Moer::Profiler::ProfileStore& store) {
    if (!store.metadata.has_time_origin || store.metadata.time_origin_ns != 200000) {
        LOG_ERROR(
            MOER_TEXT("Profile consumer expected normalized origin 200000ns, got has_origin={} origin={}"),
            store.metadata.has_time_origin,
            store.metadata.time_origin_ns
        );
        return false;
    }
    if (store.events.size() != 2 || store.tracks.size() != 2 || store.min_ts != 0 || store.max_ts != 80000) {
        LOG_ERROR(
            MOER_TEXT("Profile consumer normalized store mismatch: events={} tracks={} min={} max={}"),
            store.events.size(),
            store.tracks.size(),
            store.min_ts,
            store.max_ts
        );
        return false;
    }

    const auto* gpu_event = FindProfileConsumerEvent(store, "GpuNormalizedScope");
    const auto* cpu_event = FindProfileConsumerEvent(store, "CpuNormalizedScope");
    if (gpu_event == nullptr || cpu_event == nullptr) {
        LOG_ERROR(MOER_TEXT("Profile consumer normalized store missing CPU or GPU scope event."));
        return false;
    }

    if (gpu_event->track_type != Moer::Profiler::ProfileTrackType::GPUQueue ||
        gpu_event->ts_begin_ns != 50000 || gpu_event->ts_end_ns != 80000 ||
        gpu_event->category != "timing.gpu_scope" || gpu_event->track_name != "Graphics") {
        LOG_ERROR(
            MOER_TEXT("Profile consumer normalized GPU event mismatch: begin={} end={} category={} track={}"),
            gpu_event->ts_begin_ns,
            gpu_event->ts_end_ns,
            gpu_event->category,
            gpu_event->track_name
        );
        return false;
    }

    if (cpu_event->track_type != Moer::Profiler::ProfileTrackType::CPUThread ||
        cpu_event->ts_begin_ns != 0 || cpu_event->ts_end_ns != 10000 ||
        cpu_event->category != "timing.cpu_scope") {
        LOG_ERROR(
            MOER_TEXT("Profile consumer normalized CPU event mismatch: begin={} end={} category={}"),
            cpu_event->ts_begin_ns,
            cpu_event->ts_end_ns,
            cpu_event->category
        );
        return false;
    }

    return true;
}

bool AssertGpuProfileRecord(
    const Moer::ProfileDump::DecodedRecord& record,
    uint64_t                                expected_frame_index,
    std::string_view                        expected_queue_name,
    std::string_view                        expected_name,
    uint64_t                                expected_start_ns,
    uint64_t                                expected_end_ns,
    uint32_t                                expected_depth,
    uint64_t                                expected_total_busy_ns,
    uint64_t                                expected_exclusive_ns
) {
    if (record.fields.size() != 8) {
        LOG_ERROR(MOER_TEXT("GPU profile dump record expected 8 fields, got {}."), record.fields.size());
        return false;
    }

    const auto& frame_index = record.fields[0];
    const auto& queue_name = record.fields[1];
    const auto& name = record.fields[2];
    const auto& start_ns = record.fields[3];
    const auto& end_ns = record.fields[4];
    const auto& depth = record.fields[5];
    const auto& total_busy_ns = record.fields[6];
    const auto& exclusive_ns = record.fields[7];
    if (frame_index.type != Moer::ProfileDump::EFieldType::UInt64 ||
        queue_name.type != Moer::ProfileDump::EFieldType::String ||
        name.type != Moer::ProfileDump::EFieldType::String ||
        start_ns.type != Moer::ProfileDump::EFieldType::UInt64 ||
        end_ns.type != Moer::ProfileDump::EFieldType::UInt64 ||
        depth.type != Moer::ProfileDump::EFieldType::UInt32 ||
        total_busy_ns.type != Moer::ProfileDump::EFieldType::UInt64 ||
        exclusive_ns.type != Moer::ProfileDump::EFieldType::UInt64) {
        LOG_ERROR(MOER_TEXT("GPU profile dump record has unexpected field types."));
        return false;
    }
    if (frame_index.uint64_value != expected_frame_index ||
        queue_name.string_value != expected_queue_name ||
        name.string_value != expected_name ||
        start_ns.uint64_value != expected_start_ns ||
        end_ns.uint64_value != expected_end_ns ||
        depth.uint32_value != expected_depth ||
        total_busy_ns.uint64_value != expected_total_busy_ns ||
        exclusive_ns.uint64_value != expected_exclusive_ns) {
        LOG_ERROR(
            MOER_TEXT("GPU profile dump record mismatch: frame={} queue={} name={} start={} end={} depth={} total={} exclusive={}."),
            frame_index.uint64_value,
            queue_name.string_value,
            name.string_value,
            start_ns.uint64_value,
            end_ns.uint64_value,
            depth.uint32_value,
            total_busy_ns.uint64_value,
            exclusive_ns.uint64_value
        );
        return false;
    }
    return true;
}

#if defined(MOER_TEST_WITH_PROFILE)
bool AssertCpuProfileRecord(const Moer::ProfileDump::DecodedRecord& record, std::string_view expected_name) {
    if (record.fields.size() != 5) {
        LOG_ERROR(MOER_TEXT("CPU profile dump record expected 5 fields, got {}."), record.fields.size());
        return false;
    }

    const auto& thread_id = record.fields[0];
    const auto& name = record.fields[1];
    const auto& start_us = record.fields[2];
    const auto& duration_us = record.fields[3];
    const auto& depth = record.fields[4];
    if (thread_id.type != Moer::ProfileDump::EFieldType::UInt64 ||
        name.type != Moer::ProfileDump::EFieldType::String ||
        start_us.type != Moer::ProfileDump::EFieldType::Int64 ||
        duration_us.type != Moer::ProfileDump::EFieldType::Int64 ||
        depth.type != Moer::ProfileDump::EFieldType::UInt32) {
        LOG_ERROR(MOER_TEXT("CPU profile dump record has unexpected field types."));
        return false;
    }
    if (thread_id.uint64_value == 0 ||
        name.string_value != expected_name ||
        duration_us.int64_value < 0 ||
        depth.uint32_value != 0) {
        LOG_ERROR(
            MOER_TEXT("CPU profile dump record mismatch: thread={} name={} start={} duration={} depth={}."),
            thread_id.uint64_value,
            name.string_value,
            start_us.int64_value,
            duration_us.int64_value,
            depth.uint32_value
        );
        return false;
    }
    return true;
}
#endif

#if defined(_WIN32)
class ProfileDumpTcpCaptureServer {
public:
    ~ProfileDumpTcpCaptureServer() {
        Stop();
    }

    bool Start() {
        if (running.exchange(true)) {
            return true;
        }

        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            running.store(false);
            return false;
        }
        winsock_initialized = true;

        listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_socket == INVALID_SOCKET) {
            Stop();
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        const int reuse = 1;
        setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        if (bind(listen_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            listen(listen_socket, 1) != 0) {
            Stop();
            return false;
        }

        int actual_length = sizeof(addr);
        if (getsockname(listen_socket, reinterpret_cast<sockaddr*>(&addr), &actual_length) != 0) {
            Stop();
            return false;
        }
        port = ntohs(addr.sin_port);

        server_thread = std::thread([this]() {
            sockaddr_in client_addr{};
            int         client_length = sizeof(client_addr);
            const SOCKET accepted_socket =
                accept(listen_socket, reinterpret_cast<sockaddr*>(&client_addr), &client_length);
            if (accepted_socket == INVALID_SOCKET) {
                return;
            }

            client_socket = accepted_socket;
            uint8_t buffer[4096]{};
            while (running.load()) {
                const int received = recv(
                    accepted_socket,
                    reinterpret_cast<char*>(buffer),
                    static_cast<int>(sizeof(buffer)),
                    0
                );
                if (received <= 0) {
                    break;
                }

                std::lock_guard lock(buffer_mutex);
                received_bytes.insert(received_bytes.end(), buffer, buffer + received);
            }
        });

        return true;
    }

    void Stop() {
        if (!running.exchange(false)) {
            return;
        }

        if (listen_socket != INVALID_SOCKET) {
            closesocket(listen_socket);
            listen_socket = INVALID_SOCKET;
        }
        if (client_socket != INVALID_SOCKET) {
            closesocket(client_socket);
            client_socket = INVALID_SOCKET;
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }
        if (winsock_initialized) {
            WSACleanup();
            winsock_initialized = false;
        }
    }

    uint16_t GetPort() const {
        return port;
    }

    bool WaitForAtLeast(size_t min_bytes, int timeout_ms) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock(buffer_mutex);
                if (received_bytes.size() >= min_bytes) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        std::lock_guard lock(buffer_mutex);
        return received_bytes.size() >= min_bytes;
    }

    std::vector<uint8_t> SnapshotBytes() const {
        std::lock_guard lock(buffer_mutex);
        return received_bytes;
    }

private:
    mutable std::mutex   buffer_mutex{};
    std::vector<uint8_t> received_bytes{};
    std::atomic<bool>    running{false};
    bool                 winsock_initialized = false;
    uint16_t             port = 0;
    SOCKET               listen_socket = INVALID_SOCKET;
    SOCKET               client_socket = INVALID_SOCKET;
    std::thread          server_thread{};
};
#endif

int RunProfileDumpStartupReadOnlyCVarTest() {
    static int startup_locked_value = 5;
    static Moer::CVar::TCVar<int> startup_locked_cvar(
        "Test.ProfileDump.StartupLockedValue",
        startup_locked_value,
        "Startup-only profile dump cvar for test.",
        "Startup-only profile dump cvar set.",
        "Startup-only profile dump cvar set.",
        Moer::CVar::EFlags::StartupConfigReadOnly
    );

    Moer::UnorderedMap<std::string, std::string> values;
    values["Test.ProfileDump.StartupLockedValue"] = "19";
    if (!Moer::CVar::ApplyValueMap(values, "ProfileDumpTest")) {
        LOG_ERROR(MOER_TEXT("Profile dump startup readonly test failed to apply startup override."));
        return 1;
    }
    Moer::CVar::SealStartupConfigReadOnlyCVars();
    if (startup_locked_value != 19) {
        LOG_ERROR(MOER_TEXT("Profile dump startup readonly test expected value 19, got {}."), startup_locked_value);
        return 1;
    }
    if (startup_locked_cvar.SetValueFromString("23") == nullptr) {
        LOG_ERROR(MOER_TEXT("Profile dump startup readonly cvar accepted runtime mutation after seal."));
        return 1;
    }
    return 0;
}

int RunProfileDumpFileSinkTest() {
    const std::filesystem::path output_path = MakeProfileDumpTestPath("profile_dump_runtime_test.mpd");

    Moer::ProfileDump::RuntimeConfig config{};
    config.enable_file = true;
    config.enable_tcp = false;
    config.tls_max_records = 128;
    config.tls_max_bytes = 32768;
    config.auto_publish_records = 128;
    config.auto_publish_bytes = 32768;
    config.file_path = output_path.generic_string();
    Moer::ProfileDump::OverrideConfigForTesting(config);

    DUMP_STREAM(ProfileDumpRuntimeTemplate)
        << uint64_t{42}
        << "ProfileDumpRuntimeRecord"
        << uint32_t{99};

    if (std::filesystem::exists(output_path)) {
        LOG_ERROR(MOER_TEXT("Profile dump file sink wrote before explicit TLS flush."));
        Moer::ProfileDump::ClearTestingConfigOverride();
        return 1;
    }

    Moer::ProfileDump::FlushThreadLocal();

    if (!std::filesystem::exists(output_path)) {
        LOG_ERROR(MOER_TEXT("Profile dump file sink did not create output file after explicit flush."));
        Moer::ProfileDump::ClearTestingConfigOverride();
        return 1;
    }

    const std::vector<uint8_t> binary = ReadBinaryFile(output_path);
    ParsedProfileDumpCapture   capture{};
    if (!ParseProfileDumpCapture(std::span<const uint8_t>(binary.data(), binary.size()), capture)) {
        LOG_ERROR(MOER_TEXT("Profile dump file sink output failed structured parsing."));
        Moer::ProfileDump::ClearTestingConfigOverride();
        return 1;
    }

    const Moer::ProfileDump::DecodedSchema* runtime_schema =
        FindProfileDumpSchemaByEventType(capture, "test.runtime_instant");
    if (!runtime_schema) {
        LOG_ERROR(MOER_TEXT("Profile dump file sink output missing runtime schema."));
        Moer::ProfileDump::ClearTestingConfigOverride();
        return 1;
    }
    if (capture.packet_order.size() != 2 ||
        capture.packet_order[0] != Moer::ProfileDump::EPacketType::Schema ||
        capture.packet_order[1] != Moer::ProfileDump::EPacketType::Record) {
        LOG_ERROR(MOER_TEXT("Profile dump file sink packet order is invalid."));
        Moer::ProfileDump::ClearTestingConfigOverride();
        return 1;
    }
    if (capture.records.size() != 1 || capture.records[0].schema_id != runtime_schema->schema_id ||
        !AssertRuntimeProfileRecord(capture.records[0], 42, "ProfileDumpRuntimeRecord", 99)) {
        LOG_ERROR(MOER_TEXT("Profile dump file sink record payload is invalid."));
        Moer::ProfileDump::ClearTestingConfigOverride();
        return 1;
    }

    Moer::ProfileDump::ClearTestingConfigOverride();
    return 0;
}

int RunProfileDumpTcpSinkTest() {
#if !defined(_WIN32)
    return 0;
#else
    ProfileDumpTcpCaptureServer server{};
    if (!server.Start()) {
        LOG_ERROR(MOER_TEXT("Profile dump TCP test failed to start loopback server."));
        return 1;
    }

    Moer::ProfileDump::RuntimeConfig config{};
    config.enable_file = false;
    config.enable_tcp = true;
    config.tls_max_records = 128;
    config.tls_max_bytes = 32768;
    config.auto_publish_records = 128;
    config.auto_publish_bytes = 32768;
    config.tcp_host = "127.0.0.1";
    config.tcp_port = server.GetPort();
    Moer::ProfileDump::OverrideConfigForTesting(config);

    DUMP_STREAM(ProfileDumpRuntimeTemplate)
        << uint64_t{7}
        << "TcpRuntimeRecordA"
        << uint32_t{11};
    DUMP_STREAM(ProfileDumpRuntimeTemplate)
        << uint64_t{8}
        << "TcpRuntimeRecordB"
        << uint32_t{13};
    Moer::ProfileDump::FlushThreadLocal();

    if (!server.WaitForAtLeast(sizeof(Moer::ProfileDump::PacketHeader), 2000)) {
        LOG_ERROR(MOER_TEXT("Profile dump TCP sink produced no payload."));
        Moer::ProfileDump::ClearTestingConfigOverride();
        server.Stop();
        return 1;
    }

    Moer::ProfileDump::ClearTestingConfigOverride();
    server.Stop();

    const std::vector<uint8_t> binary = server.SnapshotBytes();
    ParsedProfileDumpCapture   capture{};
    if (!ParseProfileDumpCapture(std::span<const uint8_t>(binary.data(), binary.size()), capture)) {
        LOG_ERROR(MOER_TEXT("Profile dump TCP sink payload failed structured parsing."));
        return 1;
    }

    const Moer::ProfileDump::DecodedSchema* runtime_schema =
        FindProfileDumpSchemaByEventType(capture, "test.runtime_instant");
    if (!runtime_schema) {
        LOG_ERROR(MOER_TEXT("Profile dump TCP sink payload missing runtime schema."));
        return 1;
    }
    if (capture.packet_order.size() != 3 ||
        capture.packet_order[0] != Moer::ProfileDump::EPacketType::Schema ||
        capture.packet_order[1] != Moer::ProfileDump::EPacketType::Record ||
        capture.packet_order[2] != Moer::ProfileDump::EPacketType::Record) {
        LOG_ERROR(MOER_TEXT("Profile dump TCP sink packet order is invalid."));
        return 1;
    }
    if (capture.records.size() != 2 ||
        capture.records[0].schema_id != runtime_schema->schema_id ||
        capture.records[1].schema_id != runtime_schema->schema_id ||
        !AssertRuntimeProfileRecord(capture.records[0], 7, "TcpRuntimeRecordA", 11) ||
        !AssertRuntimeProfileRecord(capture.records[1], 8, "TcpRuntimeRecordB", 13)) {
        LOG_ERROR(MOER_TEXT("Profile dump TCP sink records are invalid."));
        return 1;
    }

    return 0;
#endif
}

int RunGpuEventStreamProfileDumpTest() {
    const std::filesystem::path output_path = MakeProfileDumpTestPath("profile_dump_gpu_event_test.mpd");

    Moer::ProfileDump::RuntimeConfig config{};
    config.enable_file = true;
    config.enable_tcp = false;
    config.tls_max_records = 128;
    config.tls_max_bytes = 32768;
    config.auto_publish_records = 128;
    config.auto_publish_bytes = 32768;
    config.file_path = output_path.generic_string();
    Moer::ProfileDump::OverrideConfigForTesting(config);

    auto& stream = GPUEventStream::Get();
    stream.ResetForTesting();

    Array<GPUEvent> events;
    events.emplace_back(MakeResolvedGpuEvent(GPUEvent::EType::BeginGPU, "GpuDumpScope", 0, 100));
    events.emplace_back(MakeResolvedGpuEvent(GPUEvent::EType::EndGPU, "GpuDumpScope", 0, 180));
    events.emplace_back(MakeResolvedGpuEvent(GPUEvent::EType::FrameBoundary, "FrameBoundary", 0, 200));
    stream.InjectResolvedSubmitForTesting(std::move(events), EQueueType::Graphics);
    stream.EndFrame();
    stream.FlushToProfiler();

    if (!std::filesystem::exists(output_path)) {
        LOG_ERROR(MOER_TEXT("GPUEventStream profile dump integration did not write output file."));
        Moer::ProfileDump::ClearTestingConfigOverride();
        return 1;
    }

    const std::vector<uint8_t> binary = ReadBinaryFile(output_path);
    ParsedProfileDumpCapture   capture{};
    if (!ParseProfileDumpCapture(std::span<const uint8_t>(binary.data(), binary.size()), capture)) {
        LOG_ERROR(MOER_TEXT("GPUEventStream profile dump output failed structured parsing."));
        Moer::ProfileDump::ClearTestingConfigOverride();
        return 1;
    }

    const Moer::ProfileDump::DecodedSchema* gpu_schema =
        FindProfileDumpSchemaByEventType(capture, "timing.gpu_scope");
    if (!gpu_schema) {
        LOG_ERROR(MOER_TEXT("GPUEventStream profile dump output missing GPU schema."));
        Moer::ProfileDump::ClearTestingConfigOverride();
        return 1;
    }
    if (capture.records.size() != 1 || capture.records[0].schema_id != gpu_schema->schema_id ||
        !AssertGpuProfileRecord(capture.records[0], 0, "Graphics", "GpuDumpScope", 100, 180, 0, 80, 80)) {
        LOG_ERROR(MOER_TEXT("GPUEventStream profile dump output has invalid GPU record payload."));
        Moer::ProfileDump::ClearTestingConfigOverride();
        return 1;
    }

    Moer::ProfileDump::ClearTestingConfigOverride();
    return 0;
}

int RunProfileConsumerFileLoadTest() {
    const std::filesystem::path output_path = MakeProfileDumpTestPath("profile_consumer_file_load_test.mpd");

    Moer::ProfileDump::RuntimeConfig config{};
    config.enable_file = true;
    config.enable_tcp = false;
    config.tls_max_records = 128;
    config.tls_max_bytes = 32768;
    config.auto_publish_records = 128;
    config.auto_publish_bytes = 32768;
    config.file_path = output_path.generic_string();
    Moer::ProfileDump::OverrideConfigForTesting(config);

    DUMP_STREAM(Moer::ProfileDump::Templates::GpuScopeTemplate)
        << uint64_t{3}
        << "Graphics"
        << "GpuNormalizedScope"
        << uint64_t{250000}
        << uint64_t{280000}
        << uint32_t{0}
        << uint64_t{30000}
        << uint64_t{30000};
    DUMP_STREAM(Moer::ProfileDump::Templates::CpuScopeTemplate)
        << uint64_t{5}
        << "CpuNormalizedScope"
        << int64_t{200}
        << int64_t{10}
        << uint32_t{1};
    Moer::ProfileDump::FlushThreadLocal();

    Moer::Profiler::ProfileStore store{};
    const bool loaded = Moer::Profiler::LoadProfileDumpFile(output_path, store, true);
    Moer::ProfileDump::ClearTestingConfigOverride();
    if (!loaded) {
        LOG_ERROR(MOER_TEXT("Profile consumer file load failed."));
        return 1;
    }
    if (store.metadata.session_name != output_path.filename().string()) {
        LOG_ERROR(MOER_TEXT("Profile consumer file load session name mismatch: {}"), store.metadata.session_name);
        return 1;
    }
    if (!AssertProfileConsumerNormalizedStore(store)) {
        return 1;
    }
    return 0;
}

int RunProfileConsumerStreamNormalizationTest() {
    const std::filesystem::path output_path = MakeProfileDumpTestPath("profile_consumer_stream_test.mpd");

    Moer::ProfileDump::RuntimeConfig config{};
    config.enable_file = true;
    config.enable_tcp = false;
    config.tls_max_records = 128;
    config.tls_max_bytes = 32768;
    config.auto_publish_records = 128;
    config.auto_publish_bytes = 32768;
    config.file_path = output_path.generic_string();
    Moer::ProfileDump::OverrideConfigForTesting(config);

    DUMP_STREAM(Moer::ProfileDump::Templates::GpuScopeTemplate)
        << uint64_t{3}
        << "Graphics"
        << "GpuNormalizedScope"
        << uint64_t{250000}
        << uint64_t{280000}
        << uint32_t{0}
        << uint64_t{30000}
        << uint64_t{30000};
    DUMP_STREAM(Moer::ProfileDump::Templates::CpuScopeTemplate)
        << uint64_t{5}
        << "CpuNormalizedScope"
        << int64_t{200}
        << int64_t{10}
        << uint32_t{1};
    Moer::ProfileDump::FlushThreadLocal();
    Moer::ProfileDump::ClearTestingConfigOverride();

    const std::vector<uint8_t> binary = ReadBinaryFile(output_path);
    if (binary.empty()) {
        LOG_ERROR(MOER_TEXT("Profile consumer stream normalization test produced no binary capture."));
        return 1;
    }

    Moer::Profiler::ProfileDumpSessionDecoder decoder{};
    Moer::Profiler::ProfileStore              store{};
    store.SetSessionName("ProfileDump TCP");

    size_t offset = 0;
    while (offset < binary.size()) {
        Moer::ProfileDump::PacketHeader header{};
        std::span<const uint8_t> payload{};
        if (!Moer::ProfileDump::ReadNextPacket(std::span<const uint8_t>(binary.data(), binary.size()), offset, header, payload)) {
            LOG_ERROR(MOER_TEXT("Profile consumer stream normalization failed to read packet."));
            return 1;
        }

        Array<Moer::Profiler::ProfileEvent> packet_events{};
        if (!decoder.ConsumePacket(header, payload, packet_events)) {
            LOG_ERROR(MOER_TEXT("Profile consumer stream normalization failed to consume packet."));
            return 1;
        }
        if (!packet_events.empty()) {
            store.AppendEvents(packet_events);
        }
    }

    if (store.metadata.session_name != "ProfileDump TCP") {
        LOG_ERROR(MOER_TEXT("Profile consumer stream normalization session name mismatch: {}"), store.metadata.session_name);
        return 1;
    }
    if (!AssertProfileConsumerNormalizedStore(store)) {
        return 1;
    }
    return 0;
}

#if defined(MOER_TEST_WITH_PROFILE)
int RunFlameProfilerProfileDumpTest() {
    const std::filesystem::path output_path = MakeProfileDumpTestPath("profile_dump_flame_test.mpd");

    FlameProfiler::Get().Begin("CpuDumpScope");
    FlameProfiler::Get().End();
    FlameProfiler::Get().Save(output_path.generic_string());

    if (!std::filesystem::exists(output_path)) {
        LOG_ERROR(MOER_TEXT("FlameProfiler unified profile dump did not write output file."));
        return 1;
    }

    const std::vector<uint8_t> binary = ReadBinaryFile(output_path);
    ParsedProfileDumpCapture   capture{};
    if (!ParseProfileDumpCapture(std::span<const uint8_t>(binary.data(), binary.size()), capture)) {
        LOG_ERROR(MOER_TEXT("FlameProfiler unified profile dump output failed structured parsing."));
        return 1;
    }

    const Moer::ProfileDump::DecodedSchema* cpu_schema =
        FindProfileDumpSchemaByEventType(capture, "timing.cpu_scope");
    if (!cpu_schema) {
        LOG_ERROR(MOER_TEXT("FlameProfiler unified profile dump output missing CPU schema."));
        return 1;
    }
    if (capture.records.size() != 1 || capture.records[0].schema_id != cpu_schema->schema_id ||
        !AssertCpuProfileRecord(capture.records[0], "CpuDumpScope")) {
        LOG_ERROR(MOER_TEXT("FlameProfiler unified profile dump output has invalid CPU record payload."));
        return 1;
    }

    return 0;
}
#endif

void ApplyShuffle(
    const std::vector<uint32_t>& input,
    const std::vector<IndicePair>& pairs,
    std::vector<uint32_t>& output
) {
    std::fill(output.begin(), output.end(), 0u);
    for (const auto& pair : pairs) {
        output[pair.dst] = input[pair.src];
    }
}

std::vector<IndicePair> BuildPermutationPairs(uint32_t multiplier, uint32_t bias) {
    std::vector<IndicePair> pairs(kElementCount);
    for (uint32_t i = 0; i < kElementCount; ++i) {
        pairs[i] = {.src = i, .dst = (i * multiplier + bias) % kElementCount};
    }
    return pairs;
}

void SubmitAndWait(Array<CommandList>&& command_lists) {
    RHIExecutor::Get().Submit(std::move(command_lists), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
}

bool ValidateResult(
    uint32_t iter,
    const std::vector<uint32_t>& expected,
    const std::vector<uint32_t>& got
) {
    if (expected == got) {
        return true;
    }
    uint32_t mismatch_count = 0;
    for (uint32_t i = 0; i < kElementCount; ++i) {
        if (expected[i] != got[i]) {
            LOG_ERROR(
                MOER_TEXT("Mismatch at iter={}, index={}, expected={}, got={}"),
                iter,
                i,
                expected[i],
                got[i]
            );
            ++mismatch_count;
            if (mismatch_count >= 8) {
                break;
            }
        }
    }
    return false;
}

bool ValidateUniformValue(uint32_t iter, uint32_t expected, const std::vector<uint32_t>& got) {
    for (uint32_t i = 0; i < got.size(); ++i) {
        if (got[i] != expected) {
            LOG_ERROR(
                MOER_TEXT("Uniform mismatch at iter={}, index={}, expected={}, got={}"),
                iter,
                i,
                expected,
                got[i]
            );
            return false;
        }
    }
    return true;
}

bool ValidateAllocatedRanges(const std::vector<uint64_t>& offsets, uint64_t alloc_size) {
    if (offsets.empty()) {
        LOG_ERROR(MOER_TEXT("Concurrent descriptor range test produced no offsets"));
        return false;
    }

    std::vector<uint64_t> sorted = offsets;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i] < sorted[i - 1] + alloc_size) {
            LOG_ERROR(
                MOER_TEXT("Descriptor range overlap detected: prev={}, current={}, alloc_size={}"),
                sorted[i - 1],
                sorted[i],
                alloc_size
            );
            return false;
        }
    }
    return true;
}

int RunDescriptorHeapConcurrentRangeAllocationTest() {
    LOG_INFO(MOER_TEXT("Descriptor heap concurrent range allocation test started"));
    auto* vk_device = dynamic_cast<VulkanDevice*>(RenderDevice::Get().GetImpl());
    if (vk_device == nullptr) {
        LOG_ERROR(MOER_TEXT("Descriptor heap range allocation test requires VulkanDevice"));
        return 1;
    }

    VulkanDescriptorHeap& descriptor_heap = vk_device->GetGlobalDescriptorHeap();
    VulkanDescriptorBinder binder = descriptor_heap.BeginPushDescriptors();
    if (!binder.IsValid()) {
        LOG_ERROR(MOER_TEXT("Descriptor heap failed to start concurrent range-allocation binder"));
        return 1;
    }

    constexpr uint32_t kThreadCount = 8;
    constexpr uint32_t kAllocationsPerThread = 24;
    const uint64_t alignment =
        vk_device->GetOptionalProperties().descriptor_buffer_properties.descriptorBufferOffsetAlignment;
    const uint64_t alloc_size = Moer::AlignUp(uint64_t(64), alignment);

    std::vector<uint64_t> allocated_offsets;
    allocated_offsets.reserve(kThreadCount * kAllocationsPerThread);
    std::mutex allocation_mutex;
    std::array<std::thread, kThreadCount> threads;
    for (uint32_t thread_idx = 0; thread_idx < kThreadCount; ++thread_idx) {
        threads[thread_idx] = std::thread([&]() {
            binder.ActivateOnCurrentThread();
            std::vector<uint64_t> local_offsets;
            local_offsets.reserve(kAllocationsPerThread);
            for (uint32_t i = 0; i < kAllocationsPerThread; ++i) {
                local_offsets.push_back(descriptor_heap.AllocateOnlineDescriptorRange(alloc_size));
            }
            binder.DeactivateOnCurrentThread();
            std::lock_guard<std::mutex> lock(allocation_mutex);
            allocated_offsets.insert(
                allocated_offsets.end(), local_offsets.begin(), local_offsets.end()
            );
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    LOG_INFO(MOER_TEXT("Descriptor heap test: concurrent range allocation finished"));

    if (!ValidateAllocatedRanges(allocated_offsets, alloc_size)) {
        binder = descriptor_heap.EndPushDescriptors(std::move(binder));
        descriptor_heap.RecycleOnlineDescriptorLease(std::move(binder));
        return 1;
    }

    const uint64_t trailing_offset = descriptor_heap.AllocateOnlineDescriptorRange(alloc_size);
    const uint64_t max_offset = *std::max_element(allocated_offsets.begin(), allocated_offsets.end());
    if (trailing_offset < max_offset + alloc_size) {
        LOG_ERROR(
            MOER_TEXT("Trailing descriptor allocation overlapped earlier concurrent allocations: trailing={}, max={}"),
            trailing_offset,
            max_offset
        );
        binder = descriptor_heap.EndPushDescriptors(std::move(binder));
        descriptor_heap.RecycleOnlineDescriptorLease(std::move(binder));
        return 1;
    }

    binder = descriptor_heap.EndPushDescriptors(std::move(binder));
    descriptor_heap.RecycleOnlineDescriptorLease(std::move(binder));
    LOG_INFO(MOER_TEXT("Descriptor heap concurrent range allocation test passed"));
    return 0;
}

int RunCommandListQueueBindingTest() {
    CommandList graphics_cmd(EQueueType::Graphics);

    if (graphics_cmd.GetQueueType() != EQueueType::Graphics) {
        LOG_ERROR(MOER_TEXT("Graphics command list queue binding mismatch"));
        return 1;
    }

    LOG_INFO(MOER_TEXT("CommandList queue binding test passed"));
    return 0;
}

int RunTranslateExecutionClassRoundTripTest() {
    const RHITranslateFence translate_fence = RHITranslateFence::Create();

    CommandList cmd(EQueueType::Graphics);
    cmd.SetTranslateExecutionClass(ERHITranslateExecutionClass::SerialControl);
    cmd.LambdaCommand([] {});
    cmd.TranslateFence(translate_fence);

    CmdSubmit submit = cmd.Submit();
    if (submit.translate_execution_class != ERHITranslateExecutionClass::SerialControl) {
        LOG_ERROR(MOER_TEXT("Translate execution class did not round-trip through CommandList::Submit"));
        return 1;
    }
    if (submit.cmds.size() != 2 || submit.cmds.back()->Type() != Command::EType::Custom) {
        LOG_ERROR(MOER_TEXT("Translate fence command did not round-trip through CommandList::Submit"));
        return 1;
    }

    const auto* custom_cmd = static_cast<const CustomCmd*>(submit.cmds.back().get());
    if (custom_cmd->CustomId() != CustomCmd::CustomCmdId::CUSTOM_TRANSLATE_FENCE) {
        LOG_ERROR(MOER_TEXT("Submitted command was not a TranslateFence command"));
        return 1;
    }

    const auto* translate_fence_cmd = static_cast<const TranslateFenceCmd*>(custom_cmd);
    if (translate_fence_cmd->Fence().event.Get() != translate_fence.event.Get()) {
        LOG_ERROR(MOER_TEXT("Translate fence event did not round-trip through CommandList::Submit"));
        return 1;
    }

    translate_fence.event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    LOG_INFO(MOER_TEXT("Translate execution control round-trip test passed"));
    return 0;
}

int RunTranslateLambdaCommandTest() {
    std::atomic_uint32_t lambda_counter{0};

    CommandList cmd(EQueueType::Graphics);
    cmd.LambdaCommand([&lambda_counter]() {
        lambda_counter.fetch_add(1, std::memory_order_relaxed);
    });

    Array<CommandList> frame_cmds{};
    frame_cmds.emplace_back(std::move(cmd));
    RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (lambda_counter.load(std::memory_order_relaxed) != 1u) {
        LOG_ERROR(MOER_TEXT("LambdaCommand did not execute exactly once during translate"));
        return 1;
    }

    LOG_INFO(MOER_TEXT("Translate LambdaCommand test passed"));
    return 0;
}

int RunRHITranslateMultiQueueReadbackTest() {
    auto& device = RenderDevice::Get();

    auto src = device.CreateBuffer<uint32_t>(
        "translate_src",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );
    auto mid = device.CreateBuffer<uint32_t>(
        "translate_mid",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_SRC
    );
    auto dst = device.CreateBuffer<uint32_t>(
        "translate_dst",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_SRC
    );
    auto indices_stage0 = device.CreateBuffer<IndicePair>(
        "translate_indices_stage0",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );
    auto indices_stage1 = device.CreateBuffer<IndicePair>(
        "translate_indices_stage1",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );

    auto shuffle_pipeline =
        ShaderManager::Get().Compute<ComponentShuffleShader>("core/utils/ShuffleBufferIndices.hlsl");

    auto perm0 = BuildPermutationPairs(37u, 11u);
    auto perm1 = BuildPermutationPairs(53u, 7u);

    std::vector<uint32_t> src_data(kElementCount);
    std::vector<uint32_t> stage0_expected(kElementCount);
    std::vector<uint32_t> final_expected(kElementCount);
    std::vector<uint32_t> readback_data(kElementCount);

    for (uint32_t iter = 0; iter < kIterations; ++iter) {
        for (uint32_t i = 0; i < kElementCount; ++i) {
            src_data[i] = iter * 4096u + i * 3u + 17u;
        }

        ApplyShuffle(src_data, perm0, stage0_expected);
        ApplyShuffle(stage0_expected, perm1, final_expected);

        CommandList upload_cmd(EQueueType::Graphics);
        upload_cmd.CopyFrom(ToByteSpan(src_data), src->GetView());
        upload_cmd.CopyFrom(ToByteSpan(perm0), indices_stage0->GetView());
        upload_cmd.CopyFrom(ToByteSpan(perm1), indices_stage1->GetView());

        ComponentShuffleShader::Arg shuffle_args{
            .stride = 1u,
            .component_cnt = kElementCount,
        };

        CommandList compute_stage0_cmd(EQueueType::Graphics);
        compute_stage0_cmd
            .Compute(shuffle_pipeline, shuffle_args, indices_stage0->GetView(), src->GetView(), mid->GetView())
            .Dispatch((kElementCount + 63u) / 64u, "TranslateStage0Dispatch");

        CommandList compute_stage1_cmd(EQueueType::Graphics);
        compute_stage1_cmd
            .Compute(shuffle_pipeline, shuffle_args, indices_stage1->GetView(), mid->GetView(), dst->GetView())
            .Dispatch((kElementCount + 63u) / 64u, "TranslateStage1Dispatch");

        std::fill(readback_data.begin(), readback_data.end(), 0u);
        CommandList readback_cmd(EQueueType::Graphics);
        GraphEventRef readback_event = readback_cmd.ReadbackCopy(dst->GetView(), ToByteSpan(readback_data));

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(upload_cmd));
        frame_cmds.emplace_back(std::move(compute_stage0_cmd));
        frame_cmds.emplace_back(std::move(compute_stage1_cmd));
        frame_cmds.emplace_back(std::move(readback_cmd));

        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
        if (readback_event) {
            readback_event->Wait();
        }

        if (!ValidateResult(iter, final_expected, readback_data)) {
            return 1;
        }
    }

    LOG_INFO(MOER_TEXT("RHI translate multiqueue readback test passed, iterations={}"), kIterations);
    return 0;
}

int RunMultiCommandListSubmitOrderingTest() {
    auto& device = RenderDevice::Get();
    auto buffer = device.CreateBuffer<uint32_t>(
        "translate_multicmd_submit_order",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC
    );

    std::vector<uint32_t> upload_values(kElementCount);
    std::vector<uint32_t> readback_values(kElementCount, 0u);
    for (uint32_t i = 0; i < kElementCount; ++i) {
        upload_values[i] = 0x120000u + i * 13u + 7u;
    }

    CommandList upload_cmd(EQueueType::Graphics);
    upload_cmd.CopyFrom(ToByteSpan(upload_values), buffer->GetView());

    CommandList readback_cmd(EQueueType::Graphics);
    GraphEventRef readback_event =
        readback_cmd.ReadbackCopy(buffer->GetView(), ToByteSpan(readback_values));

    Array<CommandList> frame_cmds{};
    frame_cmds.emplace_back(std::move(upload_cmd));
    frame_cmds.emplace_back(std::move(readback_cmd));

    RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    if (readback_event) {
        readback_event->Wait();
    }

    if (!ValidateResult(0u, upload_values, readback_values)) {
        return 1;
    }

    LOG_INFO(MOER_TEXT("Multi-commandlist submit ordering test passed"));
    return 0;
}

int RunSerialControlTranslateOrderingTest() {
    auto& device = RenderDevice::Get();
    auto buffer = device.CreateBuffer<uint32_t>(
        "translate_serial_control_order",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC
    );

    std::vector<uint32_t> upload_values(kElementCount);
    std::vector<uint32_t> readback_values(kElementCount, 0u);
    for (uint32_t i = 0; i < kElementCount; ++i) {
        upload_values[i] = 0x560000u + i * 17u + 3u;
    }

    CommandList upload_cmd(EQueueType::Graphics);
    upload_cmd.SetTranslateExecutionClass(ERHITranslateExecutionClass::SerialControl);
    upload_cmd.CopyFrom(ToByteSpan(upload_values), buffer->GetView());

    CommandList readback_cmd(EQueueType::Graphics);
    readback_cmd.SetTranslateExecutionClass(ERHITranslateExecutionClass::SerialControl);
    GraphEventRef readback_event =
        readback_cmd.ReadbackCopy(buffer->GetView(), ToByteSpan(readback_values));

    Array<CommandList> frame_cmds{};
    frame_cmds.emplace_back(std::move(upload_cmd));
    frame_cmds.emplace_back(std::move(readback_cmd));

    RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    if (readback_event) {
        readback_event->Wait();
    }

    if (!ValidateResult(0u, upload_values, readback_values)) {
        return 1;
    }

    LOG_INFO(MOER_TEXT("SerialControl translate ordering test passed"));
    return 0;
}

int RunGraphicsCopyScopeRoundTripTest() {
    auto& device = RenderDevice::Get();
    auto scratch_buffer = device.CreateBuffer<uint32_t>(
        "copyscope_graphics_roundtrip_scratch",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto transfer_buffer = device.CreateBuffer<uint32_t>(
        "copyscope_graphics_roundtrip",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::UNORDERED_ACCESS
    );

    std::vector<uint32_t> upload_values(kElementCount);
    std::vector<uint32_t> readback_values(kElementCount, 0u);

    for (uint32_t iter = 0; iter < kCopyScopeIterations; ++iter) {
        for (uint32_t i = 0; i < kElementCount; ++i) {
            upload_values[i] = iter * 2048u + i * 5u + 9u;
        }
        std::fill(readback_values.begin(), readback_values.end(), 0u);

        CommandList graphics_cmd(EQueueType::Graphics);
        graphics_cmd.ClearResource(scratch_buffer->GetView(), 0u);
        {
            auto copy_scope = graphics_cmd.BeginCopyScope();
            copy_scope.CopyFrom(ToByteSpan(upload_values), transfer_buffer->GetView());
        }
        GraphEventRef readback_event =
            graphics_cmd.ReadbackCopy(transfer_buffer->GetView(), ToByteSpan(readback_values));

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(graphics_cmd));
        SubmitAndWait(std::move(frame_cmds));
        if (readback_event) {
            readback_event->Wait();
        }

        if (!ValidateResult(iter, upload_values, readback_values)) {
            return 1;
        }
    }

    LOG_INFO(MOER_TEXT("Graphics -> CopyScope -> Graphics test passed, iterations={}"), kCopyScopeIterations);
    return 0;
}

int RunBindlessBufferReadbackTest() {
    auto& device = RenderDevice::Get();

    auto bindless_array = device.CreateBindlessArray();
    auto src_a = device.CreateBuffer<uint32_t>(
        "bindless_src_a",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );
    auto src_b = device.CreateBuffer<uint32_t>(
        "bindless_src_b",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );
    auto output = device.CreateBuffer<uint32_t>(
        "bindless_readback_output",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );

    auto pipeline = ShaderManager::Get().Compute<BindlessBufferReadbackPipeline>(
        "tests/BindlessReadback.comp.hlsl"
    );

    std::vector<uint32_t> upload_values(kElementCount);
    std::vector<uint32_t> expected_values(kElementCount);
    std::vector<uint32_t> readback_values(kElementCount, 0u);

    const auto run_case = [&](uint32_t iter,
                              const BufferRef& src,
                              uint32_t src_handle,
                              uint32_t base,
                              uint32_t stride,
                              uint32_t bias,
                              uint32_t xor_mask) -> bool {
        for (uint32_t i = 0; i < kElementCount; ++i) {
            upload_values[i] = base + i * stride + bias;
            expected_values[i] = upload_values[i] ^ xor_mask;
        }
        std::fill(readback_values.begin(), readback_values.end(), 0u);

        CommandList cmd(EQueueType::Graphics);
        cmd.CopyFrom(ToByteSpan(upload_values), src->GetView());
        cmd.ClearResource(output->GetView(), 0u);
        cmd.UpdateBindlessArray(bindless_array);

        BindlessReadbackArgs args{
            .src_handle = src_handle,
            .xor_mask = xor_mask,
            .element_count = kElementCount,
        };

        cmd.Compute(pipeline, args, output->GetView(), bindless_array)
            .Dispatch((kElementCount + 63u) / 64u, "BindlessBufferReadbackDispatch");

        GraphEventRef readback_event =
            cmd.ReadbackCopy(output->GetView(), ToByteSpan(readback_values));

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(cmd));
        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
        if (readback_event) {
            readback_event->Wait();
        }

        return ValidateResult(iter, expected_values, readback_values);
    };

    const uint32_t src_handle_a = bindless_array->AllocateBuffer(src_a->GetView());
    if (!run_case(0u, src_a, src_handle_a, 0x1000u, 3u, 7u, 0x13572468u)) {
        return 1;
    }

    const uint32_t src_handle_b = bindless_array->AllocateBuffer(src_b->GetView());
    if (!run_case(1u, src_b, src_handle_b, 0x4000u, 5u, 11u, 0x89ABCDEFu)) {
        return 1;
    }

    LOG_INFO(
        MOER_TEXT("Bindless buffer readback test passed, handles=({}, {})"),
        src_handle_a,
        src_handle_b
    );
    return 0;
}

int RunBindlessTextureReadbackTest() {
    auto& device = RenderDevice::Get();

    auto bindless_array = device.CreateBindlessArray();
    auto output = device.CreateBuffer<uint32_t>(
        "bindless_texture_readback_output",
        8u,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );

    auto pipeline = ShaderManager::Get().Compute<BindlessTextureReadbackPipeline>(
        "tests/BindlessTextureReadback.comp.hlsl"
    );

    std::vector<uint32_t> readback_values(8u, 0u);

    const auto readback_and_validate = [&](const std::vector<uint32_t>& expected,
                                           const char*                 label) -> bool {
        std::fill(readback_values.begin(), readback_values.end(), 0u);

        CommandList readback_cmd(EQueueType::Graphics);
        GraphEventRef readback_event =
            readback_cmd.ReadbackCopy(output->GetView(), ToByteSpan(readback_values));

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(readback_cmd));
        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
        if (readback_event) {
            readback_event->Wait();
        }

        for (size_t i = 0; i < expected.size(); ++i) {
            if (readback_values[i] != expected[i]) {
                LOG_ERROR(
                    MOER_TEXT("{} mismatch at index={}, expected={}, got={}"),
                    label,
                    i,
                    expected[i],
                    readback_values[i]
                );
                return false;
            }
        }
        return true;
    };

    auto mip_texture = device.CreateTexture(
        "bindless_texture_mip",
        Extent2D(2u, 2u),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::SAMPLED,
        2u
    );
    auto mip0_data = MakeSolidRgba8(2u, 2u, 64u);
    auto mip1_data = MakeSolidRgba8(1u, 1u, 192u);
    const uint32_t mip_handle = bindless_array->AllocateTexture(
        mip_texture->GetView(0u, 2u),
        Sampler{SF_NEAREST, SAM_CLAMP_TO_EDGE}
    );

    {
        CommandList cmd(EQueueType::Graphics);
        cmd.CopyFrom(ToByteSpan(mip0_data), mip_texture->GetView(0u));
        cmd.CopyFrom(ToByteSpan(mip1_data), mip_texture->GetView(1u));
        cmd.ClearResource(output->GetView(), 0u);
        cmd.UpdateBindlessArray(bindless_array);

        BindlessTextureReadbackArgs args{
            .handle0 = mip_handle,
            .handle1 = mip_handle,
            .output_offset = 0u,
            .sample_count = 2u,
            .uv0_x = 0.5f,
            .uv0_y = 0.5f,
            .uv1_x = 0.5f,
            .uv1_y = 0.5f,
            .mip0 = 0.0f,
            .mip1 = 1.0f,
        };

        cmd.Compute(pipeline, args, output->GetView(), bindless_array)
            .Dispatch(1u, "BindlessTextureMipDispatch");

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(cmd));
        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    }

    if (!readback_and_validate({64u, 192u}, "BindlessTextureMip")) {
        return 1;
    }

    auto sampler_texture = device.CreateTexture(
        "bindless_texture_sampler",
        Extent2D(2u, 1u),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::SAMPLED
    );
    std::vector<uint8_t> sampler_data{
        0u, 0u, 0u, 255u,
        255u, 0u, 0u, 255u,
    };
    const uint32_t sampler_handle_clamp = bindless_array->AllocateTexture(
        sampler_texture->GetView(),
        Sampler{SF_NEAREST, SAM_CLAMP_TO_EDGE}
    );
    const uint32_t sampler_handle_repeat = bindless_array->AllocateTexture(
        sampler_texture->GetView(),
        Sampler{SF_NEAREST, SAM_REPEAT}
    );

    {
        CommandList cmd(EQueueType::Graphics);
        cmd.CopyFrom(ToByteSpan(sampler_data), sampler_texture->GetView());
        cmd.ClearResource(output->GetView(), 0u);
        cmd.UpdateBindlessArray(bindless_array);

        BindlessTextureReadbackArgs args{
            .handle0 = sampler_handle_clamp,
            .handle1 = sampler_handle_repeat,
            .output_offset = 0u,
            .sample_count = 2u,
            .uv0_x = 1.25f,
            .uv0_y = 0.5f,
            .uv1_x = 1.25f,
            .uv1_y = 0.5f,
            .mip0 = 0.0f,
            .mip1 = 0.0f,
        };

        cmd.Compute(pipeline, args, output->GetView(), bindless_array)
            .Dispatch(1u, "BindlessTextureSamplerDispatch");

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(cmd));
        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    }

    if (!readback_and_validate({255u, 0u}, "BindlessTextureSampler")) {
        return 1;
    }

    auto update_texture_a = device.CreateTexture(
        "bindless_texture_update_a",
        Extent2D(1u, 1u),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::SAMPLED
    );
    auto update_texture_b = device.CreateTexture(
        "bindless_texture_update_b",
        Extent2D(1u, 1u),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::SAMPLED
    );
    auto update_data_a = MakeSolidRgba8(1u, 1u, 32u);
    auto update_data_b = MakeSolidRgba8(1u, 1u, 224u);
    const Sampler update_sampler{SF_NEAREST, SAM_CLAMP_TO_EDGE};
    const uint32_t update_handle =
        bindless_array->AllocateTexture(update_texture_a->GetView(), update_sampler);
    uint32_t rebound_handle = 0u;

    {
        CommandList cmd(EQueueType::Graphics);
        cmd.CopyFrom(ToByteSpan(update_data_a), update_texture_a->GetView());
        cmd.CopyFrom(ToByteSpan(update_data_b), update_texture_b->GetView());
        cmd.ClearResource(output->GetView(), 0u);
        cmd.UpdateBindlessArray(bindless_array);

        BindlessTextureReadbackArgs dispatch_a_args{
            .handle0 = update_handle,
            .handle1 = update_handle,
            .output_offset = 0u,
            .sample_count = 1u,
            .uv0_x = 0.5f,
            .uv0_y = 0.5f,
            .uv1_x = 0.5f,
            .uv1_y = 0.5f,
            .mip0 = 0.0f,
            .mip1 = 0.0f,
        };
        cmd.Compute(pipeline, dispatch_a_args, output->GetView(), bindless_array)
            .Dispatch(1u, "BindlessTextureUpdateDispatchA");

        rebound_handle = bindless_array->AllocateTexture(update_texture_b->GetView(), update_sampler);

        cmd.UpdateBindlessArray(bindless_array);

        BindlessTextureReadbackArgs dispatch_b_args{
            .handle0 = rebound_handle,
            .handle1 = rebound_handle,
            .output_offset = 1u,
            .sample_count = 1u,
            .uv0_x = 0.5f,
            .uv0_y = 0.5f,
            .uv1_x = 0.5f,
            .uv1_y = 0.5f,
            .mip0 = 0.0f,
            .mip1 = 0.0f,
        };
        cmd.Compute(pipeline, dispatch_b_args, output->GetView(), bindless_array)
            .Dispatch(1u, "BindlessTextureUpdateDispatchB");

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(cmd));
        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    }

    if (!readback_and_validate({32u, 224u}, "BindlessTextureUpdate")) {
        return 1;
    }

    LOG_INFO(
        MOER_TEXT("Bindless texture readback test passed, mip_handle={}, sampler_handles=({}, {}), update_handle={}"),
        mip_handle,
        sampler_handle_clamp,
        sampler_handle_repeat,
        rebound_handle
    );
    return 0;
}

int RunMultiBindlessArrayReadbackTest() {
    auto& device = RenderDevice::Get();

    auto bindless_array_a = device.CreateBindlessArray();
    auto bindless_array_b = device.CreateBindlessArray();
    auto output = device.CreateBuffer<uint32_t>(
        "multi_bindless_array_readback_output",
        4u,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );

    auto pipeline = ShaderManager::Get().Compute<BindlessTextureReadbackPipeline>(
        "tests/BindlessTextureReadback.comp.hlsl"
    );

    auto texture_a = device.CreateTexture(
        "multi_bindless_array_texture_a",
        Extent2D(1u, 1u),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::SAMPLED
    );
    auto texture_b = device.CreateTexture(
        "multi_bindless_array_texture_b",
        Extent2D(1u, 1u),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::SAMPLED
    );

    auto data_a = MakeSolidRgba8(1u, 1u, 11u);
    auto data_b = MakeSolidRgba8(1u, 1u, 203u);
    const Sampler sampler{SF_NEAREST, SAM_CLAMP_TO_EDGE};

    const uint32_t handle_a = bindless_array_a->AllocateTexture(texture_a->GetView(), sampler);
    const uint32_t handle_b = bindless_array_b->AllocateTexture(texture_b->GetView(), sampler);

    if (handle_a != handle_b) {
        LOG_ERROR(
            MOER_TEXT("Multi-bindless-array test requires identical local handles, got handle_a={}, handle_b={}"),
            handle_a,
            handle_b
        );
        return 1;
    }

    {
        CommandList cmd(EQueueType::Graphics);
        cmd.CopyFrom(ToByteSpan(data_a), texture_a->GetView());
        cmd.CopyFrom(ToByteSpan(data_b), texture_b->GetView());
        cmd.ClearResource(output->GetView(), 0u);

        BindlessTextureReadbackArgs args_a{
            .handle0 = handle_a,
            .handle1 = handle_a,
            .output_offset = 0u,
            .sample_count = 1u,
            .uv0_x = 0.5f,
            .uv0_y = 0.5f,
            .uv1_x = 0.5f,
            .uv1_y = 0.5f,
            .mip0 = 0.0f,
            .mip1 = 0.0f,
        };
        BindlessTextureReadbackArgs args_b{
            .handle0 = handle_b,
            .handle1 = handle_b,
            .output_offset = 1u,
            .sample_count = 1u,
            .uv0_x = 0.5f,
            .uv0_y = 0.5f,
            .uv1_x = 0.5f,
            .uv1_y = 0.5f,
            .mip0 = 0.0f,
            .mip1 = 0.0f,
        };
        BindlessTextureReadbackArgs args_a_again = args_a;
        args_a_again.output_offset = 2u;

        cmd.UpdateBindlessArray(bindless_array_a);
        cmd.Compute(pipeline, args_a, output->GetView(), bindless_array_a)
            .Dispatch(1u, "MultiBindlessArrayDispatchA");

        cmd.UpdateBindlessArray(bindless_array_b);
        cmd.Compute(pipeline, args_b, output->GetView(), bindless_array_b)
            .Dispatch(1u, "MultiBindlessArrayDispatchB");

        cmd.UpdateBindlessArray(bindless_array_a);
        cmd.Compute(pipeline, args_a_again, output->GetView(), bindless_array_a)
            .Dispatch(1u, "MultiBindlessArrayDispatchARepeat");

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(cmd));
        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    }

    std::vector<uint32_t> readback_values(4u, 0u);
    {
        CommandList readback_cmd(EQueueType::Graphics);
        GraphEventRef readback_event =
            readback_cmd.ReadbackCopy(output->GetView(), ToByteSpan(readback_values));

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(readback_cmd));
        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
        if (readback_event) {
            readback_event->Wait();
        }
    }

    constexpr std::array<uint32_t, 3> expected{11u, 203u, 11u};
    for (size_t i = 0; i < expected.size(); ++i) {
        if (readback_values[i] != expected[i]) {
            LOG_ERROR(
                MOER_TEXT("MultiBindlessArray mismatch at index={}, expected={}, got={}"),
                i,
                expected[i],
                readback_values[i]
            );
            return 1;
        }
    }

    LOG_INFO(
        MOER_TEXT("Multi-bindless-array readback test passed, shared_local_handle={}, results=({}, {}, {})"),
        handle_a,
        readback_values[0],
        readback_values[1],
        readback_values[2]
    );
    return 0;
}

int RunMultiCopyScopeOrderingTest() {
    auto& device = RenderDevice::Get();
    auto buffer_a = device.CreateBuffer<uint32_t>(
        "copyscope_multi_scope_a",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto buffer_b = device.CreateBuffer<uint32_t>(
        "copyscope_multi_scope_b",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::UNORDERED_ACCESS
    );

    std::vector<uint32_t> values_a(kElementCount);
    std::vector<uint32_t> values_b(kElementCount);
    std::vector<uint32_t> readback_a(kElementCount, 0u);
    std::vector<uint32_t> readback_b(kElementCount, 0u);

    for (uint32_t i = 0; i < kElementCount; ++i) {
        values_a[i] = i * 7u + 1u;
        values_b[i] = i * 11u + 5u;
    }

    CommandList graphics_cmd(EQueueType::Graphics);
    {
        auto copy_scope = graphics_cmd.BeginCopyScope();
        copy_scope.CopyFrom(ToByteSpan(values_a), buffer_a->GetView());
    }
    GraphEventRef readback_a_event = graphics_cmd.ReadbackCopy(buffer_a->GetView(), ToByteSpan(readback_a));
    {
        auto copy_scope = graphics_cmd.BeginCopyScope();
        copy_scope.CopyFrom(ToByteSpan(values_b), buffer_b->GetView());
    }
    GraphEventRef readback_b_event = graphics_cmd.ReadbackCopy(buffer_b->GetView(), ToByteSpan(readback_b));

    Array<CommandList> frame_cmds{};
    frame_cmds.emplace_back(std::move(graphics_cmd));
    SubmitAndWait(std::move(frame_cmds));
    if (readback_a_event) {
        readback_a_event->Wait();
    }
    if (readback_b_event) {
        readback_b_event->Wait();
    }

    if (!ValidateResult(0u, values_a, readback_a)) {
        return 1;
    }
    if (!ValidateResult(1u, values_b, readback_b)) {
        return 1;
    }

    LOG_INFO(MOER_TEXT("Multi-CopyScope ordering test passed"));
    return 0;
}

int RunCopyScopeUnknownFirstUseTest() {
    auto& device = RenderDevice::Get();
    auto buffer = device.CreateBuffer<uint32_t>(
        "copyscope_unknown_first_use",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC
    );

    std::vector<uint32_t> upload_values(kElementCount);
    std::vector<uint32_t> readback_values(kElementCount, 0u);
    for (uint32_t i = 0; i < kElementCount; ++i) {
        upload_values[i] = 0xABC000u + i;
    }

    CommandList graphics_cmd(EQueueType::Graphics);
    {
        auto copy_scope = graphics_cmd.BeginCopyScope();
        copy_scope.CopyFrom(ToByteSpan(upload_values), buffer->GetView());
    }
    GraphEventRef readback_event = graphics_cmd.ReadbackCopy(buffer->GetView(), ToByteSpan(readback_values));

    Array<CommandList> frame_cmds{};
    frame_cmds.emplace_back(std::move(graphics_cmd));
    SubmitAndWait(std::move(frame_cmds));
    if (readback_event) {
        readback_event->Wait();
    }

    if (!ValidateResult(0u, upload_values, readback_values)) {
        return 1;
    }

    LOG_INFO(MOER_TEXT("CopyScope unknown-first-use test passed"));
    return 0;
}

int RunGpuEventStreamHierarchyTest() {
    auto& device = RenderDevice::Get();
    GPUEventStream::Get().ResetForTesting();

    auto buffer = device.CreateBuffer<uint32_t>(
        "gpu_event_stream_buffer",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC
    );

    std::vector<uint32_t> readback_values(kElementCount, 0u);
    GraphEventRef readback_event{nullptr};

    CommandList graphics_cmd(EQueueType::Graphics);
    {
        GPU_PROFILE_EVENT_SCOPE(graphics_cmd, "FrameOuter");
        graphics_cmd.ClearResource(buffer->GetView(), 0x11u);
        {
            GPU_PROFILE_EVENT_SCOPE(graphics_cmd, "FrameInner");
            graphics_cmd.ClearResource(buffer->GetView(), 0x55u);
        }
    }
    readback_event =
        graphics_cmd.ReadbackCopy(buffer->GetView(), ToByteSpan(readback_values));
    graphics_cmd.TickFrame();

    Array<CommandList> frame_cmds{};
    frame_cmds.emplace_back(std::move(graphics_cmd));
    RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    if (readback_event) {
        readback_event->Wait();
    }
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (!ValidateUniformValue(0u, 0x55u, readback_values)) {
        return 1;
    }

    const std::string frame_text = GPUEventStream::Get().FormatLastResolvedFrame();
    if (frame_text.empty()) {
        LOG_ERROR(MOER_TEXT("GPUEventStream did not resolve any frame profile text"));
        return 1;
    }

    const std::array<std::string_view, 7> required_tokens{
        "Frame ",
        "valid=true",
        "Queue Graphics",
        "GPU [queue=Graphics",
        "FrameOuter",
        "FrameInner",
        "exclusive_ns=",
    };
    for (std::string_view token : required_tokens) {
        if (frame_text.find(token) == std::string::npos) {
            LOG_ERROR(MOER_TEXT("GPUEventStream frame text missing token '{}':\n{}"), token, frame_text);
            return 1;
        }
    }

    const size_t outer_pos = frame_text.find("FrameOuter");
    const size_t inner_pos = frame_text.find("FrameInner");
    if (outer_pos == std::string::npos || inner_pos == std::string::npos || outer_pos >= inner_pos) {
        LOG_ERROR(MOER_TEXT("GPUEventStream frame hierarchy order is invalid:\n{}"), frame_text);
        return 1;
    }

    LOG_INFO(MOER_TEXT("GPUEventStream frame debug:\n{}"), frame_text);
    return 0;
}

int RunGpuEventStreamCrossSubmitAggregationTest() {
    auto& stream = GPUEventStream::Get();
    stream.ResetForTesting();

    Array<GPUEvent> first_submit{};
    first_submit.emplace_back(MakeResolvedGpuEvent(GPUEvent::EType::BeginGPU, "GPU", 0, 100));
    first_submit.emplace_back(MakeResolvedGpuEvent(GPUEvent::EType::BeginEvent, "FrameOuter", 1, 110));
    stream.InjectResolvedSubmitForTesting(std::move(first_submit), EQueueType::Graphics);

    Array<GPUEvent> second_submit{};
    second_submit.emplace_back(MakeResolvedGpuEvent(GPUEvent::EType::BeginEvent, "FrameInner", 2, 120));
    second_submit.emplace_back(MakeResolvedGpuEvent(GPUEvent::EType::EndEvent, "", 2, 170));
    second_submit.emplace_back(MakeResolvedGpuEvent(GPUEvent::EType::EndEvent, "", 1, 190));
    second_submit.emplace_back(MakeResolvedGpuEvent(GPUEvent::EType::EndGPU, "", 0, 200));
    second_submit.emplace_back(MakeResolvedGpuEvent(GPUEvent::EType::FrameBoundary, "FrameBoundary", 0, 210));
    stream.InjectResolvedSubmitForTesting(std::move(second_submit), EQueueType::Graphics);
    stream.EndFrame();

    const std::string frame_text = stream.FormatLastResolvedFrame();
    if (frame_text.empty()) {
        LOG_ERROR(MOER_TEXT("GPUEventStream cross-submit aggregation did not resolve any frame text"));
        return 1;
    }

    const std::array<std::string_view, 5> required_tokens{
        "valid=true",
        "boundary_ns=210",
        "FrameOuter [queue=Graphics, start_ns=110, end_ns=190, total_busy_ns=80, exclusive_ns=30]",
        "FrameInner [queue=Graphics, start_ns=120, end_ns=170, total_busy_ns=50, exclusive_ns=50]",
        "GPU [queue=Graphics, start_ns=100, end_ns=200, total_busy_ns=100, exclusive_ns=20]",
    };
    for (std::string_view token : required_tokens) {
        if (frame_text.find(token) == std::string::npos) {
            LOG_ERROR(MOER_TEXT("GPUEventStream cross-submit frame text missing token '{}':\n{}"), token, frame_text);
            return 1;
        }
    }

    LOG_INFO(MOER_TEXT("GPUEventStream cross-submit frame debug:\n{}"), frame_text);
    return 0;
}

int RunGpuEventStreamBoundaryValidationTest() {
    auto& stream = GPUEventStream::Get();
    stream.ResetForTesting();

    Array<GPUEvent> first_submit{};
    first_submit.emplace_back(MakeResolvedGpuEvent(GPUEvent::EType::BeginGPU, "GPU", 0, 300));
    first_submit.emplace_back(MakeResolvedGpuEvent(GPUEvent::EType::BeginEvent, "CrossBoundary", 1, 320));
    stream.InjectResolvedSubmitForTesting(std::move(first_submit), EQueueType::Graphics);

    Array<GPUEvent> second_submit{};
    second_submit.emplace_back(MakeResolvedGpuEvent(GPUEvent::EType::FrameBoundary, "FrameBoundary", 0, 360));
    second_submit.emplace_back(MakeResolvedGpuEvent(GPUEvent::EType::EndEvent, "", 1, 390));
    second_submit.emplace_back(MakeResolvedGpuEvent(GPUEvent::EType::EndGPU, "", 0, 420));
    stream.InjectResolvedSubmitForTesting(std::move(second_submit), EQueueType::Graphics);
    stream.EndFrame();

    const std::string frame_text = stream.FormatLastResolvedFrame();
    if (frame_text.empty()) {
        LOG_ERROR(MOER_TEXT("GPUEventStream boundary validation did not resolve any frame text"));
        return 1;
    }
    if (frame_text.find("valid=false") == std::string::npos) {
        LOG_ERROR(MOER_TEXT("GPUEventStream boundary validation should produce an invalid frame:\n{}"), frame_text);
        return 1;
    }
    if (frame_text.find("Queue Graphics") != std::string::npos) {
        LOG_ERROR(MOER_TEXT("GPUEventStream invalid frame should not materialize queue roots:\n{}"), frame_text);
        return 1;
    }

    LOG_INFO(MOER_TEXT("GPUEventStream invalid frame debug:\n{}"), frame_text);
    return 0;
}

int RunPresentWithCopyScopeTests() {
    auto& device = RenderDevice::Get();
    auto* window = WindowContext::GetMainWindow();
    if (window == nullptr) {
        LOG_ERROR(MOER_TEXT("CopyScope present test window is null."));
        return 1;
    }

    constexpr uint32_t kWidth  = 640;
    constexpr uint32_t kHeight = 360;

    SwapchainCreateInfo swapchain_ci{
        .surface = Moer::WindowContext::CreateSwapchainSurfaceInfo(*window),
        .size = {kWidth, kHeight},
        .back_buffer_sz = 2,
        .preferred_format = PF_R8G8B8A8_SRGB
    };
    SwapchainRef swapchain = device.CreateSwapchain(swapchain_ci);
    TextureRef   output    = device.CreateTexture(
        "translate_present_copyscope_output",
        Extent2D(kWidth, kHeight),
        swapchain->format,
        ETextureUsageFlags::TRANSFER_SRC | ETextureUsageFlags::TRANSFER_DST
    );

    std::vector<uint32_t> upload_values(kWidth * kHeight, 0u);
    std::vector<uint32_t> readback_values(kWidth * kHeight, 0u);

    for (uint32_t iter = 0; iter < kPresentIterations; ++iter) {
        WindowContext::Tick();

        std::fill(readback_values.begin(), readback_values.end(), 0u);
        std::fill(upload_values.begin(), upload_values.end(), 0xFF000000u | (iter * 131u + 23u));

        CommandList graphics_cmd(EQueueType::Graphics);
        {
            auto copy_scope = graphics_cmd.BeginCopyScope();
            copy_scope.CopyFrom(ToByteSpan(upload_values), output->GetView());
        }
        GraphEventRef readback_event =
            graphics_cmd.ReadbackCopy(output->GetView(), ToByteSpan(readback_values));
        graphics_cmd.TickFrame();

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(graphics_cmd));

        RHIPresentRequest present_request{swapchain, output->GetView()};
        RHIExecutor::Get().Submit(
            std::move(frame_cmds),
            ERHIExecSubmitFlags::FlushGPU,
            &present_request
        );
        if (readback_event) {
            readback_event->Wait();
        }
        RHIExecutor::Get().Sync(ERHISyncDepth::Present);

        if (upload_values != readback_values) {
            LOG_ERROR(MOER_TEXT("CopyScope present readback mismatch at iter={}"), iter);
            return 1;
        }
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    LOG_INFO(MOER_TEXT("Present + CopyScope test passed, iterations={}"), kPresentIterations);
    return 0;
}

int RunPresentTests() {
    auto& device = RenderDevice::Get();
    auto* window = WindowContext::GetMainWindow();
    if (window == nullptr) {
        LOG_ERROR(MOER_TEXT("Present test window is null."));
        return 1;
    }

    constexpr uint32_t kWidth  = 640;
    constexpr uint32_t kHeight = 360;

    SwapchainCreateInfo swapchain_ci{
        .surface = Moer::WindowContext::CreateSwapchainSurfaceInfo(*window),
        .size = {kWidth, kHeight},
        .back_buffer_sz = 2,
        .preferred_format = PF_R8G8B8A8_SRGB
    };
    SwapchainRef swapchain = device.CreateSwapchain(swapchain_ci);
    TextureRef   output    = device.CreateTexture(
        "translate_present_output",
        Extent2D(kWidth, kHeight),
        swapchain->format,
        ETextureUsageFlags::TRANSFER_SRC | ETextureUsageFlags::TRANSFER_DST
    );

    auto graphics_buffer = device.CreateBuffer<uint32_t>(
        "translate_present_graphics_buf",
        kElementCount,
        EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST
    );
    std::vector<uint32_t> readback_values(kElementCount, 0u);
    std::vector<uint32_t> output_values(kWidth * kHeight, 0u);

    for (uint32_t iter = 0; iter < kPresentIterations; ++iter) {
        WindowContext::Tick();

        for (uint32_t i = 0; i < kElementCount; ++i) {
            readback_values[i] = 0u;
        }
        std::fill(output_values.begin(), output_values.end(), 0xFF000000u | (iter * 97u + 17u));

        CommandList graphics_cmd(EQueueType::Graphics);
        graphics_cmd.ClearResource(graphics_buffer->GetView(), iter + 1u);
        GraphEventRef readback_event =
            graphics_cmd.ReadbackCopy(graphics_buffer->GetView(), ToByteSpan(readback_values));
        graphics_cmd.CopyFrom(ToByteSpan(output_values), output->GetView());
        graphics_cmd.TickFrame();

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(graphics_cmd));

        RHIPresentRequest present_request{swapchain, output->GetView()};
        RHIExecutor::Get().Submit(
            std::move(frame_cmds),
            ERHIExecSubmitFlags::FlushGPU,
            &present_request
        );
        if (readback_event) {
            readback_event->Wait();
        }

        if (!ValidateUniformValue(iter, iter + 1u, readback_values)) {
            return 1;
        }
    }

    auto* vk_swapchain = static_cast<VkSwapchain*>(swapchain.Get());
    if (vk_swapchain == nullptr) {
        LOG_ERROR(MOER_TEXT("Present test failed to resolve Vulkan swapchain implementation."));
        return 1;
    }

    const uint64_t present_only_before = vk_swapchain->image_idx;
    RHIPresentRequest present_only_request{swapchain, output->GetView()};
    Array<CommandList> present_only_cmds{};
    RHIExecutor::Get().Submit(
        std::move(present_only_cmds),
        ERHIExecSubmitFlags::FlushGPU,
        &present_only_request
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    if (vk_swapchain->image_idx != present_only_before + 1u) {
        LOG_ERROR(
            MOER_TEXT("Present-only submit did not advance swapchain image index: before={}, after={}"),
            present_only_before,
            vk_swapchain->image_idx
        );
        return 1;
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    LOG_INFO(MOER_TEXT("RHI translate present tests passed, iterations={}"), kPresentIterations);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path path = (argc > 0) ? std::filesystem::path(argv[0]) : std::filesystem::current_path();
    if (path.extension() == ".exe") {
        path = path.parent_path();
    }

    Moer::ConfigManager::GetInstance().Init(path);
    Moer::LogSystem::Init();
    Moer::TaskSystem::Init();
    Moer::Diagnostics::SetEnsureFailureEscalation(true);
    Moer::Diagnostics::ResetEnsureFailures();

    DeviceInitInfo info{
        .rhi_type = ERHIType::Vulkan,
        .name = "TestRHITranslate",
        .rhi_api_version = "1.3",
    };

    if (info.rhi_type != ERHIType::Vulkan) {
        std::cout << "SKIP (Vulkan-only)" << std::endl;
        return 0;
    }

    bool window_inited = false;
    RenderDevice::Init(std::move(info));
    ShaderCompiler::Init();

    auto shutdown_and_return = [&](int code) {
        int exit_code = code;
        if (exit_code == 0 && Moer::Diagnostics::HasEnsureFailures()) {
            LOG_ERROR(MOER_TEXT("TestRHITranslate observed escalated ensure failures."));
            exit_code = 1;
        }
        if (window_inited) {
            WindowContext::ShutDown();
            window_inited = false;
        }
        ShaderManager::ShutDown();
        ShutdownRHIForTest();
        Moer::TaskSystem::ShutDown();
        return exit_code;
    };

    auto& vk_device = static_cast<VulkanDevice&>(*RenderDevice::Get().GetImpl());
    if (!vk_device.HasDescriptorHeapRuntime()) {
        LOG_INFO(
            MOER_TEXT("[TESTCASE][SKIP] DescriptorHeapRuntimeUnsupported :: VK_EXT_descriptor_heap is unavailable")
        );
        return shutdown_and_return(0);
    }

    const int queue_ret = RunNamedTestCase("CommandListQueueBinding", RunCommandListQueueBindingTest);
    if (queue_ret != 0) {
        return shutdown_and_return(queue_ret);
    }

    const int translate_metadata_ret = RunNamedTestCase(
        "TranslateExecutionMetadataRoundTrip",
        RunTranslateExecutionClassRoundTripTest
    );
    if (translate_metadata_ret != 0) {
        return shutdown_and_return(translate_metadata_ret);
    }

    const int translate_lambda_ret =
        RunNamedTestCase("TranslateLambdaCommand", RunTranslateLambdaCommandTest);
    if (translate_lambda_ret != 0) {
        return shutdown_and_return(translate_lambda_ret);
    }

    const int translate_readback_ret =
        RunNamedTestCase("MultiQueueReadback", RunRHITranslateMultiQueueReadbackTest);
    if (translate_readback_ret != 0) {
        return shutdown_and_return(translate_readback_ret);
    }

    const int multi_cmd_order_ret =
        RunNamedTestCase("MultiCommandListSubmitOrdering", RunMultiCommandListSubmitOrderingTest);
    if (multi_cmd_order_ret != 0) {
        return shutdown_and_return(multi_cmd_order_ret);
    }

    const int serial_control_ret = RunNamedTestCase(
        "SerialControlTranslateOrdering",
        RunSerialControlTranslateOrderingTest
    );
    if (serial_control_ret != 0) {
        return shutdown_and_return(serial_control_ret);
    }

    const int descriptor_heap_ret = RunNamedTestCase(
        "ConcurrentDescriptorRangeAllocation",
        RunDescriptorHeapConcurrentRangeAllocationTest
    );
    if (descriptor_heap_ret != 0) {
        return shutdown_and_return(descriptor_heap_ret);
    }

    const int graphics_copyscope_ret =
        RunNamedTestCase("GraphicsCopyScopeRoundTrip", RunGraphicsCopyScopeRoundTripTest);
    if (graphics_copyscope_ret != 0) {
        return shutdown_and_return(graphics_copyscope_ret);
    }

    const int bindless_readback_ret =
        RunNamedTestCase("BindlessBufferReadback", RunBindlessBufferReadbackTest);
    if (bindless_readback_ret != 0) {
        return shutdown_and_return(bindless_readback_ret);
    }

    const int bindless_texture_readback_ret =
        RunNamedTestCase("BindlessTextureReadback", RunBindlessTextureReadbackTest);
    if (bindless_texture_readback_ret != 0) {
        return shutdown_and_return(bindless_texture_readback_ret);
    }

    const int multi_bindless_array_ret =
        RunNamedTestCase("MultiBindlessArrayReadback", RunMultiBindlessArrayReadbackTest);
    if (multi_bindless_array_ret != 0) {
        return shutdown_and_return(multi_bindless_array_ret);
    }

    const int multi_scope_ret =
        RunNamedTestCase("MultiCopyScopeOrdering", RunMultiCopyScopeOrderingTest);
    if (multi_scope_ret != 0) {
        return shutdown_and_return(multi_scope_ret);
    }

    const int unknown_first_use_ret =
        RunNamedTestCase("CopyScopeUnknownFirstUse", RunCopyScopeUnknownFirstUseTest);
    if (unknown_first_use_ret != 0) {
        return shutdown_and_return(unknown_first_use_ret);
    }

    const int gpu_event_stream_ret =
        RunNamedTestCase("GPUEventStreamHierarchy", RunGpuEventStreamHierarchyTest);
    if (gpu_event_stream_ret != 0) {
        return shutdown_and_return(gpu_event_stream_ret);
    }

    const int gpu_event_cross_submit_ret =
        RunNamedTestCase("GPUEventStreamCrossSubmitAggregation", RunGpuEventStreamCrossSubmitAggregationTest);
    if (gpu_event_cross_submit_ret != 0) {
        return shutdown_and_return(gpu_event_cross_submit_ret);
    }

    const int gpu_event_boundary_validation_ret =
        RunNamedTestCase("GPUEventStreamBoundaryValidation", RunGpuEventStreamBoundaryValidationTest);
    if (gpu_event_boundary_validation_ret != 0) {
        return shutdown_and_return(gpu_event_boundary_validation_ret);
    }

    const int profile_dump_cvar_ret =
        RunNamedTestCase("ProfileDumpStartupReadOnlyCVar", RunProfileDumpStartupReadOnlyCVarTest);
    if (profile_dump_cvar_ret != 0) {
        return shutdown_and_return(profile_dump_cvar_ret);
    }

    const int profile_dump_file_ret =
        RunNamedTestCase("ProfileDumpFileSinkFlush", RunProfileDumpFileSinkTest);
    if (profile_dump_file_ret != 0) {
        return shutdown_and_return(profile_dump_file_ret);
    }

    const int profile_dump_tcp_ret =
        RunNamedTestCase("ProfileDumpTcpSinkConsumerParse", RunProfileDumpTcpSinkTest);
    if (profile_dump_tcp_ret != 0) {
        return shutdown_and_return(profile_dump_tcp_ret);
    }

    const int profile_dump_gpu_ret =
        RunNamedTestCase("ProfileDumpGpuEventIntegration", RunGpuEventStreamProfileDumpTest);
    if (profile_dump_gpu_ret != 0) {
        return shutdown_and_return(profile_dump_gpu_ret);
    }

    const int profile_consumer_file_ret =
        RunNamedTestCase("ProfileConsumerFileLoadNormalization", RunProfileConsumerFileLoadTest);
    if (profile_consumer_file_ret != 0) {
        return shutdown_and_return(profile_consumer_file_ret);
    }

    const int profile_consumer_stream_ret =
        RunNamedTestCase("ProfileConsumerStreamNormalization", RunProfileConsumerStreamNormalizationTest);
    if (profile_consumer_stream_ret != 0) {
        return shutdown_and_return(profile_consumer_stream_ret);
    }

#if defined(MOER_TEST_WITH_PROFILE)
    const int profile_dump_flame_ret =
        RunNamedTestCase("ProfileDumpFlameProfilerIntegration", RunProfileDumpFlameProfilerProfileDumpTest);
    if (profile_dump_flame_ret != 0) {
        return shutdown_and_return(profile_dump_flame_ret);
    }
#endif

    WindowContext::Init(SurfaceInitInfo(640, 360, "TestRHITranslatePresent", false));
    window_inited = true;
    const int present_copyscope_ret =
        RunNamedTestCase("PresentWithCopyScope", RunPresentWithCopyScopeTests);
    if (present_copyscope_ret != 0) {
        return shutdown_and_return(present_copyscope_ret);
    }
    const int present_ret = RunNamedTestCase("PresentRoundTrip", RunPresentTests);
    if (present_ret != 0) {
        return shutdown_and_return(present_ret);
    }
    return shutdown_and_return(0);
}
