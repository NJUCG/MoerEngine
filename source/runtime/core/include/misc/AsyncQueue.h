#ifndef STAT_QUEUE_H
#define STAT_QUEUE_H
#include <atomic>
#include <mutex>
#include <vector>
#include <deque>
#include "boost/lockfree/stack.hpp"
#include "boost/lockfree/queue.hpp"

template<typename T, uint32_t InitialSize = 1024>
class StatMPSCQueue {
public:
    bool TryPush(T value) {
        return m_queue.push(value);
    }
    bool IsClosed() { return m_queue.isClosed(); }
    void PopAll(std::vector<T>& target) {
        auto func = [&target](T value) { target.push_back(value); };
        m_queue.consume_all_atomic_reversed(func);
    }

private:
    boost::lockfree::closable_stack<T, boost::lockfree::capacity<InitialSize>> m_queue;
};
template<typename T, uint32_t InitialSize = 1024>
class ConsumeAllMPMCQueue {
public:
    bool Push(const T& value) {
        return m_queue.push(value);
    }
    void PopAll(std::vector<T>& target) {
        auto func = [&target](T value) { target.push_back(value); };
        m_queue.consume_all(func);
    }
    template<typename F>
    void ComsumeAll(const F& func) {
        m_queue.consume_all(func);
    }

private:
    boost::lockfree::queue<T, boost::lockfree::capacity<InitialSize>> m_queue;
};

template<typename T>
class LockQueue {
public:
    ~LockQueue() {
    }
    bool Pop(T*& target) {
        std::unique_lock<std::mutex> lk(m_mutex);
        if (m_queue.size() == 0) {
            target = nullptr;
            lk.unlock();
            return false;
        }
        target = m_queue.front();
        m_queue.pop_front();
        lk.unlock();
        return true;
    }

    void Push(T* value) {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_queue.push_back(value);
        lk.unlock();
    }

private:
    std::mutex     m_mutex;
    std::deque<T*> m_queue;
};

typedef uint64_t      l_ptr_t;
typedef uint32_t      ptr_t;
typedef uint16_t      tag_t;
static const uint64_t ptr_mask   = ((1ull << 32) - 1) << 32;
static const uint64_t tag_mask   = 0xffff0000;
static const uint16_t stat_mask  = 3;
static const uint16_t stat_sheft = 2;
static const uint32_t tag_index  = 2;

struct DoublePtr {
public:
    DoublePtr(DoublePtr& other) : value{other.value.load()} {
    }
    DoublePtr() : value{0} {}
    DoublePtr(l_ptr_t val) : value{val} {
    }
    DoublePtr(ptr_t ptr, tag_t tag) : DoublePtr(((0ull | ptr) << 32) | ((0ull | tag) << 16)) {
    }
    DoublePtr(DoublePtr&& other) : value{other.value.load()} {
    }
    DoublePtr& operator=(DoublePtr& other) {
        this->value = other.value.load();
        return *this;
    }
    DoublePtr& operator=(DoublePtr&& other) {
        this->value = other.value.load();
        return *this;
    }
    tag_t get_tag() {

        return (value & tag_mask) >> 16;
        ;
    }
    tag_t get_stat() {
        return get_tag() & stat_mask;
    }
    ptr_t get_ptr() {

        return (value & ptr_mask) >> 32;
    }

    void set_ptr(ptr_t _ptr) {
        l_ptr_t v = _ptr;

        value = v << 32 | value & tag_mask;
    }
    bool compare_exchange_weak(l_ptr_t expected, l_ptr_t desired) {
        return value.compare_exchange_weak(expected, desired);
    }
    bool compare_exchange_strong(l_ptr_t expected, l_ptr_t desired) {
        return value.compare_exchange_strong(expected, desired);
    }
    operator l_ptr_t() {
        return value.load();
    }
    std::atomic<l_ptr_t> value;
};

