#ifndef STAT_QUEUE_H
#define STAT_QUEUE_H
#include <atomic>
#include <functional>
#include <mutex>
#include <stdalign.h>
#include <stdint.h>
#include <vector>
#include <deque>
#include "API_Macro.h"
#include "boost/lockfree/lockfree_forward.hpp"
#include "boost/lockfree/policies.hpp"
#include "boost/lockfree/stack.hpp"
#include "boost/lockfree/queue.hpp"
#include "boost/lockfree/spsc_queue.hpp"
#include "misc/MMemory.h"
#include "misc/MacroUtils.h"
#include "misc/STL.h"

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

/**
 * @brief lock-free Fixed size allocator without pre-thread cache
 * when allocate, it will allocate a block of memory of 1 << 18 bytes
 * it only allocate and don't free until quit
 * @tparam T 
 * @tparam MAX_ITEM_COUNT 
 * @tparam ITEM_PER_BLOCK 
 */
template<class T, uint32_t MAX_ITEM_COUNT, uint32_t ITEM_PER_BLOCK>
class LockFreeIndexedAllocator {
    constexpr static uint32_t s_max_block_count = (MAX_ITEM_COUNT + ITEM_PER_BLOCK - 1) / ITEM_PER_BLOCK;

public:
    LockFreeIndexedAllocator() {
        for (uint32_t i = 0; i < s_max_block_count; i++) {
            m_blocks[i] = nullptr;
        }
    }
    FORCEINLINE uint32_t Allocate(uint32_t count = 1) {
        uint32_t index = index_counter.fetch_add(count);
        if (index + count > MAX_ITEM_COUNT) {
            assert(false && "index out of range, no more space to allocate");
        }
        for (uint32_t i = 0; i < count; i++) {
            new (GetPayLoad(index + i)) T();
        }
        return index;
    }
    FORCEINLINE void Free(uint32_t index, uint32_t count = 1) {
        assert(index < index_counter.load() && index < MAX_ITEM_COUNT && "index out of range");
        for (uint32_t i = 0; i < count; i++) {
            GetPayLoad(index + i)->~T();
        }
    }

    //get without allocate new item
    FORCEINLINE T* Get(uint32_t index) {
        if (!index) return nullptr;
        uint32_t block_index = index / ITEM_PER_BLOCK;
        uint32_t item_index  = index % ITEM_PER_BLOCK;
        assert(index < index_counter.load() && index < MAX_ITEM_COUNT && block_index < s_max_block_count && m_blocks[block_index] && "index out of range");
        return m_blocks[block_index] + item_index;
    }

private:
    void* GetPayLoad(uint32_t index) {
        uint32_t block_index = index / ITEM_PER_BLOCK;
        uint32_t item_index  = index % ITEM_PER_BLOCK;
        assert(index < index_counter.load() && index < MAX_ITEM_COUNT && block_index < s_max_block_count && "index out of range");
        if (!m_blocks[block_index]) {
            T* new_block = (T*)Memory::Malloc(ITEM_PER_BLOCK * sizeof(T));
            assert(Moer::IsAligned(new_block, alignof(T)));
            if (std::atomic_compare_exchange_weak(&m_blocks[block_index], nullptr, new_block)) {
                //swap success
                assert(m_blocks[block_index] != nullptr && "block should not be null");
            } else {
                //swap failed means other thread has allocated new block and attached to position,free current new block
                assert(m_blocks[block_index] != new_block && "block should not be null");
                Memory::Free(ITEM_PER_BLOCK * sizeof(T), new_block);
            }
        }

        return m_blocks[block_index] + item_index;
    }
    std::atomic_uint64_t index_counter{0};
    //per-block contains ITEM_PER_BLOCK Ts
    T* m_blocks[s_max_block_count];
};
#define MAX_LOCK_FREE_NODE_COUNT     ((1ull << 32) - 1)
#define MAX_LOCK_FREE_NODE_BIT_COUNT (32)
struct LockFreeNode {
    ABADoublePtr next;
    void*        data;
    // for LockFreeNodeListLIFO to allocate node ptr
    uint32_t next_index;
};
struct LockFreeNodePolicy {
    using TPtr  = int32_t;
    using TNode = LockFreeNode;

