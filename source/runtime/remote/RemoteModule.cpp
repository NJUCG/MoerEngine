#include "remote/RemoteModule.h"

#include "remote/service/RemoteScriptService.h"
#include "remote/transport/http/RemoteHttpServer.h"
#include "remote/transport/ws/RemoteWebSocketServer.h"

namespace Moer::remote {

RemoteModule::RemoteModule(RemoteConfig config, RemoteSubmitScriptExecutionFn submit_fn) :
    m_config(std::move(config)),
    m_submit_fn(std::move(submit_fn)) {
    m_script_service = MakeUnique<RemoteScriptService>(m_submit_fn, m_registry, m_event_hub);
}

RemoteModule::~RemoteModule() {
    Stop();
}

bool RemoteModule::SetEnabled(bool enabled) {
    if (!enabled) {
        Stop();
        m_config.enable = false;
        return true;
    }

    if (m_running) {
        m_config.enable = true;
        return true;
    }

    m_config.enable = true;
    if (!Start()) {
        m_config.enable = false;
        return false;
    }

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

} // namespace Moer::remote