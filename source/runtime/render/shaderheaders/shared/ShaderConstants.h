#pragma once

// A shared list X(type, name) declares logical arguments in C++ and one GPU block in HLSL.
#ifdef __cplusplus
#define MOER_DECLARE_CONSTANT_ARGUMENT(type, name) DEFINE_SHADER_CONSTANT(type, name);
#define DEFINE_SHADER_CONSTANTS(arguments) arguments(MOER_DECLARE_CONSTANT_ARGUMENT)
#else
#define MOER_CONSTANT_OFFSET_IMPL(name) MOER_PC_OFFSET_##name
#define MOER_CONSTANT_OFFSET(name) MOER_CONSTANT_OFFSET_IMPL(name)
#define MOER_DECLARE_CONSTANT_MEMBER(type, name) \
    [[vk::offset(MOER_CONSTANT_OFFSET(name))]] type name;
#define DEFINE_SHADER_CONSTANTS(arguments)                        \
    namespace Moer {                                             \
        struct MoerPipelineConstants {                           \
            arguments(MOER_DECLARE_CONSTANT_MEMBER)              \
        };                                                       \
    }                                                            \
    [[vk::push_constant]] Moer::MoerPipelineConstants moer_constants;
#endif
