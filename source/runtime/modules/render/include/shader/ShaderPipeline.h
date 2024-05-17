#ifndef MOER_SHADER_PIPELINE_H
#define MOER_SHADER_PIPELINE_H
#include "rhi/RHIResource.h"
#include <string_view>
struct TDummy {};
#define BEGIN_SHADER_PARAMS \
    using InnerTArg = ShaderArgs < TDummy,

#define END_SHADER_PARAMS \
    > ;

#define DEFINE_SHADER_TEXTURE(name) \
    , ShaderArg<RHITextureRef, #name>

#define DEFINE_SHADER_BUFFER(name) \
    , ShaderArg<RHIBufferRef, #name>

#define ShaderArgType(type, in_name)                       \
    struct in_name {                                       \
        using Type                             = type;     \
        static constexpr std::string_view name = #in_name; \
        static const std::string_view     GetName() {      \
            return name;                               \
        }                                                  \
        size_t hash_code;                                  \
        in_name() { hash_code = GetHash(name); }           \
    };

namespace Moer {
    template<const char... Name>
    struct StringType {
        static constexpr char name[] = {Name..., '\0'};
    };
    template<typename T, StringType arg_name>
    struct ShaderArg {
        static constexpr char* name = arg_name;
        size_t                 hash;
        ShaderArg() {
            hash = std::hash<char*>{}(name);
        }
        static const std::string_view GetName() {
            return name;
        }
    };
    template<typename... Args>
    struct ShaderArgs {
        static constexpr size_t size = sizeof...(Args);
        using tuple_helper           = std::tuple<Args...>;
        template<typename T, typename TArg>
        void SetParam(T& t){};

        template<typename... dst_args, std::size_t... Is>
        void SetParams(std::tuple<dst_args...>& tuple, std::index_sequence<Is...>) {
            //set arg for specific index in tuple_helper
            (..., std::invoke([=, this](auto& item) {
                SetParam<decltype(item), decltype(std::get<Is>(tuple_helper))>(item);
            }(), std::get<Is>(tuple)));
            
            
        }
    };

    class ShaderPipeline {

        ShaderArgType(RHITextureRef, Texture0)
            ShaderArgType(RHIBufferRef, Buffer0)
            // BEGIN_SHADER_PARAMS
            // DEFINE_SHADER_TEXTURE(Texture0)
            // DEFINE_SHADER_BUFFER(Buffer0)
            // END_SHADER_PARAMS
            using InnerShaderArgs = ShaderArgs<Texture0, Buffer0>;

        template<typename T>
        void SetParam(T&& _param) {
        }

        template<typename... T>
        void SetArgs(T&&... _args) {
            //need to make sure the args are in the right order with InnerShaderArgs
        }
    };
};// namespace Moer
#endif