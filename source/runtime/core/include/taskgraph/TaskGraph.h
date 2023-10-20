#ifndef TASK_GRAPH_H
#define TASK_GRAPH_H
#include <memory>
#include "API_Macro.h"
#include "Thread.h"
#include "misc/AsyncQueue.h"
#include "misc/CountableRef.h"
class GraphEvent;
typedef CountableRef<GraphEvent>   GraphEventRef;
typedef std::vector<GraphEventRef> GraphEventArray;
enum class ESchedule {
    Max_Local_Capacity = 16
};

class TaskGraph {
private:
    static TaskGraph* instance;

public:
    CORE_API static TaskGraph& GetInterface();
    static void                Init();
    TaskGraph();
    ~TaskGraph();
    CORE_API void          WaitUntilTasksComplete(const GraphEventArray& task_events, EThread::Type currentThread);
    CORE_API void          WaitUntilTaskComplete(const GraphEventRef& task, EThread::Type currentThread);
    CORE_API void          WaitUntilTaskComplete(GraphEventRef&& task, EThread::Type currentThread);
    CORE_API void          AttachToNameThread(EThread::Type type);
    virtual void           QueueTask(BaseGraphTask* task, EThread::Type prefered_thread, EThread::Type current_thread = EThread::UNKNOWN_THREAD, bool wake_worker = true);
    virtual void           ReturnThread(EThread::Type index);
    virtual BaseGraphTask* DequeueTask(int32_t threadIndex);
    CORE_API virtual void  ProcessThreadUntilIdle(EThread::Type index);
    CORE_API virtual void  ProcessThreadUntilReturn(EThread::Type index);
    CORE_API bool          IsThreadProcessingTask(EThread::Type index);

protected:
    EThread::Type GetCurrentThread(bool localQueue = false);

    void TriggerEventWhenTasksComplete(Event* event, const GraphEventArray& task_events, EThread::Type currentThread = EThread::UNKNOWN_THREAD, EThread::Type triggerThread = EThread::UNKNOWN_THREAD);

private:
    TaskThreadBase& GetThread(ThreadIndex index);
    int32_t         GetThreadPriorityFromIndex(int32_t threadIndex) {
        return (threadIndex - m_named_thread_count) / m_worker_per_priority;
    }
    void         WakeUpWorkerThread(int32_t threadIndex, QueueIndex index);
    WorkerThread m_workers[INT16_MAX];
    int32_t      m_thread_count;
    int32_t      m_named_thread_count;
    int32_t      m_worker_per_priority;
    int32_t      m_worker_thread_count;

    TaskFIFOQueue<BaseGraphTask, 2> m_task_queue[EThread::PriorityCount];
    TaskFIFOQueue<BaseGraphTask, 1> m_global_queue;//
};
#endif// !TASK_GRAPH_H
