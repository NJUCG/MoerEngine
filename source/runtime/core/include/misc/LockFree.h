#ifndef MOER_LOCK_FREE_H
#define MOER_LOCK_FREE_H

#include "API_Macro.h"
#include "log/LogSystem.h"
#include "misc/Alignment.h"
#include "misc/MMemory.h"
#include "misc/MacroUtils.h"
#include "misc/STL.h"
#include "misc/Types.h"
#include <atomic>
#include <functional>
#include <stdalign.h>
#include <stdint.h>

#include "platform/Platform.h"

static const Moer::uint64 ptr_mask   = ((1ull << 32) - 1) << 32;
static const Moer::uint64 tag_mask   = 0xffff0000;
static const Moer::uint16 stat_mask  = 3;
static const Moer::uint16 stat_sheft = 2;
static const Moer::uint32 tag_index  = 2;
struct alignas(8) ABADoublePtr {
    static constexpr Moer::uint64 max_ptr_bit_count = 32;
    /**
 * @brief 0xffffffff00000000 for state
 *        0x00000000ffffffff for ptr
 *        0x0ff0000000000000 for tag
 *        0xf000000000000000 for reserved
 *        0x000fffff00000000 for counter 
 */
    static constexpr Moer::uint64 s_tag_offset          = 52;
    static constexpr Moer::uint64 s_tag_offset_in_state = 20;

    static constexpr Moer::uint64 s_state_mask = ~((1ull << max_ptr_bit_count) - 1);

    static constexpr Moer::uint64 s_ptr_mask = (1ull << max_ptr_bit_count) - 1;

    static constexpr Moer::uint64 s_tag_mask = 0xffull << s_tag_offset;

    static constexpr Moer::uint64 s_tag_mask_in_state = 0xffull << s_tag_offset_in_state;

    static constexpr Moer::uint64 s_counter_mask_in_state = 0xfffffull;

    ABADoublePtr(Moer::uint64 _value) : value{_value} {}
    ABADoublePtr(const ABADoublePtr& other) : value{other.value.load()} {}
    ABADoublePtr() : value{0} {}

    Moer::uint64 GetState() {
        return (value & s_state_mask) >> max_ptr_bit_count;
    }
    Moer::uint32 GetValue() {
        return value & s_ptr_mask;
    }
    Moer::uint64 GetTag() {
        return (value & s_tag_mask) >> s_tag_offset;
    }
    static Moer::uint64 GetTagFromState(Moer::uint64 _state) {
        return (_state & s_tag_mask_in_state) >> s_tag_offset_in_state;
    }
    void SetAll(Moer::uint32 _ptr, Moer::uint64 _state) {
        value = _ptr | (_state << max_ptr_bit_count);
    }
    void SetValue(Moer::uint32 _ptr) {
        SetAll(_ptr, GetState());
    }
    void SetTag(Moer::uint64 _tag) {
        SetState((_tag << s_tag_offset_in_state) | GetState());
    }
    static Moer::uint64 SetTagInState(Moer::uint64 _state, Moer::uint64 _tag) {
        return ((_tag << s_tag_offset_in_state) & s_tag_mask_in_state) | (_state & (~s_tag_mask_in_state));
    }
    void SetState(Moer::uint64 _state) {
        SetAll(GetValue(), _state);
    }
    void AdvanceCounter(Moer::uint64 _increment) {
        Moer::uint64 state = GetState();
        //avoid overflow
        SetState(((state + _increment) & s_counter_mask_in_state) | (state & (~s_counter_mask_in_state)));
    }
    static Moer::uint64 AdvanceStateCounter(Moer::uint64 _state, Moer::uint64 _increment) {
        return ((_state + _increment) & s_counter_mask_in_state) | (_state & (~s_counter_mask_in_state));
    }

    Moer::uint64 AtomicLoad(std::memory_order _order) {
        return value.load(_order);
    }
    bool AtomicCompareExchangeWeak(Moer::uint64 _expected, Moer::uint64 _desired, std::memory_order _order) {
        return value.compare_exchange_weak(_expected, _desired, _order);
    }
    bool AtomicCompareExchangeStrong(Moer::uint64 _expected, Moer::uint64 _desired, std::memory_order _order) {
        return value.compare_exchange_strong(_expected, _desired, _order);
    }

