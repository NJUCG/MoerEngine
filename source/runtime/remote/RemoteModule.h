#pragma once

#include "misc/STL.h"
#include "remote/RemoteApi.h"
#include "remote/RemoteConfig.h"
#include "remote/RemoteEventHub.h"
#include "remote/RemoteRequestRegistry.h"
#include "remote/RemoteSubmitFn.h"

namespace Moer::remote {

class RemoteScriptService;
class RemoteHttpServer;
class RemoteWebSocketServer;

// 统一持有并编排 Remote 子系统各个组件的顶层模块
class REMOTE_API RemoteModule {
public:
    RemoteModule(RemoteConfig config, RemoteSubmitScriptExecutionFn submit_fn);
    ~RemoteModule();

    bool SetEnabled(bool enabled);
    bool Start();
    void Stop();

    bool                IsEnabled() const;
    bool                IsRunning() const;
    const RemoteConfig& GetConfig() const;

private:
    RemoteConfig                     m_config;
    RemoteSubmitScriptExecutionFn    m_submit_fn;
    RemoteRequestRegistry            m_registry;
    RemoteEventHub                   m_event_hub;
    UniquePtr<RemoteScriptService>   m_script_service;
    UniquePtr<RemoteHttpServer>      m_http_server;
    UniquePtr<RemoteWebSocketServer> m_ws_server;
    bool                             m_running = false;
};

} // namespace Moer::remote