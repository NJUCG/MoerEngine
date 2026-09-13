#include "rhi/RHIThreadOwnership.h"

#include <cassert>
#include <stdexcept>
#include <string>
#include <utility>

namespace Moer::Render {
namespace {

thread_local ERHIThreadRole g_rhi_thread_role = ERHIThreadRole::Unknown;
thread_local uint32_t       g_rhi_command_access_restriction_depth = 0;

} // namespace

ERHIThreadRole GetCurrentRHIThreadRole() noexcept {
    return g_rhi_thread_role;
}

const char* RHIThreadRoleName(ERHIThreadRole _role) noexcept {
    switch (_role) {
        case ERHIThreadRole::Unknown:
            return "Unknown";
        case ERHIThreadRole::Executor:
            return "Executor";
        case ERHIThreadRole::Translate:
            return "Translate";
        case ERHIThreadRole::Submission:
            return "Submission";
        case ERHIThreadRole::Completion:
            return "Completion";
        case ERHIThreadRole::RecordWorker:
            return "RecordWorker";
    }
    return "Invalid";
}

bool IsRHIBlockingCallAllowedOnCurrentThread() noexcept {
    return GetCurrentRHIThreadRole() == ERHIThreadRole::Unknown;
}

bool IsRHICommandAccessRestricted() noexcept {
    return g_rhi_command_access_restriction_depth != 0;
}

void ValidateRHICommandAccess(std::string_view _operation) {
    if (!IsRHICommandAccessRestricted()) {
        return;
    }
    throw std::logic_error(
        std::string(_operation) +
        " is forbidden during restricted RenderGraph Prepare"
    );
}

RHICommandAccessRestrictionScope::RHICommandAccessRestrictionScope(
    bool _restricted
) noexcept : restricted(_restricted) {
    if (restricted) {
        ++g_rhi_command_access_restriction_depth;
    }
}

RHICommandAccessRestrictionScope::~RHICommandAccessRestrictionScope() {
    if (!restricted) {
        return;
    }
    assert(g_rhi_command_access_restriction_depth != 0);
    --g_rhi_command_access_restriction_depth;
}

RHIThreadRoleScope::RHIThreadRoleScope(ERHIThreadRole _role) noexcept :
    previous_role(g_rhi_thread_role) {
    g_rhi_thread_role = _role;
}

RHIThreadRoleScope::~RHIThreadRoleScope() {
    g_rhi_thread_role = previous_role;
}

RHITransferableOwnershipGate::Lease::~Lease() {
    Release();
}

RHITransferableOwnershipGate::Lease::Lease(Lease&& _other) noexcept :
    owner(std::exchange(_other.owner, nullptr)) {}

RHITransferableOwnershipGate::Lease&
RHITransferableOwnershipGate::Lease::operator=(Lease&& _other) noexcept {
    if (this == &_other) {
        return *this;
    }
    Release();
    owner = std::exchange(_other.owner, nullptr);
    return *this;
}

void RHITransferableOwnershipGate::Lease::Release() noexcept {
    if (owner == nullptr) {
        return;
    }
    RHITransferableOwnershipGate* gate = std::exchange(owner, nullptr);
    gate->permit.release();
}

RHITransferableOwnershipGate::Lease RHITransferableOwnershipGate::Acquire() {
    permit.acquire();
    return Lease(this);
}

} // namespace Moer::Render
