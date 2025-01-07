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
    struct TConstsant {
        using type = T;
        T value;
    };

};// namespace Moer::Render

#define BEGIN_SHADER_PARAMS \
    using InnerTArg = ShaderArgs < TDummy,

#define END_SHADER_PARAMS \
    > ;

#define DEFINE_SHADER_TEX(name) \
    StringType(name) using name = ShaderArg<TextureArg<1>, GetStringType(name)>

#define DEFINE_SHADER_TEX_ARRAY(name, max_num) \
    StringType(name) using name = ShaderArg<TextureArg<max_num>, GetStringType(name)>

#define DEFINE_SHADER_BUFFER(name) \
    StringType(name) using name = ShaderArg<BufferArg<1>, GetStringType(name)>

#define DEFINE_SHADER_BUFFER_ARRAY(name, max_num) \
    StringType(name) using name = ShaderArg<BufferArg<max_num>, GetStringType(name)>

#define DEFINE_SHADER_CONSTANT_STRUCT(type, name) \
    StringType(name) using name = ShaderArg<TConstsant<type>, GetStringType(name)>

#define DEFINE_SHADER_SAMPLER(name) \
    StringType(name) using name = ShaderArg<SamplerArg, GetStringType(name)>

#define DEFINE_SHADER_BINDLESS_ARRAY(name) \
    StringType(name) using name = ShaderArg<BindlessArg, GetStringType(name)>

#define DEFINE_SHADER_TLAS(name) \
    StringType(name) using name = ShaderArg<TLASArg, GetStringType(name)>

#define DEFINE_RASTER_PIPELINE_CLASS(name)                    \
    using TPipeline = name;                                   \
    name(PipelineHandle _handle) : RasterPipeline(_handle) {} \
    name() : RasterPipeline() {}                              \
    MOVE_CONSTRUCTOR(name)                                    \
    NO_COPY_CONSTRUCTOR(name)

#define DEFINE_COMPUTE_PIPELINE_CLASS(name)                    \
    using TPipeline = name;                                    \
    name(PipelineHandle _handle) : ComputePipeline(_handle) {} \
    name() : ComputePipeline() {}                              \
    MOVE_CONSTRUCTOR(name)                                     \
    NO_COPY_CONSTRUCTOR(name)

#define DEFINE_SHADER_ARGS(...)                                                                                            \
public:                                                                                                                    \
    using InnerArgs = ShaderArgs<TPipeline __VA_OPT__(, ) __VA_ARGS__>;                                                    \
    template<typename... T>                                                                                                \
    ArrayArguments SetArgs(T&&... _args) {                                                                                 \
        return std::move(InnerArgs::SetParams(std::make_index_sequence<sizeof...(T)>(), std::forward<T>(_args)...));       \
    }                                                                                                                      \
                                                                                                                           \
private:                                                                                                                   \
    template<size_t... idx>                                                                                                \
    static StaticArray<std::string_view, InnerArgs::arg_size> GetHashArray(std::index_sequence<idx...>) {                  \
        return {std::tuple_element_t<idx, InnerArgs::tuple_helper>::GetName()...};                                         \
    }                                                                                                                      \
                                                                                                                           \
    template<size_t... idx>                                                                                                \
    static StaticArray<uint64, InnerArgs::arg_size> GetHashCodeArray(std::index_sequence<idx...>) {                        \
        return {std::tuple_element_t<idx, InnerArgs::tuple_helper>::GetHashCode()...};                                     \
    }                                                                                                                      \
                                                                                                                           \
    template<size_t... idx>                                                                                                \
    static StaticArray<Moer::Render::ShaderArgCppInfo, InnerArgs::arg_size> GetArgInfoArray(std::index_sequence<idx...>) { \
        return {std::tuple_element_t<idx, InnerArgs::tuple_helper>::GetArgInfo()...};                                      \
    }                                                                                                                      \
                                                                                                                           \
