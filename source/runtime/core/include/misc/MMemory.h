#ifndef MOER_ENGINE_MEMORY_H
#define MOER_ENGINE_MEMORY_H

class Memory {
    static void* Malloc(size_t size) noexcept;
    static void* MallocAligned(size_t size, size_t alignment) noexcept;
    static void* Calloc(size_t count, size_t size) noexcept;
    static void* CallocAligned(size_t count, size_t size, size_t alignment) noexcept;
    static void* ReAlloc(void* p, size_t newsize) noexcept;
    static void  Free(void* p) noexcept;
};
namespace Moer {

}// namespace Moer
#endif//MOER_ENGINE_MEMORY_H