//
// Created by 17152 on 2023/9/16.
//

#ifndef MOERENGINE_COUNTABLEREF_H
#define MOERENGINE_COUNTABLEREF_H

#include "misc/Types.h"
#include <atomic>
#include <cassert>
#include <type_traits>
template<typename TCountable>
concept concept_is_countable = requires(TCountable t) {
    t.AddRef() + (Moer::uint32)1;
    t.DeRef() + (Moer::uint32)1;
    t.GetRefCount() + (Moer::uint32)1;
};

template<typename T>
class CountableRef;

template<typename T>
struct IsCountableRef : std::false_type {};

template<typename T>
constexpr bool is_countable_ref_v = IsCountableRef<T>::value;

template<typename T>
struct IsCountableRef<CountableRef<T>> : std::true_type {};
class Countable {
public:
    Moer::int32 AddRef() {
        return m_counter.fetch_add(1) + 1;
    };
    virtual void Destroy() = 0;
    Moer::int32  DeRef() {
        assert(m_counter >= 0);
        Moer::int32 current = m_counter.fetch_sub(1);
        if (current == 1) {
            Destroy();
        }
        return current - 1;
    };
    Moer::int32 GetRefCount() {
        return m_counter.load();
    }

protected:
    std::atomic<Moer::int32> m_counter;
};

#define COUNTABLE_IMPLEMENTATION                      \
    std::atomic<Moer::int32> m_counter{0};            \
    inline Moer::int32       AddRef() {               \
        return m_counter.fetch_add(1) + 1;            \
    }                                                 \
    inline Moer::uint32 DeRef() {                     \
        assert(m_counter >= 0);                       \
        Moer::int32 current = m_counter.fetch_sub(1); \
        if (current == 1) {                           \
            Destroy();                                \
        }                                             \
        return current - 1;                           \
    }                                                 \
    inline Moer::uint32 GetRefCount() const {         \
        return m_counter.load();                      \
    }                                                 \
    inline void SetRefCount(Moer::uint32 _count) {    \
        m_counter.store(_count);                      \
    }

#define COUNTABLE_IMPLEMENTATION_AUTO_DESTROY \
    COUNTABLE_IMPLEMENTATION                  \
    inline void Destroy() {                   \
        MoerDelete(this);                     \
    }

template<typename T>
class CountableRef {
public:
    using CountableType = T;
    CountableRef() : ptr{nullptr} {
        //if(std::is_convertible<T, Countable>::value) return;
        //assert(false);
    }

    CountableRef(T* _countable, bool _add_ref = true) {
        if constexpr (!concept_is_countable<T>) {
            //  static_assert(false, "T must be a Countable type");
        }
        ptr = _countable;
        if (ptr != nullptr && _add_ref) {
            ptr->AddRef();
        }
    }
    template<typename TOther>
        requires std::is_convertible_v<TOther, T>
    CountableRef(TOther* _countable, bool _add_ref = true) {
        ptr = (T*)_countable;
        if (ptr != nullptr && _add_ref) {
            ptr->AddRef();
        }
    }

    CountableRef(const CountableRef& _copy) {
        ptr = _copy.ptr;
        if (ptr != nullptr) {
            ptr->AddRef();
        }
    }
    template<typename CopyType>
        requires std::is_convertible_v<CopyType, T>
    CountableRef(const CountableRef<CopyType>& _copy) {
        ptr = (T*)(_copy.ptr);
        if (ptr != nullptr) {
            ptr->AddRef();
        }
    }
    template<typename MoveType>
        requires std::is_convertible_v<MoveType, T>
    CountableRef(CountableRef<MoveType>&& _move) noexcept {
        ptr       = _move.ptr;
        _move.ptr = nullptr;
    }

