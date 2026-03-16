#include "Core.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "misc/Timer.h"
#include "renderer/common/UIRenderer.h"
#include "rhi/RHI.h"
#include "trace/Trace.h"
#include "window/WindowContext.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <nfd.hpp>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace Moer {
namespace {

using namespace Moer::Render;

struct TrackInfo {
    uint64_t         key{0};
    Trace::TrackType type{Trace::TrackType::CPUThread};
    uint64_t         id{0};
    std::string      name{"Unknown"};
    int              max_depth{0};
};

uint64_t MakeTrackKey(Trace::TrackType type, uint64_t id) {
    return (uint64_t(static_cast<uint8_t>(type)) << 56u) ^ id;
}

struct TraceStore {
    std::mutex                        mutex{};
    Trace::SessionMetadata            metadata{};
    Array<Trace::TraceEvent>          events{};
    UnorderedMap<uint64_t, TrackInfo> tracks{};
    uint64_t                          min_ts{0};
    uint64_t                          max_ts{0};
    uint64_t                          generation{1};

    void AppendEvents(const Array<Trace::TraceEvent>& in_events) {
        if (in_events.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        events.reserve(events.size() + in_events.size());
        for (const auto& e : in_events) {
            events.emplace_back(e);
            if (min_ts == 0 || e.ts_begin_ns < min_ts) {
                min_ts = e.ts_begin_ns;
            }
            if (e.ts_end_ns > max_ts) {
                max_ts = e.ts_end_ns;
            }

            const uint64_t track_key = MakeTrackKey(e.track_type, e.track_id);
            auto&          track     = tracks[track_key];
            track.key                = track_key;
            track.id                 = e.track_id;
            track.type               = e.track_type;
            if (!e.track_name.empty()) {
                track.name = e.track_name;
            } else if (track.name == "Unknown") {
                track.name = std::string("Track ") + std::to_string(e.track_id);
            }
            track.max_depth = std::max(track.max_depth, static_cast<int>(e.depth));
        }
        ++generation;
    }
};

class TraceIngestServer {
public:
    explicit TraceIngestServer(TraceStore& store) : store_(store) {}
    ~TraceIngestServer() {
        Stop();
    }

    bool Start(uint16_t port) {
#if !defined(_WIN32)
        (void)port;
        return false;
#else
        if (running_.exchange(true)) {
            return true;
        }
        port_ = port;

        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            running_.store(false);
            return false;
        }
        server_thread_ = std::thread([this]() { ServerMain(); });
        return true;
#endif
    }

    void Stop() {
#if defined(_WIN32)
        if (!running_.exchange(false)) {
            return;
        }
        if (listen_sock_ != INVALID_SOCKET) {
            closesocket(listen_sock_);
            listen_sock_ = INVALID_SOCKET;
        }
        if (client_sock_ != INVALID_SOCKET) {
            closesocket(client_sock_);
            client_sock_ = INVALID_SOCKET;
        }
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        WSACleanup();
#endif
    }

private:
#if defined(_WIN32)
    static bool RecvAll(SOCKET sock, std::byte* data, size_t len) {
        size_t recv_total = 0;
        while (recv_total < len) {
            const int ret =
                recv(sock, reinterpret_cast<char*>(data + recv_total), static_cast<int>(len - recv_total), 0);
            if (ret <= 0) {
                return false;
            }
            recv_total += static_cast<size_t>(ret);
        }
        return true;
    }

    void ServerMain() {
        listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock_ == INVALID_SOCKET) {
            return;
        }

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(port_);

        int yes = 1;
        setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
        if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return;
        }
        if (listen(listen_sock_, 1) != 0) {
            return;
        }

