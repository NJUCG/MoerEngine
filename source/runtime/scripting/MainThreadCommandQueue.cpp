#include "scripting/MainThreadCommandQueue.h"

namespace Moer::scripting {

MainThreadCommandQueue::~MainThreadCommandQueue() {
    CancelPending("MainThreadCommandQueue destroyed before command execution.");
}

void MainThreadCommandQueue::ProcessPendingCommands(Scene& scene) {
    std::deque<std::unique_ptr<PendingCommandBase>> pending_commands;
    {
        std::lock_guard lock(m_mutex);
        pending_commands.swap(m_pending_commands);
    }

    for (auto& pending_command : pending_commands) {
        pending_command->Execute(scene);
    }
}

void MainThreadCommandQueue::CancelPending(std::string_view reason) {
    std::deque<std::unique_ptr<PendingCommandBase>> pending_commands;
    {
        std::lock_guard lock(m_mutex);
        pending_commands.swap(m_pending_commands);
    }

    for (auto& pending_command : pending_commands) {
        pending_command->Cancel(reason);
    }
}

void MainThreadCommandQueue::Enqueue(std::unique_ptr<PendingCommandBase> command) {
    std::lock_guard lock(m_mutex);
    m_pending_commands.push_back(std::move(command));
}

} // namespace Moer::scripting