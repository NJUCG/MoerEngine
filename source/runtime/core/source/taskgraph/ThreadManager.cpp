#include "taskgraph/ThreadManager.h"
#include <assert.h>
#include <thread>
#include <functional>
#include <algorithm>
#include <iostream>
#include "spdlog/details/os.h"
#include "spdlog/spdlog.h"
#include "platform/Platform.h"
#include "taskgraph/Event.h"

uint32_t ThreadManager::g_game_thread_id = 0;
uint32_t ThreadManager::g_render_thread_id = 0;

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
void ThreadManager::AddThread(uint32_t id, RunnableThread* thread) {
    if (m_threads.find(id) == m_threads.end()) m_threads.emplace(id, thread);
}

void ThreadManager::RemoveThread(RunnableThread* thread) {
    assert(thread != nullptr);
    auto target = m_threads.find(thread->id);
    if (target != m_threads.end()) m_threads.erase(target);
}

void ThreadManager::Tick() {
}

void ThreadManager::initialize() {
    g_game_thread_id = Platform::GetCurrentThreadID();
}
uint32_t ThreadManager::GetCurrentThreadID() {
    return Platform::GetCurrentThreadID();
}

ThreadManager& ThreadManager::Instance() {
    static ThreadManager singleton;
    // TODO: �ڴ˴����� return ���
    return singleton;
}

void ThreadManager::ShutDown() {
    for (auto i = m_threads.begin(); i != m_threads.end(); i++) {
        if (i->second != nullptr && i->second->Joinable()) {
            i->second->Join();
        }
    }
}

std::string ThreadManager::GetRunnableThreadName(uint32_t id) {
    auto target = m_threads.find(id);
    if (target != m_threads.end()) return target->second->name;
    return "NameUnknown";
}

RunnableThread* ThreadManager::GetRunnableThread(uint32_t id) {
    if (id == g_game_thread_id) return nullptr;
    auto thread = m_threads.at(id);
    return thread;
}

void RunnableThread::Setup(uint64_t affinity) {
    Platform::SetThreadAffinity((void*)m_thread->native_handle(), affinity);
}

RunnableThread::~RunnableThread() {
    ThreadManager::Instance().RemoveThread(this);
    Join();
}

RunnableThread* RunnableThread::Create(Runnable* runnable, std::string name, uint64_t affinity_mask) {

    RunnableThread* createdThread = nullptr;

    createdThread = new RunnableThread(runnable, name);
    createdThread->Setup(affinity_mask);
    ThreadManager::Instance().AddThread(createdThread->id, createdThread);
    return createdThread;
}

void RunnableThread::Tick() {
}

RunnableThread::RunnableThread(Runnable* inRunnable, std::string name) {
    assert(inRunnable != nullptr);
    m_runnable    = inRunnable;
    m_createEvent = EventPool::Get()->GetEvent(false);
    m_endEvent    = EventPool::Get()->GetEvent(false);
    EventRef createEvent(m_createEvent);
    m_thread = new std::thread(&RunnableThread::Run, this);
    createEvent.Wait();
    this->name = name;
    SPDLOG_INFO("[{}] {} thread created", name, this->id);
}

uint32_t RunnableThread::Run() {
    assert(m_runnable != nullptr);
    id = Platform::GetCurrentThreadID();
    m_createEvent->Trigger();
    m_runnable->Init();

    uint32_t exit_code = m_runnable->Run();

    m_runnable->Exit();
    m_endEvent->Trigger();
    return exit_code;
}

void RunnableThread::WaitUntilFinished() {
    m_endEvent->Wait();
}

uint32_t TestRunnanble::Run() {
    SPDLOG_INFO("[{}] start running", Platform::GetCurrentThreadID());
    while (!m_stop) {
    }
    SPDLOG_INFO("[{}] finish running", Platform::GetCurrentThreadID());
    return 0;
}

void TestRunnanble::Init() {
}

void TestRunnanble::Stop() {
    m_stop = true;
}

void TestRunnanble::Exit() {
    SPDLOG_INFO("thread {} exit", static_cast<size_t>(std::hash<std::thread::id>()(std::this_thread::get_id())));
}
