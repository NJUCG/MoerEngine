#ifndef MOER_RAII_H
#define MOER_RAII_H
#include <type_traits>
namespace {
template<typename T>
struct Disposer {
private:
    std::remove_reference_t<T> dispose_func;

public:
    Disposer(T&& _dispose_func) : dispose_func(std::forward<T>(_dispose_func)) {}
    ~Disposer() {
        dispose_func();
    }
    Disposer()                           = delete;
    Disposer(const Disposer&)            = delete;
    Disposer& operator=(const Disposer&) = delete;
    Disposer(Disposer&&)                 = default;
};

template<typename T>
Disposer<T> OnScopeExit(T&& _dispose_func) {
    return Disposer<T>(std::forward<T>(_dispose_func));
}
}; // namespace
#endif