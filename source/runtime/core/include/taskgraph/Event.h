#ifndef EVENT_H
#define EVENT_H
#include "StatQueue.h"
#include <mutex>
class Event;
class EventPool {
public:
	Event* getEvent(bool autoReset=true);
	static EventPool* get();
	void releaseEvent(Event* target);
private:
	EventPool();
	LockQueue<Event> m_pool;
};
class Event {
	friend class EventPool;
	friend class EventRef;
public:
	
	
	void trigger();
	void wait();
private:
	void onReset();
	std::mutex m_mutex;
	std::condition_variable m_cond;
	Event() :Event(true) {}
	Event(bool autoReset);
	std::atomic<uint32_t> m_signal;
	bool m_autoReset;
	//void* _event;
};

class EventRef {
public:
	EventRef() {
		m_event = EventPool::get()->getEvent();
	}
	EventRef(Event* _event) :m_event{ _event } {}
	~EventRef() {
		EventPool::get()->releaseEvent(m_event);
	}
	void trigger() {
		if (m_event) m_event->trigger();
	}
	void wait() {
		if (m_event) m_event->wait();
	}
private:
	Event* m_event;
};
class ScopeEventRef {
	friend class TaskGraph;
public:
	ScopeEventRef(bool autoReset=true) {
		m_event = EventPool::get()->getEvent(autoReset);
	}
	ScopeEventRef(Event* _event) :m_event(_event) {}
	~ScopeEventRef() {
		m_event->wait();
		EventPool::get()->releaseEvent(m_event);
	}
	void trigger() {
		if (m_event) m_event->trigger();
	}
	void wait() {
		if (m_event) m_event->wait();
	}
private:
	Event* m_event;
};
#endif // !EVENT_H
