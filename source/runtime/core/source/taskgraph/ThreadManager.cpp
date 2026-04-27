#include "taskgraph/ThreadManager.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "taskgraph/Event.h"
#include "trace/Trace.h"
#include <assert.h>
#include <functional>
#include <thread>

#if defined(PLATFORM_WINDOWS)
#include <windows.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#else
#endif

namespace {

constexpr Moer::Utf8StringView main_thread_name{MOER_ASCII_TEXT("MainThread")};
constexpr Moer::Utf8StringView render_thread_name{MOER_ASCII_TEXT("RenderThread")};
constexpr Moer::Utf8StringView unknown_thread_name{MOER_ASCII_TEXT("UnknownThread")};

} // namespace

CORE_API uint32_t ThreadManager::g_game_thread_id   = 0;
CORE_API uint32_t ThreadManager::g_render_thread_id = 0;

Moer::Utf8StringView GetPriorityName(int32_t priority) {
    assert(priority < EThread::PriorityCount);
    priority = priority << EThread::PRIORITY_SHEFT;
    switch (priority) {
        case EThread::HIGH_PRI:
            return Moer::Utf8StringView{MOER_ASCII_TEXT("Pri_High")};
        case EThread::NORMAL_PRI:
            return Moer::Utf8StringView{MOER_ASCII_TEXT("Pri_Normal")};
        default:
            return Moer::Utf8StringView{MOER_ASCII_TEXT("Pri_Low")};
    }
}
ThreadManager::~ThreadManager() {
    ShutDown();
}
void ThreadManager::RegisterThread(uint32_t id, RunnableThread* thread, Moer::Utf8StringView name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_threads.find(id) != m_threads.end()) {
        return;
    }
    m_threads.emplace(id, ThreadInfo{.thread = thread, .name = Moer::Utf8String(name), .index = m_next_thread_index++});
}

void ThreadManager::UnregisterThread(RunnableThread* thread) {
    assert(thread != nullptr);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto target = m_threads.find(thread->id);
    if (target != m_threads.end()) {
        m_threads.erase(target);
    }
}
void ThreadManager::SetGameThreadID(uint32_t _game_thread_id) {
    g_game_thread_id = _game_thread_id;
}

void ThreadManager::SetRenderThreadID(uint32_t _render_thread_id) {
    g_render_thread_id = _render_thread_id;
}
uint32_t ThreadManager::GetRenderThreadID() {
    return g_render_thread_id;
}
uint32_t ThreadManager::GetGameThreadID() {
    return g_game_thread_id;
}
void ThreadManager::Tick() {}

void ThreadManager::Initialize() {
    g_game_thread_id = Platform::GetCurrentThreadID();
    RegisterThread(g_game_thread_id, nullptr, main_thread_name);
    Platform::SetCurrentThreadName(main_thread_name);
    Moer::Trace::SetThreadName(main_thread_name);
}
uint32_t ThreadManager::GetCurrentThreadID() {
    return Platform::GetCurrentThreadID();
}

uint32_t ThreadManager::GetCurrentThreadIndex() {
    ThreadManager& manager = Instance();
    std::lock_guard<std::mutex> lock(manager.m_mutex);
    return manager.m_threads.at(Platform::GetCurrentThreadID()).index;
}

Moer::Utf8String ThreadManager::GetThreadName(uint32_t id) {
    return Instance().GetRunnableThreadName(id);
}

ThreadManager& ThreadManager::Instance() {
    static ThreadManager singleton;
    return singleton;
}

void ThreadManager::ShutDown() {
    Moer::Array<RunnableThread*> threads_to_join{};
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        threads_to_join.reserve(m_threads.size());
        for (auto& [id, info] : m_threads) {
            if (info.thread != nullptr && info.thread->Joinable()) {
                threads_to_join.push_back(info.thread);
            }
        }
    }

    for (RunnableThread* thread : threads_to_join) {
        thread->Join();
    }
}

Moer::Utf8String ThreadManager::GetRunnableThreadName(uint32_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto target = m_threads.find(id);
    if (target != m_threads.end()) {
        return target->second.name;
    }
    if (id == g_render_thread_id) {
        return Moer::Utf8String(render_thread_name);
    }
    if (id == g_game_thread_id) {
        return Moer::Utf8String(main_thread_name);
    }
    return Moer::Utf8String(unknown_thread_name);
}

RunnableThread* ThreadManager::GetRunnableThread(uint32_t id) {
    if (id == g_game_thread_id) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto target = m_threads.find(id);
    return target == m_threads.end() ? nullptr : target->second.thread;
}

void RunnableThread::Setup(uint64_t _affinity) {
    Platform::SetThreadAffinity((void*)m_thread->native_handle(), _affinity);
}

void RunnableThread::SetAffinity(Affinity&& _affinity) {
    Platform::SetCurrentThreadAffinity(std::move(_affinity));
}

void RunnableThread::SetName(Moer::Utf8StringView _name) {
    Platform::SetCurrentThreadName(_name);
    Moer::Trace::SetThreadName(_name);
}

RunnableThread::~RunnableThread() {
    Join();
    ThreadManager::Instance().UnregisterThread(this);
}

RunnableThread* RunnableThread::Create(Runnable* _runnable, ThreadAttributes _attributes) {
    RunnableThread* created_thread = nullptr;
    created_thread                 = MoerNew(RunnableThread)(_runnable, _attributes);
    return created_thread;
}

void RunnableThread::Tick() {}

RunnableThread::RunnableThread(Runnable* _in_runnable, ThreadAttributes _attributes) {
    assert(_in_runnable != nullptr);
    m_runnable     = _in_runnable;
    m_create_event = EventPool::Get()->GetEvent(false);
    m_end_event    = EventPool::Get()->GetEvent(false);
    EventRef create_event(m_create_event);
    Moer::Utf8String thread_name(_attributes.name);
    ThreadManager::Instance();
    m_thread = MoerNew(std::thread)(
        [_in_runnable, name(std::move(thread_name)), affinity(std::move(_attributes.affinity)), this]() {
            id = Platform::GetCurrentThreadID();
            ThreadManager::Instance().RegisterThread(id, this, name);
            SetName(name);
            auto tmp_affinity = affinity;
            SetAffinity(std::move(tmp_affinity));
            Run();
        }
    );
    create_event.Wait();

    LOG_INFO(MOER_TEXT("[{}] {} thread created"), GetName(), this->id);
}

uint32_t RunnableThread::Run() {
    assert(m_runnable != nullptr);
    m_create_event->Trigger();
    m_runnable->Init();

    uint32_t exit_code = m_runnable->Run();

    m_runnable->Exit();
    m_end_event->Trigger();
    return exit_code;
}

void RunnableThread::WaitUntilFinished() {
    m_end_event->Wait();
}

uint32_t TestRunnanble::Run() {
    LOG_INFO(MOER_TEXT("[{}] start running"), Platform::GetCurrentThreadID());
    while (!m_stop) {
    }
    LOG_INFO(MOER_TEXT("[{}] finish running"), Platform::GetCurrentThreadID());
    return 0;
}

void TestRunnanble::Init() {}

void TestRunnanble::Stop() {
    m_stop = true;
}

void TestRunnanble::Exit() {
    LOG_INFO(
        MOER_TEXT("thread {} exit"), static_cast<size_t>(std::hash<std::thread::id>()(std::this_thread::get_id()))
    );
}
