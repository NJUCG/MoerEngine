#ifndef STAT_QUEUE_H
#define STAT_QUEUE_H
#include <atomic>
#include <mutex>
#include <stdalign.h>
#include <vector>
#include <deque>
#include "boost/lockfree/lockfree_forward.hpp"
#include "boost/lockfree/policies.hpp"
#include "boost/lockfree/stack.hpp"
#include "boost/lockfree/queue.hpp"
#include "boost/lockfree/spsc_queue.hpp"

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
class SPSCQueue {
public:
    bool Push(const T& value) {
        return m_queue.push(value);
    }
    template<typename F>
    void ComsumeAll(const F& func) {
        m_queue.consume_all(func);
    }

private:
    boost::lockfree::spsc_queue<T, boost::lockfree::capacity<InitialSize>> m_queue;
};

template<typename T, uint32_t InitialSize = 1024>
class ConsumeAllMPMCQueue {
public:
    bool Push(const T& value) {
        return m_queue.push(value);
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

static const uint64_t ptr_mask   = ((1ull << 32) - 1) << 32;
static const uint64_t tag_mask   = 0xffff0000;
static const uint16_t stat_mask  = 3;
static const uint16_t stat_sheft = 2;
static const uint32_t tag_index  = 2;
struct alignas(8) ABADoublePtr {
    static constexpr uint64_t max_ptr_bit_count = 32;
    /**
 * @brief 0xffffffff00000000 for state
 *        0x00000000ffffffff for ptr
 *        0x0ff0000000000000 for tag
 *        0xf000000000000000 for reserved
 *        0x000fffff00000000 for counter 
 */
    static constexpr uint64_t s_tag_offset          = 52;
    static constexpr uint64_t s_tag_offset_in_state = 20;

    static constexpr uint64_t s_state_mask = ~((1ull << max_ptr_bit_count) - 1);

    static constexpr uint64_t s_ptr_mask = (1ull << max_ptr_bit_count) - 1;

    static constexpr uint64_t s_tag_mask = 0xffull << s_tag_offset;

    static constexpr uint64_t s_tag_mask_in_state = 0xffull << s_tag_offset_in_state;

    static constexpr uint64_t s_counter_mask_in_state = 0xfffffull;

    ABADoublePtr(uint64_t _value) : value{_value} {
    }
    ABADoublePtr(ABADoublePtr& other) : value{other.value.load()} {
    }
    ABADoublePtr() : value{0} {}

    uint64_t GetState() {
        return (value & s_state_mask) >> max_ptr_bit_count;
    }
    uint32_t GetPtr() {
        return value & s_ptr_mask;
    }
    uint64_t GetTag() {
        return (value & s_tag_mask) >> s_tag_offset;
    }
    void SetAll(uint32_t _ptr, uint64_t _state) {
        value = _ptr | (_state << max_ptr_bit_count);
    }
    void SetPtr(uint32_t _ptr) {
        value = _ptr | value;
    }
    void SetTag(uint64_t _tag) {
        SetState((_tag << s_tag_offset_in_state) | GetState());
    }
    void SetState(uint64_t _state) {
        SetAll(GetPtr(), _state);
    }
    void AdvanceCounter(uint64_t _increment) {
        uint64_t state = GetState();
        //avoid overflow
        SetState(((state + _increment) & s_counter_mask_in_state) | (state & ~s_counter_mask_in_state));
    }

    uint64_t AtomicLoad(std::memory_order _order) {
        return value.load(_order);
    }
    bool AtomicCompareExchangeWeak(uint64_t _expected, uint64_t _desired, std::memory_order _order) {
        return value.compare_exchange_weak(_expected, _desired, _order);
    }
    bool AtomicCompareExchangeStrong(uint64_t _expected, uint64_t _desired, std::memory_order _order) {
        return value.compare_exchange_strong(_expected, _desired, _order);
    }

    bool AtomicCompareExchangeWeak(const ABADoublePtr& _expected, const ABADoublePtr& _desired, std::memory_order _order) {
        return value.compare_exchange_weak((uint64_t&)_expected.value, _desired.value, _order);
    }
    bool AtomicCompareExchangeStrong(const ABADoublePtr& _expected, const ABADoublePtr& _desired, std::memory_order _order) {
        return value.compare_exchange_strong((uint64_t&)_expected.value, _desired.value, _order);
    }

    bool operator==(ABADoublePtr& other) {
        return value == other.value;
    }
    bool operator!=(ABADoublePtr& other) {
        return value != other.value;
    }

    ABADoublePtr& operator=(const ABADoublePtr& other) {
        this->value = other.value.load();
        return *this;
    }
    std::atomic<uint64_t> value;
};

static_assert(sizeof(ABADoublePtr) == sizeof(uint64_t) && "size of ABADoublePtr is not equal to uint64_t");

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
        ABADoublePtr local_state;
        ABADoublePtr new_state;
        int32_t      possible_thread = -1;
        m_queue[index].push(task);

        do {
            local_state       = state;
            uint32_t possible = local_state.GetPtr();
            possible_thread   = GetFirstOne(possible);

            new_state = local_state;
            new_state.AdvanceCounter(1);
            if (possible_thread >= 0) {
                uint32_t thread_mask = ~(uint32_t(1u << (32 - possible_thread - 1)));

                new_state.SetPtr(new_state.GetPtr() & thread_mask);
            }
        } while (!state.AtomicCompareExchangeWeak(local_state, new_state, std::memory_order_acq_rel));
        return possible_thread;
    }

    T* Pop(int32_t index, bool allowHang = true) {
        ABADoublePtr local_state(state.AtomicLoad(std::memory_order_acquire));
        ABADoublePtr new_state;
        T*           result = nullptr;
        auto         func   = [&result](T* value) { result = value; };

        do {
            for (int32_t i = 0; i < TaskPriorityCount; i++) {
                m_queue[i].consume_one(func);
                if (result != nullptr) {
                    do {
                        local_state = state;
                        new_state.AdvanceCounter(1);

                    } while (!state.AtomicCompareExchangeWeak(local_state.value, new_state.value, std::memory_order_acq_rel));
                    return result;
                }
            }
            local_state = state;

            new_state = local_state;
            new_state.AdvanceCounter(1);
            auto new_ptr = new_state.GetPtr() | (1ul << (32 - index - 1));
            if (allowHang) new_state.SetPtr(new_ptr);

        } while (!state.AtomicCompareExchangeWeak(local_state.value, new_state.value, std::memory_order_acq_rel));
        return result;
    }

private:
    boost::lockfree::queue<T*, boost::lockfree::capacity<128>> m_queue[TaskPriorityCount];
    // DoublePtr                                                  state;

    ABADoublePtr state;
};

#endif// !STAT_QUEUE_H
