#ifndef MOER_SHADER_PIPELINE_H
#define MOER_SHADER_PIPELINE_H
#include "math/Matrix.h"
#include "misc/STL.h"
// #include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <variant>
struct TDummy {};
namespace Moer::Render {
    using TConstantArg = std::variant<int, float, bool, uint>;
    using TScalarArg   = std::variant<float2, float3, float4, uint2, uint3, uint4>;
    using TMatrixArg   = std::variant<Matrix4x4f, Matrix3x3f, Matrix3x4f, Matrix3x4ui, Matrix3x4i>;
    template<typename T>
        requires std::is_trivial_v<T>
    using TConstantStruct = T;
    template<typename T>
    struct TConstsant{
        using type = T;
        T value;
    };
};// namespace Moer::Render

#define BEGIN_SHADER_PARAMS \
    using InnerTArg = ShaderArgs < TDummy,

#define END_SHADER_PARAMS \
    > ;

#define DEFINE_SHADER_TEX(name) \
    StringType(name) using name = ShaderArg<TextureArg, GetStringType(name)>

#define DEFINE_SHADER_BUFFER(name) \
    StringType(name) using name = ShaderArg<BufferArg, GetStringType(name)>

#define DEFINE_SHADER_CONSTANT_STRUCT(type, name) \
    StringType(name) using name = ShaderArg<TConstsant<type>, GetStringType(name)>

#define DEFINE_SHADER_SAMPLER(name) \
    StringType(name) using name = ShaderArg<SamplerArg, GetStringType(name)>

#define DEFINE_RASTER_PIPELINE_CLASS(name) \
using TPipeline = name;\
name(PipelineHandle _handle) : RasterPipeline(_handle) {}\
name() : RasterPipeline() {}

#define DEFINE_SHADER_ARGS(...)                                                                                \
public:                                                                                                        \
    using InnerArgs = ShaderArgs<TPipeline, __VA_ARGS__>;                                                      \
    template<typename... T>                                                                                    \
    ArrayArguments SetArgs(T&&... _args) {                                                                     \
        InnerArgs args(*this);                                                                                 \
        return std::move(args.SetParams(std::make_index_sequence<sizeof...(T)>(), std::forward<T>(_args)...)); \
    }                                                                                                          \
                                                                                                               \
private:                                                                                                       \
    template<size_t... idx>                                                                                    \
    static StaticArray<std::string_view, InnerArgs::arg_size> GetHashArray(std::index_sequence<idx...>) {      \
        return {std::tuple_element_t<idx, InnerArgs::tuple_helper>::GetName()...};                             \
    }                                                                                                          \
                                                                                                               \
    template<size_t... idx>                                                                                    \
    static StaticArray<uint64, InnerArgs::arg_size> GetHashCodeArray(std::index_sequence<idx...>) {            \
        return {std::tuple_element_t<idx, InnerArgs::tuple_helper>::GetHashCode()...};                         \
    }                                                                                                          \
                                                                                                               \
