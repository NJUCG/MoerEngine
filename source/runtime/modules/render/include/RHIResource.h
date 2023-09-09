#ifndef RHI_RESOURCE_H
#define RHI_RESOURCE_H
#include "RHICommon.h"
#include "API_Macro.h"
#include <assert.h>
#include <atomic>
#include <misc/StatQueue.h>
#include <unordered_set>
namespace __ENGINE_NAME__ {
namespace RHI {

}
}
class RHI_API RHIResource {
public:
RHIResource(ERHIResourceType _type = ERHIResourceType::RRT_NONE) : type(_type) {}
virtual ~RHIResource() {}

public:
int32_t AddRef() {
    return m_counter.fetch_add(1) + 1;
};
virtual void Destroy() = 0;
int32_t      TryDrop() {
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

private:
struct ResourceAtomicFlags {
    std::atomic_int32_t ref_count;
    std::atomic_bool     b_pending_deleting;

public:
    int32_t AddRef(std::memory_order memory_order) {
        return ref_count.fetch_add(1, memory_order) + 1;
    }
    int32_t TryDrop(std::memory_order memory_order) {
        return ref_count.fetch_sub(1, memory_order) - 1;
    }
    bool MarkToDelete(std::memory_order memory_order) {
        return b_pending_deleting.exchange(true, memory_order);
    }
    bool UnMarkToDelete(std::memory_order memory_order) {
        return b_pending_deleting.exchange(false, memory_order);
    }
    bool IsDeleteing() {
        assert(b_pending_deleting.load(std::memory_order_relaxed) == 1);
        if (ref_count.load(std::memory_order_acquire) != 0) {
            return true;
        }
        b_pending_deleting.exchange(false, std::memory_order_release);
        return false;
    }
    bool IsValid(std::memory_order memory_order) {
        return !b_pending_deleting.load(memory_order) && ref_count.load(memory_order) > 0;
    }
    int32_t GetRefCount(std::memory_order memory_order) {
        std::unordered_set<uint32_t> s;
        s.count(1);
        return ref_count.load(memory_order);
    }
};
ERHIResourceType type;
//for const resource state change
mutable ResourceAtomicFlags flags;
static std::atomic<StatMPSCQueue<RHIResource*>*> pending_deletings;



};
#endif// !RHI_RESOURCE_H
