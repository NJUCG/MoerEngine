#include "taskgraph/Event.h"
#include "platform/Platform.h"
#include <chrono>

Event* EventPool::GetEvent(bool autoReset) {
    Event* target;
    while (!(target = m_pool.Pop())) {
        for (size_t i = 0; i < 10; i++) {
            m_pool.Push(new Event(autoReset));
        }
    }
    target->m_autoReset = autoReset;
    return target;
}

EventPool* EventPool::Get() {
    static EventPool pool;
    return &pool;
}

void EventPool::ReleaseEvent(Event* target) {
    target->OnReset();
    m_pool.Push(target);
}

EventPool::EventPool() {
    for (size_t i = 0; i < 100; i++) {
        m_pool.Push(new Event());
    }
}
EventPool::~EventPool() {}

Event::Event(bool autoReset) : m_signal{0}, m_autoReset{autoReset} {}
void Event::Trigger() {
    std::unique_lock<std::mutex> lock{m_mutex};

    m_signal.store(1, std::memory_order_release);
    m_cond.notify_one();
    lock.unlock();
}

void Event::Wait() {
    std::unique_lock<std::mutex> lock{m_mutex};
    m_cond.wait(lock, [this]() {
        return m_signal.load(std::memory_order_acquire) >= 1;
    });
    if (m_autoReset)
        OnReset();
    lock.unlock();
}

void Event::OnReset() {
    //ResetEvent(_event);
    m_signal.store(0, std::memory_order_release);
    //cycle++;
}
