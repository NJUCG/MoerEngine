#ifndef TASK_GRAPH_H
#define TASK_GRAPH_H
#include <memory>
#include "Thread.h"
#include "misc/StatQueue.h"
#include "misc/CountableRef.h"
class GraphEvent;
typedef CountableRef<GraphEvent>  GraphEventRef;
typedef std::vector<GraphEventRef> GraphEventArray;
enum class ESchedule {
	Max_Local_Capacity=16
};

class TaskGraph {
private:
	static TaskGraph* instance;
public:
	static TaskGraph& getInterface();
	static void init();
	TaskGraph();
	~TaskGraph();
	void waitUntilTasksComplete(const GraphEventArray& task_events, EThread::Type currentThread);
	void waitUntilTaskComplete(const GraphEventRef& task, EThread::Type currentThread);
	void waitUntilTaskComplete(GraphEventRef&& task, EThread::Type currentThread);
	void attachToNameThread(EThread::Type type);
	virtual void queueTask(BaseGraphTask* task, EThread::Type prefered_thread, EThread::Type current_thread=EThread::UNKNOWN_THREAD, bool wake_worker=true);
	virtual void returnThread(EThread::Type index);
	virtual BaseGraphTask* dequeueTask(int32_t threadIndex);
	virtual void processThreadUntilIdle(EThread::Type index);
	virtual void processThreadUntilReturn(EThread::Type index);
protected:
	EThread::Type getCurrentThread(bool localQueue=false);
	
	void triggerEventWhenTasksComplete(Event* event, const GraphEventArray& task_events, EThread::Type currentThread = EThread::UNKNOWN_THREAD, EThread::Type triggerThread = EThread::UNKNOWN_THREAD);
private:
	bool isThreadProcessingTask(EThread::Type index);
	TaskThreadBase& getThread(ThreadIndex index);
	int32_t getThreadPriorityFromIndex(int32_t threadIndex) {
		return (threadIndex - m_named_thread_count) / m_worker_per_priority;
	}
	void wakeUpWorkerThread(int32_t threadIndex, QueueIndex index);
	WorkerThread m_workers[INT16_MAX];
	int32_t m_thread_count;
	int32_t m_named_thread_count;
	int32_t m_worker_per_priority;
	int32_t m_worker_thread_count;

	TaskFIFOQueue<BaseGraphTask, 2> m_task_queue[EThread::PriorityCount];
	TaskFIFOQueue<BaseGraphTask, 1> m_global_queue; //
};
#endif // !TASK_GRAPH_H