static tag_t advance_tag(tag_t in_tag) {
    return (((in_tag >> stat_sheft) + 1) << stat_sheft) | in_tag & stat_mask;
}
static DoublePtr advance_ptr(DoublePtr target) {
    return DoublePtr(target.get_ptr(), (((target.get_tag() >> stat_sheft) + 1) << stat_sheft) | target.get_stat());
}
static DoublePtr set_state(DoublePtr target, tag_t state) {

    return DoublePtr((target.get_ptr(), (target.get_tag() & ~tag_mask)) | (state & stat_mask));
}
static DoublePtr advance_ptr_and_state(DoublePtr target, tag_t state) {
    return DoublePtr(advance_ptr(set_state(target, state)));
}

template<typename T>
class LockFreeQueue {

    struct Node {
        friend class LockFreeQueue;

    public:
        Node(T* _value, DoublePtr _next) : value{_value}, next{_next} {}
        void set_next(DoublePtr _next) {
            next = _next;
        }

    private:
        T*        value;
        DoublePtr next;
    };

public:
    LockFreeQueue() : m_head(0, 0), m_tail(0, 0) {
    }
    bool push(T* value) {
        Node new_node(value, 0);
        for (;;) {
            //push to tail  index of tail become 1 if head
            DoublePtr local_tail(m_tail);
            //            int       new_index = local_tail.get_ptr() + 1;
            int       new_index = local_tail.get_ptr() + 1;
            DoublePtr local_head(m_head);
            DoublePtr new_tail(local_head.get_ptr() + 1, advance_tag(local_head.get_tag()));
            Node&     node = pool[local_head.get_ptr() % capacity];
            node.next      = new_index;
            if (local_head.get_ptr() == local_tail.get_ptr()) {
                //empty queue
            }
        }
        return true;
    }

private:
    DoublePtr             m_head;
    DoublePtr             m_tail;
    std::vector<Node>     pool;
    std::atomic<uint32_t> capacity;
};
template<typename T, uint32_t TaskPriorityCount>
class TaskFIFOQueue {

    static int32_t GetFirstOne(uint32_t target) {
        int index = -1;
        for (int i = 0; i < 31; i++) {
            if (target == 0) break;
            index++;
            if (target & (1ul << 31)) {
                break;
            }
            target = target << 1;
        }
        if (index == 32) return -1;
        return index;
    }

public:
    TaskFIFOQueue() : state{0} {};
    int32_t Push(T* task, uint32_t index) {
        DoublePtr local_state;
        DoublePtr new_state;
        int32_t   possibleThread = -1;
        m_queue[index].push(task);

        do {
            local_state       = state;
            uint32_t possible = local_state.get_ptr();
            possibleThread    = GetFirstOne(possible);
            new_state         = advance_ptr(local_state);
            if (possibleThread >= 0) {
                new_state.set_ptr(new_state.get_ptr() & ~(1ul << (32 - possibleThread - 1)));
            }
        } while (!state.compare_exchange_weak(local_state, new_state));
        return possibleThread;
    }

    T* Pop(int32_t index, bool allowHang = true) {
        DoublePtr local_state(state.value.load(std::memory_order_acquire));
        DoublePtr new_state;
        T*        result = nullptr;
        auto      func   = [&result](T* value) { result = value; };

        do {
            for (int32_t i = 0; i < TaskPriorityCount; i++) {
                m_queue[i].consume_one(func);
                if (result != nullptr) {
                    do {
                        local_state = state;
                        new_state   = advance_ptr(local_state);

                    } while (!state.compare_exchange_weak(local_state.value, new_state.value));
                    return result;
                }
            }
            local_state = state;
            new_state   = advance_ptr(local_state);
            if (allowHang) new_state.set_ptr(new_state.get_ptr() | (1ul << (32 - index - 1)));

        } while (!state.compare_exchange_weak(local_state.value, new_state.value));
        return result;
    }

private:
    boost::lockfree::queue<T*, boost::lockfree::capacity<128>> m_queue[TaskPriorityCount];
    DoublePtr                                                  state;
};

#endif// !STAT_QUEUE_H
