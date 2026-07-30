#include "profile_viewer_ui/ProfileViewerUI.h"

#include "profile_viewer_ui/ProfileViewerModel.h"

#include "log/LogSystem.h"
#include "profile_consumer/ProfileDocument.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>
#include <nfd.hpp>

namespace Moer {
namespace {

using namespace ProfileDump;

constexpr std::size_t   k_query_capacity         = 4096;
constexpr std::size_t   k_frame_event_budget     = 16384;
constexpr std::size_t   k_filter_capacity        = 128;
constexpr std::size_t   k_filter_name_byte_limit = 1024;
constexpr std::size_t   k_draw_name_byte_limit   = 256;
constexpr std::size_t   k_track_scan_budget      = 4096;
constexpr std::size_t   k_axis_scan_budget       = kProfileViewerGpuRenderableAxisMax;
constexpr float         k_track_label_width      = 248.0f;
constexpr float         k_lane_height            = 18.0f;
constexpr std::uint32_t k_direct_depth_lanes     = 3;
constexpr std::uint32_t k_overflow_depth_lane    = k_direct_depth_lanes;
constexpr float k_track_row_height = (static_cast<float>(k_direct_depth_lanes) + 1.0f) * k_lane_height + 8.0f;

enum class ERangeDomain : std::uint8_t {
    None = 0,
    Cpu,
    Gpu,
};

struct RangeSelection {
    ERangeDomain                domain{ERangeDomain::None};
    ProfileViewerGpuViewportKey gpu_key{};
    bool                        dragging{false};
    bool                        committed{false};
    std::uint64_t               anchor{0};
    std::uint64_t               begin{0};
    std::uint64_t               end{0};

    void Clear() noexcept {
        *this = {};
    }

    [[nodiscard]] bool ActiveForCpu() const noexcept {
        return domain == ERangeDomain::Cpu && (dragging || committed) && begin < end;
    }

    [[nodiscard]] bool ActiveForGpu(ProfileViewerGpuViewportKey _key) const noexcept {
        return domain == ERangeDomain::Gpu && gpu_key == _key && (dragging || committed) && begin < end;
    }
};

[[nodiscard]] const char* LoadPhaseText(ProfileDocumentLoadPhase _phase) noexcept {
    switch (_phase) {
        case ProfileDocumentLoadPhase::Idle:
            return "Idle";
        case ProfileDocumentLoadPhase::Opening:
            return "Opening";
        case ProfileDocumentLoadPhase::Reading:
            return "Reading (indeterminate)";
        case ProfileDocumentLoadPhase::MaterializingSession:
            return "Materializing session";
        case ProfileDocumentLoadPhase::BuildingTimeline:
            return "Building timeline";
        case ProfileDocumentLoadPhase::Ready:
            return "Ready";
        case ProfileDocumentLoadPhase::Failed:
            return "Failed";
        case ProfileDocumentLoadPhase::Cancelled:
            return "Cancelled";
        case ProfileDocumentLoadPhase::Shutdown:
            return "Shutdown";
    }
    return "Unknown";
}

[[nodiscard]] const char* LoadDiagnosticText(ProfileDocumentLoadDiagnostic _diagnostic) noexcept {
    switch (_diagnostic) {
        case ProfileDocumentLoadDiagnostic::None:
            return "None";
        case ProfileDocumentLoadDiagnostic::WorkerUnavailable:
            return "Loader worker unavailable";
        case ProfileDocumentLoadDiagnostic::Cancelled:
            return "Load cancelled";
        case ProfileDocumentLoadDiagnostic::SessionLoadFailed:
            return "Profile session load failed";
        case ProfileDocumentLoadDiagnostic::TimelineBuildFailed:
            return "Timeline index build failed";
        case ProfileDocumentLoadDiagnostic::DocumentAllocationFailed:
            return "Profile document allocation failed";
    }
    return "Unknown";
}

[[nodiscard]] const char* LogicalQueueText(ProfileLogicalQueue _queue) noexcept {
    switch (_queue) {
        case ProfileLogicalQueue::Graphics:
            return "Graphics";
        case ProfileLogicalQueue::Compute:
            return "Compute";
        case ProfileLogicalQueue::Copy:
            return "Copy";
    }
    return "Unknown";
}

[[nodiscard]] const char* GpuFrameStatusText(ProfileGpuFrameStatus _status) noexcept {
    switch (_status) {
        case ProfileGpuFrameStatus::Complete:
            return "Complete";
        case ProfileGpuFrameStatus::Incomplete:
            return "Incomplete";
        case ProfileGpuFrameStatus::Invalid:
            return "Invalid";
    }
    return "Unknown";
}

[[nodiscard]] std::string PathForDisplay(const std::filesystem::path& _path) noexcept {
    try {
        const std::u8string utf8 = _path.generic_u8string();
        return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    } catch (...) {
        return "<path unavailable>";
    }
}

[[nodiscard]] std::string_view
BoundedUtf8Prefix(std::string_view _text, std::size_t _maximum_bytes) noexcept {
    if (_text.size() <= _maximum_bytes) {
        return _text;
    }

    std::size_t end = _maximum_bytes;
    while (end > 0 && end < _text.size() && (static_cast<unsigned char>(_text[end]) & 0xc0u) == 0x80u) {
        --end;
    }
    return _text.substr(0, end);
}

void DrawBoundedText(std::string_view _text, std::size_t _maximum_bytes = k_draw_name_byte_limit) {
    const std::string_view bounded = BoundedUtf8Prefix(_text, _maximum_bytes);
    ImGui::TextUnformatted(bounded.data(), bounded.data() + bounded.size());
    if (bounded.size() < _text.size()) {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextUnformatted("...");
    }
}

[[nodiscard]] char FoldAscii(char _value) noexcept {
    const unsigned char byte = static_cast<unsigned char>(_value);
    return byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z') ?
               static_cast<char>(byte + static_cast<unsigned char>('a' - 'A')) :
               _value;
}

class NameFilterMatcher final {
public:
    void Compile(std::string_view _filter) noexcept {
        _filter         = BoundedUtf8Prefix(_filter, k_filter_capacity - 1);
        pattern_length_ = _filter.size();
        for (std::size_t index = 0; index < pattern_length_; ++index) {
            pattern_[index] = FoldAscii(_filter[index]);
        }

        prefix_.fill(0);
        for (std::size_t index = 1, matched = 0; index < pattern_length_; ++index) {
            while (matched > 0 && pattern_[index] != pattern_[matched]) {
                matched = prefix_[matched - 1];
            }
            if (pattern_[index] == pattern_[matched]) {
                ++matched;
            }
            prefix_[index] = matched;
        }
    }

    [[nodiscard]] bool Empty() const noexcept {
        return pattern_length_ == 0;
    }

    // KMP keeps each bounded name check linear even for adversarial repetitive
    // names and filters. Non-ASCII UTF-8 bytes remain exact-match bytes.
    [[nodiscard]] bool Matches(std::string_view _name) const noexcept {
        if (Empty()) {
            return true;
        }

        _name = BoundedUtf8Prefix(_name, k_filter_name_byte_limit);
        for (std::size_t index = 0, matched = 0; index < _name.size(); ++index) {
            const char value = FoldAscii(_name[index]);
            while (matched > 0 && value != pattern_[matched]) {
                matched = prefix_[matched - 1];
            }
            if (value == pattern_[matched]) {
                ++matched;
            }
            if (matched == pattern_length_) {
                return true;
            }
        }
        return false;
    }

private:
    std::array<char, k_filter_capacity>        pattern_{};
    std::array<std::size_t, k_filter_capacity> prefix_{};
    std::size_t                                pattern_length_{0};
};

[[nodiscard]] ImU32 EventColor(std::string_view _name) noexcept {
    std::uint32_t hash    = 2166136261u;
    const auto    bounded = BoundedUtf8Prefix(_name, k_filter_name_byte_limit);
    for (const char value : bounded) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 16777619u;
    }

    const float hue   = static_cast<float>(hash % 360u) / 360.0f;
    float       red   = 0.0f;
    float       green = 0.0f;
    float       blue  = 0.0f;
    ImGui::ColorConvertHSVtoRGB(hue, 0.48f, 0.78f, red, green, blue);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(red, green, blue, 0.92f));
}

[[nodiscard]] std::uint64_t SaturatingIncrement(std::uint64_t _value) noexcept {
    return _value == std::numeric_limits<std::uint64_t>::max() ? _value : _value + 1;
}

[[nodiscard]] std::uint64_t IntervalExtent(std::uint64_t _begin, std::uint64_t _end) noexcept {
    return _end >= _begin ? _end - _begin : 0;
}

[[nodiscard]] std::uint64_t CpuDomainEnd(const ProfileSessionSummary& _summary) noexcept {
    if (!_summary.has_cpu_range || _summary.cpu_end_ns < _summary.cpu_begin_ns) {
        return 0;
    }
    return SaturatingIncrement(_summary.cpu_end_ns - _summary.cpu_begin_ns);
}

[[nodiscard]] std::uint64_t GpuDomainEnd(const GpuTimelineAxisFrame& _frame) noexcept {
    return _frame.timing_available ? SaturatingIncrement(_frame.extent_ticks) : 0;
}

