#include "taskgraph/TaskPipe.h"
#include "taskgraph/GraphTask.h"

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
    if (m_last_event) {
        m_last_event->Wait();
    }
}

GraphEventRef TaskPipe::Enqueue(
    std::function<void()>&& _func,
    GraphEventArray&& _deps,
    EThread::Type _thread
) {
    TaskNode* new_task = TaskNode::Create(std::move(_func), std::move(_deps), _thread);
    
    GraphEventRef wait_event;
    
    if (m_current != nullptr) {
        TaskNode* old_current = m_current;
        TaskNode* expected = nullptr;
        if (old_current->next.compare_exchange_strong(
            expected, new_task, std::memory_order_acq_rel)) {
            m_current = new_task;
            m_last_event = new_task->event;
            new_task->Release();
            return new_task->event;
        }
        wait_event = old_current->event;
    } else if (m_last_event) {
        wait_event = m_last_event;
    }
    
    m_current = new_task;
    m_last_event = new_task->event;
    
    GraphEventArray prereqs_to_wait;
    if (wait_event) {
        prereqs_to_wait.push_back(wait_event);
    }
    for (auto& dep : new_task->prereqs) {
        prereqs_to_wait.push_back(dep);
    }
    
    auto create_info = LambdaTask::Create([this, new_task]() {
        ExecuteTaskChain(new_task);
    }, new_task->logical_thread);
    
    if (!prereqs_to_wait.empty()) {
        create_info.Wait(std::move(prereqs_to_wait));
    }
    
    create_info.Dispatch();
    
    new_task->Release();
    
    return new_task->event;
}

GraphEventRef TaskPipe::Close() {
    GraphEventRef last_event = m_last_event;
    
    if (m_current) {
        TaskNode* expected = nullptr;
        m_current->next.compare_exchange_strong(expected, m_current, std::memory_order_acq_rel);
    }
    
    m_current = nullptr;
    
    return last_event;
}

GraphEventRef TaskPipe::GetLastEvent() const {
    return m_last_event;
}

void TaskPipe::ExecuteTaskChain(TaskNode* _task) {
    TaskNode* current = _task;
    
execute_batch:
    current->lambda();
    current->event->TryUnlockSubsequents();
    
    TaskNode* next = current->next.load(std::memory_order_acquire);
    
    if (next == nullptr) {
        if (current->next.compare_exchange_strong(
            next, current, std::memory_order_acq_rel)) {
            current->Release();
            return;
        }
    }
    
    if (next != nullptr && next != current) {
        current->Release();
        current = next;
        
        if (!current->prereqs.empty()) {
            bool has_uncompleted = false;
            for (auto& dep : current->prereqs) {
                if (!dep->IsComplete()) {
                    has_uncompleted = true;
                    break;
                }
            }
            
            if (has_uncompleted) {
                GraphEventArray prereqs_to_wait;
                for (auto& dep : current->prereqs) {
                    prereqs_to_wait.push_back(dep);
                }
                
                auto create_info = LambdaTask::Create([this, current]() {
                    ExecuteTaskChain(current);
                }, current->logical_thread);
                
                create_info.Wait(std::move(prereqs_to_wait));
                create_info.Dispatch();
                return;
            }
        }
        
        goto execute_batch;
    }
    
    current->Release();
}

} // namespace Moer
