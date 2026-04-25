// TaskSystem.cpp: 定义应用程序的入口点。
//

#include "Core.h"
#include "platform/Platform.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/ThreadManager.h"
#include <chrono>
#include <functional>
#include <thread>

using namespace std;

class MTask {
public:
    MTask(EThread::Type _thread_to_return, std::string _info) :
        m_thread_to_return{_thread_to_return},
        m_info{_info} {}
    EThread::Type GetPreferredThread() {
        return m_thread_to_return;
    }
    void Fire(EThread::Type _thread_to_return, const GraphEventRef& _event) {

        SPDLOG_ERROR(MOER_TEXT("info: {} thread:{}"), m_info, Platform::GetCurrentThreadID());
    }

private:
    EThread::Type m_thread_to_return;
    std::string   m_info;
};
//class MTestRenderThread :public Runnable {
//public:
//	MTestRenderThread() :syncEvent{EventPool::get()->getEvent(false)} {}
//	~MTestRenderThread() { EventPool::get()->releaseEvent(syncEvent); syncEvent = nullptr; }
//	virtual uint32_t run() override {
//
//		RenderMain();
//		return 0;
//	};
//	virtual void init() override {};
//	virtual void stop() override {};
//	virtual void exit() override {};
//	virtual ThreadIndex getIndex() override {
//		return EThread::ERenderThread;
//	};
//	void RenderMain() {
//		//create Render thread
//
//		TaskGraph::getInterface().attachToNameThread(EThread::ERenderThread);
//		syncEvent->trigger();
//		SPDLOG_INFO(MOER_TEXT("process render thread until return"));
//		TaskGraph::getInterface().processThreadUntilReturn(EThread::ERenderThread);
//
//	}
//	Event* getTaskGraphSyncEvent() { return syncEvent; }
//protected:
//	Event* syncEvent;
//};
//MTestRenderThread* g_render_thread_runnable;
//RunnableThread* g_render_thread;

namespace Moer {
void TaskSystem::Init() {
    TaskGraph::Init();
}
void TaskSystem::ShutDown() {
    TaskGraph::Shutdown();
}

} // namespace Moer

