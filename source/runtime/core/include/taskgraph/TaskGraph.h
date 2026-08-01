#ifndef TASK_GRAPH_H
#define TASK_GRAPH_H
#include "API_Macro.h"
#include "Thread.h"
#include "misc/CountableRef.h"

enum class ESchedule {
    Max_Local_Capacity = 16
};

class TaskGraph {
private:
    static TaskGraph* instance;

public:
    CORE_API static TaskGraph& GetInterface();
    CORE_API static bool       IsInitialized() noexcept;
    CORE_API static void       Init();
    CORE_API static void       Shutdown();
    TaskGraph();
    ~TaskGraph();
    CORE_API void WaitUntilTasksComplete(const GraphEventArray& task_events, EThread::Type currentThread);
    CORE_API void WaitUntilTaskComplete(const GraphEventRef& task, EThread::Type currentThread);
    CORE_API void WaitUntilTaskComplete(GraphEventRef&& task, EThread::Type currentThread);
    CORE_API void AttachToNameThread(EThread::Type type);
    // Task allocation and construction happen before this publication
    // boundary and may fail normally. Once QueueTask is entered, ownership
    // can become visible to a worker, so no exception may escape back to the
    // producer. A platform wake failure is therefore process-fatal.
    virtual void QueueTask(
        BaseGraphTask* task,
        EThread::Type  prefered_thread,
        EThread::Type  current_thread = EThread::UNKNOWN_THREAD,
        bool           wake_worker    = true
    ) noexcept;
    virtual void           ReturnThread(EThread::Type index);
    virtual BaseGraphTask* DequeueTask(int32_t threadIndex);
    CORE_API virtual void  ProcessThreadUntilIdle(EThread::Type index);
    CORE_API virtual void  ProcessThreadUntilReturn(EThread::Type index);
    CORE_API bool          IsThreadProcessingTask(EThread::Type index);
    inline uint32_t        GetWorkerThreadCount() const {
        return m_worker_thread_count;
    }
    /** Returns the scheduler identity of the caller, or UNKNOWN for an external thread. */
    CORE_API EThread::Type GetCurrentThread(bool localQueue = false);

protected:
    void TriggerEventWhenTasksComplete(
        Event*                 event,
        const GraphEventArray& task_events,
        EThread::Type          currentThread = EThread::UNKNOWN_THREAD,
        EThread::Type          triggerThread = EThread::UNKNOWN_THREAD
    );

private:
    TaskThreadBase& GetThread(ThreadIndex index);
    int32_t         GetThreadPriorityFromIndex(int32_t threadIndex) {
        return (threadIndex - m_named_thread_count) / m_worker_per_priority;
    }
    void         WakeUpWorkerThread(int32_t threadIndex, QueueIndex index) noexcept;
    WorkerThread m_workers[INT16_MAX];
    int32_t      m_thread_count;
    int32_t      m_named_thread_count;
    int32_t      m_worker_per_priority;
    int32_t      m_worker_thread_count;

    TaskFIFOQueue<BaseGraphTask, 1> m_task_queue[EThread::PriorityCount];
    TaskFIFOQueue<BaseGraphTask, 1> m_global_queue; //
};

class CORE_API TriggerEventGraphTask {
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
    void Fire(EThread::Type _threadToReturn, const GraphEventRef& _event) {}

private:
    EThread::Type m_thread_to_return;
};

template<typename FunctionType>
class FunctionGraphTaskInner {
public:
    FunctionGraphTaskInner(std::function<FunctionType>&& functionRef, EThread::Type inType) :
        m_preferred_thread{inType},
        m_function{std::make_unique<std::function<FunctionType>>(std::move(functionRef))} {}
    FunctionGraphTaskInner(std::function<FunctionType>& functionRef, EThread::Type inType) :
        m_preferred_thread{inType},
        m_function{std::make_unique<std::function<FunctionType>>(std::move(functionRef))} {}
    EThread::Type GetPreferredThread() {
        return m_preferred_thread;
    }
    void Fire(EThread::Type _current_thread, const GraphEventRef& _event) {
        FireInner(_current_thread, _event, *m_function);
    }

private:
    FORCEINLINE static void FireInner(
        EThread::Type                                             currentThread,
        const GraphEventRef&                                      _event,
        std::function<void(EThread::Type, const GraphEventRef&)>& func
    ) {
        func(currentThread, _event);
    }
    FORCEINLINE static void
    FireInner(EThread::Type currentThread, const GraphEventRef& _event, std::function<void()>& func) {
        func();
    }
    EThread::Type                                m_preferred_thread;
    std::unique_ptr<std::function<FunctionType>> m_function;
};

