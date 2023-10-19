#ifndef THREAD_H
#define THREAD_H
#include "ThreadManager.h"
#include "Event.h"
#include "misc/StatQueue.h"
#include <iostream>
class BaseGraphTask;

enum QuitCommand : int32_t {
    QUIT   = -1,
    RETURN = 0
};
class TaskThreadBase;
struct WorkerThread {
    RunnableThread* actualThread;
    TaskThreadBase* taskThread;
    bool            attached;

public:
    WorkerThread() : actualThread{nullptr}, taskThread{nullptr}, attached{false} {}
};
class TaskThreadBase : public Runnable {
    friend class TaskGraph;

public:
    TaskThreadBase() : m_worker{nullptr}, m_threadType{EThread::UNKNOWN_THREAD} {
        m_graphTasks.reserve(128);
    }

    void SetAttributes(EThread::Type _threadIndex, WorkerThread* worker) {
        m_worker     = worker;
        m_threadType = _threadIndex;
    }

    //hang the thread when finishing executing while there's no task in target job queue
    virtual void ProcessTaskUntilFinished(QueueIndex queueIndex) {}

    virtual void Wake(QueueIndex queueIndex = 0) = 0;

    virtual void Init() override {
    }
    //thread stay awake even when there's no task to acquire until quit signal's recieved
    virtual void ProcessTaskUntilQuit(QueueIndex queueIndex) {}

    virtual void RequestQuit(QueueIndex queueIndex) = 0;

    virtual bool IsProcessingTask(QueueIndex queueIndex) = 0;

    virtual void EnqueueFromCurrentThread(QueueIndex queueIndex, BaseGraphTask* task) {}

    virtual bool EnqueueFromExternThread(QueueIndex queueIndex, BaseGraphTask* task) { return false; }

    virtual void Tick() {
        ProcessTaskUntilFinished(0);//for single threaded
    }
    virtual uint32_t Run() override {
        ProcessTaskUntilQuit(0);
        return 0;
    }

    virtual void Stop() override {
        RequestQuit(-1);
    }

    virtual void Exit() override {}

    ThreadIndex GetIndex() override {
        return EThread::GetThreadIndex(m_threadType);
    }

protected:
    WorkerThread*               m_worker;
    EThread::Type               m_threadType;
    std::vector<BaseGraphTask*> m_graphTasks;
    std::atomic<uint32_t>       m_hanged;
};

class TaskThreadAnyThread : public TaskThreadBase {
    struct AnyThreadTaskQueue {
        Event*   m_hangEvent;
        bool     m_close;
        bool     m_hang;
        uint32_t callAmount;

    public:
        AnyThreadTaskQueue() : m_hangEvent{EventPool::Get()->GetEvent()},
                               callAmount{0},
                               m_close{false}, m_hang{false} {
        }
        ~AnyThreadTaskQueue() {
            EventPool::Get()->ReleaseEvent(m_hangEvent);
            m_hangEvent = nullptr;
        }
    };

public:
    TaskThreadAnyThread(EThread::Type type) {
        m_threadType = type;
    }
    virtual void ProcessTaskUntilQuit(QueueIndex queueIndex) override {
        do {
            ProcessTasks();

        } while (!m_queue.m_close);
    }
    virtual void ProcessTaskUntilFinished(QueueIndex queueIndex) override {
        ProcessTasks();
    }
    virtual void Wake(QueueIndex queueIndex) override {
        m_queue.m_hangEvent->Trigger();
    }
    virtual void RequestQuit(QueueIndex queueIndex) override {
        assert(m_queue.m_hangEvent != nullptr);
        m_queue.m_close = true;
        m_queue.m_hangEvent->Trigger();
    }
    virtual uint32_t Run() override {
        return TaskThreadBase::Run();//process task until quit
    }
    uint32_t     ProcessTasks();
    virtual bool IsProcessingTask(QueueIndex queueIndex) override {
        return m_queue.callAmount > 0;
    }

protected:
    AnyThreadTaskQueue m_queue;
    BaseGraphTask*     FindTaskToDo();
};

class NamedThread : public TaskThreadBase {
    struct NamedTaskQueue {
        TaskFIFOQueue<BaseGraphTask, 2> m_queue;
        Event*                          m_hangEvent;
        bool                            m_close;
        bool                            m_should_return;
        bool                            m_hang;
        uint32_t                        callAmount;

        explicit NamedTaskQueue() : m_hangEvent{EventPool::Get()->GetEvent()},
                                    callAmount{0},
                                    m_close{false}, m_hang{false}, m_should_return{false} {
        }
        ~NamedTaskQueue() {
            EventPool::Get()->ReleaseEvent(m_hangEvent);
            m_hangEvent = nullptr;
        }
    };

public:
    virtual void ProcessTaskUntilQuit(QueueIndex queueIndex) override {
        m_queue[queueIndex].m_should_return = false;
        do {
            ProcessTasks(queueIndex, true);//hang up thread when there's no task to execute

        } while (!m_queue[queueIndex].m_close && !m_queue[queueIndex].m_should_return);
    }
    virtual void ProcessTaskUntilFinished(QueueIndex queueIndex) override {
        m_queue[queueIndex].m_should_return = false;
        ProcessTasks(queueIndex, false);//don't hang up thread and break the loop when there's no task to execute
    }
    virtual void Wake(QueueIndex queueIndex) override {
        m_queue[queueIndex].m_hangEvent->Trigger();
    }
    virtual void RequestQuit(QueueIndex queueIndex) override {
        if (m_queue[queueIndex].m_hangEvent == nullptr) return;
        //main queue means quit
        if (queueIndex == QUIT) {
            m_queue[EThread::MAIN_QUEUE].m_close = true;
            m_queue[EThread::MAIN_QUEUE].m_hangEvent->Trigger();
            m_queue[EThread::LOCAL_QUEUE >> EThread::QUEUE_MASK_SHEFT].m_close = true;
            m_queue[EThread::LOCAL_QUEUE >> EThread::QUEUE_MASK_SHEFT].m_hangEvent->Trigger();
        } else {
            m_queue[queueIndex].m_should_return = true;
        }
    }
    virtual void EnqueueFromCurrentThread(QueueIndex queueIndex, BaseGraphTask* task) override {

        //todo: get priority from task
        int32_t thread_to_invoke = m_queue[queueIndex].m_queue.Push(task, queueIndex);
        assert(thread_to_invoke < 0);
    }

    virtual bool EnqueueFromExternThread(QueueIndex queueIndex, BaseGraphTask* task) override {

        //todo: get priority from task
        int32_t thread_to_invoke = m_queue[queueIndex].m_queue.Push(task, queueIndex);
        if (thread_to_invoke >= 0) {
            m_queue[queueIndex].m_hangEvent->Trigger();
            return true;
        }

        return false;
    }
    virtual uint32_t Run() override {
        return TaskThreadBase::Run();//process task until quit
    }
    uint32_t     ProcessTasks(QueueIndex index, bool allowHang);
    virtual bool IsProcessingTask(QueueIndex queueIndex) override {
        return m_queue[queueIndex].callAmount > 0;
    }

protected:
    NamedTaskQueue m_queue[2];
};
#endif// !THREAD_H
