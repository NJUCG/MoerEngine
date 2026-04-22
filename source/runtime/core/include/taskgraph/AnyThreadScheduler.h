#ifndef ANY_THREAD_SCHEDULER_H
#define ANY_THREAD_SCHEDULER_H

#include "API_Macro.h"
#include "GraphTask.h"
#include "ThreadManager.h"
#include "misc/LockFree.h"
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class CORE_API AnyThreadScheduler {
public:
    using WakeWorkerFn = std::function<void(int32_t)>;

    AnyThreadScheduler(
        int32_t                     namedThreadCount,
        int32_t                     workerPerPriority,
        const std::array<int32_t, EThread::PriorityCount>& workerCountPerPriority,
        WakeWorkerFn                wakeWorkerFn
    );
    ~AnyThreadScheduler();

    void           Enqueue(BaseGraphTask* task, EThread::Type currentThread);
    BaseGraphTask* Dequeue(int32_t threadIndex);

private:
    class ChaseLevDeque {
    public:
        ChaseLevDeque();
        ~ChaseLevDeque();

        void           PushBottom(BaseGraphTask* task);
        BaseGraphTask* PopBottom();
        BaseGraphTask* StealTop();

    private:
        struct Buffer {
            explicit Buffer(int64_t initialCapacity);

            int64_t Capacity() const {
                return capacity;
            }

            BaseGraphTask* Load(int64_t index) const {
                return slots[index & (capacity - 1)].load(std::memory_order_relaxed);
            }

            void Store(int64_t index, BaseGraphTask* task) {
                slots[index & (capacity - 1)].store(task, std::memory_order_relaxed);
            }

            int64_t                                  capacity;
            std::unique_ptr<std::atomic<BaseGraphTask*>[]> slots;
        };

        Buffer* GetBuffer(std::memory_order order = std::memory_order_acquire) const {
            return m_buffer.load(order);
        }

        Buffer* AllocateBuffer(int64_t capacity);
        void    Grow(int64_t top, int64_t bottom);

        std::atomic<int64_t> m_top;
        std::atomic<int64_t> m_bottom;
        std::atomic<Buffer*> m_buffer;
        std::mutex           m_resize_mutex;
        std::vector<std::unique_ptr<Buffer>> m_retired_buffers;
    };

    struct PriorityPool {
        PriorityPool();

        bool HasWorkers() const {
            return worker_count > 0;
        }

        int32_t                                     worker_count;
        std::vector<std::unique_ptr<ChaseLevDeque>> deques;
        LockFreeQueueBase<BaseGraphTask, false>     global_queue;
        std::atomic<uint32_t>                       wake_cursor;
    };

    bool TryGetLocalWorker(EThread::Type currentThread, ThreadPriority priority, int32_t& localWorkerIndex) const;
    PriorityPool& GetPool(ThreadPriority priority);
    const PriorityPool& GetPool(ThreadPriority priority) const;
    void    WakeOneWorker(ThreadPriority priority);

    int32_t m_named_thread_count;
    int32_t m_worker_per_priority;
    std::array<int32_t, EThread::PriorityCount> m_worker_count_per_priority;
    std::array<PriorityPool, EThread::PriorityCount> m_pools;
    WakeWorkerFn m_wake_worker_fn;
};

#endif // ANY_THREAD_SCHEDULER_H
