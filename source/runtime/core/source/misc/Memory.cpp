#include "misc/MMemory.h"
#include "profile/RdgDiagnostics.h"
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
    void* result = mi_malloc(size);
#else
    void* result = malloc(size);
#endif
    MOER_RDG_ENGINE_ALLOC(result, size);
    return result;
}

void* Memory::Calloc(size_t count, size_t size) noexcept {
#if USE_MIMALLOC
    void* result = mi_calloc(count, size);
#else
    void* result = calloc(count, size);
#endif
    MOER_RDG_ENGINE_ALLOC(result, count * size);
    return result;
}

void* Memory::ReAlloc(void* p, size_t newsize) noexcept {
#if USE_MIMALLOC

    void* result = mi_realloc(p, newsize);
#else
    void* result = realloc(p, newsize);
#endif
    MOER_RDG_ENGINE_ALLOC(result, newsize);
    return result;
}

void Memory::Free(void* p) noexcept {
    MOER_RDG_FREE(p);
#if USE_MIMALLOC
    mi_free(p);
#else
    free(p);
#endif
}

void Memory::Free(void* p, size_t size) noexcept {
    MOER_RDG_FREE(p);
#if USE_MIMALLOC
    mi_free_size(p, size);
#else
    free(p);
#endif
}

void* Memory::MallocAligned(size_t size, size_t alignment) noexcept {
#if USE_MIMALLOC
    void* result = mi_malloc_aligned(size, alignment);
#else
    void* result = _aligned_malloc(size, alignment);
#endif
    MOER_RDG_ENGINE_ALLOC(result, size);
    return result;
}

void* Memory::MallocN(size_t count, size_t size) {
    if (size != 0 && count > std::numeric_limits<size_t>::max() / size) {
        throw std::bad_array_new_length();
    }
#if USE_MIMALLOC
    void* result = mi_new_n(count, size);
#else
    void* result = malloc(count * size);
    if (result == nullptr && count != 0 && size != 0) {
        throw std::bad_alloc();
    }
#endif
    MOER_RDG_ENGINE_ALLOC(result, count * size);
    return result;
}
void* Memory::CallocAligned(size_t count, size_t size, size_t alignment) noexcept {
#if USE_MIMALLOC
    void* result = mi_calloc_aligned(count, size, alignment);
#else
    void* result = _aligned_malloc(count * size, alignment);
#endif
    MOER_RDG_ENGINE_ALLOC(result, count * size);
    return result;
}

namespace Moer {
}