class [[deprecated("will not be supported in the future")]] FunctionGraphTask {
public:
    static GraphEventRef ConstructAndDispatchWhenReady(
        std::function<void()>& func,
        const GraphEventArray* prerequest       = nullptr,
        EThread::Type          preferred_thread = EThread::AnyThread_NormalPri
    ) {
        return ConstructAndDispatchWhenReady(std::move(func), prerequest, preferred_thread);
    }

    static GraphEventRef ConstructAndDispatchWhenReady(
        std::function<void()>&& func,
        const GraphEventArray*  prerequest       = nullptr,
        EThread::Type           preferred_thread = EThread::AnyThread_NormalPri
    ) {
        return GraphTask<FunctionGraphTaskInner<void()>>::CreateTask(prerequest)
            .ConstructAndDispatchWhenReady(func, preferred_thread);
    }

    static GraphEventRef ConstructAndDispatchWhenReady(
        std::function<void(EThread::Type, const GraphEventRef&)>& func,
        const GraphEventArray*                                    prerequest = nullptr,
        EThread::Type preferred_thread                                       = EThread::AnyThread_NormalPri
    ) {
        return ConstructAndDispatchWhenReady(std::move(func), prerequest, preferred_thread);
    }

    static GraphEventRef ConstructAndDispatchWhenReady(
        std::function<void(EThread::Type, const GraphEventRef&)>&& func,
        const GraphEventArray*                                     prerequest = nullptr,
        EThread::Type preferred_thread                                        = EThread::AnyThread_NormalPri
    ) {
        return GraphTask<FunctionGraphTaskInner<void(EThread::Type, const GraphEventRef&)>>::CreateTask(
                   prerequest
        )
            .ConstructAndDispatchWhenReady(func, preferred_thread);
    }

    static GraphEventRef ConstructAndDispatchWhenReady(
        std::function<void()>& func,
        const GraphEventRef&   prerequest,
        EThread::Type          preferred_thread = EThread::AnyThread_NormalPri
    ) {

        return ConstructAndDispatchWhenReady(std::move(func), prerequest, preferred_thread);
    }
    static GraphEventRef ConstructAndDispatchWhenReady(
        std::function<void()>&& _func,
        const GraphEventRef&    _prerequest,
        EThread::Type           preferred_thread = EThread::AnyThread_NormalPri
    ) {
        GraphEventArray prerequests{_prerequest};
        return GraphTask<FunctionGraphTaskInner<void()>>::CreateTask(&prerequests)
            .ConstructAndDispatchWhenReady(_func, preferred_thread);
    }

    static GraphEventRef ConstructAndDispatchWhenReady(
        std::function<void(EThread::Type, const GraphEventRef&)>& func,
        const GraphEventRef&                                      prerequest,
        EThread::Type preferred_thread = EThread::AnyThread_NormalPri
    ) {

        return ConstructAndDispatchWhenReady(std::move(func), prerequest, preferred_thread);
    }

    static GraphEventRef ConstructAndDispatchWhenReady(
        std::function<void(EThread::Type, const GraphEventRef&)>&& func,
        const GraphEventRef&                                       prerequest,
        EThread::Type preferred_thread = EThread::AnyThread_NormalPri
    ) {
        GraphEventArray prerequests{prerequest};
        return GraphTask<FunctionGraphTaskInner<void(EThread::Type, const GraphEventRef&)>>::CreateTask(
                   &prerequests
        )
            .ConstructAndDispatchWhenReady(func, preferred_thread);
    }
};
class LambdaTask {
public:
    using TGraphTask          = GraphTask<FunctionGraphTaskInner<void()>>;
    using TGraphTaskWithParam = GraphTask<FunctionGraphTaskInner<void(EThread::Type, const GraphEventRef&)>>;

    static GraphEventRef
    Dispatch(std::function<void()>& _func, EThread::Type _thread = EThread::AnyThread_NormalPri) {
        return TGraphTask::Create(_func, _thread).Dispatch();
    }

    static GraphEventRef
    Dispatch(std::function<void()>&& _func, EThread::Type _thread = EThread::AnyThread_NormalPri) {
        return TGraphTask::Create(std::forward<std::function<void()>>(_func), _thread).Dispatch();
    }

