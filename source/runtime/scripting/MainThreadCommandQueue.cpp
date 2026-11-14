#include "scripting/MainThreadCommandQueue.h"

namespace Moer::scripting {

MainThreadCommandQueue::~MainThreadCommandQueue() {
    CancelPending("MainThreadCommandQueue destroyed before command execution.");
}

void MainThreadCommandQueue::ProcessPendingCommands(Scene& scene) {
    for (auto& pending_command : TakePendingCommands()) {
        pending_command->Execute(scene);
    }
}

void MainThreadCommandQueue::CancelPending(std::string_view reason) {
    for (auto& pending_command : TakePendingCommands()) {
        pending_command->Cancel(reason);
    }
}

MainThreadCommandQueue::PendingCommands MainThreadCommandQueue::TakePendingCommands() {
    PendingCommands pending_commands;
    {
        std::lock_guard lock(m_mutex);
        pending_commands.swap(m_pending_commands);
    }
    return pending_commands;
}

void MainThreadCommandQueue::Enqueue(std::unique_ptr<PendingCommandBase> command) {
    std::lock_guard lock(m_mutex);
    m_pending_commands.push_back(std::move(command));
}

} // namespace Moer::scripting
