#ifndef TASK_PIPE_H
#define TASK_PIPE_H

#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "misc/LockFree.h"
#include <functional>
#include <atomic>

namespace Moer {

namespace TaskPipeImp {

class TaskPool {
public:
    static constexpr size_t TASK_SIZE = sizeof(std::function<void()>) + sizeof(GraphEventArray) + sizeof(std::atomic<void*>) + sizeof(std::atomic<uint32_t>) + sizeof(EThread::Type) * 2 + sizeof(GraphEventRef);
    static constexpr size_t CACHELINE_SIZE = 64;
    static constexpr size_t ALIGNED_TASK_SIZE = ((TASK_SIZE + CACHELINE_SIZE - 1) / CACHELINE_SIZE) * CACHELINE_SIZE;

    static TaskPool& Get() {
        static TaskPool instance;
        return instance;
    }

    void* Allocate() {
        return Memory::MallocAligned(ALIGNED_TASK_SIZE, CACHELINE_SIZE);
    }

    void Free(void* _ptr) {
        if (_ptr) {
            Memory::Free(_ptr);
        }
    }

private:
    TaskPool() = default;
    ~TaskPool() = default;
};

} // namespace TaskPipeImp

struct TaskNode {
    std::function<void()>  lambda;
    GraphEventArray        prereqs;
    std::atomic<TaskNode*> next{nullptr};
    std::atomic<uint32_t>  ref_count{0};
    EThread::Type          logical_thread{EThread::Invalid};
    EThread::Type          actual_thread{EThread::Invalid};
    GraphEventRef          event;

    TaskNode() = default;
    
    TaskNode(
        std::function<void()>&& _lambda,
        GraphEventArray&& _prereqs,
        EThread::Type _logical_thread
    ) : lambda(std::move(_lambda)),
        prereqs(std::move(_prereqs)),
        logical_thread(_logical_thread),
        actual_thread(_logical_thread),
        event(GraphEvent::CreateGraphEvent()) {
        ref_count.store(2, std::memory_order_release);
    }

    void Release() {
        if (ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            this->~TaskNode();
            TaskPipeImp::TaskPool::Get().Free(this);
        }
    }

    static TaskNode* Create(
        std::function<void()>&& _lambda,
        GraphEventArray&& _prereqs,
        EThread::Type _logical_thread
    ) {
        void* ptr = TaskPipeImp::TaskPool::Get().Allocate();
        return new (ptr) TaskNode(std::move(_lambda), std::move(_prereqs), _logical_thread);
    }

    static void Destroy(TaskNode* _task) {
        if (_task) {
            _task->Release();
        }
    }
};

class TaskPipe {
public:
    CORE_API TaskPipe();
    CORE_API ~TaskPipe();

    /**
     * @brief Enqueue a task into the pipe for execution.
     * 
     * The task will be executed in FIFO order relative to other tasks in this pipe.
     * Supports enqueuing after Close() - the task will wait for the last event from
     * the previous pipe lifecycle before execution.
     * 
     * @param _func The function to execute
     * @param _deps Optional dependencies that must complete before this task
     * @param _thread The thread type to execute on (default: AnyThread_NormalPri)
     * @return GraphEventRef Event that can be used to wait for this task's completion
     */
    CORE_API GraphEventRef Enqueue(
        std::function<void()>&& _func,
        GraphEventArray&& _deps = {},
        EThread::Type _thread = EThread::AnyThread_NormalPri
    );
    
    /**
     * @brief Close the pipe and mark the end of the current task chain.
     * 
     * After calling Close(), the pipe can still accept new tasks via Enqueue(),
     * but those tasks will start a new chain and depend on the last event
     * from the previous chain.
     * 
     * @return GraphEventRef Event for the last task in the chain, or nullptr if pipe was empty
     */
    CORE_API GraphEventRef Close();
    
    /**
     * @brief Get the event for the last enqueued task.
     * 
     * @return GraphEventRef Event for the last task, or nullptr if no tasks have been enqueued
     */
    CORE_API GraphEventRef GetLastEvent() const;

#ifdef STATS
    uint32 GetPipeId() const { return m_pipe_id; }
    uint32 GetPipeIndex() const { return m_pipe_index; }
#endif

private:
    void ExecuteTaskChain(TaskNode* _task);

private:
    TaskNode*     m_current{nullptr};
    GraphEventRef m_last_event;

#ifdef STATS
    uint32 m_pipe_id{0};
    uint32 m_pipe_index{0};
    static std::atomic<uint32> s_pipe_id_counter;
#endif
};

} // namespace Moer

#endif // TASK_PIPE_H
