#ifndef MOER_ENGINE_STL_H
#define MOER_ENGINE_STL_H
#include <any>
#include <array>
#include <cstring>
#include <map>
#include <queue>
#include <set>
#include <stack>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "MMemory.h"
#include "m_vector/m_vector.h"
#include <deque>
#include <memory>

template<typename T>
using m_defualt_allocator = MoerStlAllocator<T>;

namespace Moer {

// SFINAE detection for GetTypeHash
namespace detail {
template<typename T, typename = void>
struct has_get_type_hash : std::false_type {};

template<typename T>
struct has_get_type_hash<T, std::void_t<decltype(GetTypeHash(std::declval<const T&>()))>> : std::true_type {};
} // namespace detail

// Moer hash: uses GetTypeHash if available, otherwise falls back to std::hash
template<typename T>
struct MoerHash {
    size_t operator()(const T& val) const noexcept {
        if constexpr (detail::has_get_type_hash<T>::value) {
            return static_cast<size_t>(GetTypeHash(val));
        } else {
            return std::hash<T>{}(val);
        }
    }
};

template<typename T, class allocator = m_defualt_allocator<T>>
using Array = std::vector<T, allocator>;

//template<typename T, class allocator = m_defualt_allocator<T>>
//using Array = m_vector<T, allocator>;

template<
    typename K,
    typename V,
    class Pr        = std::less<K>,
    class allocator = m_defualt_allocator<std::pair<const K, V>>>
using Map = std::map<K, V, Pr, allocator>;

template<
    typename K,
    typename V,
    class Hash      = MoerHash<K>,
    class KeyEqual  = std::equal_to<K>,
    class allocator = m_defualt_allocator<std::pair<const K, V>>>
using UnorderedMap = std::unordered_map<K, V, Hash, KeyEqual, allocator>;

template<typename K, class Pr = std::less<K>, class allocator = m_defualt_allocator<K>>
using Set = std::set<K, Pr, allocator>;

template<
    typename K,
    class Hash      = MoerHash<K>,
    class KeyEqual  = std::equal_to<K>,
    class allocator = m_defualt_allocator<K>>
using UnorderedSet = std::unordered_set<K, Hash, KeyEqual, allocator>;

template<typename T, size_t N>
using StaticArray = std::array<T, N>;

using Any = std::any;

template<typename T, class Deleter = MoerDeleter>
using UniquePtr = std::unique_ptr<T, Deleter>;

template<typename T>
using DEQueue = std::deque<T, m_defualt_allocator<T>>;

template<typename T>
using Queue = std::queue<T, DEQueue<T>>;

template<typename T, typename... Args>
    requires std::is_constructible_v<T, Args...>
constexpr UniquePtr<T> MakeUnique(Args&&... _args) {
    return UniquePtr<T>(MoerNew(T)(std::forward<Args>(_args)...));
}

template<typename T>
using SharedPtr = std::shared_ptr<T>;

template<typename T, typename... Args>
//requires std::is_constructible_v<T, Args...> // note(spc): fail to compile on my vs2022 17.12.0. but can run without this
constexpr SharedPtr<T> MakeShared(Args&&... _args) {
    return SharedPtr<T>(MoerNew(T)(std::forward<Args>(_args)...), MoerDelete<T>);
}

template<typename T>
constexpr bool StringEqual(const T& a, const T& b) {
    return std::strcmp(a.c_str(), b.c_str()) == 0;
}

template<typename T>
using Stack = std::stack<T, DEQueue<T>>;

//Visitor Overload Template
template<typename... Ts>
struct Overload : Ts... {
    using Ts::operator()...;
};
template<typename... Ts>
Overload(Ts...) -> Overload<Ts...>;

template<typename T, size_t N>
    requires std::is_trivially_copyable_v<T>
class CircularQueue {
private:
    StaticArray<T, N> data;
    size_t            head = 0;
    size_t            tail = 0;
    size_t            size = 0;

public:
    bool Enqueue(const T& _value) {
        if (Full()) {
            Dequeue();
        }

        data[tail] = _value;
        tail       = (tail + 1) % N;
        ++size;
        return true;
    }

    bool Enqueue(const T&& _value) {
        if (Full()) {
            Dequeue();
        }

        data[tail] = std::forward<T>(_value);
        tail       = (tail + 1) % N;
        ++size;
        return true;
    }

    bool Dequeue() {
        assert(!Empty() && "CircularQueue is empty, cannot dequeue!");
        head = (head + 1) % N;
        --size;
        return true;
    }

    T& Front() {
        assert(!Empty() && "CircularQueue is empty, cannot get front!");
        return data[head];
    }

    const T& Front() const {
        assert(!Empty() && "CircularQueue is empty, cannot get front!");
        return data[head];
    }

    T& Back() {
        assert(!Empty() && "CircularQueue is empty, cannot get back!");
        return data[(tail - 1 + N) % N];
    }

    const T& Back() const {
        assert(!Empty() && "CircularQueue is empty, cannot get back!");
        return data[(tail - 1 + N) % N];
    }

    bool Full() const {
        return size == N;
    }

    bool Empty() const {
        return size == 0;
    }

    size_t Size() const {
        return size;
    }

    constexpr size_t Capacity() const {
        return N;
    }
};

} // namespace Moer
#endif //MOER_ENGINE_STL_H