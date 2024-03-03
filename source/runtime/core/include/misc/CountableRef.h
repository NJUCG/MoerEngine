//
// Created by 17152 on 2023/9/16.
//

#ifndef MOERENGINE_COUNTABLEREF_H
#define MOERENGINE_COUNTABLEREF_H
#include <atomic>
#include <cassert>
#include <type_traits>
template<typename TCountable>
concept concept_is_countable = requires(TCountable t) {
    //    t.m_counter;
    //    std::is_same_v<typeof(t.m_counter), std::atomic<int32_t>>;
    t.AddRef() + (uint32_t)1;
    t.DeRef() + (uint32_t)1;
    t.GetRefCount() + (uint32_t)1;
    //    t.Destroy();
};

class Countable {
public:
    int32_t AddRef() {
        return m_counter.fetch_add(1) + 1;
    };
    virtual void Destroy() = 0;
    int32_t      DeRef() {
        assert(m_counter >= 0);
        int32_t current = m_counter.fetch_sub(1);
        if (current == 1) {
            Destroy();
        }
        return current - 1;
    };
    int32_t GetRefCount() { return m_counter.load(); }

protected:
    std::atomic<int32_t> m_counter;
};

template<typename T>
class CountableRef {
public:
    CountableRef() : ptr{nullptr} {
        //if(std::is_convertible<T, Countable>::value) return;
        //assert(false);
    }
    CountableRef(T* countable, bool AddRef = true) {
        ptr = countable;
        if (ptr != nullptr && AddRef) {
            ptr->AddRef();
        }
    }

    CountableRef(const CountableRef& copy) {
        ptr = copy.ptr;
        if (ptr != nullptr) {
            ptr->AddRef();
        }
    }
    template<typename CopyType>
    CountableRef(const CountableRef<CopyType>& copy) {
        ptr = copy.ptr;
        if (ptr != nullptr) {
            ptr->AddRef();
        }
    }
    template<typename MoveType>
    CountableRef(CountableRef<MoveType>&& move) {
        ptr      = move.ptr;
        move.ptr = nullptr;
    }

    CountableRef(CountableRef&& move) {
        ptr      = move.ptr;
        move.ptr = nullptr;
    }
    ~CountableRef() {
        if (ptr != nullptr) {
            ptr->DeRef();
        }
    }
    CountableRef& operator=(T* _ptr) {
        if (ptr != _ptr) {
            T* old = ptr;
            ptr    = _ptr;
            if (ptr != nullptr) {
                ptr->AddRef();
            }
            if (old != nullptr) {
                old->DeRef();
            }
        }
        return *this;
    }
    template<typename RefType>
    CountableRef& operator=(const CountableRef<RefType>& _ref) {
        return *this = _ref.ptr;
    }
    CountableRef& operator=(const CountableRef& _ref) {
        return *this = _ref.ptr;
    }

    template<typename MoveType>
    CountableRef& operator=(CountableRef<MoveType>&& _ref_move) {
        T* old        = ptr;
        ptr           = _ref_move.ptr;
        _ref_move.ptr = nullptr;
        if (old != nullptr) {
            old->DeRef();
        }
        return *this;
    }

    CountableRef& operator=(CountableRef&& _ref_move) {
        if (this != &_ref_move) {
            T* old        = ptr;
            ptr           = _ref_move.ptr;
            _ref_move.ptr = nullptr;
            if (old != nullptr) {
                old->DeRef();
            }
        }
        return *this;
    }
    T* Get() const { return ptr; }
    T* operator->() const {
        return ptr;
    }
    operator T*() const {
        return ptr;
    }
    bool IsValid() const {
        return ptr != nullptr;
    }

    void Swap(CountableRef& other) {
        T* old    = ptr;
        ptr       = other.ptr;
        other.ptr = old;
    }
    template<typename Other>
    inline bool operator==(const CountableRef<Other>& other) {
        return ptr == other.ptr;
    }

