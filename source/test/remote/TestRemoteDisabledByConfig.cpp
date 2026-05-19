#include "remote/RemoteConfig.h"
#include "remote/RemoteModule.h"

#include <iostream>

int main() {
    Moer::remote::RemoteConfig config;
    config.enable         = false;
    config.bind_address   = "127.0.0.1";
    config.http_port      = 18170;
    config.websocket_port = 18171;

    bool                       submit_called = false;
    Moer::remote::RemoteModule module(
        config, [&submit_called](Moer::scripting::ScriptExecutionRequest request) {
            (void)request;
            submit_called = true;
            return Moer::scripting::ScriptExecutionFuture();
        }
    );

    if (module.IsEnabled()) {
        std::cerr << "Remote module should report disabled before Start." << std::endl;
        return 1;
    }

    if (!module.SetEnabled(false)) {
        std::cerr << "Remote module SetEnabled(false) should succeed when already disabled." << std::endl;
        return 2;
    }

    if (module.Start()) {
        std::cerr << "Remote module Start should return false when config.enable=false." << std::endl;
        return 3;
    }

    if (module.IsRunning()) {
        std::cerr << "Remote module should not run when disabled." << std::endl;
        return 4;
    }

    if (!module.SetEnabled(true)) {
        std::cerr << "Remote module should support runtime enable." << std::endl;
        return 5;
    }

    if (!module.IsEnabled() || !module.IsRunning()) {
        std::cerr << "Remote module should be enabled and running after runtime enable." << std::endl;
        return 6;
    }

    if (!module.SetEnabled(false)) {
        std::cerr << "Remote module should support runtime disable." << std::endl;
        return 7;
    }

    if (module.IsEnabled() || module.IsRunning()) {
        std::cerr << "Remote module should be disabled and stopped after runtime disable." << std::endl;
        return 8;
    }

    if (submit_called) {
        std::cerr << "Remote module should not call submit_fn while disabled." << std::endl;
        return 9;
    }

    std::cout << "TestRemoteDisabledByConfig passed." << std::endl;
    return 0;
}