    static GraphEventRef Dispatch(
        std::function<void(EThread::Type, const GraphEventRef&)>&& _func,
        EThread::Type                                              _thread = EThread::AnyThread_NormalPri
    ) {
        return TGraphTaskWithParam::Create(
                   std::forward<std::function<void(EThread::Type, const GraphEventRef&)>>(_func), _thread
        )
            .Dispatch();
    }

    static GraphEventRef Dispatch(
        std::function<void(EThread::Type, const GraphEventRef&)>& _func,
        EThread::Type                                             _thread = EThread::AnyThread_NormalPri
    ) {
        return TGraphTaskWithParam::Create(_func, _thread).Dispatch();
    }
    static TGraphTask::CreateInfo
    Create(std::function<void()>& _func, EThread::Type _thread = EThread::AnyThread_NormalPri) {
        return TGraphTask::Create(_func, _thread);
    }
    static TGraphTask::CreateInfo
    Create(std::function<void()>&& _func, EThread::Type _thread = EThread::AnyThread_NormalPri) {
        return TGraphTask::Create(std::forward<std::function<void()>>(_func), _thread);
    }
};

template<typename ReturnType, typename... Args>
struct SharedFunctor {
    struct SharedFunctorStruct {
        void (*destroy_func)(void*) = nullptr;
        uint32_t             size   = 0;
        std::atomic_uint32_t ref_count{1};
    };
    SharedFunctorStruct* m_functor = nullptr;
    ReturnType (*m_func)(void*, Args...);
    void Destroy() {
        if (!m_functor) {
            return;
        }
        if (m_functor->ref_count.fetch_sub(1) == 1) {
            if (m_functor->destroy_func) {
                m_functor->destroy_func(m_functor);
            }
            Memory::Free(m_functor, m_functor->size);
        }

        m_functor = nullptr;
    }
    ReturnType operator()(Args... _args) const noexcept {
        assert(m_functor != nullptr);
        if constexpr (std::is_same_v<ReturnType, void>) {
            m_func(m_functor, _args...);
            return;
        }
        return m_func(m_functor, _args...);
    }

    template<typename T>
        requires(
            (!std::is_same_v<std::remove_cvref_t<T>, SharedFunctor>) &&
            (std::is_invocable_r_v<ReturnType, T, Args && ...>)
        )
    SharedFunctor(T&& _func) {
        struct ConcreteFunctor : SharedFunctorStruct {
            Container<sizeof(T)> storage;
        };
        using Func            = std::remove_cvref_t<T>;
        auto concrete_functor = MoerPlacementNew(Memory::Malloc(sizeof(ConcreteFunctor))) ConcreteFunctor();
        m_functor             = concrete_functor;
        m_functor->size       = sizeof(ConcreteFunctor);
        MoerPlacementNew(&concrete_functor->storage) Func(std::forward<T>(_func));

        if constexpr (std::is_trivially_destructible_v<Func>) {
            m_functor->destroy_func = nullptr;
        } else {
            m_functor->destroy_func = [](void* _ptr) -> void {
                return ((Func*)&((ConcreteFunctor*)_ptr)->storage)->~Func();
            };
        }
        m_func = [](void* _ptr, Args... _args) -> ReturnType {
            auto&& func = reinterpret_cast<Func&>(((ConcreteFunctor*)_ptr)->storage);
            if constexpr (std::is_same_v<ReturnType, void>) {
                func(std::forward<Args>(_args)...);
                return;
            } else {
                return func(std::forward<Args>(_args)...);
            }
        };
    }
    SharedFunctor() noexcept : m_functor{nullptr} {}
    ~SharedFunctor() noexcept {
        Destroy();
    }

    template<typename Functor>
        requires std::is_invocable_r_v<ReturnType, Functor, Args&&...>
    SharedFunctor& operator=(Functor&& _f) noexcept {
        Destroy();
        MoerPlacementNew(std::launder(this)) SharedFunctor(std::forward<Functor>(_f));
        return *this;
    }

