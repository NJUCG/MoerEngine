#include "misc/LockFree.h"

class GlobalLockFreePoolCache {
    using TNodeIndex = LockFreeNodeStrategy::TNodeIndex;
    using TNode      = LockFreeNodeStrategy::TNode;

public:
    GlobalLockFreePoolCache()                                          = default;
    ~GlobalLockFreePoolCache()                                         = default;
    GlobalLockFreePoolCache(const GlobalLockFreePoolCache&)            = delete;
    GlobalLockFreePoolCache& operator=(const GlobalLockFreePoolCache&) = delete;
    GlobalLockFreePoolCache(GlobalLockFreePoolCache&&)                 = delete;
    GlobalLockFreePoolCache& operator=(GlobalLockFreePoolCache&&)      = delete;

    void Push(TNodeIndex index) {

        TNode* node = LockFreeNodeStrategy::GetNode(index);
        node->next_double.SetValue(0);
        node->SetData(nullptr);
        node->next_single = 0;
        node_stack.Push(index);
    }

    TNodeIndex Pop() {
        TNodeIndex index = node_stack.Pop();
        if (index == 0) {
            index = LockFreeNodeStrategy::GetAllocator().Allocate();
        }
        assert(index != 0);
        //init nodes
        TNode* node = LockFreeNodeStrategy::GetNode(index);
        node->next_double.SetAll(0, 0);
        node->SetData(nullptr);
        assert(node->next_single == 0);
        return index;
    }

private:
    LockFreeNodeStack<64> node_stack;
};

static GlobalLockFreePoolCache& GetLockFreeNodeCache() {
    static GlobalLockFreePoolCache cache;
    return cache;
}

LockFreeNodeStrategy::TAllocator& LockFreeNodeStrategy::GetAllocator() {
    static TAllocator allocator{};
    static bool       init = false;

    if (!init) {
        // new (&allocator) TAllocator();
        init = true;
    }

    return allocator;
}

LockFreeNodeStrategy::TNodeIndex LockFreeNodeStrategy::AllocateNodeIndex() {
    auto& cache = GetLockFreeNodeCache();
    return cache.Pop();
}

void LockFreeNodeStrategy::FreeNodeIndex(TNodeIndex index) {
    GetLockFreeNodeCache().Push(index);
}