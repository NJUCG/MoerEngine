// Diagnostic targets only. Preserve CRT allocation/deallocation pairing and new-handler behavior.
#include "profile/RdgDiagnostics.h"
#include <algorithm>
#include <cstdlib>
#include <malloc.h>
#include <new>

namespace {
void* Allocate(size_t size, size_t alignment = 0) {
    for (;;) {
        void* memory = alignment ? _aligned_malloc(std::max(size, size_t{1}), alignment) :
                                   std::malloc(std::max(size, size_t{1}));
        if (memory) { Moer::RdgDiagnostics::Allocation(memory, size); return memory; }
        if (const auto handler = std::get_new_handler()) { handler(); }
        else { throw std::bad_alloc(); }
    }
}
void Release(void* pointer, bool aligned = false) noexcept {
    Moer::RdgDiagnostics::Free(pointer);
    if (aligned) { _aligned_free(pointer); } else { std::free(pointer); }
}
}
void* operator new(size_t size) { return Allocate(size); }
void* operator new[](size_t size) { return Allocate(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { try { return Allocate(size); } catch (...) { return nullptr; } }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { try { return Allocate(size); } catch (...) { return nullptr; } }
void operator delete(void* pointer) noexcept { Release(pointer); }
void operator delete[](void* pointer) noexcept { Release(pointer); }
void operator delete(void* pointer, size_t) noexcept { Release(pointer); }
void operator delete[](void* pointer, size_t) noexcept { Release(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { Release(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { Release(pointer); }
void* operator new(size_t size, std::align_val_t alignment) { return Allocate(size, size_t(alignment)); }
void* operator new[](size_t size, std::align_val_t alignment) { return Allocate(size, size_t(alignment)); }
void* operator new(size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept { try { return Allocate(size, size_t(alignment)); } catch (...) { return nullptr; } }
void* operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept { try { return Allocate(size, size_t(alignment)); } catch (...) { return nullptr; } }
void operator delete(void* pointer, std::align_val_t) noexcept { Release(pointer, true); }
void operator delete[](void* pointer, std::align_val_t) noexcept { Release(pointer, true); }
void operator delete(void* pointer, size_t, std::align_val_t) noexcept { Release(pointer, true); }
void operator delete[](void* pointer, size_t, std::align_val_t) noexcept { Release(pointer, true); }
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { Release(pointer, true); }
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { Release(pointer, true); }
