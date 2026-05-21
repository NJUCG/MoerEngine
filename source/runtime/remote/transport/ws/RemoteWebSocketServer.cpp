#include "remote/transport/ws/RemoteWebSocketServer.h"

#include "remote/RemoteEventHub.h"

#include "hv/WebSocketServer.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace Moer::remote {

namespace {

std::string MakeEventJson(const RemoteEvent& event) {
    nlohmann::json json{
        {"type", event.type},
        {"request_id", event.request_id},
        {"state", ToString(event.state)},
        {"message", event.message},
        {"stdout_chunk", event.stdout_chunk},
        {"stderr_chunk", event.stderr_chunk},
    };
    return json.dump();
}

} // namespace

struct RemoteWebSocketServer::Impl {
    Impl(RemoteConfig config, RemoteEventHub& event_hub) : config(std::move(config)), event_hub(event_hub) {}

    bool Start() {
        if (running) {
            return true;
        }

        service         = std::make_unique<hv::WebSocketService>();
        service->onopen = [this](const WebSocketChannelPtr& channel, const HttpRequestPtr& request) {
            if (request == nullptr || request->Path() != "/ws/events") {
                channel->close();
                return;
            }

            std::lock_guard lock(channels_mutex);
            channels.push_back(channel);
        };
        service->onmessage = [](const WebSocketChannelPtr& channel, const std::string& msg) {
            (void)channel;
            (void)msg;
        };
        service->onclose = [this](const WebSocketChannelPtr& channel) {
            std::lock_guard lock(channels_mutex);
            channels.erase(std::remove(channels.begin(), channels.end(), channel), channels.end());
        };

        subscription_id = event_hub.Subscribe([this](const RemoteEvent& event) {
            Broadcast(MakeEventJson(event));
        });

        server = std::make_unique<hv::WebSocketServer>();
        server->registerWebSocketService(service.get());
        server->setHost(config.bind_address.c_str());
        server->setPort(config.websocket_port);

        const int start_result = server->start();
        running                = start_result == 0;
        if (!running) {
            Stop();
        }

        return running;
    }

    void Stop() {
        if (subscription_id != 0) {
            event_hub.Unsubscribe(subscription_id);
            subscription_id = 0;
        }

        if (server) {
            server->stop();
            server.reset();
        }

        service.reset();
        {
            std::lock_guard lock(channels_mutex);
            channels.clear();
        }
        running = false;
    }

    void Broadcast(const std::string& message) {
        std::vector<WebSocketChannelPtr> targets;
        {
            std::lock_guard lock(channels_mutex);
            targets = channels;
        }

        for (const auto& channel : targets) {
            if (channel) {
                channel->send(message);
            }
        }
    }

    RemoteConfig                          config;
    RemoteEventHub&                       event_hub;
    std::unique_ptr<hv::WebSocketService> service;
    std::unique_ptr<hv::WebSocketServer>  server;
    std::mutex                            channels_mutex;
    std::vector<WebSocketChannelPtr>      channels;
    uint64_t                              subscription_id = 0;
    bool                                  running         = false;
};

RemoteWebSocketServer::RemoteWebSocketServer(const RemoteConfig& config, RemoteEventHub& event_hub) :
    m_impl(std::make_unique<Impl>(config, event_hub)) {}

RemoteWebSocketServer::~RemoteWebSocketServer() {
    Stop();
}

bool RemoteWebSocketServer::Start() {
    return m_impl->Start();
}

void RemoteWebSocketServer::Stop() {
    if (m_impl) {
        m_impl->Stop();
    }
}

bool RemoteWebSocketServer::IsRunning() const {
    return m_impl && m_impl->running;
}

} // namespace Moer::remote