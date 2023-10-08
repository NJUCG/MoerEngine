#ifndef GRAPH_TASK_H
#define GRAPH_TASK_H
#include "ThreadManager.h"
#include <utility>
#include <vector>
#include <memory>
#include "misc/CountableRef.h"
#include "TaskGraph.h"
#include "misc/StatQueue.h"

class BaseGraphTask {
public:
    BaseGraphTask(int32_t count) : m_prerequests_count{count} {}
    virtual ~BaseGraphTask() {}
    virtual void executeTasks(std::vector<BaseGraphTask*>& tasks, EThread::Type currentThread) = 0;
    virtual void destroyTask()                                                                 = 0;
    void         execute(std::vector<BaseGraphTask*>& newTasks, EThread::Type currentThread) {
        executeTasks(newTasks, currentThread);
    }
    void setPreferredThread(EThread::Type thread) {
        m_preferdThread = thread;
    }
    void setPriority(ThreadPriority priority) {
        m_preferdThread = EThread::SetPriority(m_preferdThread, priority << EThread::PRIORITY_SHEFT);
    }
    void queueTask(EThread::Type currentThread, bool shouldWakeWorker);
    void prerequestsComplete(EThread::Type currentThread, int32_t finishedCount, bool unlock = true);
    bool conditionalQueueTask(EThread::Type currentThread, bool shouldWakeWorker) {
        if (m_prerequests_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            queueTask(currentThread, shouldWakeWorker);
            return true;
        }
        return shouldWakeWorker;
    }
    ThreadPriority getPriority() { return EThread::GetThreadPriority(m_preferdThread); }
    EThread::Type  getPreferredThread() { return m_preferdThread; }

protected:
    EThread::Type        m_preferdThread{EThread::UNKNOWN_THREAD};
    std::atomic<int32_t> m_prerequests_count;
};

template<int32_t size>
struct Container {
    uint8_t pad[size];
};

class GraphEvent : public Countable {
    friend class GraphEventPool;

public:
    GraphEvent() : threadToWaitOn{EThread::UNKNOWN_THREAD} {};
    ~GraphEvent() = default;
    static GraphEventRef CreateGraphEvent();
    bool                 addSubsequent(BaseGraphTask* subsequent);
    void                 Destroy() override {
        delete this;
    }
    bool isComplete();
    void waitUntil(GraphEventRef _eventRef) {
        assert(!isComplete());

        m_events_to_wait.emplace_back(std::move(_eventRef));
    }
    void tryUnlockSubsequents(std::vector<BaseGraphTask*>& tasks, EThread::Type currentThread = EThread::UNKNOWN_THREAD);
    void tryUnlockSubsequents(EThread::Type currentThread = EThread::UNKNOWN_THREAD);

    void          wait(EThread::Type currentThread = EThread::UNKNOWN_THREAD);
    GraphEventRef getHandle() {
        return this;
    }

private:
    StatMPSCQueue<BaseGraphTask*> m_subsequents;
    GraphEventArray               m_events_to_wait;

    EThread::Type threadToWaitOn;
};
template<typename TaskType>
class GraphTask final : public BaseGraphTask {

    class Constructor {
    public:
        friend class GraphTask;
        Constructor(GraphTask* _owner, const GraphEventArray* _prerequests = nullptr, EThread::Type _currentThread = EThread::AnyThread_NormalPri) : owner{_owner}, prerequests{_prerequests}, currentThread{_currentThread} {}

    public:
        template<typename... Ts>
        GraphEventRef ConstructAndDispatchWhenReady(Ts&&... arg_left) {
            new ((void*)&owner->taskSlot) TaskType(std::forward<Ts>(arg_left)...);
            return owner->setup(prerequests, currentThread);
        }
        template<typename... Ts>
        GraphTask* ConstructAndHold(Ts&&... arg_left) {
            new ((void*)&owner->taskSlot) TaskType(std::forward<Ts>(arg_left)...);
            return owner->hold();
        }

