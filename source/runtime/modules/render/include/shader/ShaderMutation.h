#ifndef MOER_ENGINE_SHADER_MUTATION_H
#define MOER_ENGINE_SHADER_MUTATION_H

#include "rhi/RHICommon.h"
#include "shader/ShaderCommon.h"
#include <stdint.h>
#include <utility>
#pragma region shader mutation

struct ShaderMutationParameters {

    const EShaderPlatform platform;
    const uint32_t        mutation_id;

    ShaderMutationParameters(EShaderPlatform _platform, uint32_t _mutation_id) : platform(_platform), mutation_id(_mutation_id) {}
};
//shader mutation basic types
template<typename T>
concept TShaderMutationBasicType = requires(T) {
    std::is_same_v<decltype(T::mutation_count), uint32_t>&& T::mutation_count > 0;
    std::is_same_v<decltype(T::has_multiple_slot), bool>;
    std::is_same_v<decltype(T::GetMutationID(std::declval<typename T::Type>())), uint32_t>;
    std::is_same_v<decltype(T::GetMutationTypeFromID(std::declval<uint32_t>())), typename T::Type>;
    std::is_same_v<decltype(T::GetMutationValueFromType(std::declval<typename T::Type>())), typename T::Type>;
};

struct ShaderMutationBool {
    using Type                                  = bool;
    static constexpr uint32_t mutation_count    = 2;
    static constexpr bool     has_multiple_slot = false;

    static uint32_t GetMutationID(Type _value) {
        return _value ? 1 : 0;
    }

    static Type GetMutationTypeFromID(uint32_t _id) {
        return _id == 1;
    }
    static bool GetMutationValueFromType(Type _value) {
        return _value;
    }
};

template<uint32_t MutationCount, int32_t start_value = 0>
struct ShaderMutationInt {
    using Type                                  = int32_t;
    static constexpr uint32_t mutation_count    = MutationCount;
    static constexpr bool     has_multiple_slot = false;

    static constexpr Type s_min_value = static_cast<Type>(start_value);
    static constexpr Type s_max_value = static_cast<Type>(start_value + MutationCount - 1);

    static_assert(std::is_integral_v<Type> && std::is_signed_v<Type> && MutationCount > 0 && s_max_value > s_min_value);

    static uint32_t GetMutationID(Type _value) {
        return static_cast<uint32_t>(_value - s_min_value);
    }

    static Type GetMutationTypeFromID(uint32_t _id) {
        return static_cast<Type>(_id + s_min_value);
    }

    static uint32_t GetMutationValueFromType(Type _value) {
        return static_cast<uint32_t>(_value);
    }
};

template<uint32_t... values>
struct ShaderMutationSparseUInt {
    using Type                                  = uint32_t;
    static constexpr uint32_t mutation_count    = 0;
    static constexpr bool     has_multiple_slot = false;

    static uint32_t GetMutationID(Type _value) {
        assert(false && "no mutation id for given value");
        return 0;
    }

    static Type GetMutationTypeFromID(uint32_t _id) {
        assert(false && "no mutation type for given id");
        return 0;
    }
};

template<uint32_t unique_value, uint32_t... values>
struct ShaderMutationSparseUInt<unique_value, values...> {
    using Type                                  = uint32_t;
    static constexpr uint32_t mutation_count    = ShaderMutationSparseUInt<values...>::mutation_count + 1;
    static constexpr bool     has_multiple_slot = false;

    static uint32_t GetMutationID(Type _value) {
        if (_value == unique_value) {
            return mutation_count - 1;//index of unique value
        }
        return ShaderMutationSparseUInt<values...>::GetMutationID(_value);
    }

    static Type GetMutationTypeFromID(uint32_t _id) {
        if (_id == mutation_count - 1) {
            return unique_value;
        }
        return ShaderMutationSparseUInt<values...>::GetMutationTypeFromID(_id);
    }

    static uint32_t GetMutationValueFromType(Type _value) {
        return _value;
    }
};

// };

template<class TTarget, typename TTuple, uint32_t index = 0>
consteval uint32_t GetTypeIndex() {
    if constexpr (std::is_same_v<TTarget, std::tuple_element_t<index, TTuple>>) {
        return index;
    } else {
        return GetTypeIndex<TTarget, TTuple, index + 1>();
    }
    return index;
}

template<TShaderMutationBasicType... Types>
struct TShaderMutationSet {
    using Type       = TShaderMutationSet<Types...>;
    using TypeSeries = std::tuple<Types...>;