        while (running_.load()) {
            sockaddr_in client_addr{};
            int         client_len = sizeof(client_addr);
            SOCKET      sock =
                accept(listen_sock_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (sock == INVALID_SOCKET) {
                if (!running_.load()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            if (client_sock_ != INVALID_SOCKET) {
                closesocket(client_sock_);
            }
            client_sock_ = sock;
            ClientMain(sock);
            closesocket(sock);
            client_sock_ = INVALID_SOCKET;
        }
    }

    void ClientMain(SOCKET sock) {
        while (running_.load()) {
            Trace::PacketHeader header{};
            if (!RecvAll(sock, reinterpret_cast<std::byte*>(&header), sizeof(header))) {
                break;
            }
            if (header.magic != 0x4D525443 || header.version != 1) {
                break;
            }
            Array<std::byte> payload{};
            payload.resize(header.payload_size);
            if (header.payload_size > 0 &&
                !RecvAll(sock, payload.data(), payload.size())) {
                break;
            }

            if (header.type == 1) {
                Trace::SessionMetadata meta{};
                if (Trace::DeserializeSessionMetadataPacket(header, payload, meta)) {
                    std::lock_guard<std::mutex> lock(store_.mutex);
                    store_.metadata = std::move(meta);
                    ++store_.generation;
                }
            } else if (header.type == 2) {
                Array<Trace::TraceEvent> events{};
                if (Trace::DeserializeEventsPacket(header, payload, events)) {
                    store_.AppendEvents(events);
                }
            }
        }
    }
#endif

private:
    TraceStore&         store_;
    std::atomic<bool>   running_{false};
    uint16_t            port_{19090};
    std::thread         server_thread_{};
#if defined(_WIN32)
    SOCKET              listen_sock_{INVALID_SOCKET};
    SOCKET              client_sock_{INVALID_SOCKET};
#endif
};

bool ContainsCaseInsensitive(std::string_view text, std::string_view token) {
    if (token.empty()) {
        return true;
    }
    auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    for (size_t i = 0; i + token.size() <= text.size(); ++i) {
        bool matched = true;
        for (size_t j = 0; j < token.size(); ++j) {
            if (lower(text[i + j]) != lower(token[j])) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return true;
        }
    }
    return false;
}

ImU32 EventColorByName(std::string_view name) {
    const uint32_t h = static_cast<uint32_t>(std::hash<std::string_view>{}(name));
    // Keep saturation/value in a pleasant range and spread hue by hash.
    const float hue = (h % 360u) / 360.0f;
    const float sat = 0.45f + float((h >> 8u) & 0x1Fu) / 255.0f;
    const float val = 0.68f + float((h >> 16u) & 0x1Fu) / 255.0f;
    return ImColor::HSV(hue, std::clamp(sat, 0.35f, 0.62f), std::clamp(val, 0.62f, 0.88f), 0.92f);
}

uint64_t EstimateProfileSizeBytes(const TraceStore& store) {
    uint64_t bytes = static_cast<uint64_t>(store.events.size() * sizeof(Trace::TraceEvent));
    for (const auto& e : store.events) {
        bytes += static_cast<uint64_t>(e.name.size() + e.category.size() + e.track_name.size() + e.args.size());
    }
    bytes += static_cast<uint64_t>(store.tracks.size() * sizeof(TrackInfo));
    for (const auto& [id, t] : store.tracks) {
        (void)id;
        bytes += static_cast<uint64_t>(t.name.size());
    }
    bytes += static_cast<uint64_t>(store.metadata.session_name.size());
    return bytes;
}

uint64_t EstimateProfileSizeBytesFast(
    size_t event_count,
    size_t track_count,
    size_t session_name_size
) {
    uint64_t bytes = static_cast<uint64_t>(event_count * sizeof(Trace::TraceEvent));
    bytes += static_cast<uint64_t>(track_count * sizeof(TrackInfo));
    bytes += static_cast<uint64_t>(session_name_size);
    return bytes;
}

std::string FormatBytes(uint64_t bytes) {
    const double value = static_cast<double>(bytes);
    char         buf[64]{};
    if (value >= 1024.0 * 1024.0 * 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.2f GiB", value / (1024.0 * 1024.0 * 1024.0));
    } else if (value >= 1024.0 * 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.2f MiB", value / (1024.0 * 1024.0));
    } else if (value >= 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.2f KiB", value / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return std::string(buf);
}

Array<std::string> SplitCsv(const std::string& line) {
    Array<std::string> out{};
    std::string current{};
    bool        quoted = false;
    for (char c : line) {
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (c == ',' && !quoted) {
            out.emplace_back(std::move(current));
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    out.emplace_back(std::move(current));
    return out;
}

bool LoadCsvEvents(const std::filesystem::path& path, TraceStore& store, bool clear_before_load) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    Array<Trace::TraceEvent> parsed{};
    std::string              line{};
    bool                     header_skipped = false;
    while (std::getline(file, line)) {
        if (!header_skipped) {
            header_skipped = true;
            continue;
        }
        if (line.empty()) {
            continue;
        }
        Array<std::string> cols = SplitCsv(line);
        if (cols.size() < 13) {
            continue;
        }
        Trace::TraceEvent e{};
        try {
            e.event_id      = std::stoull(cols[0]);
            e.session_id    = std::stoull(cols[1]);
            e.type          = static_cast<Trace::EventType>(std::stoul(cols[2]));
            e.track_type    = static_cast<Trace::TrackType>(std::stoul(cols[3]));
            e.track_id      = std::stoull(cols[4]);
            e.depth         = static_cast<uint32_t>(std::stoul(cols[5]));
            e.ts_begin_ns   = std::stoull(cols[6]);
            e.ts_end_ns     = std::stoull(cols[7]);
            e.counter_value = std::stod(cols[8]);
            e.name          = cols[9];
            e.category      = cols[10];
            e.track_name    = cols[11];
            e.args          = cols[12];
        } catch (...) {
            continue;
        }
        parsed.emplace_back(std::move(e));
    }

    if (clear_before_load) {
        std::lock_guard<std::mutex> lock(store.mutex);
        store.events.clear();
        store.tracks.clear();
        store.min_ts = 0;
        store.max_ts = 0;
        ++store.generation;
    }
    store.AppendEvents(parsed);
    return true;
}

void DrawTimeRuler(ImDrawList* draw_list, const ImVec2& origin, float width, float y, uint64_t start_ns, uint64_t end_ns) {
    draw_list->AddLine({origin.x, y}, {origin.x + width, y}, IM_COL32(180, 180, 180, 255), 1.0f);

    const double span_ns = static_cast<double>(std::max<uint64_t>(1, end_ns - start_ns));
    const int    major_ticks = 10;
    for (int i = 0; i <= major_ticks; ++i) {
        const float  t = static_cast<float>(i) / static_cast<float>(major_ticks);
        const float  x = origin.x + t * width;
        const double ts_ns = static_cast<double>(start_ns) + t * span_ns;
        draw_list->AddLine({x, y}, {x, y + 8.0f}, IM_COL32(200, 200, 200, 255), 1.0f);

        char label[64]{};
        if (ts_ns >= 1e9) {
            std::snprintf(label, sizeof(label), "%.3fs", ts_ns / 1e9);
        } else if (ts_ns >= 1e6) {
            std::snprintf(label, sizeof(label), "%.3fms", ts_ns / 1e6);
        } else if (ts_ns >= 1e3) {
            std::snprintf(label, sizeof(label), "%.3fus", ts_ns / 1e3);
        } else {
            std::snprintf(label, sizeof(label), "%.0fns", ts_ns);
        }
        draw_list->AddText({x + 2.0f, y + 10.0f}, IM_COL32(200, 200, 200, 255), label);
    }
}

struct TimelineViewState {
    uint64_t view_start_ns{0};
    uint64_t view_end_ns{0};
    bool     auto_follow{true};
    char     search_text[128]{};
};

void DrawTimelinePanel(TraceStore& store, TimelineViewState& ui_state) {
    struct TimelineDataCache {
        uint64_t store_generation{0};
        Array<Trace::TraceEvent> events{};
        Array<TrackInfo> tracks{};
        uint64_t min_ts{0};
        uint64_t max_ts{0};
        UnorderedMap<uint64_t, Array<size_t>> scope_indices_by_track{};
        Array<uint32_t> gpu_display_depth_by_index{};
        UnorderedMap<uint64_t, int> gpu_display_max_depth_by_track{};
        UnorderedMap<uint64_t, size_t> event_index_by_id{};
        uint64_t search_generation{0};
        std::string cached_search{};
        Array<uint8_t> search_match_mask{};
        Array<size_t> matched_indices{};
    };

    static TimelineDataCache cache{};
    bool                     data_rebuilt = false;
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        if (cache.store_generation != store.generation) {
            cache.store_generation = store.generation;
            cache.events           = store.events;
            cache.tracks.clear();
            cache.tracks.reserve(store.tracks.size());
            for (const auto& [id, track] : store.tracks) {
                (void)id;
                cache.tracks.emplace_back(track);
            }
            std::sort(cache.tracks.begin(), cache.tracks.end(), [](const TrackInfo& a, const TrackInfo& b) {
                if (a.type != b.type) {
                    return static_cast<uint8_t>(a.type) < static_cast<uint8_t>(b.type);
                }
                return a.name < b.name;
            });
            cache.min_ts       = store.min_ts;
            cache.max_ts       = store.max_ts;
            cache.search_generation = 0;
            data_rebuilt       = true;
        }
    }

    if (data_rebuilt) {
        const auto& events = cache.events;
        cache.scope_indices_by_track.clear();
        cache.scope_indices_by_track.reserve(std::max<size_t>(8, cache.tracks.size()));
        cache.gpu_display_depth_by_index.assign(events.size(), 0);
        cache.gpu_display_max_depth_by_track.clear();
        cache.event_index_by_id.clear();
        cache.event_index_by_id.reserve(std::max<size_t>(8, events.size()));

        for (size_t i = 0; i < events.size(); ++i) {
            const auto& e = events[i];
            cache.event_index_by_id[e.event_id] = i;
            cache.gpu_display_depth_by_index[i] = e.depth;
            if (e.type != Trace::EventType::Scope) {
                continue;
            }
            const uint64_t track_key = MakeTrackKey(e.track_type, e.track_id);
            cache.scope_indices_by_track[track_key].emplace_back(i);
        }

        for (auto& [track_key, indices] : cache.scope_indices_by_track) {
            std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
                if (events[a].ts_begin_ns != events[b].ts_begin_ns) {
                    return events[a].ts_begin_ns < events[b].ts_begin_ns;
                }
                const uint64_t dur_a = events[a].ts_end_ns >= events[a].ts_begin_ns ?
                                           events[a].ts_end_ns - events[a].ts_begin_ns :
                                           0;
                const uint64_t dur_b = events[b].ts_end_ns >= events[b].ts_begin_ns ?
                                           events[b].ts_end_ns - events[b].ts_begin_ns :
                                           0;
                return dur_a > dur_b;
            });

            if (indices.empty() ||
                events[indices.front()].track_type != Trace::TrackType::GPUQueue) {
                continue;
            }

            Array<uint64_t> depth_end_ns{};
            for (size_t idx : indices) {
                const auto& e = events[idx];
                size_t      depth = 0;
                while (depth < depth_end_ns.size() && e.ts_begin_ns < depth_end_ns[depth]) {
                    ++depth;
                }
                if (depth == depth_end_ns.size()) {
                    depth_end_ns.emplace_back(e.ts_end_ns);
                } else {
                    depth_end_ns[depth] = e.ts_end_ns;
                }
                cache.gpu_display_depth_by_index[idx] = static_cast<uint32_t>(depth);
                auto& max_depth = cache.gpu_display_max_depth_by_track[track_key];
                max_depth = std::max(max_depth, static_cast<int>(depth));
            }
        }
    }

    const auto&   events = cache.events;
    const auto&   tracks = cache.tracks;
    const uint64_t min_ts = cache.min_ts;
    const uint64_t max_ts = cache.max_ts;
    if (events.empty()) {
        ImGui::Text("Waiting for trace stream on port 19090...");
        return;
    }

    const std::string search_key = ui_state.search_text;
    if (cache.search_generation != cache.store_generation || cache.cached_search != search_key) {
        cache.cached_search = search_key;
        cache.search_generation = cache.store_generation;
        cache.search_match_mask.assign(events.size(), 0);
        cache.matched_indices.clear();
        cache.matched_indices.reserve(events.size() / 16 + 8);
        for (size_t i = 0; i < events.size(); ++i) {
            const auto& e = events[i];
            if (e.type != Trace::EventType::Scope) {
                continue;
            }
            if (search_key.empty() || ContainsCaseInsensitive(e.name, search_key)) {
                cache.search_match_mask[i] = 1;
                if (!search_key.empty()) {
                    cache.matched_indices.emplace_back(i);
                }
            }
        }
    }

    static int      match_cursor = 0;
    static uint64_t selected_event_id = 0;
    const auto&     matched_indices = cache.matched_indices;
    const auto&     search_match_mask = cache.search_match_mask;
    if (!matched_indices.empty()) {
        if (match_cursor >= static_cast<int>(matched_indices.size())) {
            match_cursor = static_cast<int>(matched_indices.size()) - 1;
        }
        ImGui::Text("Matches: %d", static_cast<int>(matched_indices.size()));
        ImGui::SameLine();
        if (ImGui::Button("Prev")) {
            match_cursor = (match_cursor - 1 + static_cast<int>(matched_indices.size())) %
                           static_cast<int>(matched_indices.size());
            const auto& e = events[matched_indices[match_cursor]];
            selected_event_id = e.event_id;
            const uint64_t span = std::max<uint64_t>(1, ui_state.view_end_ns - ui_state.view_start_ns);
            ui_state.view_start_ns = e.ts_begin_ns > span / 2 ? e.ts_begin_ns - span / 2 : 0;
            ui_state.view_end_ns   = ui_state.view_start_ns + span;
            ui_state.auto_follow   = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Next")) {
            match_cursor = (match_cursor + 1) % static_cast<int>(matched_indices.size());
            const auto& e = events[matched_indices[match_cursor]];
            selected_event_id = e.event_id;
            const uint64_t span = std::max<uint64_t>(1, ui_state.view_end_ns - ui_state.view_start_ns);
            ui_state.view_start_ns = e.ts_begin_ns > span / 2 ? e.ts_begin_ns - span / 2 : 0;
            ui_state.view_end_ns   = ui_state.view_start_ns + span;
            ui_state.auto_follow   = false;
        }
        ImGui::Separator();
    }

    if (ui_state.auto_follow || ui_state.view_end_ns <= ui_state.view_start_ns) {
        ui_state.view_start_ns = min_ts;
        ui_state.view_end_ns   = max_ts + 1;
    }

    const float label_col_w  = 320.0f;
    const float top_header_h = 44.0f;
    const float lane_h       = 18.0f;
    const float track_gap_h  = 8.0f;

    static UnorderedMap<uint64_t, bool> track_visibility{};
    for (const auto& track : tracks) {
        if (!track_visibility.contains(track.key)) {
            track_visibility[track.key] = true;
        }
    }

    if (ImGui::Button("Visible Tracks")) {
        ImGui::OpenPopup("VisibleTracksPopup");
    }
    if (ImGui::BeginPopup("VisibleTracksPopup")) {
        for (const auto& track : tracks) {
            bool visible = track_visibility[track.key];
            const std::string label = std::string("[") +
                                      (track.type == Trace::TrackType::CPUThread ? "CPU" : "GPU") + "] " +
                                      track.name + "##vis_" + std::to_string(track.key);
            if (ImGui::Checkbox(label.c_str(), &visible)) {
                track_visibility[track.key] = visible;
            }
        }
        ImGui::EndPopup();
    }
    size_t visible_count = 0;
    for (const auto& [id, visible] : track_visibility) {
        (void)id;
        if (visible) {
            ++visible_count;
        }
    }
    ImGui::SameLine();
    ImGui::Text(
        "Visible: %llu / %llu",
        static_cast<unsigned long long>(visible_count),
        static_cast<unsigned long long>(tracks.size())
    );
    ImGui::Separator();

    struct VisibleTrackRow {
        TrackInfo info{};
        float     row_h{0.0f};
    };
    Array<VisibleTrackRow> visible_rows{};
    visible_rows.reserve(tracks.size());
    for (const auto& track : tracks) {
        bool visible = true;
        if (auto it = track_visibility.find(track.key); it != track_visibility.end()) {
            visible = it->second;
        }
        if (visible) {
            int row_max_depth = track.max_depth;
            if (auto it = cache.gpu_display_max_depth_by_track.find(track.key);
                it != cache.gpu_display_max_depth_by_track.end()) {
                row_max_depth = std::max(row_max_depth, it->second);
            }
            visible_rows.emplace_back(VisibleTrackRow{
                .info  = track,
                .row_h = lane_h * static_cast<float>(row_max_depth + 1) + track_gap_h
            });
        }
    }

    auto clamp_view_range = [&](uint64_t data_start, uint64_t data_end) {
        if (ui_state.view_end_ns <= ui_state.view_start_ns) {
            ui_state.view_start_ns = data_start;
            ui_state.view_end_ns   = data_end;
            return;
        }
        const uint64_t data_span = std::max<uint64_t>(1, data_end - data_start);
        uint64_t view_span = std::max<uint64_t>(1, ui_state.view_end_ns - ui_state.view_start_ns);
        if (view_span >= data_span) {
            ui_state.view_start_ns = data_start;
            ui_state.view_end_ns   = data_end;
            return;
        }
        if (ui_state.view_start_ns < data_start) {
            ui_state.view_start_ns = data_start;
            ui_state.view_end_ns   = data_start + view_span;
        }
        if (ui_state.view_end_ns > data_end) {
            ui_state.view_end_ns   = data_end;
            ui_state.view_start_ns = data_end - view_span;
        }
    };

    ImGui::BeginChild("TimelineMergedCanvas", {0.0f, 0.0f}, true, ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 canvas_origin = ImGui::GetCursorScreenPos();
    const float  canvas_w      = std::max(1000.0f, ImGui::GetContentRegionAvail().x);
    const float  timeline_x0   = canvas_origin.x + label_col_w;
    const float  timeline_w    = std::max(300.0f, canvas_w - label_col_w - 20.0f);
    const ImVec2 mouse_pos     = ImGui::GetMousePos();
    const bool   mouse_in_timeline =
        mouse_pos.x >= timeline_x0 && mouse_pos.x <= (timeline_x0 + timeline_w);

    if (ui_state.auto_follow || ui_state.view_end_ns <= ui_state.view_start_ns) {
        ui_state.view_start_ns = min_ts;
        ui_state.view_end_ns   = max_ts + 1;
    }
    clamp_view_range(min_ts, max_ts + 1);

    // Zoom + pan
    if (ImGui::IsWindowHovered()) {
        ImGuiIO& io = ImGui::GetIO();
        const uint64_t span = std::max<uint64_t>(1, ui_state.view_end_ns - ui_state.view_start_ns);

        if (io.MouseWheel != 0.0f && !io.KeyShift && mouse_in_timeline) {
            ui_state.auto_follow = false;
            const double zoom = io.MouseWheel > 0.0f ? 0.8 : 1.25;
            const uint64_t new_span =
                static_cast<uint64_t>(std::clamp(span * zoom, 1000.0, double(std::max<uint64_t>(1, (max_ts + 1) - min_ts))));
            const double mouse_t = std::clamp((io.MousePos.x - timeline_x0) / std::max(1.0f, timeline_w), 0.0f, 1.0f);
            const uint64_t anchor = ui_state.view_start_ns + static_cast<uint64_t>(mouse_t * double(span));
            uint64_t new_start = anchor > static_cast<uint64_t>(mouse_t * double(new_span)) ?
                                     anchor - static_cast<uint64_t>(mouse_t * double(new_span)) :
                                     0;
            ui_state.view_start_ns = new_start;
            ui_state.view_end_ns   = new_start + new_span;
            clamp_view_range(min_ts, max_ts + 1);
        }

        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) && mouse_in_timeline) {
            ui_state.auto_follow = false;
            const double delta_t = -double(io.MouseDelta.x) / std::max(1.0f, timeline_w);
            const int64_t shift_ns = static_cast<int64_t>(delta_t * double(span));
            int64_t new_start = static_cast<int64_t>(ui_state.view_start_ns) + shift_ns;
            int64_t new_end   = static_cast<int64_t>(ui_state.view_end_ns) + shift_ns;
            if (new_start < 0) {
                new_end -= new_start;
                new_start = 0;
            }
            ui_state.view_start_ns = static_cast<uint64_t>(new_start);
            ui_state.view_end_ns   = static_cast<uint64_t>(new_end);
            clamp_view_range(min_ts, max_ts + 1);
        }

        // Horizontal pan support: Shift + wheel and arrow keys
        if (io.MouseWheel != 0.0f && io.KeyShift && mouse_in_timeline) {
            ui_state.auto_follow = false;
            const int64_t shift_ns = static_cast<int64_t>(-io.MouseWheel * double(span) * 0.12);
            int64_t new_start = static_cast<int64_t>(ui_state.view_start_ns) + shift_ns;
            int64_t new_end   = static_cast<int64_t>(ui_state.view_end_ns) + shift_ns;
            if (new_start < 0) {
                new_end -= new_start;
                new_start = 0;
            }
            ui_state.view_start_ns = static_cast<uint64_t>(new_start);
            ui_state.view_end_ns   = static_cast<uint64_t>(new_end);
            clamp_view_range(min_ts, max_ts + 1);
        }
        if (ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow)) {
            ui_state.auto_follow = false;
            const int dir = ImGui::IsKeyDown(ImGuiKey_LeftArrow) ? -1 : 1;
            const int64_t shift_ns = static_cast<int64_t>(double(span) * 0.015 * dir);
            int64_t new_start = static_cast<int64_t>(ui_state.view_start_ns) + shift_ns;
            int64_t new_end   = static_cast<int64_t>(ui_state.view_end_ns) + shift_ns;
            if (new_start < 0) {
                new_end -= new_start;
                new_start = 0;
            }
            ui_state.view_start_ns = static_cast<uint64_t>(new_start);
            ui_state.view_end_ns   = static_cast<uint64_t>(new_end);
            clamp_view_range(min_ts, max_ts + 1);
        }
    }

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_F)) {
        auto it = cache.event_index_by_id.find(selected_event_id);
        if (it != cache.event_index_by_id.end()) {
            const auto&   e = events[it->second];
            const uint64_t event_span = std::max<uint64_t>(1, e.ts_end_ns - e.ts_begin_ns);
            const uint64_t margin     = std::max<uint64_t>(event_span / 5, 1000);
            ui_state.auto_follow      = false;
            ui_state.view_start_ns    = e.ts_begin_ns > margin ? e.ts_begin_ns - margin : 0;
            ui_state.view_end_ns      = e.ts_end_ns + margin;
            clamp_view_range(min_ts, max_ts + 1);
        }
    }

    draw_list->AddRectFilled(
        canvas_origin,
        {canvas_origin.x + canvas_w, canvas_origin.y + top_header_h},
        IM_COL32(20, 20, 20, 255)
    );
    draw_list->AddRectFilled(
        {canvas_origin.x, canvas_origin.y},
        {canvas_origin.x + label_col_w, canvas_origin.y + top_header_h},
        IM_COL32(34, 34, 34, 255)
    );
    draw_list->AddLine(
        {timeline_x0, canvas_origin.y},
        {timeline_x0, canvas_origin.y + top_header_h},
        IM_COL32(80, 80, 80, 255),
        1.0f
    );
    DrawTimeRuler(
        draw_list,
        {timeline_x0, canvas_origin.y},
        timeline_w,
        canvas_origin.y,
        ui_state.view_start_ns,
        ui_state.view_end_ns
    );
    ImGui::Dummy({canvas_w, top_header_h});

    const double range_ns = static_cast<double>(std::max<uint64_t>(1, ui_state.view_end_ns - ui_state.view_start_ns));
    auto to_x = [&](uint64_t ts) {
        const double t = (static_cast<double>(ts) - static_cast<double>(ui_state.view_start_ns)) / range_ns;
        return timeline_x0 + static_cast<float>(std::clamp(t, 0.0, 1.0) * timeline_w);
    };

    float y_cursor = canvas_origin.y + top_header_h;
    int   last_type_group = -1;
    const float group_header_h = 20.0f;
    for (const auto& row : visible_rows) {
        const auto& track  = row.info;
        const float track_h = row.row_h;
        const int   cur_type_group = track.type == Trace::TrackType::CPUThread ? 0 : 1;
        if (cur_type_group != last_type_group) {
            const char* group_label = cur_type_group == 0 ? "CPU Tracks" : "GPU Tracks";
            draw_list->AddRectFilled(
                {canvas_origin.x, y_cursor},
                {canvas_origin.x + canvas_w, y_cursor + group_header_h},
                IM_COL32(40, 40, 40, 255)
            );
            draw_list->AddText(
                {canvas_origin.x + 8.0f, y_cursor + 2.0f},
                IM_COL32(230, 230, 230, 255),
                group_label
            );
            y_cursor += group_header_h;
            ImGui::Dummy({1.0f, group_header_h});
            last_type_group = cur_type_group;
        }

        draw_list->AddRectFilled(
            {canvas_origin.x, y_cursor},
            {canvas_origin.x + label_col_w, y_cursor + track_h},
            IM_COL32(30, 30, 30, 255)
        );
        draw_list->AddRectFilled(
            {timeline_x0, y_cursor},
            {timeline_x0 + timeline_w, y_cursor + track_h},
            IM_COL32(24, 24, 24, 255)
        );
        draw_list->AddLine(
            {canvas_origin.x, y_cursor},
            {canvas_origin.x + canvas_w, y_cursor},
            IM_COL32(70, 70, 70, 255),
            1.0f
        );

        const std::string track_label = std::string("[") +
                                        (track.type == Trace::TrackType::CPUThread ? "CPU" : "GPU") + "] " +
                                        track.name;
        const float label_y = y_cursor + 3.0f;
        draw_list->AddText(
            {canvas_origin.x + 8.0f, label_y},
            IM_COL32(220, 220, 220, 255),
            track_label.c_str()
        );

        auto indices_it = cache.scope_indices_by_track.find(track.key);
        if (indices_it != cache.scope_indices_by_track.end()) {
            const auto& track_indices = indices_it->second;
            auto start_it = std::lower_bound(
                track_indices.begin(),
                track_indices.end(),
                ui_state.view_start_ns,
                [&](size_t idx, uint64_t ts) { return events[idx].ts_begin_ns < ts; }
            );
            size_t start_pos = static_cast<size_t>(start_it - track_indices.begin());
            while (start_pos > 0 && events[track_indices[start_pos - 1]].ts_end_ns >= ui_state.view_start_ns) {
                --start_pos;
            }

            for (size_t pos = start_pos; pos < track_indices.size(); ++pos) {
                const size_t      idx = track_indices[pos];
                const auto&       e   = events[idx];
                if (e.ts_begin_ns > ui_state.view_end_ns) {
                    break;
                }
                if (!search_match_mask.empty() && !search_match_mask[idx]) {
                    continue;
                }
                if (e.ts_end_ns < ui_state.view_start_ns || e.ts_begin_ns > ui_state.view_end_ns) {
                    continue;
                }
            float x0 = to_x(e.ts_begin_ns);
            float       x1 = to_x(e.ts_end_ns);
            if (x1 <= x0) {
                x1 = x0 + 1.0f;
            }
            x0 = std::max(x0, timeline_x0);
            x1 = std::min(x1, timeline_x0 + timeline_w);
            if (x1 <= x0) {
                continue;
            }
            uint32_t depth = e.depth;
            if (e.track_type == Trace::TrackType::GPUQueue) {
                    depth = cache.gpu_display_depth_by_index[idx];
            }
            const float y0 = y_cursor + lane_h * static_cast<float>(depth);
            const float y1 = y0 + lane_h - 2.0f;

            const ImU32 color = EventColorByName(e.name);

            draw_list->AddRectFilled({x0, y0}, {x1, y1}, color, 2.0f);
            if (e.event_id == selected_event_id) {
                draw_list->AddRect({x0, y0}, {x1, y1}, IM_COL32(255, 255, 0, 255), 2.0f, 0, 2.0f);
            }
            const float inner_w = x1 - x0 - 6.0f;
            const float inner_h = y1 - y0 - 2.0f;
            if (inner_w > 8.0f && inner_h > 7.0f) {
                float text_size = std::min(ImGui::GetFontSize(), inner_h);
                text_size = std::max(8.0f, text_size);
                ImVec2 text_sz = ImGui::CalcTextSize(e.name.c_str());
                if (text_sz.x > 0.0f && text_sz.x > inner_w) {
                    text_size = std::max(8.0f, text_size * (inner_w / text_sz.x));
                }
                const ImVec2 text_pos = {
                    x0 + 4.0f,
                    y0 + (y1 - y0 - text_size) * 0.5f
                };
                const ImVec4 clip4 = {x0 + 2.0f, y0, x1 - 2.0f, y1};
                draw_list->AddText(
                    ImGui::GetFont(),
                    text_size,
                    text_pos,
                    IM_COL32(230, 230, 230, 255),
                    e.name.c_str(),
                    nullptr,
                    0.0f,
                    &clip4
                );
            }

            if (ImGui::IsMouseHoveringRect({x0, y0}, {x1, y1})) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    selected_event_id = e.event_id;
                }
                std::string parent_name = "None";
                if (e.depth > 0) {
                    uint64_t best_begin = 0;
                        for (size_t candidate_idx : track_indices) {
                            const auto& candidate = events[candidate_idx];
                            if (candidate.depth + 1 != e.depth) {
                            continue;
                        }
                        if (candidate.ts_begin_ns <= e.ts_begin_ns && candidate.ts_end_ns >= e.ts_end_ns &&
                            candidate.ts_begin_ns >= best_begin) {
                            best_begin  = candidate.ts_begin_ns;
                            parent_name = candidate.name;
                        }
                    }
                }

                ImGui::BeginTooltip();
                ImGui::Text("Name: %s", e.name.c_str());
                ImGui::Text("Category: %s", e.category.c_str());
                ImGui::Text("Depth: %u", depth);
                ImGui::Text("Duration: %.3f ms", static_cast<double>(e.ts_end_ns - e.ts_begin_ns) / 1e6);
                ImGui::Text("Parent: %s", parent_name.c_str());
                ImGui::EndTooltip();
            }
        }
        }

        y_cursor += track_h;
        ImGui::Dummy({1.0f, track_h});
    }

    draw_list->AddLine(
        {canvas_origin.x, y_cursor},
        {canvas_origin.x + canvas_w, y_cursor},
        IM_COL32(70, 70, 70, 255),
        1.0f
    );
    ImGui::Dummy({canvas_w, std::max(200.0f, y_cursor - canvas_origin.y + 20.0f)});
    ImGui::EndChild();
}

} // namespace

