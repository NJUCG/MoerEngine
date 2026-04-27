#include "taskgraph/TaskPipe.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"

namespace Moer {

#ifdef STATS
std::atomic<uint32> TaskPipe::s_pipe_id_counter{0};
#endif

TaskPipe::TaskPipe() {
#ifdef STATS
    m_pipe_id = s_pipe_id_counter.fetch_add(1, std::memory_order_relaxed);
#endif
}

TaskPipe::~TaskPipe() {
    GraphEventRef last_event;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        last_event = m_last_event;
    }
    if (last_event) {
        last_event->Wait();
    }
}

GraphEventRef TaskPipe::Enqueue(
    std::function<void()>&& _func,
    GraphEventArray&& _deps,
    EThread::Type _thread
) {
    GraphEventArray prereqs_to_wait;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_last_event) {
        prereqs_to_wait.push_back(m_last_event);
    }
    prereqs_to_wait.insert(prereqs_to_wait.end(), _deps.begin(), _deps.end());

    auto create_info = LambdaTask::Create(std::move(_func), _thread);
    if (!prereqs_to_wait.empty()) {
        create_info.Wait(std::move(prereqs_to_wait));
    }

    GraphEventRef event = create_info.Dispatch();
    m_last_event        = event;
    return event;
}

GraphEventRef TaskPipe::Close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    GraphEventRef last_event = m_last_event;
    return last_event;
}

GraphEventRef TaskPipe::GetLastEvent() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_event;
}

} // namespace Moer