    template<typename Ret, typename... FuncArgs>
        requires std::is_invocable_r_v<ReturnType, Ret (*)(FuncArgs...), Args&&...>
    SharedFunctor(Ret (*_other_ptr)(FuncArgs...)) noexcept {
        using OtherFuncType = decltype(_other_ptr);
        struct ConcreteFunctor : SharedFunctorStruct {
            OtherFuncType other_ptr;
        };
        auto concrete_functor = MoerPlacementNew(Memory::Malloc(sizeof(ConcreteFunctor))) ConcreteFunctor();
        m_functor             = concrete_functor;
        m_functor->size       = sizeof(ConcreteFunctor);
        concrete_functor->other_ptr = _other_ptr;
        m_functor->destroy_func     = nullptr;

        m_func = [](void* _ptr, Args... _args) -> ReturnType {
            auto func = (&((ConcreteFunctor*)_ptr)->other_ptr);
            if constexpr (std::is_same_v<ReturnType, void>) {
                func(std::forward<Args>(_args)...);
                return;
            } else {
                return func(std::forward<Args>(_args)...);
            }
        };
    }

    template<typename Ret, typename... FuncArgs>
        requires std::is_invocable_r_v<ReturnType, Ret (*)(FuncArgs...), Args&&...>
    SharedFunctor& operator=(Ret (*_other_ptr)(FuncArgs...)) noexcept {
        Destroy();
        MoerPlacementNew(std::launder(this)) SharedFunctor(_other_ptr);
        return *this;
    }

    SharedFunctor(const SharedFunctor& _other) noexcept {
        if (!_other.m_functor) {
            m_functor = nullptr;
            return;
        }
        m_functor = _other.m_functor;
        m_func    = _other.m_func;
        m_functor->ref_count.fetch_add(1);
    }

    SharedFunctor(SharedFunctor&& _other) noexcept {
        m_functor        = _other.m_functor;
        m_func           = _other.m_func;
        _other.m_functor = nullptr;
    }

    SharedFunctor& operator=(const SharedFunctor& _other) noexcept {
        if (this == &_other) {
            return *this;
        }
        Destroy();
        MoerPlacementNew(std::launder(this)) SharedFunctor(_other);
        return *this;
    }
};

template<typename FunctionType>
    requires std::is_invocable_v<FunctionType, uint32_t>
inline static void ParallelFor(uint32_t _size, FunctionType&& _func) {
    uint32_t worker_cnt = TaskGraph::GetInterface().GetWorkerThreadCount();
    if (worker_cnt == 0) {
        worker_cnt = 1;
    }
    uint32_t chunk_size = (_size + worker_cnt - 1) / worker_cnt;
    if (chunk_size == 0) {
        chunk_size = 1;
    }
    uint32_t        chunk_count = (_size + chunk_size - 1) / chunk_size;
    GraphEventArray events;
    events.reserve(chunk_count);

    SharedFunctor<void, int> func([lambda = std::forward<FunctionType>(_func)](int _i) mutable {
        lambda(_i);
    });

    for (uint32_t i = 0; i < chunk_count; i++) {
        uint32_t start = i * chunk_size;
        uint32_t end   = std::min(start + chunk_size, _size);
        events.push_back(LambdaTask::Dispatch([=](EThread::Type, const GraphEventRef&) {
            for (uint32_t j = start; j < end; j++) {
                func(j);
            }
        }));
    }
    TaskGraph::GetInterface().WaitUntilTasksComplete(events, EThread::UNKNOWN_THREAD);
}

template<typename FunctionType>
    requires std::is_invocable_v<FunctionType, uint32_t>
GraphEventRef ParallelForAsync(uint32_t _size, FunctionType&& _func) {
    uint32_t worker_cnt = TaskGraph::GetInterface().GetWorkerThreadCount();
    if (worker_cnt == 0) {
        worker_cnt = 1;
    }
    uint32_t chunk_size = (_size + worker_cnt - 1) / worker_cnt;
    if (chunk_size == 0) {
        chunk_size = 1;
    }
    uint32_t        chunk_count = (_size + chunk_size - 1) / chunk_size;
    GraphEventArray events;
    events.reserve(chunk_count);

    SharedFunctor<void, int> func([lambda = std::forward<FunctionType>(_func)](int _i) mutable {
        lambda(_i);
    });

    for (uint32_t i = 0; i < chunk_count; i++) {
        uint32_t start = i * chunk_size;
        uint32_t end   = std::min(start + chunk_size, _size);
        events.push_back(LambdaTask::Dispatch([=](EThread::Type, const GraphEventRef&) mutable {
            for (uint32_t j = start; j < end; j++) {
                func(j);
            }
        }));
    }

    return GraphTask<EmptyGraphTask>::Create(EThread::AnyThread_HighPri).Wait(std::move(events)).Dispatch();
}
#endif // !TASK_GRAPH_H
