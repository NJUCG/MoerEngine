#include "taskgraph/TaskGraph.h"
#include "taskgraph/ThreadManager.h"
#include "platform/Platform.h"
#include "taskgraph/GraphTask.h"
//#define NOMINMAX 1
#undef max
TaskGraph* TaskGraph::instance = nullptr;

TaskGraph& TaskGraph::GetInterface() {
    assert(instance != nullptr);
    return *instance;
}

void TaskGraph::Init() {
    if (instance == nullptr) {
        instance = new TaskGraph();
    }
}
void TaskGraph::AttachToNameThread(EThread::Type type) {
    m_workers[EThread::GetThreadIndex(type)].attached = true;
}
TaskThreadBase& TaskGraph::GetThread(ThreadIndex index) {
    assert(m_workers[index].taskThread->GetIndex() == index);
    return *m_workers[index].taskThread;
}

void TaskGraph::WakeUpWorkerThread(int32_t threadIndex, QueueIndex index) {
    assert(threadIndex >= 0);
    m_workers[threadIndex].taskThread->Wake(index);
}
void TaskGraph::WaitUntilTaskComplete(const GraphEventRef& task, EThread::Type currentThread) { WaitUntilTasksComplete({task}, currentThread); }
void TaskGraph::WaitUntilTaskComplete(GraphEventRef&& task, EThread::Type currentThread) { WaitUntilTasksComplete({std::move(task)}, currentThread); };
TaskGraph::TaskGraph() {
    assert(instance == nullptr);
    int32_t actual_thread_num = Platform::GetProcessorCoreCount();

    m_named_thread_count                   = EThread::NamedThreadCount;
    int32_t min_worker_thread_per_priority = 1;
    int32_t min_thread_count               = min_worker_thread_per_priority * EThread::PriorityCount + m_named_thread_count;

    m_thread_count = std::max(min_thread_count, actual_thread_num);

    m_worker_thread_count = m_thread_count - m_named_thread_count;

    m_worker_per_priority      = m_worker_thread_count / EThread::PriorityCount;
    m_worker_per_priority      = m_worker_thread_count % EThread::PriorityCount ? m_worker_per_priority + 1 : m_worker_per_priority;
    int32_t priority_set_count = m_worker_thread_count / EThread::PriorityCount;

    for (int32_t i = 0; i < m_thread_count; i++) {
        EThread::Type  type     = EThread::SetThreadIndex(EThread::UNKNOWN_THREAD, i);
        ThreadPriority priority = GetThreadPriorityFromIndex(i) << EThread::PRIORITY_SHEFT;
        type                    = EThread::SetPriority(type, priority);
        if (i >= m_named_thread_count) {
            //any_thread
            m_workers[i].taskThread = new TaskThreadAnyThread(type);
        } else {
            m_workers[i].taskThread = new NamedThread;
            //named_thread
        }
        m_workers[i].taskThread->SetAttributes(type, &m_workers[i]);
    }
    instance = this;//set here to make sure access of worker_threads below
    //todo no handle for thread groups
    for (int32_t i = m_named_thread_count; i < m_thread_count; i++) {
        int32_t     priority      = GetThreadPriorityFromIndex(i);
        std::string name          = "WorkerThread_" + GetPriorityStr(priority) + "_" + std::to_string((i - m_named_thread_count) % m_worker_per_priority);
        m_workers[i].actualThread = RunnableThread::Create(&GetThread(i), name, 1ull << i);
        m_workers[i].attached     = true;
    }
}

