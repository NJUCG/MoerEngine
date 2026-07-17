#include "StartupSplash.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwchar>

namespace Moer {
namespace {

using Clock = std::chrono::steady_clock;

constexpr wchar_t kWindowClassName[] = L"MoerEngineStartupSplashWindow";
constexpr wchar_t kWindowTitle[]     = L"MoerEngine Startup";

constexpr UINT kRefreshMessage = WM_APP + 0x51;
constexpr UINT kFinishMessage  = WM_APP + 0x52;
constexpr UINT kStopMessage    = WM_APP + 0x53;
constexpr UINT kKeepOpenMessage = WM_APP + 0x54;

constexpr UINT_PTR kAnimationTimerId = 1;
constexpr UINT     kAnimationPeriod  = 33;

constexpr int kLogicalWindowWidth  = 680;
constexpr int kLogicalWindowHeight = 380;

constexpr auto kMinimumVisibleTime = std::chrono::milliseconds(350);
constexpr auto kHandoffOverlapTime = std::chrono::milliseconds(350);

// The official NJU purple is specified for print as C50 M100 Y0 K40. The
// darker screen approximation anchors the brand while the brighter tints keep
// fine geometry readable on the near-black startup surface.
constexpr COLORREF kBackgroundTop    = RGB(23, 17, 32);
constexpr COLORREF kBackground       = RGB(14, 11, 21);
constexpr COLORREF kRaisedBackground = RGB(27, 21, 38);
constexpr COLORREF kNjuPurple        = RGB(77, 0, 153);
constexpr COLORREF kAccent           = RGB(139, 92, 246);
constexpr COLORREF kAccentLight      = RGB(196, 181, 253);
constexpr COLORREF kAccentDim        = RGB(50, 38, 83);
constexpr COLORREF kWarmGold         = RGB(214, 183, 106);
constexpr COLORREF kPrimaryText      = RGB(245, 243, 255);
constexpr COLORREF kSecondaryText    = RGB(184, 178, 200);
constexpr COLORREF kMutedText        = RGB(119, 112, 135);
constexpr COLORREF kFailure          = RGB(240, 106, 123);

int Scale(int value, UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

class ScopedGdiObject final {
public:
    explicit ScopedGdiObject(HGDIOBJ object = nullptr) noexcept
        : m_object(object) {}

    ~ScopedGdiObject() {
        if (m_object != nullptr) {
            DeleteObject(m_object);
        }
    }

    ScopedGdiObject(const ScopedGdiObject&)            = delete;
    ScopedGdiObject& operator=(const ScopedGdiObject&) = delete;

    [[nodiscard]] HGDIOBJ Get() const noexcept {
        return m_object;
    }

private:
    HGDIOBJ m_object = nullptr;
};

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }

    const auto byte_count = static_cast<int>((std::min)(
        text.size(),
        static_cast<std::size_t>((std::numeric_limits<int>::max)())
    ));

    auto convert = [text, byte_count](UINT code_page, DWORD flags) -> std::wstring {
        const int character_count = MultiByteToWideChar(
            code_page,
            flags,
            text.data(),
            byte_count,
            nullptr,
            0
        );
        if (character_count <= 0) {
            return {};
        }

        std::wstring result(static_cast<std::size_t>(character_count), L'\0');
        if (MultiByteToWideChar(
                code_page,
                flags,
                text.data(),
                byte_count,
                result.data(),
                character_count
            ) <= 0) {
            return {};
        }
        return result;
    };

    if (auto result = convert(CP_UTF8, MB_ERR_INVALID_CHARS); !result.empty()) {
        return result;
    }
    return convert(CP_ACP, 0);
}

struct SplashSnapshot {
    std::wstring title  = L"Starting MoerEditor";
    std::wstring detail = L"Preparing engine services";
    bool         failed = false;
};

class SharedSplashState final {
public:
    void Set(std::wstring title, std::wstring detail, bool failed) {
        std::scoped_lock lock(m_mutex);
        m_snapshot.title  = std::move(title);
        m_snapshot.detail = std::move(detail);
        m_snapshot.failed = failed;
    }

    [[nodiscard]] SplashSnapshot Read() const noexcept {
        try {
            std::scoped_lock lock(m_mutex);
            return m_snapshot;
        } catch (...) {
            return {};
        }
    }

private:
    mutable std::mutex m_mutex;
    SplashSnapshot     m_snapshot;
};

HFONT CreateUiFont(int point_size, int weight, UINT dpi) noexcept {
    return CreateFontW(
        -MulDiv(point_size, static_cast<int>(dpi), 72),
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );
}

struct WindowContext final {
    explicit WindowContext(std::shared_ptr<SharedSplashState> shared_state)
        : state(std::move(shared_state)), started_at(Clock::now()) {}

    ~WindowContext() {
        ResetFonts();
    }

    void ResetFonts() noexcept {
        if (product_font != nullptr) {
            DeleteObject(product_font);
            product_font = nullptr;
        }
        if (title_font != nullptr) {
            DeleteObject(title_font);
            title_font = nullptr;
        }
        if (body_font != nullptr) {
            DeleteObject(body_font);
            body_font = nullptr;
        }
        if (meta_font != nullptr) {
            DeleteObject(meta_font);
            meta_font = nullptr;
        }
    }

