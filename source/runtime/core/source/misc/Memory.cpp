#include "misc/MMemory.h"
#if USE_MIMALLOC
#include "mimalloc.h"
#endif
#include "platform/Platform.h"
#include <limits>
#include <memory>
#include <new>

#if USE_MIMALLOC
#if PLATFORM_WINDOWS
#include "mimalloc-new-delete.h"

#include "mimalloc-override.h"
#endif
#endif

void* Memory::Malloc(size_t size) noexcept {
#if USE_MIMALLOC
    return mi_malloc(size);
#else
    return malloc(size);
#endif
}

void* Memory::Calloc(size_t count, size_t size) noexcept {
#if USE_MIMALLOC
    return mi_calloc(count, size);
#else
    return calloc(count, size);
#endif
}

void* Memory::ReAlloc(void* p, size_t newsize) noexcept {
#if USE_MIMALLOC

    return mi_realloc(p, newsize);
#else
    return realloc(p, newsize);
#endif
}

void Memory::Free(void* p) noexcept {
#if USE_MIMALLOC
    mi_free(p);
#else
    free(p);
#endif
}

void Memory::Free(void* p, size_t size) noexcept {
#if USE_MIMALLOC
    mi_free_size(p, size);
#else
    free(p);
#endif
}

void* Memory::MallocAligned(size_t size, size_t alignment) noexcept {
#if USE_MIMALLOC
    return mi_malloc_aligned(size, alignment);
#else
    return _aligned_malloc(size, alignment);
#endif
}

void* Memory::MallocN(size_t count, size_t size) {
    if (size != 0 && count > std::numeric_limits<size_t>::max() / size) {
        throw std::bad_array_new_length();
    }
#if USE_MIMALLOC
    return mi_new_n(count, size);
#else
    void* allocation = malloc(count * size);
    if (allocation == nullptr && count != 0 && size != 0) {
        throw std::bad_alloc();
    }
    return allocation;
#endif
}
void* Memory::CallocAligned(size_t count, size_t size, size_t alignment) noexcept {
#if USE_MIMALLOC
    return mi_calloc_aligned(count, size, alignment);
#else
    return _aligned_malloc(count * size, alignment);
#endif
}

namespace Moer {
}
