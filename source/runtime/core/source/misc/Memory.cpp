#include "misc/MMemory.h"

#include "mimalloc.h"
#include "platform/Platform.h"

#if PLATFORM_WINDOWS
#include "mimalloc-new-delete.h"
#endif

#include "mimalloc-override.h"

void* Memory::Malloc(size_t size) noexcept {
    return mi_malloc(size);
}

void* Memory::Calloc(size_t count, size_t size) noexcept {
    return mi_calloc(count, size);
}

void* Memory::ReAlloc(void* p, size_t newsize) noexcept {
    return mi_realloc(p, newsize);
}

void Memory::Free(void* p) noexcept {
    mi_free(p);
}

void* Memory::MallocAligned(size_t size, size_t alignment) noexcept {
    return mi_malloc_aligned(size, alignment);
}

void* Memory::CallocAligned(size_t count, size_t size, size_t alignment) noexcept {
    return mi_calloc_aligned(count, size, alignment);
}

namespace Moer {
}