    static constexpr uint32_t s_node_per_block = (1 << 18) / sizeof(LockFreeNode);

    static FORCEINLINE LockFreeNode* GetLink(uint32_t index) {
        return fix_size_allocator.Get(index);
    }

    CORE_API static TPtr AllocatePtr();
    CORE_API static void FreePtr(TPtr index);

    CORE_API static LockFreeIndexedAllocator<LockFreeNode, MAX_LOCK_FREE_NODE_COUNT, s_node_per_block> fix_size_allocator;
};

/**
 * @brief lock-free allocator for LockFreeNode
 * 
 * @tparam Padding, avoid cacheline contention
 */
template<uint32_t Padding>
class LockFreeNodeListLIFO {
    using TPtr  = LockFreeNodePolicy::TPtr;
    using TNode = LockFreeNodePolicy::TNode;

public:
    void Reset() {
        m_head.SetAll(0, 0);
    }
    void Push(TPtr ptr) {
        ABADoublePtr local_head;
        ABADoublePtr new_head;
        do {
            local_head = m_head.AtomicLoad(std::memory_order_acquire);
            new_head   = local_head;
            new_head.AdvanceCounter(1);
            new_head.SetPtr(ptr);
            LockFreeNodePolicy::GetLink(ptr)->next_index = local_head.GetPtr();
        } while (!m_head.AtomicCompareExchangeWeak(local_head, new_head, std::memory_order_acq_rel));
    }

    bool PushIf(std::function<TPtr(uint64_t)> allocate_if_true) {
        ABADoublePtr local_head;
        ABADoublePtr new_head;
        do {
            local_head = m_head.AtomicLoad(std::memory_order_acquire);
            new_head   = local_head;
            new_head.AdvanceCounter(1);
            uint64_t ptr = allocate_if_true(local_head.GetState());
            if (ptr == 0) return false;
            new_head.SetPtr(ptr);
            LockFreeNodePolicy::GetLink(ptr)->next_index = local_head.GetPtr();
        } while (!m_head.AtomicCompareExchangeWeak(local_head, new_head, std::memory_order_acq_rel));
        return true;
    }

    TPtr Pop() {
        ABADoublePtr local_head;
        ABADoublePtr new_head;

        TNode* node = nullptr;
        do {
            local_head = m_head.AtomicLoad(std::memory_order_acquire);
            TPtr ptr   = local_head.GetPtr();
            if (ptr == 0) return 0;
            new_head = local_head;
            new_head.AdvanceCounter(1);
            node = LockFreeNodePolicy::GetLink(ptr);
            new_head.SetPtr(node->next_index);
            if (local_head.GetPtr() == 0) return 0;
            new_head.SetAll(LockFreeNodePolicy::GetLink(local_head.GetPtr())->next.GetPtr(), local_head.GetState() + 1);
        } while (!m_head.AtomicCompareExchangeWeak(local_head, new_head, std::memory_order_acq_rel));
        node->next_index = 0;
        return local_head.GetPtr();
    }

    TPtr PopAll() {
        ABADoublePtr local_head;
        ABADoublePtr new_head;
        do {
            local_head = m_head.AtomicLoad(std::memory_order_acquire);
            TPtr ptr   = local_head.GetPtr();
            if (ptr == 0) return 0;
            new_head = local_head;
            new_head.AdvanceCounter(1);
            new_head.SetPtr(0);
        } while (!m_head.AtomicCompareExchangeWeak(local_head, new_head, std::memory_order_acq_rel));
        return local_head.GetPtr();
    }

    TPtr PopAllAndChangeTag(std::function<uint64_t(uint64_t)> func_change_state) {
        ABADoublePtr local_head;
        ABADoublePtr new_head;
        do {
            local_head = m_head.AtomicLoad(std::memory_order_acquire);
            TPtr ptr   = local_head.GetPtr();
            if (ptr == 0) return 0;
            new_head = local_head;
            new_head.AdvanceCounter(1);
            new_head.SetPtr(0);
            new_head.SetState(func_change_state(local_head.GetState()));
        } while (!m_head.AtomicCompareExchangeWeak(local_head, new_head, std::memory_order_acq_rel));
        return local_head.GetPtr();
    }

