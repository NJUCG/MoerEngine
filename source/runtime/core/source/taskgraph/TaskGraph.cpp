#include "taskgraph/AnyThreadScheduler.h"
#include "taskgraph/TaskGraph.h"
#include "platform/Platform.h"
#include "string/Format.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/ThreadManager.h"
//#define NOMINMAX 1
#undef max
TaskGraph* TaskGraph::instance = nullptr;

TaskGraph& TaskGraph::GetInterface() {
    assert(instance != nullptr);
    return *instance;
}

void TaskGraph::Init() {
    if (instance == nullptr) {
        instance = MoerNew(TaskGraph)();
    }
}
void TaskGraph::Shutdown() {
    if (instance != nullptr) {
        MoerDelete(instance);
        instance = nullptr;
    }
}
void TaskGraph::AttachToNameThread(EThread::Type type) {
    m_workers[EThread::GetThreadIndex(type)].attached = true;
}
TaskThreadBase& TaskGraph::GetThread(ThreadIndex index) {
    assert(m_workers[index].task_thread->GetIndex() == index);
    return *m_workers[index].task_thread;
}

void TaskGraph::WakeUpWorkerThread(int32_t threadIndex, QueueIndex index) {
    assert(threadIndex >= 0);
    m_workers[threadIndex].task_thread->Wake(index);
}
void TaskGraph::WaitUntilTaskComplete(const GraphEventRef& task, EThread::Type currentThread) {
    WaitUntilTasksComplete({task}, currentThread);
}
void TaskGraph::WaitUntilTaskComplete(GraphEventRef&& task, EThread::Type currentThread) {
    WaitUntilTasksComplete({std::move(task)}, currentThread);
};
TaskGraph::TaskGraph() {
    assert(instance == nullptr);
    int32_t actual_thread_num = Platform::GetProcessorCoreCount();

    m_named_thread_count                   = EThread::NamedThreadCount;
    int32_t min_worker_thread_per_priority = 1;
    int32_t min_thread_count = min_worker_thread_per_priority * EThread::PriorityCount + m_named_thread_count;

    m_thread_count = std::max(min_thread_count, actual_thread_num);

    m_worker_thread_count = m_thread_count - m_named_thread_count;

    m_worker_per_priority = m_worker_thread_count / EThread::PriorityCount;
    m_worker_per_priority =
        m_worker_thread_count % EThread::PriorityCount ? m_worker_per_priority + 1 : m_worker_per_priority;

    for (int32_t i = 0; i < m_thread_count; i++) {
        EThread::Type  type     = EThread::SetThreadIndex(EThread::UNKNOWN_THREAD, i);
        ThreadPriority priority = GetThreadPriorityFromIndex(i) << EThread::PRIORITY_SHEFT;
        type                    = EThread::SetPriority(type, priority);
        if (i >= m_named_thread_count) {
            //any_thread
            m_workers[i].task_thread = new TaskThreadAnyThread(type);
            m_worker_count_per_priority[GetThreadPriorityFromIndex(i)]++;
        } else {
            m_workers[i].task_thread = new NamedThread;
            //named_thread
        }
        m_workers[i].task_thread->SetAttributes(type, &m_workers[i]);
    }
    instance = this; //set here to make sure access of worker_threads below
    m_scheduler = std::make_unique<AnyThreadScheduler>(
        m_named_thread_count,
        m_worker_per_priority,
        m_worker_count_per_priority,
        [this](int32_t threadIndex) {
            WakeUpWorkerThread(threadIndex, 0);
        }
    );
    //todo no handle for thread groups
    for (int32_t i = m_named_thread_count; i < m_thread_count; i++) {
        int32_t     priority = GetThreadPriorityFromIndex(i);
        Moer::Utf8StringView priority_name = GetPriorityName(priority);
        Moer::Utf8String name = Moer::Utf8Printf(
            MOER_ASCII_TEXT("WorkerThread_{}_{}"),
            priority_name.data(),
            (i - m_named_thread_count) % m_worker_per_priority
        );
        m_workers[i].actual_thread = RunnableThread::Create(
            &GetThread(i),
            ThreadAttributes{.affinity = Affinity::AnyOf(i, std::move(Affinity::All())), .name = name}
        );
        m_workers[i].attached = true;
    }
}

