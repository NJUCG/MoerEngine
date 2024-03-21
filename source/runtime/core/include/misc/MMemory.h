#ifndef MOER_ENGINE_MEMORY_H
#define MOER_ENGINE_MEMORY_H
#include "API_Macro.h"
#include <assert.h>
#include <cstdint>

#define USE_MIMALLOC 1
class CORE_API Memory {
public:
    static void* Malloc(size_t size) noexcept;
    static void* MallocAligned(size_t size, size_t alignment) noexcept;
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
namespace Moer {
    static inline bool IsAligned(void* p, size_t alignment) {
        assert(alignment != 0);
        return (((uintptr_t)p % alignment) == 0);
    }
}// namespace Moer
#endif//MOER_ENGINE_MEMORY_H