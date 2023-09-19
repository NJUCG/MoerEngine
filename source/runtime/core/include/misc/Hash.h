#ifndef HASHABLE_H
#define HASHABLE_H
#include "MacroUtils.h"
#include "PicoSHA2.h"
#include <type_traits>
#include <string_view>
#include <cstring>
#include <array>

template<typename TEnum>
concept concept_t_is_enum = std::is_enum<TEnum>::value;
template<typename TEnum>
concept concept_t_enum_underlying_uint8 = concept_t_is_enum<TEnum> && std::is_same_v<std::underlying_type_t<TEnum>, uint8_t>;
template<typename TNum>
concept concept_t_is_vec2 = requires (TNum t)
{
    t.x; t.y;
    sizeof(t.x) + sizeof(t.y) == sizeof(t);
    {t.x + t.y} -> std::same_as<typeof(t.x)>;
    {t.x - t.y} -> std::same_as<typeof(t.x)>;
    {t.x * t.y} -> std::same_as<typeof(t.x)>;
    {t.x / t.y} -> std::same_as<typeof(t.x)>;
};

template<typename TNum>
concept concept_t_is_vec3 = requires (TNum t)
{
    t.x; t.y; t.z;
    sizeof(t.x) + sizeof(t.y) + sizeof(t.z) == sizeof(t);
    {t.x + t.y} -> std::same_as<typeof(t.x)>;
    {t.x - t.y} -> std::same_as<typeof(t.x)>;
    {t.x * t.y} -> std::same_as<typeof(t.x)>;
    {t.x / t.y} -> std::same_as<typeof(t.x)>;
};

template<typename T>
inline void hash_combine(uint16_t& seed, const T& val) {
    seed ^= std::hash<T>{}(val) + 0x9e37U + (seed << 3) + (seed >> 1);
}

template<typename T>
inline void hash_combine(uint32_t& seed, const T& val) {
    seed ^= std::hash<T>{}(val) + 0x9e3779b9U + (seed << 6) + (seed >> 2);
}

template<typename T>
inline void hash_combine(uint64_t& seed, const T& val) {
    seed ^= std::hash<T>{}(val) + 0x9e3779b97f4a7c15LLU + (seed << 12) + (seed >> 4);
}

template<typename T, typename... Rest>
inline void hash_combine(uint64_t& seed, const T& v, const Rest&... rest) {
    seed ^= std::hash<T>{}(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    (hash_combine(seed, rest), ...);
}
#define MAKE_HASHABLE_64(type, ...)     \
    uint64_t hash() const {             \
        uint64_t ret = 0;               \
        hash_combine(ret, __VA_ARGS__); \
        return ret;                     \
    }
template<concept_t_enum_underlying_uint8 TEnum>
class EnumInByte {
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
    TEnum GetValue() const { return (TEnum)value; }

private:
    friend uint32_t GetHash(const EnumInByte& target);
    uint8_t         value;
};

uint32_t GetHash(uint32_t value) {
    return value;
}
uint32_t GetHash(int32_t value) {
    return value;
}
uint32_t GetHash(uint8_t value) {
    return value;
}
/*from UE5.03*/
FORCEINLINE uint32_t GetHash(uint64_t value) {
    return (uint32_t)value + ((uint32_t)(value >> 32) * 23);
}

/*from UE5.03*/
inline uint32_t GetHash(int64_t target) {
    return (uint32_t)target + ((uint32_t)(target >> 32) * 23);
}

uint32_t GetHash(float value) {
    return *(uint32_t*)&value;
}
uint32_t GetHash(double value) {
    return GetHash(*(uint64_t*)&value);
}
uint32_t GetHash(const char* value) {
    return std::hash<std::string_view>{}(std::string_view(value));
}
template<concept_t_is_vec2 T>
uint32_t GetHash(const T& value) {
    uint32_t hash = GetHash(value.x);
    hash_combine(hash, GetHash(value.y));
    return hash;
}

template<concept_t_is_vec3 T>
uint32_t GetHash(const T& value) {
    uint32_t hash = GetHash(value.x);
    hash_combine(hash, GetHash(value.y));
    hash_combine(hash, GetHash(value.z));
    return hash;
}

template<concept_t_is_enum T>
FORCEINLINE uint32_t GetHash(const T& t) {
    return GetHash((std::underlying_type_t<T>)t);
}
template<typename T>
FORCEINLINE uint32_t GetHash(const EnumInByte<T>& t) {
    return GetHash(t.value);
}
FORCEINLINE uint32_t GetHash(const std::string& value){
    return std::hash<std::string>{}(value);
}

struct SHA256Hash {
public:
    std::array<uint8_t,32> hash_code{};
    SHA256Hash() {
        for (unsigned char& i : hash_code) {
            i = 0;
        }
    }
    FORCEINLINE std::string ToString(){
        return picosha2::bytes_to_hex_string(hash_code.begin(), hash_code.end());
    }
    FORCEINLINE void FromString(std::string_view& src){
        picosha2::hash256(src, hash_code);

    }
    friend bool operator==(const SHA256Hash& lhs, const SHA256Hash& rhs){
        return std::memcmp(lhs.hash_code.data(), rhs.hash_code.data(), sizeof (lhs.hash_code)) == 0;
    }
    friend bool operator!=(const SHA256Hash& lhs, const SHA256Hash& rhs){
        return !(lhs == rhs);
    }
    friend bool operator< (const SHA256Hash& lhs, const SHA256Hash& rhs){
        return std::memcmp(lhs.hash_code.data(), rhs.hash_code.data(), sizeof (lhs.hash_code)) < 0;
    }
};
static_assert(picosha2::k_digest_size == 32);


namespace inner_utils
{
    template <typename T, std::size_t ... Is>
    constexpr std::array<T, sizeof...(Is)>
    create_array(T value, std::index_sequence<Is...>)
    {
        // cast Is to void to remove the warning: unused value
        return {{(static_cast<void>(Is), value)...}};
    }
}

template <std::size_t N, typename T>
constexpr std::array<T, N> create_array(const T& value)
{
    return inner_utils::create_array(value, std::make_index_sequence<N>());
}

#endif// !HASHABLE_H