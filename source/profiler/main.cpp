#include "config/ConfigManager.h"
#include "file/FileDialog.h"
#include "log/LogSystem.h"
#include "network/Socket.h"
#include "ProfileSession.h"
#include "profile/ProfileDump.h"
#include "renderer/common/PresentationSurface.h"
#include "renderer/common/UIRenderer.h"
#include "renderer/common/ui/synapse/Synapse.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "window/WindowContext.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace Moer {
namespace {

using namespace Moer::Render;
using namespace Moer::Profiler;

static constexpr uint32_t trace_packet_magic = 0x4D525443u;

class ProfilerIngestServer {
public:
    explicit ProfilerIngestServer(ProfileStore& profile_store) : m_store(profile_store) {}
    ~ProfilerIngestServer() {
        Stop();
    }

    bool Start(uint16_t port) {
        if (m_running.exchange(true)) {
            return true;
        }
        m_listen_port = port;
        m_server_thread = std::thread([this]() { ServerMain(); });
        return true;
    }

    void Stop() {
        if (!m_running.exchange(false)) {
            return;
        }
        m_listen_socket.Close();
        m_client_socket.Close();
        if (m_server_thread.joinable()) {
            m_server_thread.join();
        }
    }

private:
    static bool RecvAll(Network::TcpSocket& socket, uint8_t* data, size_t len) {
        return socket.RecvAll(std::span<std::byte>(reinterpret_cast<std::byte*>(data), len)) ==
               Network::ESocketStatus::Success;
    }

    template<typename HeaderT>
    static bool RecvHeaderAfterMagic(Network::TcpSocket& socket, uint32_t magic, HeaderT& header) {
        header = {};
        header.magic = magic;
        return RecvAll(
            socket,
            reinterpret_cast<uint8_t*>(&header) + sizeof(magic),
            sizeof(HeaderT) - sizeof(magic)
        );
    }

    void ServerMain() {
        if (m_listen_socket.BindListen(Network::TcpListenDesc{.port = m_listen_port, .backlog = 1}) !=
            Network::ESocketStatus::Success) {
            m_running.store(false);
            return;
        }

        while (m_running.load()) {
            Network::TcpSocket client{};
            if (m_listen_socket.Accept(client) != Network::ESocketStatus::Success) {
                if (!m_running.load()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            m_client_socket = std::move(client);
            ClientMain(m_client_socket);
            m_client_socket.Close();
        }
    }

    void ClientMain(Network::TcpSocket& socket) {
        m_store.Reset();
        m_store.SetSessionName("Profiler TCP");
        m_profile_dump_decoder.Reset();
        m_trace_decoder.Reset();
        while (m_running.load()) {
            uint32_t magic = 0;
            if (!RecvAll(socket, reinterpret_cast<uint8_t*>(&magic), sizeof(magic))) {
                break;
            }

            if (magic == ProfileDump::packet_magic) {
                ProfileDump::PacketHeader header{};
                if (!RecvHeaderAfterMagic(socket, magic, header) || header.version != ProfileDump::packet_version) {
                    break;
                }
                Array<uint8_t> payload{};
                payload.resize(header.payload_size);
                if (header.payload_size > 0 && !RecvAll(socket, payload.data(), payload.size())) {
                    break;
                }

                Array<ProfileEvent> events{};
                if (!m_profile_dump_decoder.ConsumePacket(header, payload, events)) {
                    break;
                }
                if (!events.empty()) {
                    m_store.AppendEvents(events, m_trace_decoder.TimeOriginNs());
                }
                continue;
            }

            if (magic == trace_packet_magic) {
                Trace::PacketHeader header{};
                if (!RecvHeaderAfterMagic(socket, magic, header)) {
                    break;
                }
                Array<uint8_t> payload{};
                payload.resize(header.payload_size);
                if (header.payload_size > 0 && !RecvAll(socket, payload.data(), payload.size())) {
                    break;
                }

                Array<ProfileEvent> events{};
                Utf8String session_name{};
                if (!m_trace_decoder.ConsumePacket(header, payload, events, &session_name)) {
                    break;
                }
                if (!session_name.empty()) {
                    m_store.SetSessionName(std::move(session_name));
                }
                if (!events.empty()) {
                    m_store.AppendEvents(events);
                }
                continue;
            }

            break;
        }

        m_profile_dump_decoder.Reset();
        m_trace_decoder.Reset();
    }

private:
    ProfileStore&             m_store;
    std::atomic<bool>         m_running{false};
    uint16_t                  m_listen_port{19090};
    std::thread               m_server_thread{};
    ProfileDumpSessionDecoder m_profile_dump_decoder{};
    TraceSessionDecoder       m_trace_decoder{};
    Network::TcpSocket        m_listen_socket{};
    Network::TcpSocket        m_client_socket{};
};

Utf8String ToProfilerString(const char* text) {
    return Utf8String(text ? text : "");
}

bool ContainsCaseInsensitive(Utf8StringView text, Utf8StringView token) {
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

bool Utf8Less(Utf8StringView lhs, Utf8StringView rhs) {
    return std::lexicographical_compare(lhs.data(), lhs.data() + lhs.size(), rhs.data(), rhs.data() + rhs.size());
}

uint32_t HashUtf8(Utf8StringView text) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < text.size(); ++i) {
        hash ^= static_cast<uint8_t>(text[i]);
        hash *= 16777619u;
    }
    return hash;
}

uint32_t PackColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (static_cast<uint32_t>(a) << 24u) |
           (static_cast<uint32_t>(b) << 16u) |
           (static_cast<uint32_t>(g) << 8u) |
           static_cast<uint32_t>(r);
}

uint32_t HsvToPackedColor(float hue, float saturation, float value, float alpha) {
    hue = hue - static_cast<float>(static_cast<int>(hue));
    const float scaled_hue = hue * 6.0f;
    const int   sector = static_cast<int>(scaled_hue);
    const float f = scaled_hue - static_cast<float>(sector);
    const float p = value * (1.0f - saturation);
    const float q = value * (1.0f - saturation * f);
    const float t = value * (1.0f - saturation * (1.0f - f));

    float r = value;
    float g = t;
    float b = p;
    switch (sector % 6) {
        case 0:
            r = value;
            g = t;
            b = p;
            break;
        case 1:
            r = q;
            g = value;
            b = p;
            break;
        case 2:
            r = p;
            g = value;
            b = t;
            break;
        case 3:
            r = p;
            g = q;
            b = value;
            break;
        case 4:
            r = t;
            g = p;
            b = value;
            break;
        default:
            r = value;
            g = p;
            b = q;
            break;
    }

    return PackColor(
        static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f),
        static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f),
        static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f),
        static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)
    );
}

