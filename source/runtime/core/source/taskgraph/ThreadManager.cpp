#include "taskgraph/ThreadManager.h"
#include <assert.h>
#include <thread>
#include <functional>
#include <algorithm>
#include <iostream>
#include "spdlog/spdlog.h"
#include "platform/Platform.h"

uint32_t ThreadManager::g_gameThreadID = 0;

std::string getPriorityStr(int32_t priority) {
	assert(priority < EThread::PriorityCount);
	priority = priority << EThread::PRIORITY_SHEFT;
	switch (priority)
	{
	case EThread::HIGH_PRI :
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
	shutDown();
}
void ThreadManager::AddThread(uint32_t id, RunnableThread* thread)
{
	if (m_threads.find(id)==m_threads.end())m_threads.emplace(id, thread);
}

void ThreadManager::RemoveThread(RunnableThread* thread)
{
	assert(thread != nullptr);
	auto target = m_threads.find(thread->id);
	if (target != m_threads.end())m_threads.erase(target);
}

void ThreadManager::tick()
{
}
uint32_t ThreadManager::getCurrentThreadID() {
	return Platform::GetCurrentThreadID();
}

ThreadManager& ThreadManager::Instance()
{
	static ThreadManager singleton;
	// TODO: 在此处插入 return 语句
	return singleton;
}

void ThreadManager::shutDown()
{
	for (auto i = m_threads.begin(); i != m_threads.end(); i++)
	{
		if (i->second != nullptr) {
			i->second = nullptr;
		}
		
	}
}

std::string ThreadManager::getRunnableThreadName(uint32_t id)
{
	auto target = m_threads.find(id);
	if (target != m_threads.end())return target->second->name;
	return "NameUnknown";
}

RunnableThread* ThreadManager::getRunnableThread(uint32_t thread_id) {
	if (thread_id == g_gameThreadID) return nullptr;
	auto thread = m_threads.at(thread_id);
	return thread;
}

void RunnableThread::setup(uint64_t affinity) {
	Platform::SetThreadAffinity(m_thread->native_handle(), affinity);
}

RunnableThread* RunnableThread::create(Runnable* runnable, std::string name, uint64_t affinity)
{
	
	RunnableThread* createdThread = nullptr;
	
	
	createdThread = new RunnableThread(runnable, name);
	createdThread->setup(affinity);
	ThreadManager::Instance().AddThread(createdThread->id, createdThread);
	return createdThread;
}

void RunnableThread::tick()
{
}

RunnableThread::RunnableThread(Runnable* inRunnable, std::string name)
{
	assert(inRunnable != nullptr);
	m_runnable = inRunnable;
	m_createEvent = EventPool::get()->getEvent(false);
	m_endEvent = EventPool::get()->getEvent(false);
	EventRef createEvent(m_createEvent);
	m_thread = new std::thread(&RunnableThread::run, this);
	createEvent.wait();
	this->name = name;
	SPDLOG_INFO("[{}] {} thread created", name, this->id);
	
}

uint32_t RunnableThread::run()
{
	assert(m_runnable != nullptr);
	id = Platform::GetCurrentThreadID();
	m_createEvent->trigger();
	m_runnable->init();

	uint32_t exit_code = m_runnable->run();

	m_runnable->exit();
	m_endEvent->trigger();
	return exit_code;
}

void RunnableThread::waitUntilFinished() {
	m_endEvent->wait();
}

uint32_t TestRunnanble::run()
{
	SPDLOG_INFO("[{}] start running", Platform::GetCurrentThreadID());
	while (!m_stop)
	{
		
	}
	SPDLOG_INFO("[{}] finish running", Platform::GetCurrentThreadID());
	return 0;
}

void TestRunnanble::init()
{
}

void TestRunnanble::stop()
{
	m_stop = true;
}

void TestRunnanble::exit()
{
	SPDLOG_INFO("thread {} exit", __threadid());
}
