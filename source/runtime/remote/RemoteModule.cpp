#include "remote/RemoteModule.h"

#include "log/LogSystem.h"
#include "remote/service/RemoteScriptService.h"
#include "remote/transport/http/RemoteHttpServer.h"
#include "remote/transport/ws/RemoteWebSocketServer.h"

namespace Moer::remote {

namespace {

class RemoteModuleControllerBridge final : public IRemoteModuleControllerBridge {
public:
    explicit RemoteModuleControllerBridge(RemoteModule& module) : m_module(module) {}

    bool IsEnabled() const override {
        return m_module.IsEnabled();
    }

    bool IsRunning() const override {
        return m_module.IsRunning();
    }

    bool SetEnabled(bool enabled) override {
        return m_module.SetEnabled(enabled);
    }

    RemoteConfig GetConfigSnapshot() const override {
        return m_module.GetConfig();
    }

private:
    RemoteModule& m_module;
};

} // namespace

RemoteModule::RemoteModule(RemoteConfig config, RemoteSubmitScriptExecutionFn submit_fn) :
    m_config(std::move(config)),
    m_submit_fn(std::move(submit_fn)) {
    m_controller_bridge = MakeShared<RemoteModuleControllerBridge>(*this);
    m_script_service    = MakeUnique<RemoteScriptService>(m_submit_fn, m_registry, m_event_hub);
}

RemoteModule::~RemoteModule() {
    Stop();
    m_controller_bridge.reset();
}

bool RemoteModule::SetEnabled(bool enabled) {
    if (!enabled) {
        if (!m_config.enable && !m_running) {
            LOG_INFO("RemoteModule disable request ignored: already disabled.");
            return true;
        }

        Stop();
        m_config.enable = false;
        LOG_INFO("RemoteModule disabled.");
        return true;
    }

    if (m_running) {
        m_config.enable = true;
        LOG_INFO("RemoteModule enable request ignored: already enabled.");
        return true;
    }

    m_config.enable = true;
    if (!Start()) {
        m_config.enable = false;
        LOG_ERROR("RemoteModule enable failed.");
        return false;
    }

    LOG_INFO("RemoteModule enabled.");
    return true;
}

bool RemoteModule::Start() {
    if (m_running) {
        return true;
    }

    if (!m_config.enable || !m_submit_fn) {
        return false;
    }

    m_http_server = MakeUnique<RemoteHttpServer>(m_config, *m_script_service, m_registry);
    if (!m_http_server->Start()) {
        m_http_server.reset();
        m_running = false;
        return false;
    }

    m_ws_server = MakeUnique<RemoteWebSocketServer>(m_config, m_event_hub);
    if (!m_ws_server->Start()) {
        m_ws_server.reset();
        m_http_server->Stop();
        m_http_server.reset();
        m_running = false;
        return false;
    }

    m_running = true;
    return true;
}

void RemoteModule::Stop() {
    if (!m_running && !m_http_server && !m_ws_server) {
        return;
    }

    if (m_ws_server) {
        m_ws_server->Stop();
        m_ws_server.reset();
    }

    if (m_http_server) {
        m_http_server->Stop();
        m_http_server.reset();
    }

    m_running = false;
}

bool RemoteModule::IsEnabled() const {
    return m_config.enable;
}

bool RemoteModule::IsRunning() const {
    return m_running;
}

const RemoteConfig& RemoteModule::GetConfig() const {
    return m_config;
}

RemoteModuleController RemoteModule::GetController() const {
    return RemoteModuleController(m_controller_bridge);
}

} // namespace Moer::remote