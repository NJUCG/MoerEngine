#pragma once

#include "remote/RemoteApi.h"
#include "remote/RemoteConfig.h"

#include <memory>

namespace hv {
struct HttpService;
class HttpServer;
} // namespace hv

namespace Moer::remote {

class RemoteRequestRegistry;
class RemoteScriptService;

// 基于 libhv 的 HTTP 传输层服务端实现
class REMOTE_API RemoteHttpServer {
public:
    RemoteHttpServer(
        const RemoteConfig&    config,
        RemoteScriptService&   script_service,
        RemoteRequestRegistry& registry
    );
    ~RemoteHttpServer();

    bool Start();
    void Stop();
    bool IsRunning() const;

private:
    // 注册 Remote 模块当前暴露的所有 HTTP 路由
    void RegisterRoutes();

    RemoteConfig                     m_config;
    RemoteScriptService&             m_script_service;
    RemoteRequestRegistry&           m_registry;
    std::unique_ptr<hv::HttpService> m_router;
    std::unique_ptr<hv::HttpServer>  m_server;
    bool                             m_running = false;
};

} // namespace Moer::remote