    void RecreateFonts(UINT new_dpi) noexcept {
        ResetFonts();
        dpi          = new_dpi == 0 ? USER_DEFAULT_SCREEN_DPI : new_dpi;
        product_font = CreateUiFont(26, FW_SEMIBOLD, dpi);
        title_font   = CreateUiFont(15, FW_SEMIBOLD, dpi);
        body_font    = CreateUiFont(10, FW_NORMAL, dpi);
        meta_font    = CreateUiFont(8, FW_SEMIBOLD, dpi);
    }

    std::shared_ptr<SharedSplashState> state;
    Clock::time_point                   started_at;
    UINT                                dpi            = USER_DEFAULT_SCREEN_DPI;
    std::uint32_t                       animation_tick = 0;
    BYTE                                opacity        = 0;
    bool                                finish_requested = false;
    bool                                fading_out       = false;
    std::optional<Clock::time_point>    finish_requested_at;
    HFONT                               product_font     = nullptr;
    HFONT                               title_font       = nullptr;
    HFONT                               body_font        = nullptr;
    HFONT                               meta_font        = nullptr;
};

WindowContext* GetWindowContext(HWND window) noexcept {
    return reinterpret_cast<WindowContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void FillSolidRect(HDC dc, const RECT& rect, COLORREF color) noexcept {
    ScopedGdiObject brush(CreateSolidBrush(color));
    FillRect(dc, &rect, static_cast<HBRUSH>(brush.Get()));
}

void SelectFont(HDC dc, HFONT font) noexcept {
    SelectObject(dc, font != nullptr ? font : GetStockObject(DEFAULT_GUI_FONT));
}

COLORREF BlendColor(COLORREF background, COLORREF foreground, int alpha) noexcept {
    alpha = (std::clamp)(alpha, 0, 255);
    const auto blend_channel = [alpha](int base, int accent) {
        return (base * (255 - alpha) + accent * alpha + 127) / 255;
    };
    return RGB(
        blend_channel(GetRValue(background), GetRValue(foreground)),
        blend_channel(GetGValue(background), GetGValue(foreground)),
        blend_channel(GetBValue(background), GetBValue(foreground))
    );
}

void FillGradientRect(
    HDC      dc,
    const RECT& rect,
    COLORREF first,
    COLORREF second,
    bool     horizontal
) noexcept {
    TRIVERTEX vertices[2]{};
    vertices[0].x     = rect.left;
    vertices[0].y     = rect.top;
    vertices[0].Red   = static_cast<COLOR16>(GetRValue(first) << 8);
    vertices[0].Green = static_cast<COLOR16>(GetGValue(first) << 8);
    vertices[0].Blue  = static_cast<COLOR16>(GetBValue(first) << 8);
    vertices[0].Alpha = 0xFF00;
    vertices[1].x     = rect.right;
    vertices[1].y     = rect.bottom;
    vertices[1].Red   = static_cast<COLOR16>(GetRValue(second) << 8);
    vertices[1].Green = static_cast<COLOR16>(GetGValue(second) << 8);
    vertices[1].Blue  = static_cast<COLOR16>(GetBValue(second) << 8);
    vertices[1].Alpha = 0xFF00;

    GRADIENT_RECT gradient_rect{0, 1};
    if (!GradientFill(
            dc,
            vertices,
            2,
            &gradient_rect,
            1,
            horizontal ? GRADIENT_FILL_RECT_H : GRADIENT_FILL_RECT_V
        )) {
        FillSolidRect(dc, rect, first);
    }
}

void DrawLine(
    HDC      dc,
    int      x0,
    int      y0,
    int      x1,
    int      y1,
    COLORREF color,
    int      thickness = 1
) noexcept {
    ScopedGdiObject pen(CreatePen(PS_SOLID, (std::max)(1, thickness), color));
    const auto old_pen = SelectObject(dc, pen.Get());
    MoveToEx(dc, x0, y0, nullptr);
    LineTo(dc, x1, y1);
    SelectObject(dc, old_pen);
}

void DrawRectOutline(HDC dc, const RECT& rect, COLORREF color, int thickness = 1) noexcept {
    ScopedGdiObject pen(CreatePen(PS_SOLID, (std::max)(1, thickness), color));
    const auto old_pen   = SelectObject(dc, pen.Get());
    const auto old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
}

void DrawSegmentedViewportFrame(
    HDC         dc,
    const RECT& client,
    UINT        dpi,
    bool        failed
) noexcept {
    const int width     = client.right - client.left;
    const int height    = client.bottom - client.top;
    const int thickness = (std::max)(1, Scale(1, dpi));
    const COLORREF active = failed ? kFailure : kAccent;

    DrawRectOutline(dc, RECT{0, 0, width - 1, height - 1}, BlendColor(kBackground, active, 62), thickness);
    for (int logical_inset = 2; logical_inset <= 5; ++logical_inset) {
        const int inset = Scale(logical_inset, dpi);
        DrawRectOutline(
            dc,
            RECT{inset, inset, width - inset, height - inset},
            BlendColor(kBackground, active, 30 - logical_inset * 4)
        );
    }

    const int edge   = Scale(3, dpi);
    const int corner = Scale(44, dpi);
    const COLORREF dim_corner = BlendColor(kBackground, active, 110);
    DrawLine(dc, edge, edge, edge + corner, edge, active, thickness);
    DrawLine(dc, edge, edge, edge, edge + corner, active, thickness);
    DrawLine(dc, width - edge, height - edge, width - edge - corner, height - edge, active, thickness);
    DrawLine(dc, width - edge, height - edge, width - edge, height - edge - corner, active, thickness);

    DrawLine(dc, width - edge, edge, width - edge - corner, edge, dim_corner, thickness);
    DrawLine(dc, width - edge, edge, width - edge, edge + corner, dim_corner, thickness);
    DrawLine(dc, edge, height - edge, edge + corner, height - edge, dim_corner, thickness);
    DrawLine(dc, edge, height - edge, edge, height - edge - corner, dim_corner, thickness);

    const int tick_y = Scale(3, dpi);
    for (int tick = 0; tick < 3; ++tick) {
        const int x = width - Scale(118 - tick * 16, dpi);
        DrawLine(dc, x, tick_y, x + Scale(7, dpi), tick_y, BlendColor(kBackground, active, 90 + tick * 28), thickness);
    }

    const int sample_size = (std::max)(Scale(4, dpi), 2);
    RECT sample{
        Scale(22, dpi),
        height - Scale(25, dpi),
        Scale(22, dpi) + sample_size,
        height - Scale(25, dpi) + sample_size
    };
    FillSolidRect(dc, sample, failed ? kFailure : kWarmGold);
}

void DrawRayField(
    HDC         dc,
    const RECT& client,
    const RECT& mark_rect,
    UINT        dpi,
    bool        failed
) noexcept {
    const COLORREF active = failed ? kFailure : kAccent;
    const COLORREF ray = BlendColor(kBackgroundTop, active, failed ? 34 : 20);
    const int origin_x = mark_rect.right - Scale(4, dpi);
    const int origin_y = mark_rect.top + (mark_rect.bottom - mark_rect.top) / 2;

    DrawLine(dc, origin_x, origin_y, client.right - Scale(90, dpi), Scale(18, dpi), ray);
    DrawLine(dc, origin_x, origin_y, client.right - Scale(52, dpi), Scale(66, dpi), ray);
    DrawLine(dc, origin_x, origin_y, client.right - Scale(112, dpi), Scale(126, dpi), ray);
    DrawLine(dc, origin_x, origin_y, mark_rect.right + Scale(255, dpi), Scale(128, dpi), ray);

    const int dot_size = (std::max)(Scale(2, dpi), 1);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            const int x = client.right - Scale(285 - column * 15, dpi);
            const int y = Scale(42 + row * 15, dpi);
            RECT dot{x, y, x + dot_size, y + dot_size};
            FillSolidRect(dc, dot, BlendColor(kRaisedBackground, active, 32 + (row + column) * 10));
        }
    }
}