TaskGraph::~TaskGraph() {
    for (int32_t i = 0; i < m_thread_count; i++) {

        m_workers[i].taskThread->RequestQuit(QUIT);
    }
    for (int32_t i = 0; i < m_thread_count; i++) {
        if (i >= m_named_thread_count) {
            m_workers[i].actualThread->WaitUntilFinished();

            delete m_workers[i].actualThread;
            m_workers[i].actualThread = nullptr;
        }
        m_workers[i].attached = false;
    }
    instance = nullptr;
}
EThread::Type TaskGraph::GetCurrentThread(bool localQueue) {
    if (Platform::GetCurrentThreadID() == ThreadManager::g_game_thread_id) return EThread::Type(localQueue ? (EThread::EGameThread | EThread::LOCAL_QUEUE) : EThread::EGameThread);
    auto thread = ThreadManager::Instance().GetRunnableThread(Platform::GetCurrentThreadID());
    if (thread) {
        ThreadIndex index = thread->m_runnable->GetIndex();
        return EThread::Type(m_workers[index].taskThread->m_threadType | (localQueue ? EThread::LOCAL_QUEUE : EThread::MAIN_QUEUE));
    }
    assert(false);
    return EThread::UNKNOWN_THREAD;
}
bool TaskGraph::IsThreadProcessingTask(EThread::Type index) {
    return m_workers[EThread::GetThreadIndex(index)].taskThread->IsProcessingTask(EThread::GetQueueIndex(index));
}
void TaskGraph::ProcessThreadUntilIdle(EThread::Type index){};
void TaskGraph::ProcessThreadUntilReturn(EThread::Type index) {
    m_workers[EThread::GetThreadIndex(index)].taskThread->ProcessTaskUntilQuit(EThread::GetQueueIndex(index));
};
void TaskGraph::ReturnThread(EThread::Type index) {
    QueueIndex queue_index = EThread::GetQueueIndex(index);
    m_workers[EThread::GetThreadIndex(index)].taskThread->RequestQuit(queue_index);
};
void TaskGraph::WaitUntilTasksComplete(const GraphEventArray& task_events, EThread::Type currentThread) {
    EThread::Type current_thread_type = currentThread;

    bool is_named_thread = false;
    if (EThread::GetThreadIndex(currentThread) == EThread::UNKNOWN_THREAD) {
        ThreadPriority priority = EThread::GetThreadPriority(currentThread);
        currentThread           = GetCurrentThread();
        current_thread_type     = EThread::SetPriority(currentThread, priority << EThread::PRIORITY_SHEFT);

    } else {
        currentThread = (EThread::Type)EThread::GetThreadIndex(currentThread);
    }
    is_named_thread = EThread::GetThreadIndex(current_thread_type) < EThread::NamedThreadCount;
    if (is_named_thread && !IsThreadProcessingTask(current_thread_type)) {
        bool pending = false;
        for (int32_t index = 0; index < task_events.size(); index++) {
            GraphEvent* task = task_events[index].Get();
            if (task != nullptr && !task->IsComplete()) {
                pending = true;
                break;
            }
        }
        if (!pending) {
            // no task left
            return;
        }
        // task not complete
        //TODO: named thread waiting for a task on other thread which is waiting on task on this named thread will cause dead lock
        // because named thread won't flush its task queue until waiting operation
        //check dependency ?

        GraphTask<ReturnGraphTask>::CreateTask(&task_events, current_thread_type).ConstructAndDispatchWhenReady(current_thread_type);
        ProcessThreadUntilReturn(current_thread_type);
    } else {
        bool pending = false;
        for (int32_t index = 0; index < task_events.size(); index++) {
            GraphEvent* task = reinterpret_cast<GraphEvent*>(task_events[index].Get());
            if (task != nullptr && !task->IsComplete()) {
                pending = true;
                break;
            }
        }
        if (!pending) {
            // no task left
            return;
        }
        ScopeEventRef scope_event;
        TriggerEventWhenTasksComplete(scope_event.m_event, task_events, currentThread);
    }
}

void TaskGraph::TriggerEventWhenTasksComplete(Event* event, const GraphEventArray& task_events, EThread::Type currentThread, EThread::Type triggerThread) {
    assert(event != nullptr);
    bool pending = true;
    if (task_events.size() < 8)// don't bother to check for completion if there are lots of prereqs...too expensive to check
    {
        bool pending = false;
        for (int32_t index = 0; index < task_events.size(); index++) {
            GraphEvent* task = reinterpret_cast<GraphEvent*>(task_events[index].Get());
            if (task != nullptr && !task->IsComplete()) {
                pending = true;
                break;
            }
        }
    }
    if (!pending) {
        event->Trigger();
        return;
    }
    GraphTask<TriggerEventGraphTask>::CreateTask(&task_events, currentThread).ConstructAndDispatchWhenReady(event, triggerThread);
}
void TaskGraph::QueueTask(BaseGraphTask* task, EThread::Type prefered_thread, EThread::Type current_thread, bool wake_worker) {
    if (EThread::GetThreadIndex(prefered_thread) == EThread::UNKNOWN_THREAD) {//any thread is ok
        EThread::Type  prefered_thread         = task->GetPreferredThread();
        ThreadPriority priority                = task->GetPriority();
        int32_t        possible_thread_to_wake = m_task_queue[priority].Push(task, 1);
        if (possible_thread_to_wake >= 0) {
            //start task thread
            possible_thread_to_wake = possible_thread_to_wake + priority * m_worker_per_priority + m_named_thread_count;
            WakeUpWorkerThread(possible_thread_to_wake, 0);
        }
        return;
    }
    //named thread
    EThread::Type currentThread;
    if (EThread::GetThreadIndex(current_thread) == EThread::UNKNOWN_THREAD) {
        currentThread = (EThread::Type)EThread::GetThreadIndex(GetCurrentThread());

    } else {
        currentThread = (EThread::Type)EThread::GetThreadIndex(current_thread);
    }
    ThreadIndex index       = EThread::GetThreadIndex(prefered_thread);
    QueueIndex  queue_index = EThread::GetQueueIndex(prefered_thread);
    if (currentThread == index) {
        m_workers[index].taskThread->EnqueueFromCurrentThread(queue_index, task);
    } else {
        m_workers[index].taskThread->EnqueueFromExternThread(queue_index, task);
    }
}
BaseGraphTask* TaskGraph::DequeueTask(int32_t threadIndex) {
    int32_t        priority          = GetThreadPriorityFromIndex(threadIndex);
    int32_t        index_in_priority = (threadIndex - m_named_thread_count) % m_worker_per_priority;
    BaseGraphTask* task              = m_task_queue[priority].Pop(index_in_priority);
    //if (task != nullptr) {
    //	task = m_global_queue.pop(index_in_priority);
    //}
    return task;
}