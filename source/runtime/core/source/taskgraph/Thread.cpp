#include "taskgraph/Thread.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/GraphTask.h"
#include "platform/Platform.h"
#include "spdlog/spdlog.h"
BaseGraphTask* TaskThreadAnyThread::findTaskToDo() {


	return TaskGraph::getInterface().dequeueTask(EThread::getThreadIndex(m_threadType));
}

uint32_t TaskThreadAnyThread::processTasks() {
	assert(++m_queue.callAmount == 1);
	while (true) {
		BaseGraphTask* task = findTaskToDo();
		if (task == nullptr) {
			//SPDLOG_INFO("{} hanged", platform::GetCurrentThreadID());
			m_queue.m_hangEvent->wait();
			if (m_queue.m_close)break;
			continue;
		}
		task->execute(m_graphTasks, m_threadType);

	}
	assert(--m_queue.callAmount == 0);
	return 0;
}

uint32_t NamedThread::processTasks(QueueIndex queueIndex, bool allowHang) {
	assert(++m_queue[queueIndex].callAmount == 1);
	bool isRenderQueue = (m_threadType & EThread::INDEX_MASK) == EThread::ERenderThread;
	while (!m_queue[queueIndex].m_should_return) {
		BaseGraphTask* task = m_queue[queueIndex].m_queue.pop(0, !isRenderQueue && allowHang);//set avaliable thread bit
		if (!task) {
			if (allowHang) {
				m_queue[queueIndex].m_hangEvent->wait(); //hang up thread when there's no task to execute
				if (m_queue[queueIndex].m_close)return 0;
				continue;
			}
			else {
				break;
			}
		}
		else {
			task->execute(m_graphTasks, m_threadType);
		}

	}
	assert(--m_queue[queueIndex].callAmount == 0);
	return 0;
}