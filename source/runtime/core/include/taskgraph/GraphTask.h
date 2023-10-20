#ifndef GRAPH_TASK_H
#define GRAPH_TASK_H
#include "ThreadManager.h"
#include <utility>
#include <vector>
#include <memory>
#include "misc/CountableRef.h"
#include "TaskGraph.h"
#include "misc/AsyncQueue.h"

class BaseGraphTask {
public:
    BaseGraphTask(int32_t count) : m_prerequests_count{count} {}
    virtual ~BaseGraphTask() {}
    virtual void ExecuteTasks(std::vector<BaseGraphTask*>& tasks, EThread::Type currentThread) = 0;
    virtual void DestroyTask()                                                                 = 0;
    void         Execute(std::vector<BaseGraphTask*>& newTasks, EThread::Type currentThread) {
        ExecuteTasks(newTasks, currentThread);
    }
    void SetPreferredThread(EThread::Type thread) {
        m_preferd_thread = thread;
    }
    void SetPriority(ThreadPriority priority) {
        m_preferd_thread = EThread::SetPriority(m_preferd_thread, priority << EThread::PRIORITY_SHEFT);
    }
    void          QueueTask(EThread::Type currentThread, bool shouldWakeWorker);
    CORE_API void PrerequestsComplete(EThread::Type currentThread, int32_t finishedCount, bool unlock = true);
    bool          ConditionalQueueTask(EThread::Type currentThread, bool shouldWakeWorker) {
        if (m_prerequests_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            QueueTask(currentThread, shouldWakeWorker);
            return true;
        }
        return shouldWakeWorker;
    }
    ThreadPriority GetPriority() { return EThread::GetThreadPriority(m_preferd_thread); }
    EThread::Type  GetPreferredThread() { return m_preferd_thread; }

protected:
    EThread::Type        m_preferd_thread{EThread::UNKNOWN_THREAD};
    std::atomic<int32_t> m_prerequests_count;
};

template<int32_t size>
struct Container {
    uint8_t pad[size];
};

class GraphEvent : public Countable {
    friend class GraphEventPool;

public:
    GraphEvent() : thread_to_wait_on{EThread::UNKNOWN_THREAD} {};
    ~GraphEvent() = default;
    CORE_API static GraphEventRef CreateGraphEvent();
    CORE_API bool                 AddSubsequent(BaseGraphTask* subsequent);
    void                          Destroy() override {
        delete this;
    }
    bool IsComplete();
    void WaitUntil(GraphEventRef _eventRef) {
        assert(!IsComplete());

        m_events_to_wait.emplace_back(std::move(_eventRef));
    }
    CORE_API void TryUnlockSubsequents(std::vector<BaseGraphTask*>& tasks, EThread::Type currentThread = EThread::UNKNOWN_THREAD);
    CORE_API void TryUnlockSubsequents(EThread::Type currentThread = EThread::UNKNOWN_THREAD);

    CORE_API void Wait(EThread::Type currentThread = EThread::UNKNOWN_THREAD);
    GraphEventRef GetHandle() {
        return this;
    }

private:
    StatMPSCQueue<BaseGraphTask*> m_subsequents;
    GraphEventArray               m_events_to_wait;

    EThread::Type thread_to_wait_on;
};
template<typename TaskType>
class GraphTask final : public BaseGraphTask {

    class Constructor {
    public:
        friend class GraphTask;
        Constructor(GraphTask* _owner, const GraphEventArray* _prerequests = nullptr, EThread::Type _currentThread = EThread::AnyThread_NormalPri) : owner{_owner}, prerequests{_prerequests}, current_thread{_currentThread} {}

    public:
        template<typename... Ts>
        GraphEventRef ConstructAndDispatchWhenReady(Ts&&... arg_left) {
            new ((void*)&owner->task_slot) TaskType(std::forward<Ts>(arg_left)...);
            return owner->Setup(prerequests, current_thread);
        }
        template<typename... Ts>
        GraphTask* ConstructAndHold(Ts&&... arg_left) {
            new ((void*)&owner->task_slot) TaskType(std::forward<Ts>(arg_left)...);
            return owner->Hold();
        }

