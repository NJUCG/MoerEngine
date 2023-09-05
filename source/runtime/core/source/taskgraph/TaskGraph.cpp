#include "taskgraph/TaskGraph.h"
#include "taskgraph/StatQueue.h"
#include "taskgraph/ThreadManager.h"
#include "platform/Platform.h"
#include "taskgraph/GraphTask.h"
//#define NOMINMAX 1
#undef max
TaskGraph* TaskGraph::instance = nullptr;

TaskGraph& TaskGraph::getInterface() {
    assert(instance != nullptr);
    return *instance;
}

void TaskGraph::init() {
    if (instance == nullptr) {
        instance = new TaskGraph();
    }
}
void TaskGraph::attachToNameThread(EThread::Type type) {
    m_workers[EThread::getThreadIndex(type)].attached = true;
}
TaskThreadBase& TaskGraph::getThread(ThreadIndex index) {
    assert(m_workers[index].taskThread->getIndex() == index);
    return *m_workers[index].taskThread;
}

void TaskGraph::wakeUpWorkerThread(int32_t threadIndex, QueueIndex index) {
    assert(threadIndex >= 0);
    m_workers[threadIndex].taskThread->wake(index);
}
void TaskGraph::waitUntilTaskComplete(const GraphEventRef& task, EThread::Type currentThread) { waitUntilTasksComplete({task}, currentThread); }
void TaskGraph::waitUntilTaskComplete(GraphEventRef&& task, EThread::Type currentThread) { waitUntilTasksComplete({std::move(task)}, currentThread); };
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
        EThread::Type  type     = EThread::setThreadIndex(EThread::UNKNOWN_THREAD, i);
        ThreadPriority priority = getThreadPriorityFromIndex(i) << EThread::PRIORITY_SHEFT;
        type                    = EThread::setPriority(type, priority);
        if (i >= m_named_thread_count) {
            //any_thread
            m_workers[i].taskThread = new TaskThreadAnyThread(type);
        } else {
            m_workers[i].taskThread = new NamedThread;
            //named_thread
        }
        m_workers[i].taskThread->setAttributes(type, &m_workers[i]);
    }
    instance = this;//set here to make sure access of worker_threads below
    //todo no handle for thread groups
    for (int32_t i = m_named_thread_count; i < m_thread_count; i++) {
        int32_t     priority      = getThreadPriorityFromIndex(i);
        std::string name          = "WorkerThread_" + getPriorityStr(priority) + "_" + std::to_string((i - m_named_thread_count) % m_worker_per_priority);
        m_workers[i].actualThread = RunnableThread::create(&getThread(i), name, 1ull << i);
        m_workers[i].attached     = true;
    }
}

