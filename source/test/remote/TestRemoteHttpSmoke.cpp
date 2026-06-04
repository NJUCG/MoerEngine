#include "hv/HttpServer.h"
#include "hv/requests.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr int  kDefaultPort    = 18080;
constexpr int  kMaxAttempts    = 20;
constexpr auto kRetryDelay     = std::chrono::milliseconds(50);
constexpr char kExpectedBody[] = R"({"ok":true,"service":"remote-http-smoke"})";

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

requests::Response GetWithRetry(const std::string& url) {
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        auto response = requests::get(url.c_str());
        if (response != nullptr) {
            return response;
        }

        std::this_thread::sleep_for(kRetryDelay);
    }

    return nullptr;
}

} // namespace

int main(int argc, const char** argv) {
    const int port = ParsePort(argc, argv);
    if (port < 0) {
        std::cerr << "Invalid port. Usage: TestRemoteHttpSmoke [port]" << std::endl;
        return 1;
    }

    hv::HttpService router;
    router.GET("/healthz", [](HttpRequest* req, HttpResponse* resp) {
        (void)req;
        resp->SetHeader("Content-Type", "application/json");
        return resp->String(kExpectedBody);
    });

    hv::HttpServer server;
    server.registerHttpService(&router);
    server.setHost("127.0.0.1");
    server.setPort(port);

    const int start_result = server.start();
    if (start_result != 0) {
        std::cerr << "Failed to start HTTP server on 127.0.0.1:" << port << ", ret=" << start_result
                  << std::endl;
        return 2;
    }

    // 如果想测试服务器，可以在这里通过 cin 来阻塞
    // std::string str;
    // std::cin >> str;

    const std::string url      = "http://127.0.0.1:" + std::to_string(port) + "/healthz";
    auto              response = GetWithRetry(url);
    if (response == nullptr) {
        std::cerr << "Failed to GET " << url << std::endl;
        server.stop();
        return 3;
    }

    if (response->status_code != 200) {
        std::cerr << "Unexpected status code: " << response->status_code << std::endl;
        server.stop();
        return 4;
    }

    if (response->body != kExpectedBody) {
        std::cerr << "Unexpected response body: " << response->body << std::endl;
        server.stop();
        return 5;
    }

    const int stop_result = server.stop();
    if (stop_result != 0) {
        std::cerr << "Failed to stop HTTP server, ret=" << stop_result << std::endl;
        return 6;
    }

    std::cout << "TestRemoteHttpSmoke passed on " << url << std::endl;
    return 0;
}