#pragma once

#include "remote/RemoteApi.h"
#include "remote/RemoteConfig.h"

#include <memory>

namespace Moer::remote {

class RemoteEventHub;

// 基于 libhv 的 WebSocket 事件广播服务端实现
//
// TODO: WebSocket 实时通道
// - 说明：用于推送执行中状态变化、增量输出和完成/失败事件
// - 路径：在现有 /ws/events 基础上补齐事件类型、订阅语义和输出推送
class REMOTE_API RemoteWebSocketServer {
public:
    RemoteWebSocketServer(const RemoteConfig& config, RemoteEventHub& event_hub);
    ~RemoteWebSocketServer();

    bool Start();
    void Stop();
    bool IsRunning() const;

private:
    // 隐藏 libhv 相关实现细节并减少头文件依赖
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Moer::remote