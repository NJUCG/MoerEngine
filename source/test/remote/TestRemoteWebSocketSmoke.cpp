#include "hv/WebSocketClient.h"
#include "hv/WebSocketServer.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr int  kDefaultPort       = 18081;
constexpr auto kStartupDelay      = std::chrono::milliseconds(100);
constexpr auto kWaitTimeout       = std::chrono::seconds(5);
constexpr char kExpectedMessage[] = "remote-websocket-smoke";

struct TestState {
    bool        opened  = false;
    bool        done    = false;
    bool        success = false;
    std::string detail;
};

int ParsePort(int argc, const char** argv) {
    if (argc <= 1) {
        return kDefaultPort;
    }

    char* end  = nullptr;
    long  port = std::strtol(argv[1], &end, 10);
    if (end == argv[1] || (end != nullptr && *end != '\0')) {
        return -1;
    }

    if (port <= 0 || port > 65535) {
        return -1;
    }

    return static_cast<int>(port);
}

void Finish(
    TestState&               state,
    std::mutex&              mutex,
    std::condition_variable& cv,
    bool                     success,
    std::string              detail
) {
    std::lock_guard<std::mutex> lock(mutex);
    if (state.done) {
        return;
    }

    state.done    = true;
    state.success = success;
    state.detail  = std::move(detail);
    cv.notify_one();
}

} // namespace

int main(int argc, const char** argv) {
    const int port = ParsePort(argc, argv);
    if (port < 0) {
        std::cerr << "Invalid port. Usage: TestRemoteWebSocketSmoke [port]" << std::endl;
        return 1;
    }

    hv::WebSocketService ws_service;
    ws_service.onmessage = [](const WebSocketChannelPtr& channel, const std::string& msg) {
        channel->send(msg);
    };

    hv::WebSocketServer server;
    server.setHost("127.0.0.1");
    server.setPort(port);
    server.registerWebSocketService(&ws_service);

    const int server_start_result = server.start();
    if (server_start_result != 0) {
        std::cerr << "Failed to start WebSocket server on 127.0.0.1:" << port
                  << ", ret=" << server_start_result << std::endl;
        return 2;
    }

    std::this_thread::sleep_for(kStartupDelay);

    std::mutex              mutex;
    std::condition_variable cv;
    TestState               state;

    hv::WebSocketClient client;
    client.onopen = [&]() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            state.opened = true;
        }

        if (client.send(kExpectedMessage) < 0) {
            Finish(state, mutex, cv, false, "failed to send test payload");
        }
    };
    client.onmessage = [&](const std::string& msg) {
        if (msg != kExpectedMessage) {
            Finish(state, mutex, cv, false, "unexpected echoed payload: " + msg);
            client.close();
            return;
        }

        Finish(state, mutex, cv, true, "received expected echo");
        client.close();
    };
    client.onclose = [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!state.done) {
            state.done    = true;
            state.success = false;
            state.detail =
                state.opened ? "connection closed before echo arrived" : "failed to open websocket";
            cv.notify_one();
        }
    };

    const std::string url                = "ws://127.0.0.1:" + std::to_string(port) + "/echo";
    const int         client_open_result = client.open(url.c_str());
    if (client_open_result != 0) {
        std::cerr << "Failed to start WebSocket client for " << url << ", ret=" << client_open_result
                  << std::endl;
        server.stop();
        return 3;
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, kWaitTimeout, [&] {
                return state.done;
            })) {
            state.done    = true;
            state.success = false;
            state.detail  = "timed out waiting for websocket echo";
        }
    }

    client.stop();

    const int server_stop_result = server.stop();
    if (server_stop_result != 0) {
        std::cerr << "Failed to stop WebSocket server, ret=" << server_stop_result << std::endl;
        return 4;
    }

    if (!state.success) {
        std::cerr << "TestRemoteWebSocketSmoke failed: " << state.detail << std::endl;
        return 5;
    }

    std::cout << "TestRemoteWebSocketSmoke passed on " << url << std::endl;
    return 0;
}