    private:
        GraphTask*             owner;
        const GraphEventArray* prerequests;
        EThread::Type          currentThread;
    };

public:
    ~GraphTask() {}

public:
    friend class Constructor;
    virtual void executeTasks(std::vector<BaseGraphTask*>& tasks, EThread::Type currentThread) override {

        TaskType& task = *(TaskType*)&taskSlot;

        task.Fire(currentThread, m_subsequents);
        task.~TaskType();

        {
            std::atomic_thread_fence(std::memory_order_acq_rel);
            m_subsequents->tryUnlockSubsequents(tasks, currentThread);
        }

        destroyTask();
    };
    void dispatch(EThread::Type currentThread = EThread::UNKNOWN_THREAD) {
        conditionalQueueTask(currentThread, true);
    }
    virtual void destroyTask() override {
        delete this;
    }
    static Constructor CreateTask(GraphEventRef subsequents, const GraphEventArray* graphEvents = nullptr, EThread::Type currentThread = EThread::UNKNOWN_THREAD) {
        int eventCount = graphEvents == nullptr ? 0 : graphEvents->size();
        return Constructor(new GraphTask(subsequents, eventCount), graphEvents, currentThread);
    }
    static Constructor CreateTask(const GraphEventArray* graphEvents = nullptr, EThread::Type currentThread = EThread::UNKNOWN_THREAD) {
        int eventCount = graphEvents == nullptr ? 0 : graphEvents->size();
        return Constructor(new GraphTask(GraphEvent::CreateGraphEvent(), eventCount), graphEvents, currentThread);
    }
    GraphEventRef GetCompletionEvent() { return m_subsequents; };

private:
    GraphTask(GraphEventRef _subsequents, int32_t initialCount) : BaseGraphTask(initialCount + 1) {
        m_subsequents.swap(_subsequents);
    }

    GraphEventRef setup(const GraphEventArray* prerequests = nullptr, EThread::Type currentThread = EThread::UNKNOWN_THREAD, bool unlock = true) {
        GraphEventRef prevent_deconstruct = m_subsequents;
        TaskType&     task                = *(TaskType*)&taskSlot;
        BaseGraphTask::setPreferredThread(task.getPreferredThread());
        int32_t completed_prerequestCount{0};
        if (prerequests != nullptr) {
            for (int32_t i = 0; i < prerequests->size(); i++) {
                GraphEvent* _event = (*prerequests)[i].Get();
                if (_event == nullptr || !_event->addSubsequent(this)) {
                    completed_prerequestCount++;
                }
            }
        }
        prerequestsComplete(currentThread, completed_prerequestCount, unlock);
        return prevent_deconstruct;
    }

    GraphTask* hold(const GraphEventArray* prerequests = nullptr, EThread::Type currentThread = EThread::UNKNOWN_THREAD) {
        setup(prerequests, currentThread, false);
        return this;
    }

    Container<sizeof(TaskType)> taskSlot;
    GraphEventRef               m_subsequents;
};

class TriggerEventGraphTask {
public:
    TriggerEventGraphTask(Event* _event, EThread::Type _type) : m_event{_event}, m_preferredThread{_type} {}
    EThread::Type getPreferredThread() {
        return m_preferredThread;
    }
    void Fire(EThread::Type _type, const GraphEventRef& _event);

private:
    Event*        m_event;
    EThread::Type m_preferredThread;
};

class ReturnGraphTask {
public:
    ReturnGraphTask(EThread::Type _threadToReturn) : m_threadToReturn{_threadToReturn} {}
    EThread::Type getPreferredThread() {
        return m_threadToReturn;
    }
    void Fire(EThread::Type _threadToReturn, const GraphEventRef& _event) {
        TaskGraph::GetInterface().ReturnThread(m_threadToReturn);
    }

private:
    EThread::Type m_threadToReturn;
};

class EmptyGraphTask {
public:
    EmptyGraphTask(EThread::Type _threadToReturn) : m_threadToReturn{_threadToReturn} {}
    EThread::Type getPreferredThread() {
        return m_threadToReturn;
    }
    void Fire(EThread::Type _threadToReturn, const GraphEventRef& _event) {
    }

private:
    EThread::Type m_threadToReturn;
};