void DrawRayframeMark(
    HDC                  dc,
    const RECT&          bounds,
    const WindowContext& context,
    bool                 failed
) noexcept {
    const UINT dpi = context.dpi;
    const COLORREF active = failed ? kFailure : kAccent;
    const std::uint32_t pulse_phase = context.animation_tick % 120U;
    const int pulse = static_cast<int>(pulse_phase <= 60U ? pulse_phase : 120U - pulse_phase);

    for (int layer = 0; layer < 4; ++layer) {
        const int inset = Scale(layer + 1, dpi);
        DrawRectOutline(
            dc,
            RECT{bounds.left + inset, bounds.top + inset, bounds.right - inset, bounds.bottom - inset},
            BlendColor(kRaisedBackground, active, 18 + pulse / 4 - layer * 3)
        );
    }

    const int bracket = Scale(18, dpi);
    const int thickness = (std::max)(1, Scale(2, dpi));
    DrawLine(dc, bounds.left, bounds.top, bounds.left + bracket, bounds.top, active, thickness);
    DrawLine(dc, bounds.left, bounds.top, bounds.left, bounds.top + bracket, active, thickness);
    DrawLine(dc, bounds.right, bounds.bottom, bounds.right - bracket, bounds.bottom, active, thickness);
    DrawLine(dc, bounds.right, bounds.bottom, bounds.right, bounds.bottom - bracket, active, thickness);
    const COLORREF dim = BlendColor(kRaisedBackground, active, 125);
    DrawLine(dc, bounds.right, bounds.top, bounds.right - bracket, bounds.top, dim, thickness);
    DrawLine(dc, bounds.right, bounds.top, bounds.right, bounds.top + bracket, dim, thickness);
    DrawLine(dc, bounds.left, bounds.bottom, bounds.left + bracket, bounds.bottom, dim, thickness);
    DrawLine(dc, bounds.left, bounds.bottom, bounds.left, bounds.bottom - bracket, dim, thickness);

    const int margin_x = Scale(13, dpi);
    const int margin_y = Scale(14, dpi);
    const RECT inner{
        bounds.left + margin_x,
        bounds.top + margin_y,
        bounds.right - margin_x,
        bounds.bottom - margin_y
    };
    const int inner_width  = inner.right - inner.left;
    const int inner_height = inner.bottom - inner.top;
    const auto x = [&inner, inner_width](int percent) {
        return inner.left + MulDiv(percent, inner_width, 100);
    };
    const auto y = [&inner, inner_height](int percent) {
        return inner.top + MulDiv(percent, inner_height, 100);
    };
    POINT mark_points[] = {
        {x(0), y(100)},
        {x(0), y(0)},
        {x(19), y(0)},
        {x(50), y(48)},
        {x(81), y(0)},
        {x(100), y(0)},
        {x(100), y(100)},
        {x(78), y(100)},
        {x(78), y(42)},
        {x(50), y(86)},
        {x(22), y(42)},
        {x(22), y(100)}
    };

    ScopedGdiObject mark_region(CreatePolygonRgn(mark_points, 12, WINDING));
    if (mark_region.Get() != nullptr) {
        const int saved_dc = SaveDC(dc);
        SelectClipRgn(dc, static_cast<HRGN>(mark_region.Get()));
        FillGradientRect(dc, inner, failed ? kFailure : kNjuPurple, failed ? kFailure : kAccentLight, true);
        RestoreDC(dc, saved_dc);
    } else {
        ScopedGdiObject mark_brush(CreateSolidBrush(active));
        const auto old_brush = SelectObject(dc, mark_brush.Get());
        const auto old_pen   = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, mark_points, 12);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
    }

    ScopedGdiObject outline_pen(CreatePen(PS_SOLID, (std::max)(1, Scale(1, dpi)), BlendColor(active, kAccentLight, 145)));
    const auto old_pen   = SelectObject(dc, outline_pen.Get());
    const auto old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Polygon(dc, mark_points, 12);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);

    const int sample_x    = x(50);
    const int sample_y    = y(48);
    const int sample_half = (std::max)(Scale(3, dpi), 2);
    POINT sample[] = {
        {sample_x, sample_y - sample_half},
        {sample_x + sample_half, sample_y},
        {sample_x, sample_y + sample_half},
        {sample_x - sample_half, sample_y}
    };
    ScopedGdiObject sample_brush(CreateSolidBrush(failed ? kFailure : kWarmGold));
    const auto old_sample_brush = SelectObject(dc, sample_brush.Get());
    const auto old_sample_pen   = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, sample, 4);
    SelectObject(dc, old_sample_pen);
    SelectObject(dc, old_sample_brush);
}

