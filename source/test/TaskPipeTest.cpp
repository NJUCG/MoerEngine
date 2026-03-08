#include "taskgraph/TaskPipe.h"
#include "taskgraph/TaskSystem.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <cassert>
#include <vector>

using namespace Moer;

void TestTaskPipeBasic() {
    std::cout << "Starting TestTaskPipeBasic...\n";
    TaskPipe pipe;
    std::atomic<int> counter{0};

    pipe.Enqueue([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        counter++;
        std::cout << "Task 1 finished (counter=" << counter << ")\n";
    });

    pipe.Enqueue([&]() {
        int val = counter.load();
        assert(val == 1);
        counter++;
        std::cout << "Task 2 finished (counter=" << counter << ")\n";
    });

    pipe.Enqueue([&]() {
        int val = counter.load();
        assert(val == 2);
        counter++;
        std::cout << "Task 3 finished (counter=" << counter << ")\n";
    });

    GraphEventRef last_event = pipe.Close();
    
    if (last_event) {
        TaskGraph::GetInterface().WaitUntilTaskComplete(last_event, EThread::EMainThread);
    }

    assert(counter == 3);
    std::cout << "TestTaskPipeBasic passed!\n\n";
}

void TestTaskPipeDependencies() {
    std::cout << "Starting TestTaskPipeDependencies...\n";
    TaskPipe pipe;
    std::atomic<int> counter{0};

    GraphEventRef external_event = GraphEvent::CreateGraphEvent();

    pipe.Enqueue([&]() {
        counter++;
        std::cout << "Dependent task finished (counter=" << counter << ")\n";
    }, {external_event});

    pipe.Enqueue([&]() {
        assert(counter == 1);
        counter++;
        std::cout << "Subsequent task finished (counter=" << counter << ")\n";
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(counter == 0);
    std::cout << "Verified tasks are waiting for external event\n";

    external_event->TryUnlockSubsequents();

    GraphEventRef last_event = pipe.Close();
    if (last_event) {
        TaskGraph::GetInterface().WaitUntilTaskComplete(last_event, EThread::EMainThread);
    }

    assert(counter == 2);
    std::cout << "TestTaskPipeDependencies passed!\n\n";
}

void TestTaskPipeCloseAndEnqueue() {
    std::cout << "Starting TestTaskPipeCloseAndEnqueue...\n";
    TaskPipe pipe;
    std::atomic<int> counter{0};

    pipe.Enqueue([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        counter++;
        std::cout << "Chain1-Task1 finished (counter=" << counter << ")\n";
    });

    pipe.Enqueue([&]() {
        int val = counter.load();
        assert(val == 1);
        counter++;
        std::cout << "Chain1-Task2 finished (counter=" << counter << ")\n";
    });

    GraphEventRef close_event = pipe.Close();
    assert(close_event);
    std::cout << "First chain closed\n";

    pipe.Enqueue([&]() {
        int val = counter.load();
        assert(val == 2);
        counter++;
        std::cout << "Chain2-Task1 finished (counter=" << counter << ")\n";
    });

    pipe.Enqueue([&]() {
        int val = counter.load();
        assert(val == 3);
        counter++;
        std::cout << "Chain2-Task2 finished (counter=" << counter << ")\n";
    });

    GraphEventRef last_event = pipe.Close();
    if (last_event) {
        TaskGraph::GetInterface().WaitUntilTaskComplete(last_event, EThread::EMainThread);
    }

    assert(counter == 4);
    std::cout << "TestTaskPipeCloseAndEnqueue passed!\n\n";
}

int main() {
    TaskSystem::Init();
    
    TestTaskPipeBasic();
    TestTaskPipeDependencies();
    TestTaskPipeCloseAndEnqueue();

    TaskSystem::ShutDown();
    return 0;
}
