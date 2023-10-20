#ifndef HASHABLE_H
#define HASHABLE_H
#include "API_Macro.h"
#include "MacroUtils.h"
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string.h>
#include <type_traits>
#include <string_view>
#include <cstring>
#include <array>
#include <cstdint>
#include <string>
#include <shared_mutex>

template<typename TEnum>
concept concept_t_is_enum = std::is_enum<TEnum>::value;
template<typename TEnum>
concept concept_t_enum_underlying_uint8 = concept_t_is_enum<TEnum> && std::is_same_v<std::underlying_type_t<TEnum>, uint8_t>;
template<typename TNum>
concept concept_t_is_vec2 = requires(TNum t) {
    t.x;
    t.y;

    sizeof(t.x) + sizeof(t.y) == sizeof(t);
    t.x + t.y;
    t.x - t.y;
    t.x* t.y;
    t.x / t.y;
};

template<typename TNum>
concept concept_t_is_vec3 = requires(TNum t) {
    t.x;
    t.y;
    t.z;
    sizeof(t.x) + sizeof(t.y) + sizeof(t.z) == sizeof(t);
    t.x + t.y;
    t.x - t.y;
    t.x* t.y;
    t.x / t.y;
};

template<typename T>
CORE_API inline void HashCombine(uint16_t& seed, const T& val) {
    seed ^= std::hash<T>{}(val) + 0x9e37U + (seed << 3) + (seed >> 1);
}

template<typename T>
CORE_API inline void HashCombine(uint32_t& seed, const T& val) {
    seed ^= std::hash<T>{}(val) + 0x9e3779b9U + (seed << 6) + (seed >> 2);
}

template<typename T>
CORE_API inline void HashCombine(uint64_t& seed, const T& val) {
    seed ^= std::hash<T>{}(val) + 0x9e3779b97f4a7c15LLU + (seed << 12) + (seed >> 4);
}

