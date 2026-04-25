#include "RenderDocApi.h"

#if WITH_RENDERDOC

// 减少 Windows.h 的污染
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "log/LogSystem.h"

namespace Moer::Render {

static std::atomic<bool> is_requesting_capture{false};

RenderDocApi& RenderDocApi::Get() {
    static RenderDocApi instance;
    return instance;
}

RenderDocApi::RenderDocApi() {
    // 检查 RenderDoc 是否已经注入了进程
    // 只有通过 RenderDoc 软件 Launch 时，DLL 才会被注入
    HMODULE mod = GetModuleHandleA("renderdoc.dll");

    if (mod) {
        pRENDERDOC_GetAPI get_api = (pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");
        if (get_api) {
            // 获取 1.1.2 版本的指针
            get_api(eRENDERDOC_API_Version_1_1_2, (void**)&m_api);
        }

        LOG_INFO(MOER_TEXT("RenderDoc API is enabled."));
    } else {
        LOG_WARNING(MOER_TEXT("RenderDoc isn't running. RenderDoc API is disabled."));
    }
}

void RenderDocApi::StartCapture() {
    if (m_api && !is_requesting_capture) {
        is_requesting_capture = true;
        m_api->StartFrameCapture(nullptr, nullptr);
        LOG_INFO(MOER_TEXT("RenderDoc frame capture started."));
    }
}

void RenderDocApi::EndCapture() {
    if (m_api && is_requesting_capture) {
        m_api->EndFrameCapture(nullptr, nullptr);
        is_requesting_capture = false; // 捕获完成，重置状态
        LOG_INFO(MOER_TEXT("RenderDoc frame capture ended."));
    }
}

bool RenderDocApi::IsActive() const {
    return m_api != nullptr;
}

} // namespace Moer::Render

#endif // WITH_RENDERDOC