TaskGraph::~TaskGraph() {
    for (int32_t i = 0; i < m_thread_count; i++) {
        m_workers[i].task_thread->RequestQuit(QUIT);
    }
    for (int32_t i = 0; i < m_thread_count; i++) {
        if (i >= m_named_thread_count) {
            m_workers[i].actual_thread->WaitUntilFinished();

            MoerDelete(m_workers[i].actual_thread);
            m_workers[i].actual_thread = nullptr;
        }
        m_workers[i].attached = false;
    }
    instance = nullptr;
}
EThread::Type TaskGraph::GetCurrentThread(bool localQueue) {
    ThreadIndex index = Platform::GetCurrentThreadID();
    if (Platform::GetCurrentThreadID() == ThreadManager::g_game_thread_id)
        return EThread::Type(
            localQueue ? (EThread::EMainThread | EThread::LOCAL_QUEUE) : EThread::EMainThread
        );
    if (Platform::GetCurrentThreadID() == ThreadManager::g_render_thread_id)
        return EThread::Type(
            localQueue ? (EThread::ERenderThread | EThread::LOCAL_QUEUE) : EThread::ERenderThread
        );
    auto* thread = ThreadManager::Instance().GetRunnableThread(Platform::GetCurrentThreadID());
    if (thread) {
        ThreadIndex index = thread->m_runnable->GetIndex();
        if (index < 0 || index >= m_thread_count) {
            return EThread::SetPriority(
                EThread::UNKNOWN_THREAD,
                EThread::NORMAL_PRI
            );
        }
        return EThread::Type(
            m_workers[index].task_thread->m_thread_type |
            (localQueue ? EThread::LOCAL_QUEUE : EThread::MAIN_QUEUE)
        );
    }
    return EThread::SetPriority(
        EThread::UNKNOWN_THREAD,
        EThread::NORMAL_PRI
    );
}
bool TaskGraph::IsThreadProcessingTask(EThread::Type index) {
    return m_workers[EThread::GetThreadIndex(index)].task_thread->IsProcessingTask(
        EThread::GetQueueIndex(index)
    );
}
void TaskGraph::ProcessThreadUntilIdle(EThread::Type index) {};
void TaskGraph::ProcessThreadUntilReturn(EThread::Type index) {
    m_workers[EThread::GetThreadIndex(index)].task_thread->ProcessTaskUntilQuit(EThread::GetQueueIndex(index)
    );
};
void TaskGraph::ReturnThread(EThread::Type index) {
    QueueIndex queue_index = EThread::GetQueueIndex(index);
    m_workers[EThread::GetThreadIndex(index)].task_thread->RequestQuit(queue_index);
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
        if (task_events.size() < 8) {
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
        }

        // task not complete
        //TODO: named thread waiting for a task on other thread which is waiting on task on this named thread will cause dead lock
        // because named thread won't flush its task queue until waiting operation
        //check dependency ?

        // GraphTask<ReturnGraphTask>::CreateTask(&task_events, current_thread_type).ConstructAndDispatchWhenReady(current_thread_type);
        GraphTask<ReturnGraphTask>::Create(current_thread_type).Wait(task_events).Dispatch();
        ProcessThreadUntilReturn(current_thread_type);
    } else {
        ScopeEventRef scope_event;
        TriggerEventWhenTasksComplete(scope_event.m_event, task_events, currentThread);
    }
}

void TaskGraph::TriggerEventWhenTasksComplete(
    Event*                 event,
    const GraphEventArray& task_events,
    EThread::Type          currentThread,
    EThread::Type          triggerThread
) {
    assert(event != nullptr);
    bool pending = true;
    if (task_events.size() <
        8) // don't bother to check for completion if there are lots of prereqs...too expensive to check
    {
        bool pending = false;
        for (int32_t index = 0; index < task_events.size(); index++) {
            GraphEventRef task = task_events[index];
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
    // GraphTask<TriggerEventGraphTask>::CreateTask(&task_events, currentThread).ConstructAndDispatchWhenReady(event, triggerThread);

    GraphTask<TriggerEventGraphTask>::Create(event, triggerThread).Wait(task_events).Dispatch();
}
void TaskGraph::QueueTask(
    BaseGraphTask* task,
    EThread::Type  _prefered_thread,
    EThread::Type  _current_thread,
    bool           wake_worker
) {
    if (EThread::GetThreadIndex(_prefered_thread) == EThread::UNKNOWN_THREAD) { //any thread is ok
        m_scheduler->Enqueue(task, _current_thread);
        return;
    }
    //named thread
    EThread::Type temp_current_thread;
    temp_current_thread = GetCurrentThread();

    ThreadIndex index       = EThread::GetThreadIndex(_prefered_thread);
    QueueIndex  queue_index = EThread::GetQueueIndex(_prefered_thread);
    if (EThread::GetThreadIndex(temp_current_thread) == index) {
        m_workers[index].task_thread->EnqueueFromCurrentThread(queue_index, task);
    } else {
        m_workers[index].task_thread->EnqueueFromExternThread(queue_index, task);
    }
}
BaseGraphTask* TaskGraph::DequeueTask(int32_t threadIndex) {
    return m_scheduler->Dequeue(threadIndex);
}
