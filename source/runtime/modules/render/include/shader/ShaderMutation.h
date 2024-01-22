#ifndef MOER_ENGINE_SHADER_MUTATION_H
#define MOER_ENGINE_SHADER_MUTATION_H

#include "shader/ShaderCommon.h"
#include <stdint.h>
#pragma region shader mutation

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

template<typename IType, uint32_t MutationCount, uint32_t start_value = 0>
struct ShaderMutationUInt {
    using Type                                  = IType;
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

    // static uint32_t GetMutationValueFromType(Type _value) {
    //     assert(false && "no mutation value for given type");
    //     return 0;
    // }
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

template<typename... Types>
struct TShaderMutationSet {
    using Type = TShaderMutationSet<Types...>;

    constexpr static bool has_multiple_slot = true;

    constexpr static uint32_t mutation_count = 1;

    TShaderMutationSet<Types...>() {}
    explicit TShaderMutationSet<Types...>(uint32_t _mutation_id) {
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
};

template<TShaderMutationBasicType TMutation, typename... Types>
struct TShaderMutationSet<TMutation, Types...> {
    using Type = TShaderMutationSet<TMutation, Types...>;

    using TNextSet = TShaderMutationSet<Types...>;

    static constexpr uint32_t mutation_count = TNextSet::mutation_count * TMutation::mutation_count;

    constexpr static bool has_multiple_slot = true;

    TShaderMutationSet<TMutation, Types...>() : mutation_value(TMutation::GetMutationTypeFromID(0)) {}
    explicit TShaderMutationSet<TMutation, Types...>(uint32_t _mutation_id) : mutation_value(TMutation::GetMutationTypeFromID(_mutation_id % TMutation::mutation_count)), next_set(_mutation_id / TMutation::mutation_count) {
        assert(_mutation_id == 0 && "invalid mutation id");
    }

    template<TShaderMutationBasicType TMutationToGet>
    typename TMutationToGet::Type GetMutationValue() const {
        if constexpr (std::is_same_v<TMutationToGet, TMutation>) {
            return TMutationToGet::GetMutationValueFromType(mutation_value);
        } else {
            return next_set.template GetMutationValue<TMutationToGet>();
        }
    }

    template<TShaderMutationBasicType TMutationToSet>
    void SetMutation(typename TMutationToSet::Type _value) {
        if constexpr (std::is_same_v<TMutationToSet, TMutation>) {
            mutation_value = TMutationToSet::GetMutationValueFromType(_value);
        } else {
            next_set.template SetMutation<TMutationToSet>(_value);
        }
    }

    TMutation::Type mutation_value;
    TNextSet        next_set;
    //build a static linked list
};

using TShaderMutationSetEmpty = TShaderMutationSet<>;

#define MUTATION_CLASS_BOOL(ClassName)                                 \
    struct ClassName : public TShaderMutationSet<ShaderMutationBool> { \
    public:                                                            \
        static constexpr char* mutation_name = #ClassName;             \
    }

#pragma endregion
#endif