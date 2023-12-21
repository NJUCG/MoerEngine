#include "taskgraph/Thread.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/GraphTask.h"
#include "platform/Platform.h"
#include "spdlog/spdlog.h"
BaseGraphTask* TaskThreadAnyThread::FindTaskToDo() {

    return TaskGraph::GetInterface().DequeueTask(EThread::GetThreadIndex(m_thread_type));
}

uint32_t TaskThreadAnyThread::ProcessTasks() {
    assert(++m_queue.call_amount == 1);
    while (true) {
        BaseGraphTask* task = FindTaskToDo();
        if (task == nullptr) {
            //SPDLOG_INFO("{} hanged", platform::GetCurrentThreadID());
            m_queue.m_hang_event->Wait();
            if (m_queue.m_close) break;
            continue;
        }
        task->Execute(m_graph_tasks, m_thread_type);
    }
    assert(--m_queue.call_amount == 0);
    return 0;
}

uint32_t NamedThread::ProcessTasks(QueueIndex queueIndex, bool allowHang) {
    assert(++m_queue[queueIndex].call_amount == 1);
    // bool isRenderQueue = (m_threadType & EThread::INDEX_MASK) == EThread::ERenderThread;
    while (!m_queue[queueIndex].m_should_return) {
        BaseGraphTask* task = m_queue[queueIndex].m_queue.Pop(0, allowHang);//set avaliable thread bit
        if (!task) {
            if (allowHang) {
                m_queue[queueIndex].m_hang_event->Wait();//hang up thread when there's no task to execute
                if (m_queue[queueIndex].m_close) return 0;
                continue;
            } else {
                break;
            }
        } else {
            task->Execute(m_graph_tasks, m_thread_type);
        }
    }
    assert(--m_queue[queueIndex].call_amount == 0);
    return 0;
}