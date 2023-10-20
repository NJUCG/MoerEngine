#ifndef EVENT_H
#define EVENT_H
#include "API_Macro.h"
#include <mutex>
#include <condition_variable>
class Event;

template<class T>
class LockQueue;
class EventPool {
public:
    ~EventPool();
    CORE_API Event*            GetEvent(bool autoReset = true);
    CORE_API static EventPool* Get();
    CORE_API void              ReleaseEvent(Event* target);

private:
    EventPool();
    LockQueue<Event>* m_pool;
};
class Event {
    friend class EventPool;
    friend class EventRef;

public:
    CORE_API void Trigger();
    CORE_API void Wait();

private:
    void                    OnReset();
    std::mutex              m_mutex;
    std::condition_variable m_cond;
    Event() : Event(true) {}
    Event(bool autoReset);
    std::atomic<uint32_t> m_signal;
    bool                  m_autoReset;
    //void* _event;
};

class EventRef {
public:
    EventRef() {
        m_event = EventPool::Get()->GetEvent();
    }
    EventRef(Event* _event) : m_event{_event} {}
    ~EventRef() {
        EventPool::Get()->ReleaseEvent(m_event);
    }
    void Trigger() {
        if (m_event) m_event->Trigger();
    }
    void Wait() {
        if (m_event) m_event->Wait();
    }

private:
    Event* m_event;
};
class ScopeEventRef {
    friend class TaskGraph;

public:
    ScopeEventRef(bool autoReset = true) {
        m_event = EventPool::Get()->GetEvent(autoReset);
    }
    ScopeEventRef(Event* _event) : m_event(_event) {}
    ~ScopeEventRef() {
        m_event->Wait();
        EventPool::Get()->ReleaseEvent(m_event);
    }
    void Trigger() {
        if (m_event) m_event->Trigger();
    }
    void Wait() {
        if (m_event) m_event->Wait();
    }

private:
    Event* m_event;
};
#endif// !EVENT_H