bool Moer::TaskGraphTest() {
    // UE task graph test
    SPDLOG_INFO(MOER_TEXT("===============test started================"));
    { // task completes before it's waited for
        auto event = LambdaTask::Dispatch([] {
            SPDLOG_INFO(MOER_TEXT("LambdaTask"));
        });

        while (!event->IsComplete()) // in single-threaded mode tasks are executed only when waited for
        {
        }
        event->Wait(EThread::EMainThread);
    }
    SPDLOG_INFO(MOER_TEXT("=============== task completes before it's waited for success ================"));

    { // task completes after it's waited for
        for (int i = 0; i != 100; ++i) {
            GraphEventRef event = LambdaTask::Dispatch([]() {
                this_thread::sleep_for(30ms); // pause for a bit to let waiting start
            });
            assert(!event->IsComplete());
            event->Wait(EThread::EMainThread);
        }
    }
    SPDLOG_INFO(MOER_TEXT("=============== task completes after it's waited for success ================"));
    { // event w/o a task, signaled by explicit call to DispatchSubsequents before it's waited for
        for (int i = 0; i != 10000; ++i) {
            GraphEventRef event = GraphEvent::CreateGraphEvent();

            LambdaTask::Dispatch([&event] {
                event->TryUnlockSubsequents();
            });
            while (!event->IsComplete()) // in single-threaded mode tasks are executed only when waited for
            {
            }
            event->Wait(EThread::EMainThread);
        }
    }
    SPDLOG_INFO(MOER_TEXT("=============== task signaled by explicit call to DispatchSubsequents before it's waited ")
                "for success ================");
    { // event w/o a task, signaled by explicit call to DispatchSubsequents after it's waited for
        for (int i = 0; i != 100; ++i) {
            GraphEventRef event  = GraphEvent::CreateGraphEvent();
            auto          lambda = [&event] {
                this_thread::sleep_for(25ms); // pause for a bit to let waiting start
                event->TryUnlockSubsequents();
            };
            GraphEventRef task = LambdaTask::Dispatch(std::move(lambda));
            assert(!event->IsComplete());
            event->Wait();
            task->Wait();
        }
    }
    SPDLOG_INFO(MOER_TEXT("=============== task signaled by explicit call to DispatchSubsequents after it's waited for ")
                "success ================");
    { // wait for prereq by DontCompleteUntil
        for (int i = 0; i != 10000; ++i) {

            auto lambda = [](EThread::Type _current_thread, const GraphEventRef& _my_completion_graph_event) {
                GraphEventRef prereq_holder = GraphEvent::CreateGraphEvent();

                GraphEventRef prereq = LambdaTask::Dispatch([prereq_holder] {
                    prereq_holder->Wait(); // hold it until it's used for `DontCompleteUntil`
                });
                _my_completion_graph_event->WaitUntil(prereq);
                assert(!prereq->IsComplete()); // check that prereq was incomplete during DontCompleteUntil ^^

                // now that Prereq was registered in DontCompleteUntil, unlock it
                // this_thread::sleep_for(std::chrono::nanoseconds(10));// pause for a bit to let waiting start
                prereq_holder->TryUnlockSubsequents();
            };

            GraphEventRef event = LambdaTask::Dispatch(std::move(lambda));
            // assert(!Event->IsComplete());
            // PrereqHolder->TryUnlockSubsequents();
            event->Wait(EThread::EMainThread);
        }
    }
    SPDLOG_INFO(MOER_TEXT("=============== wait for prereq by waitUntil success ================"));
    { // prereq is completed before DontCompleteUntil is called
        for (int i = 0; i != 100000; ++i) {
            GraphEventRef prereq = LambdaTask::Dispatch([] {

            });
            // std::this_thread::sleep_for(std::chrono::nanoseconds(10));
            prereq->Wait(EThread::EMainThread);

            GraphEventRef event = LambdaTask::Dispatch(
                [&prereq](EThread::Type _current_thread, const GraphEventRef& _my_completion_graph_event) {
                    _my_completion_graph_event->WaitUntil(prereq);
                }
            );

            while (!event->IsComplete()) // in single-threaded mode tasks are executed only when waited for
            {
            }
            event->Wait(EThread::EMainThread);
        }
    }
    SPDLOG_INFO(MOER_TEXT("=============== prereq is completed before waitUntil is called success ================"));

    // dependencies

    { // a task is not executed until its prerequisite is completed
        for (int i = 0; i != 100000; ++i) {
            bool          b_executed = false;
            GraphEventRef prereq     = GraphEvent::CreateGraphEvent();
            // GraphEventRef main_task  = LambdaTask::Dispatch([&b_executed] { b_executed = true; }, prereq);
            GraphEventRef main_task = LambdaTask::Create([&b_executed] {
                                          b_executed = true;
                                      })
                                          .Wait(prereq)
                                          .Dispatch();

            // dummy task that is executed while the main task is waiting for its prereq
            LambdaTask::Dispatch([] {})->Wait();
            assert(!b_executed);
            prereq->TryUnlockSubsequents();
            main_task->Wait();
            assert(b_executed);
        }
    }
    SPDLOG_INFO(
        MOER_TEXT("=============== task is not executed until its prerequisite is completed success ================")
    );
    { // a task is not executed until all its prerequisites are completed
        for (int i = 0; i != 100000; ++i) {
            bool            b_executed = false;
            GraphEventArray prereqs{GraphEvent::CreateGraphEvent(), GraphEvent::CreateGraphEvent()};
            GraphEventRef   main_task = LambdaTask::Create([&b_executed] {
                                          b_executed = true;
                                      })
                                          .Wait(prereqs)
                                          .Dispatch();
            // dummy task that is executed while the main task is waiting for its prereqs
            LambdaTask::Dispatch([] {})->Wait();
            assert(!b_executed);

            prereqs[0]->TryUnlockSubsequents();
            LambdaTask::Dispatch([] {})->Wait();
            assert(!b_executed);

            prereqs[1]->TryUnlockSubsequents();
            main_task->Wait();
            assert(b_executed);
        }
    }
    SPDLOG_INFO(MOER_TEXT("=============== a task is not executed until all its prerequisites are completed success ")
                "================");
    { // holding a task
        struct FTask {

            EThread::Type GetPreferredThread() {
                return EThread::UNKNOWN_THREAD;
            }

            void Fire(EThread::Type _current_thread, const GraphEventRef& _my_completion_graph_event) {}
        };
        for (int i = 0; i != 100000; ++i) {
            auto          task  = std::move(GraphTask<FTask>::Create());
            GraphEventRef event = task.GetCompletionEvent();
            assert(!event->IsComplete());
            task.Dispatch();
            event->Wait();
            assert(event->IsComplete());
        }
    }
    SPDLOG_INFO(MOER_TEXT("=============== holding a task success ================"));
    { // check ref count for named thread tasks
        for (int i = 0; i != 100000; ++i) {
            GraphEventRef local_queue_task = LambdaTask::Dispatch([] {}, EThread::EMainThread);
            local_queue_task->Wait(EThread::EMainThread);
            assert(local_queue_task.GetRefCount() == 1);
        }
    }
    SPDLOG_INFO(MOER_TEXT("=============== check ref count for named thread tasks ================"));
    for (int i = 0; i != 100000;
         ++i) { // a particular real-life case that doesn't work in the old TaskGraph if run in single-threaded mode.
        // the culprit is that when a task is waited for, in single-threaded mode the queue it was pushed to is executed.
        // Here the (local queue) task  depends on a task that is not in the same queue and so it doesn't get executed

        GraphEventRef any_task = LambdaTask::Dispatch([] {});
        GraphEventRef local_queue_task =
            LambdaTask::Create([] {}).Wait(any_task).Dispatch(EThread::EMainThread);
        local_queue_task->Wait(EThread::EMainThread);
        assert(local_queue_task.GetRefCount() == 1);
    }
    SPDLOG_INFO(MOER_TEXT("=============== (local queue) task  depends on a task that is not in the same queue succuss ")
                "================");
    { // launch a GT task, then an any-thread task that depends on it. wait for the any-thread task. this was a deadlock on the new frontend
        GraphEventRef gt_task = LambdaTask::Dispatch(
            [] {
                return Moer::GetGameThreadId() == Platform::GetCurrentThreadID();
            },
            EThread::EMainThread
        );
        gt_task->Wait(); //delete this will cause dead lock
        GraphEventRef any_thread_task = LambdaTask::Create([] {}).Wait(gt_task).Dispatch();
        any_thread_task->Wait();
    }
    SPDLOG_INFO(MOER_TEXT("=============== deadlock test succuss ================"));
    SPDLOG_INFO(MOER_TEXT("===============test over================"));
    return true;
}
