#ifndef THREAD_MANAGER_H
#define THREAD_MANAGER_H

#include "API_Macro.h"
#include "misc/MMemory.h"
#include "misc/STL.h"
#include "misc/Types.h"
#include "platform/Platform.h"
#include <assert.h>
#include <string>
#include <thread>
#include <variant>
typedef Moer::int32 ThreadIndex;
typedef Moer::int32 QueueIndex;
typedef Moer::int32 ThreadPriority;
class Event;
class EThread {
public:
    // 0-7
    enum Type : Moer::int32 {
        EMainThread,
        ERenderThread,
        NamedThreadCount,
        EAnyThread,
        INDEX_MASK          = 0xff,
        UNKNOWN_THREAD      = 0xff,
        LOCAL_QUEUE         = 0x100,
        QUEUE_MASK_SHEFT    = 8,
        MAIN_QUEUE          = 0x0,
        HIGH_PRI            = 0x000,
        NORMAL_PRI          = 0x200,
        LOW_PRI             = 0x400,
        PRIORITY_SHEFT      = 9,
        PRIORITY_MASK       = 0x600, //8-10 for priority
        EGameThread_local   = EMainThread | LOCAL_QUEUE,
        ERenderThread_local = ERenderThread | LOCAL_QUEUE,
        PriorityCount       = 3,
        AnyThread_HighPri   = UNKNOWN_THREAD | HIGH_PRI,
        AnyThread_NormalPri = UNKNOWN_THREAD | NORMAL_PRI,
        AnyThread_LowPri    = UNKNOWN_THREAD | LOW_PRI,
        Invalid             = 0x7fffffff
    };
    static ThreadIndex GetThreadIndex(EThread::Type type) {
        return type & INDEX_MASK;
    }
    static QueueIndex GetQueueIndex(EThread::Type type) {
        return (type & LOCAL_QUEUE) >> QUEUE_MASK_SHEFT;
    }
    static ThreadPriority GetThreadPriority(EThread::Type type) {
        return (type & PRIORITY_MASK) >> PRIORITY_SHEFT;
    }
    static bool IsUnKnownThread(EThread::Type type) {
        return (type & INDEX_MASK) == UNKNOWN_THREAD;
    }
    static EThread::Type SetPriority(EThread::Type type, ThreadPriority priority) {
        assert(priority <= LOW_PRI);
        return Type((type & ~PRIORITY_MASK) | (priority & PRIORITY_MASK));
    }
    static EThread::Type SetThreadIndex(EThread::Type type, ThreadIndex index) {
        assert(index < INDEX_MASK);
        return Type((type & ~INDEX_MASK) | (INDEX_MASK & index));
    }
};
std::string GetPriorityStr(Moer::int32 priority);
//thread executing structure
class Runnable;
//actual thread
class RunnableThread;
//thread manager for runnable thread registration
class CORE_API ThreadManager {
    friend class TaskGraph;

public:
    ~ThreadManager();

private:
    static Moer::uint32 g_game_thread_id;
    static Moer::uint32 g_render_thread_id;
    ThreadManager() {
        Initialize();
    }
    void Initialize();

public:
    void               AddThread(Moer::uint32 id, RunnableThread*) noexcept;
    void           RemoveThread(RunnableThread*);
    inline Moer::int32 GetNum() {
        return m_threads.size();
    }
    void                  Tick();
    static ThreadManager& Instance();
    static const char*    GetThreadName(Moer::uint32 id);
    static void           SetGameThreadID(Moer::uint32 _game_thread_id);
    static void           SetRenderThreadID(Moer::uint32 _render_thread_id);
    static Moer::uint32   GetRenderThreadID();
    static Moer::uint32   GetGameThreadID();
    static Moer::uint32   GetCurrentThreadID();
    static Moer::uint32   GetCurrentThreadIndex();

private:
    void                                 ShutDown();
    RunnableThread*                          GetRunnableThread(Moer::uint32 id);
    Moer::Map<Moer::uint32, RunnableThread*> m_threads;
    Moer::Map<Moer::uint32, Moer::uint32>    m_thread_indexs;
    const char*                              GetRunnableThreadName(Moer::uint32 id);
};

struct ThreadAttributes {
    Affinity         affinity;
    std::string_view name;
};

class RunnableThread {
    friend class ThreadManager;
    friend class TaskGraph;

public:
    CORE_API static RunnableThread* Create(Runnable* _runnable, ThreadAttributes _attributes);
    CORE_API virtual ~RunnableThread();
    void Tick();
    void Join() {
        if (m_thread && m_thread->joinable()) {
            m_thread->join();
            delete m_thread;
            m_thread = nullptr;
        }
    }
    void Detach() {
        m_thread->detach();
    }
    bool Joinable() {
        return m_thread->joinable();
    }
    CORE_API void WaitUntilFinished();

    inline const std::string& GetName() {
        return name;
    }

protected:
    void Setup(Moer::uint64 affinity);
    void SetAffinity(Affinity&& _affinity);
    void SetName(std::string_view _name);
    RunnableThread(Runnable*, ThreadAttributes _attributes);
    Moer::uint32 Run();

private:
    Runnable*    m_runnable     = nullptr;
    Event*       m_create_event = nullptr;
    Event*       m_end_event    = nullptr;
    Moer::uint32 id             = 0;
    std::string  name;
    std::thread* m_thread = nullptr;
};

class Runnable {
public:
    virtual Moer::uint32 Run()      = 0;
    virtual void        Init()     = 0;
    virtual void        Stop()     = 0;
    virtual void        Exit()     = 0;
    virtual ThreadIndex GetIndex() = 0;

protected:
    bool m_stop;
};

class TestRunnanble : public Runnable {
public:
    virtual Moer::uint32 Run() override;
    virtual void        Init() override;
    virtual void        Stop() override;
    virtual void        Exit() override;
    virtual ThreadIndex GetIndex() override {
        return 0;
    }
};
#endif // !THREAD_MANAGER_H
