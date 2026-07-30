#ifndef MOERENGINE_WINDOW_CONTEXT_H
#define MOERENGINE_WINDOW_CONTEXT_H

#include "RenderAPI.h"

#include "rhi/RHIWindowSurface.h"
#include "window/WindowFrameSnapshot.h"

namespace Moer {
using WindowType = void;

enum class EFontType {
    Greek,
    Chinese,
    Korean,
    Japanese,
    Cyrillic,
    Thai,
    Vietnamese,
    Icon,
    Default
};

struct RENDER_API FontDesc {
    FontDesc(const char* _font_path, float _font_size, EFontType _font_type) :
        font_path(_font_path),
        font_size(_font_size),
        font_type(_font_type) {}
    std::string font_path;
    float       font_size = 13.f;
    EFontType   font_type;
};

struct RENDER_API WindowHandle {
    WindowType* window{nullptr};
};

struct RENDER_API SurfaceInitInfo {
    SurfaceInitInfo(
        uint32_t           _width,
        uint32_t           _height,
        const std::string& _title,
        bool               _full_screen,
        bool               _visible = true
    ) :
        width(_width),
        height(_height),
        title(_title),
        b_fullscreen(_full_screen),
        b_visible(_visible) {}

    SurfaceInitInfo() : SurfaceInitInfo(1920, 1080, "untitled", false) {}

    int         width{1280};
    int         height{720};
    std::string title{"MoerEngine"};
    bool        b_fullscreen{false};
    bool        b_vsync{false};
    bool        b_visible{true};
};
//mean to support multi window creation and io-management
class RENDER_API WindowContext {
    friend class WindowImpl;

public:
    WindowContext() = default;
    ~WindowContext();
    static void Init(const SurfaceInitInfo& info);
    static void Tick();
    static void ShutDown();
    //new support for multi window
    // Captures logical and drawable extents together on the Game Thread.
    // Render/RHI code consumes the returned value and never queries GLFW.
    static Render::WindowFrameMetrics CaptureWindowFrameMetrics(const WindowHandle&);
    // Applies cursor capture on the Game Thread. Repeated requests for the
    // current mode are ignored by the platform implementation.
    static void          ApplyCursorMode(WindowHandle*, bool hidden);
    static void          SetFocusMode(WindowHandle*, bool focused);
    static bool          GetFocusMode(WindowHandle*);
    static WindowHandle* GetMainWindow();
    static void          ShowMainWindow();
    static void          SetTitle(WindowHandle*, const char* newTitle);
    static void          RequestClose(WindowHandle*);
    static bool          ShouldClose(WindowHandle*);
    // Captures an immutable native-window identity and surface factory. This
    // is a Game Thread operation; the returned value can be retained by RT/RHI.
    static Render::SwapchainSurfaceInfo CreateSwapchainSurfaceInfo(const WindowHandle&);

protected:
    friend class WindowImpl;
};
} // namespace Moer
#endif // !UICONTEXT_H