TaskGraph::~TaskGraph() {
    for (int32_t i = 0; i < m_thread_count; i++) {

        m_workers[i].taskThread->requestQuit(QUIT);
    }
    for (int32_t i = 0; i < m_thread_count; i++) {
        if (i >= m_named_thread_count) {
            m_workers[i].actualThread->waitUntilFinished();

            delete m_workers[i].actualThread;
            m_workers[i].actualThread = nullptr;
        }
        m_workers[i].attached = false;
    }
    instance = nullptr;
}
EThread::Type TaskGraph::getCurrentThread(bool localQueue) {
    if (Platform::GetCurrentThreadID() == ThreadManager::g_gameThreadID) return EThread::Type(localQueue ? (EThread::EGameThread | EThread::LOCAL_QUEUE) : EThread::EGameThread);
    auto thread = ThreadManager::Instance().getRunnableThread(Platform::GetCurrentThreadID());
    if (thread) {
        ThreadIndex index = thread->m_runnable->getIndex();
        return EThread::Type(m_workers[index].taskThread->m_threadType | (localQueue ? EThread::LOCAL_QUEUE : EThread::MAIN_QUEUE));
    }
    assert(false);
    return EThread::UNKNOWN_THREAD;
}
bool TaskGraph::isThreadProcessingTask(EThread::Type index) {
    return m_workers[EThread::getThreadIndex(index)].taskThread->isProcessingTask(EThread::getQueueIndex(index));
}
void TaskGraph::processThreadUntilIdle(EThread::Type index){};
void TaskGraph::processThreadUntilReturn(EThread::Type index) {
    m_workers[EThread::getThreadIndex(index)].taskThread->processTaskUntilQuit(EThread::getQueueIndex(index));
};
void TaskGraph::returnThread(EThread::Type index) {
    QueueIndex queue_index = EThread::getQueueIndex(index);
    m_workers[EThread::getThreadIndex(index)].taskThread->requestQuit(queue_index);
};
void TaskGraph::waitUntilTasksComplete(const GraphEventArray& task_events, EThread::Type currentThread) {
    EThread::Type current_thread_type = currentThread;

    bool is_named_thread = false;
    if (EThread::getThreadIndex(currentThread) == EThread::UNKNOWN_THREAD) {
        ThreadPriority priority = EThread::getThreadPriority(currentThread);
        currentThread           = getCurrentThread();
        current_thread_type     = EThread::setPriority(currentThread, priority << EThread::PRIORITY_SHEFT);

    } else {
        currentThread = (EThread::Type)EThread::getThreadIndex(currentThread);
    }
    is_named_thread = EThread::getThreadIndex(current_thread_type) < EThread::NamedThreadCount;
    if (is_named_thread && !isThreadProcessingTask(current_thread_type)) {
        bool pending = false;
        for (int32_t Index = 0; Index < task_events.size(); Index++) {
            GraphEvent* task = task_events[Index].get();
            if (task != nullptr && !task->isComplete()) {
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
        processThreadUntilReturn(current_thread_type);
    } else {
        bool pending = false;
        for (int32_t Index = 0; Index < task_events.size(); Index++) {
            GraphEvent* task = reinterpret_cast<GraphEvent*>(task_events[Index].get());
            if (task != nullptr && !task->isComplete()) {
                pending = true;
                break;
            }
        }
        if (!pending) {
            // no task left
            return;
        }
        ScopeEventRef scopeEvent;
        triggerEventWhenTasksComplete(scopeEvent.m_event, task_events, currentThread);
    }
}

void TaskGraph::triggerEventWhenTasksComplete(Event* event, const GraphEventArray& task_events, EThread::Type currentThread, EThread::Type triggerThread) {
    assert(event != nullptr);
    bool pending = true;
    if (task_events.size() < 8)// don't bother to check for completion if there are lots of prereqs...too expensive to check
    {
        bool pending = false;
        for (int32_t Index = 0; Index < task_events.size(); Index++) {
            GraphEvent* task = reinterpret_cast<GraphEvent*>(task_events[Index].get());
            if (task != nullptr && !task->isComplete()) {
                pending = true;
                break;
            }
        }
    }
    if (!pending) {
        event->trigger();
        return;
    }
    GraphTask<TriggerEventGraphTask>::CreateTask(&task_events, currentThread).ConstructAndDispatchWhenReady(event, triggerThread);
}
void TaskGraph::queueTask(BaseGraphTask* task, EThread::Type prefered_thread, EThread::Type current_thread, bool wake_worker) {
    if (EThread::getThreadIndex(prefered_thread) == EThread::UNKNOWN_THREAD) {//any thread is ok
        EThread::Type  prefered_thread         = task->getPreferredThread();
        ThreadPriority priority                = task->getPriority();
        int32_t        possible_thread_to_wake = m_task_queue[priority].push(task, 1);
        if (possible_thread_to_wake >= 0) {
            //start task thread
            possible_thread_to_wake = possible_thread_to_wake + priority * m_worker_per_priority + m_named_thread_count;
            wakeUpWorkerThread(possible_thread_to_wake, 0);
        }
        return;
    }
    //named thread
    EThread::Type currentThread;
    if (EThread::getThreadIndex(current_thread) == EThread::UNKNOWN_THREAD) {
        currentThread = (EThread::Type)EThread::getThreadIndex(getCurrentThread());

    } else {
        currentThread = (EThread::Type)EThread::getThreadIndex(current_thread);
    }
    ThreadIndex index       = EThread::getThreadIndex(prefered_thread);
    QueueIndex  queue_index = EThread::getQueueIndex(prefered_thread);
    if (currentThread == index) {
        m_workers[index].taskThread->enqueueFromCurrentThread(queue_index, task);
    } else {
        m_workers[index].taskThread->enqueueFromExternThread(queue_index, task);
    }
}
BaseGraphTask* TaskGraph::dequeueTask(int32_t threadIndex) {
    int32_t        priority          = getThreadPriorityFromIndex(threadIndex);
    int32_t        index_in_priority = (threadIndex - m_named_thread_count) % m_worker_per_priority;
    BaseGraphTask* task              = m_task_queue[priority].pop(index_in_priority);
    //if (task != nullptr) {
    //	task = m_global_queue.pop(index_in_priority);
    //}
    return task;
}