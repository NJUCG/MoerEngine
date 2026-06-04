#include "remote/RemoteEventHub.h"

#include <utility>

namespace Moer::remote {

uint64_t RemoteEventHub::Subscribe(RemoteEventCallback callback) {
    std::lock_guard lock(m_mutex);
    const uint64_t  subscription_id = m_next_subscription_id++;
    m_callbacks.emplace(subscription_id, std::move(callback));
    return subscription_id;
}

void RemoteEventHub::Unsubscribe(uint64_t subscription_id) {
    std::lock_guard lock(m_mutex);
    m_callbacks.erase(subscription_id);
}

void RemoteEventHub::Publish(RemoteEvent event) {
    std::vector<RemoteEventCallback> callbacks;
    {
        std::lock_guard lock(m_mutex);
        callbacks.reserve(m_callbacks.size());
        for (const auto& [_, callback] : m_callbacks) {
            callbacks.push_back(callback);
        }
    }

    for (const auto& callback : callbacks) {
        callback(event);
    }
}

} // namespace Moer::remote