    static constexpr uint32_t mutation_count = std::tuple_size_v<TypeSeries> == 0 ? 1 : (1 * ... * Types::mutation_count);

    constexpr static bool has_multiple_slot = mutation_count > 1;

    TShaderMutationSet<Types...>() {
        (... = (std::get<GetTypeIndex<Types, TypeSeries>()>(mutation_values) = Types::GetMutationTypeFromID(0)));
    }
    explicit TShaderMutationSet(uint32_t _mutation_id)// mutation_value(TMutation::GetMutationTypeFromID(_mutation_id % TMutation::mutation_count)), next_set(_mutation_id / TMutation::mutation_count)
    {
        if constexpr (mutation_count > 1) {
            assert(_mutation_id >= 0 && "invalid mutation id");
            uint32_t temp_id = _mutation_id;

            (..., (std::get<GetTypeIndex<Types, TypeSeries>()>(mutation_values) = Types::GetMutationTypeFromID(temp_id % Types::mutation_count), temp_id /= Types::mutation_count));
        }
    }

    template<TShaderMutationBasicType TMutationToSet>
    void SetMutation(typename TMutationToSet::Type _value) {

        std::get<GetTypeIndex<TMutationToSet, TypeSeries>()>(mutation_values) = _value;
    }

    void SetCompileEnvironment(ShaderCompilerEnvironment& _environment) const {
        if constexpr (has_multiple_slot) {
            (..., (_environment.SetDefine(Types::mutation_name, GetMutationValue<Types>())));
        }
    }

    uint32_t GetMutationID() const {
        uint32_t multiple   = 1;
        uint32_t temp_count = 0;
        (... = (temp_count *= multiple, temp_count += Types::GetMutationID(std::get<GetTypeIndex<Types, TypeSeries>()>(mutation_values)), multiple = Types::mutation_count));

        return temp_count;
    }

    static Type GetMutationTypeFromID(uint32_t _id) {
        return Type(_id);
    }

    template<TShaderMutationBasicType TMutationToGet>
    typename TMutationToGet::Type GetMutationValue() const {
        return std::get<GetTypeIndex<TMutationToGet, TypeSeries>()>(mutation_values);
    }

    // TMutation::Type mutation_value;

    std::tuple<typename Types::Type...> mutation_values;
    // TNextSet        next_set;
    //build a static linked list
};

template<>
struct TShaderMutationSet<> {
    using Type = TShaderMutationSet<>;

    constexpr static bool has_multiple_slot = true;

    constexpr static uint32_t mutation_count = 1;

    TShaderMutationSet<>() {}
    explicit TShaderMutationSet<>(uint32_t _mutation_id) {
        assert(_mutation_id == 0 && "invalid mutation id");
    }

    template<TShaderMutationBasicType TMutationType>
    typename TMutationType::Type GetMutationValue() const {
        static_assert(sizeof(typename TMutationType::Type) == 0);
        return TMutationType::Type();
    }

    template<TShaderMutationBasicType TMutationType>
    void SetMutation(typename TMutationType::Type _value) {
        static_assert(sizeof(typename TMutationType::Type) == 0);
    }

    void SetCompileEnvironment(ShaderCompilerEnvironment& _environment) const {
        //last set, do nothing
    }

    uint32_t GetMutationID() const {
        return 0;
    }

    static Type GetMutationTypeFromID(uint32_t _id) {
        return Type(_id);
    }
};

using TShaderMutationSetEmpty = TShaderMutationSet<>;

#define MUTATION_BOOL(ClassName)                                 \
    class ClassName : public ShaderMutationBool {                \
    public:                                                      \
        static constexpr char const* mutation_name = #ClassName; \
    }
#define MUTATION_INT(ClassName, MutationCount, StartValue)                  \
    class ClassName : public ShaderMutationInt<MutationCount, StartValue> { \
    public:                                                                 \
        static constexpr char const* mutation_name = #ClassName;            \
    }

#define MUTATION_SPARSE_UINT(ClassName, ...)                         \
    class ClassName : public ShaderMutationSparseUInt<__VA_ARGS__> { \
    public:                                                          \
        static constexpr char const* mutation_name = #ClassName;     \
    }
#define DEFINE_MUTATION_SET(...) using TMutationSet = TShaderMutationSet<__VA_ARGS__>
#define MUTATION_SET(StructName, ...) \
    struct StructName : public TShaderMutationSet<__VA_ARGS__> {}
#pragma endregion
#endif