#include "taskgraph/ThreadManager.h"
#include "platform/Platform.h"
#include "taskgraph/Event.h"
#include <algorithm>
#include <assert.h>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#if defined(PLATFORM_WINDOWS)
#include <windows.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#else
#endif

#define MAIN_THREAD_NAME    "MainThread"
#define RENDER_THREAD_NAME  "RenderThread"
#define UNKNOWN_THREAD_NAME "UnknownThread"

CORE_API uint32_t ThreadManager::g_game_thread_id   = 0;
CORE_API uint32_t ThreadManager::g_render_thread_id = 0;

std::string GetPriorityStr(int32_t priority) {
    assert(priority < EThread::PriorityCount);
    priority = priority << EThread::PRIORITY_SHEFT;
    switch (priority) {
        case EThread::HIGH_PRI:
            return "Pri_High";
        case EThread::NORMAL_PRI:
            return "Pri_Normal";
        default:
            return "Pri_Low";
            break;
    }
    return "";
}
ThreadManager::~ThreadManager() {
    ShutDown();
}
void ThreadManager::AddThread(uint32_t id, RunnableThread* thread) noexcept {
    try {
        if (m_threads.find(id) == m_threads.end()) {
            m_threads.emplace(id, thread);
            m_thread_indexs.emplace(id, m_threads.size() - 1);
        }
    } catch (...) {
        // RunnableThread registration happens only after the OS thread has
        // started; generic Runnable::Stop is not strong enough for rollback.
        Platform::FailFast("RunnableThread registry publication failed");
    }
}

void ThreadManager::RemoveThread(RunnableThread* thread) {
    assert(thread != nullptr);
    auto target = m_threads.find(thread->id);
    if (target != m_threads.end()) {
        m_threads.erase(target);
        m_thread_indexs.erase(thread->id);
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
    Platform::SetCurrentThreadName(MAIN_THREAD_NAME);
    AddThread(g_game_thread_id, nullptr);
}
uint32_t ThreadManager::GetCurrentThreadID() {
    return Platform::GetCurrentThreadID();
}

uint32_t ThreadManager::GetCurrentThreadIndex() {
    return Instance().m_thread_indexs.at(Platform::GetCurrentThreadID());
}

const char* ThreadManager::GetThreadName(uint32_t id) {

    if (id == g_game_thread_id)
        return MAIN_THREAD_NAME;
    if (id == g_render_thread_id)
        return RENDER_THREAD_NAME;
    return Instance().GetRunnableThreadName(id);
}

ThreadManager& ThreadManager::Instance() {
    static ThreadManager singleton;
    return singleton;
}

void ThreadManager::ShutDown() {
    for (auto i = m_threads.begin(); i != m_threads.end(); i++) {
        if (i->second != nullptr && i->second->Joinable()) {
            i->second->Join();
        }
    }
}

const char* ThreadManager::GetRunnableThreadName(uint32_t id) {
    auto target = m_threads.find(id);
    if (target != m_threads.end())
        return target->second->name.c_str();
    return UNKNOWN_THREAD_NAME;
}

RunnableThread* ThreadManager::GetRunnableThread(uint32_t id) {
    if (id == g_game_thread_id)
        return nullptr;
    const auto target = m_threads.find(id);
    return target == m_threads.end() ? nullptr : target->second;
}

void RunnableThread::Setup(uint64_t _affinity) {
    Platform::SetThreadAffinity((void*)m_thread->native_handle(), _affinity);
}

void RunnableThread::SetAffinity(Affinity&& _affinity) {
    Platform::SetCurrentThreadAffinity(std::move(_affinity));
}

void RunnableThread::SetName(std::string_view _name) {
    Platform::SetCurrentThreadName(_name);
    name = _name;
}

RunnableThread::~RunnableThread() {
    ThreadManager::Instance().RemoveThread(this);
    Join();
    if (m_end_event != nullptr) {
        EventPool::Get()->ReleaseEvent(m_end_event);
        m_end_event = nullptr;
    }
}

RunnableThread* RunnableThread::Create(Runnable* _runnable, ThreadAttributes _attributes) {
    // Resolve the registry before starting the OS thread. Once construction
    // succeeds the worker is externally observable, so registration is a
    // process-fatal publication boundary rather than a recoverable exception.
    ThreadManager& manager = ThreadManager::Instance();
    void* storage = Memory::Malloc(sizeof(RunnableThread));
    if (storage == nullptr) {
        throw std::bad_alloc();
    }

    RunnableThread* created_thread = nullptr;
    try {
        created_thread = MoerPlacementNew(storage) RunnableThread(_runnable, _attributes);
    } catch (...) {
        Memory::Free(storage);
        throw;
    }
    manager.AddThread(created_thread->id, created_thread);
    return created_thread;
}

void RunnableThread::Tick() {}

RunnableThread::RunnableThread(Runnable* _in_runnable, ThreadAttributes _attributes) {
    assert(_in_runnable != nullptr);
    m_runnable = _in_runnable;
    name       = _attributes.name;
    std::string thread_name = name;
    Affinity    affinity    = std::move(_attributes.affinity);

    m_create_event = EventPool::Get()->GetEvent(false);
    EventRef create_event(m_create_event);
    m_end_event    = EventPool::Get()->GetEvent(false);

    try {
        m_thread = new std::thread(
            [thread_name = std::move(thread_name), affinity = std::move(affinity), this]() mutable {
                Platform::SetCurrentThreadName(thread_name);
                SetAffinity(std::move(affinity));
                Run();
            }
        );
    } catch (...) {
        EventPool::Get()->ReleaseEvent(m_end_event);
        m_end_event = nullptr;
        throw;
    }

    // No exception may escape after the OS thread has started: the generic
    // Runnable contract cannot guarantee that Stop() can unwind every worker.
    try {
        create_event.Wait();
    } catch (...) {
        Platform::FailFast("RunnableThread startup handshake failed");
    }
    m_create_event = nullptr;

    // SPDLOG_DEBUG("[{}] {} thread created", name, this->id);
}

uint32_t RunnableThread::Run() {
    assert(m_runnable != nullptr);
    id = Platform::GetCurrentThreadID();
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
    SPDLOG_INFO("[{}] start running", Platform::GetCurrentThreadID());
    while (!m_stop) {
    }
    SPDLOG_INFO("[{}] finish running", Platform::GetCurrentThreadID());
    return 0;
}

void TestRunnanble::Init() {}

void TestRunnanble::Stop() {
    m_stop = true;
}

void TestRunnanble::Exit() {
    SPDLOG_INFO(
        "thread {} exit", static_cast<size_t>(std::hash<std::thread::id>()(std::this_thread::get_id()))
    );
}