    FORCEINLINE uint64_t GetState() {
        ABADoublePtr local_head = m_head.AtomicLoad(std::memory_order_relaxed);
        return local_head.GetState();
    }

private:
    alignas(Padding) ABADoublePtr m_head;
};

template<class T, uint32_t Padding>
class LockFreeListLIFOBase {
    using TPtr  = LockFreeNodePolicy::TPtr;
    using TNode = LockFreeNodePolicy::TNode;

public:
    LockFreeListLIFOBase() = default;
    ~LockFreeListLIFOBase() {
        while (Pop() != nullptr) {}
    }
    void Push(T* _payload) {
        TPtr ptr = LockFreeNodePolicy::AllocatePtr();

        LockFreeNode* node = LockFreeNodePolicy::GetLink(ptr);

        node->data = _payload;
        node_list.Push(ptr);
    }
    bool PushIf(T* _payload, std::function<bool(T*)> allocate_if_true) {
        TPtr ptr = 0;
        if (!node_list.PushIf([&ptr, &_payload, &allocate_if_true](uint64_t state) {
                if (allocate_if_true(_payload)) {
                    if (ptr == 0) {
                        ptr                = LockFreeNodePolicy::AllocatePtr();
                        LockFreeNode* node = LockFreeNodePolicy::GetLink(ptr);
                        node->data         = _payload;
                    }
                    return ptr;
                }
                return 0;
            })) {
            if (ptr) {
                LockFreeNodePolicy::FreePtr(ptr);
            }
            return false;
        }
        return true;
    }

    T* Pop() {
        TPtr ptr = node_list.Pop();
        if (ptr == 0) return nullptr;

        LockFreeNode* node = LockFreeNodePolicy::GetLink(ptr);

        T* data = (T*)node->data;
        LockFreeNodePolicy::FreePtr(ptr);

        return data;
    }

    void PopAll(Moer::Array<T*>& target) {
        TPtr ptr = node_list.PopAll();
        while (ptr != 0) {
            LockFreeNode* node = LockFreeNodePolicy::GetLink(ptr);
            target.push_back((T*)node->data);
            TPtr current_node_index = ptr;

            ptr = node->next_index;
            LockFreeNodePolicy::FreePtr(current_node_index);
        }
    }

    void PopAllAndChangeTag(Moer::Array<T*>& target, std::function<uint64_t(uint64_t)> func_change_state) {
        TPtr ptr = node_list.PopAllAndChangeTag(func_change_state);
        while (ptr != 0) {
            LockFreeNode* node = LockFreeNodePolicy::GetLink(ptr);
            target.push_back((T*)node->data);
            TPtr current_node_index = ptr;

            ptr = node->next_index;
            LockFreeNodePolicy::FreePtr(current_node_index);
        }
    }

    uint64_t GetState() {
        return node_list.GetState();
    }

private:
    LockFreeNodeListLIFO<Padding> node_list;
};

