

#ifndef D3D12_MACRO_H
#define D3D12_MACRO_H

#include "log/LogSystem.h"

namespace Moer::Render {

    // maybe throw...
    // seems likely to incur some macro redefinitions

#define DX_CHECK_HRESULT(hr)                                         \
    do {                                                             \
        HRESULT _hr = (hr);                                          \
        if (_hr < 0) {                                               \
            LOG_CRITICAL("ERROR: hresult={:#x}", (unsigned int)_hr); \
            std::terminate();                                        \
        }                                                            \
    } while (0)

#define ASSERT(x)                                      \
    do {                                               \
        bool ok = (x);                                 \
        if (!ok) {                                     \
            LOG_CRITICAL("assert failed EXPR {}", #x); \
            std::terminate();                          \
        }                                              \
    } while (0)

#define ASSERT2(x, msg)                                               \
    do {                                                              \
        bool ok = (x);                                                \
        if (!ok) {                                                    \
            LOG_CRITICAL("assert failed EXPR {} NOTE {}", #x, (msg)); \
            std::terminate();                                         \
        }                                                             \
    } while (0)

#if defined(_DEBUG) || defined(DEBUG)
#define DX_DEBUG 1
#else
#define DX_DEBUG 0
#endif

#if DX_DEBUG == 1
#define DASSERT(x)       ASSERT(x)
#define DASSERT2(x, msg) ASSERT2(x, msg)
#else
#define DASSERT(x)
#define DASSERT2(x, msg)
#endif

#define FATAL(...)                 \
    do {                           \
        LOG_CRITICAL(__VA_ARGS__); \
        std::terminate();          \
    } while (0)

}// namespace Moer::Render

#endif// D3D12_MACRO_H