template<typename T, typename... Rest>
CORE_API inline void HashCombine(uint64_t& seed, const T& v, const Rest&... rest) {
    seed ^= std::hash<T>{}(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    (HashCombine(seed, rest), ...);
}
#define MAKE_HASHABLE_64(type, ...)    \
    CORE_API uint64_t Hash() const {   \
        uint64_t ret = 0;              \
        HashCombine(ret, __VA_ARGS__); \
        return ret;                    \
    }
template<concept_t_enum_underlying_uint8 TEnum>
class CORE_API EnumInByte {
public:
    EnumInByte()                        = default;
    EnumInByte(const EnumInByte& other) = default;
    EnumInByte(TEnum _enum_value) : value(static_cast<uint8_t>(_enum_value)) {}
    EnumInByte(uint8_t _value) : value(_value) {}
    EnumInByte(uint32_t _value) : value(static_cast<uint8_t>(_value)) {}
    EnumInByte(int32_t _value) : value(static_cast<uint8_t>(_value)) {}
    EnumInByte& operator=(const EnumInByte&) = default;

    operator TEnum() const { return (TEnum)value; }
    bool operator==(const EnumInByte& other) {
        return other.value == value;
    }
    bool operator==(TEnum other) {
        return (TEnum)value == other;
    }
    bool operator==(uint8_t _value) {
        return _value == value;
    }
    TEnum           GetValue() const { return (TEnum)value; }
    friend uint32_t CORE_API inline GetHash(const EnumInByte& target) {
        return GetHash(target.value);
    };

private:
    uint8_t value;
};

CORE_API inline uint32_t GetHash(uint32_t value) {
    return value;
}
CORE_API inline uint32_t GetHash(int32_t value) {
    return value;
}
CORE_API inline uint32_t GetHash(uint8_t value) {
    return value;
}
/*from UE5.03*/
CORE_API FORCEINLINE uint32_t GetHash(uint64_t value) {
    return (uint32_t)value + ((uint32_t)(value >> 32) * 23);
}

/*from UE5.03*/
CORE_API inline uint32_t GetHash(int64_t target) {
    return (uint32_t)target + ((uint32_t)(target >> 32) * 23);
}

CORE_API inline uint32_t GetHash(float value) {
    return *(uint32_t*)&value;
}
CORE_API inline uint32_t GetHash(double value) {
    return GetHash(*(uint64_t*)&value);
}
CORE_API inline uint32_t GetHash(const char* value) {
    return std::hash<std::string_view>{}(std::string_view(value));
}
template<concept_t_is_vec2 T>
CORE_API uint32_t GetHash(const T& value) {
    uint32_t hash = GetHash(value.x);
    HashCombine(hash, GetHash(value.y));
    return hash;
}

template<concept_t_is_vec3 T>
CORE_API uint32_t GetHash(const T& value) {
    uint32_t hash = GetHash(value.x);
    HashCombine(hash, GetHash(value.y));
    HashCombine(hash, GetHash(value.z));
    return hash;
}

template<concept_t_is_enum T>
CORE_API uint32_t GetHash(const T& t) {
    return GetHash((std::underlying_type_t<T>)t);
}
template<typename T>
CORE_API FORCEINLINE uint32_t GetHash(const EnumInByte<T>& t) {
    return GetHash(t.value);
}
CORE_API FORCEINLINE uint32_t GetHash(const std::string& value) {
    return std::hash<std::string>{}(value);
}

struct SHA256Hash {
public:
    std::array<uint8_t, 32> hash_code{};
    SHA256Hash() {
        for (unsigned char& i : hash_code) {
            i = 0;
        }
    }
    std::string ToString();
    void        FromString(std::string_view& src);
    friend bool operator==(const SHA256Hash& lhs, const SHA256Hash& rhs) {
        return std::memcmp(lhs.hash_code.data(), rhs.hash_code.data(), sizeof(lhs.hash_code)) == 0;
    }
    friend bool operator!=(const SHA256Hash& lhs, const SHA256Hash& rhs) {
        return !(lhs == rhs);
    }
    friend bool operator<(const SHA256Hash& lhs, const SHA256Hash& rhs) {
        return std::memcmp(lhs.hash_code.data(), rhs.hash_code.data(), sizeof(lhs.hash_code)) < 0;
    }
};

struct CORE_API Hash64City {
public:
    std::array<uint8_t, 8> hash_code{};

    Hash64City() {
        for (unsigned char& i : hash_code) {
            i = 0;
        }
    }

    std::string ToString();
    void        FromString(std::string_view& src);
    void        FromData(const uint8_t* data, size_t size);
    void        Update(std::string_view& src);
    void        Update(const uint8_t* data, uint32_t size);
    //todo: Update() not utterly correct
    void        Update(const char* data, uint32_t size);
    friend bool operator==(const Hash64City& lhs, const Hash64City& rhs) {
        return std::memcmp(lhs.hash_code.data(), rhs.hash_code.data(), sizeof(lhs.hash_code)) == 0;
    }
    friend bool operator!=(const Hash64City& lhs, const Hash64City& rhs) {
        return !(lhs == rhs);
    }
    friend bool operator<(const Hash64City& lhs, const Hash64City& rhs) {
        return std::memcmp(lhs.hash_code.data(), rhs.hash_code.data(), sizeof(lhs.hash_code)) < 0;
    }
};
static_assert(sizeof(Hash64City) == 8);

namespace inner_utils {
    template<typename T, std::size_t... Is>
    constexpr std::array<T, sizeof...(Is)>
    CreateArray(T value, std::index_sequence<Is...>) {
        // cast Is to void to remove the warning: unused value
        return {{(static_cast<void>(Is), value)...}};
    }
}// namespace inner_utils

template<std::size_t N, typename T>
constexpr std::array<T, N> CreateArray(const T& value) {
    return inner_utils::CreateArray(value, std::make_index_sequence<N>());
}

class HashedName {
    friend struct std::equal_to<HashedName>;
    friend struct std::hash<HashedName>;
    const char* value;

    static std::atomic_uint32_t s_size;

    static std::shared_mutex                s_rw_mutex;
    static std::map<const char*, uint32_t>& GetNameToHash() {
        static std::map<const char*, uint32_t> s_name_to_hash;
        return s_name_to_hash;
    }

public:
    HashedName(const char* _value) : value(_value) {
        RegisterName();
    }
    HashedName(const HashedName& other) : value(other.value) {}
    HashedName(HashedName&& other) = default;

    operator const char*() const {
        return value;
    }
    friend uint32_t GetHash(const HashedName& value) {
        {
            std::shared_lock<std::shared_mutex> read_lock(s_rw_mutex);
            if (const auto& iter = GetNameToHash().find(value); iter != GetNameToHash().end()) {
                return iter->second;
            }
        }
        return value.RegisterName();
    }

private:
    //thread safe
    inline uint32_t RegisterName() const {
        {
            std::shared_lock<std::shared_mutex> read_lock(s_rw_mutex);
            if (const auto& iter = GetNameToHash().find(value); iter != GetNameToHash().end()) {
                //found
                return iter->second;
            }
        }
        {
            std::unique_lock<std::shared_mutex> write_lock(s_rw_mutex);
            uint32_t                            index = s_size.fetch_add(1) + 1;
            GetNameToHash().insert({value, index});
            return index;
        }
    }
};

// namespace std {
//     template<>
//     class hash<HashedName> {
//     public:
//         size_t operator()(const HashedName& value) const {
//             return GetHash(value);
//         }
//     };
//     template<>
//     struct equal_to<HashedName> {
//     public:
//         bool operator()(const HashedName& lhs, const HashedName& rhs) const {
//             return strcmp(lhs.value, rhs.value) == 1;
//         }
//     };
// }// namespace std

#endif// !HASHABLE_H