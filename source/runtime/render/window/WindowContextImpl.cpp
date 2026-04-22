#include "WindowContextImpl.h"
#include "window/WindowContext.h"
#define WINDOW_USE_GLFW
#if defined(WINDOW_USE_GLFW)
#include "glfw/GLFWWindowImpl.h"
#endif

namespace Moer {
WindowImpl& WindowImpl::GetInstance() {
#if defined(WINDOW_USE_GLFW)
    static GLFWWindowImpl impl;
#endif
    return impl;
};
} // namespace Moer
