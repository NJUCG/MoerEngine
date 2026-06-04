#pragma once

#include "remote/RemoteModuleController.h"

namespace Moer {

// RemoteExamplesUI 负责展示 Remote 接口示例，并提供 URL、命令和 JSON 的复制入口
class RemoteExamplesUI {
public:
    // 打开示例窗口
    void Open() {
        m_b_show = true;
    }

    // 关闭示例窗口
    void Close() {
        m_b_show = false;
    }

    // 在 Remote 启用时绘制示例窗口，禁用后自动关闭
    void ShowWindow(const remote::RemoteModuleController& remote_controller);

private:
    bool m_b_show = false;
};

} // namespace Moer