    private:
        GraphTask*             owner;
        const GraphEventArray* prerequests;
        EThread::Type          current_thread;
    };

public:
    ~GraphTask() {}

public:
    friend class Constructor;
    virtual void ExecuteTasks(std::vector<BaseGraphTask*>& tasks, EThread::Type currentThread) override {

        TaskType& task = *(TaskType*)&task_slot;

        task.Fire(currentThread, m_subsequents);
        task.~TaskType();

        {
            std::atomic_thread_fence(std::memory_order_acq_rel);
            m_subsequents->TryUnlockSubsequents(tasks, currentThread);
        }

        DestroyTask();
    };
    void Dispatch(EThread::Type currentThread = EThread::UNKNOWN_THREAD) {
        ConditionalQueueTask(currentThread, true);
    }
    virtual void DestroyTask() override {
        delete this;
    }
    static Constructor CreateTask(GraphEventRef subsequents, const GraphEventArray* preRequests = nullptr, EThread::Type currentThread = EThread::UNKNOWN_THREAD) {
        int event_count = preRequests == nullptr ? 0 : preRequests->size();
        return Constructor(new GraphTask(subsequents, event_count), preRequests, currentThread);
    }
    static Constructor CreateTask(const GraphEventArray* preRequests = nullptr, EThread::Type currentThread = EThread::UNKNOWN_THREAD) {
        int event_count = preRequests == nullptr ? 0 : preRequests->size();
        return Constructor(new GraphTask(GraphEvent::CreateGraphEvent(), event_count), preRequests, currentThread);
    }
    GraphEventRef GetCompletionEvent() { return m_subsequents; };

private:
    GraphTask(GraphEventRef _subsequents, int32_t initialCount) : BaseGraphTask(initialCount + 1) {
        m_subsequents.swap(_subsequents);
    }

    GraphEventRef Setup(const GraphEventArray* prerequests = nullptr, EThread::Type currentThread = EThread::UNKNOWN_THREAD, bool unlock = true) {
        GraphEventRef prevent_deconstruct = m_subsequents;
        TaskType&     task                = *(TaskType*)&task_slot;
        BaseGraphTask::SetPreferredThread(task.GetPreferredThread());
        int32_t completed_prerequest_count{0};
        if (prerequests != nullptr) {
            for (int32_t i = 0; i < prerequests->size(); i++) {
                GraphEvent* event = (*prerequests)[i].Get();
                if (event == nullptr || !event->AddSubsequent(this)) {
                    completed_prerequest_count++;
                }
            }
        }
        PrerequestsComplete(currentThread, completed_prerequest_count, unlock);
        return prevent_deconstruct;
    }

    GraphTask* Hold(const GraphEventArray* prerequests = nullptr, EThread::Type currentThread = EThread::UNKNOWN_THREAD) {
        Setup(prerequests, currentThread, false);
        return this;
    }

    Container<sizeof(TaskType)> task_slot;
    GraphEventRef               m_subsequents;
};

class TriggerEventGraphTask {
public:
    TriggerEventGraphTask(Event* _event, EThread::Type _type) : m_event{_event}, m_preferred_thread{_type} {}
    EThread::Type GetPreferredThread() {
        return m_preferred_thread;
    }
    void Fire(EThread::Type _type, const GraphEventRef& _event);

private:
    Event*        m_event;
    EThread::Type m_preferred_thread;
};

class ReturnGraphTask {
public:
    ReturnGraphTask(EThread::Type _threadToReturn) : m_thread_to_return{_threadToReturn} {}
    EThread::Type GetPreferredThread() {
        return m_thread_to_return;
    }
    void Fire(EThread::Type _threadToReturn, const GraphEventRef& _event) {
        TaskGraph::GetInterface().ReturnThread(m_thread_to_return);
    }

private:
    EThread::Type m_thread_to_return;
};

class EmptyGraphTask {
public:
    EmptyGraphTask(EThread::Type _threadToReturn) : m_thread_to_return{_threadToReturn} {}
    EThread::Type GetPreferredThread() {
        return m_thread_to_return;
    }
    void Fire(EThread::Type _threadToReturn, const GraphEventRef& _event) {
    }

private:
    EThread::Type m_thread_to_return;
};

template<typename FunctionType>
class FunctionGraphTaskInner {
public:
    FunctionGraphTaskInner(std::function<FunctionType>&& functionRef, EThread::Type inType) : m_preferred_thread{inType}, m_function{std::make_unique<std::function<FunctionType>>(std::move(functionRef))} {}
    FunctionGraphTaskInner(std::function<FunctionType>& functionRef, EThread::Type inType) : m_preferred_thread{inType}, m_function{std::make_unique<std::function<FunctionType>>(std::move(functionRef))} {}
    EThread::Type GetPreferredThread() {
        return m_preferred_thread;
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
    EThread::Type                                m_preferred_thread;
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