uint32_t EventColorByName(Utf8StringView name) {
    const uint32_t h = HashUtf8(name);
    const float hue = (h % 360u) / 360.0f;
    const float sat = 0.45f + float((h >> 8u) & 0x1Fu) / 255.0f;
    const float val = 0.68f + float((h >> 16u) & 0x1Fu) / 255.0f;
    return HsvToPackedColor(hue, std::clamp(sat, 0.35f, 0.62f), std::clamp(val, 0.62f, 0.88f), 0.92f);
}

uint64_t EstimateProfileSizeBytes(const ProfileStore& store) {
    uint64_t bytes = static_cast<uint64_t>(store.events.size() * sizeof(ProfileEvent));
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
    uint64_t bytes = static_cast<uint64_t>(event_count * sizeof(ProfileEvent));
    bytes += static_cast<uint64_t>(track_count * sizeof(TrackInfo));
    bytes += static_cast<uint64_t>(session_name_size);
    return bytes;
}

Utf8String FormatBytes(uint64_t bytes) {
    const double value = static_cast<double>(bytes);
    AsciiChar    buf[64]{};
    if (value >= 1024.0 * 1024.0 * 1024.0) {
        std::snprintf(buf, sizeof(buf), MOER_ASCII_TEXT("%.2f GiB"), value / (1024.0 * 1024.0 * 1024.0));
    } else if (value >= 1024.0 * 1024.0) {
        std::snprintf(buf, sizeof(buf), MOER_ASCII_TEXT("%.2f MiB"), value / (1024.0 * 1024.0));
    } else if (value >= 1024.0) {
        std::snprintf(buf, sizeof(buf), MOER_ASCII_TEXT("%.2f KiB"), value / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), MOER_ASCII_TEXT("%llu B"), static_cast<unsigned long long>(bytes));
    }
    return ToProfilerString(buf);
}

struct TimelineViewState {
    uint64_t view_start_ns{0};
    uint64_t view_end_ns{0};
    bool     auto_follow{true};
    AsciiChar search_text[128]{};
};

void DrawTimeRuler(
    Synapse::Context& ui,
    Synapse::Size     origin,
    float             width,
    float             y,
    uint64_t          start_ns,
    uint64_t          end_ns
) {
    ui.DrawLine({origin.x, y}, {origin.x + width, y}, PackColor(180, 180, 180), 1.0f);

    const double span_ns = static_cast<double>(std::max<uint64_t>(1, end_ns - start_ns));
    const int major_ticks = std::clamp(static_cast<int>(width / 130.0f), 3, 12);
    for (int i = 0; i <= major_ticks; ++i) {
        const float  t = static_cast<float>(i) / static_cast<float>(major_ticks);
        const float  x = origin.x + t * width;
        const double ts_ns = static_cast<double>(start_ns) + t * span_ns;
        ui.DrawLine({x, y}, {x, y + 8.0f}, PackColor(200, 200, 200), 1.0f);

        AsciiChar label[64]{};
        if (ts_ns >= 1e9) {
            std::snprintf(label, sizeof(label), MOER_ASCII_TEXT("%.3fs"), ts_ns / 1e9);
        } else if (ts_ns >= 1e6) {
            std::snprintf(label, sizeof(label), MOER_ASCII_TEXT("%.3fms"), ts_ns / 1e6);
        } else if (ts_ns >= 1e3) {
            std::snprintf(label, sizeof(label), MOER_ASCII_TEXT("%.3fus"), ts_ns / 1e3);
        } else {
            std::snprintf(label, sizeof(label), MOER_ASCII_TEXT("%.0fns"), ts_ns);
        }
        ui.DrawCanvasText({x + 2.0f, y + 10.0f}, PackColor(200, 200, 200), label);
    }
}

