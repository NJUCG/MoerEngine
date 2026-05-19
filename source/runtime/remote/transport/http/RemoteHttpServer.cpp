#include "remote/transport/http/RemoteHttpServer.h"

#include "remote/RemoteRequestRegistry.h"
#include "remote/service/RemoteScriptService.h"
#include "remote/transport/http/RemoteHttpJson.h"

#include "hv/HttpServer.h"

#include <utility>

namespace Moer::remote {

namespace {

int WriteJson(HttpResponse* response, int status_code, const std::string& body) {
    response->SetHeader("Content-Type", "application/json");
    response->body = body;
    return status_code;
}

} // namespace

RemoteHttpServer::RemoteHttpServer(
    const RemoteConfig&    config,
    RemoteScriptService&   script_service,
    RemoteRequestRegistry& registry
) :
    m_config(config),
    m_script_service(script_service),
    m_registry(registry) {}

RemoteHttpServer::~RemoteHttpServer() {
    Stop();
}

bool RemoteHttpServer::Start() {
    if (m_running) {
        return true;
    }

    m_router = std::make_unique<hv::HttpService>();
    RegisterRoutes();

    m_server = std::make_unique<hv::HttpServer>();
    m_server->registerHttpService(m_router.get());
    m_server->setHost(m_config.bind_address.c_str());
    m_server->setPort(m_config.http_port);

    const int start_result = m_server->start();
    m_running              = start_result == 0;
    return m_running;
}

void RemoteHttpServer::Stop() {
    if (m_server) {
        m_server->stop();
        m_server.reset();
    }

    m_router.reset();
    m_running = false;
}

bool RemoteHttpServer::IsRunning() const {
    return m_running;
}

void RemoteHttpServer::RegisterRoutes() {
    m_router->GET("/healthz", [](HttpRequest* request, HttpResponse* response) {
        (void)request;
        return WriteJson(response, HTTP_STATUS_OK, MakeHealthzJson());
    });

    m_router->GET("/api/remote/status", [this](HttpRequest* request, HttpResponse* response) {
        (void)request;
        return WriteJson(response, HTTP_STATUS_OK, MakeRemoteStatusJson(m_config, IsRunning()));
    });

    m_router->POST("/api/script/execute", [this](HttpRequest* request, HttpResponse* response) {
        RemoteExecuteScriptRequest execute_request;
        std::string                error_message;
        if (!ParseExecuteScriptRequest(request->body, execute_request, error_message)) {
            return WriteJson(response, HTTP_STATUS_BAD_REQUEST, MakeErrorJson(error_message));
        }

        const auto execute_response = m_script_service.ExecuteAndWait(std::move(execute_request));
        return WriteJson(response, HTTP_STATUS_OK, MakeExecuteScriptResponseJson(execute_response));
    });

    m_router->GET("/api/script/requests/:request_id", [this](HttpRequest* request, HttpResponse* response) {
        const std::string request_id = request->GetParam("request_id");
        auto              record     = m_registry.Find(request_id);
        if (!record.has_value()) {
            return WriteJson(response, HTTP_STATUS_NOT_FOUND, MakeErrorJson("Remote request not found."));
        }

        return WriteJson(response, HTTP_STATUS_OK, MakeRequestRecordJson(*record));
    });
}

} // namespace Moer::remote