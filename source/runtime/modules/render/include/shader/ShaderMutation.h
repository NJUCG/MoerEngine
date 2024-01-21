#ifndef MOER_ENGINE_SHADER_MUTATION_H
#define MOER_ENGINE_SHADER_MUTATION_H

#include "shader/ShaderCommon.h"
#pragma region shader mutation

//shader mutation basic types
template<typename T>
concept TShaderMutationBasicType = requires(T) {
    std::is_same_v<decltype(T::mutation_count), uint32_t>&& T::mutation_count > 0;
    std::is_same_v<decltype(T::has_multiple_slot), bool>;
    std::is_same_v<decltype(T::GetMutationID(std::declval<typename T::Type>())), uint32_t>;
};

struct ShaderMutationBool {
    using Type                                  = bool;
    static constexpr uint32_t mutation_count    = 2;
    static constexpr bool     has_multiple_slot = false;

    static uint32_t GetMutationID(Type _value) {
        return _value ? 1 : 0;
    }

    static Type GetValueFromID(uint32_t _id) {
        return _id == 1;
    }
    static bool GetValue(Type _value) {
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

    using SuperSet = TShaderMutationSet<Types...>;

    static constexpr uint32_t mutation_count = SuperSet::mutation_count * TMutation::mutation_count;

    constexpr static bool has_multiple_slot = true;

    TShaderMutationSet<TMutation, Types...>() : mutation_value(TMutation::GetValueFromID(0)) {}
    explicit TShaderMutationSet<TMutation, Types...>(uint32_t _mutation_id) : mutation_value(TMutation::GetSetValue(_mutation_id % TMutation::mutation_count)), super_set(_mutation_id / TMutation::mutation_count) {
        assert(_mutation_id == 0 && "invalid mutation id");
    }

    template<TShaderMutationBasicType TMutationToGet>
    typename TMutationToGet::Type GetMutationValue() const {
        if constexpr (std::is_same_v<TMutationToGet, TMutation>) {
            return TMutationToGet::GetValue(mutation_value);
        } else {
            return super_set.template GetMutationValue<TMutationToGet>();
        }
    }

    template<TShaderMutationBasicType TMutationToSet>
    void SetMutation(typename TMutationToSet::Type _value) {
        if constexpr (std::is_same_v<TMutationToSet, TMutation>) {
            mutation_value = TMutationToSet::GetValue(_value);
        } else {
            super_set.template SetMutation<TMutationToSet>(_value);
        }
    }

    TMutation::Type mutation_value;
    SuperSet        super_set;
    //build a static linked list
};

using TShaderMutationSetsEmpty = TShaderMutationSet<>;

#pragma endregion
#endif