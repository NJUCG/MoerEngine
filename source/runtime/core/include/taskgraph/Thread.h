#ifndef THREAD_H
#define THREAD_H
#include "Event.h"
#include "GraphTask.h"
#include "ThreadManager.h"
#include "misc/LockFree.h"
#include <iostream>

enum QuitCommand : int32_t {
    QUIT   = -1,
    RETURN = 0
};
class TaskThreadBase;
struct WorkerThread {
    RunnableThread* actual_thread;
    TaskThreadBase* task_thread;
    bool            attached;

public:
    WorkerThread() : actual_thread{nullptr}, task_thread{nullptr}, attached{false} {}
};
class TaskThreadBase : public Runnable {
    friend class TaskGraph;

public:
    TaskThreadBase() : m_worker{nullptr}, m_thread_type{EThread::UNKNOWN_THREAD} {
        // m_graph_tasks.reserve(128);
    }

    void SetAttributes(EThread::Type _threadIndex, WorkerThread* worker) {
        m_worker      = worker;
        m_thread_type = _threadIndex;
    }

    //hang the thread when finishing executing while there's no task in target job queue
    virtual void ProcessTaskUntilFinished(QueueIndex queueIndex) {}

    virtual void Wake(QueueIndex queueIndex = 0) = 0;

    virtual void Init() override {}
    //thread stay awake even when there's no task to acquire until quit signal's recieved
    virtual void ProcessTaskUntilQuit(QueueIndex queueIndex) {}

    virtual void RequestQuit(QueueIndex queueIndex) = 0;

    virtual bool IsProcessingTask(QueueIndex queueIndex) = 0;

    virtual void EnqueueFromCurrentThread(QueueIndex queueIndex, BaseGraphTask* task) {}

    virtual bool EnqueueFromExternThread(QueueIndex queueIndex, BaseGraphTask* task) {
        return false;
    }

    virtual void Tick() {
        ProcessTaskUntilFinished(0); //for single threaded
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
        return EThread::GetThreadIndex(m_thread_type);
    }

protected:
    WorkerThread* m_worker;
    EThread::Type m_thread_type;
    // Moer::Array<BaseGraphTask*> m_graph_tasks;
    std::atomic<uint32_t> m_hanged;
};

class TaskThreadAnyThread : public TaskThreadBase {
    struct AnyThreadTaskQueue {
        Event*   m_hang_event;
        bool     m_close;
        bool     m_hang;
        uint32_t call_amount;

    public:
        AnyThreadTaskQueue() :
            m_hang_event{EventPool::Get()->GetEvent()},
            call_amount{0},
            m_close{false},
            m_hang{false} {}
        ~AnyThreadTaskQueue() {
            EventPool::Get()->ReleaseEvent(m_hang_event);
            m_hang_event = nullptr;
        }
    };

public:
    TaskThreadAnyThread(EThread::Type type) {
        m_thread_type = type;
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
        m_queue.m_hang_event->Trigger();
    }
    virtual void RequestQuit(QueueIndex queueIndex) override {
        assert(m_queue.m_hang_event != nullptr);
        m_queue.m_close = true;
        m_queue.m_hang_event->Trigger();
    }
    virtual uint32_t Run() override {
        return TaskThreadBase::Run(); //process task until quit
    }
    uint32_t     ProcessTasks();
    virtual bool IsProcessingTask(QueueIndex queueIndex) override {
        return m_queue.call_amount > 0;
    }

protected:
    AnyThreadTaskQueue m_queue;
    BaseGraphTask*     FindTaskToDo();
};

class NamedThread : public TaskThreadBase {
    struct NamedTaskQueue {
        TaskFIFOQueue<BaseGraphTask, 2> m_queue;
        Event*                          m_hang_event;
        bool                            m_close;
        bool                            m_should_return;
        bool                            m_hang;
        uint32_t                        call_amount;

        explicit NamedTaskQueue() :
            m_hang_event{EventPool::Get()->GetEvent()},
            call_amount{0},
            m_close{false},
            m_hang{false},
            m_should_return{false} {}
        ~NamedTaskQueue() {
            EventPool::Get()->ReleaseEvent(m_hang_event);
            m_hang_event = nullptr;
        }
    };

public:
    virtual void ProcessTaskUntilQuit(QueueIndex queueIndex) override {
        m_queue[queueIndex].m_should_return = false;
        do {
            ProcessTasks(queueIndex, true); //hang up thread when there's no task to execute

        } while (!m_queue[queueIndex].m_close && !m_queue[queueIndex].m_should_return);
    }
    virtual void ProcessTaskUntilFinished(QueueIndex queueIndex) override {
        m_queue[queueIndex].m_should_return = false;
        ProcessTasks(
            queueIndex, false
        ); //don't hang up thread and break the loop when there's no task to execute
    }
    virtual void Wake(QueueIndex queueIndex) override {
        m_queue[queueIndex].m_hang_event->Trigger();
    }
    virtual void RequestQuit(QueueIndex queueIndex) override {
        if (m_queue[queueIndex].m_hang_event == nullptr)
            return;
        //main queue means quit
        if (queueIndex == QUIT) {
            m_queue[EThread::MAIN_QUEUE].m_close = true;
            m_queue[EThread::MAIN_QUEUE].m_hang_event->Trigger();
            m_queue[EThread::LOCAL_QUEUE >> EThread::QUEUE_MASK_SHEFT].m_close = true;
            m_queue[EThread::LOCAL_QUEUE >> EThread::QUEUE_MASK_SHEFT].m_hang_event->Trigger();
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
            m_queue[queueIndex].m_hang_event->Trigger();
            return true;
        }

        return false;
    }
    virtual uint32_t Run() override {
        return TaskThreadBase::Run(); //process task until quit
    }
    uint32_t     ProcessTasks(QueueIndex index, bool allowHang);
    virtual bool IsProcessingTask(QueueIndex queueIndex) override {
        return m_queue[queueIndex].call_amount > 0;
    }

protected:
    NamedTaskQueue m_queue[2];
};
#endif // !THREAD_H
