#include "network/Socket.h"

#include "log/LogSystem.h"

#include <algorithm>
#include <limits>
#include <string>

#if defined(_WIN32) || defined(_WIN64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Moer::Network {
namespace {

#if defined(_WIN32) || defined(_WIN64)
using NativeSocket = SOCKET;
constexpr NativeSocket k_invalid_socket = INVALID_SOCKET;

bool EnsureSocketRuntime() {
    static bool initialized = []() {
        WSADATA data{};
        const int ret = WSAStartup(MAKEWORD(2, 2), &data);
        if (ret != 0) {
            LOG_WARNING(MOER_TEXT("Socket runtime failed to initialize WinSock: {}"), ret);
            return false;
        }
        return true;
    }();
    return initialized;
}

void CloseNativeSocket(NativeSocket socket) {
    closesocket(socket);
}
#else
using NativeSocket = int;
constexpr NativeSocket k_invalid_socket = -1;

bool EnsureSocketRuntime() {
    return true;
}

void CloseNativeSocket(NativeSocket socket) {
    close(socket);
}
#endif

NativeSocket ToNative(uintptr_t handle) {
    return static_cast<NativeSocket>(handle);
}

uintptr_t FromNative(NativeSocket socket) {
    return static_cast<uintptr_t>(socket);
}

std::string ToStdString(Utf8StringView text) {
    return std::string(text.data(), text.size());
}

bool SetReuseAddress(NativeSocket socket) {
    int enabled = 1;
    return setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0;
}

bool SetTcpNoDelay(NativeSocket socket) {
    int enabled = 1;
    return setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0;
}

} // namespace

TcpSocket::TcpSocket() : native_handle(FromNative(k_invalid_socket)) {}

TcpSocket::TcpSocket(uintptr_t native_handle) : native_handle(native_handle) {}

TcpSocket::~TcpSocket() {
    Close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : native_handle(other.native_handle) {
    other.native_handle = FromNative(k_invalid_socket);
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Close();
    native_handle = other.native_handle;
    other.native_handle = FromNative(k_invalid_socket);
    return *this;
}

bool TcpSocket::IsOpen() const {
    return ToNative(native_handle) != k_invalid_socket;
}

void TcpSocket::Close() {
    NativeSocket socket = ToNative(native_handle);
    if (socket == k_invalid_socket) {
        return;
    }
    CloseNativeSocket(socket);
    native_handle = FromNative(k_invalid_socket);
}

ESocketStatus TcpSocket::Connect(Utf8StringView host, uint16_t port) {
    Close();
    if (!EnsureSocketRuntime()) {
        return ESocketStatus::Error;
    }

    const std::string host_text = ToStdString(host);
    const std::string port_text = std::to_string(port);

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    if (getaddrinfo(host_text.c_str(), port_text.c_str(), &hints, &addresses) != 0 || addresses == nullptr) {
        return ESocketStatus::Error;
    }

    NativeSocket connected_socket = k_invalid_socket;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        NativeSocket candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == k_invalid_socket) {
            continue;
        }
        if (connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
            connected_socket = candidate;
            break;
        }
        CloseNativeSocket(candidate);
    }
    freeaddrinfo(addresses);

    if (connected_socket == k_invalid_socket) {
        return ESocketStatus::Error;
    }
    SetTcpNoDelay(connected_socket);
    native_handle = FromNative(connected_socket);
    return ESocketStatus::Success;
}

ESocketStatus TcpSocket::BindListen(const TcpListenDesc& desc) {
    Close();
    if (!EnsureSocketRuntime()) {
        return ESocketStatus::Error;
    }

    const std::string host_text = ToStdString(desc.host);
    const std::string port_text = std::to_string(desc.port);

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags    = AI_PASSIVE;

    addrinfo* addresses = nullptr;
    const char* host_ptr = host_text.empty() ? nullptr : host_text.c_str();
    if (getaddrinfo(host_ptr, port_text.c_str(), &hints, &addresses) != 0 || addresses == nullptr) {
        return ESocketStatus::Error;
    }

    NativeSocket listen_socket = k_invalid_socket;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        NativeSocket candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == k_invalid_socket) {
            continue;
        }
        if (desc.reuse_address) {
            SetReuseAddress(candidate);
        }
        if (bind(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0 &&
            listen(candidate, desc.backlog) == 0) {
            listen_socket = candidate;
            break;
        }
        CloseNativeSocket(candidate);
    }
    freeaddrinfo(addresses);

    if (listen_socket == k_invalid_socket) {
        return ESocketStatus::Error;
    }
    native_handle = FromNative(listen_socket);
    return ESocketStatus::Success;
}

ESocketStatus TcpSocket::Accept(TcpSocket& out_client) {
    out_client.Close();
    NativeSocket socket = ToNative(native_handle);
    if (socket == k_invalid_socket) {
        return ESocketStatus::Closed;
    }

    NativeSocket client = accept(socket, nullptr, nullptr);
    if (client == k_invalid_socket) {
        return IsOpen() ? ESocketStatus::Error : ESocketStatus::Closed;
    }
    SetTcpNoDelay(client);
    out_client = TcpSocket(FromNative(client));
    return ESocketStatus::Success;
}

ESocketStatus TcpSocket::SendAll(std::span<const std::byte> bytes) {
    NativeSocket socket = ToNative(native_handle);
    if (socket == k_invalid_socket) {
        return ESocketStatus::Closed;
    }

    size_t sent_total = 0;
    while (sent_total < bytes.size()) {
        const size_t transfer_size = std::min(bytes.size() - sent_total, static_cast<size_t>(std::numeric_limits<int>::max()));
        const int sent = send(
            socket,
            reinterpret_cast<const char*>(bytes.data() + sent_total),
            static_cast<int>(transfer_size),
            0
        );
        if (sent <= 0) {
            Close();
            return ESocketStatus::Closed;
        }
        sent_total += static_cast<size_t>(sent);
    }
    return ESocketStatus::Success;
}

ESocketStatus TcpSocket::RecvAll(std::span<std::byte> bytes) {
    NativeSocket socket = ToNative(native_handle);
    if (socket == k_invalid_socket) {
        return ESocketStatus::Closed;
    }

    size_t recv_total = 0;
    while (recv_total < bytes.size()) {
        const size_t transfer_size = std::min(bytes.size() - recv_total, static_cast<size_t>(std::numeric_limits<int>::max()));
        const int received = recv(
            socket,
            reinterpret_cast<char*>(bytes.data() + recv_total),
            static_cast<int>(transfer_size),
            0
        );
        if (received <= 0) {
            Close();
            return ESocketStatus::Closed;
        }
        recv_total += static_cast<size_t>(received);
    }
    return ESocketStatus::Success;
}

} // namespace Moer::Network