#pragma once

#include "remote/RemoteApi.h"

#include <cstdint>
#include <string>

namespace Moer::Config {
struct GlobalConfig;
}

namespace Moer::remote {

// 描述 Remote 模块的基础网络配置
struct REMOTE_API RemoteConfig {
    bool        enable         = false;
    std::string bind_address   = "127.0.0.1";
    uint16_t    http_port      = 18080;
    uint16_t    websocket_port = 18081;
};

// 从全局配置中提取 Remote 模块所需的配置项
REMOTE_API RemoteConfig MakeRemoteConfigFromGlobalConfig(const Config::GlobalConfig& config);

} // namespace Moer::remote