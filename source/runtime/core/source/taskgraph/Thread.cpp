#include "taskgraph/Thread.h"
#include "misc/Types.h"
#include "platform/Platform.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"

Moer::uint32 TaskThreadAnyThread::Run() {
    TaskGraph::SetCurrentWorkerThread(m_thread_type);
    try {
        const Moer::uint32 result = TaskThreadBase::Run();
        TaskGraph::ClearCurrentWorkerThread();
        return result;
    } catch (...) {
        TaskGraph::ClearCurrentWorkerThread();
        throw;
    }
}

BaseGraphTask* TaskThreadAnyThread::FindTaskToDo() {

    return TaskGraph::GetInterface().DequeueTask(EThread::GetThreadIndex(m_thread_type));
}

Moer::uint32 TaskThreadAnyThread::ProcessTasks() {
    assert(++m_queue.call_amount == 1);
    while (true) {
        BaseGraphTask* task = FindTaskToDo();
        if (task == nullptr) {
            //SPDLOG_INFO("{} hanged", platform::GetCurrentThreadID());
            m_queue.m_hang_event->Wait();
            if (m_queue.m_close)
                break;
            continue;
        }
        task->Execute(m_thread_type);
        TaskGraph::GetInterface().NotifyAnyThreadTaskCompleted();
    }
    assert(--m_queue.call_amount == 0);
    return 0;
}

Moer::uint32 NamedThread::ProcessTasks(QueueIndex queueIndex, bool allowHang) {
    assert(++m_queue[queueIndex].call_amount == 1);
    // bool isRenderQueue = (m_threadType & EThread::INDEX_MASK) == EThread::ERenderThread;
    while (!m_queue[queueIndex].m_should_return) {
        BaseGraphTask* task = m_queue[queueIndex].m_queue.Pop(0, allowHang); //set avaliable thread bit
        if (!task) {
            if (allowHang) {
                m_queue[queueIndex].m_hang_event->Wait(); //hang up thread when there's no task to execute
                if (m_queue[queueIndex].m_close)
                    return 0;
                continue;
            } else {
                break;
            }
        } else {
            task->Execute(m_thread_type);
        }
    }
    assert(--m_queue[queueIndex].call_amount == 0);
    return 0;
}
