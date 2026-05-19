#pragma once

#include "remote/RemoteApi.h"
#include "remote/RemoteConfig.h"

#include <memory>

namespace Moer::remote {

class RemoteEventHub;

// 基于 libhv 的 WebSocket 事件广播服务端实现
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