#include "remote/RemoteModuleController.h"

namespace Moer::remote {

RemoteModuleController::RemoteModuleController(const SharedPtr<IRemoteModuleControllerBridge>& bridge) :
    m_bridge(bridge) {}

bool RemoteModuleController::IsValid() const {
    return !m_bridge.expired();
}

bool RemoteModuleController::IsEnabled() const {
    const auto bridge = m_bridge.lock();
    return bridge && bridge->IsEnabled();
}

bool RemoteModuleController::IsRunning() const {
    const auto bridge = m_bridge.lock();
    return bridge && bridge->IsRunning();
}

bool RemoteModuleController::SetEnabled(bool enabled) const {
    const auto bridge = m_bridge.lock();
    return bridge && bridge->SetEnabled(enabled);
}

RemoteConfig RemoteModuleController::GetConfigSnapshot() const {
    const auto bridge = m_bridge.lock();
    return bridge ? bridge->GetConfigSnapshot() : RemoteConfig{};
}

} // namespace Moer::remote