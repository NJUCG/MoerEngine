#include "trace/Trace.h"

#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "network/Socket.h"
#include "taskgraph/ThreadManager.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <format>
#include <mutex>
#include <thread>
#include <type_traits>

namespace Moer::Trace {
namespace {

using SteadyClock = std::chrono::steady_clock;

static constexpr uint16_t kPacketTypeMetadata = 1;
static constexpr uint16_t kPacketTypeEvents   = 2;

void SenderMain();

class TraceSenderRunnable : public Runnable {
public:
    uint32_t Run() override {
        SenderMain();
        return 0;
    }

    void Init() override {}
    void Stop() override {}
    void Exit() override {}
    ThreadIndex GetIndex() override {
        return EThread::UNKNOWN_THREAD;
    }
};

struct TraceRuntimeState {
    std::atomic<bool> initialized{false};
    std::atomic<bool> running{false};   // worker thread lifecycle
    std::atomic<bool> recording{false}; // capture on/off
    std::atomic<bool> connected{false};

    Config config{};

    uint64_t session_id{0};
    uint64_t session_start_ts_ns{0};

    std::mutex queue_mutex{};
    std::condition_variable queue_cv{};
    std::deque<TraceEvent> pending_events{};
    std::atomic<uint64_t> dropped_events{0};
    std::atomic<uint64_t> event_id_seed{1};
    std::atomic<uint64_t> span_id_seed{1};

    struct PendingSpan {
        uint64_t    ts_begin_ns{0};
        std::string name{};
        std::string category{};
        TrackType   track_type{TrackType::CPUThread};
        uint64_t    track_id{0};
        uint32_t    depth{0};
        bool        auto_depth{false};
        std::string track_name{};
        std::string args{};
    };
    std::mutex pending_spans_mutex{};
    UnorderedMap<uint64_t, PendingSpan> pending_spans{};

    std::mutex thread_name_mutex{};
    UnorderedMap<uint64_t, std::string> track_names{};

    TraceSenderRunnable* sender_runnable{nullptr};
    RunnableThread*      sender_thread{nullptr};

    std::mutex csv_mutex{};
    std::ofstream csv_file{};
    bool csv_header_written{false};
    bool csv_path_auto_generated{false};
};

TraceRuntimeState& G() {
    static TraceRuntimeState g{};
    return g;
}

uint64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch()).count();
}

std::string BuildDefaultCsvPath() {
    const std::filesystem::path trace_dir = ConfigManager::GetInstance().GetWorkspacePath() / "trace";
    std::filesystem::create_directories(trace_dir);

    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto tt  = clock::to_time_t(now);
    std::tm    tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif

    const int millis = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000
    );

    char base_name[128]{};
    std::snprintf(
        base_name,
        sizeof(base_name),
        "trace_stream_%04d%02d%02d_%02d%02d%02d_%03d",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec,
        millis
    );

    std::filesystem::path candidate = trace_dir / (std::string(base_name) + ".csv");
    for (uint32_t suffix = 1; std::filesystem::exists(candidate); ++suffix) {
        char unique_name[160]{};
        std::snprintf(unique_name, sizeof(unique_name), "%s_%u", base_name, suffix);
        candidate = trace_dir / (std::string(unique_name) + ".csv");
    }
    return candidate.string();
}

bool EnsureCsvFileOpenLocked(TraceRuntimeState& state) {
    if (!state.config.enable_csv) {
        return false;
    }
    if (state.csv_file.is_open()) {
        return true;
    }

    std::filesystem::path csv_path = state.config.csv_path;
    if (csv_path.empty()) {
        csv_path = BuildDefaultCsvPath();
        state.config.csv_path = csv_path.string();
        state.csv_path_auto_generated = true;
    }

    std::filesystem::create_directories(csv_path.parent_path());
    const bool has_existing_content =
        std::filesystem::exists(csv_path) && std::filesystem::file_size(csv_path) > 0;
    state.csv_file.open(csv_path, std::ios::out | std::ios::app);
    state.csv_header_written = has_existing_content;
    if (!state.csv_file.is_open()) {
        LOG_WARNING(MOER_TEXT("Trace CSV output cannot be opened: {}"), csv_path.string());
        return false;
    }
    return true;
}

