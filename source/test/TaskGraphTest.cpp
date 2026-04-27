#include "taskgraph/TaskGraph.h"
#include "Core.h"
#include "misc/LockFree.h"
#include "misc/MMemory.h"
#include "misc/STL.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskSystem.h"
#include "taskgraph/ThreadManager.h"
#include <atomic>
#include <cassert>
#include <functional>

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

class TestFunctionRunnable : public Runnable {
public:
    explicit TestFunctionRunnable(std::function<void()>&& in_function) : m_function(std::move(in_function)) {}

    uint32_t Run() override {
        m_function();
        return 0;
    }

    void Init() override {}
    void Stop() override {}
    void Exit() override {}
    ThreadIndex GetIndex() override {
        return EThread::UNKNOWN_THREAD;
    }

private:
    std::function<void()> m_function;
};

class ScopedTestThread {
public:
    ScopedTestThread(Moer::Utf8StringView name, std::function<void()>&& function) :
        m_runnable(std::move(function)),
        m_thread(RunnableThread::Create(
            &m_runnable,
            ThreadAttributes{.affinity = Affinity{}, .name = name}
        )) {}

    ~ScopedTestThread() {
        if (m_thread != nullptr) {
            MoerDelete(m_thread);
            m_thread = nullptr;
        }
    }

    void Join() {
        if (m_thread != nullptr) {
            m_thread->Join();
        }
    }

private:
    TestFunctionRunnable m_runnable;
    RunnableThread*      m_thread{nullptr};
};

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

    ScopedTestThread t2(MOER_ASCII_TEXT("LockFreeStackPush0"), [&]() {
        for (int i = 0; i < 1000; ++i) {
            stack.Push(&a);
        }
    });

    ScopedTestThread t4(MOER_ASCII_TEXT("LockFreeStackPush1"), [&]() {
        for (int i = 0; i < 1099; ++i) {
            stack.Push(&a);
        }
    });

    //pop

    ScopedTestThread t5(MOER_ASCII_TEXT("LockFreeStackPop0"), [&]() {
        for (int i = 0; i < 1000; ++i) {
            stack.Pop();
        }
    });

    ScopedTestThread t6(MOER_ASCII_TEXT("LockFreeStackPop1"), [&]() {
        for (int i = 0; i < 1000; ++i) {
            stack.Pop();
        }
    });
}

void LockFreeQueueTest() {
    LockFreeQueueBase<class A> queue;

    A a;
    queue.Push(&a);

    ScopedTestThread t2(MOER_ASCII_TEXT("LockFreeQueuePush0"), [&]() {
        for (int i = 0; i < 1000; ++i) {
            queue.Push(&a);
        }
    });

    ScopedTestThread t4(MOER_ASCII_TEXT("LockFreeQueuePush1"), [&]() {
        for (int i = 0; i < 1099; ++i) {
            queue.Push(&a);
        }
    });

    //pop

    ScopedTestThread t5(MOER_ASCII_TEXT("LockFreeQueuePop0"), [&]() {
        for (int i = 0; i < 1000; ++i) {
            queue.Pop();
        }
    });

    ScopedTestThread t6(MOER_ASCII_TEXT("LockFreeQueuePop1"), [&]() {
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

    ScopedTestThread t1(MOER_ASCII_TEXT("ClosableStackPush0"), std::function<void()>(push_operation));
    ScopedTestThread t8(MOER_ASCII_TEXT("ClosableStackConsume"), std::function<void()>(consume_operation));
    ScopedTestThread t2(MOER_ASCII_TEXT("ClosableStackPush1"), std::function<void()>(push_operation));
    ScopedTestThread t3(MOER_ASCII_TEXT("ClosableStackPush2"), std::function<void()>(push_operation));
    ScopedTestThread t4(MOER_ASCII_TEXT("ClosableStackPush3"), std::function<void()>(push_operation));
    ScopedTestThread t5(MOER_ASCII_TEXT("ClosableStackPush4"), std::function<void()>(push_operation));
    ScopedTestThread t6(MOER_ASCII_TEXT("ClosableStackPush5"), std::function<void()>(push_operation));
    ScopedTestThread t7(MOER_ASCII_TEXT("ClosableStackPush6"), std::function<void()>(push_operation));

    ScopedTestThread t9(MOER_ASCII_TEXT("LockFreeQueueEnqueue"), std::function<void()>(queue_enque_operation));
    ScopedTestThread t10(MOER_ASCII_TEXT("LockFreeQueueDequeue"), std::function<void()>(queue_deque_operation));

    t1.Join();
    t2.Join();
    t3.Join();
    t4.Join();
    t5.Join();
    t6.Join();
    t7.Join();
    t8.Join();

    t9.Join();
    t10.Join();

    assert(push_count == array.size());
    std::reverse(array.begin(), array.end());
    for (auto& i : array) {
        assert(i != nullptr);
        assert(i->a == 1 && i->b == 2 && i->c == 3 && i->d == 4);
    }
    LOG_INFO(MOER_TEXT("push count: {}"), push_count.load());
}
int main() {
    Moer::TaskSystem::Init();

    Moer::TaskGraphTest();
    std::cout << "[TESTCASE][PASS] TaskGraphSuite\n";

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