template<class T, uint32_t Padding = 64>
class LockFreeListFIFOBase {
    using TPtr  = LockFreeNodePolicy::TPtr;
    using TNode = LockFreeNodePolicy::TNode;

public:
    LockFreeListFIFOBase() {
        m_head.SetAll(0, 0);
        m_tail.SetAll(0, 0);
        TPtr avoid_zero = LockFreeNodePolicy::AllocatePtr();
        m_head.SetPtr(avoid_zero);
        m_tail.SetPtr(avoid_zero);
    };
    ~LockFreeListFIFOBase() {
        while (Pop() != nullptr) {
            //delete avoid zero node stub
            LockFreeNodePolicy::FreePtr(m_head.GetPtr());
        }
    }
    void Push(T* _payload) {
        //use fetch and add
        TPtr ptr = LockFreeNodePolicy::AllocatePtr();

        LockFreeNodePolicy::GetLink(ptr)->data = _payload;

        ABADoublePtr local_tail;
        ABADoublePtr new_tail;
        ABADoublePtr local_next;
        ABADoublePtr new_next;

        //a thread must set their own node to some node's next
        //a thread optionally helps other thread to set tail when they find next has changed by other thread
        while (true) {
            local_tail                      = m_tail.AtomicLoad(std::memory_order_acquire);
            LockFreeNode* local_tail_node   = LockFreeNodePolicy::GetLink(local_tail.GetPtr());
            local_next                      = local_tail_node->next.AtomicLoad(std::memory_order_acquire);
            ABADoublePtr test_conflict_tail = m_tail.AtomicLoad(std::memory_order_acquire);

            if (test_conflict_tail == local_tail) {

                //1.1 next has not changed, update next node
                if (local_next.GetPtr() == 0) {
                    new_next.SetAll(ptr, local_next.GetState());
                    new_next.AdvanceCounter(1);
                    if (local_tail_node->next.AtomicCompareExchangeWeak(local_next, new_next, std::memory_order_acq_rel)) {
                        break;
                    }
                }
                //1.2 next has changed after fetching tail(changed by other thread), update tail to changed next-value(abandon push in this loop and help other thread)
                else {
                    new_tail.SetAll(local_next.GetPtr(), local_tail.GetState());
                    new_tail.AdvanceCounter(1);
                    m_tail.AtomicCompareExchangeWeak(local_tail, new_tail, std::memory_order_acq_rel);
                }
            }
        }
        //if no other thread's helping current thread to update tail, update tail to current node
        //hint: when failed update tail below, it means some NEXT has been set to break the loop
        //and there must be some other thread set tail for current thread(because tail has changed)
        {
            new_tail.SetAll(ptr, local_tail.GetState());
            new_tail.AdvanceCounter(1);
            m_tail.AtomicCompareExchangeWeak(local_tail, new_tail, std::memory_order_acq_rel);
        }
    }

    T* Pop() {
        ABADoublePtr local_head;
        ABADoublePtr new_head;
        ABADoublePtr local_tail;
        ABADoublePtr new_tail;
        ABADoublePtr local_next;
        ABADoublePtr new_next;
        T*           result = nullptr;
        while (true) {
            local_head                      = m_head.AtomicLoad(std::memory_order_acquire);
            local_tail                      = m_tail.AtomicLoad(std::memory_order_acquire);
            LockFreeNode* local_head_node   = LockFreeNodePolicy::GetLink(local_head.GetPtr());
            local_next                      = local_head_node->next.AtomicLoad(std::memory_order_acquire);
            ABADoublePtr test_conflict_head = m_head.AtomicLoad(std::memory_order_acquire);

            if (test_conflict_head == local_head) {
                //if head is the last node, the queue may be pushing or its empty
                if (local_head.GetPtr() == local_tail.GetPtr()) {
                    //empty return null
                    if (local_next.GetPtr() == 0) {
                        return nullptr;
                    }
                    //next has been pushed, help to update tail
                    new_tail.SetAll(local_next.GetPtr(), local_tail.GetState());
                    new_tail.AdvanceCounter(1);
                    m_tail.AtomicCompareExchangeWeak(local_tail, new_tail, std::memory_order_acq_rel);

                } else {
                    //pop current node
                    result = (T*)local_next.GetPtr();
                    new_head.SetAll(local_next.GetPtr(), local_head.GetState());
                    new_head.AdvanceCounter(1);
                    if (m_head.AtomicCompareExchangeWeak(local_head, new_head, std::memory_order_acq_rel)) {
                        break;
                    }
                }
            }
        }
        LockFreeNodePolicy::FreePtr(local_head.GetPtr());
        return result;
    }

    void PopAll(Moer::Array<T*>& target) {
        while (T* item = Pop()) {
            target.push_back(item);
        }
    }

private:
    ABADoublePtr m_head;
    ABADoublePtr m_tail;
};
#endif// !STAT_QUEUE_H
