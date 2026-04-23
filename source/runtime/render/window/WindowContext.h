#ifndef MOERENGINE_WINDOW_CONTEXT_H
#define MOERENGINE_WINDOW_CONTEXT_H

#include "RenderAPI.h"

#include "rhi/RHICommon.h"

#include <string>

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

struct RENDER_API GuiWindowInitInfo {
    WindowType* window;
    bool        b_install_callbacks = true;
    ERHIType    rhi_type;
};

struct RENDER_API SurfaceInitInfo {
    SurfaceInitInfo(
        const ERHIType&    _rhi_type,
        uint32_t           _width,
        uint32_t           _height,
        const std::string& _title,
        bool               _full_screen
    ) :
        rhi_type(_rhi_type),
        width(_width),
        height(_height),
        title(_title),
        b_fullscreen(_full_screen) {}

    SurfaceInitInfo() : SurfaceInitInfo(ERHIType::Vulkan, 1920, 1080, "untitled", false) {}

    ERHIType    rhi_type;
    int         width{1280};
    int         height{720};
    std::string title{"MoerEngine"};
    bool        b_fullscreen{false};
    bool        b_vsync{false};
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
    static void          GetWindowSize(WindowHandle*, int* width, int* height);
    static void          SetFocusMode(WindowHandle*, bool focused);
    static bool          GetFocusMode(WindowHandle*);
    static WindowHandle* GetMainWindow();
    static void          SetTitle(WindowHandle*, const char* newTitle);
    static bool          ShouldClose(WindowHandle*);

protected:
    friend class WindowImpl;
};
} // namespace Moer
#endif // !UICONTEXT_H
