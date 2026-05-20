#pragma once

#include "misc/STL.h"
#include "remote/RemoteApi.h"
#include "remote/RemoteConfig.h"
#include "remote/RemoteEventHub.h"
#include "remote/RemoteModuleController.h"
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

    // 提供给 Editor 等外部系统的弱控制句柄，避免直接暴露 RemoteModule 生命周期
    RemoteModuleController GetController() const;

private:
    RemoteConfig                             m_config;
    RemoteSubmitScriptExecutionFn            m_submit_fn;
    SharedPtr<IRemoteModuleControllerBridge> m_controller_bridge;
    RemoteRequestRegistry                    m_registry;
    RemoteEventHub                           m_event_hub;
    UniquePtr<RemoteScriptService>           m_script_service;
    UniquePtr<RemoteHttpServer>              m_http_server;
    UniquePtr<RemoteWebSocketServer>         m_ws_server;
    bool                                     m_running = false;
};

} // namespace Moer::remote