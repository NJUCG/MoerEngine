#ifndef GRAPH_TASK_H
#define GRAPH_TASK_H
#include "API_Macro.h"
#include "ThreadManager.h"
#include "misc/CountableRef.h"
#include "misc/LockFree.h"
#include "misc/MMemory.h"
#include "misc/MacroUtils.h"
#include <atomic>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

class GraphEvent;
typedef CountableRef<GraphEvent>   GraphEventRef;
typedef Moer::Array<GraphEventRef> GraphEventArray;
class BaseGraphTask {
public:
    BaseGraphTask(int32_t count) : m_prerequests_count{count} {}
    virtual ~BaseGraphTask() {}
    virtual void ExecuteTasks(EThread::Type currentThread) = 0;
    virtual void DestroyTask()                             = 0;
    void         Execute(EThread::Type currentThread) {
        ExecuteTasks(currentThread);
    }
    void SetPreferredThread(EThread::Type thread) {
        m_preferd_thread = thread;
    }
    void SetPriority(ThreadPriority priority) {
        m_preferd_thread = EThread::SetPriority(m_preferd_thread, priority << EThread::PRIORITY_SHEFT);
    }
    void QueueTask(EThread::Type currentThread, bool shouldWakeWorker) noexcept;

    CORE_API void
    PrerequestsComplete(EThread::Type currentThread, int32_t finishedCount, bool unlock = true) noexcept;

    bool ConditionalQueueTask(EThread::Type currentThread, bool shouldWakeWorker) noexcept {
        if (m_prerequests_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            QueueTask(currentThread, shouldWakeWorker);
            return true;
        }
        return shouldWakeWorker;
    }
    ThreadPriority GetPriority() {
        return EThread::GetThreadPriority(m_preferd_thread);
    }
    EThread::Type GetPreferredThread() {
        return m_preferd_thread;
    }

protected:
    EThread::Type        m_preferd_thread{EThread::Invalid};
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
    CORE_API bool IsComplete() const;
    void          WaitUntil(GraphEventRef _eventRef) {
        assert(!IsComplete());

        m_events_to_wait.emplace_back(std::move(_eventRef));
    }
    CORE_API void TryUnlockSubsequents(EThread::Type currentThread = EThread::UNKNOWN_THREAD);

    CORE_API void Wait(EThread::Type currentThread = EThread::UNKNOWN_THREAD);
    GraphEventRef GetHandle() {
        return this;
    }

private:
    ClosableLockFreeMpScStack<BaseGraphTask> m_subsequents;

    GraphEventArray m_events_to_wait;

    EThread::Type thread_to_wait_on;
};
template<typename TaskType>
class GraphTask final : public BaseGraphTask {

    class Constructor {
    public:
        friend class GraphTask;
        Constructor(
            GraphTask*             _owner,
            const GraphEventArray* _prerequests   = nullptr,
            EThread::Type          _currentThread = EThread::AnyThread_NormalPri
        ) :
            owner{_owner},
            prerequests{_prerequests},
            current_thread{_currentThread} {}

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
    class CreateInfo {
    public:
        CreateInfo() = default;
        CreateInfo(GraphTask* _task_holder) : task_holder{_task_holder} {}
        CreateInfo& Wait(const GraphEventArray& _waits) {
            task_holder->m_prerequests_count.fetch_add(_waits.size());

            waits.insert(waits.end(), _waits.begin(), _waits.end());
            return *this;
        }
        // CreateInfo& Wait(const GraphEventArray* _waits) {
        //     // task_holder->m_prerequests_count.fetch_add(_waits.size());
        //     waits = _waits;
        //     // waits.insert(waits.end(), _waits.begin(), _waits.end());
        //     return *this;
        // }
        CreateInfo& Wait(GraphEventArray&& _waits) {
            task_holder->m_prerequests_count.fetch_add(_waits.size());
            if (waits.empty()) {
                waits = std::move(_waits);
            } else {
                waits.insert(waits.end(), _waits.begin(), _waits.end());
            }
            return *this;
        }
        CreateInfo& Wait(GraphEventRef _event_to_wait) {
            task_holder->m_prerequests_count.fetch_add(1);
            waits.emplace_back(_event_to_wait);
            return *this;
        }
        CreateInfo& Next(GraphEventRef _subsequent) {
            task_holder->m_subsequents = _subsequent;
            return *this;
        }
        GraphEventRef Dispatch(EThread::Type _target_thread = EThread::Invalid) {
            assert(task_holder != nullptr);
            if (_target_thread != EThread::Invalid) {
                task_holder->SetPreferredThread(_target_thread);
            }
            if (task_holder->GetPreferredThread() == EThread::Invalid) {
                task_holder->SetPreferredThread(EThread::AnyThread_NormalPri);
            }
            if (task_holder->m_subsequents == nullptr) {
                task_holder->m_subsequents = GraphEvent::CreateGraphEvent();
            }
            return task_holder->Setup(&waits);
        }

