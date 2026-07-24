#include "rhi/RHIThreadOwnership.h"

#include <utility>

namespace Moer::Render {
namespace {

thread_local ERHIThreadRole g_rhi_thread_role = ERHIThreadRole::Unknown;

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
