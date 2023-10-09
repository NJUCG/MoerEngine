#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/Event.h"
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

bool GraphEvent::AddSubsequent(BaseGraphTask* subsequent) {
    return m_subsequents.TryPush(subsequent);
}
bool GraphEvent::IsComplete() {
    return m_subsequents.IsClosed();
}
void BaseGraphTask::QueueTask(EThread::Type currentThread, bool shouldWakeupWorker) {
    TaskGraph::GetInterface().QueueTask(this, m_preferdThread, currentThread, shouldWakeupWorker);
}

void BaseGraphTask::PrerequestsComplete(EThread::Type currentThread, int32_t finishedCount, bool unlock) {
    int32_t finished = finishedCount + (unlock ? 1 : 0);
    if (m_prerequests_count.fetch_sub(finished) == finished) {
        QueueTask(currentThread, true);
    }
}

void GraphEvent::TryUnlockSubsequents(std::vector<BaseGraphTask*>& tasks, EThread::Type currentThread) {
    if (tasks.size() > 0) {
        GraphEventArray tempEvents;
        std::swap(m_events_to_wait, tempEvents);// m_events removed

        //test events to wait has complete
        bool generateEmptyTask = false;
        for (int32_t i = 0; i < tempEvents.size(); i++) {
            GraphEvent* _event = tempEvents[i].Get();
            if (!_event->IsComplete()) {
                generateEmptyTask = true;
                break;
            }
        }

        if (generateEmptyTask) {
            EThread::Type collectionThread = EThread::SetPriority(EThread::UNKNOWN_THREAD, EThread::HIGH_PRI);
            GraphTask<EmptyGraphTask>::CreateTask(GraphEventRef(this), &tempEvents, currentThread).ConstructAndDispatchWhenReady(collectionThread);
        }
        return;
    }
    std::vector<BaseGraphTask*> poped;
    m_subsequents.PopAll(poped);
    bool should_wake_up_worker = false;// todo: useless in this version
    for (BaseGraphTask* task : poped) {
        assert(task != nullptr);
        should_wake_up_worker = task->ConditionalQueueTask(currentThread, should_wake_up_worker);
    }
}
void GraphEvent::tryUnlockSubsequents(EThread::Type currentThread) {
    std::vector<BaseGraphTask*> tasks;
    TryUnlockSubsequents(tasks, currentThread);
}

void GraphEvent::Wait(EThread::Type currentThread) {

    TaskGraph::GetInterface().WaitUntilTaskComplete(this, currentThread);
}

void TriggerEventGraphTask::Fire(EThread::Type _type, const GraphEventRef& _event) {
    m_event->Trigger();
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
