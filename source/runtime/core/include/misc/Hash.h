#ifndef HASHABLE_H
#define HASHABLE_H

#include "API_Macro.h"
#include "MacroUtils.h"

#include "misc/STL.h"

#include "misc/Types.h"
#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <type_traits>

template<typename TEnum>
concept concept_t_is_enum = std::is_enum<TEnum>::value;
template<typename TEnum>
concept concept_t_enum_underlying_uint8 =
    concept_t_is_enum<TEnum> && std::is_same_v<std::underlying_type_t<TEnum>, Moer::uint8>;
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
inline void HashCombine(Moer::uint16& seed, const T& val) {
    seed ^= std::hash<T>{}(val) + 0x9e37U + (seed << 3) + (seed >> 1);
}

template<typename T>
inline void HashCombine(Moer::uint32& seed, const T& val) {
    seed ^= std::hash<T>{}(val) + 0x9e3779b9U + (seed << 6) + (seed >> 2);
}

template<typename T>
inline void HashCombine(Moer::uint64& seed, const T& val) {
    seed ^= std::hash<T>{}(val) + 0x9e3779b97f4a7c15LLU + (seed << 12) + (seed >> 4);
}

template<typename T, typename... Rest>
inline void HashCombine(Moer::uint64& seed, const T& v, const Rest&... rest) {
    seed ^= std::hash<T>{}(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    (HashCombine(seed, rest), ...);
}
#define MAKE_HASHABLE_64(type, ...)      \
    CORE_API Moer::uint64 Hash() const { \
        Moer::uint64 ret = 0;            \
        HashCombine(ret, __VA_ARGS__);   \
        return ret;                      \
    }

template<concept_t_enum_underlying_uint8 TEnum>
class EnumInByte;

inline Moer::uint32 GetHash(Moer::uint32 value) {
    return value;
}
inline Moer::uint32 GetHash(Moer::int32 value) {
    return value;
}

/*from UE5.03*/
FORCEINLINE Moer::uint32 GetHash(Moer::uint64 value) {
    return (Moer::uint32)value + ((Moer::uint32)(value >> 32) * 23);
}

/*from UE5.03*/
inline Moer::uint32 GetHash(Moer::int64 target) {
    return (Moer::uint32)target + ((Moer::uint32)(target >> 32) * 23);
}

inline Moer::uint32 GetHash(float value) {
    return *(Moer::uint32*)&value;
}
inline Moer::uint32 GetHash(double value) {
    return GetHash(*(Moer::uint64*)&value);
}
inline Moer::uint32 GetHash(const char* value) {
    return std::hash<std::string_view>{}(std::string_view(value));
}
template<concept_t_is_vec2 T>
Moer::uint32 GetHash(const T& value) {
    Moer::uint32 hash = GetHash(value.x);
    HashCombine(hash, GetHash(value.y));
    return hash;
}

template<concept_t_is_vec3 T>
Moer::uint32 GetHash(const T& value) {
    Moer::uint32 hash = GetHash(value.x);
    HashCombine(hash, GetHash(value.y));
    HashCombine(hash, GetHash(value.z));
    return hash;
}

template<concept_t_is_enum T>
Moer::uint32 GetHash(const T& t) {
    return GetHash((std::underlying_type_t<T>)t);
}
template<typename T>
FORCEINLINE Moer::uint32 GetHash(const EnumInByte<T>& t) {
    return GetHash(t.value);
}
FORCEINLINE Moer::uint32 GetHash(const std::string& value) {
    return std::hash<std::string>{}(value);
}

FORCEINLINE Moer::uint32 GetHash(const std::string_view& value) {
    return std::hash<std::string_view>{}(value);
}

template<concept_t_enum_underlying_uint8 TEnum>
class EnumInByte {
public:
    EnumInByte()                        = default;
    EnumInByte(const EnumInByte& other) = default;
    EnumInByte(TEnum _enum_value) : value(static_cast<Moer::uint8>(_enum_value)) {}
    EnumInByte(Moer::uint8 _value) : value(_value) {}
    EnumInByte(Moer::uint32 _value) : value(static_cast<Moer::uint8>(_value)) {}
    EnumInByte(Moer::int32 _value) : value(static_cast<Moer::uint8>(_value)) {}
    EnumInByte& operator=(const EnumInByte&) = default;

    operator TEnum() const {
        return (TEnum)value;
    }
    bool operator==(const EnumInByte& other) {
        return other.value == value;
    }
    bool operator==(TEnum other) {
        return (TEnum)value == other;
    }
    bool operator==(Moer::uint8 _value) {
        return _value == value;
    }
    inline TEnum GetValue() const {
        return (TEnum)value;
    }
    friend Moer::uint32 inline GetHash(const EnumInByte& target) {
        return GetHash(target.value);
    };

private:
    Moer::uint8 value;
};

struct SHA256Hash {
public:
    Moer::StaticArray<Moer::uint8, 32> hash_code{};
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
    Moer::StaticArray<Moer::uint8, 8> hash_code{};

    Hash64City() {
        for (unsigned char& i : hash_code) {
            i = 0;
        }
    }

    std::string ToString();
    void        FromString(std::string_view& src);
    void        FromData(const Moer::uint8* data, size_t size);
    void        Update(std::string_view& src);
    void        Update(const Moer::uint8* data, Moer::uint32 size);
    //todo: Update() not utterly correct
    void        Update(const char* data, Moer::uint32 size);
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
constexpr Moer::StaticArray<T, sizeof...(Is)> CreateArray(T value, std::index_sequence<Is...>) {
    // cast Is to void to remove the warning: unused value
    return {{(static_cast<void>(Is), value)...}};
}
} // namespace inner_utils

template<std::size_t N, typename T>
constexpr Moer::StaticArray<T, N> CreateArray(const T& value) {
    return inner_utils::CreateArray(value, std::make_index_sequence<N>());
}

class HashedName {
    friend struct std::equal_to<HashedName>;
    friend struct std::hash<HashedName>;
    const char* value;

    static std::atomic<Moer::uint32> s_size;

    static std::shared_mutex                 s_rw_mutex;
    static Moer::Map<const char*, Moer::uint32>& GetNameToHash() {
        static Moer::Map<const char*, Moer::uint32> s_name_to_hash;
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
    friend Moer::uint32 GetHash(const HashedName& value) {
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
    inline Moer::uint32 RegisterName() const {
        {
            std::shared_lock<std::shared_mutex> read_lock(s_rw_mutex);
            if (const auto& iter = GetNameToHash().find(value); iter != GetNameToHash().end()) {
                //found
                return iter->second;
            }
        }
        {
            std::unique_lock<std::shared_mutex> write_lock(s_rw_mutex);
            Moer::uint32                        index = s_size.fetch_add(1) + 1;
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

#endif // !HASHABLE_H