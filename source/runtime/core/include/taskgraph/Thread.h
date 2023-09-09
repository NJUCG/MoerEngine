#ifndef THREAD_H
#define THREAD_H
#include "ThreadManager.h"
#include "Event.h"
#include "misc/StatQueue.h"
#include <iostream>
class BaseGraphTask;

enum QuitCommand:int32_t {
	QUIT=-1,
	RETURN=0
};
class TaskThreadBase;
struct WorkerThread {
	RunnableThread* actualThread;
	TaskThreadBase* taskThread;
	bool attached;
public:
	WorkerThread() :actualThread{ nullptr }, taskThread{ nullptr }, attached{ false }{}
};
class TaskThreadBase: public Runnable {
	friend class TaskGraph;
public:
	TaskThreadBase() :m_worker{ nullptr }, m_threadType{ EThread::UNKNOWN_THREAD }{
		m_graphTasks.reserve(128);
	}

	void setAttributes(EThread::Type _threadIndex, WorkerThread* worker) {
		m_worker = worker;
		m_threadType = _threadIndex;
	}

	//hang the thread when finishing executing while there's no task in target job queue
	virtual void processTaskUntilFinished(QueueIndex queueIndex) {}

	virtual void wake(QueueIndex queueIndex=0) = 0;

	virtual void init() override {

	}
	//thread stay awake even when there's no task to acquire until quit signal's recieved
	virtual void processTaskUntilQuit(QueueIndex queueIndex) {}

	virtual void requestQuit(QueueIndex queueIndex) = 0;

	virtual bool isProcessingTask(QueueIndex queueIndex) = 0;

	virtual void enqueueFromCurrentThread(QueueIndex queueIndex, BaseGraphTask* task) {}

	virtual bool enqueueFromExternThread(QueueIndex queueIndex, BaseGraphTask* task) { return false; }

	virtual void tick() {
		processTaskUntilFinished(0); //for single threaded
	}
	virtual uint32_t run() override {
		processTaskUntilQuit(0);
		return 0;
	}

	virtual void stop() override {
		requestQuit(-1);
	}

	virtual void exit() override {}

	ThreadIndex getIndex() override{
		return EThread::getThreadIndex(m_threadType);
	}

protected:
	WorkerThread* m_worker;
	EThread::Type m_threadType;
	std::vector<BaseGraphTask*> m_graphTasks;
	std::atomic<uint32_t> m_hanged;
};

class TaskThreadAnyThread :public TaskThreadBase {
	struct AnyThreadTaskQueue {
		Event* m_hangEvent;
		bool m_close;
		bool m_hang;
		uint32_t callAmount;
	public:
		AnyThreadTaskQueue() :
			m_hangEvent{EventPool::get()->getEvent()},
			callAmount{ 0 },
			m_close{ false }, m_hang{ false } {

		}
		~AnyThreadTaskQueue() {
			EventPool::get()->releaseEvent(m_hangEvent);
			m_hangEvent = nullptr;
		}
	};
public:
	TaskThreadAnyThread(EThread::Type type) {
		m_threadType = type;
	}
	virtual void processTaskUntilQuit(QueueIndex queueIndex) override {
		do {
			processTasks();

		} while (!m_queue.m_close);
	}
	virtual void processTaskUntilFinished(QueueIndex queueIndex) override {
		processTasks();
	}
	virtual void wake(QueueIndex queueIndex) override {
		m_queue.m_hangEvent->trigger();
	}
	virtual void requestQuit(QueueIndex queueIndex) override {
		assert(m_queue.m_hangEvent != nullptr);
		m_queue.m_close = true;
		m_queue.m_hangEvent->trigger();
	}
	virtual uint32_t run() override {
		return TaskThreadBase::run();//process task until quit
	}
	uint32_t processTasks();
	virtual bool isProcessingTask(QueueIndex queueIndex) override {
		return m_queue.callAmount > 0;
	}
protected:
	AnyThreadTaskQueue m_queue;
	BaseGraphTask* findTaskToDo();
};

class NamedThread :public TaskThreadBase {
	struct NamedTaskQueue {
		TaskFIFOQueue<BaseGraphTask, 2> m_queue;
		Event* m_hangEvent;
		bool m_close;
		bool m_should_return;
		bool m_hang;
		uint32_t callAmount;

		explicit NamedTaskQueue():m_hangEvent{ EventPool::get()->getEvent() },
			callAmount{ 0 },
			m_close{ false }, m_hang{ false }, m_should_return{false}{

		}
		~NamedTaskQueue() {
			EventPool::get()->releaseEvent(m_hangEvent);
			m_hangEvent = nullptr;

		}
	};
public:
	virtual void processTaskUntilQuit(QueueIndex queueIndex) override {
		m_queue[queueIndex].m_should_return = false;
		do {
			processTasks(queueIndex,true);//hang up thread when there's no task to execute

		} while (!m_queue[queueIndex].m_close && !m_queue[queueIndex].m_should_return);
	}
	virtual void processTaskUntilFinished(QueueIndex queueIndex) override {
		m_queue[queueIndex].m_should_return = false;
		processTasks(queueIndex, false);//don't hang up thread and break the loop when there's no task to execute
	}
	virtual void wake(QueueIndex queueIndex) override {
		m_queue[queueIndex].m_hangEvent->trigger();
	}
	virtual void requestQuit(QueueIndex queueIndex) override {
		if(m_queue[queueIndex].m_hangEvent == nullptr)return;
		//main queue means quit
		if (queueIndex == QUIT) {
			m_queue[EThread::MAIN_QUEUE].m_close = true;
			m_queue[EThread::MAIN_QUEUE].m_hangEvent->trigger();
			m_queue[EThread::LOCAL_QUEUE >> EThread::QUEUE_MASK_SHEFT].m_close = true;
			m_queue[EThread::LOCAL_QUEUE >> EThread::QUEUE_MASK_SHEFT].m_hangEvent->trigger();
		}
		else {
			m_queue[queueIndex].m_should_return = true;
		}
	}
	virtual void enqueueFromCurrentThread(QueueIndex queueIndex, BaseGraphTask* task) override{
	
		//todo: get priority from task
		int32_t threadToInvoke = m_queue[queueIndex].m_queue.push(task, queueIndex);
		assert(threadToInvoke < 0);
	}

	virtual bool enqueueFromExternThread(QueueIndex queueIndex, BaseGraphTask* task) override {

		//todo: get priority from task
		int32_t threadToInvoke = m_queue[queueIndex].m_queue.push(task, queueIndex);
		if (threadToInvoke >= 0) {
			m_queue[queueIndex].m_hangEvent->trigger();
			return true;
		}
		
		return false; 
	}
	virtual uint32_t run() override {
		return TaskThreadBase::run();//process task until quit
	}
	uint32_t processTasks(QueueIndex index, bool allowHang);
	virtual bool isProcessingTask(QueueIndex queueIndex) override {
		return m_queue[queueIndex].callAmount > 0;
	}
protected:
	NamedTaskQueue m_queue[2];

};
#endif // !THREAD_H