void DrawTimelinePanel(Synapse::Context& ui, ProfileStore& store, TimelineViewState& ui_state) {
    struct TimelineDataCache {
        uint64_t store_generation{0};
        Array<ProfileEvent>      events{};
        Array<TrackInfo> tracks{};
        uint64_t min_ts{0};
        uint64_t max_ts{0};
        UnorderedMap<uint64_t, Array<size_t>> scope_indices_by_track{};
        Array<uint32_t> gpu_display_depth_by_index{};
        UnorderedMap<uint64_t, int> gpu_display_max_depth_by_track{};
        UnorderedMap<uint64_t, size_t> event_index_by_id{};
        uint64_t search_generation{0};
        Utf8String cached_search{};
        Array<uint8_t> search_match_mask{};
        Array<size_t> matched_indices{};
        Array<uint32_t> overview_bins{};
        uint32_t overview_max_bin{0};
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
                return Utf8Less(a.name, b.name);
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
        static constexpr size_t overview_bin_count = 192;
        cache.overview_bins.assign(overview_bin_count, 0);
        cache.overview_max_bin = 0;
        const uint64_t overview_span = std::max<uint64_t>(1, cache.max_ts + 1 - cache.min_ts);

        for (size_t i = 0; i < events.size(); ++i) {
            const auto& e = events[i];
            cache.event_index_by_id[e.event_id] = i;
            cache.gpu_display_depth_by_index[i] = e.depth;
            const uint64_t track_key = MakeTrackKey(e.track_type, e.track_id);
            cache.scope_indices_by_track[track_key].emplace_back(i);
            const double bin_t = double(e.ts_begin_ns - cache.min_ts) / double(overview_span);
            const size_t bin = std::min(
                overview_bin_count - 1,
                static_cast<size_t>(std::max(0.0, bin_t) * double(overview_bin_count))
            );
            cache.overview_bins[bin] += 1;
            cache.overview_max_bin = std::max(cache.overview_max_bin, cache.overview_bins[bin]);
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
                events[indices.front()].track_type != ProfileTrackType::GPUQueue) {
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
        ui.Text("Waiting for ProfileDump/Trace stream on port 19090...");
        return;
    }

    const Utf8String search_key = ToProfilerString(ui_state.search_text);
    if (cache.search_generation != cache.store_generation || cache.cached_search != search_key) {
        cache.cached_search = search_key;
        cache.search_generation = cache.store_generation;
        cache.search_match_mask.assign(events.size(), 0);
        cache.matched_indices.clear();
        cache.matched_indices.reserve(events.size() / 16 + 8);
        for (size_t i = 0; i < events.size(); ++i) {
            const auto& e = events[i];
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
        ui.Text("Matches: %d", static_cast<int>(matched_indices.size()));
        ui.SameLine();
        if (ui.Button("Prev")) {
            match_cursor = (match_cursor - 1 + static_cast<int>(matched_indices.size())) %
                           static_cast<int>(matched_indices.size());
            const auto& e = events[matched_indices[match_cursor]];
            selected_event_id = e.event_id;
            const uint64_t span = std::max<uint64_t>(1, ui_state.view_end_ns - ui_state.view_start_ns);
            ui_state.view_start_ns = e.ts_begin_ns > span / 2 ? e.ts_begin_ns - span / 2 : 0;
            ui_state.view_end_ns   = ui_state.view_start_ns + span;
            ui_state.auto_follow   = false;
        }
        ui.SameLine();
        if (ui.Button("Next")) {
            match_cursor = (match_cursor + 1) % static_cast<int>(matched_indices.size());
            const auto& e = events[matched_indices[match_cursor]];
            selected_event_id = e.event_id;
            const uint64_t span = std::max<uint64_t>(1, ui_state.view_end_ns - ui_state.view_start_ns);
            ui_state.view_start_ns = e.ts_begin_ns > span / 2 ? e.ts_begin_ns - span / 2 : 0;
            ui_state.view_end_ns   = ui_state.view_start_ns + span;
            ui_state.auto_follow   = false;
        }
        ui.Separator();
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

    if (ui.Button(MOER_ASCII_TEXT("Visible Tracks"))) {
        ui.OpenPopup(MOER_ASCII_TEXT("VisibleTracksPopup"));
    }
    if (ui.BeginPopup(MOER_ASCII_TEXT("VisibleTracksPopup"))) {
        for (const auto& track : tracks) {
            bool visible = track_visibility[track.key];
            AsciiChar key_suffix[64]{};
            std::snprintf(key_suffix, sizeof(key_suffix), MOER_ASCII_TEXT("##vis_%llu"), static_cast<unsigned long long>(track.key));
            Utf8String label = ToProfilerString(MOER_ASCII_TEXT("["));
            label += track.type == ProfileTrackType::CPUThread ? MOER_ASCII_TEXT("CPU") : MOER_ASCII_TEXT("GPU");
            label += MOER_ASCII_TEXT("] ");
            label += Utf8StringView(track.name);
            label += key_suffix;
            if (ui.Checkbox(label.c_str(), &visible)) {
                track_visibility[track.key] = visible;
            }
        }
        ui.EndPopup();
    }
    size_t visible_count = 0;
    for (const auto& [id, visible] : track_visibility) {
        (void)id;
        if (visible) {
            ++visible_count;
        }
    }
    ui.SameLine();
    ui.Text(
        "Visible: %llu / %llu",
        static_cast<unsigned long long>(visible_count),
        static_cast<unsigned long long>(tracks.size())
    );
    ui.Separator();

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

    auto zoom_view_at = [&](double anchor_t, double zoom) {
        ui_state.auto_follow = false;
        const uint64_t span = std::max<uint64_t>(1, ui_state.view_end_ns - ui_state.view_start_ns);
        const uint64_t data_span = std::max<uint64_t>(1, (max_ts + 1) - min_ts);
        const uint64_t new_span = static_cast<uint64_t>(std::clamp(span * zoom, 1000.0, double(data_span)));
        const uint64_t anchor = ui_state.view_start_ns + static_cast<uint64_t>(anchor_t * double(span));
        const uint64_t anchor_offset = static_cast<uint64_t>(anchor_t * double(new_span));
        const uint64_t new_start = anchor > anchor_offset ? anchor - anchor_offset : 0;
        ui_state.view_start_ns = new_start;
        ui_state.view_end_ns = new_start + new_span;
        clamp_view_range(min_ts, max_ts + 1);
    };

    auto pan_view_by = [&](int64_t shift_ns) {
        ui_state.auto_follow = false;
        int64_t new_start = static_cast<int64_t>(ui_state.view_start_ns) + shift_ns;
        int64_t new_end = static_cast<int64_t>(ui_state.view_end_ns) + shift_ns;
        if (new_start < 0) {
            new_end -= new_start;
            new_start = 0;
        }
        ui_state.view_start_ns = static_cast<uint64_t>(new_start);
        ui_state.view_end_ns = static_cast<uint64_t>(new_end);
        clamp_view_range(min_ts, max_ts + 1);
    };

    auto center_view_at = [&](uint64_t center_ns) {
        ui_state.auto_follow = false;
        const uint64_t span = std::max<uint64_t>(1, ui_state.view_end_ns - ui_state.view_start_ns);
        const uint64_t half_span = span / 2;
        ui_state.view_start_ns = center_ns > half_span ? center_ns - half_span : 0;
        ui_state.view_end_ns = ui_state.view_start_ns + span;
        clamp_view_range(min_ts, max_ts + 1);
    };

    const float overview_child_h = 64.0f;
    const float timeline_child_h = std::max(220.0f, ui.GetContentRegionAvail().y - overview_child_h - 6.0f);

    ui.BeginChild(MOER_ASCII_TEXT("TimelineMergedCanvas"), {0.0f, timeline_child_h}, true, true);

    const Synapse::Size canvas_origin = ui.GetCursorScreenPos();
    const float  canvas_w      = std::max(1000.0f, ui.GetContentRegionAvail().x);
    const float  timeline_x0   = canvas_origin.x + label_col_w;
    const float  timeline_w    = std::max(300.0f, canvas_w - label_col_w - 20.0f);
    const Synapse::Size mouse_pos     = ui.GetMousePos();
    const bool   mouse_in_timeline =
        mouse_pos.x >= timeline_x0 && mouse_pos.x <= (timeline_x0 + timeline_w);

    if (ui_state.auto_follow || ui_state.view_end_ns <= ui_state.view_start_ns) {
        ui_state.view_start_ns = min_ts;
        ui_state.view_end_ns   = max_ts + 1;
    }
    clamp_view_range(min_ts, max_ts + 1);

    // Zoom + pan
    if (ui.IsWindowHovered()) {
        const uint64_t span = std::max<uint64_t>(1, ui_state.view_end_ns - ui_state.view_start_ns);
        const float mouse_wheel = ui.GetMouseWheel();
        const Synapse::Size mouse_delta = ui.GetMouseDelta();

        if (mouse_wheel != 0.0f && !ui.IsShiftDown() && mouse_in_timeline) {
            const double zoom = mouse_wheel > 0.0f ? 0.8 : 1.25;
            const double mouse_t = std::clamp((mouse_pos.x - timeline_x0) / std::max(1.0f, timeline_w), 0.0f, 1.0f);
            zoom_view_at(mouse_t, zoom);
        }

        if (ui.IsMouseDragging(Synapse::EMouseButton::Middle) && mouse_in_timeline) {
            const double delta_t = -double(mouse_delta.x) / std::max(1.0f, timeline_w);
            const int64_t shift_ns = static_cast<int64_t>(delta_t * double(span));
            pan_view_by(shift_ns);
        }

        // Horizontal pan support: Shift + wheel and arrow keys
        if (mouse_wheel != 0.0f && ui.IsShiftDown() && mouse_in_timeline) {
            const int64_t shift_ns = static_cast<int64_t>(-mouse_wheel * double(span) * 0.12);
            pan_view_by(shift_ns);
        }
        if (ui.IsKeyDown(Synapse::EKey::LeftArrow) || ui.IsKeyDown(Synapse::EKey::RightArrow)) {
            const int dir = ui.IsKeyDown(Synapse::EKey::LeftArrow) ? -1 : 1;
            const int64_t shift_ns = static_cast<int64_t>(double(span) * 0.015 * dir);
            pan_view_by(shift_ns);
        }
    }

    if (ui.IsWindowFocusedChildWindows() && ui.IsKeyPressed(Synapse::EKey::F)) {
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

    ui.DrawRectFilled(
        canvas_origin,
        {canvas_origin.x + canvas_w, canvas_origin.y + top_header_h},
        PackColor(20, 20, 20)
    );
    ui.DrawRectFilled(
        {canvas_origin.x, canvas_origin.y},
        {canvas_origin.x + label_col_w, canvas_origin.y + top_header_h},
        PackColor(34, 34, 34)
    );
    ui.DrawLine(
        {timeline_x0, canvas_origin.y},
        {timeline_x0, canvas_origin.y + top_header_h},
        PackColor(80, 80, 80),
        1.0f
    );
    DrawTimeRuler(
        ui,
        {timeline_x0, canvas_origin.y},
        timeline_w,
        canvas_origin.y,
        ui_state.view_start_ns,
        ui_state.view_end_ns
    );
    ui.Dummy({canvas_w, top_header_h});

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
        const int   cur_type_group = track.type == ProfileTrackType::CPUThread ? 0 : 1;
        if (cur_type_group != last_type_group) {
            const char* group_label = cur_type_group == 0 ? "CPU Tracks" : "GPU Tracks";
            ui.DrawRectFilled(
                {canvas_origin.x, y_cursor},
                {canvas_origin.x + canvas_w, y_cursor + group_header_h},
                PackColor(40, 40, 40)
            );
            ui.DrawCanvasText(
                {canvas_origin.x + 8.0f, y_cursor + 2.0f},
                PackColor(230, 230, 230),
                group_label
            );
            y_cursor += group_header_h;
            ui.Dummy({1.0f, group_header_h});
            last_type_group = cur_type_group;
        }

        ui.DrawRectFilled(
            {canvas_origin.x, y_cursor},
            {canvas_origin.x + label_col_w, y_cursor + track_h},
            PackColor(30, 30, 30)
        );
        ui.DrawRectFilled(
            {timeline_x0, y_cursor},
            {timeline_x0 + timeline_w, y_cursor + track_h},
            PackColor(24, 24, 24)
        );
        ui.DrawLine(
            {canvas_origin.x, y_cursor},
            {canvas_origin.x + canvas_w, y_cursor},
            PackColor(70, 70, 70),
            1.0f
        );

        Utf8String track_label = ToProfilerString("[");
        track_label += track.type == ProfileTrackType::CPUThread ? "CPU" : "GPU";
        track_label += "] ";
        track_label += Utf8StringView(track.name);
        const float label_y = y_cursor + 3.0f;
        ui.DrawCanvasText(
            {canvas_origin.x + 8.0f, label_y},
            PackColor(220, 220, 220),
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
                float x1 = to_x(e.ts_end_ns);
                if (e.type != ProfileEventType::Scope || x1 <= x0) {
                    x1 = x0 + 3.0f;
                    x0 = x0 - 1.5f;
                }
                x0 = std::max(x0, timeline_x0);
                x1 = std::min(x1, timeline_x0 + timeline_w);
                if (x1 <= x0) {
                    continue;
                }
                uint32_t depth = e.depth;
                if (e.track_type == ProfileTrackType::GPUQueue) {
                    depth = cache.gpu_display_depth_by_index[idx];
                }
                const float y0 = y_cursor + lane_h * static_cast<float>(depth);
                const float y1 = y0 + lane_h - 2.0f;

                const uint32_t color = EventColorByName(e.name);

                if (e.type == ProfileEventType::Scope) {
                    ui.DrawRectFilled({x0, y0}, {x1, y1}, color, 2.0f);
                } else {
                    const float marker_x = (x0 + x1) * 0.5f;
                    const float marker_y = (y0 + y1) * 0.5f;
                    ui.DrawCircleFilled({marker_x, marker_y}, 4.0f, color, 12);
                    ui.DrawLine({marker_x, y0}, {marker_x, y1}, color, 1.0f);
                }
                if (e.event_id == selected_event_id) {
                    ui.DrawRect({x0, y0}, {x1, y1}, PackColor(255, 255, 0), 2.0f, 2.0f);
                }
                const float inner_w = x1 - x0 - 6.0f;
                const float inner_h = y1 - y0 - 2.0f;
                if (inner_w > 8.0f && inner_h > 7.0f) {
                    float text_size = std::min(ui.GetFontSize(), inner_h);
                    text_size = std::max(8.0f, text_size);
                    const Synapse::Size text_sz = ui.CalcTextSize(e.name.c_str());
                    if (text_sz.x > 0.0f && text_sz.x > inner_w) {
                        text_size = std::max(8.0f, text_size * (inner_w / text_sz.x));
                    }
                    ui.DrawTextClipped(
                        {x0 + 4.0f, y0 + (y1 - y0 - text_size) * 0.5f},
                        PackColor(230, 230, 230),
                        e.name.c_str(),
                        text_size,
                        {x0 + 2.0f, y0},
                        {x1 - 2.0f, y1}
                    );
                }

                if (ui.IsMouseHoveringRect({x0, y0}, {x1, y1})) {
                    if (ui.IsMouseClicked(Synapse::EMouseButton::Left)) {
                        selected_event_id = e.event_id;
                    }
                    Utf8String parent_name = ToProfilerString("None");
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

                    ui.BeginTooltip();
                    ui.Text("Name: %s", e.name.c_str());
                    ui.Text("Category: %s", e.category.c_str());
                    ui.Text("Type: %s", e.type == ProfileEventType::Scope ? "Scope" : (e.type == ProfileEventType::Counter ? "Counter" : "Instant"));
                    ui.Text("Depth: %u", depth);
                    ui.Text("Duration: %.3f ms", static_cast<double>(e.ts_end_ns - e.ts_begin_ns) / 1e6);
                    if (e.type == ProfileEventType::Counter) {
                        ui.Text("Value: %.4f", e.counter_value);
                    }
                    ui.Text("Parent: %s", parent_name.c_str());
                    ui.EndTooltip();
                }
            }
        }

        y_cursor += track_h;
        ui.Dummy({1.0f, track_h});
    }

    ui.DrawLine(
        {canvas_origin.x, y_cursor},
        {canvas_origin.x + canvas_w, y_cursor},
        PackColor(70, 70, 70),
        1.0f
    );
    ui.Dummy({canvas_w, std::max(200.0f, y_cursor - canvas_origin.y + 20.0f)});
    ui.EndChild();

    ui.BeginChild(MOER_ASCII_TEXT("TimelineOverview"), {0.0f, overview_child_h}, true, false);
    const Synapse::Size overview_origin = ui.GetCursorScreenPos();
    const float overview_w = std::max(700.0f, ui.GetContentRegionAvail().x);
    const float overview_x0 = overview_origin.x + label_col_w;
    const float overview_timeline_w = std::max(300.0f, overview_w - label_col_w - 20.0f);
    const float overview_y0 = overview_origin.y + 24.0f;
    const float overview_h = 20.0f;
    const uint64_t data_span = std::max<uint64_t>(1, (max_ts + 1) - min_ts);
    const Synapse::Size overview_mouse_pos = ui.GetMousePos();
    const bool mouse_in_overview = overview_mouse_pos.x >= overview_x0 &&
                                   overview_mouse_pos.x <= overview_x0 + overview_timeline_w &&
                                   overview_mouse_pos.y >= overview_y0 - 10.0f &&
                                   overview_mouse_pos.y <= overview_y0 + overview_h + 14.0f;

    ui.DrawCanvasText(
        {overview_origin.x + 8.0f, overview_origin.y + 6.0f},
        PackColor(205, 205, 205),
        MOER_ASCII_TEXT("Overview")
    );
    ui.DrawRectFilled(
        {overview_x0, overview_y0},
        {overview_x0 + overview_timeline_w, overview_y0 + overview_h},
        PackColor(18, 18, 20),
        3.0f
    );
    if (!cache.overview_bins.empty() && cache.overview_max_bin > 0) {
        const float bin_w = overview_timeline_w / static_cast<float>(cache.overview_bins.size());
        for (size_t i = 0; i < cache.overview_bins.size(); ++i) {
            const float fraction = static_cast<float>(cache.overview_bins[i]) / static_cast<float>(cache.overview_max_bin);
            const float x0 = overview_x0 + static_cast<float>(i) * bin_w;
            const float x1 = x0 + std::max(1.0f, bin_w - 1.0f);
            const float y0 = overview_y0 + overview_h - std::max(2.0f, fraction * overview_h);
            ui.DrawRectFilled({x0, y0}, {x1, overview_y0 + overview_h}, PackColor(96, 142, 166, 190), 1.0f);
        }
    }

    auto overview_to_x = [&](uint64_t ts) {
        const double t = (static_cast<double>(ts) - static_cast<double>(min_ts)) / static_cast<double>(data_span);
        return overview_x0 + static_cast<float>(std::clamp(t, 0.0, 1.0) * overview_timeline_w);
    };
    const float view_x0 = overview_to_x(ui_state.view_start_ns);
    const float view_x1 = overview_to_x(ui_state.view_end_ns);
    ui.DrawRectFilled(
        {view_x0, overview_y0 - 3.0f},
        {view_x1, overview_y0 + overview_h + 3.0f},
        PackColor(225, 179, 72, 58),
        3.0f
    );
    ui.DrawRect(
        {view_x0, overview_y0 - 3.0f},
        {view_x1, overview_y0 + overview_h + 3.0f},
        PackColor(225, 179, 72, 230),
        3.0f,
        1.5f
    );

    AsciiChar range_label[96]{};
    std::snprintf(
        range_label,
        sizeof(range_label),
        MOER_ASCII_TEXT("%.3f ms / %.3f ms"),
        static_cast<double>(ui_state.view_end_ns - ui_state.view_start_ns) / 1e6,
        static_cast<double>(data_span) / 1e6
    );
    ui.DrawCanvasText({overview_x0, overview_origin.y + 5.0f}, PackColor(150, 150, 150), range_label);

    if (ui.IsWindowHovered() && mouse_in_overview) {
        const double mouse_t = std::clamp(
            (overview_mouse_pos.x - overview_x0) / std::max(1.0f, overview_timeline_w),
            0.0f,
            1.0f
        );
        const float wheel = ui.GetMouseWheel();
        if (wheel != 0.0f && !ui.IsShiftDown()) {
            zoom_view_at(mouse_t, wheel > 0.0f ? 0.8 : 1.25);
        } else if (ui.IsMouseClicked(Synapse::EMouseButton::Left) ||
                   ui.IsMouseDragging(Synapse::EMouseButton::Left)) {
            const uint64_t target_ns = min_ts + static_cast<uint64_t>(mouse_t * double(data_span));
            center_view_at(target_ns);
        }
    }
    ui.Dummy({overview_w, overview_child_h});
    ui.EndChild();
}

} // namespace

int RunProfilerMain(int argc, const char** argv) {
    std::filesystem::path path = argv[0];
    path = path.extension() == ".exe" ? path.parent_path() : path;
    LogSystem::Init();
    LOG_INFO(MOER_TEXT("MoerProfiler starting..."));
    ConfigManager::GetInstance().Init(path);
    TaskSystem::Init();
    if (!FileDialog::Init()) {
        LOG_WARNING(MOER_TEXT("Native file dialog is unavailable in profiler."));
    }

    RenderDevice::Init(
        DeviceInitInfo{
            .rhi_type        = ERHIType::Vulkan,
            .name            = "MoerProfiler",
            .rhi_api_version = "1.3",
        }
    );

    WindowContext::Init(SurfaceInitInfo(1680, 980, "MoerProfiler", false));

    RenderDevice& device     = RenderDevice::Get();
    CommandQueue& gfx_queue  = device.GetCommandQueue(EQueueType::Graphics);
    FenceRef      timeline   = device.CreateFence();
    uint64_t      frame_time = 0;

    uint2 resolution = {1680, 980};
    static constexpr uint profiler_back_buffer_count = 2;
    auto presentation_surface = MakeUnique<PresentationSurface>(
        device,
        PresentationSurfaceDesc{
            .window            = *WindowContext::GetMainWindow(),
            .size              = {resolution.x, resolution.y},
            .back_buffer_count = profiler_back_buffer_count,
            .preferred_format  = PF_R8G8B8A8_SRGB,
            .debug_name        = "Profiler Presentation Surface",
        }
    );
    TextureRef   output =
        device.CreateTexture("ProfilerOutput", Extent2D(resolution.x, resolution.y), presentation_surface->GetFormat(), ETextureUsageFlags::COLOR_ATTACHMENT);

    auto ui_renderer = MakeUnique<Render::UIRenderer>(device);
    Synapse::Context synapse_context{};
    Synapse::Theme profiler_theme{};
    profiler_theme.panel_bg = {0.012f, 0.013f, 0.014f, 1.0f};
    profiler_theme.panel_border = {0.18f, 0.18f, 0.18f, 0.55f};
    profiler_theme.panel_header = {0.08f, 0.085f, 0.09f, 0.82f};
    profiler_theme.panel_header_hovered = {0.12f, 0.125f, 0.13f, 0.86f};
    profiler_theme.panel_header_active = {0.16f, 0.16f, 0.15f, 0.90f};
    profiler_theme.toolbar_bg = {0.025f, 0.026f, 0.028f, 0.92f};
    profiler_theme.accent = {0.88f, 0.70f, 0.28f, 1.0f};
    profiler_theme.button = {0.10f, 0.11f, 0.11f, 0.78f};
    profiler_theme.button_hovered = {0.16f, 0.17f, 0.17f, 0.86f};
    profiler_theme.button_active = {0.25f, 0.22f, 0.14f, 0.92f};
    profiler_theme.panel_rounding = 4.0f;
    profiler_theme.child_rounding = 4.0f;
    profiler_theme.frame_rounding = 3.0f;
    synapse_context.ApplyTheme(profiler_theme);

    ProfileStore            store{};
    ProfilerIngestServer ingest(store);
    ingest.Start(19090);

    TimelineViewState timeline_state{};
    timeline_state.auto_follow = true;
    Utf8String capture_path{};

    while (!WindowContext::ShouldClose(WindowContext::GetMainWindow())) {
        WindowContext::Tick();
        const uint64_t max_frames_in_flight =
            std::max<uint64_t>(1, static_cast<uint64_t>(profiler_back_buffer_count));
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
            presentation_surface->Resize({resolution.x, resolution.y});
            output = device.CreateTexture(
                "ProfilerOutput",
                Extent2D(resolution.x, resolution.y),
                presentation_surface->GetFormat(),
                ETextureUsageFlags::COLOR_ATTACHMENT
            );
        }

        ui_renderer->BeginGUIFrame();
        synapse_context.BeginFrame(ui_renderer->GetInputSnapshot());
        Synapse::Context& ui = synapse_context;
        if (ui.BeginRootPanel(Synapse::PanelDesc{.name = MOER_ASCII_TEXT("ProfilerTrace")})) {
            if (ui.ToolbarButton(Synapse::EIcon::FolderOpen, MOER_ASCII_TEXT("Load"))) {
                static constexpr std::array<FileDialog::Filter, 4> capture_filters = {{
                    {MOER_ASCII_TEXT("Profiler Capture"), MOER_ASCII_TEXT("mpd,mrtc,csv,bin")},
                    {MOER_ASCII_TEXT("Trace CSV"), MOER_ASCII_TEXT("csv")},
                    {MOER_ASCII_TEXT("Binary"), MOER_ASCII_TEXT("bin")},
                    {MOER_ASCII_TEXT("All"), MOER_ASCII_TEXT("*")},
                }};
                const FileDialog::EOpenFileStatus status = FileDialog::OpenFile(FileDialog::OpenFileRequest{
                    .filters = capture_filters,
                    .callback = [](Utf8StringView selected_path, void* user_data) {
                        *static_cast<Utf8String*>(user_data) = Utf8String(selected_path);
                    },
                    .user_data = &capture_path,
                });
                if (status == FileDialog::EOpenFileStatus::Success) {
                    if (!LoadProfilerCaptureFile(capture_path, store, true)) {
                        LOG_WARNING(MOER_TEXT("Profiler capture load failed: {}"), capture_path);
                    }
                }
            }
            ui.SameLine();
            ui.Checkbox(MOER_ASCII_TEXT("Auto Follow"), &timeline_state.auto_follow);
            ui.SameLine();
            ui.InputTextWithHint(
                MOER_ASCII_TEXT("##search"),
                MOER_ASCII_TEXT("Search timescope name"),
                timeline_state.search_text,
                sizeof(timeline_state.search_text)
            );

            Utf8String ui_session_name{};
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
            const Utf8String profile_size =
                FormatBytes(EstimateProfileSizeBytesFast(ui_event_count, ui_track_count, ui_session_name.size()));
            ui.SameLine();
            ui.Text(
                MOER_ASCII_TEXT("%s | %llu events | %s"),
                ui_session_name.empty() ? MOER_ASCII_TEXT("No session") : ui_session_name.c_str(),
                static_cast<unsigned long long>(ui_event_count),
                profile_size.c_str()
            );
            ui.Text(
                MOER_ASCII_TEXT("Capture: %s | Time: [%llu, %llu] ns"),
                capture_path.empty() ? MOER_ASCII_TEXT("(live)") : capture_path.c_str(),
                static_cast<unsigned long long>(ui_min_ts),
                static_cast<unsigned long long>(ui_max_ts)
            );
            ui.Separator();
            DrawTimelinePanel(ui, store, timeline_state);
            ui.EndPanel();
        }
        synapse_context.EndFrame();
        ui_renderer->EndGUIFrame();

        CommandList cmd_list{};
        ui_renderer->RenderGUI(cmd_list, output->GetView());

        ++frame_time;
        cmd_list.Signal(timeline, frame_time).DeleteResources().TickFrame();
        Array<CommandList> frame_cmd_lists;
        frame_cmd_lists.emplace_back(std::move(cmd_list));
        RHIPresentRequest present_request = presentation_surface->CreatePresentRequest(output);
        RHIExecutor::Get().Submit(
            std::move(frame_cmd_lists),
            ERHIExecSubmitFlags::FlushGPU,
            &present_request
        );
        ui_renderer->PresentWindows();
    }

    ingest.Stop();
    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    ui_renderer.reset();
    output = {};
    timeline = {};
    presentation_surface.reset();
    WindowContext::ShutDown();
    RHIExecutor::ShutDown();
    RenderDevice::Dispose();
    FileDialog::ShutDown();
    TaskSystem::ShutDown();
    return 0;
}

} // namespace Moer

int main(int argc, const char** argv) {
    return Moer::RunProfilerMain(argc, argv);
}
