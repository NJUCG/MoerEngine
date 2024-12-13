

#ifndef D3D12_MACRO_H
#define D3D12_MACRO_H

#include "log/LogSystem.h"

#define DX_CHECK_HRESULT(hr)                                                   \
    do {                                                                       \
        if ((hr) < 0) {                                                        \
            LOG_CRITICAL("ERROR: hresult={}, at [{}:{}]", __FILE__, __LINE__); \
            std::terminate();                                                  \
        }                                                                      \
    } while (0)

#endif// D3D12_MACRO_H
