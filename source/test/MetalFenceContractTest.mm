#include "rhi/backends/metal/MetalResource.h"

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace Moer::Render;
using namespace std::chrono_literals;

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void CheckPendingWait(bool reject) {
    MetalFence native_fence;
    Fence&     fence = native_fence;
    auto       waiter = std::async(std::launch::async, [&] { return fence.Wait(1); });
    try {
        fence.MarkSubmitted(1);
        Check(waiter.wait_for(20ms) == std::future_status::timeout,
              "submission acceptance incorrectly completed the wait");
        if (reject) {
            fence.Reject(1);
        } else {
            native_fence.Complete(1);
        }
        Check(waiter.wait_for(1s) == std::future_status::ready, "terminal wait did not return");
        Check(waiter.get() == !reject, "wait result did not distinguish completion from rejection");
    } catch (...) {
        fence.Reject(1); // Release the waiting thread before destroying the fixture.
        throw;
    }
}
} // namespace

int main() {
    // MetalFence's CPU bookkeeping can verify the common Fence API without a
    // graphics device; Vulkan's native-wait cases live in its integration suite.
    CheckPendingWait(false);
    CheckPendingWait(true);

    MetalFence native_fence;
    Fence&     fence = native_fence;
    Check(fence.Wait(0), "the initial value was not complete");
    fence.Reject(1);
    native_fence.Complete(2);
    Check(!fence.Wait(1), "a later completion hid an exact rejected value");
    Check(fence.Wait(2), "a successful later value inherited rejection");
    std::cout << "Fence wait success, rejection and submission separation passed\n";
}
