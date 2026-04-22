#include "taskgraph/AnyThreadScheduler.h"
#include <algorithm>

AnyThreadScheduler::ChaseLevDeque::Buffer::Buffer(int64_t initialCapacity) : capacity(initialCapacity) {
    assert(initialCapacity > 0 && "ChaseLevDeque capacity must be positive.");
    assert(
        (initialCapacity & (initialCapacity - 1)) == 0 &&
        "ChaseLevDeque capacity must stay power-of-two for masked indexing."
    );

    slots = std::make_unique<std::atomic<BaseGraphTask*>[]>(capacity);
    for (int64_t index = 0; index < capacity; ++index) {
        slots[index].store(nullptr, std::memory_order_relaxed);
    }
}

AnyThreadScheduler::ChaseLevDeque::ChaseLevDeque() : m_top(0), m_bottom(0), m_buffer(nullptr) {
    Buffer* buffer = AllocateBuffer(32);
    m_buffer.store(buffer, std::memory_order_release);
}

AnyThreadScheduler::ChaseLevDeque::~ChaseLevDeque() {}

AnyThreadScheduler::ChaseLevDeque::Buffer* AnyThreadScheduler::ChaseLevDeque::AllocateBuffer(int64_t capacity) {
    auto buffer = std::make_unique<Buffer>(capacity);
    Buffer* raw = buffer.get();
    m_retired_buffers.emplace_back(std::move(buffer));
    return raw;
}

void AnyThreadScheduler::ChaseLevDeque::Grow(int64_t top, int64_t bottom) {
    std::lock_guard lock(m_resize_mutex);

    Buffer* current = GetBuffer(std::memory_order_acquire);
    if ((bottom - top) < current->Capacity() - 1) {
        return;
    }

    Buffer* expanded = AllocateBuffer(current->Capacity() << 1);
    for (int64_t index = top; index < bottom; ++index) {
        expanded->Store(index, current->Load(index));
    }
    m_buffer.store(expanded, std::memory_order_release);
}

void AnyThreadScheduler::ChaseLevDeque::PushBottom(BaseGraphTask* task) {
    assert(task != nullptr);

    int64_t bottom = m_bottom.load(std::memory_order_relaxed);
    int64_t top    = m_top.load(std::memory_order_acquire);
    Buffer* buffer = GetBuffer(std::memory_order_relaxed);
    if ((bottom - top) >= buffer->Capacity() - 1) {
        Grow(top, bottom);
        buffer = GetBuffer(std::memory_order_relaxed);
    }

    buffer->Store(bottom, task);
    m_bottom.store(bottom + 1, std::memory_order_release);
}

