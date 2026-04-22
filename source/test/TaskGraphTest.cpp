#include "taskgraph/TaskGraph.h"
#include "Core.h"
#include "misc/LockFree.h"
#include "misc/MMemory.h"
#include "misc/STL.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskSystem.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
static void msleep(unsigned long msecs) {
    Sleep(msecs);
}
#else
#include <unistd.h>
static void msleep(unsigned long msecs) {
    usleep(msecs * 1000UL);
}
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <mimalloc-new-delete.h>
#endif
struct A {
    explicit A() {
        a = 1;
        b = 2;
        c = 3;
        d = 4;
    }
    int a;
    int b;
    int c;
    int d;
};

void LockFreeStackTest() {
    LockFreeQueueBase<class A> stack;

    A a;
    stack.Push(MoerNew(A)());

    std::jthread t2([&]() {
        for (int i = 0; i < 1000; ++i) {
            stack.Push(&a);
        }
    });

    std::jthread t4([&]() {
        for (int i = 0; i < 1099; ++i) {
            stack.Push(&a);
        }
    });

    //pop

    std::jthread t5([&]() {
        for (int i = 0; i < 1000; ++i) {
            stack.Pop();
        }
    });

    std::jthread t6([&]() {
        for (int i = 0; i < 1000; ++i) {
            stack.Pop();
        }
    });
}

void LockFreeQueueTest() {
    LockFreeQueueBase<class A> queue;

    A a;
    queue.Push(&a);

    std::jthread t2([&]() {
        for (int i = 0; i < 1000; ++i) {
            queue.Push(&a);
        }
    });

    std::jthread t4([&]() {
        for (int i = 0; i < 1099; ++i) {
            queue.Push(&a);
        }
    });

    //pop

    std::jthread t5([&]() {
        for (int i = 0; i < 1000; ++i) {
            queue.Pop();
        }
    });

    std::jthread t6([&]() {
        for (int i = 0; i < 1000; ++i) {
            queue.Pop();
        }
    });
}

void ClosableMpScStackTest() {
    ClosableLockFreeMpScStack<A> stack;
    LockFreeQueueBase<A>         queue;
    // StatMPSCQueue<A*, 40086>     stack1;
    Moer::Array<A*> array;
    A               a;

    std::atomic_int32_t push_count    = 0;
    std::atomic_int32_t consume_count = 0;

    auto push_operation = [&]() {
        for (int i = 0; i < 1; ++i) {
            bool b_pushed = stack.TryPush(&a);
            if (b_pushed)
                push_count++;
            if (!b_pushed) {
                assert(stack.IsClosed());
            }
        }
    };

    auto consume_operation = [&]() {
        stack.ComsumeAllAndClose(array);
        assert(stack.IsClosed());
    };

    auto queue_enque_operation = [&]() {
        for (int i = 0; i < 10; ++i) {
            queue.Push(&a);
        }
    };

    auto queue_deque_operation = [&]() {
        for (int i = 0; i < 10; ++i) {
            queue.Pop();
        }
    };

    std::jthread t1(push_operation);
    std::jthread t8(consume_operation);
    std::jthread t2(push_operation);
    std::jthread t3(push_operation);
    std::jthread t4(push_operation);
    std::jthread t5(push_operation);
    std::jthread t6(push_operation);
    std::jthread t7(push_operation);

    std::jthread t9(queue_enque_operation);
    std::jthread t10(queue_deque_operation);

    t1.join();
    t2.join();
    t3.join();
    t4.join();
    t5.join();
    t6.join();
    t7.join();
    t8.join();

    t9.join();
    t10.join();

    assert(push_count == array.size());
    std::reverse(array.begin(), array.end());
    for (auto& i : array) {
        assert(i != nullptr);
        assert(i->a == 1 && i->b == 2 && i->c == 3 && i->d == 4);
    }
    LOG_INFO("push count: {}", push_count.load());
}
int main() {
    Moer::TaskSystem::Init();

    Moer::TaskGraphTest();
    std::cout << "[TESTCASE][PASS] TaskGraphSuite\n";

#if defined(MOER_CORE_ONLY) && defined(PLATFORM_LINUX)
    std::cout << "[TESTCASE][SKIP] ParallelFor :: skipped in linux core-only mode\n";
    std::cout << "[TESTCASE][SKIP] ParallelForAsync :: skipped in linux core-only mode\n";
    Moer::TaskSystem::ShutDown();
    return 0;
#endif

    std::atomic<int> k = 0;
    ParallelFor(10, [&](int _idx) {
        k += 60;
    });
    assert(k == 600);
    std::cout << "[TESTCASE][PASS] ParallelFor\n";

    auto finished = ParallelForAsync(10, [&](int _idx) {
        k += 60;
    });
    TaskGraph::GetInterface().WaitUntilTaskComplete(finished, EThread::EMainThread);
    assert(k == 1200);
    std::cout << "[TESTCASE][PASS] ParallelForAsync\n";

    Moer::TaskSystem::ShutDown();
    return 0;
}
