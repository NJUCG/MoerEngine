#ifdef DESCRIPTOR_VMA_CANDIDATE
#include "vma_candidate.h"
#else
#include "VulkanDescriptorBufferAllocator.h"
#endif
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <new>
#include <stdexcept>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

// Separate executable: inject CPU bookkeeping OOM without altering engine code.
thread_local bool fail_allocation = false;
const char* failure_phase = "startup";
void* operator new(std::size_t bytes) {
    if (fail_allocation) throw std::bad_alloc();
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

void Check(bool ok) {
    if (!ok) throw std::runtime_error("allocation failure contract violated");
}
template<class F> void ExpectBadAlloc(F operation) {
    bool caught = false;
    fail_allocation = true;
    try { operation(); }
    catch (const std::bad_alloc&) { caught = true; }
    catch (...) { fail_allocation = false; throw; }
    fail_allocation = false;
    Check(caught);
}

int main() {
    std::set_terminate([] {
        std::fprintf(stderr, "FAIL: unexpected termination during %s\n", failure_phase);
        std::_Exit(2);
    });
#ifdef _MSC_VER
    // A fault-injection probe must fail on stderr, never wait on a CRT dialog.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#ifdef DESCRIPTOR_VMA_CANDIDATE
    using Allocator = VmaDescriptorCandidate;
#else
    using Allocator = Moer::Render::VulkanDescriptorBufferAllocator;
#endif
    constexpr uint64_t block = 64 * 1024, capacity = 6 * 1024 * 1024;
    try {
#ifdef DESCRIPTOR_VMA_CANDIDATE
        Allocator allocator(capacity, 32, 32);
#else
        Allocator allocator(capacity, block, 32);
#endif
        failure_phase = "CreateLease OOM";
        ExpectBadAlloc([&] { allocator.CreateLease(); });
        auto lease = allocator.CreateLease();
        failure_phase = "first ReserveRange OOM";
        ExpectBadAlloc([&] { allocator.ReserveRange(*lease, 32); });
        Check(allocator.GetUsage(*lease).reserved_bytes == 0);
        Check(allocator.ReserveRange(*lease, 32) == 0);
        auto checkpoint = allocator.CreateCheckpoint(*lease);
        // Do not assume an STL's vector growth policy. Force an append that
        // needs new bookkeeping, and check that *that* append is transactional.
        failure_phase = "append ReserveRange OOM";
        bool append_failed = false;
        for (unsigned attempt = 0; attempt < capacity / block - 1; ++attempt) {
            const auto before = allocator.GetUsage(*lease);
            fail_allocation = true;
            try {
                const auto offset = allocator.ReserveRange(*lease, block);
                fail_allocation = false;
                Check(offset.has_value());
            } catch (const std::bad_alloc&) {
                fail_allocation = false;
                const auto after = allocator.GetUsage(*lease);
                Check(after.used_bytes == before.used_bytes);
                Check(after.reserved_bytes == before.reserved_bytes);
                append_failed = true;
                break;
            }
        }
        Check(append_failed);
        Check(allocator.ReserveRange(*lease, block).has_value());
        failure_phase = "GetWrittenRanges OOM";
        ExpectBadAlloc([&] { allocator.GetWrittenRanges(*lease); });
        failure_phase = "Rewind/Release with operator new disabled";
        fail_allocation = true;
        allocator.Rewind(*lease, checkpoint);
        allocator.Release(*lease);
        fail_allocation = false;
        auto full = allocator.CreateLease();
        Check(allocator.ReserveRange(*full, capacity) == 0);
        allocator.Release(*full);
#ifdef DESCRIPTOR_VMA_CANDIDATE
        std::cout << "PASS: C++ wrapper create/reserve/append/snapshot OOM; rewind/release without operator new; full recovery\n"
                     "NOT COVERED: VMA internal malloc/aligned_malloc OOM (not gracefully supported by bundled VMA)\n";
#else
        std::cout << "PASS: create/reserve/append/snapshot OOM; allocation-free rewind/release; full recovery\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        fail_allocation = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
