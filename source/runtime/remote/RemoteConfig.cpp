#include "remote/RemoteConfig.h"
#include "config/GlobalConfig.h"
#include "misc/Types.h"

#include <limits>

namespace Moer::remote {

namespace {

uint16 ClampPort(uint32 port, uint16 fallback) {
    if (port == 0 || port > std::numeric_limits<uint16>::max()) {
        return fallback;
    }

    return static_cast<uint16>(port);
}

} // namespace

RemoteConfig MakeRemoteConfigFromGlobalConfig(const Config::GlobalConfig& config) {
    RemoteConfig remote_config;
    remote_config.enable       = config.engine.remote.enable;
    remote_config.bind_address = config.engine.remote.bind_address;
    remote_config.http_port    = ClampPort(config.engine.remote.http_port, remote_config.http_port);
    remote_config.websocket_port =
        ClampPort(config.engine.remote.websocket_port, remote_config.websocket_port);
    return remote_config;
}

} // namespace Moer::remote