#ifndef MOERENGINE_RENDER_THREAD_H
#define MOERENGINE_RENDER_THREAD_H
#include "API_Macro.h"
#include "Core.h"
#include "RenderAPI.h"
#include "misc/STL.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>
class Runnable;
class RunnableThread;
namespace Moer {
class RenderThread;
extern Runnable*       g_render_thread_runnable;
extern RunnableThread* g_render_thread;
extern void RENDER_API StartRenderThread();

extern void RENDER_API StopRenderThread();

extern void RENDER_API ShutDownRenderThread();

extern void RENDER_API RestartRenderThread();

//
extern void RENDER_API SuspendRenderThread(bool _restart_later = true);

extern void RENDER_API ResumeRenderThread(bool _promised_restart_before = true);

extern bool RENDER_API IsRenderThreadRunning();

template<typename Function>
class RenderThreadServiceTask {
public:
    explicit RenderThreadServiceTask(Function&& function) : m_function(std::move(function)) {}

    static EThread::Type GetPreferredThread() {
        return EThread::ERenderThread;
    }

    void Fire(EThread::Type, const GraphEventRef&) {
        m_function();
    }

private:
    Function m_function;
};

struct RenderFrameFence {
    uint64        frame_id = 0;
    GraphEventRef rt_done{};

    bool IsComplete() const {
        return !rt_done || rt_done->IsComplete();
    }
};

class RENDER_API RenderThreadService {
public:
    ~RenderThreadService();

    void Start();
    void Stop();

    template<typename Function>
        requires std::is_invocable_v<std::decay_t<Function>&>
    GraphEventRef Enqueue(Function&& task) {
        assert(running && "Cannot enqueue work before the render thread starts.");
        assert(IsCurrentlyGameThread() && "RenderThreadService is submitted by the game thread.");

        using StoredFunction = std::decay_t<Function>;
        using TaskType       = RenderThreadServiceTask<StoredFunction>;
        return GraphTask<TaskType>::Create(StoredFunction(std::forward<Function>(task)))
            .Dispatch(EThread::ERenderThread);
    }

    template<typename Function>
        requires std::is_invocable_v<std::decay_t<Function>&>
    RenderFrameFence EnqueueFrame(uint64 frame_id, Function&& task) {
        return RenderFrameFence{frame_id, Enqueue(std::forward<Function>(task))};
    }

    void Wait(const GraphEventRef& event);
    void Wait(const RenderFrameFence& fence);

    template<typename Function>
        requires std::is_invocable_v<std::decay_t<Function>&>
    void RunAndWait(Function&& task) {
        auto event = Enqueue(std::forward<Function>(task));
        Wait(event);
    }

    void Flush();

    bool IsRunning() const {
        return running;
    }

private:
    bool running = false;
};

template<typename Feedback>
class BoundedRenderFrameQueue {
public:
    BoundedRenderFrameQueue(RenderThreadService& service, uint max_frame_lag) :
        service(service), max_frame_lag(max_frame_lag) {}

    ~BoundedRenderFrameQueue() {
        assert(pending_frames.empty() && "Render frame queue must be flushed before destruction.");
    }

    template<typename Function>
        requires std::is_invocable_r_v<Feedback, std::decay_t<Function>&>
    void Submit(uint64 frame_id, Function&& render_frame) {
        auto feedback = MakeShared<std::optional<Feedback>>();
        using StoredFunction = std::decay_t<Function>;
        auto fence = service.EnqueueFrame(
            frame_id,
            [feedback,
             render_frame = StoredFunction(std::forward<Function>(render_frame))]() mutable {
                feedback->emplace(std::invoke(render_frame));
            }
        );
        pending_frames.emplace_back(PendingFrame{std::move(fence), std::move(feedback)});
    }

    template<typename ApplyFunction>
    void RetireCompleted(ApplyFunction&& apply_feedback) {
        while (!pending_frames.empty() && pending_frames.front().fence.IsComplete()) {
            RetireFront(apply_feedback);
        }
    }

    template<typename ApplyFunction>
    void EnforceLagLimit(ApplyFunction&& apply_feedback) {
        while (pending_frames.size() > max_frame_lag) {
            RetireFront(apply_feedback);
        }
    }

    template<typename ApplyFunction>
    void Flush(ApplyFunction&& apply_feedback) {
        while (!pending_frames.empty()) {
            RetireFront(apply_feedback);
        }
    }

    size_t PendingFrameCount() const {
        return pending_frames.size();
    }

private:
    struct PendingFrame {
        RenderFrameFence                  fence{};
        SharedPtr<std::optional<Feedback>> feedback;
    };

    template<typename ApplyFunction>
    void RetireFront(ApplyFunction& apply_feedback) {
        assert(!pending_frames.empty());
        PendingFrame frame = std::move(pending_frames.front());
        pending_frames.pop_front();

        service.Wait(frame.fence);
        assert(frame.feedback && frame.feedback->has_value());

        Feedback feedback = std::move(frame.feedback->value());
        if constexpr (requires { feedback.frame_id; }) {
            assert(feedback.frame_id == frame.fence.frame_id && "Render feedback retired out of order.");
        }
        std::invoke(apply_feedback, std::move(feedback));
    }

    RenderThreadService& service;
    uint                 max_frame_lag = 0;
    DEQueue<PendingFrame> pending_frames;
};

RENDER_API void RunRenderThreadControlAndWait(std::function<void()> task);

class RENDER_API ScopedResumeRenderThread {
public:
    ScopedResumeRenderThread();
    ~ScopedResumeRenderThread();
};

class RENDER_API RenderThreadFence {
public:
    RenderThreadFence() {}

    void BeginFence();

    bool IsFenceComplete();

    void Wait();

private:
    GraphEventRef complete_event;
};

} // namespace Moer

#endif
