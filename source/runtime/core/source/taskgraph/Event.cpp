#include "taskgraph/Event.h"
#include "taskgraph/StatQueue.h"
#include "spdlog/spdlog.h"
#include "platform/Platform.h"
#include <chrono>

Event* EventPool::getEvent(bool autoReset) {
    Event* target;
    while (!m_pool->pop(target)) {
        for (size_t i = 0; i < 10; i++) {
            m_pool->push(new Event(autoReset));
        }
    }
    target->m_autoReset = autoReset;
    return target;
}

EventPool* EventPool::get() {
    static EventPool pool;
    return &pool;
}

void EventPool::releaseEvent(Event* target) {
    target->onReset();
    m_pool->push(target);
}

EventPool::EventPool() {
    m_pool = new LockQueue<Event>;
    for (size_t i = 0; i < 100; i++) {
        m_pool->push(new Event());
    }
}
EventPool::~EventPool() {
    delete m_pool;
    m_pool = nullptr;
}

Event::Event(bool autoReset) : m_signal{0}, m_autoReset{autoReset} {

    //_event = CreateEventW(nullptr, !autoReset, 0, nullptr);
}
void Event::trigger() {
    std::unique_lock<std::mutex> lock{m_mutex};

    //SPDLOG_ERROR("event triggered");
    m_signal.store(1, std::memory_order_release);
    m_cond.notify_one();
    lock.unlock();
    //SetEvent(_event);
}

void Event::wait() {
    std::unique_lock<std::mutex> lock{m_mutex};
    //SPDLOG_WARN("start waiting {} signal {}", platform::GetCurrentThreadID(), m_signal);

    if (m_signal < 1) {
        //SPDLOG_WARN("actual waiting thread:{}", platform::GetCurrentThreadID());
        m_cond.wait(lock);
    }
    if (m_autoReset) onReset();
    lock.unlock();
    //WaitForSingleObject(_event, MAXDWORD);
}

void Event::onReset() {
    //ResetEvent(_event);
    m_signal.store(0, std::memory_order_release);
    //cycle++;
}