BaseGraphTask* AnyThreadScheduler::ChaseLevDeque::PopBottom() {
    int64_t bottom = m_bottom.load(std::memory_order_relaxed) - 1;
    Buffer* buffer = GetBuffer(std::memory_order_relaxed);
    m_bottom.store(bottom, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    int64_t top = m_top.load(std::memory_order_relaxed);
    if (top <= bottom) {
        BaseGraphTask* task = buffer->Load(bottom);
        if (top == bottom) {
            if (!m_top.compare_exchange_strong(
                    top,
                    top + 1,
                    std::memory_order_seq_cst,
                    std::memory_order_relaxed
                )) {
                // Another worker stole the last slot before the owner committed the pop, so the
                // previously loaded task pointer is no longer owned by this thread.
                task = nullptr;
            }
            m_bottom.store(bottom + 1, std::memory_order_relaxed);
        }
        return task;
    }

    m_bottom.store(bottom + 1, std::memory_order_relaxed);
    return nullptr;
}

BaseGraphTask* AnyThreadScheduler::ChaseLevDeque::StealTop() {
    int64_t top = m_top.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    int64_t bottom = m_bottom.load(std::memory_order_acquire);
    if (top >= bottom) {
        return nullptr;
    }

    Buffer* buffer = GetBuffer(std::memory_order_acquire);
    BaseGraphTask* task = buffer->Load(top);
    if (m_top.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
        return task;
    }
    return nullptr;
}

AnyThreadScheduler::PriorityPool::PriorityPool() : worker_count(0), wake_cursor(0) {}

AnyThreadScheduler::AnyThreadScheduler(
    int32_t                     namedThreadCount,
    int32_t                     workerPerPriority,
    const std::array<int32_t, EThread::PriorityCount>& workerCountPerPriority,
    WakeWorkerFn                wakeWorkerFn
) :
    m_named_thread_count(namedThreadCount),
    m_worker_per_priority(workerPerPriority),
    m_worker_count_per_priority(workerCountPerPriority),
    m_wake_worker_fn(std::move(wakeWorkerFn)) {
    for (int32_t priority = 0; priority < EThread::PriorityCount; ++priority) {
        PriorityPool& pool = m_pools[priority];
        pool.worker_count  = m_worker_count_per_priority[priority];
        pool.deques.reserve(pool.worker_count);
        for (int32_t worker = 0; worker < pool.worker_count; ++worker) {
            pool.deques.emplace_back(std::make_unique<ChaseLevDeque>());
        }
    }
}

AnyThreadScheduler::~AnyThreadScheduler() {}

bool AnyThreadScheduler::TryGetLocalWorker(
    EThread::Type  currentThread,
    ThreadPriority priority,
    int32_t&       localWorkerIndex
) const {
    if (EThread::IsUnKnownThread(currentThread)) {
        return false;
    }

    ThreadIndex threadIndex = EThread::GetThreadIndex(currentThread);
    if (threadIndex < m_named_thread_count) {
        return false;
    }

    if (EThread::GetThreadPriority(currentThread) != priority) {
        return false;
    }

    int32_t localIndex = GetLocalWorkerIndex(threadIndex, priority);
    if (localIndex < 0 || localIndex >= m_worker_count_per_priority[priority]) {
        return false;
    }

    localWorkerIndex = localIndex;
    return true;
}

int32_t AnyThreadScheduler::GetLocalWorkerIndex(int32_t threadIndex, ThreadPriority priority) const {
    return threadIndex - m_named_thread_count - priority * m_worker_per_priority;
}

AnyThreadScheduler::PriorityPool& AnyThreadScheduler::GetPool(ThreadPriority priority) {
    assert(priority >= 0 && priority < EThread::PriorityCount);
    return m_pools[priority];
}

const AnyThreadScheduler::PriorityPool& AnyThreadScheduler::GetPool(ThreadPriority priority) const {
    assert(priority >= 0 && priority < EThread::PriorityCount);
    return m_pools[priority];
}

void AnyThreadScheduler::WakeOneWorker(ThreadPriority priority) {
    PriorityPool& pool = GetPool(priority);
    if (!pool.HasWorkers()) {
        return;
    }

    uint32_t localIndex  = pool.wake_cursor.fetch_add(1, std::memory_order_relaxed) % pool.worker_count;
    int32_t  threadIndex = m_named_thread_count + priority * m_worker_per_priority + static_cast<int32_t>(localIndex);
    m_wake_worker_fn(threadIndex);
}

void AnyThreadScheduler::Enqueue(BaseGraphTask* task, EThread::Type currentThread) {
    assert(task != nullptr);

    ThreadPriority priority = task->GetPriority();
    PriorityPool&  pool     = GetPool(priority);
    assert(pool.HasWorkers());

    int32_t localWorkerIndex = -1;
    if (TryGetLocalWorker(currentThread, priority, localWorkerIndex)) {
        pool.deques[localWorkerIndex]->PushBottom(task);
        return;
    }

    pool.global_queue.Push(task);
    WakeOneWorker(priority);
}

BaseGraphTask* AnyThreadScheduler::Dequeue(int32_t threadIndex) {
    ThreadPriority priorityFromIndex = (threadIndex - m_named_thread_count) / m_worker_per_priority;
    PriorityPool&  pool              = GetPool(priorityFromIndex);
    int32_t        localWorkerIndex  = GetLocalWorkerIndex(threadIndex, priorityFromIndex);

    assert(localWorkerIndex >= 0 && localWorkerIndex < pool.worker_count);

    if (BaseGraphTask* task = pool.deques[localWorkerIndex]->PopBottom()) {
        return task;
    }

    if (BaseGraphTask* task = pool.global_queue.Pop()) {
        return task;
    }

    for (int32_t offset = 1; offset < pool.worker_count; ++offset) {
        int32_t victim = (localWorkerIndex + offset) % pool.worker_count;
        if (BaseGraphTask* task = pool.deques[victim]->StealTop()) {
            return task;
        }
    }

    return nullptr;
}
