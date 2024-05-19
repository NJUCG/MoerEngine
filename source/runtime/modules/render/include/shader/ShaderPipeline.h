#ifndef MOER_SHADER_PIPELINE_H
#define MOER_SHADER_PIPELINE_H
#include "misc/STL.h"
#include "rhi/RHIResource.h"
#include <string_view>
#include <tuple>
#include <typeindex>
#include <utility>
#include <variant>
struct TDummy {};
#define BEGIN_SHADER_PARAMS \
    using InnerTArg = ShaderArgs < TDummy,

#define END_SHADER_PARAMS \
    > ;

#define DEFINE_SHADER_TEX(name) \
    StringType(name) using name = ShaderArg<RHITextureRef, GetStringType(name)>

#define DEFINE_SHADER_BUFFER(name) \
    StringType(name) using name = ShaderArg<RHIBufferRef, GetStringType(name)>

#define DEFINE_SHADER_ARGS(...)                                                              \
    using InnerArgs = ShaderArgs<__VA_ARGS__>;                                               \
    template<typename... T>                                                                  \
    void SetArgs(T&&... _args) {                                                             \
        InnerArgs args;                                                                      \
        args.SetParams(std::make_index_sequence<sizeof...(T)>(), std::forward<T>(_args)...); \
    }

#define StringType(in_name)                                \
    struct String##in_name {                               \
        static constexpr std::string_view name = #in_name; \
        static constexpr std::string_view GetName() {      \
            return name;                                   \
        }                                                  \
    };

#define GetStringType(name) \
    String##name

namespace Moer {

    template<typename T, typename arg_name>
    struct ShaderArg {
        static constexpr std::string_view name = arg_name::name;
        using type                             = T;
        ShaderArg() {
        }
        static const std::string_view GetName() {
            return name;
        }
        static uint64 GetHashCode() {
            return GetHash(name);
        }
    };

    struct BufferArg {};
    struct TextureArg {};

    template<typename... Args>
    struct ShaderArgs {
        static constexpr size_t arg_size = sizeof...(Args);
        using tuple_helper               = std::tuple<Args...>;

        template<typename T, typename Tuple>
        struct Index;

        template<typename T, typename... Types>
        struct Index<T, std::tuple<T, Types...>> {
            static const std::size_t value = 0;
        };

        template<typename T, typename U, typename... Types>
        struct Index<T, std::tuple<U, Types...>> {
            static const std::size_t value = 1 + Index<T, std::tuple<Types...>>::value;
        };

        template<typename T, typename TArg>
        void SetParam(T&& _t) {
            using cpp_type       = typename TArg::type;
            constexpr auto index = Index<TArg, tuple_helper>::value;
            if constexpr (std::is_same_v<cpp_type, TextureArg>) {
                //do texture stuff
                if constexpr (std::is_same_v<T, RHITextureRef>) {
                    //do texture stuff
                } else {
                    static_assert(false, "not a texture type");
                }
            } else if constexpr (std::is_same_v<cpp_type, BufferArg>) {
                //do buffer stuff
            }
        }

        template<typename... T, std::size_t... Is>
        void SetParams(std::index_sequence<Is...>, T&&... _args) {
            (..., SetParam<T, std::tuple_element_t<Is, tuple_helper>>((std::forward<T>(_args))));
        }
    };

    class ShaderPipeline {
    protected:
        template<typename T>
        void SetParam(T&& _param) {
        }
        struct InnerArgs {
            using tuple_helper             = std::tuple<>;
            static constexpr uint arg_size = 0;
            void                  SetParams() {}
        };

        template<size_t... idx>
        void InitHashArray(std::index_sequence<idx...>) {
            hash_array = {std::tuple_element_t<idx, InnerArgs::tuple_helper>::GetHashCode()...};
        }

    public:
        ShaderPipeline() {
            InitHashArray(std::make_index_sequence<InnerArgs::arg_size>());
        }
        StaticArray<uint64_t, InnerArgs::arg_size> hash_array;
    };

    class GBufferRenderPipeline : public ShaderPipeline {

        DEFINE_SHADER_BUFFER(PositionBuffer);
        DEFINE_SHADER_BUFFER(NormalBuffer);
        DEFINE_SHADER_BUFFER(DiffuseBuffer);
        DEFINE_SHADER_BUFFER(SpecularBuffer);
        DEFINE_SHADER_TEX(DiffuseTexture);
        DEFINE_SHADER_TEX(SpecularTexture);

        DEFINE_SHADER_ARGS(PositionBuffer, NormalBuffer, DiffuseBuffer, SpecularBuffer, DiffuseTexture, SpecularTexture);

        void Test() {
            SetArgs(RHIBufferRef(), RHIBufferRef(), RHIBufferRef(), RHIBufferRef(), RHITextureRef(), RHITextureRef());
        }
    };
};// namespace Moer
#endif