    bool AtomicCompareExchangeWeak(
        const ABADoublePtr& _expected,
        const ABADoublePtr& _desired,
        std::memory_order   _order
    ) {
        return value.compare_exchange_weak((Moer::uint64&)_expected.value, _desired.value, _order);
    }
    bool AtomicCompareExchangeStrong(
        const ABADoublePtr& _expected,
        const ABADoublePtr& _desired,
        std::memory_order   _order
    ) {
        return value.compare_exchange_strong((Moer::uint64&)_expected.value, _desired.value, _order);
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
    std::atomic<Moer::uint64> value;
};

static_assert(sizeof(ABADoublePtr) == sizeof(Moer::uint64) && "size of ABADoublePtr is not equal to uint64_t");

/**
 * @brief lock-free Fixed size allocator without pre-thread cache
 * when allocate, it will allocate a block of memory of 1 << 18 bytes
 * it only allocate and don't free until quit
 * @tparam T 
 * @tparam MAX_ITEM_COUNT 
 * @tparam ITEM_PER_BLOCK 
 */
template<class T, Moer::uint32 MAX_ITEM_COUNT, Moer::uint32 ITEM_PER_BLOCK>
class CORE_API LockFreeIndexedAllocator {
    constexpr static Moer::uint32 s_max_block_count = (MAX_ITEM_COUNT + ITEM_PER_BLOCK - 1) / ITEM_PER_BLOCK;

public:
    LockFreeIndexedAllocator() {
        index_counter++;
        for (Moer::uint32 i = 0; i < s_max_block_count; i++) {
            m_blocks[i] = 0;
        }
    }
    FORCEINLINE Moer::uint32 Allocate(Moer::uint32 count = 1) {
        Moer::uint32 index = index_counter.fetch_add(count);
        if (index + count > MAX_ITEM_COUNT) {
            assert(false && "index out of range, no more space to allocate");
        }
        for (Moer::uint32 i = 0; i < count; i++) {
            void* newed_address = GetPayLoad(index + i);
            new (newed_address) T();
        }
        return index;
    }
    FORCEINLINE void Free(Moer::uint32 index, Moer::uint32 count = 1) {
        assert(index < index_counter.load() && index < MAX_ITEM_COUNT && "index out of range");
        for (Moer::uint32 i = 0; i < count; i++) {
            GetPayLoad(index + i)->~T();
        }
    }

    //get without allocate new item
    FORCEINLINE T* Get(Moer::uint32 index) {
        if (!index)
            return nullptr;
        Moer::uint32 block_index = index / ITEM_PER_BLOCK;
        Moer::uint32 item_index  = index % ITEM_PER_BLOCK;
        assert(
            index < index_counter.load() && index < MAX_ITEM_COUNT && block_index < s_max_block_count &&
            m_blocks[block_index] && "index out of range"
        );
        return (T*)(m_blocks[block_index].load()) + item_index;
    }

private:
    void* GetPayLoad(Moer::uint32 index) {
        Moer::uint32 block_index = index / ITEM_PER_BLOCK;
        Moer::uint32 item_index  = index % ITEM_PER_BLOCK;
        assert(
            index < index_counter.load() && index < MAX_ITEM_COUNT && block_index < s_max_block_count &&
            "index out of range"
        );
        if (!m_blocks[block_index]) {
            T* new_block = (T*)Memory::Malloc(ITEM_PER_BLOCK * sizeof(T));
            assert(Moer::IsAligned(new_block, alignof(T)));
            uintptr_t zero = 0;
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
        Moer::uint32 size           = sizeof(T);

        return (T*)(m_blocks[block_index].load()) + item_index;
    }
    std::atomic<Moer::uint64> index_counter{0};
    //per-block contains ITEM_PER_BLOCK Ts
    std::atomic<uintptr_t> m_blocks[s_max_block_count];
};
#define MAX_LOCK_FREE_NODE_COUNT     ((1ull << 31) - 1)
#define MAX_LOCK_FREE_NODE_BIT_COUNT (32)

enum class LockFreeNodeState : Moer::uint32 {
    Free    = 0,
    SetFree = 1,
    Used    = 2,
};
struct CORE_API LockFreeNode {

    LockFreeNode() : next_double{0}, next_single{0} {}
    // union {
    ABADoublePtr
        next_double; //double index for queue, because we need to change both head&head-next or tail&tail-next
    // };
    Moer::uint64 data = 0;
    Moer::uint32 next_single; //single index for stack, because we only need to change head
    inline void  SetData(Moer::uint64 _data) {
        { data = _data; }
    }
};
struct LockFreeNodeStrategy {
    using TNodeIndex = Moer::int32;
    using TNode      = LockFreeNode;

    static constexpr Moer::uint32 s_node_per_block = (1 << 18) / sizeof(LockFreeNode); //16KB

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
struct InlineValue {};

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
template<Moer::uint32 Padding>
class LockFreeNodeStack {
    using TNodeIndex = LockFreeNodeStrategy::TNodeIndex;
    using TNode      = LockFreeNodeStrategy::TNode;

public:
    void Reset() {
        m_head.SetAll(0, 0);
    }
    void Push(TNodeIndex _index) {
        ABADoublePtr local_head;
        ABADoublePtr new_head;
        do {
            local_head = m_head;
            new_head   = local_head;
            new_head.AdvanceCounter(1);
            new_head.SetValue(_index);
            LockFreeNodeStrategy::GetNode(_index)->next_single = local_head.GetValue();
        } while (!m_head.AtomicCompareExchangeWeak(local_head, new_head, std::memory_order_acq_rel));
    }