    CountableRef(CountableRef&& _move) noexcept {
        ptr       = _move.ptr;
        _move.ptr = nullptr;
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
    T* Get() const {
        return ptr;
    }
    T* operator->() const {
        return ptr;
    }
    operator T*() const {
        return ptr;
    }
    bool IsValid() const {
        return ptr != nullptr;
    }

    void Swap(CountableRef& _other) {
        T* old     = ptr;
        ptr        = _other.ptr;
        _other.ptr = old;
    }
    template<typename Other>
    inline bool operator==(const CountableRef<Other>& _other) {
        return ptr == _other.ptr;
    }

    inline bool operator==(const CountableRef& _other) {
        return ptr == _other.ptr;
    }

    inline bool operator==(const T* _other) {
        return ptr == _other;
    }
    Moer::int32 GetRefCount() {
        if (ptr == nullptr) {
            return 0;
        }
        return ptr->GetRefCount();
    }

    T* Release() {
        T* old = ptr;
        ptr    = nullptr;
        return old;
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
    Moer::uint32 AddRef() {
        //first AddRef() happens before DeRef
        Moer::int32 ref_count = flags.AddRef(std::memory_order_acquire);
        assert(ref_count > 0);
        return ref_count;
    };

    Moer::uint32 DeRef() {
        Moer::int32 ref_count = flags.DeRef(std::memory_order_release);
        assert(ref_count >= 0);
        if (ref_count == 0) {
            Destroy();
        }
        return (Moer::uint32)ref_count;
    };
    //only for look-up purposes, don't care about sequences
    Moer::uint32 GetRefCount() const {
        return (Moer::uint32)flags.GetRefCount(std::memory_order_relaxed);
    }

    bool IsValid() const {
        return flags.IsValid(std::memory_order_relaxed);
    }
    void Delete() {
        if (flags.MarkToDelete(std::memory_order_acquire)) {
            // delete this;
        }
    }

protected:
private:
    void Destroy() {}
    struct ResourceAtomicFlags {
        std::atomic<Moer::uint32> packed;

        static constexpr Moer::uint32 s_mark_for_delete_mask = 1 << 31;
        static constexpr Moer::uint32 s_is_deleting_mask     = 1 << 30;
        static constexpr Moer::uint32 s_ref_count_mask       = s_is_deleting_mask - 1;

    public:
        Moer::int32 AddRef(std::memory_order memory_order) {
            Moer::uint32 current_packed = packed.fetch_add(1, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            Moer::int32 num_ref = (current_packed & s_ref_count_mask) + 1;
            assert(num_ref < s_mark_for_delete_mask);
            return num_ref;
        }
        Moer::int32 DeRef(std::memory_order memory_order) {
            Moer::uint32 current_packed = packed.fetch_sub(1, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            Moer::int32 num_ref = (current_packed & s_ref_count_mask) - 1;
            assert(num_ref >= 0);
            return num_ref;
        }
        bool MarkToDelete(std::memory_order memory_order) {
            Moer::uint32 current_packed = packed.fetch_or(s_mark_for_delete_mask, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            return (current_packed & s_mark_for_delete_mask) != 0;
        }

        bool UnMarkToDelete(std::memory_order memory_order) {
            Moer::uint32 current_packed = packed.fetch_xor(s_mark_for_delete_mask, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            bool current_mark_for_delete = (current_packed & s_mark_for_delete_mask) != 0;
            assert(current_mark_for_delete && "resource is not marked for deleting");
            return current_mark_for_delete;
        }
        bool IsDeleting() {
            /* make sure packed data processing sequence handled correctly - acquire-rel */
            Moer::uint32 current_packed = packed.load(std::memory_order_acquire);
            assert((current_packed & s_mark_for_delete_mask) != 0 && "resource not marked for deleting");
            assert((current_packed & s_is_deleting_mask) != 0 && "resource is currently deleting");
            Moer::uint32 num_ref = current_packed & s_ref_count_mask;
            if (num_ref == 0) {
                return true;
            }
            UnMarkToDelete(std::memory_order_release);
            return false;
        }
        bool IsValid(std::memory_order memory_order) {
            Moer::uint32 current_packed = packed.load(memory_order);
            return (current_packed & s_mark_for_delete_mask) == 0 && (current_packed & s_ref_count_mask) > 0;
        }

        bool IsMarkedForDeleting(std::memory_order memory_order) {
            return (packed.load(memory_order) & s_mark_for_delete_mask) != 0;
        }
        Moer::int32 GetRefCount(std::memory_order memory_order) {
            return packed.load(memory_order) & s_ref_count_mask;
        }
    };
    //for const resource state change
    mutable ResourceAtomicFlags flags;
};
#endif //MOERENGINE_COUNTABLEREF_H
