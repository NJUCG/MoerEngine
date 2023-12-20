#ifndef THREAD_MANAGER_H
#define THREAD_MANAGER_H
#include "API_Macro.h"
#include "misc/STL.h"
#include <string>
#include <thread>
#include <assert.h>
typedef int32_t ThreadIndex;
typedef int32_t QueueIndex;
typedef int32_t ThreadPriority;
class Event;
class EThread {
public:
    // 0-7
    enum Type : int32_t {
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
        PRIORITY_MASK       = 0x600,//8-10 for priority
        EGameThread_local   = EMainThread | LOCAL_QUEUE,
        ERenderThread_local = ERenderThread | LOCAL_QUEUE,
        PriorityCount       = 3,
        AnyThread_HighPri   = UNKNOWN_THREAD | HIGH_PRI,
        AnyThread_NormalPri = UNKNOWN_THREAD | NORMAL_PRI,
        AnyThread_LowPri    = UNKNOWN_THREAD | LOW_PRI
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
std::string GetPriorityStr(int32_t priority);
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
    static uint32_t g_game_thread_id;
    static uint32_t g_render_thread_id;
    ThreadManager() {
        Initialize();
    }
    void Initialize();

public:
    void                  AddThread(uint32_t id, RunnableThread*);
    void                  RemoveThread(RunnableThread*);
    inline int32_t        GetNum() { return m_threads.size(); }
    void                  Tick();
    static ThreadManager& Instance();
    static const char*    GetThreadName(uint32_t id);
    static void           SetGameThreadID(uint32_t _game_thread_id);
    static void           SetRenderThreadID(uint32_t _render_thread_id);
    static uint32_t       GetRenderThreadID();
    static uint32_t       GetGameThreadID();
    static uint32_t       GetCurrentThreadID();
    static uint32_t       GetCurrentThreadIndex();

private:
    void                                 ShutDown();
    RunnableThread*                      GetRunnableThread(uint32_t id);
    Moer::Map<uint32_t, RunnableThread*> m_threads;
    Moer::Map<uint32_t, uint32_t>        m_thread_indexs;
    const char*                          GetRunnableThreadName(uint32_t id);
};

class RunnableThread {
    friend class ThreadManager;
    friend class TaskGraph;

public:
    CORE_API static RunnableThread* Create(Runnable*          runnable,
                                           const std::string& name,
                                           uint64_t           affinity_mask);
    virtual ~RunnableThread();
    void Tick();
    void Join() {
        if (m_thread->joinable()) m_thread->join();
        m_thread = nullptr;
    }
    void Detach() { m_thread->detach(); }
    bool Joinable() { return m_thread->joinable(); }
    void WaitUntilFinished();

    inline const std::string& GetName() { return name; }

protected:
    void Setup(uint64_t affinity);
    RunnableThread(Runnable*,
                   const std::string& name);
    uint32_t Run();

private:
    Runnable*    m_runnable;
    Event*       m_create_event;
    Event*       m_end_event;
    uint32_t     id;
    std::string  name;
    std::thread* m_thread;
};

class Runnable {
public:
    virtual uint32_t    Run()      = 0;
    virtual void        Init()     = 0;
    virtual void        Stop()     = 0;
    virtual void        Exit()     = 0;
    virtual ThreadIndex GetIndex() = 0;

protected:
    bool m_stop;
};

class TestRunnanble : public Runnable {
public:
    virtual uint32_t    Run() override;
    virtual void        Init() override;
    virtual void        Stop() override;
    virtual void        Exit() override;
    virtual ThreadIndex GetIndex() override { return 0; }
};
#endif// !THREAD_MANAGER_H
