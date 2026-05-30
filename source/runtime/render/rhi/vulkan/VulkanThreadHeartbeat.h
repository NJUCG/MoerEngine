#pragma once

#include "string/String.h"

#include <cstdint>

namespace Moer::Render {

class VulkanThreadHeartbeat {
public:
    struct Handle {
        uint32_t slot_index = UINT32_MAX;

        bool IsValid() const {
            return slot_index != UINT32_MAX;
        }
    };

    static VulkanThreadHeartbeat& Get();

    bool   Enabled() const;
    Handle Register(StringView label, StringView stage = MOER_TEXT("Registered"));
    void   Pulse(Handle handle, StringView stage);
    void   PulseCurrent(StringView stage);
    void   Unregister(Handle& handle);

private:
    VulkanThreadHeartbeat();
    ~VulkanThreadHeartbeat();

    VulkanThreadHeartbeat(const VulkanThreadHeartbeat&)                = delete;
    VulkanThreadHeartbeat& operator=(const VulkanThreadHeartbeat&)     = delete;
    VulkanThreadHeartbeat(VulkanThreadHeartbeat&&) noexcept            = delete;
    VulkanThreadHeartbeat& operator=(VulkanThreadHeartbeat&&) noexcept = delete;

    void EnsureStarted();
    void MonitorLoop();

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace Moer::Render