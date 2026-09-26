#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace Moer {

#if defined(__APPLE__)
// Apple libc++ does not provide std::atomic<std::shared_ptr<T>> on this SDK.
// The shared_ptr atomic free functions preserve acquire/release publication.
template<typename T>
class AtomicSharedPtr {
public:
    AtomicSharedPtr() = default;
    explicit AtomicSharedPtr(std::shared_ptr<T> initial) : value(std::move(initial)) {}

    std::shared_ptr<T> load(std::memory_order order) const noexcept {
        return std::atomic_load_explicit(&value, order);
    }

    void store(std::shared_ptr<T> next, std::memory_order order) noexcept {
        std::atomic_store_explicit(&value, std::move(next), order);
    }

    bool compare_exchange_weak(
        std::shared_ptr<T>& expected,
        std::shared_ptr<T> desired,
        std::memory_order success,
        std::memory_order failure
    ) noexcept {
        return std::atomic_compare_exchange_weak_explicit(
            &value, &expected, std::move(desired), success, failure
        );
    }

private:
    std::shared_ptr<T> value{};
};
#else
template<typename T>
using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;
#endif

} // namespace Moer