[[nodiscard]] float
TimeToX(const ProfileViewerViewport& _viewport, std::uint64_t _time, float _x0, float _width) noexcept {
    if (!_viewport.valid || _viewport.view_end <= _viewport.view_begin || _width <= 0.0f) {
        return _x0;
    }
    const std::uint64_t clamped = std::clamp(_time, _viewport.view_begin, _viewport.view_end);
    const double        ratio   = static_cast<double>(clamped - _viewport.view_begin) /
                         static_cast<double>(_viewport.view_end - _viewport.view_begin);
    return _x0 + static_cast<float>(ratio) * _width;
}

[[nodiscard]] std::uint32_t AnchorNumerator(float _mouse_x, float _x0, float _width) noexcept;

[[nodiscard]] std::uint64_t
XToTime(const ProfileViewerViewport& _viewport, float _x, float _x0, float _width) noexcept {
    if (!_viewport.valid || _viewport.view_end <= _viewport.view_begin || _width <= 0.0f) {
        return _viewport.view_begin;
    }
    constexpr std::uint32_t denominator = 1'000'000;
    return ProfileViewerMapFraction(
        _viewport.view_begin, _viewport.view_end, AnchorNumerator(_x, _x0, _width), denominator
    );
}

[[nodiscard]] std::int64_t
PixelPanDelta(const ProfileViewerViewport& _viewport, float _pixel_delta, float _width) noexcept {
    if (!_viewport.valid || _viewport.view_end <= _viewport.view_begin || !std::isfinite(_pixel_delta) ||
        !std::isfinite(_width) || _width <= 0.0f) {
        return 0;
    }

    const long double span = static_cast<long double>(_viewport.view_end - _viewport.view_begin);
    const long double value =
        -static_cast<long double>(_pixel_delta) * span / static_cast<long double>(_width);
    if (!std::isfinite(value)) {
        return 0;
    }
    const long double minimum = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    const long double maximum = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    return static_cast<std::int64_t>(std::clamp(value, minimum, maximum));
}

[[nodiscard]] std::int64_t
KeyboardPanDelta(const ProfileViewerViewport& _viewport, bool _toward_end) noexcept {
    if (!_viewport.valid || _viewport.view_end <= _viewport.view_begin) {
        return 0;
    }

    const std::uint64_t span      = _viewport.view_end - _viewport.view_begin;
    const std::uint64_t magnitude = std::min<std::uint64_t>(
        std::max<std::uint64_t>(span / 10, 1),
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
    );
    const std::int64_t signed_magnitude = static_cast<std::int64_t>(magnitude);
    return _toward_end ? signed_magnitude : -signed_magnitude;
}

[[nodiscard]] std::uint32_t AnchorNumerator(float _mouse_x, float _x0, float _width) noexcept {
    constexpr std::uint32_t denominator = 1'000'000;
    if (!std::isfinite(_mouse_x) || !std::isfinite(_x0) || !std::isfinite(_width) || _width <= 0.0f) {
        return denominator / 2;
    }

    const double ratio = std::clamp(
        (static_cast<double>(_mouse_x) - static_cast<double>(_x0)) / static_cast<double>(_width), 0.0, 1.0
    );
    return static_cast<std::uint32_t>(ratio * denominator);
}

void DrawTimeRuler(
    const ProfileViewerViewport& _viewport,
    float                        _x0,
    float                        _width,
    float                        _y,
    double                       _unit_ns
) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddLine(ImVec2(_x0, _y), ImVec2(_x0 + _width, _y), IM_COL32(170, 170, 170, 255));

    const float bounded_tick_count = std::isfinite(_width) ? std::clamp(_width / 130.0f, 3.0f, 10.0f) : 3.0f;
    const int   tick_count         = static_cast<int>(bounded_tick_count);
    for (int tick = 0; tick <= tick_count; ++tick) {
        const double        ratio    = static_cast<double>(tick) / static_cast<double>(tick_count);
        const float         x        = _x0 + static_cast<float>(ratio) * _width;
        const std::uint64_t position = ProfileViewerMapFraction(
            _viewport.view_begin,
            _viewport.view_end,
            static_cast<std::uint32_t>(tick),
            static_cast<std::uint32_t>(tick_count)
        );
        const double value_ns = static_cast<double>(position) * _unit_ns;

        char label[48]{};
        if (value_ns >= 1.0e9) {
            std::snprintf(label, sizeof(label), "%.3fs", value_ns / 1.0e9);
        } else if (value_ns >= 1.0e6) {
            std::snprintf(label, sizeof(label), "%.3fms", value_ns / 1.0e6);
        } else if (value_ns >= 1.0e3) {
            std::snprintf(label, sizeof(label), "%.3fus", value_ns / 1.0e3);
        } else {
            std::snprintf(label, sizeof(label), "%.0fns", value_ns);
        }
        draw_list->AddLine(ImVec2(x, _y), ImVec2(x, _y + 6.0f), IM_COL32(190, 190, 190, 255));
        draw_list->AddText(ImVec2(x + 2.0f, _y + 7.0f), IM_COL32(205, 205, 205, 255), label);
    }
}

void DrawOverview(const ProfileViewerViewport& _viewport, float _x0, float _width, float _height) {
    ImDrawList*  draw_list = ImGui::GetWindowDrawList();
    const ImVec2 origin    = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##ProfileOverview", ImVec2(_x0 + _width - origin.x, _height));

    const float y0 = origin.y + 7.0f;
    const float y1 = y0 + 16.0f;
    draw_list->AddRectFilled(ImVec2(_x0, y0), ImVec2(_x0 + _width, y1), IM_COL32(28, 28, 31, 255), 3.0f);
    if (_viewport.valid && _viewport.domain_end > 0) {
        const double begin_ratio =
            static_cast<double>(_viewport.view_begin) / static_cast<double>(_viewport.domain_end);
        const double end_ratio =
            static_cast<double>(_viewport.view_end) / static_cast<double>(_viewport.domain_end);
        const float view_x0 = _x0 + static_cast<float>(std::clamp(begin_ratio, 0.0, 1.0)) * _width;
        const float view_x1 = _x0 + static_cast<float>(std::clamp(end_ratio, 0.0, 1.0)) * _width;
        draw_list->AddRectFilled(
            ImVec2(view_x0, y0 - 2.0f), ImVec2(view_x1, y1 + 2.0f), IM_COL32(225, 179, 72, 62), 3.0f
        );
        draw_list->AddRect(
            ImVec2(view_x0, y0 - 2.0f), ImVec2(view_x1, y1 + 2.0f), IM_COL32(225, 179, 72, 230), 3.0f
        );
    }
}

void DrawQuality(const ProfileTimelineQuality& _quality) {
    if (_quality.Clean()) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.52f, 1.0f), "Capture quality: clean");
        return;
    }

    ImGui::TextColored(ImVec4(1.0f, 0.68f, 0.22f, 1.0f), "Capture quality warnings:");
    if (_quality.Has(TimelineQualityFlag::ForensicTruncated)) {
        ImGui::BulletText("Forensic/truncated input");
    }
    if (_quality.Has(TimelineQualityFlag::LostRecords)) {
        ImGui::BulletText(
            "Notified lost records: %llu", static_cast<unsigned long long>(_quality.lost_record_count)
        );
    }
    if (_quality.Has(TimelineQualityFlag::UnnotifiedDrops)) {
        ImGui::BulletText(
            "Unnotified drops: %llu", static_cast<unsigned long long>(_quality.unnotified_drop_count)
        );
    }
    if (_quality.Has(TimelineQualityFlag::CpuOrphans)) {
        ImGui::BulletText(
            "CPU orphan scopes: %llu", static_cast<unsigned long long>(_quality.orphan_cpu_scope_count)
        );
    }
    if (_quality.Has(TimelineQualityFlag::GpuOrphans)) {
        ImGui::BulletText(
            "GPU orphan scopes: %llu", static_cast<unsigned long long>(_quality.orphan_gpu_scope_count)
        );
    }
    if (_quality.Has(TimelineQualityFlag::DegradedGpuFrames)) {
        ImGui::BulletText(
            "Degraded complete GPU frames: %llu",
            static_cast<unsigned long long>(_quality.degraded_complete_gpu_frame_count)
        );
    }
    if (_quality.Has(TimelineQualityFlag::IncompleteGpuFrames)) {
        ImGui::BulletText(
            "Incomplete GPU frames: %llu",
            static_cast<unsigned long long>(_quality.incomplete_gpu_frame_count)
        );
    }
    if (_quality.Has(TimelineQualityFlag::InvalidGpuFrames)) {
        ImGui::BulletText(
            "Invalid GPU frames: %llu", static_cast<unsigned long long>(_quality.invalid_gpu_frame_count)
        );
    }
    if (_quality.Has(TimelineQualityFlag::GpuScopeErrors)) {
        ImGui::BulletText(
            "GPU scope errors: %llu", static_cast<unsigned long long>(_quality.error_gpu_scope_count)
        );
    }
    if (_quality.Has(TimelineQualityFlag::UntrustedGpuTiming)) {
        ImGui::BulletText(
            "Untrusted GPU frames: %llu", static_cast<unsigned long long>(_quality.untrusted_gpu_frame_count)
        );
    }
    if (_quality.Has(TimelineQualityFlag::UnknownRecords)) {
        ImGui::BulletText(
            "Unknown records: %llu", static_cast<unsigned long long>(_quality.unknown_record_count)
        );
    }
}