void DrawCometProgress(
    HDC                  dc,
    const RECT&          track,
    const WindowContext& context,
    bool                 failed
) noexcept {
    FillSolidRect(dc, track, failed ? BlendColor(kBackground, kFailure, 58) : kAccentDim);

    const int tick_width = (std::max)(Scale(1, context.dpi), 1);
    for (int x = track.left + Scale(48, context.dpi); x < track.right; x += Scale(48, context.dpi)) {
        RECT tick{x, track.top, x + tick_width, track.bottom};
        FillSolidRect(dc, tick, BlendColor(kAccentDim, failed ? kFailure : kAccent, 58));
    }

    if (failed) {
        RECT failure_segment{
            track.left,
            track.top,
            track.left + (track.right - track.left) / 5,
            track.bottom
        };
        FillSolidRect(dc, failure_segment, kFailure);
        return;
    }

    const int track_width   = track.right - track.left;
    const int segment_width = (std::max)(Scale(150, context.dpi), track_width / 4);
    const int travel        = track_width + segment_width;
    const int phase         = static_cast<int>(context.animation_tick % 110U);
    const int segment_left  = track.left - segment_width + (travel * phase) / 109;
    const int saved_dc      = SaveDC(dc);
    IntersectClipRect(dc, track.left, track.top, track.right, track.bottom);

    constexpr int tail_steps = 6;
    for (int step = 0; step < tail_steps; ++step) {
        const int left  = segment_left + (segment_width * step) / tail_steps;
        const int right = segment_left + (segment_width * (step + 1)) / tail_steps;
        RECT tail{left, track.top, right, track.bottom};
        FillSolidRect(dc, tail, BlendColor(kAccentDim, kAccent, 26 + step * 27));
    }

    const int body_left = segment_left + (segment_width * 2) / 3;
    RECT body{body_left, track.top, segment_left + segment_width, track.bottom};
    FillSolidRect(dc, body, kAccent);
    const int head_width = (std::max)(Scale(7, context.dpi), 2);
    RECT head{body.right - head_width, track.top, body.right, track.bottom};
    FillSolidRect(dc, head, kAccentLight);
    RestoreDC(dc, saved_dc);
}

