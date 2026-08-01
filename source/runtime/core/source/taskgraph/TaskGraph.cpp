#include "taskgraph/TaskGraph.h"
#include "AnyThreadScheduler.h"
#include "platform/Platform.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/ThreadManager.h"
//#define NOMINMAX 1
#undef max

namespace {
thread_local EThread::Type g_task_graph_worker_thread = EThread::UNKNOWN_THREAD;
}

TaskGraph* TaskGraph::instance = nullptr;

TaskGraph& TaskGraph::GetInterface() {
    assert(instance != nullptr);
    return *instance;
}

bool TaskGraph::IsInitialized() noexcept {
    return instance != nullptr;
}

void TaskGraph::Init() {
    if (instance == nullptr) {
        void* storage = Memory::Malloc(sizeof(TaskGraph));
        if (storage == nullptr) {
            throw std::bad_alloc();
        }
        try {
            instance = MoerPlacementNew(storage) TaskGraph();
        } catch (...) {
            Memory::Free(storage);
            throw;
        }
    }
}
void TaskGraph::Shutdown() {
    if (instance != nullptr) {
        // A worker cannot wait for pending==0 while its own executing task is
        // still part of that count. Shutdown is an external-owner operation.
        if (!EThread::IsUnKnownThread(g_task_graph_worker_thread)) {
            Platform::FailFast("TaskGraph shutdown requested from an AnyThread worker");
        }
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

void TaskGraph::WakeUpWorkerThread(int32_t threadIndex, QueueIndex index) noexcept {
    assert(threadIndex >= m_named_thread_count && threadIndex < m_thread_count);
    m_workers[threadIndex].task_thread->Wake(index);
}
void TaskGraph::WakeWorkerCallback(void* context, int32_t threadIndex) noexcept {
    static_cast<TaskGraph*>(context)->WakeUpWorkerThread(threadIndex, EThread::MAIN_QUEUE);
}
void TaskGraph::SetCurrentWorkerThread(EThread::Type thread) noexcept {
    g_task_graph_worker_thread = thread;
}
void TaskGraph::ClearCurrentWorkerThread() noexcept {
    g_task_graph_worker_thread = EThread::UNKNOWN_THREAD;
}
void TaskGraph::NotifyAnyThreadTaskCompleted() noexcept {
    m_any_thread_scheduler->NotifyTaskCompleted();
}
void TaskGraph::WaitUntilTaskComplete(const GraphEventRef& task, EThread::Type currentThread) {
    WaitUntilTasksComplete({task}, currentThread);
}
void TaskGraph::WaitUntilTaskComplete(GraphEventRef&& task, EThread::Type currentThread) {
    WaitUntilTasksComplete({std::move(task)}, currentThread);
};
TaskGraph::TaskGraph() {
    assert(instance == nullptr);
    const int32_t actual_thread_num = Platform::GetProcessorCoreCount();

    m_named_thread_count                   = EThread::NamedThreadCount;
    const int32_t min_worker_thread_per_priority = 1;
    const int32_t min_thread_count =
        min_worker_thread_per_priority * EThread::PriorityCount + m_named_thread_count;

    // EThread reserves the all-ones 8-bit index for UNKNOWN_THREAD, so valid
    // worker/named indices end at 254 even on very large server topologies.
    m_thread_count = std::min(
        std::max(min_thread_count, actual_thread_num),
        static_cast<int32_t>(EThread::UNKNOWN_THREAD)
    );

    m_worker_thread_count = m_thread_count - m_named_thread_count;
    m_worker_topology = Moer::TaskGraphDetail::BuildWorkerPoolTopology(
        m_named_thread_count,
        m_worker_thread_count
    );

    // Allocate the owning scheduler before raw queue objects so a failure here
    // cannot strand TaskThread Event resources in a partially-built TaskGraph.
    m_any_thread_scheduler = std::make_unique<AnyThreadScheduler>(
        m_worker_topology,
        &TaskGraph::WakeWorkerCallback,
        this
    );

    int32_t constructed_task_threads = 0;
    try {
        for (int32_t i = 0; i < m_thread_count; i++) {
            EThread::Type type = EThread::SetThreadIndex(EThread::UNKNOWN_THREAD, i);
            const int32_t priority_index =
                i < m_named_thread_count ? 0 : GetThreadPriorityFromIndex(i);
            assert(priority_index >= 0 && priority_index < EThread::PriorityCount);
            const ThreadPriority priority = priority_index << EThread::PRIORITY_SHEFT;
            type                          = EThread::SetPriority(type, priority);
            m_workers[i].task_thread = i >= m_named_thread_count
                                           ? static_cast<TaskThreadBase*>(new TaskThreadAnyThread(type))
                                           : static_cast<TaskThreadBase*>(new NamedThread);
            m_workers[i].task_thread->SetAttributes(type, &m_workers[i]);
            ++constructed_task_threads;
        }

        instance = this; // Worker entry resolves the graph through the singleton.
        // TODO: no handle for thread groups.
        for (int32_t i = m_named_thread_count; i < m_thread_count; i++) {
            const int32_t priority = GetThreadPriorityFromIndex(i);
            const std::string name = "WorkerThread_" + GetPriorityStr(priority) + "_" +
                                     std::to_string(m_worker_topology.LocalIndex(i, priority));
            m_workers[i].actual_thread = RunnableThread::Create(
                &GetThread(i),
                ThreadAttributes{
                    .affinity = Affinity::AnyOf(i, std::move(Affinity::All())),
                    .name     = name
                }
            );
            m_workers[i].attached = true;
        }
    } catch (...) {
        // Successfully-started workers still require the live singleton while
        // they observe their close signal and leave the scheduler loop.
        for (int32_t i = m_named_thread_count; i < m_thread_count; ++i) {
            if (m_workers[i].actual_thread != nullptr) {
                m_workers[i].task_thread->RequestQuit(QUIT);
            }
        }
        for (int32_t i = m_named_thread_count; i < m_thread_count; ++i) {
            if (m_workers[i].actual_thread != nullptr) {
                m_workers[i].actual_thread->WaitUntilFinished();
                MoerDelete(m_workers[i].actual_thread);
                m_workers[i].actual_thread = nullptr;
            }
        }
        if (instance == this) {
            instance = nullptr;
        }
        for (int32_t i = 0; i < constructed_task_threads; ++i) {
            delete m_workers[i].task_thread;
            m_workers[i].task_thread = nullptr;
        }
        m_any_thread_scheduler.reset();
        throw;
    }
}

TaskGraph::~TaskGraph() {
    // TaskSystem shutdown requires external producers to be quiescent. Keep
    // every priority pool alive until already-published work (including
    // cross-priority continuations) has completed, then close the workers.
    m_any_thread_scheduler->BeginDrain();
    m_any_thread_scheduler->WaitUntilIdle();
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
        // Named owners are externally joined before TaskSystem shutdown (the
        // render service enforces this ordering), but TaskGraph still owns the
        // NamedThread queue objects and their pooled Events.
        delete m_workers[i].task_thread;
        m_workers[i].task_thread = nullptr;
    }
    m_any_thread_scheduler.reset();
    instance = nullptr;
}
EThread::Type TaskGraph::GetCurrentThread(bool localQueue) {
    if (!EThread::IsUnKnownThread(g_task_graph_worker_thread)) {
        return EThread::Type(
            g_task_graph_worker_thread |
            (localQueue ? EThread::LOCAL_QUEUE : EThread::MAIN_QUEUE)
        );
    }

    const uint32_t current_thread_id = Platform::GetCurrentThreadID();
    if (current_thread_id == ThreadManager::g_game_thread_id)
        return EThread::Type(
            localQueue ? (EThread::EMainThread | EThread::LOCAL_QUEUE) : EThread::EMainThread
        );
    if (current_thread_id == ThreadManager::g_render_thread_id)
        return EThread::Type(
            localQueue ? (EThread::ERenderThread | EThread::LOCAL_QUEUE) : EThread::ERenderThread
        );
    // Dedicated runtime owners such as the RHI Executor are intentionally not
    // TaskGraph workers. Treat them as external callers so GraphEvent::Wait()
    // installs an ordinary completion event instead of indexing a named
    // worker queue. This matches the dev_parallel_rhi ownership model.
    return EThread::SetPriority(EThread::UNKNOWN_THREAD, EThread::NORMAL_PRI);
}
bool TaskGraph::IsDraining() const noexcept {
    return m_any_thread_scheduler->IsDraining();
}
bool TaskGraph::IsWaitingForIdle() const noexcept {
    return m_any_thread_scheduler->IsWaitingForIdle();
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
        pending = false;
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
) noexcept {
    if (EThread::GetThreadIndex(_prefered_thread) == EThread::UNKNOWN_THREAD) { //any thread is ok
        // Scheduler ownership comes only from trusted worker TLS, never from
        // ThreadManager's mutable registry or the caller-supplied hint.
        const EThread::Type actual_current_thread = g_task_graph_worker_thread;
        m_any_thread_scheduler->Enqueue(task, actual_current_thread, wake_worker);
        return;
    }
    //named thread
    // Shutdown only owns/drains the AnyThread pools. Keep the drain check and
    // named-queue publication under the same admission gate so BeginDrain
    // cannot linearize between them and strand a continuation after shutdown.
    [[maybe_unused]] auto named_admission =
        m_any_thread_scheduler->AcquireNamedPublication();
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
    return m_any_thread_scheduler->DequeueOrPrepareToPark(threadIndex);
}
