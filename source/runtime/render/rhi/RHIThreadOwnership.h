#pragma once

#include "RenderAPI.h"

#include <cstdint>
#include <semaphore>

namespace Moer::Render {

// Logical ownership roles for the modern RHI pipeline. These are intentionally
// independent from task-graph thread indices: a role describes which RHI
// lifecycle domain the current OS thread owns while executing a scoped body.
enum class ERHIThreadRole : uint8_t {
    Unknown,
    Executor,
    Translate,
    Submission,
    Completion,
    RecordWorker,
};

RENDER_API ERHIThreadRole GetCurrentRHIThreadRole() noexcept;
RENDER_API const char*    RHIThreadRoleName(ERHIThreadRole _role) noexcept;

// Public blocking lifecycle calls may wait for every owned stage downstream.
// Calling one from any stage owner can therefore form a self-join.
RENDER_API bool IsRHIBlockingCallAllowedOnCurrentThread() noexcept;

class RENDER_API RHIThreadRoleScope final {
public:
    explicit RHIThreadRoleScope(ERHIThreadRole _role) noexcept;
    ~RHIThreadRoleScope();

    RHIThreadRoleScope(const RHIThreadRoleScope&) = delete;
    RHIThreadRoleScope& operator=(const RHIThreadRoleScope&) = delete;
    RHIThreadRoleScope(RHIThreadRoleScope&&) = delete;
    RHIThreadRoleScope& operator=(RHIThreadRoleScope&&) = delete;

private:
    ERHIThreadRole previous_role{ERHIThreadRole::Unknown};
};

// Unlike std::mutex ownership, a semaphore permit may be acquired by the
// translate owner and released by the submission owner. A recorded packet
// carries this move-only lease across that boundary and keeps queue-local
// recording state atomic with its eventual native submit.
class RENDER_API RHITransferableOwnershipGate final {
public:
    class RENDER_API Lease final {
    public:
        Lease() noexcept = default;
        ~Lease();

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& _other) noexcept;
        Lease& operator=(Lease&& _other) noexcept;

        [[nodiscard]] bool OwnsGate() const noexcept {
            return owner != nullptr;
        }

        void Release() noexcept;

    private:
        friend class RHITransferableOwnershipGate;
        explicit Lease(RHITransferableOwnershipGate* _owner) noexcept : owner(_owner) {}

        RHITransferableOwnershipGate* owner{nullptr};
    };

    RHITransferableOwnershipGate() = default;
    ~RHITransferableOwnershipGate() = default;

    RHITransferableOwnershipGate(const RHITransferableOwnershipGate&) = delete;
    RHITransferableOwnershipGate& operator=(const RHITransferableOwnershipGate&) = delete;
    RHITransferableOwnershipGate(RHITransferableOwnershipGate&&) = delete;
    RHITransferableOwnershipGate& operator=(RHITransferableOwnershipGate&&) = delete;

    [[nodiscard]] Lease Acquire();

private:
    std::binary_semaphore permit{1};
};

} // namespace Moer::Render