void DrawSplashContent(HDC dc, const RECT& client, const WindowContext& context) noexcept {
    const SplashSnapshot snapshot = context.state->Read();
    const UINT           dpi      = context.dpi;
    const int            width    = client.right - client.left;
    const int            height   = client.bottom - client.top;
    const COLORREF       active   = snapshot.failed ? kFailure : kAccent;

    FillGradientRect(dc, client, kBackgroundTop, kBackground, false);

    RECT header_panel{0, 0, width, Scale(132, dpi)};
    FillGradientRect(
        dc,
        header_panel,
        BlendColor(kRaisedBackground, active, snapshot.failed ? 22 : 12),
        BlendColor(kBackground, active, 5),
        false
    );

    RECT mark_rect{
        Scale(49, dpi),
        Scale(28, dpi),
        Scale(117, dpi),
        Scale(96, dpi)
    };
    DrawRayField(dc, client, mark_rect, dpi, snapshot.failed);

    const int separator_y = Scale(128, dpi);
    DrawLine(
        dc,
        Scale(52, dpi),
        separator_y,
        width - Scale(52, dpi),
        separator_y,
        BlendColor(kBackground, active, 34)
    );
    DrawLine(
        dc,
        Scale(52, dpi),
        separator_y,
        Scale(124, dpi),
        separator_y,
        BlendColor(kBackground, active, 178),
        (std::max)(1, Scale(1, dpi))
    );

    DrawRayframeMark(dc, mark_rect, context, snapshot.failed);

    SetBkMode(dc, TRANSPARENT);

    RECT product_rect{
        Scale(137, dpi),
        Scale(32, dpi),
        width - Scale(190, dpi),
        Scale(73, dpi)
    };
    SetTextColor(dc, kPrimaryText);
    SelectFont(dc, context.product_font);
    DrawTextW(
        dc,
        L"MoerEngine",
        -1,
        &product_rect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX
    );

    RECT edition_rect{
        Scale(139, dpi),
        Scale(73, dpi),
        width - Scale(160, dpi),
        Scale(101, dpi)
    };
    SetTextColor(dc, snapshot.failed ? BlendColor(kMutedText, kFailure, 150) : kAccentLight);
    SelectFont(dc, context.meta_font);
    DrawTextW(
        dc,
        L"NJU GRAPHICS  /  RESEARCH RENDERER",
        -1,
        &edition_rect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX
    );

    RECT editor_rect{
        width - Scale(184, dpi),
        Scale(35, dpi),
        width - Scale(52, dpi),
        Scale(54, dpi)
    };
    SetTextColor(dc, kPrimaryText);
    DrawTextW(
        dc,
        L"EDITOR",
        -1,
        &editor_rect,
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX
    );

    RECT boot_rect{
        width - Scale(184, dpi),
        Scale(55, dpi),
        width - Scale(52, dpi),
        Scale(78, dpi)
    };
    SetTextColor(dc, kMutedText);
    DrawTextW(
        dc,
        snapshot.failed ? L"STARTUP FAULT" : L"BOOT SEQUENCE",
        -1,
        &boot_rect,
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX
    );

    const int status_line_x = Scale(52, dpi);
    DrawLine(
        dc,
        status_line_x,
        Scale(151, dpi),
        status_line_x,
        Scale(231, dpi),
        active,
        (std::max)(Scale(2, dpi), 2)
    );
    const int status_sample_size = (std::max)(Scale(4, dpi), 2);
    RECT status_sample{
        status_line_x - status_sample_size / 2,
        Scale(151, dpi) - status_sample_size / 2,
        status_line_x + (status_sample_size + 1) / 2,
        Scale(151, dpi) + (status_sample_size + 1) / 2
    };
    FillSolidRect(dc, status_sample, snapshot.failed ? kFailure : kWarmGold);

    RECT stage_label_rect{
        Scale(69, dpi),
        Scale(141, dpi),
        width - Scale(52, dpi),
        Scale(162, dpi)
    };
    SetTextColor(dc, snapshot.failed ? kFailure : kMutedText);
    SelectFont(dc, context.meta_font);
    DrawTextW(
        dc,
        snapshot.failed ? L"STARTUP STATUS  /  FAILED" : L"CURRENT STAGE",
        -1,
        &stage_label_rect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX
    );

    RECT title_rect{
        Scale(69, dpi),
        Scale(161, dpi),
        width - Scale(52, dpi),
        Scale(198, dpi)
    };
    SetTextColor(dc, snapshot.failed ? kFailure : kPrimaryText);
    SelectFont(dc, context.title_font);
    DrawTextW(
        dc,
        snapshot.title.empty() ? L"Starting MoerEditor" : snapshot.title.c_str(),
        -1,
        &title_rect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX
    );

    RECT detail_rect{
        Scale(69, dpi),
        Scale(204, dpi),
        width - Scale(52, dpi),
        height - Scale(92, dpi)
    };
    SetTextColor(dc, kSecondaryText);
    SelectFont(dc, context.body_font);
    DrawTextW(
        dc,
        snapshot.detail.empty() ? L"Please wait while the editor gets ready." : snapshot.detail.c_str(),
        -1,
        &detail_rect,
        DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX
    );

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - context.started_at
    );
    const auto minutes = elapsed.count() / 60000;
    const auto seconds = (elapsed.count() / 1000) % 60;
    const auto tenths  = (elapsed.count() / 100) % 10;
    wchar_t elapsed_text[64]{};
    swprintf_s(
        elapsed_text,
        sizeof(elapsed_text) / sizeof(elapsed_text[0]),
        L"ELAPSED  %02lld:%02lld.%01lld",
        static_cast<long long>(minutes),
        static_cast<long long>(seconds),
        static_cast<long long>(tenths)
    );

    RECT elapsed_rect{
        Scale(52, dpi),
        height - Scale(64, dpi),
        width / 2,
        height - Scale(40, dpi)
    };
    SetTextColor(dc, snapshot.failed ? kFailure : kMutedText);
    SelectFont(dc, context.meta_font);
    DrawTextW(
        dc,
        snapshot.failed ? L"STARTUP FAILED" : elapsed_text,
        -1,
        &elapsed_rect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX
    );

    RECT signature_rect{
        width / 2,
        height - Scale(64, dpi),
        width - Scale(52, dpi),
        height - Scale(40, dpi)
    };
    SetTextColor(
        dc,
        snapshot.failed ? BlendColor(kMutedText, kFailure, 150) : BlendColor(kMutedText, kAccent, 86)
    );
    DrawTextW(
        dc,
        L"RAYFRAME  /  STARTUP",
        -1,
        &signature_rect,
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX
    );

    const int bar_left   = Scale(52, dpi);
    const int bar_right  = width - Scale(52, dpi);
    const int bar_top    = height - Scale(27, dpi);
    const int bar_height = (std::max)(Scale(3, dpi), 2);
    RECT track{bar_left, bar_top, bar_right, bar_top + bar_height};
    DrawCometProgress(dc, track, context, snapshot.failed);

    DrawSegmentedViewportFrame(dc, client, dpi, snapshot.failed);
}