public:                                                                                                                    \
    static StaticArray<std::string_view, InnerArgs::arg_size> GetHashArray() {                                             \
        if constexpr (InnerArgs::arg_size == 0) { return {}; }                                                             \
        return GetHashArray(std::make_index_sequence<InnerArgs::arg_size>());                                              \
    }                                                                                                                      \
    static StaticArray<uint64, InnerArgs::arg_size> GetHashCodeArray() {                                                   \
        if constexpr (InnerArgs::arg_size == 0) { return {}; }                                                             \
        return GetHashCodeArray(std::make_index_sequence<InnerArgs::arg_size>());                                          \
    }                                                                                                                      \
    static StaticArray<Moer::Render::ShaderArgCppInfo, InnerArgs::arg_size> GetArgInfoArray() {                            \
        if constexpr (InnerArgs::arg_size == 0) { return {}; }                                                             \
        return GetArgInfoArray(std::make_index_sequence<InnerArgs::arg_size>());                                           \
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
    using TInvalidArg = uint;
    using TArg        = std::variant<TInvalidArg, BufferView, TextureView, std::span<TextureView>, std::span<BufferView>, Sampler, BindlessArrayRef, RaytracingTlasRef>;

    template<typename T>
    struct ShaderArgEnum {
        static constexpr EShaderArgType arg_type   = SDA_Constant;
        static constexpr uint           array_size = 1;
    };

    // struct Arguments {
    //     Arguments() = default;
    //     Arguments(UnorderedMap<uint, TArg> _args) : args(std::move(_args)) {
    //     }

    //     // Array<TArg> args;
    //     TArg& operator[](uint _idx) {
    //         return args[_idx];
    //     }

    //     const TArg& operator[](uint _idx) const {
    //         return args.at(_idx);
    //     }
    //     uint Size() const {
    //         return args.size();
    //     }
    //     UnorderedMap<uint, TArg> args;
    // };

    struct ArrayArguments {
        ArrayArguments() = default;
        ArrayArguments(uint _arg_cnt, uint _const_size, bool _b_use_bdls) : args(_arg_cnt), constants(_const_size), b_use_bindless(_b_use_bdls) {}
        ArrayArguments(ArrayArguments&& _other) {
            args      = std::move(_other.args);
            constants = std::move(_other.constants);
        }

        // Array<TArg> args;
        TArg&       operator[](uint _idx) { return args[_idx]; }
        const TArg& operator[](uint _idx) const { return args[_idx]; }
        uint        Size() const { return args.size(); }
        Array<TArg> args;
        Array<uint> constants;

        Array<uint>&& StealConstants() { return std::move(constants); }
        bool          b_use_bindless = false;
    };

    template<typename T, typename arg_name>
    struct ShaderArg {
        static constexpr std::string_view name         = arg_name::name;
        using type                                     = T;
        static constexpr EShaderArgType arg_type       = ShaderArgEnum<T>::arg_type;
        static constexpr uint           max_array_size = ShaderArgEnum<T>::array_size;
        ShaderArg() {}
        static const std::string_view GetName() { return name; }
        static uint64                 GetHashCode() { return GetHash(name); }

        static ShaderArgCppInfo GetArgInfo() {
            return {max_array_size, arg_type};
        }
    };
    struct NonConstant {};

    template<uint array_size>
    struct BufferArg {
        using type = NonConstant;
    };

    struct ConstantBufferArg {
        using type = NonConstant;
    };

    template<uint array_size>
    struct TextureArg {
        using type = NonConstant;
    };

    struct SamplerArg {
        using type = NonConstant;
    };

    struct ConstantArg {
        using type = NonConstant;
    };

    struct BindlessArg {
        using type = NonConstant;
    };

    struct TLASArg {
        using type = NonConstant;
    };

    template<uint _array_size>
    struct ShaderArgEnum<BufferArg<_array_size>> {
        static constexpr EShaderArgType arg_type   = SDA_Buffer;
        static constexpr uint           array_size = _array_size;
    };

    template<>
    struct ShaderArgEnum<ConstantBufferArg> {
        static constexpr EShaderArgType arg_type   = SDA_ConstantBuffer;
        static constexpr uint           array_size = 1;
    };

    template<uint _array_size>
    struct ShaderArgEnum<TextureArg<_array_size>> {
        static constexpr EShaderArgType arg_type   = SDA_Texture;
        static constexpr uint           array_size = _array_size;
    };

    template<>
    struct ShaderArgEnum<SamplerArg> {
        static constexpr EShaderArgType arg_type   = SDA_Sampler;
        static constexpr uint           array_size = 1;
    };

    template<>
    struct ShaderArgEnum<BindlessArg> {
        static constexpr EShaderArgType arg_type   = SDA_BindlessArray;
        static constexpr uint           array_size = 1;
    };

    template<>
    struct ShaderArgEnum<TLASArg> {
        static constexpr EShaderArgType arg_type   = SDA_TLAS;
        static constexpr uint           array_size = 1;
    };

    class ShaderPipeline;

    template<typename TPipeline, typename... Args>
    struct ShaderArgs {

        using PipelineType               = TPipeline;
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
        template<typename T>
        struct is_constant_type {
            static constexpr bool value = !std::is_same_v<typename T::type::type, NonConstant>;
        };

        static constexpr uint32 GetConstantSize() {
            //if TArg is a constant, add the size of TArg to the total size, other wise add 0
            return (0 + ... + (is_constant_type<Args>::value ? sizeof(typename Args::type) : 0)) / sizeof(uint);
        }

        static constexpr bool IsUsingBdls() {
            //if TArg is a constant, add the size of TArg to the total size, other wise add 0
            return (... || (Args::arg_type == SDA_BindlessArray));
        }

        // constexpr uint32 static GetConstantSize() {
        //     //if TArg is a constant, add the size of TArg to the total size, other wise add 0
        //     return (0 + ... + (std::is_same_v<typename Args::type, TConstsant<typename Args::type::type>> ? sizeof(typename Args::type) : 0)) / sizeof(uint);
        // }
        using t_texture_array_arg = std::span<TextureView>;
        using t_buffer_array_arg  = std::span<BufferView>;

        template<typename T, typename TArg>
            requires std::is_same_v<std::remove_reference_t<T>, TextureView> || std::is_same_v<std::remove_reference_t<T>, t_texture_array_arg> ||
                     std::is_same_v<std::remove_reference_t<T>, BufferView> || std::is_same_v<std::remove_reference_t<T>, t_buffer_array_arg> || std::is_same_v<std::remove_reference_t<T>, Sampler> ||
                     std::is_same_v<std::remove_reference_t<T>, TextureRef> || std::is_same_v<std::remove_reference_t<T>, BufferRef> || std::is_same_v<typename TArg::type, TConstsant<std::remove_reference_t<T>>> ||
                     std::is_same_v<std::remove_reference_t<T>, BindlessArrayRef> || std::is_same_v<std::remove_reference_t<T>, RaytracingTlasRef>
        static void SetParam(T&& _t, ArrayArguments& _arg_setter) {
            using cpp_type       = typename TArg::type;
            constexpr auto index = Index<TArg, tuple_helper>::value;
            using Type           = std::remove_reference_t<T>;
            if constexpr (is_template_of_v<cpp_type, Moer::Render::TextureArg>) {
                //do texture stuff
                if constexpr (std::is_same_v<Type, TextureRef>) {
                    _arg_setter[index] = _t->GetView();
                    //do texture stuff
                } else if constexpr (std::is_same_v<Type, TextureView>) {
                    _arg_setter[index] = std::forward<T>(_t);
                } else if constexpr (std::is_same_v<Type, t_texture_array_arg>) {
                    _arg_setter[index] = std::forward<T>(_t);
                } else {
                    if constexpr (true)
                        assert(0 && "not a buffer type");
                }
            } else if constexpr (is_template_of_v<cpp_type, BufferArg>) {
                //do buffer stuff
                if constexpr (std::is_same_v<Type, BufferRef>) {
                    _arg_setter[index] = _t->GetView();
                } else if constexpr (std::is_same_v<Type, BufferView>) {
                    _arg_setter[index] = std::forward<T>(_t);
                } else if constexpr (std::is_same_v<Type, t_buffer_array_arg>) {
                    _arg_setter[index] = std::forward<T>(_t);
                } else {
                    if constexpr (true)
                        assert(0 && "not a buffer type");
                }
            } else if constexpr (std::is_same_v<cpp_type, SamplerArg>) {
                _arg_setter[index] = std::forward<T>(_t);
            } else if constexpr (std::is_same_v<cpp_type, BindlessArg>) {
                _arg_setter[index] = std::forward<T>(_t);

            } else if constexpr (std::is_same_v<cpp_type, TLASArg>) {
                _arg_setter[index] = std::forward<T>(_t);
            } else {
                //constant
                assert(_arg_setter.constants.size() == sizeof(T) / sizeof(uint) && "constant size mismatch");
                std::memcpy(_arg_setter.constants.data(), &_t, sizeof(T));
            }
        }

        // template<typename... T, std::size_t... Is>
        // ArrayArguments SetParams(std::index_sequence<Is...>, T&&... _args) {
        //     ArrayArguments arg_setter(arg_size);
        //     (..., SetParam<T, std::tuple_element_t<Is, tuple_helper>>((std::forward<T>(_args)), arg_setter));
        //     return std::move(arg_setter);
        // }

        template<typename... T, std::size_t... Is>
        static ArrayArguments SetParams(std::index_sequence<Is...>, T&&... _args) {
            ArrayArguments arg_setter(arg_size, GetConstantSize(), IsUsingBdls());
            (..., SetParam<T, std::tuple_element_t<Is, tuple_helper>>((std::forward<T>(_args)), arg_setter));
            return std::move(arg_setter);
        }

        // TPipeline& pipeline;
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
        void SetSampler(uint _idx, Sampler _param);

        template<typename T>
        void SetConstant(uint _idx, T&& _args) { SetConstantInner(_idx, std::span<uint>((uint*)&_args, sizeof(T) / sizeof(uint))); }

        void SetConstantInner(uint _idx, std::span<uint> _data);
        void SetBufferHash(uint64 _hash, BufferView _param);
        void SetTextureHash(uint64 _hash, TextureView _param);
        uint GetBindingIdx(uint64 _hash) { return handle.hash_2_info_index[_hash]; }
        ShaderPipeline(PipelineHandle _handle) : handle(std::move(_handle)) {}
        PipelineHandle handle;

        ShaderPipeline() : handle{} {}
        ShaderPipeline(ShaderPipeline&& _other) { handle = std::move(_other.handle); }

        ShaderPipeline& operator=(ShaderPipeline&& _other) {
            if (*this == _other) return *this;
            handle = std::move(_other.handle);
            return *this;
        }

        ShaderPipeline(const ShaderPipeline& _other)            = delete;
        ShaderPipeline& operator=(const ShaderPipeline& _other) = delete;

        virtual ~ShaderPipeline() {
            if (handle.IsValid()) MoerDelete((PipelineState*)handle.handle);
        }

        bool operator==(const ShaderPipeline& _other) const { return handle.handle == _other.handle.handle; }
        bool IsValid() const { return !handle.IsValid(); }
    };

#define MOVE_CONSTRUCTOR(name)                           \
    name& operator=(name&& _other) {                     \
        if (*this == _other) return *this;               \
        handle               = std::move(_other.handle); \
        _other.handle.handle = 0;                        \
        return *this;                                    \
    }                                                    \
    name(name&& _other) {                                \
        handle               = std::move(_other.handle); \
        _other.handle.handle = 0;                        \
    }

#define NO_COPY_CONSTRUCTOR(name)                 \
    name(const name& _other)            = delete; \
    name& operator=(const name& _other) = delete;

#define COPY_CONSTRUCTOR(name)                                 \
    name(name&& _other) : ShaderPipeline(std::move(_other)) {} \
    name() : ShaderPipeline() {}

    class RasterPipeline : public ShaderPipeline, public RHIResource {
    public:
        RasterPipeline(PipelineHandle _handle) : ShaderPipeline(_handle), RHIResource(RRT_GRAPHIC_PIPELINE_STATE) {}
        COPY_CONSTRUCTOR(RasterPipeline);
    };

    class ComputePipeline : public ShaderPipeline, public RHIResource {
    public:
        ComputePipeline(PipelineHandle _handle) : ShaderPipeline(_handle), RHIResource(RRT_COMPUTE_PIPELINE_STATE) {}
        COPY_CONSTRUCTOR(ComputePipeline);
    };

    class RTPipeline : public ShaderPipeline, public RHIResource {

    public:
        RTPipeline(PipelineHandle _handle) : ShaderPipeline(_handle), RHIResource(RRT_RAY_TRACING_PIPELINE_STATE) {}
        COPY_CONSTRUCTOR(RTPipeline);
    };

    struct ParamBlockAllcateInfo {
        //ideally we can allocate param/descriptor block from global descriptor pool(vk) or descriptor heap(dx)
        //when set params, we can allocate a descriptor set or descriptor buffer offset from the pool and bind it to the pipeline
        //and the allocation info is stored in this struct, like descriptor buffer offset, descriptor sets key
        //and when parameter changes, when using descriptor buffer, we can copy the data to the descriptor buffer,
        //we can maintain a cpu end descriptor buffer and a gpu version descriptor buffer to avoid descriptor buffer update
        //but question is, how do we know whether to delete the descriptor allocated from the pool
        //life is short, use bindless maybe a better choice
    };
    template<typename TPipeline>
        requires std::is_base_of_v<ShaderPipeline, TPipeline>
    struct ParamterBlock {
        using PipelineType = TPipeline;
        template<typename... TArgs>
        void SetParams(TArgs&&... _args) {
            args       = TPipeline::SetArgs(std::forward<TArgs>(_args)...);
            b_args_set = true;
        }
        ParamBlockAllcateInfo* alloc_info = nullptr;
        ArrayArguments         args;
        bool                   b_args_set = false;
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

        void Test() { SetArgs(BufferRef(), BufferRef(), BufferRef(), BufferRef(), TextureRef(), TextureRef(), Constant{}); }
    };
};// namespace Moer::Render

#endif