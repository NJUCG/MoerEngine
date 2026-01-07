#pragma once

#if WITH_RENDERDOC

#include "renderdoc_app.h"

namespace Moer::Render {

/**
 * RenderDoc API 封装
 * 采用单例模式，隐藏所有 Windows 和 RenderDoc 细节
 */
class RenderDocApi {
public:
    static RenderDocApi& Get();

    // 开始截帧
    void StartCapture();

    // 结束截帧
    void EndCapture();

    // 判断 API 是否激活（是否是从 RenderDoc 启动的）
    bool IsActive() const;

private:
    RenderDocApi();
    ~RenderDocApi() = default;

    // 禁止拷贝
    RenderDocApi(const RenderDocApi&)            = delete;
    RenderDocApi& operator=(const RenderDocApi&) = delete;

    RENDERDOC_API_1_1_2* m_api = nullptr;
};

} // namespace Moer::Render

#endif // WITH_RENDERDOC