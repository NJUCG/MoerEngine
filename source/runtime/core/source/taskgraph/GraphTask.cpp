#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
//class GraphEventPool {
//public:
//	GraphEvent* getEvent();
//	static GraphEventPool& get();
//	void releaseEvent(GraphEvent* target);
//private:
//	GraphEventPool();
//	LockQueue<GraphEvent*> m_pool;
//};

GraphEventRef GraphEvent::CreateGraphEvent() {
    auto instance = new GraphEvent();
    return instance;
};
void BaseGraphTask::queueTask(EThread::Type currentThread, bool shouldWakeupWorker) {
    TaskGraph::getInterface().queueTask(this, m_preferdThread, currentThread, shouldWakeupWorker);
}

void BaseGraphTask::prerequestsComplete(EThread::Type currentThread, int32_t finishedCount, bool unlock) {
    int32_t finished = finishedCount + (unlock ? 1 : 0);
    if (m_prerequests_count.fetch_sub(finished) == finished) {
        queueTask(currentThread, true);
    }
}

void GraphEvent::tryUnlockSubsequents(std::vector<BaseGraphTask*>& tasks, EThread::Type currentThread) {
    if (tasks.size() > 0) {
        GraphEventArray tempEvents;
        std::swap(m_events_to_wait, tempEvents);// m_events removed

        //test events to wait has complete
        bool generateEmptyTask = false;
        for (int32_t i = 0; i < tempEvents.size(); i++) {
            GraphEvent* _event = tempEvents[i].get();
            if (!_event->isComplete()) {
                generateEmptyTask = true;
                break;
            }
        }

        if (generateEmptyTask) {
            EThread::Type collectionThread = EThread::setPriority(EThread::UNKNOWN_THREAD, EThread::HIGH_PRI);
            GraphTask<EmptyGraphTask>::CreateTask(GraphEventRef(this), &tempEvents, currentThread).ConstructAndDispatchWhenReady(collectionThread);
        }
        return;
    }
    std::vector<BaseGraphTask*> poped;
    m_subsequents.popAll(poped);
    bool should_wake_up_worker = false;// todo: useless in this version
    for (BaseGraphTask* task : poped) {
        assert(task != nullptr);
        should_wake_up_worker = task->conditionalQueueTask(currentThread, should_wake_up_worker);
    }
}
void GraphEvent::tryUnlockSubsequents(EThread::Type currentThread) {
    std::vector<BaseGraphTask*> tasks;
    tryUnlockSubsequents(tasks, currentThread);
}

void GraphEvent::wait(EThread::Type currentThread) {

    TaskGraph::getInterface().waitUntilTaskComplete(this, currentThread);
}

//GraphEvent* GraphEventPool::getEvent()
//{
//	GraphEvent* target;
//	while (!m_pool.pop(target)) {
//		for (size_t i = 0; i < 1000; i++)
//		{
//			m_pool.push(new GraphEvent());
//		}
//	}
//	return target;
//}
//
//GraphEventPool& GraphEventPool::get()
//{
//	static GraphEventPool pool;
//	return pool;
//}
//
//void GraphEventPool::releaseEvent(GraphEvent* target)
//{
//
//	m_pool.push(target);
//}
