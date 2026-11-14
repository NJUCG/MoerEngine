#pragma once

// A shared list X(type, name) declares logical arguments in C++ and one GPU block in HLSL.
#ifdef __cplusplus
#define MOER_DECLARE_CONSTANT_ARGUMENT(type, name) DEFINE_SHADER_CONSTANT(type, name);
#define DEFINE_SHADER_CONSTANTS(arguments) arguments(MOER_DECLARE_CONSTANT_ARGUMENT)
#else
// Vulkan's bindless index table is plain data, addressed through the same
// push-constant block as the application's constants. Keep application offsets.
#ifdef MOER_PC_BYTE_SIZE
#define MOER_SINGLE_CONSTANT_BYTE_SIZE(type) MOER_PC_BYTE_SIZE
#define MOER_EMPTY_CONSTANT_BYTE_SIZE MOER_PC_BYTE_SIZE
#else
#define MOER_SINGLE_CONSTANT_BYTE_SIZE(type) sizeof(type)
#define MOER_EMPTY_CONSTANT_BYTE_SIZE 0
#endif

#if VULKAN
#define MOER_BINDLESS_ADDRESS_MEMBER(byte_size) \
    [[vk::offset(((byte_size + 7) / 8) * 8)]] uint64_t moer_bindless_index_address;
#define MOER_BINDLESS_ADDRESS_ACCESSOR(address) \
    uint64_t MoerBindlessIndexTableAddress() { return address; }
#define DEFINE_SHADER_BINDLESS_CONSTANTS() \
    struct MoerPipelineConstants { \
        MOER_BINDLESS_ADDRESS_MEMBER(MOER_EMPTY_CONSTANT_BYTE_SIZE) \
    }; \
    [[vk::push_constant]] MoerPipelineConstants moer_constants; \
    MOER_BINDLESS_ADDRESS_ACCESSOR(moer_constants.moer_bindless_index_address)
#else
#define MOER_BINDLESS_ADDRESS_MEMBER(byte_size)
#define MOER_BINDLESS_ADDRESS_ACCESSOR(address)
#define DEFINE_SHADER_BINDLESS_CONSTANTS()
#endif

#define DEFINE_SHADER_CONSTANT(type, name) \
    struct MoerPipelineConstants { \
        [[vk::offset(0)]] type name; \
        MOER_BINDLESS_ADDRESS_MEMBER(MOER_SINGLE_CONSTANT_BYTE_SIZE(type)) \
    }; \
    [[vk::push_constant]] ConstantBuffer<MoerPipelineConstants> moer_constants; \
    MOER_BINDLESS_ADDRESS_ACCESSOR(moer_constants.moer_bindless_index_address)

#define MOER_CONSTANT_OFFSET_IMPL(name) MOER_PC_OFFSET_##name
#define MOER_CONSTANT_OFFSET(name) MOER_CONSTANT_OFFSET_IMPL(name)
#define MOER_DECLARE_CONSTANT_MEMBER(type, name) \
    [[vk::offset(MOER_CONSTANT_OFFSET(name))]] type name;
#define DEFINE_SHADER_CONSTANTS(arguments)                        \
    namespace Moer {                                             \
        struct MoerPipelineConstants {                           \
            arguments(MOER_DECLARE_CONSTANT_MEMBER)              \
            MOER_BINDLESS_ADDRESS_MEMBER(MOER_PC_BYTE_SIZE)       \
        };                                                       \
    }                                                            \
    [[vk::push_constant]] ConstantBuffer<Moer::MoerPipelineConstants> moer_constants; \
    MOER_BINDLESS_ADDRESS_ACCESSOR(moer_constants.moer_bindless_index_address)
#endif