void DrawParentName(const ProfileSession& _session, const CpuScopeRecord& _scope) {
    ImGui::TextUnformatted("Parent:");
    ImGui::SameLine();
    if (_scope.parent_index != kInvalidSessionIndex && _scope.parent_index < _session.CpuScopes().size()) {
        DrawBoundedText(_session.String(_session.CpuScopes()[_scope.parent_index].name));
    } else {
        ImGui::TextUnformatted("None");
    }
}

void DrawParentName(const ProfileSession& _session, const GpuScopeRecord& _scope) {
    ImGui::TextUnformatted("Parent:");
    ImGui::SameLine();
    if (_scope.parent_index != kInvalidSessionIndex && _scope.parent_index < _session.GpuScopes().size()) {
        DrawBoundedText(_session.String(_session.GpuScopes()[_scope.parent_index].name));
    } else {
        ImGui::TextUnformatted("None");
    }
}

[[nodiscard]] bool OpenProfileDump(std::filesystem::path& _selected_path, std::string& _status) {
    NFD::UniquePath                      selected_path = nullptr;
    const std::array<nfdfilteritem_t, 1> filters       = {{
        {"Moer Profile Dump", "mpd"},
    }};
    const nfdresult_t result = NFD::OpenDialog(selected_path, filters.data(), filters.size());
    if (result == NFD_CANCEL) {
        _status = "Open cancelled.";
        return false;
    }
    if (result != NFD_OKAY || !selected_path) {
        const char* error = NFD_GetError();
        _status           = error ? error : "Native file dialog failed.";
        LOG_ERROR("[ProfileViewerUI] NFD error: {}", _status);
        return false;
    }

    try {
        _selected_path = std::filesystem::u8path(selected_path.get());
        _status        = "Load requested.";
        return true;
    } catch (...) {
        _status = "Selected profile path is invalid.";
        return false;
    }
}

[[nodiscard]] bool IsTerminalPhase(ProfileDocumentLoadPhase _phase) noexcept {
    return _phase == ProfileDocumentLoadPhase::Ready || _phase == ProfileDocumentLoadPhase::Failed ||
           _phase == ProfileDocumentLoadPhase::Cancelled || _phase == ProfileDocumentLoadPhase::Shutdown;
}

} // namespace

struct ProfileViewerUI::Impl {
    explicit Impl(ProfileDocumentLoader& _loader) : loader(_loader) {}

    ProfileDocumentLoader& loader;
    ProfileViewerModel     model{};

    struct PublicationResetState {
        std::vector<std::uint8_t>  cpu_track_visibility{};
        std::vector<std::uint8_t>  gpu_track_visibility{};
        std::vector<std::uint32_t> cpu_visible_tracks{};
        std::vector<std::uint32_t> gpu_visible_tracks{};
        std::uint64_t              selected_gpu_frame{0};
    };

    std::shared_ptr<const ProfileDocument> displayed_document{};
    std::uint64_t                          publication_generation{kInvalidProfileDocumentGeneration};
    std::uint64_t                          failed_publication_generation{kInvalidProfileDocumentGeneration};
    std::vector<std::uint8_t>              cpu_track_visibility{};
    std::vector<std::uint8_t>              gpu_track_visibility{};
    std::vector<std::uint32_t>             cpu_visible_tracks{};
    std::vector<std::uint32_t>             gpu_visible_tracks{};
    std::array<char, k_filter_capacity>    filter{};
    NameFilterMatcher                      name_filter{};
    std::array<CpuTimelineScopeRef, k_query_capacity> cpu_query_output{};
    std::array<GpuTimelineScopeRef, k_query_capacity> gpu_query_output{};
    RangeSelection                                    range{};
    std::uint64_t                                     selected_gpu_frame{0};
    std::string                                       dialog_status{};

    [[nodiscard]] PublicationResetState PreparePublication(const ProfileDocument& _document) const {
        PublicationResetState prepared;
        prepared.cpu_track_visibility.assign(
            std::min(_document.timeline_index.CpuTracks().size(), k_track_scan_budget), 1
        );
        prepared.gpu_track_visibility.assign(
            std::min(_document.timeline_index.GpuTracks().size(), k_track_scan_budget), 1
        );
        prepared.cpu_visible_tracks.reserve(prepared.cpu_track_visibility.size());
        prepared.gpu_visible_tracks.reserve(prepared.gpu_track_visibility.size());

        const auto first_gpu_frame = FindProfileViewerGpuFrameAtOrAfter(
            _document.timeline_index.GpuFrames(), _document.timeline_index.GpuAxisFrames(), 0
        );
        prepared.selected_gpu_frame = first_gpu_frame ? first_gpu_frame->frame_id : 0;
        return prepared;
    }

    void CommitPublication(
        const std::shared_ptr<const ProfileDocument>& _document,
        PublicationResetState&&                       _prepared
    ) noexcept {
        displayed_document     = _document;
        publication_generation = _document->request_generation;
        cpu_track_visibility   = std::move(_prepared.cpu_track_visibility);
        gpu_track_visibility   = std::move(_prepared.gpu_track_visibility);
        cpu_visible_tracks     = std::move(_prepared.cpu_visible_tracks);
        gpu_visible_tracks     = std::move(_prepared.gpu_visible_tracks);
        selected_gpu_frame     = _prepared.selected_gpu_frame;
        filter.fill('\0');
        name_filter.Compile({});
        range.Clear();
    }

    void ClearPublication() {
        displayed_document.reset();
        publication_generation = kInvalidProfileDocumentGeneration;
        cpu_track_visibility.clear();
        gpu_track_visibility.clear();
        cpu_visible_tracks.clear();
        gpu_visible_tracks.clear();
        filter.fill('\0');
        name_filter.Compile({});
        range.Clear();
        selected_gpu_frame = 0;
    }

    void ReportPublicationPreparationFailure(const char* _detail) noexcept {
        try {
            LOG_ERROR(
                "[ProfileViewerUI] Publication preparation failed; preserving last-good state: {}",
                _detail != nullptr && _detail[0] != '\0' ? _detail : "unknown error"
            );
        } catch (...) {
        }
    }

    void ReportLoadRequestFailure(const char* _detail) noexcept {
        try {
            dialog_status = "Load request failed";
            if (_detail != nullptr && _detail[0] != '\0') {
                dialog_status.append(": ");
                dialog_status.append(_detail);
            }
        } catch (...) {
            dialog_status.clear();
        }
        try {
            LOG_ERROR(
                "[ProfileViewerUI] Load request failed: {}",
                _detail != nullptr && _detail[0] != '\0' ? _detail : "unknown error"
            );
        } catch (...) {
        }
    }

    void DrawLoadStatus(const ProfileDocumentLoaderSnapshot& _snapshot) {
        if (ImGui::Button("Open .mpd...")) {
            std::filesystem::path selected_path;
            if (OpenProfileDump(selected_path, dialog_status)) {
                try {
                    const std::uint64_t generation = loader.RequestLoad(selected_path);
                    if (generation == kInvalidProfileDocumentGeneration) {
                        dialog_status = "Loader rejected the request.";
                    }
                } catch (const std::exception& error) {
                    ReportLoadRequestFailure(error.what());
                } catch (...) {
                    ReportLoadRequestFailure("unknown exception");
                }
            }
        }

        const bool cancellable = _snapshot.accepting_requests && !IsTerminalPhase(_snapshot.phase) &&
                                 _snapshot.request_generation != kInvalidProfileDocumentGeneration;
        ImGui::SameLine();
        if (!cancellable) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Cancel load")) {
            dialog_status = loader.CancelLatest() ? "Cancellation requested." : "No cancellable request.";
        }
        if (!cancellable) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        ImGui::Text(
            "Phase: %s | request %llu | published %llu",
            LoadPhaseText(_snapshot.phase),
            static_cast<unsigned long long>(_snapshot.request_generation),
            static_cast<unsigned long long>(_snapshot.published_generation)
        );

