#ifndef MOER_ENGINE_STL_H
#define MOER_ENGINE_STL_H
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <array>

#include "MMemory.h"
#include <memory>

#if USE_MIMALLOC
// #if 0

#include <mimalloc.h>

template<typename T>
using m_defualt_allocator = mi_stl_allocator<T>;
#else
template<typename T>
using m_defualt_allocator = std::allocator<T>;
#endif
namespace Moer {
    template<typename T, class allocator = m_defualt_allocator<T>>
    using Array = std::vector<T, allocator>;

    template<typename K, typename V, class Pr = std::less<K>, class allocator = m_defualt_allocator<std::pair<const K, V>>>
    using Map = std::map<K, V, Pr, allocator>;

    template<typename K, typename V, class Hash = std::hash<K>, class KeyEqual = std::equal_to<K>, class allocator = m_defualt_allocator<std::pair<const K, V>>>
    using UnorderedMap = std::unordered_map<K, V, Hash, KeyEqual, allocator>;

    template<typename K, class Pr = std::less<K>, class allocator = m_defualt_allocator<const K>>
    using Set = std::set<K, Pr, allocator>;

    template<typename K, class Hash = std::hash<K>, class KeyEqual = std::equal_to<K>, class allocator = m_defualt_allocator<K>>
    using UnorderedSet = std::unordered_set<K, Hash, KeyEqual, allocator>;

    template<typename T, size_t N>
    using StaticArray = std::array<T, N>;

    template<typename T, class Deleter = MoerDeleter>
    using UniquePtr = std::unique_ptr<T, Deleter>;

}// namespace Moer
#endif//MOER_ENGINE_STL_H