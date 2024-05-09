#ifndef MOER_COPY_DISPATCH_ARGS_H
#define MOER_COPY_DISPATCH_ARGS_H

#include "shader/Shader.h"
#include "misc/Traits.h"
#include "../deferred/RenderResourceDeferred.h"

class CopyDispatchArgsShader : public Shader {
    DEFINE_SHADER_TYPE(CopyDispatchArgsShader, Global, RENDER_API)
public:
    BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(Args)
    DEFINE_SHADER_PARAM(uint32_t, src_offset)
    DEFINE_SHADER_PARAM(uint32_t, dst_offset)
    DEFINE_SHADER_PARAM(uint32_t, group_size)
    END_SHADER_CONSTANT_STRUCT_DEFINITION(Args)
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    DEFINE_SHADER_PARAM_STRUCT(Args, args)
    DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<uint>, target)
    DEFINE_SHADER_PARAM_SRV(StructuredBuffer<uint>, src_buffer)
    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

namespace Moer {
    constexpr bool IsPowerOfTwo(uint _value) {
        return (_value & (_value - 1)) == 0;
    }
    template<uint _value>
    struct IsGroupSize {
        static constexpr bool value = IsPowerOfTwo(_value) && _value >= 64 && _value <= 1024;
    };
    struct CopyDispatchArgs {
        struct Impl;
        template<uint GroupSize>
            requires IsGroupSize<GroupSize>::value
        static void Dispatch(RenderContext& _context, std::string_view _src_name, RHISRVRef _src_buffer, std::string_view _target_name, RHIUAVRef _target, uint32_t _src_offset = 0, uint32_t _dst_offset = 0) {
            Dispatch(_context, _src_name, _target_name, _src_buffer, _target, _src_offset, _dst_offset, GroupSize);
        }
        static void Init(RenderContext& _context);
        static void Dispose();

    private:
        static void Dispatch(RenderContext& _context, std::string_view _src_name, std::string_view _target_name, RHISRVRef _src_buffer, RHIUAVRef _target, uint32_t _src_offset, uint32_t _dst_offset, uint _group_size);
    };
}// namespace Moer

#endif