    bool PushIf(std::function<TNodeIndex(Moer::uint64)> _allocate_if_true) {
        ABADoublePtr local_head;
        ABADoublePtr new_head;
        do {
            local_head = m_head;
            TNodeIndex index =
                _allocate_if_true(ABADoublePtr::AdvanceStateCounter(local_head.GetState(), 1ull));
            if (index == 0)
                return false;

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
            if (result == 0)
                return 0;
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

    // Consumer only(single consumer)
    TNodeIndex Top() const {
        ABADoublePtr local_head = m_head;
        TNodeIndex   index      = local_head.GetValue();
        if (index == 0)
            return 0;
        return index;
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

    TNodeIndex PopAllAndChangeTag(std::function<Moer::uint64(Moer::uint64)> func_change_state) {
        ABADoublePtr local_head;
        ABADoublePtr new_head;
        TNodeIndex   index = 0;
        do {
            local_head = m_head;
            index      = local_head.GetValue();
            new_head   = local_head;
            new_head.AdvanceCounter(1);
            new_head.SetValue(0);
            new_head.SetState(func_change_state(ABADoublePtr::AdvanceStateCounter(local_head.GetState(), 1ull)
            ));
        } while (!m_head.AtomicCompareExchangeWeak(local_head, new_head, std::memory_order_acq_rel));
        return index;
    }

    FORCEINLINE Moer::uint64 GetState() const {
        ABADoublePtr local_head = m_head;
        return local_head.GetState();
    }

private:
    alignas(Padding) ABADoublePtr m_head{0};
};

template<class T, bool InlineType, Moer::uint32 Padding>
class LockFreeStackBase {
    using TNodeIndex                 = LockFreeNodeStrategy::TNodeIndex;
    using TNode                      = LockFreeNodeStrategy::TNode;
    static constexpr bool use_inline = InlineType && sizeof(T) < sizeof(Moer::uint64);

public:
    using TValue        = InlineValue<T, use_inline>::TValue;
    LockFreeStackBase() = default;
    ~LockFreeStackBase() {
        while (Pop() != nullptr) {
        }
    }
    void Push(TValue _payload) {
        TNodeIndex index = LockFreeNodeStrategy::AllocateNodeIndex();

        LockFreeNode* node = LockFreeNodeStrategy::GetNode(index);

        node->SetData(uint64(_payload));
        node_list.Push(index);
    }
    bool PushIf(TValue _payload, std::function<bool(Moer::uint64)> push_if_true) {
        TNodeIndex index = 0;

        auto allocate_node_if_true = [&index, _payload, &push_if_true](Moer::uint64 state) {
            if (push_if_true(state)) {
                if (index == 0) {
                    index              = LockFreeNodeStrategy::AllocateNodeIndex();
                    LockFreeNode* node = LockFreeNodeStrategy::GetNode(index);
                    node->SetData(Moer::uint64(_payload));
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
        if (index == 0)
            return nullptr;

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

    void
    PopAllAndChangeTag(Moer::Array<TValue>& _target, std::function<Moer::uint64(Moer::uint64)> func_change_state) {
        TNodeIndex index = node_list.PopAllAndChangeTag(func_change_state);
        while (index != 0) {
            LockFreeNode* node = LockFreeNodeStrategy::GetNode(index);
            _target.push_back((TValue)node->data);
            TNodeIndex current_node_index = index;

            index = node->next_single;
            LockFreeNodeStrategy::FreeNodeIndex(current_node_index);
        }
    }

    // Consumer only(single consumer)
    TValue Top() {
        TNodeIndex index = node_list.Top();
        if (index == 0)
            return nullptr;

        LockFreeNode* node = LockFreeNodeStrategy::GetNode(index);
        TValue        data = (TValue)node->data;

        return data;
    }

    Moer::uint64 GetState() const {
        return node_list.GetState();
    }

private:
    LockFreeNodeStack<Padding> node_list;
};

template<class T, bool InlineType = false, Moer::uint32 Padding = PLATFORM_CACHELINE_SIZE>
class LockFreeQueueBase {
    using TNodeIndex                 = LockFreeNodeStrategy::TNodeIndex;
    using TNode                      = LockFreeNodeStrategy::TNode;
    static constexpr bool use_inline = InlineType && sizeof(T) < sizeof(Moer::uint64);

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

        LockFreeNodeStrategy::GetNode(index)->SetData(Moer::uint64(_payload));

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
                    if (local_tail_node->next_double.AtomicCompareExchangeWeak(
                            local_next, new_next, std::memory_order_acq_rel
                        )) {
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

    // Consumer only(single consumer)
    TValue Top() {
        ABADoublePtr local_head = m_head;
        ABADoublePtr local_next = LockFreeNodeStrategy::GetNode(local_head.GetValue())->next_double;

        if (local_next.GetValue() == 0)
            return zero_value;

        return (TValue)LockFreeNodeStrategy::GetNode(local_next.GetValue())->data;
    }

private:
    alignas(Padding) ABADoublePtr m_head;
    alignas(Padding) ABADoublePtr m_tail;
};

template<class T, bool InlineType = false, Moer::uint32 Padding = PLATFORM_CACHELINE_SIZE>
class ClosableLockFreeMpScStack : public LockFreeStackBase<T, InlineType, Padding> {
    using TValue = LockFreeStackBase<T, InlineType, Padding>::TValue;

public:
    ClosableLockFreeMpScStack() : LockFreeStackBase<T, InlineType, Padding>() {}
    ClosableLockFreeMpScStack(ClosableLockFreeMpScStack const&)            = delete;
    ClosableLockFreeMpScStack& operator=(ClosableLockFreeMpScStack const&) = delete;
    // void Reset(){

    // }
    bool TryPush(TValue _payload) {
        return LockFreeStackBase<T, InlineType, Padding>::PushIf(_payload, [](Moer::uint64 State) {
            return (ABADoublePtr::GetTagFromState(State) & 1) == 0;
        });
    }
    bool IsClosed() const {
        return ABADoublePtr::GetTagFromState(LockFreeStackBase<T, InlineType, Padding>::GetState()) & 1;
    }
    void ComsumeAllAndClose(Moer::Array<TValue>& target) {
        LockFreeStackBase<T, InlineType, Padding>::PopAllAndChangeTag(target, [](Moer::uint64 State) {
            return ABADoublePtr::SetTagInState(State, 1);
        });
    }

private:
    std::mutex test_mutex;
};

template<typename T, Moer::uint32 TaskPriorityCount>
class TaskFIFOQueue {

    static Moer::int32 GetFirstOne(Moer::uint32 target) {
        int index = -1;
        for (int i = 0; i < 31; i++) {
            if (target == 0)
                break;
            index++;
            if (target & (1ul << 31)) {
                break;
            }
            target = target << 1;
        }
        if (index == 32)
            return -1;
        return index;
    }

public:
    TaskFIFOQueue() : state{0} {};
    Moer::int32 Push(T* task, Moer::uint32 index) {
        ABADoublePtr local_state;
        ABADoublePtr new_state;
        Moer::int32  possible_thread = -1;
        m_queue[index].Push(task);
        do {
            local_state       = state;
            Moer::uint32 possible = local_state.GetValue();
            possible_thread   = GetFirstOne(possible);

            new_state = local_state;
            new_state.AdvanceCounter(1);
            if (possible_thread >= 0) {
                Moer::uint32 thread_mask = ~(Moer::uint32(1u << (32 - possible_thread - 1)));

                new_state.SetValue(local_state.GetValue() & thread_mask);
            } else {
                new_state.SetValue(local_state.GetValue());
            }
        } while (!state.AtomicCompareExchangeWeak(local_state, new_state, std::memory_order_acq_rel));
        return possible_thread;
    }

    T* Pop(Moer::int32 index, bool allowHang = true) {
        ABADoublePtr local_state;
        ABADoublePtr new_state;

        T* result = nullptr;
        do {
            local_state = state;
            for (Moer::int32 i = 0; i < TaskPriorityCount; i++) {
                result = m_queue[i].Pop();
                if (result != nullptr) {
                    do {
                        local_state = state;
                        new_state   = local_state;
                        new_state.AdvanceCounter(1);

                    } while (!state.AtomicCompareExchangeWeak(
                        local_state.value, new_state.value, std::memory_order_acq_rel
                    ));
                    return result;
                }
            }
            if (!allowHang)
                break;
            new_state = local_state;
            new_state.AdvanceCounter(1);
            auto new_ptr = local_state.GetValue() | (1ul << (32 - index - 1));
            if (allowHang)
                new_state.SetValue(new_ptr);

        } while (
            !state.AtomicCompareExchangeWeak(local_state.value, new_state.value, std::memory_order_acq_rel)
        );
        return result;
    }

private:
    LockFreeQueueBase<T, false, PLATFORM_CACHELINE_SIZE> m_queue[TaskPriorityCount];

    ABADoublePtr state;
};
#endif // !ASYNC_QUEUE_H