void PaintSplash(HWND window, WindowContext& context) noexcept {
    PAINTSTRUCT paint{};
    HDC         window_dc = BeginPaint(window, &paint);
    if (window_dc == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(window, &client);

    HDC memory_dc = CreateCompatibleDC(window_dc);
    if (memory_dc == nullptr) {
        DrawSplashContent(window_dc, client, context);
        EndPaint(window, &paint);
        return;
    }

    HBITMAP bitmap = CreateCompatibleBitmap(
        window_dc,
        (std::max)(1L, client.right - client.left),
        (std::max)(1L, client.bottom - client.top)
    );
    if (bitmap == nullptr) {
        DeleteDC(memory_dc);
        DrawSplashContent(window_dc, client, context);
        EndPaint(window, &paint);
        return;
    }

    const auto old_bitmap = SelectObject(memory_dc, bitmap);
    DrawSplashContent(memory_dc, client, context);
    BitBlt(
        window_dc,
        0,
        0,
        client.right - client.left,
        client.bottom - client.top,
        memory_dc,
        0,
        0,
        SRCCOPY
    );
    SelectObject(memory_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    EndPaint(window, &paint);
}

RECT CenteredWindowRect(UINT dpi) noexcept;
void RecenterSplashWindow(HWND window, UINT dpi) noexcept;

LRESULT CALLBACK StartupSplashWindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) noexcept {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams)
        );
    }

    WindowContext* context = GetWindowContext(window);

    switch (message) {
        case WM_CREATE:
            if (context != nullptr) {
                context->RecreateFonts(GetDpiForWindow(window));
            }
            SetTimer(window, kAnimationTimerId, kAnimationPeriod, nullptr);
            return 0;

        case WM_DPICHANGED: {
            const UINT new_dpi = HIWORD(w_param);
            if (context != nullptr) {
                context->RecreateFonts(new_dpi);
            }
            RecenterSplashWindow(window, new_dpi);
            return 0;
        }

        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            RecenterSplashWindow(window, GetDpiForWindow(window));
            return 0;

        case WM_TIMER:
            if (w_param == kAnimationTimerId && context != nullptr) {
                ++context->animation_tick;

                const auto now = Clock::now();
                if (context->finish_requested && context->finish_requested_at.has_value() &&
                    now - context->started_at >= kMinimumVisibleTime &&
                    now - *context->finish_requested_at >= kHandoffOverlapTime) {
                    context->fading_out = true;
                }

                if (context->fading_out) {
                    if (context->opacity <= 32) {
                        DestroyWindow(window);
                        return 0;
                    }
                    context->opacity = static_cast<BYTE>(context->opacity - 32);
                    SetLayeredWindowAttributes(window, 0, context->opacity, LWA_ALPHA);
                } else if (context->opacity < 255) {
                    const auto next_opacity = (std::min)(static_cast<int>(context->opacity) + 32, 255);
                    context->opacity        = static_cast<BYTE>(next_opacity);
                    SetLayeredWindowAttributes(window, 0, context->opacity, LWA_ALPHA);
                }

                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case kRefreshMessage:
            InvalidateRect(window, nullptr, FALSE);
            return 0;

        case kFinishMessage:
            if (context != nullptr) {
                context->finish_requested    = true;
                context->finish_requested_at = Clock::now();
            }
            return 0;

        case kKeepOpenMessage:
            if (context != nullptr) {
                context->finish_requested = false;
                context->fading_out       = false;
                context->finish_requested_at.reset();
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;

        case kStopMessage:
            DestroyWindow(window);
            return 0;

        case WM_NCHITTEST:
            // Match UE-style splash behavior: the window is a fixed startup
            // surface, not a draggable borderless application window.
            return HTCLIENT;

        case WM_SYSCOMMAND: {
            const WPARAM command = w_param & 0xFFF0U;
            if (command == SC_MOVE || command == SC_SIZE) {
                return 0;
            }
            break;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            if (context != nullptr) {
                PaintSplash(window, *context);
                return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            KillTimer(window, kAnimationTimerId);
            PostQuitMessage(0);
            return 0;

        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;

        default:
            break;
    }

    return DefWindowProcW(window, message, w_param, l_param);
}

bool EnsureWindowClassRegistered() noexcept {
    static std::once_flag register_once;
    static bool           registered = false;

    std::call_once(register_once, []() {
        WNDCLASSEXW window_class{};
        window_class.cbSize        = sizeof(window_class);
        window_class.style         = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc   = StartupSplashWindowProc;
        window_class.hInstance     = GetModuleHandleW(nullptr);
        window_class.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.hbrBackground = nullptr;
        window_class.lpszClassName = kWindowClassName;

        registered = RegisterClassExW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    });

    return registered;
}

UINT GetInitialDpi() noexcept {
    const UINT dpi = GetDpiForSystem();
    if (dpi > 0) {
        return dpi;
    }
    return USER_DEFAULT_SCREEN_DPI;
}

RECT CenteredWindowRect(UINT dpi) noexcept {
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    // MoerEditor currently targets the primary display by default. Keep the
    // splash anchored to the same display instead of following the cursor.
    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(monitor, &monitor_info)) {
        monitor_info.rcWork = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }

    const int available_width  = monitor_info.rcWork.right - monitor_info.rcWork.left;
    const int available_height = monitor_info.rcWork.bottom - monitor_info.rcWork.top;
    const int width            = (std::min)(Scale(kLogicalWindowWidth, dpi), available_width);
    const int height           = (std::min)(Scale(kLogicalWindowHeight, dpi), available_height);
    const int left             = monitor_info.rcWork.left + (available_width - width) / 2;
    const int top              = monitor_info.rcWork.top + (available_height - height) / 2;
    return RECT{left, top, left + width, top + height};
}

void RecenterSplashWindow(HWND window, UINT dpi) noexcept {
    const RECT centered = CenteredWindowRect(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi);
    SetWindowPos(
        window,
        nullptr,
        centered.left,
        centered.top,
        centered.right - centered.left,
        centered.bottom - centered.top,
        SWP_NOACTIVATE | SWP_NOZORDER
    );
}

class ThreadDpiScope final {
public:
    ThreadDpiScope() noexcept
        : m_previous(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}

    ~ThreadDpiScope() {
        if (m_previous != nullptr) {
            SetThreadDpiAwarenessContext(m_previous);
        }
    }

private:
    DPI_AWARENESS_CONTEXT m_previous = nullptr;
};

} // namespace

class StartupSplash::Impl final {
public:
    Impl()
        : m_state(std::make_shared<SharedSplashState>()) {}

    ~Impl() {
        StopAndJoin();
    }

    [[nodiscard]] bool Start(std::string_view title, std::string_view detail) noexcept {
        std::unique_lock lifecycle_lock(m_lifecycle_mutex);

        m_failed.store(false, std::memory_order_release);
        m_finish_requested.store(false, std::memory_order_release);
        SetStatus(title, detail, false);
        if (m_running.load(std::memory_order_acquire)) {
            PostToWindow(kKeepOpenMessage);
            return true;
        }

        if (m_ui_thread.joinable()) {
            m_ui_thread.join();
        }

        m_finish_requested.store(false, std::memory_order_release);
        m_stop_requested.store(false, std::memory_order_release);
        m_window.store(nullptr, std::memory_order_release);
        {
            std::scoped_lock ready_lock(m_ready_mutex);
            m_ready        = false;
            m_window_ready = false;
        }

        try {
            m_running.store(true, std::memory_order_release);
            m_ui_thread = std::thread([this]() noexcept { RunUiThread(); });
        } catch (...) {
            m_running.store(false, std::memory_order_release);
            return false;
        }

        lifecycle_lock.unlock();

        std::unique_lock ready_lock(m_ready_mutex);
        if (!m_ready_condition.wait_for(ready_lock, std::chrono::milliseconds(1500), [this]() { return m_ready; })) {
            // Do not allow a late-created splash to appear after the caller has
            // already fallen back to a visible main window.
            m_stop_requested.store(true, std::memory_order_release);
            PostToWindow(kStopMessage);
            return false;
        }
        return m_window_ready;
    }

    void Update(std::string_view title, std::string_view detail) noexcept {
        SetStatus(title, detail, m_failed.load(std::memory_order_acquire));
        PostToWindow(kRefreshMessage);
    }

    void Finish() noexcept {
        m_finish_requested.store(true, std::memory_order_release);
        PostToWindow(kFinishMessage);
    }

    void Fail(std::string_view title, std::string_view detail) noexcept {
        m_failed.store(true, std::memory_order_release);
        m_finish_requested.store(false, std::memory_order_release);
        SetStatus(title, detail, true);
        PostToWindow(kKeepOpenMessage);
    }

    [[nodiscard]] bool IsRunning() const noexcept {
        return m_running.load(std::memory_order_acquire);
    }

private:
    void SetStatus(std::string_view title, std::string_view detail, bool failed) noexcept {
        try {
            m_state->Set(Utf8ToWide(title), Utf8ToWide(detail), failed);
        } catch (...) {
            // A splash screen must never turn an allocation or encoding failure
            // into an editor startup failure. Keep the previous snapshot.
        }
    }

    void PostToWindow(UINT message) const noexcept {
        if (const HWND window = m_window.load(std::memory_order_acquire); window != nullptr) {
            PostMessageW(window, message, 0, 0);
        }
    }

    void SignalReady(bool window_ready) noexcept {
        {
            std::scoped_lock lock(m_ready_mutex);
            m_ready        = true;
            m_window_ready = window_ready;
        }
        m_ready_condition.notify_all();
    }

    void RunUiThread() noexcept {
        bool ready_signalled = false;
        try {
            ThreadDpiScope dpi_scope;
            if (!EnsureWindowClassRegistered()) {
                SignalReady(false);
                ready_signalled = true;
                m_running.store(false, std::memory_order_release);
                return;
            }

            WindowContext context(m_state);
            const UINT    dpi         = GetInitialDpi();
            const RECT    window_rect = CenteredWindowRect(dpi);
            HWND          window      = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                kWindowClassName,
                kWindowTitle,
                WS_POPUP,
                window_rect.left,
                window_rect.top,
                window_rect.right - window_rect.left,
                window_rect.bottom - window_rect.top,
                nullptr,
                nullptr,
                GetModuleHandleW(nullptr),
                &context
            );
            if (window == nullptr) {
                SignalReady(false);
                ready_signalled = true;
                m_running.store(false, std::memory_order_release);
                return;
            }

            // The process may start on a monitor whose effective DPI differs
            // from the system DPI used for the bootstrap rectangle. Resize and
            // re-center before the first ShowWindow so logical content is never
            // clipped on mixed-DPI desktops.
            RecenterSplashWindow(window, GetDpiForWindow(window));

            m_window.store(window, std::memory_order_release);
            SignalReady(true);
            ready_signalled = true;

            // Signal creation before making the native window visible. If Start
            // timed out, it still owns the readiness mutex while setting the
            // stop flag; SignalReady then unblocks behind it and this check
            // prevents a late splash from flashing over the visible main window.
            if (m_stop_requested.load(std::memory_order_acquire)) {
                DestroyWindow(window);
                m_window.store(nullptr, std::memory_order_release);
                m_running.store(false, std::memory_order_release);
                return;
            }

            SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA);
            ShowWindow(window, SW_SHOWNOACTIVATE);
            UpdateWindow(window);

            if (m_finish_requested.load(std::memory_order_acquire)) {
                context.finish_requested    = true;
                context.finish_requested_at = Clock::now();
            }

            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            m_window.store(nullptr, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
        } catch (...) {
            m_window.store(nullptr, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            if (!ready_signalled) {
                SignalReady(false);
            }
        }
    }

    void StopAndJoin() noexcept {
        m_stop_requested.store(true, std::memory_order_release);
        PostToWindow(kStopMessage);

        std::scoped_lock lifecycle_lock(m_lifecycle_mutex);
        if (m_ui_thread.joinable()) {
            m_ui_thread.join();
        }
        m_running.store(false, std::memory_order_release);
    }

    std::shared_ptr<SharedSplashState> m_state;
    std::thread                        m_ui_thread;
    mutable std::mutex                 m_lifecycle_mutex;
    mutable std::mutex                 m_ready_mutex;
    std::condition_variable            m_ready_condition;
    bool                               m_ready        = false;
    bool                               m_window_ready = false;
    std::atomic<HWND>                  m_window{nullptr};
    std::atomic<bool>                  m_running{false};
    std::atomic<bool>                  m_finish_requested{false};
    std::atomic<bool>                  m_stop_requested{false};
    std::atomic<bool>                  m_failed{false};
};

StartupSplash::StartupSplash()
    : m_impl(std::make_unique<Impl>()) {}

StartupSplash::~StartupSplash() = default;

bool StartupSplash::Start(std::string_view title, std::string_view detail) noexcept {
    return m_impl->Start(title, detail);
}

void StartupSplash::Update(std::string_view title, std::string_view detail) noexcept {
    m_impl->Update(title, detail);
}

void StartupSplash::Finish() noexcept {
    m_impl->Finish();
}

void StartupSplash::Fail(std::string_view title, std::string_view detail) noexcept {
    m_impl->Fail(title, detail);
}

bool StartupSplash::IsRunning() const noexcept {
    return m_impl->IsRunning();
}

} // namespace Moer

#else

namespace Moer {

class StartupSplash::Impl final {
public:
    [[nodiscard]] bool Start(std::string_view, std::string_view) noexcept {
        return false;
    }

    void Update(std::string_view, std::string_view) noexcept {}
    void Finish() noexcept {}
    void Fail(std::string_view, std::string_view) noexcept {}

    [[nodiscard]] bool IsRunning() const noexcept {
        return false;
    }
};

StartupSplash::StartupSplash()
    : m_impl(std::make_unique<Impl>()) {}

StartupSplash::~StartupSplash() = default;

bool StartupSplash::Start(std::string_view title, std::string_view detail) noexcept {
    return m_impl->Start(title, detail);
}

void StartupSplash::Update(std::string_view title, std::string_view detail) noexcept {
    m_impl->Update(title, detail);
}

void StartupSplash::Finish() noexcept {
    m_impl->Finish();
}

void StartupSplash::Fail(std::string_view title, std::string_view detail) noexcept {
    m_impl->Fail(title, detail);
}

bool StartupSplash::IsRunning() const noexcept {
    return m_impl->IsRunning();
}

} // namespace Moer

#endif