void CloseCsvFileLocked(TraceRuntimeState& state) {
    if (!state.csv_file.is_open()) {
        return;
    }
    state.csv_file.flush();
    state.csv_file.close();
}

uint64_t MakeSessionId(uint64_t start_ts_ns) {
    return start_ts_ns ^ uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

uint32_t HashBytes(const std::byte* data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint8_t>(data[i]);
        hash *= 16777619u;
    }
    return hash;
}

template<typename T>
void WritePod(Array<std::byte>& out, const T& value) {
    const auto* ptr = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), ptr, ptr + sizeof(T));
}

void WriteString(Array<std::byte>& out, const std::string& str) {
    const uint16_t len = static_cast<uint16_t>(std::min<size_t>(str.size(), UINT16_MAX));
    WritePod(out, len);
    const auto* ptr = reinterpret_cast<const std::byte*>(str.data());
    out.insert(out.end(), ptr, ptr + len);
}

template<typename T>
bool ReadPod(std::span<const std::byte> payload, size_t& offset, T& out) {
    if (offset + sizeof(T) > payload.size()) {
        return false;
    }
    std::memcpy(&out, payload.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

bool ReadString(std::span<const std::byte> payload, size_t& offset, std::string& out) {
    uint16_t len = 0;
    if (!ReadPod(payload, offset, len)) {
        return false;
    }
    if (offset + len > payload.size()) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(payload.data() + offset), len);
    offset += len;
    return true;
}

bool BuildPacket(uint16_t type, std::span<const std::byte> payload, Array<std::byte>& out_packet) {
    out_packet.clear();
    PacketHeader header{};
    header.type         = type;
    header.payload_size = static_cast<uint32_t>(payload.size());
    header.checksum     = HashBytes(payload.data(), payload.size());

    WritePod(out_packet, header);
    out_packet.insert(out_packet.end(), payload.begin(), payload.end());
    return true;
}

bool ParsePacket(
    std::span<const std::byte> packet,
    PacketHeader& out_header,
    std::span<const std::byte>& out_payload
) {
    if (packet.size() < sizeof(PacketHeader)) {
        return false;
    }
    std::memcpy(&out_header, packet.data(), sizeof(PacketHeader));
    if (out_header.magic != 0x4D525443 || out_header.version != 1) {
        return false;
    }
    if (packet.size() != sizeof(PacketHeader) + out_header.payload_size) {
        return false;
    }
    out_payload = packet.subspan(sizeof(PacketHeader), out_header.payload_size);
    if (HashBytes(out_payload.data(), out_payload.size()) != out_header.checksum) {
        return false;
    }
    return true;
}

void PushEvent(TraceEvent&& event) {
    auto& state = G();
    if (!state.running.load(std::memory_order_relaxed) ||
        !state.recording.load(std::memory_order_relaxed)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state.queue_mutex);
        if (state.pending_events.size() >= state.config.queue_limit) {
            state.pending_events.pop_front();
            state.dropped_events.fetch_add(1, std::memory_order_relaxed);
        }
        state.pending_events.emplace_back(std::move(event));
    }
    state.queue_cv.notify_one();
}

