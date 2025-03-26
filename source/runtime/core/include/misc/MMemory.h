#ifndef MOER_ENGINE_MEMORY_H
#define MOER_ENGINE_MEMORY_H
#include "API_Macro.h"
#include <assert.h>
#include <cstdint>
#include <cstddef>
#include <utility>

#define USE_MIMALLOC 1
class CORE_API Memory {
public:
    static void* Malloc(size_t size) noexcept;
    static void* MallocAligned(size_t size, size_t alignment) noexcept;
    static void* MallocN(size_t count, size_t size) noexcept;
    static void* Calloc(size_t count, size_t size) noexcept;
    static void* CallocAligned(size_t count, size_t size, size_t alignment) noexcept;
    static void* ReAlloc(void* p, size_t newsize) noexcept;
    static void  Free(void* p) noexcept;
    static void  Free(void* p, size_t size) noexcept;
};
struct MoerNewStub {};
inline void* operator new(size_t, MoerNewStub, void* ptr) { return ptr; }
inline void  operator delete(void*, MoerNewStub, void*) {}
#define MoerPlacementNew(ptr) new (MoerNewStub(), ptr)
#define MoerNew(type)         new (MoerNewStub(), Memory::Malloc(sizeof(type))) type
template<typename T>
void MoerDelete(T* p) {
    if (p) {
        p->~T();
        Memory::Free(p);
    }
}

struct MoerDeleter {
    template<typename T>
    void operator()(T* p) {
        MoerDelete(p);
    }
};

template<class T>
struct MoerStlAllocatorCommon {
    typedef T                 value_type;
    typedef size_t            size_type;
    typedef std::ptrdiff_t    difference_type;
    typedef value_type&       reference;
    typedef value_type const& const_reference;
    typedef value_type*       pointer;
    typedef value_type const* const_pointer;

#if ((__cplusplus >= 201103L) || (_MSC_VER > 1900))// C++11
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap            = std::true_type;
    template<class U, class... Args>
    void construct(U* p, Args&&... args) { ::new (p) U(std::forward<Args>(args)...); }
    template<class U>
    void destroy(U* p) noexcept { p->~U(); }
#else
    void construct(pointer p, value_type const& val) { ::new (p) value_type(val); }
    void destroy(pointer p) { p->~value_type(); }
#endif

    size_type     max_size() const noexcept { return (PTRDIFF_MAX / sizeof(value_type)); }
    pointer       address(reference x) const { return &x; }
    const_pointer address(const_reference x) const { return &x; }
};

template<class T>
struct MoerStlAllocator : public MoerStlAllocatorCommon<T> {
    using typename MoerStlAllocatorCommon<T>::size_type;
    using typename MoerStlAllocatorCommon<T>::value_type;
    using typename MoerStlAllocatorCommon<T>::pointer;
    template<class U>
    struct rebind {
        typedef MoerStlAllocator<U> other;
    };

    MoerStlAllocator() noexcept                        = default;
    MoerStlAllocator(const MoerStlAllocator&) noexcept = default;
    template<class U>
    MoerStlAllocator(const MoerStlAllocator<U>&) noexcept {}
    MoerStlAllocator select_on_container_copy_construction() const { return *this; }
    void             deallocate(T* p, size_type) { Memory::Free(p); }

#if (__cplusplus >= 201703L)// C++17
    MOER_NODISCARD T* allocate(size_type count) { return static_cast<T*>(Memory::MallocN(count, sizeof(T))); }
    MOER_NODISCARD T* allocate(size_type count, const void*) { return allocate(count); }
#else
    MOER_NODISCARD pointer allocate(size_type count, const void* = 0) { return static_cast<pointer>(Memory::MallocN(count, sizeof(value_type))); }
#endif

#if ((__cplusplus >= 201103L) || (_MSC_VER > 1900))// C++11
    using is_always_equal = std::true_type;
#endif
};
#endif//MOER_ENGINE_MEMORY_H