template<typename FunctionType>
class FunctionGraphTaskInner {
public:
    FunctionGraphTaskInner(std::function<FunctionType>&& functionRef, EThread::Type inType) : m_preferredThread{inType}, m_function{std::make_unique<std::function<FunctionType>>(std::move(functionRef))} {}
    FunctionGraphTaskInner(std::function<FunctionType>& functionRef, EThread::Type inType) : m_preferredThread{inType}, m_function{std::make_unique<std::function<FunctionType>>(std::move(functionRef))} {}
    EThread::Type getPreferredThread() {
        return m_preferredThread;
    }
    void Fire(EThread::Type currentThread, const GraphEventRef& _event) {
        FireInner(currentThread, _event, *m_function);
    }

private:
    __forceinline static void FireInner(EThread::Type currentThread, const GraphEventRef& _event, std::function<void(EThread::Type, const GraphEventRef&)>& func) {
        func(currentThread, _event);
    }
    __forceinline static void FireInner(EThread::Type currentThread, const GraphEventRef& _event, std::function<void()>& func) {
        func();
    }
    EThread::Type                                m_preferredThread;
    std::unique_ptr<std::function<FunctionType>> m_function;
};

class FunctionGraphTask {
public:
    static GraphEventRef ConstructAndDispatchWhenReady(std::function<void()>& func, const GraphEventArray* prerequest = nullptr, EThread::Type preferred_thread = EThread::AnyThread_NormalPri) {
        return ConstructAndDispatchWhenReady(std::move(func), prerequest, preferred_thread);
    }

    static GraphEventRef ConstructAndDispatchWhenReady(std::function<void()>&& func, const GraphEventArray* prerequest = nullptr, EThread::Type preferred_thread = EThread::AnyThread_NormalPri) {
        return GraphTask<FunctionGraphTaskInner<void()>>::CreateTask(prerequest).ConstructAndDispatchWhenReady(func, preferred_thread);
    }

    static GraphEventRef ConstructAndDispatchWhenReady(std::function<void(EThread::Type, const GraphEventRef&)>& func, const GraphEventArray* prerequest = nullptr, EThread::Type preferred_thread = EThread::AnyThread_NormalPri) {
        return ConstructAndDispatchWhenReady(std::move(func), prerequest, preferred_thread);
    }

    static GraphEventRef ConstructAndDispatchWhenReady(std::function<void(EThread::Type, const GraphEventRef&)>&& func, const GraphEventArray* prerequest = nullptr, EThread::Type preferred_thread = EThread::AnyThread_NormalPri) {
        return GraphTask<FunctionGraphTaskInner<void(EThread::Type, const GraphEventRef&)>>::CreateTask(prerequest).ConstructAndDispatchWhenReady(func, preferred_thread);
    }

    static GraphEventRef ConstructAndDispatchWhenReady(std::function<void()>& func, const GraphEventRef& prerequest, EThread::Type preferred_thread = EThread::AnyThread_NormalPri) {

        return ConstructAndDispatchWhenReady(std::move(func), prerequest, preferred_thread);
    }
    static GraphEventRef ConstructAndDispatchWhenReady(std::function<void()>&& func, const GraphEventRef& prerequest, EThread::Type preferred_thread = EThread::AnyThread_NormalPri) {
        GraphEventArray prerequests{prerequest};
        return GraphTask<FunctionGraphTaskInner<void()>>::CreateTask(&prerequests).ConstructAndDispatchWhenReady(func, preferred_thread);
    }

    static GraphEventRef ConstructAndDispatchWhenReady(std::function<void(EThread::Type, const GraphEventRef&)>& func, const GraphEventRef& prerequest, EThread::Type preferred_thread = EThread::AnyThread_NormalPri) {

        return ConstructAndDispatchWhenReady(std::move(func), prerequest, preferred_thread);
    }

    static GraphEventRef ConstructAndDispatchWhenReady(std::function<void(EThread::Type, const GraphEventRef&)>&& func, const GraphEventRef& prerequest, EThread::Type preferred_thread = EThread::AnyThread_NormalPri) {
        GraphEventArray prerequests{prerequest};
        return GraphTask<FunctionGraphTaskInner<void(EThread::Type, const GraphEventRef&)>>::CreateTask(&prerequests).ConstructAndDispatchWhenReady(func, preferred_thread);
    }
};
#endif// !GRAPH_TASK_H
