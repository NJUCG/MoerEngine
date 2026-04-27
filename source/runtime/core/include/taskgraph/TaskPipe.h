#ifndef TASK_PIPE_H
#define TASK_PIPE_H

#include "taskgraph/GraphTask.h"

#ifdef STATS
#include <atomic>
#endif
#include <functional>
#include <mutex>

namespace Moer {

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
    mutable std::mutex m_mutex;
    GraphEventRef      m_last_event;

#ifdef STATS
    uint32 m_pipe_id{0};
    uint32 m_pipe_index{0};
    static std::atomic<uint32> s_pipe_id_counter;
#endif
};

} // namespace Moer

#endif // TASK_PIPE_H