        GraphEventRef GetCompletionEvent() {
            if (task_holder->m_subsequents == nullptr) {
                task_holder->m_subsequents = GraphEvent::CreateGraphEvent();
            }
            return task_holder->m_subsequents;
        }

    private:
        friend class GraphTask;
        GraphEventArray waits;
        EThread::Type   current_thread;
        GraphTask*      task_holder;
    };

public:
    ~GraphTask() {}

public:
    friend class Constructor;
    template<typename... Ts>
        requires std::is_constructible_v<TaskType, Ts...>
    static CreateInfo Create(Ts&&... _args) {
        auto* task_holder = MoerNew(GraphTask)();
        new ((void*)&task_holder->task_slot) TaskType(std::forward<Ts>(_args)...);
        TaskType& task = *(TaskType*)&task_holder->task_slot;
        if (task.GetPreferredThread() != EThread::Invalid) {
            task_holder->SetPreferredThread(task.GetPreferredThread());
        }
        return std::move(CreateInfo(task_holder));
    }
    virtual void ExecuteTasks(EThread::Type currentThread) override {

        TaskType& task = *(TaskType*)&task_slot;

        task.Fire(currentThread, m_subsequents);
        task.~TaskType();

        {
            std::atomic_thread_fence(std::memory_order_acq_rel);
            m_subsequents->TryUnlockSubsequents(currentThread);
        }

        DestroyTask();
    };
    void Dispatch(EThread::Type currentThread = EThread::UNKNOWN_THREAD) {
        ConditionalQueueTask(currentThread, true);
    }
    virtual void DestroyTask() override {
        MoerDelete(this);
    }
    static Constructor CreateTask(
        GraphEventRef          subsequents,
        const GraphEventArray* preRequests   = nullptr,
        EThread::Type          currentThread = EThread::UNKNOWN_THREAD
    ) {
        int event_count = preRequests == nullptr ? 0 : preRequests->size();
        return Constructor(MoerNew(GraphTask)(subsequents, event_count), preRequests, currentThread);
    }
    static Constructor CreateTask(
        const GraphEventArray* preRequests   = nullptr,
        EThread::Type          currentThread = EThread::UNKNOWN_THREAD
    ) {
        int event_count = preRequests == nullptr ? 0 : preRequests->size();
        return Constructor(
            MoerNew(GraphTask)(GraphEvent::CreateGraphEvent(), event_count), preRequests, currentThread
        );
    }
    GraphEventRef GetCompletionEvent() {
        return m_subsequents;
    };

private:
    GraphTask(GraphEventRef _subsequents, int32_t initialCount) : BaseGraphTask(initialCount + 1) {
        m_subsequents.Swap(_subsequents);
    }
    GraphTask() : BaseGraphTask(1) {}

    GraphEventRef Setup(
        const GraphEventArray* prerequests   = nullptr,
        EThread::Type          currentThread = EThread::UNKNOWN_THREAD,
        bool                   unlock        = true
    ) {
        GraphEventRef prevent_deconstruct = m_subsequents;
        TaskType&     task                = *(TaskType*)&task_slot;
        int32_t       completed_prerequest_count{0};
        if (m_preferd_thread == EThread::Invalid) {
            BaseGraphTask::SetPreferredThread(task.GetPreferredThread());
        }
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

    GraphEventRef TryDispatch(GraphEventArray& _events_to_wait) {
        GraphEventRef prevent_deconstruct = m_subsequents;
        TaskType&     task                = *(TaskType*)&task_slot;
        if (m_preferd_thread == EThread::Invalid) {
            BaseGraphTask::SetPreferredThread(task.GetPreferredThread());
        }
        uint32_t completed_prerequest_count{0};
        for (int32_t i = 0; i < _events_to_wait.size(); i++) {
            GraphEvent* event = _events_to_wait[i].Get();
            if (event == nullptr || !event->AddSubsequent(this)) {
                completed_prerequest_count++;
            }
        }
        PrerequestsComplete(EThread::UNKNOWN_THREAD, completed_prerequest_count, false);
        return prevent_deconstruct;
    }

    GraphTask* Hold(
        const GraphEventArray* prerequests   = nullptr,
        EThread::Type          currentThread = EThread::UNKNOWN_THREAD
    ) {
        Setup(prerequests, currentThread, false);
        return this;
    }

    Container<sizeof(TaskType)> task_slot;
    GraphEventRef               m_subsequents;
};

#endif // !GRAPH_TASK_H
