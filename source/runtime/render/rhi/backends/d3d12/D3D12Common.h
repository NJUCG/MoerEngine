#pragma once

#include "log/LogSystem.h"
#include "misc/Types.h"
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dx12/d3dx12.h>
#include <dxgi1_6.h>
#include <stdexcept>
#include <string>
#include <wrl/client.h>

namespace Moer::Render::D3D12 {
template<typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

inline void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::format("D3D12 {} failed: {:#x}", operation, uint32(result)));
    }
}

inline std::wstring Widen(std::string_view name) {
    const int    count = MultiByteToWideChar(CP_UTF8, 0, name.data(), int(name.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.data(), int(name.size()), result.data(), count);
    return result;
}

constexpr uint64 Align(uint64 size, uint64 alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}
} // namespace Moer::Render::D3D12

#ifndef DX_CHECK_HRESULT
#define DX_CHECK_HRESULT(expression) ::Moer::Render::D3D12::Check((expression), #expression)
#endif