    inline bool operator==(const CountableRef& other) {
        return ptr == other.ptr;
    }

    inline bool operator==(const T* other) {
        return ptr == other;
    }
    int32_t GetRefCount() {
        return ptr->GetRefCount();
    }

protected:
    T* ptr;
    template<typename OtherType>
    friend class CountableRef;
};

class CountableResource : Countable {
public:
    explicit CountableResource() = default;
    virtual ~CountableResource() = default;

public:
    uint32_t AddRef() {
        //first AddRef() happens before DeRef
        int32_t ref_count = flags.AddRef(std::memory_order_acquire);
        assert(ref_count > 0);
        return ref_count;
    };

    uint32_t DeRef() {
        int32_t ref_count = flags.DeRef(std::memory_order_release);
        assert(ref_count >= 0);
        if (ref_count == 0) {
            Destroy();
        }
        return (uint32_t)ref_count;
    };
    //only for look-up purposes, don't care about sequences
    uint32_t GetRefCount() const { return (uint32_t)flags.GetRefCount(std::memory_order_relaxed); }

    bool IsValid() const { return flags.IsValid(std::memory_order_relaxed); }
    void Delete() {
        if (flags.MarkToDelete(std::memory_order_acquire)) {
            // delete this;
        }
    }

protected:
private:
    void Destroy() {}
    struct ResourceAtomicFlags {
        std::atomic<uint32_t> packed;

        static constexpr uint32_t s_mark_for_delete_mask = 1 << 31;
        static constexpr uint32_t s_is_deleting_mask     = 1 << 30;
        static constexpr uint32_t s_ref_count_mask       = s_is_deleting_mask - 1;

    public:
        int32_t AddRef(std::memory_order memory_order) {
            uint32_t current_packed = packed.fetch_add(1, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            int32_t num_ref = (current_packed & s_ref_count_mask) + 1;
            assert(num_ref < s_mark_for_delete_mask);
            return num_ref;
        }
        int32_t DeRef(std::memory_order memory_order) {
            uint32_t current_packed = packed.fetch_sub(1, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            int32_t num_ref = (current_packed & s_ref_count_mask) - 1;
            assert(num_ref >= 0);
            return num_ref;
        }
        bool MarkToDelete(std::memory_order memory_order) {
            uint32_t current_packed = packed.fetch_or(s_mark_for_delete_mask, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            return (current_packed & s_mark_for_delete_mask) != 0;
        }

        bool UnMarkToDelete(std::memory_order memory_order) {
            uint32_t current_packed = packed.fetch_xor(s_mark_for_delete_mask, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            bool current_mark_for_delete = (current_packed & s_mark_for_delete_mask) != 0;
            assert(current_mark_for_delete && "resource is not marked for deleting");
            return current_mark_for_delete;
        }
        bool IsDeleting() {
            /* make sure packed data processing sequence handled correctly - acquire-rel */
            uint32_t current_packed = packed.load(std::memory_order_acquire);
            assert((current_packed & s_mark_for_delete_mask) != 0 && "resource not marked for deleting");
            assert((current_packed & s_is_deleting_mask) != 0 && "resource is currently deleting");
            uint32_t num_ref = current_packed & s_ref_count_mask;
            if (num_ref == 0) {
                return true;
            }
            UnMarkToDelete(std::memory_order_release);
            return false;
        }
        bool IsValid(std::memory_order memory_order) {
            uint32_t current_packed = packed.load(memory_order);
            return (current_packed & s_mark_for_delete_mask) == 0 && (current_packed & s_ref_count_mask) > 0;
        }

        bool IsMarkedForDeleting(std::memory_order memory_order) {
            return (packed.load(memory_order) & s_mark_for_delete_mask) != 0;
        }
        int32_t GetRefCount(std::memory_order memory_order) {
            return packed.load(memory_order) & s_ref_count_mask;
        }
    };
    //for const resource state change
    mutable ResourceAtomicFlags flags;
};
#endif//MOERENGINE_COUNTABLEREF_H
