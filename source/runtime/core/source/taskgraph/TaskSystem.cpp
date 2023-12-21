// TaskSystem.cpp: 定义应用程序的入口点。
//

#include <chrono>
#include <iostream>
#include <thread>
#include "Core.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/ThreadManager.h"
#include "platform/Platform.h"
#include "taskgraph/GraphTask.h"
#include "spdlog/spdlog.h"
#include <functional>

using namespace std;

class MTask {
public:
    MTask(EThread::Type _threadToReturn, std::string info) : m_thread_to_return{_threadToReturn}, m_info{info} {}
    EThread::Type GetPreferredThread() {
        return m_thread_to_return;
    }
    void Fire(EThread::Type _threadToReturn, const GraphEventRef& _event) {

        SPDLOG_ERROR("info: {} thread:{}", m_info, Platform::GetCurrentThreadID());
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
//		SPDLOG_INFO("process render thread until return");
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
        TaskGraph::GetInterface().~TaskGraph();
    }

}// namespace Moer

bool Moer::TaskGraphTest() {
    // UE task graph test
    SPDLOG_INFO("===============test started================");
    {// task completes before it's waited for
        GraphEventRef Event = FunctionGraphTask::ConstructAndDispatchWhenReady(
            [] {
            });

        while (!Event->IsComplete())// in single-threaded mode tasks are executed only when waited for
        {
        }
        Event->Wait(EThread::EMainThread);
    }
    SPDLOG_INFO("=============== task completes before it's waited for success ================");

    {// task completes after it's waited for
        for (int i = 0; i != 100; ++i) {
            GraphEventRef Event = FunctionGraphTask::ConstructAndDispatchWhenReady([]() {
                this_thread::sleep_for(30ms);// pause for a bit to let waiting start
            });
            assert(!Event->IsComplete());
            Event->Wait(EThread::EMainThread);
        }
    }
    SPDLOG_INFO("=============== task completes after it's waited for success ================");
    {// event w/o a task, signaled by explicit call to DispatchSubsequents before it's waited for
        for (int i = 0; i != 10000; ++i) {
            GraphEventRef Event = GraphEvent::CreateGraphEvent();

            FunctionGraphTask::ConstructAndDispatchWhenReady(
                [&Event] {
                    Event->TryUnlockSubsequents();
                });
            while (!Event->IsComplete())// in single-threaded mode tasks are executed only when waited for
            {
            }
            Event->Wait(EThread::EMainThread);
        }
    }
    SPDLOG_INFO("=============== task signaled by explicit call to DispatchSubsequents before it's waited for success ================");
    {// event w/o a task, signaled by explicit call to DispatchSubsequents after it's waited for
        for (int i = 0; i != 100; ++i) {
            GraphEventRef Event  = GraphEvent::CreateGraphEvent();
            auto          Lambda = [&Event] {
                this_thread::sleep_for(25ms);// pause for a bit to let waiting start
                Event->TryUnlockSubsequents();
            };
            GraphEventRef Task = FunctionGraphTask::ConstructAndDispatchWhenReady(std::move(Lambda));
            assert(!Event->IsComplete());
            Event->Wait();
            Task->Wait();
        }
    }
    SPDLOG_INFO("=============== task signaled by explicit call to DispatchSubsequents after it's waited for success ================");
    {// wait for prereq by DontCompleteUntil
        for (int i = 0; i != 10000; ++i) {
            auto Lambda = [](EThread::Type CurrentThread, const GraphEventRef& MyCompletionGraphEvent) {
                //UE_LOG(LogTemp, Log, TEXT("Main task"));

                GraphEventRef PrereqHolder = GraphEvent::CreateGraphEvent();

                GraphEventRef Prereq = FunctionGraphTask::ConstructAndDispatchWhenReady(
                    [PrereqHolder] {
                        //UE_LOG(LogTemp, Log, TEXT("Prereq"));
                        PrereqHolder->Wait();// hold it until it's used for `DontCompleteUntil`
                    });
                GraphEvent* completion = static_cast<GraphEvent*>(MyCompletionGraphEvent.Get());
                completion->WaitUntil(Prereq);
                assert(!PrereqHolder->IsComplete());// check that prereq was incomplete during DontCompleteUntil ^^

                // now that Prereq was registered in DontCompleteUntil, unlock it
                PrereqHolder->TryUnlockSubsequents();
            };

            GraphEventRef Event = FunctionGraphTask::ConstructAndDispatchWhenReady(std::move(Lambda));
            assert(!Event->IsComplete());
            Event->Wait(EThread::EMainThread);
        }
    }
    SPDLOG_INFO("=============== wait for prereq by waitUntil success ================");
    {// prereq is completed before DontCompleteUntil is called
        for (int i = 0; i != 100000; ++i) {
            GraphEventRef Prereq = FunctionGraphTask::ConstructAndDispatchWhenReady(
                [] {

                });
            std::this_thread::sleep_for(std::chrono::nanoseconds(10));
            Prereq->Wait(EThread::EMainThread);

            GraphEventRef Event = FunctionGraphTask::ConstructAndDispatchWhenReady(
                [&Prereq](EThread::Type CurrentThread, const GraphEventRef& MyCompletionGraphEvent) {
                    MyCompletionGraphEvent->WaitUntil(Prereq);
                    //UE_LOG(LogTemp, Log, TEXT("Main task"));
                });

            while (!Event->IsComplete())// in single-threaded mode tasks are executed only when waited for
            {
            }
            Event->Wait(EThread::EMainThread);
        }
    }
    SPDLOG_INFO("=============== prereq is completed before waitUntil is called success ================");

    // dependencies

    {// a task is not executed until its prerequisite is completed
        for (int i = 0; i != 100000; ++i) {
            bool          bExecuted = false;
            GraphEventRef Prereq    = GraphEvent::CreateGraphEvent();
            GraphEventRef MainTask  = FunctionGraphTask::ConstructAndDispatchWhenReady([&bExecuted] { bExecuted = true; }, Prereq);
            // dummy task that is executed while the main task is waiting for its prereq
            FunctionGraphTask::ConstructAndDispatchWhenReady([] {})->Wait();
            assert(!bExecuted);
            Prereq->TryUnlockSubsequents();
            MainTask->Wait();
            assert(bExecuted);
        }
    }
    SPDLOG_INFO("=============== task is not executed until its prerequisite is completed success ================");
    {// a task is not executed until all its prerequisites are completed
        for (int i = 0; i != 100000; ++i) {
            bool            bExecuted = false;
            GraphEventArray Prereqs{GraphEvent::CreateGraphEvent(), GraphEvent::CreateGraphEvent()};
            GraphEventRef   MainTask = FunctionGraphTask::ConstructAndDispatchWhenReady([&bExecuted] { bExecuted = true; }, &Prereqs);
            // dummy task that is executed while the main task is waiting for its prereqs
            FunctionGraphTask::ConstructAndDispatchWhenReady([] {})->Wait();
            assert(!bExecuted);

            Prereqs[0]->TryUnlockSubsequents();
            FunctionGraphTask::ConstructAndDispatchWhenReady([] {})->Wait();
            assert(!bExecuted);

            Prereqs[1]->TryUnlockSubsequents();
            MainTask->Wait();
            assert(bExecuted);
        }
    }
    SPDLOG_INFO("=============== a task is not executed until all its prerequisites are completed success ================");
    {// holding a task
        struct FTask {

            EThread::Type GetPreferredThread() {
                return EThread::UNKNOWN_THREAD;
            }

            void Fire(EThread::Type CurrentThread, const GraphEventRef& MyCompletionGraphEvent) {
            }
        };
        for (int i = 0; i != 100000; ++i) {
            GraphTask<FTask>* Task  = GraphTask<FTask>::CreateTask().ConstructAndHold();
            GraphEventRef     Event = Task->GetCompletionEvent();
            assert(!Event->IsComplete());
            Task->Dispatch();
            Event->Wait();
            assert(Event->IsComplete());
        }
    }
    SPDLOG_INFO("=============== holding a task success ================");
    {// check ref count for named thread tasks
        for (int i = 0; i != 100000; ++i) {
            GraphEventRef LocalQueueTask = FunctionGraphTask::ConstructAndDispatchWhenReady([] {}, nullptr, EThread::EGameThread_local);
            LocalQueueTask->Wait(EThread::EGameThread_local);
            assert(LocalQueueTask.GetRefCount() == 1);
        }
    }
    SPDLOG_INFO("=============== check ref count for named thread tasks ================");
    for (int i = 0; i != 100000; ++i) {// a particular real-life case that doesn't work in the old TaskGraph if run in single-threaded mode.
        // the culprit is that when a task is waited for, in single-threaded mode the queue it was pushed to is executed.
        // Here the (local queue) task  depends on a task that is not in the same queue and so it doesn't get executed

        GraphEventRef AnyTask        = FunctionGraphTask::ConstructAndDispatchWhenReady([] {});
        GraphEventRef LocalQueueTask = FunctionGraphTask::ConstructAndDispatchWhenReady([] {}, AnyTask, EThread::EGameThread_local);
        LocalQueueTask->Wait(EThread::EGameThread_local);
        assert(LocalQueueTask.GetRefCount() == 1);
    }
    SPDLOG_INFO("=============== (local queue) task  depends on a task that is not in the same queue succuss ================");
    {// launch a GT task, then an any-thread task that depends on it. wait for the any-thread task. this was a deadlock on the new frontend
        GraphEventRef GTTask = FunctionGraphTask::ConstructAndDispatchWhenReady([] { return Moer::GetGameThreadId() == Platform::GetCurrentThreadID(); }, nullptr, EThread::EMainThread);
        GTTask->Wait();//delete this will cause dead lock
        GraphEventRef AnyThreadTask = FunctionGraphTask::ConstructAndDispatchWhenReady([] {}, GTTask);
        AnyThreadTask->Wait();
    }
    SPDLOG_INFO("=============== deadlock test succuss ================");
    SPDLOG_INFO("===============test over================");
    return true;
}
