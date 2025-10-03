#include "taskgraph/GraphTask.h"
#include "misc/STL.h"
#include "taskgraph/Event.h"
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
    auto* instance = new GraphEvent();
    return instance;
};

bool GraphEvent::AddSubsequent(BaseGraphTask* subsequent) {
    // return m_subsequents.TryPush(subsequent);
    assert(subsequent != nullptr);
    return m_subsequents.TryPush(subsequent);
}
bool GraphEvent::IsComplete() const {
    return m_subsequents.IsClosed();
}
void BaseGraphTask::QueueTask(EThread::Type currentThread, bool shouldWakeupWorker) {
    TaskGraph::GetInterface().QueueTask(this, m_preferd_thread, currentThread, shouldWakeupWorker);
}

void BaseGraphTask::PrerequestsComplete(EThread::Type currentThread, int32_t finishedCount, bool unlock) {
    int32_t finished = finishedCount + (unlock ? 1 : 0);
    if (m_prerequests_count.fetch_sub(finished) == finished) {
        QueueTask(currentThread, true);
    }
}

void GraphEvent::TryUnlockSubsequents(EThread::Type currentThread) {
    if (m_events_to_wait.size() > 0) {
        GraphEventArray temp_events;
        std::swap(m_events_to_wait, temp_events); // m_events removed

        //test events to wait has complete
        bool generate_empty_task = false;
        for (int32_t i = 0; i < temp_events.size(); i++) {
            GraphEvent* event = temp_events[i].Get();
            if (!event->IsComplete()) {
                generate_empty_task = true;
                break;
            }
        }

        if (generate_empty_task) {
            EThread::Type collection_thread =
                EThread::SetPriority(EThread::UNKNOWN_THREAD, EThread::HIGH_PRI);
            // GraphTask<EmptyGraphTask>::CreateTask(GraphEventRef(this), &temp_events, currentThread).ConstructAndDispatchWhenReady(collection_thread);
            GraphTask<EmptyGraphTask>::Create(collection_thread)
                .Wait(std::move(temp_events))
                .Next(GraphEventRef(this))
                .Dispatch();

            return;
        }
    }
    Moer::Array<BaseGraphTask*> poped;
    m_subsequents.ComsumeAllAndClose(poped);
    std::reverse(poped.begin(), poped.end());
    bool should_wake_up_worker = false; // todo: useless in this version
    for (BaseGraphTask* task : poped) {
        assert(task != nullptr);
        should_wake_up_worker = task->ConditionalQueueTask(currentThread, should_wake_up_worker);
    }
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