void WriteCsvRow(const TraceEvent& event) {
    auto& state = G();
    if (!state.config.enable_csv || !state.recording.load(std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(state.csv_mutex);
    if (!EnsureCsvFileOpenLocked(state)) {
        return;
    }
    if (!state.csv_header_written) {
        state.csv_file << MOER_ASCII_TEXT("#moer_trace_csv_version=1\n")
                       << MOER_ASCII_TEXT("#session_id=") << state.session_id << MOER_ASCII_TEXT("\n")
                       << MOER_ASCII_TEXT("#time_origin_ns=") << state.session_start_ts_ns << MOER_ASCII_TEXT("\n")
                       << MOER_ASCII_TEXT("#session_name=") << state.config.session_name << MOER_ASCII_TEXT("\n");
        state.csv_file
            << MOER_ASCII_TEXT("event_id,session_id,type,track_type,track_id,depth,ts_begin_ns,ts_end_ns,counter,name,")
               MOER_ASCII_TEXT("category,track_name,args\n");
        state.csv_header_written = true;
    }
    state.csv_file << event.event_id << "," << event.session_id << "," << static_cast<uint32_t>(event.type)
                   << "," << static_cast<uint32_t>(event.track_type) << "," << event.track_id << ","
                   << event.depth << "," << event.ts_begin_ns << "," << event.ts_end_ns << ","
                   << event.counter_value << "," << "\"" << event.name << "\"" << "," << "\""
                   << event.category << "\"" << "," << "\"" << event.track_name << "\"" << "," << "\""
                   << event.args << "\"\n";
    state.csv_file.flush();
}

void SenderMain() {
    auto& state = G();
    Network::TcpSocket socket{};
    bool metadata_sent = false;

    auto close_socket = [&]() {
        socket.Close();
        metadata_sent = false;
        state.connected.store(false, std::memory_order_release);
    };

    auto connect_server = [&]() -> bool {
        close_socket();
        if (socket.Connect(Utf8StringView(state.config.host.data(), state.config.host.size()), state.config.port) !=
            Network::ESocketStatus::Success) {
            return false;
        }
        state.connected.store(true, std::memory_order_release);
        return true;
    };

    auto send_packet = [&](const auto& packet) {
        const size_t byte_size = packet.size() * sizeof(typename std::remove_reference_t<decltype(packet)>::value_type);
        return socket.SendAll(std::span<const std::byte>(reinterpret_cast<const std::byte*>(packet.data()), byte_size)) ==
               Network::ESocketStatus::Success;
    };

    while (state.running.load(std::memory_order_acquire)) {
        if (!state.config.enable_streaming) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (!state.recording.load(std::memory_order_relaxed)) {
            if (socket.IsOpen()) {
                close_socket();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        if (!socket.IsOpen() && !connect_server()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            continue;
        }

        if (!metadata_sent) {
            SessionMetadata metadata{};
            metadata.session_id   = state.session_id;
            metadata.session_name = state.config.session_name;
            metadata.start_ts_ns  = state.session_start_ts_ns;

            Array<std::byte> packet{};
            if (!SerializeSessionMetadataPacket(metadata, packet)) {
                close_socket();
                continue;
            }
            if (!send_packet(packet)) {
                close_socket();
                continue;
            }
            metadata_sent = true;
        }

        Array<TraceEvent> batch{};
        {
            std::unique_lock<std::mutex> lock(state.queue_mutex);
            if (state.pending_events.empty()) {
                state.queue_cv.wait_for(lock, std::chrono::milliseconds(16));
            }
            if (state.pending_events.empty()) {
                continue;
            }

            const size_t batch_count = std::min<size_t>(1024, state.pending_events.size());
            batch.reserve(batch_count);
            for (size_t i = 0; i < batch_count; ++i) {
                batch.emplace_back(std::move(state.pending_events.front()));
                state.pending_events.pop_front();
            }
        }

        Array<std::byte> packet{};
        if (!SerializeEventsPacket(batch, packet)) {
            continue;
        }
        if (!send_packet(packet)) {
            close_socket();
            continue;
        }
    }

    close_socket();
}

std::string DefaultCpuTrackName() {
    const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return std::format("Thread {}", tid);
}

void EnsureTrackName(uint64_t track_id, std::string_view preferred_name) {
    auto& state = G();
    std::lock_guard<std::mutex> lock(state.thread_name_mutex);
    if (state.track_names.find(track_id) != state.track_names.end()) {
        return;
    }
    if (!preferred_name.empty()) {
        state.track_names.emplace(track_id, preferred_name);
    } else {
        state.track_names.emplace(track_id, DefaultCpuTrackName());
    }
}

std::string GetTrackName(uint64_t track_id) {
    auto& state = G();
    std::lock_guard<std::mutex> lock(state.thread_name_mutex);
    auto it = state.track_names.find(track_id);
    if (it != state.track_names.end()) {
        return it->second;
    }
    return DefaultCpuTrackName();
}

thread_local UnorderedMap<uint64_t, uint32_t> tls_scope_depth_per_track{};

} // namespace

bool Init(const Config& config) {
#if !MOER_TRACE_ENABLED
    (void)config;
    return false;
#else
    auto& state = G();
    if (state.initialized.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }

    state.config = config;
    state.session_start_ts_ns = NowNs();
    state.session_id          = MakeSessionId(state.session_start_ts_ns);
    state.running.store(true, std::memory_order_release);
    state.recording.store(config.start_recording, std::memory_order_release);
    state.connected.store(false, std::memory_order_release);

    if (state.config.enable_csv && state.recording.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(state.csv_mutex);
        EnsureCsvFileOpenLocked(state);
    }

    state.sender_runnable = MoerNew(TraceSenderRunnable)();
    state.sender_thread   = RunnableThread::Create(
        state.sender_runnable,
        ThreadAttributes{.affinity = Affinity{}, .name = MOER_ASCII_TEXT("TraceSenderThread")}
    );
    return true;
#endif
}

void Shutdown() {
#if !MOER_TRACE_ENABLED
    return;
#else
    auto& state = G();
    if (!state.initialized.load(std::memory_order_acquire)) {
        return;
    }
    state.running.store(false, std::memory_order_release);
    state.recording.store(false, std::memory_order_release);
    state.queue_cv.notify_all();

    if (state.sender_thread != nullptr) {
        MoerDelete(state.sender_thread);
        state.sender_thread = nullptr;
    }
    if (state.sender_runnable != nullptr) {
        MoerDelete(state.sender_runnable);
        state.sender_runnable = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(state.csv_mutex);
        CloseCsvFileLocked(state);
    }

    {
        std::lock_guard<std::mutex> lock(state.queue_mutex);
        state.pending_events.clear();
    }
    {
        std::lock_guard<std::mutex> lock(state.pending_spans_mutex);
        state.pending_spans.clear();
    }
    state.initialized.store(false, std::memory_order_release);
#endif
}

void StartRecording() {
#if !MOER_TRACE_ENABLED
    return;
#else
    auto& state = G();
    if (!state.initialized.load(std::memory_order_acquire)) {
        return;
    }
    if (state.recording.load(std::memory_order_acquire)) {
        return;
    }
    state.session_start_ts_ns = NowNs();
    state.session_id          = MakeSessionId(state.session_start_ts_ns);
    {
        std::lock_guard<std::mutex> lock(state.csv_mutex);
        CloseCsvFileLocked(state);
        state.csv_header_written = false;
        if (state.csv_path_auto_generated) {
            state.config.csv_path.clear();
            state.csv_path_auto_generated = false;
        }
        EnsureCsvFileOpenLocked(state);
    }
    state.recording.store(true, std::memory_order_release);
#endif
}

void StopRecording() {
#if !MOER_TRACE_ENABLED
    return;
#else
    auto& state = G();
    if (!state.initialized.load(std::memory_order_acquire)) {
        return;
    }
    state.recording.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(state.csv_mutex);
        CloseCsvFileLocked(state);
    }
#endif
}

bool IsRecording() {
#if !MOER_TRACE_ENABLED
    return false;
#else
    return G().recording.load(std::memory_order_relaxed);
#endif
}

void SetThreadName(Utf8StringView thread_name) {
#if !MOER_TRACE_ENABLED
    (void)thread_name;
    return;
#else
    const uint64_t track_id = DefaultCpuTrackId();
    auto&          state    = G();
    std::lock_guard<std::mutex> lock(state.thread_name_mutex);
    state.track_names[track_id] = std::string(thread_name.data(), thread_name.size());
#endif
}

uint64_t DefaultCpuTrackId() {
    return uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

uint64_t MakeGpuQueueTrackId(uint32_t gpu_index, uint32_t queue_type) {
    return (uint64_t(gpu_index) << 32u) | uint64_t(queue_type);
}

SpanToken BeginSpan(const SpanDesc& desc) {
#if !MOER_TRACE_ENABLED
    (void)desc;
    return {};
#else
    auto& state = G();
    if (!state.running.load(std::memory_order_relaxed)) {
        return {};
    }

    SpanToken token{};
    token.valid   = state.recording.load(std::memory_order_relaxed);
    if (!token.valid) {
        return token;
    }
    token.span_id = state.span_id_seed.fetch_add(1, std::memory_order_relaxed);

    TraceRuntimeState::PendingSpan span{};
    span.ts_begin_ns = NowNs();
    span.name        = std::string(desc.name);
    span.category    = std::string(desc.category);
    span.track_type  = desc.track_type;
    span.track_id    = desc.track_id == 0 ? DefaultCpuTrackId() : desc.track_id;
    span.depth       = desc.depth;
    span.auto_depth  = false;
    if (span.track_type == TrackType::CPUThread && desc.depth == 0) {
        uint32_t& depth = tls_scope_depth_per_track[span.track_id];
        span.depth      = depth;
        span.auto_depth = true;
        ++depth;
    }
    span.track_name  = std::string(desc.track_name);
    span.args        = std::string(desc.args);
    EnsureTrackName(span.track_id, span.track_name);

    {
        std::lock_guard<std::mutex> lock(state.pending_spans_mutex);
        state.pending_spans.emplace(token.span_id, std::move(span));
    }
    return token;
#endif
}

void EndSpan(SpanToken&& token, std::string_view end_args) {
#if !MOER_TRACE_ENABLED
    (void)token;
    (void)end_args;
    return;
#else
    if (!token.valid) {
        return;
    }

    auto& state = G();
    TraceRuntimeState::PendingSpan span{};
    {
        std::lock_guard<std::mutex> lock(state.pending_spans_mutex);
        const auto it = state.pending_spans.find(token.span_id);
        if (it == state.pending_spans.end()) {
            return;
        }
        span = std::move(it->second);
        state.pending_spans.erase(it);
    }

    TraceEvent event{};
    event.event_id     = state.event_id_seed.fetch_add(1, std::memory_order_relaxed);
    event.session_id   = state.session_id;
    event.type         = EventType::Scope;
    event.track_type   = span.track_type;
    event.track_id     = span.track_id;
    event.depth        = span.depth;
    event.ts_begin_ns  = span.ts_begin_ns;
    event.ts_end_ns    = NowNs();
    event.name         = std::move(span.name);
    event.category     = std::move(span.category);
    event.track_name   = span.track_name.empty() ? GetTrackName(span.track_id) : std::move(span.track_name);
    event.args         = std::move(span.args);
    if (!end_args.empty()) {
        if (!event.args.empty()) {
            event.args += "; ";
        }
        event.args += std::string(end_args);
    }
    if (span.auto_depth && span.track_type == TrackType::CPUThread) {
        auto it = tls_scope_depth_per_track.find(span.track_id);
        if (it != tls_scope_depth_per_track.end() && it->second > 0) {
            --it->second;
        }
    }
    WriteCsvRow(event);
    PushEvent(std::move(event));
#endif
}

void EmitScope(const EmitScopeDesc& desc) {
#if !MOER_TRACE_ENABLED
    (void)desc;
    return;
#else
    if (!G().recording.load(std::memory_order_relaxed)) {
        return;
    }
    TraceEvent event{};
    event.event_id      = G().event_id_seed.fetch_add(1, std::memory_order_relaxed);
    event.session_id    = G().session_id;
    event.type          = EventType::Scope;
    event.track_type    = desc.track_type;
    event.track_id      = desc.track_id == 0 ? DefaultCpuTrackId() : desc.track_id;
    event.depth         = desc.depth;
    event.ts_begin_ns   = desc.ts_begin_ns;
    event.ts_end_ns     = desc.ts_end_ns;
    event.name          = std::string(desc.name);
    event.category      = std::string(desc.category);
    event.track_name    = std::string(desc.track_name);
    event.args          = std::string(desc.args);
    if (event.track_name.empty()) {
        event.track_name = GetTrackName(event.track_id);
    } else {
        EnsureTrackName(event.track_id, event.track_name);
    }
    WriteCsvRow(event);
    PushEvent(std::move(event));
#endif
}

void EmitInstant(std::string_view name, std::string_view category, std::string_view args) {
#if !MOER_TRACE_ENABLED
    (void)name;
    (void)category;
    (void)args;
    return;
#else
    const uint64_t now = NowNs();
    EmitScope(
        EmitScopeDesc{
            .name       = name,
            .category   = category,
            .track_type = TrackType::CPUThread,
            .track_id   = DefaultCpuTrackId(),
            .depth      = 0,
            .ts_begin_ns = now,
            .ts_end_ns   = now,
            .track_name = {},
            .args       = args
        }
    );
#endif
}

void EmitCounter(std::string_view name, double value, std::string_view category, std::string_view args) {
#if !MOER_TRACE_ENABLED
    (void)name;
    (void)value;
    (void)category;
    (void)args;
    return;
#else
    if (!G().recording.load(std::memory_order_relaxed)) {
        return;
    }
    TraceEvent event{};
    event.event_id      = G().event_id_seed.fetch_add(1, std::memory_order_relaxed);
    event.session_id    = G().session_id;
    event.type          = EventType::Counter;
    event.track_type    = TrackType::CPUThread;
    event.track_id      = DefaultCpuTrackId();
    event.depth         = 0;
    event.ts_begin_ns   = NowNs();
    event.ts_end_ns     = event.ts_begin_ns;
    event.counter_value = value;
    event.name          = std::string(name);
    event.category      = std::string(category);
    event.track_name    = GetTrackName(event.track_id);
    event.args          = std::string(args);
    WriteCsvRow(event);
    PushEvent(std::move(event));
#endif
}

void EnableCsvExport(std::string_view csv_path) {
#if !MOER_TRACE_ENABLED
    (void)csv_path;
    return;
#else
    auto& state            = G();
    std::lock_guard<std::mutex> lock(state.csv_mutex);
    state.config.enable_csv = true;
    state.config.csv_path   = std::string(csv_path);
    state.csv_path_auto_generated = false;
#endif
}

Utf8String GetCsvPath() {
#if !MOER_TRACE_ENABLED
    return {};
#else
    auto& state = G();
    std::lock_guard<std::mutex> lock(state.csv_mutex);
    return Utf8String(state.config.csv_path.c_str());
#endif
}

Stats GetStats() {
    auto& state = G();
    Stats stats{};
    stats.enabled        = MOER_TRACE_ENABLED != 0;
    stats.recording      = state.recording.load(std::memory_order_relaxed);
    stats.connected      = state.connected.load(std::memory_order_relaxed);
    stats.dropped_events = state.dropped_events.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(state.queue_mutex);
        stats.queued_events = static_cast<uint64_t>(state.pending_events.size());
    }
    return stats;
}

bool SerializeSessionMetadataPacket(const SessionMetadata& metadata, Array<std::byte>& out_packet) {
    Array<std::byte> payload{};
    WritePod(payload, metadata.session_id);
    WritePod(payload, metadata.start_ts_ns);
    WriteString(payload, metadata.session_name);
    return BuildPacket(kPacketTypeMetadata, payload, out_packet);
}

bool SerializeEventsPacket(const Array<TraceEvent>& events, Array<std::byte>& out_packet) {
    Array<std::byte> payload{};
    const uint32_t count = static_cast<uint32_t>(events.size());
    WritePod(payload, count);
    for (const TraceEvent& event : events) {
        WritePod(payload, event.event_id);
        WritePod(payload, event.session_id);
        WritePod(payload, static_cast<uint8_t>(event.type));
        WritePod(payload, static_cast<uint8_t>(event.track_type));
        WritePod(payload, event.track_id);
        WritePod(payload, event.depth);
        WritePod(payload, event.ts_begin_ns);
        WritePod(payload, event.ts_end_ns);
        WritePod(payload, event.counter_value);
        WriteString(payload, event.name);
        WriteString(payload, event.category);
        WriteString(payload, event.track_name);
        WriteString(payload, event.args);
    }
    return BuildPacket(kPacketTypeEvents, payload, out_packet);
}

bool DeserializeSessionMetadataPacket(
    const PacketHeader& header,
    std::span<const std::byte> payload,
    SessionMetadata& out_metadata
) {
    if (header.type != kPacketTypeMetadata) {
        return false;
    }
    if (header.payload_size != payload.size() || HashBytes(payload.data(), payload.size()) != header.checksum) {
        return false;
    }
    size_t offset = 0;
    if (!ReadPod(payload, offset, out_metadata.session_id)) {
        return false;
    }
    if (!ReadPod(payload, offset, out_metadata.start_ts_ns)) {
        return false;
    }
    if (!ReadString(payload, offset, out_metadata.session_name)) {
        return false;
    }
    return offset == payload.size();
}

bool DeserializeEventsPacket(
    const PacketHeader& header,
    std::span<const std::byte> payload,
    Array<TraceEvent>& out_events
) {
    if (header.type != kPacketTypeEvents) {
        return false;
    }
    if (header.payload_size != payload.size() || HashBytes(payload.data(), payload.size()) != header.checksum) {
        return false;
    }
    out_events.clear();
    size_t   offset = 0;
    uint32_t count  = 0;
    if (!ReadPod(payload, offset, count)) {
        return false;
    }
    out_events.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        TraceEvent event{};
        uint8_t    type_u8       = 0;
        uint8_t    track_type_u8 = 0;
        if (!ReadPod(payload, offset, event.event_id) || !ReadPod(payload, offset, event.session_id) ||
            !ReadPod(payload, offset, type_u8) || !ReadPod(payload, offset, track_type_u8) ||
            !ReadPod(payload, offset, event.track_id) || !ReadPod(payload, offset, event.depth) ||
            !ReadPod(payload, offset, event.ts_begin_ns) || !ReadPod(payload, offset, event.ts_end_ns) ||
            !ReadPod(payload, offset, event.counter_value) || !ReadString(payload, offset, event.name) ||
            !ReadString(payload, offset, event.category) || !ReadString(payload, offset, event.track_name) ||
            !ReadString(payload, offset, event.args)) {
            return false;
        }
        event.type       = static_cast<EventType>(type_u8);
        event.track_type = static_cast<TrackType>(track_type_u8);
        out_events.emplace_back(std::move(event));
    }
    return offset == payload.size();
}

Scope::Scope(std::string_view name, std::string_view category) :
    token_(BeginSpan(SpanDesc{.name = name, .category = category})) {}

Scope::Scope(const SpanDesc& desc) : token_(BeginSpan(desc)) {}

Scope::~Scope() {
    EndSpan(std::move(token_));
}

} // namespace Moer::Trace
