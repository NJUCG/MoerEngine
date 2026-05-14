#ifndef MOERENGINE_WINDOW_CONTEXT_H
#define MOERENGINE_WINDOW_CONTEXT_H

#include "RenderAPI.h"

#include "rhi/RHI.h"
#include <functional>

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
    static void          RequestClose(WindowHandle*);
    static bool          ShouldClose(WindowHandle*);
    //for dx12 win32 window
    static void* GetNativeWindow(WindowHandle*);
    //for vulkan surface creation
    static void
    CreateVulkanSurface(void* instance, WindowHandle* window, void* allocation_callback, void* surface);

    typedef std::function<void(unsigned int)>                                OnCharFunc;
    typedef std::function<void(int entered)>                                 OnCursorEnterFunc;
    typedef std::function<void(double xpos, double ypos)>                    OnCursorPosFunc;
    typedef std::function<void(int path_count, const char** paths)>          OnDropFunc;
    typedef std::function<void(int width, int height)>                       OnFrameBufferSizeFunc;
    typedef std::function<void(int key, int scancode, int action, int mods)> OnKeyFunc;
    typedef std::function<void(int button, int action, int mode)>            OnMouseButtonFunc;
    typedef std::function<void(double xoffset, double yoffset)>              OnScrollFunc;
    typedef std::function<void()>                                            OnWindowCloseFunc;
    typedef std::function<void(float xscale, float yscale)>                  OnWindowContentScaleFunc;
    typedef std::function<void(int xpos, int ypos)>                          OnWindowPosFunc;
    typedef std::function<void(int width, int height)>                       OnWindowSizeFunc;
    typedef std::function<void(int focused)>                                 OnWindowFocusFunc;

    static void RegisterOnCharFunc(WindowType* handle, OnCharFunc func);
    static void RegisterOnCursorEnterFunc(WindowType* handle, OnCursorEnterFunc func);
    static void RegisterOnCursorPosFunc(WindowType* handle, OnCursorPosFunc func);
    static void RegisterOnDropFunc(WindowType* handle, OnDropFunc func);
    static void RegisterOnFrameBufferSizeFunc(WindowType* handle, OnFrameBufferSizeFunc func);
    static void RegisterOnKeyFunc(WindowType* handle, OnKeyFunc func);
    static void RegisterOnMouseButtonFunc(WindowType* handle, OnMouseButtonFunc func);
    static void RegisterOnScrollFunc(WindowType* handle, OnScrollFunc func);
    static void RegisterOnWindowCloseFunc(WindowType* handle, OnWindowCloseFunc func);
    static void RegisterOnWindowContentScaleFunc(WindowType* handle, OnWindowContentScaleFunc func);
    static void RegisterOnWindowPosFunc(WindowType* handle, OnWindowPosFunc func);
    static void RegisterOnWindowSizeFunc(WindowType* handle, OnWindowSizeFunc func);
    static void RegisterOnWindowFocusFunc(WindowType* handle, OnWindowFocusFunc func);

protected:
    friend class WindowImpl;
};
} // namespace Moer
#endif // !UICONTEXT_H