int RunProfilerMain(int argc, const char** argv) {
    std::filesystem::path path = argv[0];
    path = path.filename().string().find(".exe") != std::string::npos ? path.parent_path() : path;
    LogSystem::Init();
    LOG_INFO("MoerProfiler starting...");
    ConfigManager::GetInstance().Init(path);
    TaskSystem::Init();

    RenderDevice::Init(
        DeviceInitInfo{
            .rhi_type        = ERHIType::Vulkan,
            .name            = "MoerProfiler",
            .rhi_api_version = "1.3",
        }
    );

    WindowContext::Init(SurfaceInitInfo(ERHIType::Vulkan, 1680, 980, "MoerProfiler", false));

    RenderDevice& device     = RenderDevice::Get();
    CommandQueue& gfx_queue  = device.GetCommandQueue(EQueueType::Graphics);
    FenceRef      timeline   = device.CreateFence();
    uint64_t      frame_time = 0;

    uint2 resolution = {1680, 980};
    SwapchainCreateInfo swapchain_ci{
        .window_handle    = (uintptr_t)WindowContext::GetMainWindow(),
        .size             = {resolution.x, resolution.y},
        .back_buffer_sz   = 2,
        .preferred_format = PF_R8G8B8A8_SRGB
    };
    SwapchainRef swapchain = device.CreateSwapchain(swapchain_ci);
    TextureRef   output =
        device.CreateTexture("ProfilerOutput", Extent2D(resolution.x, resolution.y), swapchain->format, ETextureUsageFlags::COLOR_ATTACHMENT);

    auto ui_renderer = MakeUnique<Render::UIRenderer>(device);

    TraceStore        store{};
    TraceIngestServer ingest(store);
    ingest.Start(19090);

    TimelineViewState timeline_state{};
    timeline_state.auto_follow = true;
    char csv_path[512]         = {};

    while (!WindowContext::ShouldClose(WindowContext::GetMainWindow())) {
        WindowContext::Tick();
        const uint64_t max_frames_in_flight =
            std::max<uint64_t>(1, static_cast<uint64_t>(swapchain_ci.back_buffer_sz));
        if (frame_time >= max_frames_in_flight) {
            timeline->Wait(frame_time - max_frames_in_flight + 1);
        }

        int w = 0, h = 0;
        WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &w, &h);
        if (w <= 0 || h <= 0) {
            std::this_thread::yield();
            continue;
        }
        if (resolution.x != static_cast<uint32_t>(w) || resolution.y != static_cast<uint32_t>(h)) {
            resolution = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
            gfx_queue.Sync();
            swapchain_ci.size = {resolution.x, resolution.y};
            swapchain->Sync();
            swapchain->Recreate(swapchain_ci);
            output = device.CreateTexture(
                "ProfilerOutput",
                Extent2D(resolution.x, resolution.y),
                swapchain->format,
                ETextureUsageFlags::COLOR_ATTACHMENT
            );
        }

        ui_renderer->BeginGUIFrame();
        {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::SetNextWindowViewport(viewport->ID);
        }
        ImGuiWindowFlags profiler_window_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                                 ImGuiWindowFlags_NoMove;
        ImGui::Begin("ProfilerTrace", nullptr, profiler_window_flags);
        ImGui::Checkbox("Auto Follow", &timeline_state.auto_follow);
        ImGui::SameLine();
        ImGui::InputTextWithHint("##search", "Search timescope name", timeline_state.search_text, sizeof(timeline_state.search_text));
        ImGui::Text("CSV: %s", csv_path[0] == '\0' ? "(none)" : csv_path);
        if (ImGui::Button("Browse CSV (Load)")) {
            NFD::UniquePath selected_path = nullptr;
            Array<nfdfilteritem_t> filters = {
                {"CSV", "csv"},
                {"All", "*"}
            };
            nfdresult_t result = NFD::OpenDialog(selected_path, filters.data(), filters.size());
            if (result == NFD_OKAY && selected_path) {
                std::snprintf(csv_path, sizeof(csv_path), "%s", selected_path.get());
                if (!LoadCsvEvents(csv_path, store, true)) {
                    LOG_WARNING("CSV load failed: {}", csv_path);
                }
            }
        }

        std::string ui_session_name{};
        size_t      ui_event_count = 0;
        size_t      ui_track_count = 0;
        uint64_t    ui_min_ts = 0;
        uint64_t    ui_max_ts = 0;
        {
            std::lock_guard<std::mutex> lock(store.mutex);
            ui_session_name = store.metadata.session_name;
            ui_event_count  = store.events.size();
            ui_track_count  = store.tracks.size();
            ui_min_ts       = store.min_ts;
            ui_max_ts       = store.max_ts;
        }
        ImGui::Text("Session: %s", ui_session_name.empty() ? "(none)" : ui_session_name.c_str());
        ImGui::Text("Events: %llu", static_cast<unsigned long long>(ui_event_count));
        const std::string profile_size =
            FormatBytes(EstimateProfileSizeBytesFast(ui_event_count, ui_track_count, ui_session_name.size()));
        ImGui::Text("Profile Size (approx): %s", profile_size.c_str());
        ImGui::Text(
            "Time Range: [%llu, %llu]",
            static_cast<unsigned long long>(ui_min_ts),
            static_cast<unsigned long long>(ui_max_ts)
        );
        ImGui::Separator();
        DrawTimelinePanel(store, timeline_state);
        ImGui::End();
        ui_renderer->EndGUIFrame();

        CommandList cmd_list{};
        ui_renderer->RenderGUI(cmd_list, output->GetView());

        ++frame_time;
        gfx_queue.Execute(cmd_list.Submit().Signal(timeline, frame_time).DeleteResources());
        gfx_queue.Present(swapchain, output->GetView());
        ui_renderer->PresentWindows();
    }

    ingest.Stop();
    gfx_queue.Sync();
    swapchain->Sync();
    device.WaitIdle();
    ui_renderer.reset();
    output = {};
    swapchain = {};
    timeline = {};
    WindowContext::ShutDown();
    RenderDevice::Dispose();
    TaskSystem::ShutDown();
    return 0;
}

} // namespace Moer

int main(int argc, const char** argv) {
    return Moer::RunProfilerMain(argc, argv);
}
