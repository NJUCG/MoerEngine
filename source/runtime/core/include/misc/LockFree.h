#ifndef MOER_LOCK_FREE_H
#define MOER_LOCK_FREE_H
#include <atomic>
#include <functional>
#include <stdalign.h>
#include <stdint.h>
#include "API_Macro.h"
#include "log/LogSystem.h"
#include "misc/Alignment.h"
#include "misc/MMemory.h"
#include "misc/MacroUtils.h"
#include "misc/STL.h"

#include "platform/Platform.h"

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
    ABADoublePtr(const ABADoublePtr& other) : value{other.value.load()} {
    }
    ABADoublePtr() : value{0} {}

    uint64_t GetState() {
        return (value & s_state_mask) >> max_ptr_bit_count;
    }
    uint32_t GetValue() {
        return value & s_ptr_mask;
    }
    uint64_t GetTag() {
        return (value & s_tag_mask) >> s_tag_offset;
    }
    static uint64_t GetTagFromState(uint64_t _state) {
        return (_state & s_tag_mask_in_state) >> s_tag_offset_in_state;
    }
    void SetAll(uint32_t _ptr, uint64_t _state) {
        value = _ptr | (_state << max_ptr_bit_count);
    }
    void SetValue(uint32_t _ptr) {
        SetAll(_ptr, GetState());
    }
    void SetTag(uint64_t _tag) {
        SetState((_tag << s_tag_offset_in_state) | GetState());
    }
    static uint64_t SetTagInState(uint64_t _state, uint64_t _tag) {
        return ((_tag << s_tag_offset_in_state) & s_tag_mask_in_state) | (_state & (~s_tag_mask_in_state));
    }
    void SetState(uint64_t _state) {
        SetAll(GetValue(), _state);
    }
    void AdvanceCounter(uint64_t _increment) {
        uint64_t state = GetState();
        //avoid overflow
        SetState(((state + _increment) & s_counter_mask_in_state) | (state & (~s_counter_mask_in_state)));
    }
    static uint64_t AdvanceStateCounter(uint64_t _state, uint64_t _increment) {
        return ((_state + _increment) & s_counter_mask_in_state) | (_state & (~s_counter_mask_in_state));
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

/**
 * @brief lock-free Fixed size allocator without pre-thread cache
 * when allocate, it will allocate a block of memory of 1 << 18 bytes
 * it only allocate and don't free until quit
 * @tparam T 
 * @tparam MAX_ITEM_COUNT 
 * @tparam ITEM_PER_BLOCK 
 */
template<class T, uint32_t MAX_ITEM_COUNT, uint32_t ITEM_PER_BLOCK>
class CORE_API LockFreeIndexedAllocator {
    constexpr static uint32_t s_max_block_count = (MAX_ITEM_COUNT + ITEM_PER_BLOCK - 1) / ITEM_PER_BLOCK;

public:
    LockFreeIndexedAllocator() {
        index_counter++;
        for (uint32_t i = 0; i < s_max_block_count; i++) {
            m_blocks[i] = 0;
        }
    }
    FORCEINLINE uint32_t Allocate(uint32_t count = 1) {
        uint32_t index = index_counter.fetch_add(count);
        if (index + count > MAX_ITEM_COUNT) {
            assert(false && "index out of range, no more space to allocate");
        }
        for (uint32_t i = 0; i < count; i++) {
            void* newed_address = GetPayLoad(index + i);
            new (newed_address) T();
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
        return (T*)(m_blocks[block_index].load()) + item_index;
    }

private:
    void* GetPayLoad(uint32_t index) {
        uint32_t block_index = index / ITEM_PER_BLOCK;
        uint32_t item_index  = index % ITEM_PER_BLOCK;
        assert(index < index_counter.load() && index < MAX_ITEM_COUNT && block_index < s_max_block_count && "index out of range");
        if (!m_blocks[block_index]) {
            T* new_block = (T*)Memory::Malloc(ITEM_PER_BLOCK * sizeof(T));
            assert(Moer::IsAligned(new_block, alignof(T)));
            uint64_t zero = 0;
            if (m_blocks[block_index].compare_exchange_strong(zero, (uintptr_t)new_block)) {
                //swap success
                assert(m_blocks[block_index] != 0 && "block should not be null");
            } else {
                //swap failed means other thread has allocated new block and attached to position,free current new block
                assert(m_blocks[block_index] != (uintptr_t)new_block && "block should not be null");
                Memory::Free(new_block, ITEM_PER_BLOCK * sizeof(T));
            }
        }
        T*       target_address = (T*)(m_blocks[block_index].load()) + item_index;
        uint32_t size           = sizeof(T);

        return (T*)(m_blocks[block_index].load()) + item_index;
    }
    std::atomic_uint64_t index_counter{0};
    //per-block contains ITEM_PER_BLOCK Ts
    std::atomic<uintptr_t> m_blocks[s_max_block_count];
};
#define MAX_LOCK_FREE_NODE_COUNT     ((1ull << 31) - 1)
#define MAX_LOCK_FREE_NODE_BIT_COUNT (32)

enum class LockFreeNodeState : uint32_t {
    Free    = 0,
    SetFree = 1,
    Used    = 2,
};
struct CORE_API LockFreeNode {

    LockFreeNode() : next_double{0}, next_single{0} {
    }
    // union {
    ABADoublePtr next_double;//double index for queue, because we need to change both head&head-next or tail&tail-next
    // };
    uint64_t    data = 0;
    uint32_t    next_single;//single index for stack, because we only need to change head
    inline void SetData(uint64_t _data) {
        {
            data = _data;
        }
    }
};
struct LockFreeNodeStrategy {
    using TNodeIndex = int32_t;
    using TNode      = LockFreeNode;

    static constexpr uint32_t s_node_per_block = (1 << 18) / sizeof(LockFreeNode);//16KB

    using TAllocator = LockFreeIndexedAllocator<LockFreeNode, MAX_LOCK_FREE_NODE_COUNT, s_node_per_block>;

    static FORCEINLINE LockFreeNode* GetNode(TNodeIndex index) {
        return GetAllocator().Get(index);
    }

    CORE_API static TNodeIndex AllocateNodeIndex();
    CORE_API static void       FreeNodeIndex(TNodeIndex index);

    CORE_API static TAllocator& GetAllocator();

    // static LockFreeIndexedAllocator<LockFreeNode, MAX_LOCK_FREE_NODE_COUNT, s_node_per_block> fix_size_allocator;
};

template<typename T, bool SmallEnough>
struct InlineValue {
};

template<typename T>
struct InlineValue<T, false> {
    using TValue                   = T*;
    static constexpr T* zero_value = nullptr;
};

template<typename T>
struct InlineValue<T, true> {
    using TValue                  = T;
    static constexpr T zero_value = T{};
};

/**
 * @brief lock-free allocator for LockFreeNode
 * 
 * @tparam Padding, avoid cacheline contention
 */
template<uint32_t Padding>
class LockFreeNodeStack {
    using TNodeIndex = LockFreeNodeStrategy::TNodeIndex;
    using TNode      = LockFreeNodeStrategy::TNode;

public:
    void Reset() {
        m_head.SetAll(0, 0);
    }
    void Push(TNodeIndex index) {
        ABADoublePtr local_head;
        ABADoublePtr new_head;
        do {
            local_head = m_head;
            new_head   = local_head;
            new_head.AdvanceCounter(1);
            new_head.SetValue(index);
            LockFreeNodeStrategy::GetNode(index)->next_single = local_head.GetValue();
        } while (!m_head.AtomicCompareExchangeWeak(local_head, new_head, std::memory_order_acq_rel));
    }

    bool PushIf(std::function<TNodeIndex(uint64_t)> allocate_if_true) {
        ABADoublePtr local_head;
        ABADoublePtr new_head;
        do {
            local_head       = m_head;
            TNodeIndex index = allocate_if_true(ABADoublePtr::AdvanceStateCounter(local_head.GetState(), 1ull));
            if (index == 0) return false;

            new_head = local_head;
            new_head.AdvanceCounter(1);
            LockFreeNodeStrategy::GetNode(index)->next_single = local_head.GetValue();
            new_head.SetValue(index);
        } while (!m_head.AtomicCompareExchangeWeak(local_head, new_head, std::memory_order_acq_rel));
        return true;
    }

    TNodeIndex Pop() {
        ABADoublePtr local_head;
        ABADoublePtr new_head;
        TNodeIndex   result = 0;
        TNode*       node   = nullptr;
        do {
            local_head = m_head;
            result     = local_head.GetValue();
            if (result == 0) return 0;
            new_head = local_head;
            new_head.AdvanceCounter(1);
            node = LockFreeNodeStrategy::GetNode(result);
            new_head.SetValue(node->next_single);
        } while (!m_head.AtomicCompareExchangeWeak(local_head, new_head, std::memory_order_acq_rel));
        // if (result != 0) {
        node->next_single = 0;
        // }

        return result;
    }

    // TNodeIndex PopAll() {
    //     ABADoublePtr local_head;
    //     ABADoublePtr new_head;
    //     do {
    //         local_head       = m_head;
    //         TNodeIndex index = local_head.GetValue();
    //         if (index == 0) return 0;
    //         new_head = local_head;
    //         new_head.AdvanceCounter(1);
    //         new_head.SetValue(0);
    //     } while (!m_head.AtomicCompareExchangeWeak(local_head, new_head, std::memory_order_acq_rel));
    //     return local_head.GetValue();
    // }

    TNodeIndex PopAllAndChangeTag(std::function<uint64_t(uint64_t)> func_change_state) {
        ABADoublePtr local_head;
        ABADoublePtr new_head;
        TNodeIndex   index = 0;
        do {
            local_head = m_head;
            index      = local_head.GetValue();
            new_head   = local_head;
            new_head.AdvanceCounter(1);
            new_head.SetValue(0);
            new_head.SetState(func_change_state(ABADoublePtr::AdvanceStateCounter(local_head.GetState(), 1ull)));
        } while (!m_head.AtomicCompareExchangeWeak(local_head, new_head, std::memory_order_acq_rel));
        return index;
    }

    FORCEINLINE uint64_t GetState() const {
        ABADoublePtr local_head = m_head;
        return local_head.GetState();
    }

private:
    alignas(Padding) ABADoublePtr m_head{0};
};

template<class T, bool InlineType, uint32_t Padding>
class LockFreeStackBase {
    using TNodeIndex                 = LockFreeNodeStrategy::TNodeIndex;
    using TNode                      = LockFreeNodeStrategy::TNode;
    static constexpr bool use_inline = InlineType && sizeof(T) < sizeof(uint64_t);

public:
    using TValue        = InlineValue<T, use_inline>::TValue;
    LockFreeStackBase() = default;
    ~LockFreeStackBase() {
        while (Pop() != nullptr) {}
    }
    void Push(TValue _payload) {
        TNodeIndex index = LockFreeNodeStrategy::AllocateNodeIndex();

        LockFreeNode* node = LockFreeNodeStrategy::GetNode(index);

        node->SetData(uint64(_payload));
        node_list.Push(index);
    }
    bool PushIf(TValue _payload, std::function<bool(uint64_t)> push_if_true) {
        TNodeIndex index = 0;

        auto allocate_node_if_true = [&index, _payload, &push_if_true](uint64_t state) {
            if (push_if_true(state)) {
                if (index == 0) {
                    index              = LockFreeNodeStrategy::AllocateNodeIndex();
                    LockFreeNode* node = LockFreeNodeStrategy::GetNode(index);
                    node->SetData(uint64_t(_payload));
                }
                return index;
            }
            return 0;
        };
        if (!node_list.PushIf(allocate_node_if_true)) {
            if (index) {
                LockFreeNodeStrategy::FreeNodeIndex(index);
            }
            return false;
        }
        return true;
    }

    TValue Pop() {
        TNodeIndex index = node_list.Pop();
        if (index == 0) return nullptr;

        LockFreeNode* node = LockFreeNodeStrategy::GetNode(index);

        TValue data = (TValue)node->data;
        LockFreeNodeStrategy::FreeNodeIndex(index);

        return data;
    }

    void PopAll(Moer::Array<TValue>& _target) {
        TNodeIndex index = node_list.PopAll();
        while (index != 0) {
            LockFreeNode* node = LockFreeNodeStrategy::GetNode(index);
            _target.push_back((TValue)node->data);
            TNodeIndex current_node_index = index;

            index = node->next_single;
            LockFreeNodeStrategy::FreeNodeIndex(current_node_index);
        }
    }

    void PopAllAndChangeTag(Moer::Array<TValue>& _target, std::function<uint64_t(uint64_t)> func_change_state) {
        TNodeIndex index = node_list.PopAllAndChangeTag(func_change_state);
        while (index != 0) {
            LockFreeNode* node = LockFreeNodeStrategy::GetNode(index);
            _target.push_back((TValue)node->data);
            TNodeIndex current_node_index = index;

            index = node->next_single;
            LockFreeNodeStrategy::FreeNodeIndex(current_node_index);
        }
    }

    uint64_t GetState() const {
        return node_list.GetState();
    }

private:
    LockFreeNodeStack<Padding> node_list;
};

template<class T, bool InlineType = false, uint32_t Padding = PLATFORM_CACHELINE_SIZE>
class LockFreeQueueBase {
    using TNodeIndex                 = LockFreeNodeStrategy::TNodeIndex;
    using TNode                      = LockFreeNodeStrategy::TNode;
    static constexpr bool use_inline = InlineType && sizeof(T) < sizeof(uint64_t);

public:
    using TValue                       = InlineValue<T, use_inline>::TValue;
    static constexpr TValue zero_value = InlineValue<T, use_inline>::zero_value;
    LockFreeQueueBase() {
        m_head.SetAll(0, 0);
        m_tail.SetAll(0, 0);
        TNodeIndex avoid_zero = LockFreeNodeStrategy::AllocateNodeIndex();
        m_head.SetValue(avoid_zero);
        m_tail.SetValue(avoid_zero);
    };
    ~LockFreeQueueBase() {
        while (Pop() != zero_value) {
        }
        //delete avoid zero node stub
        LockFreeNodeStrategy::FreeNodeIndex(m_head.GetValue());
    }
    void Push(TValue _payload) {
        //use fetch and add
        TNodeIndex index = LockFreeNodeStrategy::AllocateNodeIndex();

        LockFreeNodeStrategy::GetNode(index)->SetData(uint64_t(_payload));

        ABADoublePtr local_tail;
        ABADoublePtr new_tail;
        ABADoublePtr local_next;
        ABADoublePtr new_next;

        //a thread must set their own node to some node's next
        //a thread optionally helps other thread to set tail when they find next has changed by other thread
        while (true) {
            local_tail                    = m_tail;
            LockFreeNode* local_tail_node = LockFreeNodeStrategy::GetNode(local_tail.GetValue());

            local_next = local_tail_node->next_double;

            ABADoublePtr test_conflict_tail = m_tail;

            if (test_conflict_tail == local_tail) {

                //1.1 next has not changed, update next node
                if (local_next.GetValue() == 0) {
                    new_next = local_next;
                    new_next.AdvanceCounter(1);
                    new_next.SetValue(index);
                    if (local_tail_node->next_double.AtomicCompareExchangeWeak(local_next, new_next, std::memory_order_acq_rel)) {
                        break;
                    }
                }
                //1.2 next has changed after fetching tail(changed by other thread), update tail to changed next-value(abandon push in this loop and help other thread)
                else {
                    new_tail = local_tail;
                    new_tail.AdvanceCounter(1);
                    new_tail.SetValue(local_next.GetValue());
                    m_tail.AtomicCompareExchangeWeak(local_tail, new_tail, std::memory_order_acq_rel);
                }
            }
        }
        //if no other thread's helping current thread to update tail, update tail to current node
        //hint: when failed update tail below, it means some NEXT has been set to break the loop
        //and there must be some other thread set tail for current thread(because tail has changed)
        {
            new_tail = local_tail;
            new_tail.AdvanceCounter(1);
            new_tail.SetValue(index);
            m_tail.AtomicCompareExchangeWeak(local_tail, new_tail, std::memory_order_acq_rel);
        }
    }

    TValue Pop() {
        ABADoublePtr local_head;
        ABADoublePtr new_head;
        ABADoublePtr local_tail;
        ABADoublePtr new_tail;
        ABADoublePtr local_next;
        ABADoublePtr new_next;

        TValue result = zero_value;

        while (true) {
            local_head = m_head;
            local_tail = m_tail;

            //because there's a stub node at the head, local_head_node cannot be nullptr
            LockFreeNode* local_head_node = LockFreeNodeStrategy::GetNode(local_head.GetValue());
            local_next                    = local_head_node->next_double;

            ABADoublePtr test_conflict_head = m_head;

            if (test_conflict_head == local_head) {
                //if head is the last node, the queue may be pushing or its empty
                if (local_head.GetValue() == local_tail.GetValue()) {
                    //empty return null
                    if (local_next.GetValue() == 0) {
                        return zero_value;
                    }
                    //next has been pushed, help to update tail
                    new_tail = local_tail;
                    new_tail.AdvanceCounter(1);
                    new_tail.SetValue(local_next.GetValue());
                    m_tail.AtomicCompareExchangeWeak(local_tail, new_tail, std::memory_order_acq_rel);

                } else {
                    //pop current node
                    result   = (TValue)LockFreeNodeStrategy::GetNode(local_next.GetValue())->data;
                    new_head = local_head;
                    new_head.AdvanceCounter(1);
                    new_head.SetValue(local_next.GetValue());

                    if (m_head.AtomicCompareExchangeWeak(local_head, new_head, std::memory_order_acq_rel)) {
                        break;
                    }
                }
            }
        }
        LockFreeNodeStrategy::FreeNodeIndex(local_head.GetValue());
        return result;
    }

    void PopAll(Moer::Array<TValue>& _target) {
        while (TValue item = Pop()) {
            _target.push_back(item);
        }
    }

private:
    alignas(Padding) ABADoublePtr m_head;
    alignas(Padding) ABADoublePtr m_tail;
};

template<class T, bool InlineType = false, uint32_t Padding = PLATFORM_CACHELINE_SIZE>
class ClosableLockFreeMpScStack : public LockFreeStackBase<T, InlineType, Padding> {
    using TValue = LockFreeStackBase<T, InlineType, Padding>::TValue;

public:
    ClosableLockFreeMpScStack() : LockFreeStackBase<T, InlineType, Padding>() {
    }
    ClosableLockFreeMpScStack(ClosableLockFreeMpScStack const&)            = delete;
    ClosableLockFreeMpScStack& operator=(ClosableLockFreeMpScStack const&) = delete;
    // void Reset(){

    // }
    bool TryPush(TValue _payload) {
        return LockFreeStackBase<T, InlineType, Padding>::PushIf(_payload, [](uint64_t State) { return (ABADoublePtr::GetTagFromState(State) & 1) == 0; });
    }
    bool IsClosed() const {
        return ABADoublePtr::GetTagFromState(LockFreeStackBase<T, InlineType, Padding>::GetState()) & 1;
    }
    void ComsumeAllAndClose(Moer::Array<TValue>& target) {
        LockFreeStackBase<T, InlineType, Padding>::PopAllAndChangeTag(target,
                                                                      [](uint64_t State) {
                                                                          return ABADoublePtr::SetTagInState(State, 1);
                                                                      });
    }

private:
    std::mutex test_mutex;
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
        ABADoublePtr local_state;
        ABADoublePtr new_state;
        int32_t      possible_thread = -1;
        m_queue[index].Push(task);
        do {
            local_state       = state;
            uint32_t possible = local_state.GetValue();
            possible_thread   = GetFirstOne(possible);

            new_state = local_state;
            new_state.AdvanceCounter(1);
            if (possible_thread >= 0) {
                uint32_t thread_mask = ~(uint32_t(1u << (32 - possible_thread - 1)));

                new_state.SetValue(local_state.GetValue() & thread_mask);
            } else {
                new_state.SetValue(local_state.GetValue());
            }
        } while (!state.AtomicCompareExchangeWeak(local_state, new_state, std::memory_order_acq_rel));
        return possible_thread;
    }

    T* Pop(int32_t index, bool allowHang = true) {
        ABADoublePtr local_state;
        ABADoublePtr new_state;

        T* result = nullptr;
        do {
            local_state = state;
            for (int32_t i = 0; i < TaskPriorityCount; i++) {
                result = m_queue[i].Pop();
                if (result != nullptr) {
                    do {
                        local_state = state;
                        new_state   = local_state;
                        new_state.AdvanceCounter(1);

                    } while (!state.AtomicCompareExchangeWeak(local_state.value, new_state.value, std::memory_order_acq_rel));
                    return result;
                }
            }
            if (!allowHang) break;
            new_state = local_state;
            new_state.AdvanceCounter(1);
            auto new_ptr = local_state.GetValue() | (1ul << (32 - index - 1));
            if (allowHang) new_state.SetValue(new_ptr);

        } while (!state.AtomicCompareExchangeWeak(local_state.value, new_state.value, std::memory_order_acq_rel));
        return result;
    }

private:
    LockFreeQueueBase<T, false, PLATFORM_CACHELINE_SIZE> m_queue[TaskPriorityCount];

    ABADoublePtr state;
};
#endif// !ASYNC_QUEUE_H