public:                                                                                                        \
    static StaticArray<std::string_view, InnerArgs::arg_size> GetHashArray() {                                 \
        return GetHashArray(std::make_index_sequence<InnerArgs::arg_size>());                                  \
    }                                                                                                          \
    static StaticArray<uint64, InnerArgs::arg_size> GetHashCodeArray() {                                       \
        return GetHashCodeArray(std::make_index_sequence<InnerArgs::arg_size>());                              \
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

namespace Moer::Render {
    using TArg = std::variant<BufferView, TextureView, Sampler>;
    struct Arguments {
        Arguments() = default;
        Arguments(UnorderedMap<uint, TArg> _args) : args(std::move(_args)) {
        }

        // Array<TArg> args;
        TArg& operator[](uint _idx) {
            return args[_idx];
        }

        const TArg& operator[](uint _idx) const {
            return args.at(_idx);
        }
        uint Size() const {
            return args.size();
        }
        UnorderedMap<uint, TArg> args;
    };

    struct ArrayArguments {
        ArrayArguments(uint _arg_size) : args(_arg_size) {
        }
        // Array<TArg> args;
        TArg& operator[](uint _idx) {
            return args[_idx];
        }
        const TArg& operator[](uint _idx) const {
            return args[_idx];
        }
        uint Size() const {
            return args.size();
        }
        Array<TArg> args;
        Array<uint> constants;

        Array<uint>&& StealConstants() {
            return std::move(constants);
        }
    };
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
    struct SamplerArg {};
    struct ConstantArg {};
    class ShaderPipeline;
    template<typename TPipeline, typename... Args>
    struct ShaderArgs {

        ShaderArgs(TPipeline& _pipeline) : pipeline(_pipeline) {
        }
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
            requires std::is_same_v<std::remove_reference_t<T>, TextureView> || std::is_same_v<std::remove_reference_t<T>, BufferView> || std::is_same_v<std::remove_reference_t<T>, Sampler> ||
            std::is_same_v<std::remove_reference_t<T>, TextureRef> || std::is_same_v<std::remove_reference_t<T>, BufferRef> || std::is_same_v<typename TArg::type, TConstsant<std::remove_reference_t<T>>>
        void SetParam(T&& _t, ArrayArguments& _arg_setter) {
            using cpp_type       = typename TArg::type;
            constexpr auto index = Index<TArg, tuple_helper>::value;
            using Type           = std::remove_reference_t<T>;
            if constexpr (std::is_same_v<cpp_type, TextureArg>) {
                //do texture stuff
                if constexpr (std::is_same_v<Type, TextureRef>) {
                    _arg_setter[index] = _t->GetView();
                    //do texture stuff
                } else if constexpr (std::is_same_v<Type, TextureView>) {
                    _arg_setter[index] = std::forward<T>(_t);
                } else {
                    if constexpr (true)
                        assert(0 && "not a buffer type");
                }
            } else if constexpr (std::is_same_v<cpp_type, BufferArg>) {
                //do buffer stuff
                if constexpr (std::is_same_v<Type, BufferRef>) {
                    _arg_setter[index] = _t->GetView();
                } else if constexpr (std::is_same_v<Type, BufferView>) {
                    _arg_setter[index] = std::forward<T>(_t);
                } else {
                    if constexpr (true)
                        assert(0 && "not a buffer type");
                }
            } else if constexpr (std::is_same_v<cpp_type, SamplerArg>) {
                _arg_setter[index] = std::forward<T>(_t);
            } else {
                //constant
                _arg_setter.constants.resize(sizeof(T) / sizeof(uint));
                std::memcpy(_arg_setter.constants.data(), &_t, sizeof(T));
            }
        }

        template<typename... T, std::size_t... Is>
        ArrayArguments SetParams(std::index_sequence<Is...>, T&&... _args) {
            ArrayArguments arg_setter(arg_size);
            (..., SetParam<T, std::tuple_element_t<Is, tuple_helper>>((std::forward<T>(_args)), arg_setter));
            return std::move(arg_setter);
        }

        TPipeline& pipeline;
    };

    class ShaderPipeline {
    protected:
        struct InnerArgs {
            using tuple_helper             = std::tuple<>;
            static constexpr uint arg_size = 0;
            void                  SetParams() {}
        };

    private:
    public:
        void SetTexture(uint _idx, TextureRef _param);
        void SetBuffer(uint _idx, BufferRef _param);
        void SetTexture(uint _idx, TextureView _param);
        void SetBuffer(uint _idx, BufferView _param);
        template<typename T>
        void SetConstant(uint _idx, T&& _args) {
            SetConstantInner(_idx, std::span<uint>((uint*)&_args, sizeof(T) / sizeof(uint)));
        }
        void SetConstantInner(uint _idx, std::span<uint> _data);
        void SetBufferHash(uint64 _hash, BufferView _param);
        void SetTextureHash(uint64 _hash, TextureView _param);
        uint GetBindingIdx(uint64 _hash) {
            return handle.hash_2_info_index[_hash];
        }
        uint GetBindingSize() {
            return handle.binding_infos.size();
        }
        ShaderPipeline(PipelineHandle _handle) {
        }
        PipelineHandle handle;

        ShaderPipeline() = default;
        ShaderPipeline(ShaderPipeline&& _other) {
            handle = std::move(_other.handle);
        }
        ShaderPipeline& operator=(ShaderPipeline&& _other) {
            handle = std::move(_other.handle);
            return *this;
        }
    };
#define COPY_CONSTRUCTOR(name)                                 \
    name(name&& _other) : ShaderPipeline(std::move(_other)) {} \
    name() : ShaderPipeline() {}                               \
    name& operator=(name&& _other) {                           \
        handle = std::move(_other.handle);                     \
        return *this;                                          \
    }
    class RasterPipeline : public ShaderPipeline, public RHIResource {
    public:
        RasterPipeline(PipelineHandle _handle) : ShaderPipeline(_handle), RHIResource(RRT_GRAPHIC_PIPELINE_STATE) {
        }
        COPY_CONSTRUCTOR(RasterPipeline);
    };

    class ComputePipeline : public ShaderPipeline, public RHIResource {
    public:
        ComputePipeline(PipelineHandle _handle) : ShaderPipeline(_handle), RHIResource(RRT_COMPUTE_PIPELINE_STATE) {
        }
        COPY_CONSTRUCTOR(ComputePipeline);
    };

    class RTPipeline : public ShaderPipeline, public RHIResource {

    public:
        RTPipeline(PipelineHandle _handle) : ShaderPipeline(_handle), RHIResource(RRT_RAY_TRACING_PIPELINE_STATE) {
        }
        COPY_CONSTRUCTOR(RTPipeline);
    };

    class GBufferLayout : public RasterPipeline {
    public:
        struct Constant {};
        DEFINE_RASTER_PIPELINE_CLASS(GBufferLayout)

        DEFINE_SHADER_BUFFER(PositionBuffer);
        DEFINE_SHADER_BUFFER(NormalBuffer);
        DEFINE_SHADER_BUFFER(DiffuseBuffer);
        DEFINE_SHADER_BUFFER(SpecularBuffer);
        DEFINE_SHADER_TEX(DiffuseTexture);
        DEFINE_SHADER_TEX(SpecularTexture);
        DEFINE_SHADER_CONSTANT_STRUCT(Constant, constant);

        DEFINE_SHADER_ARGS(PositionBuffer, NormalBuffer, DiffuseBuffer, SpecularBuffer, DiffuseTexture, SpecularTexture, constant);

        void Test() {
            SetArgs(BufferRef(), BufferRef(), BufferRef(), BufferRef(), TextureRef(), TextureRef(), Constant{});
        }
    };
};// namespace Moer::Render

#endif