        if (!_snapshot.latest_attempt_path.empty()) {
            ImGui::TextUnformatted("Latest attempt:");
            ImGui::SameLine();
            const std::string path = PathForDisplay(_snapshot.latest_attempt_path);
            DrawBoundedText(path, 2048);
        }
        if (_snapshot.phase == ProfileDocumentLoadPhase::Reading) {
            ImGui::TextDisabled("Reading progress is indeterminate; observed file size is metadata only.");
        } else if (_snapshot.reported_input_bytes_final) {
            ImGui::Text("Input bytes: %llu", static_cast<unsigned long long>(_snapshot.reported_input_bytes));
        }
        if (_snapshot.diagnostic != ProfileDocumentLoadDiagnostic::None) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.42f, 0.32f, 1.0f),
                "Latest attempt: %s",
                LoadDiagnosticText(_snapshot.diagnostic)
            );
        }
        if (!dialog_status.empty()) {
            ImGui::TextDisabled("%s", dialog_status.c_str());
        }
    }

    void DrawDocumentHeader(const ProfileDocument& _document) {
        const std::string path = PathForDisplay(_document.source_path);
        ImGui::TextUnformatted("Displayed document:");
        ImGui::SameLine();
        DrawBoundedText(path, 2048);

        const ProfileSessionSummary& summary = _document.session.Summary();
        ImGui::Text(
            "CPU scopes %llu | GPU frames %llu | GPU scopes %llu | model %llu bytes | index %llu bytes",
            static_cast<unsigned long long>(summary.cpu_scope_count),
            static_cast<unsigned long long>(summary.gpu_frame_count),
            static_cast<unsigned long long>(summary.gpu_scope_count),
            static_cast<unsigned long long>(summary.logical_model_bytes),
            static_cast<unsigned long long>(_document.timeline_index.BuildResult().logical_bytes)
        );
        if (ImGui::CollapsingHeader("Capture quality")) {
            DrawQuality(_document.timeline_index.Quality());
        }
    }

    void
    DrawFilterAndVisibility(std::span<const CpuTimelineTrackIndex> _tracks, const ProfileSession& _session) {
        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::InputTextWithHint(
                "##CpuProfileFilter", "Filter scope name", filter.data(), filter.size()
            )) {
            name_filter.Compile(filter.data());
        }
        ImGui::SameLine();
        if (ImGui::Button("CPU tracks")) {
            ImGui::OpenPopup("CpuProfileTrackVisibility");
        }
        if (ImGui::BeginPopup("CpuProfileTrackVisibility")) {
            const std::size_t track_count = std::min(_tracks.size(), cpu_track_visibility.size());
            for (std::size_t index = 0; index < track_count; ++index) {
                const CpuTimelineTrackIndex& timeline_track = _tracks[index];
                const CpuTrack& source_track = _session.CpuTracks()[timeline_track.source_track_index];
                bool visible = index < cpu_track_visibility.size() && cpu_track_visibility[index] != 0;
                char label[96]{};
                std::snprintf(
                    label,
                    sizeof(label),
                    "Thread %llu##CpuTrack%llu",
                    static_cast<unsigned long long>(source_track.thread_id),
                    static_cast<unsigned long long>(index)
                );
                if (ImGui::Checkbox(label, &visible) && index < cpu_track_visibility.size()) {
                    cpu_track_visibility[index] = visible ? 1 : 0;
                }
            }
            if (_tracks.size() > track_count) {
                ImGui::TextDisabled("Track list truncated to the fixed 4096-track viewer budget.");
            }
            ImGui::EndPopup();
        }
    }

    void DrawFilterAndVisibility(
        std::span<const GpuTimelineTrackIndex> _tracks,
        const ProfileSession&                  _session,
        const ProfileTimelineIndex&            _index,
        std::uint32_t                          _axis_index,
        std::uint64_t                          _frame_id
    ) {
        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::InputTextWithHint(
                "##GpuProfileFilter", "Filter scope name", filter.data(), filter.size()
            )) {
            name_filter.Compile(filter.data());
        }
        ImGui::SameLine();
        if (ImGui::Button("GPU tracks")) {
            ImGui::OpenPopup("GpuProfileTrackVisibility");
        }
        if (ImGui::BeginPopup("GpuProfileTrackVisibility")) {
            const std::size_t track_count = std::min(_tracks.size(), gpu_track_visibility.size());
            for (std::size_t index = 0; index < track_count; ++index) {
                if (_tracks[index].axis_index != _axis_index ||
                    _index.FindGpuFrameSlice(static_cast<std::uint32_t>(index), _frame_id) == nullptr) {
                    continue;
                }
                const GpuTrack& source_track = _session.GpuTracks()[_tracks[index].source_track_index];
                bool visible = index < gpu_track_visibility.size() && gpu_track_visibility[index] != 0;
                char label[128]{};
                std::snprintf(
                    label,
                    sizeof(label),
                    "%s native %u##GpuTrack%llu",
                    LogicalQueueText(source_track.logical_queue),
                    source_track.native_queue_id,
                    static_cast<unsigned long long>(index)
                );
                if (ImGui::Checkbox(label, &visible) && index < gpu_track_visibility.size()) {
                    gpu_track_visibility[index] = visible ? 1 : 0;
                }
            }
            if (_tracks.size() > track_count) {
                ImGui::TextDisabled("Track list truncated to the fixed 4096-track viewer budget.");
            }
            ImGui::EndPopup();
        }
    }

    void DrawCpuTooltip(
        const ProfileSession& _session,
        const CpuScopeRecord& _scope,
        std::uint64_t         _relative_begin,
        std::uint64_t         _relative_end
    ) {
        ImGui::BeginTooltip();
        DrawBoundedText(_session.String(_scope.name));
        ImGui::Separator();
        ImGui::Text("Depth: %u", _scope.depth);
        ImGui::Text(
            "Range: %llu - %llu ns",
            static_cast<unsigned long long>(_relative_begin),
            static_cast<unsigned long long>(_relative_end)
        );
        ImGui::Text(
            "Duration: %.3f us", static_cast<double>(IntervalExtent(_relative_begin, _relative_end)) / 1000.0
        );
        DrawParentName(_session, _scope);
        ImGui::EndTooltip();
    }

    void DrawGpuTooltip(
        const ProfileSession& _session,
        const GpuScopeRecord& _scope,
        std::uint64_t         _frame_id,
        std::uint64_t         _relative_begin,
        std::uint64_t         _relative_end,
        double                _unit_period_ns
    ) {
        ImGui::BeginTooltip();
        DrawBoundedText(_session.String(_scope.name));
        ImGui::Separator();
        ImGui::Text("Depth: %u", _scope.depth);
        ImGui::Text("Frame: %llu", static_cast<unsigned long long>(_frame_id));
        ImGui::Text(
            "Tick offsets: %llu - %llu",
            static_cast<unsigned long long>(_relative_begin),
            static_cast<unsigned long long>(_relative_end)
        );
        ImGui::Text(
            "Duration: %.3f us",
            static_cast<double>(IntervalExtent(_relative_begin, _relative_end)) * _unit_period_ns / 1000.0
        );
        DrawParentName(_session, _scope);
        ImGui::EndTooltip();
    }

    void DrawSelectionDetails(const ProfileDocument& _document) {
        if (!ImGui::CollapsingHeader("Selection details", ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }

        const ProfileViewerSelection selection = model.Selection();
        if (!selection.Valid() || selection.published_generation != publication_generation ||
            selection.published_generation != _document.request_generation) {
            ImGui::TextDisabled("No scope selected. Click a scope to keep its details here.");
            return;
        }

        if (ImGui::SmallButton("Clear selection")) {
            model.ClearSelection();
            return;
        }
        ImGui::SameLine();
        ImGui::Text(
            "%s scope | timeline track %llu",
            selection.kind == EProfileViewerSelectionKind::CpuScope ? "CPU" : "GPU",
            static_cast<unsigned long long>(selection.timeline_track_index)
        );

        const ProfileSession& session = _document.session;
        if (selection.kind == EProfileViewerSelectionKind::CpuScope) {
            if (selection.source_scope_index >= session.CpuScopes().size()) {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Selected CPU scope is unavailable.");
                return;
            }

            const CpuScopeRecord& scope = session.CpuScopes()[selection.source_scope_index];
            ImGui::TextUnformatted("Name:");
            ImGui::SameLine();
            DrawBoundedText(session.String(scope.name));
            ImGui::Text(
                "Range: %llu - %llu ns | duration %.3f us",
                static_cast<unsigned long long>(selection.begin),
                static_cast<unsigned long long>(selection.end),
                static_cast<double>(IntervalExtent(selection.begin, selection.end)) / 1000.0
            );
            ImGui::Text("Depth: %u", scope.depth);
            DrawParentName(session, scope);
            return;
        }

        if (selection.kind != EProfileViewerSelectionKind::GpuScope ||
            selection.source_scope_index >= session.GpuScopes().size()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Selected GPU scope is unavailable.");
            return;
        }

        const GpuScopeRecord& scope = session.GpuScopes()[selection.source_scope_index];
        ImGui::TextUnformatted("Name:");
        ImGui::SameLine();
        DrawBoundedText(session.String(scope.name));
        ImGui::Text(
            "Frame %llu | physical axis %u | tick offsets %llu - %llu",
            static_cast<unsigned long long>(selection.frame_id),
            selection.axis_index,
            static_cast<unsigned long long>(selection.begin),
            static_cast<unsigned long long>(selection.end)
        );
        if (selection.axis_index < _document.timeline_index.Axes().size()) {
            const double unit_period_ns =
                _document.timeline_index.Axes()[selection.axis_index].unit_period_ns;
            ImGui::Text(
                "Duration: %.3f us",
                static_cast<double>(IntervalExtent(selection.begin, selection.end)) * unit_period_ns / 1000.0
            );
        }
        ImGui::Text("Depth: %u", scope.depth);
        DrawParentName(session, scope);
    }

    void HandleCpuNavigation(
        const ProfileViewerViewport& _viewport,
        float                        _timeline_x0,
        float                        _timeline_width,
        bool                         _hovered
    ) {
        if (!_hovered || !_viewport.valid) {
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f && !io.KeyShift) {
            constexpr std::uint32_t denominator = 1'000'000;
            const std::uint32_t     anchor = AnchorNumerator(io.MousePos.x, _timeline_x0, _timeline_width);
            if (io.MouseWheel > 0.0f) {
                static_cast<void>(model.ZoomCpu(anchor, denominator, 4, 5));
            } else {
                static_cast<void>(model.ZoomCpu(anchor, denominator, 5, 4));
            }
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            static_cast<void>(model.PanCpu(PixelPanDelta(_viewport, io.MouseDelta.x, _timeline_width)));
        } else if (io.MouseWheel != 0.0f && io.KeyShift) {
            const float pixels = io.MouseWheel * _timeline_width * 0.12f;
            static_cast<void>(model.PanCpu(PixelPanDelta(_viewport, pixels, _timeline_width)));
        }
        if (!ImGui::IsAnyItemActive()) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
                static_cast<void>(model.PanCpu(KeyboardPanDelta(_viewport, false)));
            } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
                static_cast<void>(model.PanCpu(KeyboardPanDelta(_viewport, true)));
            }
        }
    }

    void HandleGpuNavigation(
        ProfileViewerGpuViewportKey  _key,
        const ProfileViewerViewport& _viewport,
        float                        _timeline_x0,
        float                        _timeline_width,
        bool                         _hovered
    ) {
        if (!_hovered || !_viewport.valid) {
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f && !io.KeyShift) {
            constexpr std::uint32_t denominator = 1'000'000;
            const std::uint32_t     anchor = AnchorNumerator(io.MousePos.x, _timeline_x0, _timeline_width);
            if (io.MouseWheel > 0.0f) {
                static_cast<void>(model.ZoomGpu(_key, anchor, denominator, 4, 5));
            } else {
                static_cast<void>(model.ZoomGpu(_key, anchor, denominator, 5, 4));
            }
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            static_cast<void>(model.PanGpu(_key, PixelPanDelta(_viewport, io.MouseDelta.x, _timeline_width)));
        } else if (io.MouseWheel != 0.0f && io.KeyShift) {
            const float pixels = io.MouseWheel * _timeline_width * 0.12f;
            static_cast<void>(model.PanGpu(_key, PixelPanDelta(_viewport, pixels, _timeline_width)));
        }
        if (!ImGui::IsAnyItemActive()) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
                static_cast<void>(model.PanGpu(_key, KeyboardPanDelta(_viewport, false)));
            } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
                static_cast<void>(model.PanGpu(_key, KeyboardPanDelta(_viewport, true)));
            }
        }
    }

    void HandleRangeSelection(
        ERangeDomain                 _domain,
        ProfileViewerGpuViewportKey  _gpu_key,
        const ProfileViewerViewport& _viewport,
        float                        _timeline_x0,
        float                        _timeline_width,
        bool                         _body_hovered,
        bool                         _event_hovered
    ) {
        if (!_viewport.valid) {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (_body_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            range.Clear();
            model.ClearSelection();
            return;
        }
        if (_body_hovered && !_event_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            range.domain    = _domain;
            range.gpu_key   = _gpu_key;
            range.dragging  = true;
            range.committed = false;
            range.anchor    = XToTime(_viewport, io.MousePos.x, _timeline_x0, _timeline_width);
            range.begin     = range.anchor;
            range.end       = range.anchor;
        }

        const bool same_domain =
            range.domain == _domain && (_domain == ERangeDomain::Cpu || range.gpu_key == _gpu_key);
        if (same_domain && range.dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const std::uint64_t current = XToTime(_viewport, io.MousePos.x, _timeline_x0, _timeline_width);
            range.begin                 = std::min(range.anchor, current);
            range.end                   = std::max(range.anchor, current);
        }
        // A release can happen while this tab/window is not drawn. Finishing on
        // the first observed mouse-up prevents a permanently dragging range.
        if (same_domain && range.dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            range.dragging  = false;
            range.committed = range.begin < range.end;
        }
    }

    void DrawRangeOverlay(
        const ProfileViewerViewport& _viewport,
        bool                         _active,
        float                        _x0,
        float                        _width,
        float                        _y0,
        float                        _y1
    ) const {
        if (!_active || _y1 <= _y0) {
            return;
        }
        const float x0        = TimeToX(_viewport, range.begin, _x0, _width);
        const float x1        = TimeToX(_viewport, range.end, _x0, _width);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(ImVec2(x0, _y0), ImVec2(x1, _y1), IM_COL32(92, 156, 206, 38));
        draw_list->AddRect(ImVec2(x0, _y0), ImVec2(x1, _y1), IM_COL32(132, 202, 245, 190));
    }

    void DrawCpu(const ProfileDocument& _document) {
        const ProfileSession&       session    = _document.session;
        const ProfileTimelineIndex& index      = _document.timeline_index;
        const auto                  tracks     = index.CpuTracks();
        const std::uint64_t         domain_end = CpuDomainEnd(session.Summary());

        ProfileViewerViewport viewport = model.CpuViewport();
        if ((!viewport.valid || viewport.domain_end != domain_end) && domain_end > 0) {
            static_cast<void>(model.FitCpu(domain_end));
            viewport = model.CpuViewport();
        }

        if (ImGui::Button("Fit CPU")) {
            static_cast<void>(model.FitCpu(domain_end));
            viewport = model.CpuViewport();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("CPU steady-clock axis; never aligned to GPU.");
        DrawFilterAndVisibility(tracks, session);
        if (tracks.size() > cpu_track_visibility.size()) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.66f, 0.22f, 1.0f),
                "CPU track discovery truncated to the fixed 4096-track viewer budget."
            );
        }

        if (!viewport.valid || tracks.empty()) {
            ImGui::TextDisabled("No CPU timeline data.");
            return;
        }

        const ProfileViewerSelection selection = model.Selection();
        if (selection.kind == EProfileViewerSelectionKind::CpuScope &&
            selection.published_generation == publication_generation &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
            ImGui::IsKeyPressed(ImGuiKey_F)) {
            const std::uint64_t margin =
                std::max<std::uint64_t>(IntervalExtent(selection.begin, selection.end) / 5, 1);
            static_cast<void>(model.FocusCpu(selection.begin, selection.end, margin));
            viewport = model.CpuViewport();
        }

        const float available_width = std::max(ImGui::GetContentRegionAvail().x, 360.0f);
        const float timeline_x0     = ImGui::GetCursorScreenPos().x + k_track_label_width;
        const float timeline_width  = std::max(100.0f, available_width - k_track_label_width);
        DrawTimeRuler(viewport, timeline_x0, timeline_width, ImGui::GetCursorScreenPos().y, 1.0);
        ImGui::Dummy(ImVec2(available_width, 30.0f));

        DrawOverview(viewport, timeline_x0, timeline_width, 32.0f);
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            constexpr std::uint32_t denominator = 1'000'000;
            const std::uint64_t     target      = ProfileViewerMapFraction(
                0,
                viewport.domain_end,
                AnchorNumerator(ImGui::GetIO().MousePos.x, timeline_x0, timeline_width),
                denominator
            );
            const std::uint64_t span = viewport.view_end - viewport.view_begin;
            static_cast<void>(
                model.FocusCpu(target, SaturatingIncrement(target), std::max(span / 2, std::uint64_t{1}))
            );
            viewport = model.CpuViewport();
        }

        cpu_visible_tracks.clear();
        const std::size_t cpu_track_count = std::min(tracks.size(), cpu_track_visibility.size());
        for (std::size_t index_value = 0; index_value < cpu_track_count; ++index_value) {
            if (cpu_track_visibility[index_value] != 0) {
                cpu_visible_tracks.emplace_back(static_cast<std::uint32_t>(index_value));
            }
        }

        ImGui::BeginChild(
            "CpuProfileTimelineRows", ImVec2(0.0f, std::max(180.0f, ImGui::GetContentRegionAvail().y)), true
        );
        const ImVec2 body_origin               = ImGui::GetCursorScreenPos();
        const float  body_width                = std::max(ImGui::GetContentRegionAvail().x, 360.0f);
        const float  body_x0                   = body_origin.x + k_track_label_width;
        const float  body_timeline_width       = std::max(100.0f, body_width - k_track_label_width);
        bool         event_hovered             = false;
        bool         query_truncated           = false;
        bool         filter_results_incomplete = false;
        std::size_t  remaining_budget          = k_frame_event_budget;

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(cpu_visible_tracks.size()), k_track_row_height);
        while (clipper.Step()) {
            for (int row_index = clipper.DisplayStart; row_index < clipper.DisplayEnd; ++row_index) {
                const std::uint32_t timeline_track_index =
                    cpu_visible_tracks[static_cast<std::size_t>(row_index)];
                const CpuTimelineTrackIndex& track        = tracks[timeline_track_index];
                const CpuTrack&              source_track = session.CpuTracks()[track.source_track_index];
                const ImVec2                 row_origin   = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(body_width, k_track_row_height));

                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                draw_list->AddRectFilled(
                    row_origin,
                    ImVec2(row_origin.x + k_track_label_width, row_origin.y + k_track_row_height),
                    IM_COL32(31, 31, 34, 255)
                );
                draw_list->AddRectFilled(
                    ImVec2(body_x0, row_origin.y),
                    ImVec2(body_x0 + body_timeline_width, row_origin.y + k_track_row_height),
                    IM_COL32(24, 24, 27, 255)
                );
                draw_list->AddLine(
                    ImVec2(row_origin.x, row_origin.y + k_track_row_height),
                    ImVec2(row_origin.x + body_width, row_origin.y + k_track_row_height),
                    IM_COL32(70, 70, 74, 255)
                );
                char track_label[128]{};
                std::snprintf(
                    track_label,
                    sizeof(track_label),
                    "CPU thread %llu | depth %u%s",
                    static_cast<unsigned long long>(source_track.thread_id),
                    track.max_depth,
                    track.max_depth >= k_direct_depth_lanes ? " (overflow lane)" : ""
                );
                draw_list->AddText(
                    ImVec2(row_origin.x + 7.0f, row_origin.y + 4.0f),
                    IM_COL32(220, 220, 220, 255),
                    track_label
                );

                if (remaining_budget == 0) {
                    query_truncated           = true;
                    filter_results_incomplete = filter_results_incomplete || !name_filter.Empty();
                    continue;
                }
                const std::size_t output_capacity =
                    std::min<std::size_t>(remaining_budget, cpu_query_output.size());
                const TimelineOverlapQueryResult query = index.QueryCpuTimelineOverlaps(
                    timeline_track_index,
                    viewport.view_begin,
                    viewport.view_end,
                    std::span<CpuTimelineScopeRef>(cpu_query_output).first(output_capacity)
                );
                if (!query.valid) {
                    continue;
                }
                query_truncated = query_truncated || query.truncated;
                filter_results_incomplete =
                    filter_results_incomplete || (query.truncated && !name_filter.Empty());
                remaining_budget -= static_cast<std::size_t>(query.written);

                for (std::uint64_t result_index = 0; result_index < query.written; ++result_index) {
                    const CpuTimelineScopeRef& timeline_scope =
                        cpu_query_output[static_cast<std::size_t>(result_index)];
                    const std::uint64_t source_index = timeline_scope.source_scope_index;
                    if (source_index >= session.CpuScopes().size()) {
                        continue;
                    }
                    const CpuScopeRecord&  scope = session.CpuScopes()[source_index];
                    const std::string_view name  = session.String(scope.name);
                    filter_results_incomplete =
                        filter_results_incomplete ||
                        (!name_filter.Empty() && name.size() > k_filter_name_byte_limit);
                    if (!name_filter.Matches(name)) {
                        continue;
                    }
                    const std::uint64_t begin = timeline_scope.begin_ns;
                    const std::uint64_t end   = timeline_scope.end_ns;
                    float               x0    = TimeToX(viewport, begin, body_x0, body_timeline_width);
                    float               x1    = TimeToX(viewport, end, body_x0, body_timeline_width);
                    if (x1 - x0 < 2.0f) {
                        x1 = std::min(body_x0 + body_timeline_width, x0 + 2.0f);
                    }
                    const std::uint32_t lane = std::min(scope.depth, k_overflow_depth_lane);
                    const float         y0   = row_origin.y + 3.0f + lane * k_lane_height;
                    const float         y1   = y0 + k_lane_height - 2.0f;
                    if (x1 <= body_x0 || x0 >= body_x0 + body_timeline_width) {
                        continue;
                    }

                    const ImVec2 rect_min(std::max(x0, body_x0), y0);
                    const ImVec2 rect_max(std::min(x1, body_x0 + body_timeline_width), y1);
                    draw_list->AddRectFilled(rect_min, rect_max, EventColor(name), 2.0f);
                    const bool selected = selection.kind == EProfileViewerSelectionKind::CpuScope &&
                                          selection.published_generation == publication_generation &&
                                          selection.source_scope_index == source_index;
                    if (selected) {
                        draw_list->AddRect(rect_min, rect_max, IM_COL32(255, 232, 80, 255), 2.0f, 0, 2.0f);
                    }
                    if (rect_max.x - rect_min.x >= 28.0f) {
                        const std::string_view bounded = BoundedUtf8Prefix(name, k_draw_name_byte_limit);
                        draw_list->PushClipRect(rect_min, rect_max, true);
                        draw_list->AddText(
                            ImVec2(rect_min.x + 3.0f, rect_min.y + 1.0f),
                            IM_COL32(238, 238, 238, 255),
                            bounded.data(),
                            bounded.data() + bounded.size()
                        );
                        draw_list->PopClipRect();
                    }

                    if (ImGui::IsMouseHoveringRect(rect_min, rect_max, true)) {
                        event_hovered = true;
                        DrawCpuTooltip(session, scope, begin, end);
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            static_cast<void>(model.SelectCpu(
                                timeline_track_index, source_index, begin, std::max(begin, end)
                            ));
                        }
                    }
                }
            }
        }

        const bool body_hovered = ImGui::IsWindowHovered() && ImGui::GetIO().MousePos.x >= body_x0 &&
                                  ImGui::GetIO().MousePos.x <= body_x0 + body_timeline_width;
        HandleCpuNavigation(viewport, body_x0, body_timeline_width, body_hovered);
        HandleRangeSelection(
            ERangeDomain::Cpu, {}, viewport, body_x0, body_timeline_width, body_hovered, event_hovered
        );
        DrawRangeOverlay(
            viewport,
            range.ActiveForCpu(),
            body_x0,
            body_timeline_width,
            body_origin.y,
            ImGui::GetWindowPos().y + ImGui::GetWindowHeight()
        );
        ImGui::EndChild();

        if (filter_results_incomplete) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.66f, 0.22f, 1.0f),
                "Filter results are incomplete: the query budget was exhausted or a scope name exceeded the "
                "1024-byte filter scan limit."
            );
        } else if (query_truncated || remaining_budget == 0) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.66f, 0.22f, 1.0f),
                "Visible CPU scopes truncated (4096 per track / 16384 per frame budget)."
            );
        }
    }

    void DrawGpuFrameChooser(const ProfileSession& _session, const ProfileTimelineIndex& _index) {
        const auto recorded_frames = _index.GpuFrames();
        const auto axis_frames     = _index.GpuAxisFrames();
        auto current = FindProfileViewerGpuFrameAtOrAfter(recorded_frames, axis_frames, selected_gpu_frame);
        if (!current) {
            current = FindProfileViewerGpuFrameAtOrBefore(
                recorded_frames, axis_frames, std::numeric_limits<std::uint64_t>::max()
            );
        }
        if (!current) {
            return;
        }
        if (current->frame_id != selected_gpu_frame) {
            selected_gpu_frame = current->frame_id;
        }

        if (ImGui::Button("<##GpuFrame")) {
            const auto previous =
                FindProfileViewerGpuFrameBefore(recorded_frames, axis_frames, selected_gpu_frame);
            if (previous) {
                selected_gpu_frame = previous->frame_id;
                range.Clear();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(">##GpuFrame")) {
            const auto next =
                FindProfileViewerGpuFrameAfter(recorded_frames, axis_frames, selected_gpu_frame);
            if (next) {
                selected_gpu_frame = next->frame_id;
                range.Clear();
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        std::uint64_t requested_frame = selected_gpu_frame;
        if (ImGui::InputScalar(
                "GPU frame",
                ImGuiDataType_U64,
                &requested_frame,
                nullptr,
                nullptr,
                "%llu",
                ImGuiInputTextFlags_EnterReturnsTrue
            )) {
            auto requested =
                FindProfileViewerGpuFrameAtOrAfter(recorded_frames, axis_frames, requested_frame);
            if (!requested) {
                requested = FindProfileViewerGpuFrameAtOrBefore(
                    recorded_frames, axis_frames, std::numeric_limits<std::uint64_t>::max()
                );
            }
            if (requested) {
                selected_gpu_frame = requested->frame_id;
                range.Clear();
            }
        }

        const GpuTimelineFrameRef* frame_ref = _index.FindGpuFrame(selected_gpu_frame);
        if (frame_ref != nullptr && frame_ref->source_frame_index < _session.GpuFrames().size()) {
            const GpuFrameRecord& frame = _session.GpuFrames()[frame_ref->source_frame_index];
            ImGui::SameLine();
            ImGui::Text(
                "%s | scopes %llu | dropped %llu | errors %llu",
                GpuFrameStatusText(frame.status),
                static_cast<unsigned long long>(frame.scope_count),
                static_cast<unsigned long long>(frame.dropped_scope_count),
                static_cast<unsigned long long>(frame.error_scope_count)
            );
            if (!frame.materialization_complete || !frame.timing_topology_trusted) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.66f, 0.22f, 1.0f), "Frame timing is incomplete or topology-untrusted."
                );
            }
        } else {
            ImGui::SameLine();
            ImGui::TextColored(
                ImVec4(1.0f, 0.58f, 0.30f, 1.0f),
                "Orphaned timeline | no GpuFrame record; metadata comes from indexed scopes only"
            );
        }
    }

    void DrawGpuAxis(
        const ProfileDocument&      _document,
        std::uint32_t               _axis_index,
        const GpuTimelineAxisFrame& _axis_frame
    ) {
        const ProfileSession&             session = _document.session;
        const ProfileTimelineIndex&       index   = _document.timeline_index;
        const TimelineAxis&               axis    = index.Axes()[_axis_index];
        const ProfileViewerGpuViewportKey key{
            .axis_index = _axis_index,
            .frame_id   = selected_gpu_frame,
        };
        const std::uint64_t domain_end = GpuDomainEnd(_axis_frame);

        ImGui::Text(
            "Physical domain: native queue %u, family %u, valid bits %u, %.6f ns/tick",
            axis.native_queue_id,
            axis.family_id,
            axis.valid_bits,
            axis.unit_period_ns
        );
        ImGui::SameLine();
        ImGui::TextDisabled("| frame-local only; not calibrated to CPU, other domains, or other frames");
        if (!_axis_frame.timing_available || domain_end == 0) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.50f, 0.30f, 1.0f),
                "No valid timing for this physical axis/frame; error-only scopes are not drawn."
            );
            ImGui::Text(
                "Ready scopes %llu | error scopes %llu | capability trusted: %s | topology trusted: %s",
                static_cast<unsigned long long>(_axis_frame.ready_scope_count),
                static_cast<unsigned long long>(_axis_frame.error_scope_count),
                _axis_frame.timing_capability_trusted ? "yes" : "no",
                _axis_frame.timing_topology_trusted ? "yes" : "no"
            );
            return;
        }

        std::optional<ProfileViewerViewport> viewport = model.FindGpuViewport(key);
        if (!viewport || !viewport->valid || viewport->domain_end != domain_end) {
            static_cast<void>(model.FitGpu(key, domain_end));
            viewport = model.FindGpuViewport(key);
        }
        if (!viewport) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "GPU viewport capacity exhausted.");
            return;
        }

        if (ImGui::Button("Fit GPU axis/frame")) {
            static_cast<void>(model.FitGpu(key, domain_end));
            viewport = model.FindGpuViewport(key);
        }
        ImGui::SameLine();
        ImGui::Text(
            "Ready %llu | errors %llu | materialized %s | topology trusted %s",
            static_cast<unsigned long long>(_axis_frame.ready_scope_count),
            static_cast<unsigned long long>(_axis_frame.error_scope_count),
            _axis_frame.materialization_complete ? "yes" : "no",
            _axis_frame.timing_topology_trusted ? "yes" : "no"
        );
        DrawFilterAndVisibility(index.GpuTracks(), session, index, _axis_index, selected_gpu_frame);
        if (index.GpuTracks().size() > gpu_track_visibility.size()) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.66f, 0.22f, 1.0f),
                "GPU track discovery truncated to the fixed 4096-track viewer budget."
            );
        }

        const ProfileViewerSelection selection = model.Selection();
        if (selection.kind == EProfileViewerSelectionKind::GpuScope &&
            selection.published_generation == publication_generation && selection.axis_index == _axis_index &&
            selection.frame_id == selected_gpu_frame &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
            ImGui::IsKeyPressed(ImGuiKey_F)) {
            const std::uint64_t margin =
                std::max<std::uint64_t>(IntervalExtent(selection.begin, selection.end) / 5, 1);
            static_cast<void>(model.FocusGpu(key, selection.begin, selection.end, margin));
            viewport = model.FindGpuViewport(key);
        }
        if (!viewport) {
            return;
        }

        const float available_width = std::max(ImGui::GetContentRegionAvail().x, 360.0f);
        const float timeline_x0     = ImGui::GetCursorScreenPos().x + k_track_label_width;
        const float timeline_width  = std::max(100.0f, available_width - k_track_label_width);
        DrawTimeRuler(
            *viewport, timeline_x0, timeline_width, ImGui::GetCursorScreenPos().y, axis.unit_period_ns
        );
        ImGui::Dummy(ImVec2(available_width, 30.0f));
        DrawOverview(*viewport, timeline_x0, timeline_width, 32.0f);
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            constexpr std::uint32_t denominator = 1'000'000;
            const std::uint64_t     target      = ProfileViewerMapFraction(
                0,
                viewport->domain_end,
                AnchorNumerator(ImGui::GetIO().MousePos.x, timeline_x0, timeline_width),
                denominator
            );
            const std::uint64_t span = viewport->view_end - viewport->view_begin;
            static_cast<void>(
                model.FocusGpu(key, target, SaturatingIncrement(target), std::max(span / 2, std::uint64_t{1}))
            );
            viewport = model.FindGpuViewport(key);
        }
        if (!viewport) {
            return;
        }

        const auto gpu_tracks = index.GpuTracks();
        gpu_visible_tracks.clear();
        const std::size_t gpu_track_count = std::min(gpu_tracks.size(), gpu_track_visibility.size());
        for (std::size_t track_index = 0; track_index < gpu_track_count; ++track_index) {
            if (gpu_tracks[track_index].axis_index == _axis_index && gpu_track_visibility[track_index] != 0 &&
                index.FindGpuFrameSlice(static_cast<std::uint32_t>(track_index), selected_gpu_frame) !=
                    nullptr) {
                gpu_visible_tracks.emplace_back(static_cast<std::uint32_t>(track_index));
            }
        }

        char child_name[96]{};
        std::snprintf(
            child_name,
            sizeof(child_name),
            "GpuProfileTimelineRows##%u_%llu",
            _axis_index,
            static_cast<unsigned long long>(selected_gpu_frame)
        );
        ImGui::BeginChild(child_name, ImVec2(0.0f, std::max(180.0f, ImGui::GetContentRegionAvail().y)), true);
        const ImVec2 body_origin               = ImGui::GetCursorScreenPos();
        const float  body_width                = std::max(ImGui::GetContentRegionAvail().x, 360.0f);
        const float  body_x0                   = body_origin.x + k_track_label_width;
        const float  body_timeline_width       = std::max(100.0f, body_width - k_track_label_width);
        bool         event_hovered             = false;
        bool         query_truncated           = false;
        bool         filter_results_incomplete = false;
        std::size_t  remaining_budget          = k_frame_event_budget;

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(gpu_visible_tracks.size()), k_track_row_height);
        while (clipper.Step()) {
            for (int row_index = clipper.DisplayStart; row_index < clipper.DisplayEnd; ++row_index) {
                const std::uint32_t timeline_track_index =
                    gpu_visible_tracks[static_cast<std::size_t>(row_index)];
                const GpuTimelineTrackIndex& track        = gpu_tracks[timeline_track_index];
                const GpuTrack&              source_track = session.GpuTracks()[track.source_track_index];
                const GpuTimelineFrameSlice* slice =
                    index.FindGpuFrameSlice(timeline_track_index, selected_gpu_frame);
                const ImVec2 row_origin = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(body_width, k_track_row_height));

                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                draw_list->AddRectFilled(
                    row_origin,
                    ImVec2(row_origin.x + k_track_label_width, row_origin.y + k_track_row_height),
                    IM_COL32(31, 31, 34, 255)
                );
                draw_list->AddRectFilled(
                    ImVec2(body_x0, row_origin.y),
                    ImVec2(body_x0 + body_timeline_width, row_origin.y + k_track_row_height),
                    IM_COL32(24, 24, 27, 255)
                );
                draw_list->AddLine(
                    ImVec2(row_origin.x, row_origin.y + k_track_row_height),
                    ImVec2(row_origin.x + body_width, row_origin.y + k_track_row_height),
                    IM_COL32(70, 70, 74, 255)
                );
                char track_label[160]{};
                std::snprintf(
                    track_label,
                    sizeof(track_label),
                    "%s | native %u/family %u | errors %llu",
                    LogicalQueueText(source_track.logical_queue),
                    source_track.native_queue_id,
                    source_track.family_id,
                    static_cast<unsigned long long>(slice ? slice->error_scope_count : 0)
                );
                draw_list->AddText(
                    ImVec2(row_origin.x + 7.0f, row_origin.y + 4.0f),
                    IM_COL32(220, 220, 220, 255),
                    track_label
                );

                if (remaining_budget == 0) {
                    query_truncated           = true;
                    filter_results_incomplete = filter_results_incomplete || !name_filter.Empty();
                    continue;
                }
                const std::size_t output_capacity =
                    std::min<std::size_t>(remaining_budget, gpu_query_output.size());
                const TimelineOverlapQueryResult query = index.QueryGpuTimelineOverlaps(
                    timeline_track_index,
                    selected_gpu_frame,
                    viewport->view_begin,
                    viewport->view_end,
                    std::span<GpuTimelineScopeRef>(gpu_query_output).first(output_capacity)
                );
                if (!query.valid) {
                    continue;
                }
                query_truncated = query_truncated || query.truncated;
                filter_results_incomplete =
                    filter_results_incomplete || (query.truncated && !name_filter.Empty());
                remaining_budget -= static_cast<std::size_t>(query.written);

                for (std::uint64_t result_index = 0; result_index < query.written; ++result_index) {
                    const GpuTimelineScopeRef& timeline_scope =
                        gpu_query_output[static_cast<std::size_t>(result_index)];
                    const std::uint64_t source_index = timeline_scope.source_scope_index;
                    if (source_index >= session.GpuScopes().size()) {
                        continue;
                    }
                    const GpuScopeRecord&  scope = session.GpuScopes()[source_index];
                    const std::string_view name  = session.String(scope.name);
                    filter_results_incomplete =
                        filter_results_incomplete ||
                        (!name_filter.Empty() && name.size() > k_filter_name_byte_limit);
                    if (!name_filter.Matches(name)) {
                        continue;
                    }
                    const std::uint64_t begin = timeline_scope.begin_tick_offset;
                    const std::uint64_t end   = timeline_scope.end_tick_offset;
                    if (begin > _axis_frame.extent_ticks || end > _axis_frame.extent_ticks) {
                        continue;
                    }
                    float x0 = TimeToX(*viewport, begin, body_x0, body_timeline_width);
                    float x1 = TimeToX(*viewport, end, body_x0, body_timeline_width);
                    if (x1 - x0 < 2.0f) {
                        x1 = std::min(body_x0 + body_timeline_width, x0 + 2.0f);
                    }
                    const std::uint32_t lane = std::min(scope.depth, k_overflow_depth_lane);
                    const float         y0   = row_origin.y + 3.0f + lane * k_lane_height;
                    const float         y1   = y0 + k_lane_height - 2.0f;
                    if (x1 <= body_x0 || x0 >= body_x0 + body_timeline_width) {
                        continue;
                    }

                    const ImVec2 rect_min(std::max(x0, body_x0), y0);
                    const ImVec2 rect_max(std::min(x1, body_x0 + body_timeline_width), y1);
                    draw_list->AddRectFilled(rect_min, rect_max, EventColor(name), 2.0f);
                    const bool selected = selection.kind == EProfileViewerSelectionKind::GpuScope &&
                                          selection.published_generation == publication_generation &&
                                          selection.axis_index == _axis_index &&
                                          selection.frame_id == selected_gpu_frame &&
                                          selection.source_scope_index == source_index;
                    if (selected) {
                        draw_list->AddRect(rect_min, rect_max, IM_COL32(255, 232, 80, 255), 2.0f, 0, 2.0f);
                    }
                    if (rect_max.x - rect_min.x >= 28.0f) {
                        const std::string_view bounded = BoundedUtf8Prefix(name, k_draw_name_byte_limit);
                        draw_list->PushClipRect(rect_min, rect_max, true);
                        draw_list->AddText(
                            ImVec2(rect_min.x + 3.0f, rect_min.y + 1.0f),
                            IM_COL32(238, 238, 238, 255),
                            bounded.data(),
                            bounded.data() + bounded.size()
                        );
                        draw_list->PopClipRect();
                    }
                    if (ImGui::IsMouseHoveringRect(rect_min, rect_max, true)) {
                        event_hovered = true;
                        DrawGpuTooltip(session, scope, selected_gpu_frame, begin, end, axis.unit_period_ns);
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            static_cast<void>(model.SelectGpu(
                                key, timeline_track_index, source_index, begin, std::max(begin, end)
                            ));
                        }
                    }
                }
            }
        }

        const bool body_hovered = ImGui::IsWindowHovered() && ImGui::GetIO().MousePos.x >= body_x0 &&
                                  ImGui::GetIO().MousePos.x <= body_x0 + body_timeline_width;
        HandleGpuNavigation(key, *viewport, body_x0, body_timeline_width, body_hovered);
        HandleRangeSelection(
            ERangeDomain::Gpu, key, *viewport, body_x0, body_timeline_width, body_hovered, event_hovered
        );
        DrawRangeOverlay(
            *viewport,
            range.ActiveForGpu(key),
            body_x0,
            body_timeline_width,
            body_origin.y,
            ImGui::GetWindowPos().y + ImGui::GetWindowHeight()
        );
        ImGui::EndChild();

        if (filter_results_incomplete) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.66f, 0.22f, 1.0f),
                "Filter results are incomplete: the query budget was exhausted or a scope name exceeded the "
                "1024-byte filter scan limit."
            );
        } else if (query_truncated || remaining_budget == 0) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.66f, 0.22f, 1.0f),
                "Visible GPU scopes truncated (4096 per track / 16384 per frame budget)."
            );
        }
    }

    void DrawGpu(const ProfileDocument& _document) {
        const ProfileSession&       session = _document.session;
        const ProfileTimelineIndex& index   = _document.timeline_index;
        if (!FindProfileViewerGpuFrameAtOrAfter(index.GpuFrames(), index.GpuAxisFrames(), 0)) {
            ImGui::TextDisabled("No GPU frames or indexed GPU timeline data.");
            return;
        }

        DrawGpuFrameChooser(session, index);
        ImGui::TextDisabled(
            "Each tab is one physical timestamp domain in one frame; tabs never share a ruler."
        );

        bool axis_available = false;
        if (ImGui::BeginTabBar("GpuPhysicalAxisTabs")) {
            const std::size_t axis_count = std::min(index.Axes().size(), k_axis_scan_budget + std::size_t{1});
            for (std::uint32_t axis_index = 1; axis_index < axis_count; ++axis_index) {
                const GpuTimelineAxisFrame* axis_frame =
                    index.FindGpuAxisFrame(axis_index, selected_gpu_frame);
                if (axis_frame == nullptr) {
                    continue;
                }
                axis_available           = true;
                const TimelineAxis& axis = index.Axes()[axis_index];
                char                label[128]{};
                std::snprintf(
                    label,
                    sizeof(label),
                    "Physical %u / family %u##Axis%u",
                    axis.native_queue_id,
                    axis.family_id,
                    axis_index
                );
                if (ImGui::BeginTabItem(label)) {
                    DrawGpuAxis(_document, axis_index, *axis_frame);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        if (index.Axes().size() > k_axis_scan_budget + std::size_t{1}) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.66f, 0.22f, 1.0f),
                "GPU physical-axis discovery truncated to the fixed 256-axis viewer budget."
            );
        }
        if (!axis_available) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.58f, 0.30f, 1.0f), "Selected frame has no physical GPU timestamp domain."
            );
        }
    }

    void ShowWindow(bool* _open) {
        // Exactly one coherent loader snapshot is taken for this UI frame.
        // Keep its shared document owner alive until this function returns.
        std::optional<ProfileDocumentLoaderSnapshot> snapshot_storage;
        try {
            snapshot_storage.emplace(loader.Snapshot());
        } catch (...) {
            const bool visible = ImGui::Begin("Profile Viewer", _open);
            if (visible) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.35f, 0.30f, 1.0f),
                    "Profile snapshot unavailable this frame; the last-good viewer state was preserved."
                );
            }
            ImGui::End();
            return;
        }

        const ProfileDocumentLoaderSnapshot&  snapshot = *snapshot_storage;
        const EProfileViewerPublicationUpdate update   = model.InspectSnapshot(snapshot);
        bool publication_prepare_failed                = update == EProfileViewerPublicationUpdate::Changed &&
                                          snapshot.document &&
                                          failed_publication_generation == snapshot.published_generation;

        // Publication is two-phase: all allocating UI state is prepared while
        // the model and displayed document still describe the last-good
        // generation. Only no-throw moves and model reset happen at commit.
        if (update == EProfileViewerPublicationUpdate::Changed && !publication_prepare_failed) {
            if (snapshot.document) {
                try {
                    PublicationResetState prepared = PreparePublication(*snapshot.document);
                    if (model.ObserveSnapshot(snapshot) == EProfileViewerPublicationUpdate::Changed) {
                        CommitPublication(snapshot.document, std::move(prepared));
                        failed_publication_generation = kInvalidProfileDocumentGeneration;
                    } else {
                        failed_publication_generation = snapshot.published_generation;
                        publication_prepare_failed    = true;
                        ReportPublicationPreparationFailure("snapshot changed before commit");
                    }
                } catch (const std::exception& error) {
                    failed_publication_generation = snapshot.published_generation;
                    publication_prepare_failed    = true;
                    ReportPublicationPreparationFailure(error.what());
                } catch (...) {
                    failed_publication_generation = snapshot.published_generation;
                    publication_prepare_failed    = true;
                    ReportPublicationPreparationFailure("unexpected allocation failure");
                }
            } else if (model.ObserveSnapshot(snapshot) == EProfileViewerPublicationUpdate::Changed) {
                ClearPublication();
                failed_publication_generation = kInvalidProfileDocumentGeneration;
            }
        }
        const std::shared_ptr<const ProfileDocument> document = displayed_document;

        if (!ImGui::Begin("Profile Viewer", _open)) {
            ImGui::End();
            return;
        }

        DrawLoadStatus(snapshot);
        ImGui::Separator();
        if (update == EProfileViewerPublicationUpdate::Invalid) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.35f, 0.30f, 1.0f), "Loader snapshot/document identity is invalid."
            );
        }
        if (publication_prepare_failed) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.35f, 0.30f, 1.0f),
                "New profile publication could not be prepared; the last-good viewer state was preserved."
            );
            ImGui::SameLine();
            if (ImGui::Button("Retry publication preparation")) {
                failed_publication_generation = kInvalidProfileDocumentGeneration;
            }
        }
        if (!document || !document->Valid() || !document->timeline_index.Matches(document->session)) {
            ImGui::TextDisabled("Open a valid .mpd capture to inspect its timeline.");
            ImGui::End();
            return;
        }

        DrawDocumentHeader(*document);
        DrawSelectionDetails(*document);
        ImGui::Separator();

        if (ImGui::BeginTabBar("ProfileViewerDomainTabs")) {
            if (ImGui::BeginTabItem("CPU")) {
                DrawCpu(*document);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("GPU")) {
                DrawGpu(*document);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }
};

ProfileViewerUI::ProfileViewerUI(ProfileDocumentLoader& _loader) : impl_(std::make_unique<Impl>(_loader)) {}

ProfileViewerUI::~ProfileViewerUI() = default;

void ProfileViewerUI::ShowWindow(bool* _open) {
    impl_->ShowWindow(_open);
}

} // namespace Moer
