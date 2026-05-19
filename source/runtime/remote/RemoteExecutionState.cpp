#include "remote/RemoteExecutionState.h"

namespace Moer::remote {

const char* ToString(ERemoteExecutionState state) {
    switch (state) {
        case ERemoteExecutionState::Queued:
            return "Queued";
        case ERemoteExecutionState::Running:
            return "Running";
        case ERemoteExecutionState::Completed:
            return "Completed";
        case ERemoteExecutionState::Failed:
            return "Failed";
        case ERemoteExecutionState::Cancelled:
            return "Cancelled";
    }

    return "Unknown";
}

} // namespace Moer::remote