#pragma once

#include "API_Macro.h"
#include "string/String.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace Moer::Network {

enum class ESocketStatus : uint8_t {
    Success = 0,
    Closed,
    Error,
};

struct TcpListenDesc {
    Utf8StringView host{};
    uint16_t       port{0};
    int32_t        backlog{1};
    bool           reuse_address{true};
};

class CORE_API TcpSocket {
public:
    TcpSocket();
    ~TcpSocket();

    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    bool IsOpen() const;
    void Close();

    ESocketStatus Connect(Utf8StringView host, uint16_t port);
    ESocketStatus BindListen(const TcpListenDesc& desc);
    ESocketStatus Accept(TcpSocket& out_client);
    ESocketStatus SendAll(std::span<const std::byte> bytes);
    ESocketStatus RecvAll(std::span<std::byte> bytes);

private:
    explicit TcpSocket(uintptr_t native_handle);

private:
    uintptr_t native_handle{0};
};

} // namespace Moer::Network