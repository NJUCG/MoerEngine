//
// Created by 74535 on 2023/10/12.
//

#ifndef VULKAN_RHI_RESOURCE_H
#define VULKAN_RHI_RESOURCE_H

#include "PixelFormat.h"
#include "misc/CountableRef.h"
#include "misc/Crc32.h"
#include "misc/LockFree.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"

#include "shader/ShaderCommon.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <atomic>
#include <volk.h>
#include "VulkanTypeDefs.h"
#include "VulkanSwapChain.h"
#include "vulkan/vulkan_core.h"

#include <vk_mem_alloc.h>

#include <condition_variable>
// #include "VulkanDescriptor.h"
#include <variant>

class VulkanRHIImpl;

namespace Moer::Render {
    class VulkanDevice;
    class VulkanPipelineResourceCache;

    class VulkanDescriptorSetsLayout;
}// namespace Moer::Render
#pragma region forward definitions
class VulkanRHICommandList;
class VulkanRHITexture;
class VulkanRHIAmplificationShader;
class VulkanRHIBlendState;
class VulkanRHIShaderBoundStateInput;
class VulkanRHIBuffer;
class VulkanRHIComputePipelineState;
class VulkanRHIComputeShader;
class VulkanRHIDepthStencilState;
class VulkanRHIGeometryShader;
class VulkanRHIFence;
class VulkanRHIGraphicsPipelineState;
class VulkanRHIMeshShader;
class VulkanRHIPipelineBinaryDataLibrary;
class VulkanRHIFragmentShader;
class VulkanRHIRasterizationState;
class VulkanRHIRayTracingPipelineState;
class VulkanRHIRayTracingScene;
class VulkanRHIRayTracingAccelerationStructure;
class VulkanRHIRayTracingBLAS;
class VulkanRHIRayTracingTLAS;
class VulkanRHIRayTracingShader;
class VulkanRHIRenderQuery;
class VulkanRHIRenderQueryPool;
class VulkanRHISampler;
class VulkanRHIMultisampleState;
class VulkanRHIShader;
class VulkanRHIShaderLibrary;
class VulkanRHITextureSRV;
class VulkanRHIStagingBuffer;
class VulkanRHITextureReference;
class VulkanRHIGlobalBufferLayout;
class VulkanRHIGlobalBuffer;
class VulkanRHITextureUAV;
class VulkanRHIVertexInputState;
class VulkanRHIVertexShader;
class VulkanRHIViewableResource;
class VulkanViewport;
#pragma endregion

#pragma region utils definition

namespace Moer::Render {

    //forward declaration
    class VulkanBuffer;
    class VulkanTexture;
    class VulkanFence;

    using VulkanBufferRef  = CountableRef<VulkanBuffer>;
    using VulkanTextureRef = CountableRef<VulkanTexture>;
    using VulkanFenceRef   = CountableRef<VulkanFence>;

    class VulkanMemoryManager final {
    public:
        VulkanMemoryManager()                                      = delete;
        VulkanMemoryManager(const VulkanMemoryManager&)            = delete;
        VulkanMemoryManager& operator=(const VulkanMemoryManager&) = delete;

        static VmaAllocationCreateFlags MEGenerateVmaMemoryFlags(EBufferUsageFlags _flags);
        static VmaMemoryUsage           MEGenerateVmaMemoryUsage();
    };
    struct FormatInfo {
        uint     stride;
        VkFormat format;
    };
    static const FormatInfo g_platform_pixel_formats[static_cast<uint>(PF_Num)]{
        {0, VK_FORMAT_UNDEFINED},
        {1, VK_FORMAT_R4G4_UNORM_PACK8},
        {2, VK_FORMAT_R4G4B4A4_UNORM_PACK16},
        {2, VK_FORMAT_B4G4R4A4_UNORM_PACK16},
        {2, VK_FORMAT_R5G6B5_UNORM_PACK16},
        {2, VK_FORMAT_B5G6R5_UNORM_PACK16},
        {2, VK_FORMAT_R5G5B5A1_UNORM_PACK16},
        {2, VK_FORMAT_B5G5R5A1_UNORM_PACK16},
        {2, VK_FORMAT_A1R5G5B5_UNORM_PACK16},
        {1, VK_FORMAT_R8_UNORM},
        {1, VK_FORMAT_R8_SNORM},
        {1, VK_FORMAT_R8_USCALED},
        {1, VK_FORMAT_R8_SSCALED},
        {1, VK_FORMAT_R8_UINT},
        {1, VK_FORMAT_R8_SINT},
        {1, VK_FORMAT_R8_SRGB},
        {2, VK_FORMAT_R8G8_UNORM},
        {2, VK_FORMAT_R8G8_SNORM},
        {2, VK_FORMAT_R8G8_USCALED},
        {2, VK_FORMAT_R8G8_SSCALED},
        {2, VK_FORMAT_R8G8_UINT},
        {2, VK_FORMAT_R8G8_SINT},
        {2, VK_FORMAT_R8G8_SRGB},
        {3, VK_FORMAT_R8G8B8_UNORM},
        {3, VK_FORMAT_R8G8B8_SNORM},
        {3, VK_FORMAT_R8G8B8_USCALED},
        {3, VK_FORMAT_R8G8B8_SSCALED},
        {3, VK_FORMAT_R8G8B8_UINT},
        {3, VK_FORMAT_R8G8B8_SINT},
        {3, VK_FORMAT_R8G8B8_SRGB},
        {3, VK_FORMAT_B8G8R8_UNORM},
        {3, VK_FORMAT_B8G8R8_SNORM},
        {3, VK_FORMAT_B8G8R8_USCALED},
        {3, VK_FORMAT_B8G8R8_SSCALED},
        {3, VK_FORMAT_B8G8R8_UINT},
        {3, VK_FORMAT_B8G8R8_SINT},
        {3, VK_FORMAT_B8G8R8_SRGB},
        {4, VK_FORMAT_R8G8B8A8_UNORM},
        {4, VK_FORMAT_R8G8B8A8_SNORM},
        {4, VK_FORMAT_R8G8B8A8_USCALED},
        {4, VK_FORMAT_R8G8B8A8_SSCALED},
        {4, VK_FORMAT_R8G8B8A8_UINT},
        {4, VK_FORMAT_R8G8B8A8_SINT},
        {4, VK_FORMAT_R8G8B8A8_SRGB},
        {4, VK_FORMAT_B8G8R8A8_UNORM},
        {4, VK_FORMAT_B8G8R8A8_SNORM},
        {4, VK_FORMAT_B8G8R8A8_USCALED},
        {4, VK_FORMAT_B8G8R8A8_SSCALED},
        {4, VK_FORMAT_B8G8R8A8_UINT},
        {4, VK_FORMAT_B8G8R8A8_SINT},
        {4, VK_FORMAT_B8G8R8A8_SRGB},
        {4, VK_FORMAT_A8B8G8R8_UNORM_PACK32},
        {4, VK_FORMAT_A8B8G8R8_SNORM_PACK32},
        {4, VK_FORMAT_A8B8G8R8_USCALED_PACK32},
        {4, VK_FORMAT_A8B8G8R8_SSCALED_PACK32},
        {4, VK_FORMAT_A8B8G8R8_UINT_PACK32},
        {4, VK_FORMAT_A8B8G8R8_SINT_PACK32},
        {4, VK_FORMAT_A8B8G8R8_SRGB_PACK32},
        {4, VK_FORMAT_A2R10G10B10_UNORM_PACK32},
        {4, VK_FORMAT_A2R10G10B10_SNORM_PACK32},
        {4, VK_FORMAT_A2R10G10B10_USCALED_PACK32},
        {4, VK_FORMAT_A2R10G10B10_SSCALED_PACK32},
        {4, VK_FORMAT_A2R10G10B10_UINT_PACK32},
        {4, VK_FORMAT_A2R10G10B10_SINT_PACK32},
        {4, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
        {4, VK_FORMAT_A2B10G10R10_SNORM_PACK32},
        {4, VK_FORMAT_A2B10G10R10_USCALED_PACK32},
        {4, VK_FORMAT_A2B10G10R10_SSCALED_PACK32},
        {4, VK_FORMAT_A2B10G10R10_UINT_PACK32},
        {4, VK_FORMAT_A2B10G10R10_SINT_PACK32},
        {2, VK_FORMAT_R16_UNORM},
        {2, VK_FORMAT_R16_SNORM},
        {2, VK_FORMAT_R16_USCALED},
        {2, VK_FORMAT_R16_SSCALED},
        {2, VK_FORMAT_R16_UINT},
        {2, VK_FORMAT_R16_SINT},
        {2, VK_FORMAT_R16_SFLOAT},
        {4, VK_FORMAT_R16G16_UNORM},
        {4, VK_FORMAT_R16G16_SNORM},
        {4, VK_FORMAT_R16G16_USCALED},
        {4, VK_FORMAT_R16G16_SSCALED},
        {4, VK_FORMAT_R16G16_UINT},
        {4, VK_FORMAT_R16G16_SINT},
        {4, VK_FORMAT_R16G16_SFLOAT},
        {6, VK_FORMAT_R16G16B16_UNORM},
        {6, VK_FORMAT_R16G16B16_SNORM},
        {6, VK_FORMAT_R16G16B16_USCALED},
        {6, VK_FORMAT_R16G16B16_SSCALED},
        {6, VK_FORMAT_R16G16B16_UINT},
        {6, VK_FORMAT_R16G16B16_SINT},
        {6, VK_FORMAT_R16G16B16_SFLOAT},
        {8, VK_FORMAT_R16G16B16A16_UNORM},
        {8, VK_FORMAT_R16G16B16A16_SNORM},
        {8, VK_FORMAT_R16G16B16A16_USCALED},
        {8, VK_FORMAT_R16G16B16A16_SSCALED},
        {8, VK_FORMAT_R16G16B16A16_UINT},
        {8, VK_FORMAT_R16G16B16A16_SINT},
        {8, VK_FORMAT_R16G16B16A16_SFLOAT},
        {4, VK_FORMAT_R32_UINT},
        {4, VK_FORMAT_R32_SINT},
        {4, VK_FORMAT_R32_SFLOAT},
        {8, VK_FORMAT_R32G32_UINT},
        {8, VK_FORMAT_R32G32_SINT},
        {8, VK_FORMAT_R32G32_SFLOAT},
        {12, VK_FORMAT_R32G32B32_UINT},
        {12, VK_FORMAT_R32G32B32_SINT},
        {12, VK_FORMAT_R32G32B32_SFLOAT},
        {16, VK_FORMAT_R32G32B32A32_UINT},
        {16, VK_FORMAT_R32G32B32A32_SINT},
        {16, VK_FORMAT_R32G32B32A32_SFLOAT},
        {8, VK_FORMAT_R64_UINT},
        {8, VK_FORMAT_R64_SINT},
        {8, VK_FORMAT_R64_SFLOAT},
        {16, VK_FORMAT_R64G64_UINT},
        {16, VK_FORMAT_R64G64_SINT},
        {16, VK_FORMAT_R64G64_SFLOAT},
        {24, VK_FORMAT_R64G64B64_UINT},
        {24, VK_FORMAT_R64G64B64_SINT},
        {24, VK_FORMAT_R64G64B64_SFLOAT},
        {32, VK_FORMAT_R64G64B64A64_UINT},
        {32, VK_FORMAT_R64G64B64A64_SINT},
        {32, VK_FORMAT_R64G64B64A64_SFLOAT},
        {4, VK_FORMAT_B10G11R11_UFLOAT_PACK32},
        {4, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32},
        {2, VK_FORMAT_D16_UNORM},
        {4, VK_FORMAT_X8_D24_UNORM_PACK32},
        {4, VK_FORMAT_D32_SFLOAT},
        {1, VK_FORMAT_S8_UINT},
        {4, VK_FORMAT_D16_UNORM_S8_UINT},
        {4, VK_FORMAT_D24_UNORM_S8_UINT},
        {4, VK_FORMAT_D32_SFLOAT_S8_UINT},
        {8, VK_FORMAT_BC1_RGB_UNORM_BLOCK},
        {8, VK_FORMAT_BC1_RGB_SRGB_BLOCK},
        {8, VK_FORMAT_BC1_RGBA_UNORM_BLOCK},
        {8, VK_FORMAT_BC1_RGBA_SRGB_BLOCK},
        {16, VK_FORMAT_BC2_UNORM_BLOCK},
        {16, VK_FORMAT_BC2_SRGB_BLOCK},
        {16, VK_FORMAT_BC3_UNORM_BLOCK},
        {16, VK_FORMAT_BC3_SRGB_BLOCK},
        {8, VK_FORMAT_BC4_UNORM_BLOCK},
        {8, VK_FORMAT_BC4_SNORM_BLOCK},
        {16, VK_FORMAT_BC5_UNORM_BLOCK},
        {16, VK_FORMAT_BC5_SNORM_BLOCK},
        {16, VK_FORMAT_BC6H_UFLOAT_BLOCK},
        {16, VK_FORMAT_BC6H_SFLOAT_BLOCK},
        {16, VK_FORMAT_BC7_UNORM_BLOCK},
        {16, VK_FORMAT_BC7_SRGB_BLOCK},
        {8, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK},
        {8, VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK},
        {8, VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK},
        {8, VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK},
        {8, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK},
        {8, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK},
        {8, VK_FORMAT_EAC_R11_UNORM_BLOCK},
        {8, VK_FORMAT_EAC_R11_SNORM_BLOCK},
        {16, VK_FORMAT_EAC_R11G11_UNORM_BLOCK},
        {16, VK_FORMAT_EAC_R11G11_SNORM_BLOCK},
        {16, VK_FORMAT_ASTC_4x4_UNORM_BLOCK},
        {16, VK_FORMAT_ASTC_4x4_SRGB_BLOCK},
        {16, VK_FORMAT_ASTC_5x4_UNORM_BLOCK},
        {16, VK_FORMAT_ASTC_5x4_SRGB_BLOCK},
        {16, VK_FORMAT_ASTC_5x5_UNORM_BLOCK},
        {16, VK_FORMAT_ASTC_5x5_SRGB_BLOCK},
        {16, VK_FORMAT_ASTC_6x5_UNORM_BLOCK},
        {16, VK_FORMAT_ASTC_6x5_SRGB_BLOCK},
        {16, VK_FORMAT_ASTC_6x6_UNORM_BLOCK},
        {16, VK_FORMAT_ASTC_6x6_SRGB_BLOCK},
        {16, VK_FORMAT_ASTC_8x5_UNORM_BLOCK},
        {16, VK_FORMAT_ASTC_8x5_SRGB_BLOCK},
        {16, VK_FORMAT_ASTC_8x6_UNORM_BLOCK},
        {16, VK_FORMAT_ASTC_8x6_SRGB_BLOCK},
        {16, VK_FORMAT_ASTC_8x8_UNORM_BLOCK},
        {16, VK_FORMAT_ASTC_8x8_SRGB_BLOCK},
        {16, VK_FORMAT_ASTC_10x5_UNORM_BLOCK},
        {16, VK_FORMAT_ASTC_10x5_SRGB_BLOCK},
        {16, VK_FORMAT_ASTC_10x6_UNORM_BLOCK},
        {16, VK_FORMAT_ASTC_10x6_SRGB_BLOCK},
        {16, VK_FORMAT_ASTC_10x8_UNORM_BLOCK},
        {16, VK_FORMAT_ASTC_10x8_SRGB_BLOCK},
        {16, VK_FORMAT_ASTC_10x10_UNORM_BLOCK},
        {16, VK_FORMAT_ASTC_10x10_SRGB_BLOCK},
        {16, VK_FORMAT_ASTC_12x10_UNORM_BLOCK},
        {16, VK_FORMAT_ASTC_12x10_SRGB_BLOCK},
        {16, VK_FORMAT_ASTC_12x12_UNORM_BLOCK},
        {16, VK_FORMAT_ASTC_12x12_SRGB_BLOCK},
        /*= 1000156000*/ {4, VK_FORMAT_G8B8G8R8_422_UNORM},
        /*= 1000156001*/ {4, VK_FORMAT_B8G8R8G8_422_UNORM},
        /*= 1000156002*/ {3, VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM},
        /*= 1000156003*/ {3, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM},
        /*= 1000156004*/ {3, VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM},
        /*= 1000156005*/ {3, VK_FORMAT_G8_B8R8_2PLANE_422_UNORM},
        /*= 1000156006*/ {3, VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM},
        /*= 1000156007*/ {2, VK_FORMAT_R10X6_UNORM_PACK16},
        /*= 1000156008*/ {4, VK_FORMAT_R10X6G10X6_UNORM_2PACK16},
        /*= 1000156009*/ {8, VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16},
        /*= 1000156010*/ {4, VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16},
        /*= 1000156011*/ {4, VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16},
        /*= 1000156012*/ {3, VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16},
        /*= 1000156013*/ {3, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16},
        /*= 1000156014*/ {3, VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16},
        /*= 1000156015*/ {3, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16},
        /*= 1000156016*/ {3, VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16},
        /*= 1000156017*/ {2, VK_FORMAT_R12X4_UNORM_PACK16},
        /*= 1000156018*/ {4, VK_FORMAT_R12X4G12X4_UNORM_2PACK16},
        /*= 1000156019*/ {8, VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16},
        /*= 1000156020*/ {4, VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16},
        /*= 1000156021*/ {4, VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16},
        /*= 1000156022*/ {3, VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16},
        /*= 1000156023*/ {3, VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16},
        /*= 1000156024*/ {3, VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16},
        /*= 1000156025*/ {3, VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16},
        /*= 1000156026*/ {3, VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16},
        /*= 1000156027*/ {8, VK_FORMAT_G16B16G16R16_422_UNORM},
        /*= 1000156028*/ {8, VK_FORMAT_B16G16R16G16_422_UNORM},
        /*= 1000156029*/ {3, VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM},
        /*= 1000156030*/ {3, VK_FORMAT_G16_B16R16_2PLANE_420_UNORM},
        /*= 1000156031*/ {3, VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM},
        /*= 1000156032*/ {3, VK_FORMAT_G16_B16R16_2PLANE_422_UNORM},
        /*= 1000156033*/ {3, VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM},
        /*= 1000330000*/ {3, VK_FORMAT_G8_B8R8_2PLANE_444_UNORM},
        /*= 1000330001*/ {3, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16},
        /*= 1000330002*/ {3, VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16},
        /*= 1000330003*/ {3, VK_FORMAT_G16_B16R16_2PLANE_444_UNORM},
        /*= 1000340000*/ {2, VK_FORMAT_A4R4G4B4_UNORM_PACK16},
        /*= 1000340001*/ {2, VK_FORMAT_A4B4G4R4_UNORM_PACK16},
        /*= 1000066000*/ {16, VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK},
        /*= 1000066001*/ {16, VK_FORMAT_ASTC_5x4_SFLOAT_BLOCK},
        /*= 1000066002*/ {16, VK_FORMAT_ASTC_5x5_SFLOAT_BLOCK},
        /*= 1000066003*/ {16, VK_FORMAT_ASTC_6x5_SFLOAT_BLOCK},
        /*= 1000066004*/ {16, VK_FORMAT_ASTC_6x6_SFLOAT_BLOCK},
        /*= 1000066005*/ {16, VK_FORMAT_ASTC_8x5_SFLOAT_BLOCK},
        /*= 1000066006*/ {16, VK_FORMAT_ASTC_8x6_SFLOAT_BLOCK},
        /*= 1000066007*/ {16, VK_FORMAT_ASTC_8x8_SFLOAT_BLOCK},
        /*= 1000066008*/ {16, VK_FORMAT_ASTC_10x5_SFLOAT_BLOCK},
        /*= 1000066009*/ {16, VK_FORMAT_ASTC_10x6_SFLOAT_BLOCK},
        /*= 1000066010*/ {16, VK_FORMAT_ASTC_10x8_SFLOAT_BLOCK},
        /*= 1000066011*/ {16, VK_FORMAT_ASTC_10x10_SFLOAT_BLOCK},
        /*= 1000066012*/ {16, VK_FORMAT_ASTC_12x10_SFLOAT_BLOCK},
        /*= 1000066013*/ {16, VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK},
        /*= 1000054000*/ {8, VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG},
        /*= 1000054001*/ {8, VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG},
        /*= 1000054002*/ {8, VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG},
        /*= 1000054003*/ {8, VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG},
        /*= 1000054004*/ {8, VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG},
        /*= 1000054005*/ {8, VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG},
        /*= 1000054006*/ {8, VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG},
        /*= 1000054007*/ {8, VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG},
        /*= 1000464000*/ {8, VK_FORMAT_R16G16_S10_5_NV}};

    static uint64 EncodeReflectInfo(uint _set, uint _binding, uint _stage_flags) {
        return uint64(_set << 16) | uint64(_binding) | (uint64(_stage_flags) << 32);
    }
    static uint64 EncodeReflectPushConstant(uint _offset, uint _size, uint _stage_flags) {
        return uint64(_offset << 16) | uint64(_size) | (uint64(_stage_flags) << 32);
    }
    static auto DecodeReflectPushConstant(uint64 _val) {
        return std::make_tuple(uint(_val >> 16) & 0xFFFF, uint(_val & 0xFFFF), uint(_val >> 32));
    }
    static auto DecodeReflectInfo(uint64 _val) {
        return std::make_tuple(uint(_val >> 16) & 0xFFFF, uint(_val & 0xFFFF), uint(_val >> 32));
    }
    static uint64 EncodeBindlessInfo(int _texture_set, int _buffer_set, uint _stage_flags, uint8 _has_texture, uint8 _has_buffer) {
        return uint64(_texture_set << 17) | uint64(_buffer_set << 2) | uint64(_has_texture << 1) | uint64(_has_buffer) | (uint64(_stage_flags) << 32);
    }

    static auto DecodeBindlessInfo(uint64 _val) {
        return std::make_tuple(int(_val >> 17) & 0x7FFF, int(_val >> 2) & 0x7FFF, uint(_val >> 32), uint8(_val >> 1) & 0x1, uint8(_val) & 0x1);
    }
    struct VulkanDescriptorSetLayoutBinding {
        VkDescriptorSetLayoutBinding binding;
        int                          param_idx;
    };
    struct VulkanDescriptorSetLayoutCreateInfo {
        VkDescriptorSetLayoutCreateInfo                      layout_create_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        UnorderedMap<uint, VulkanDescriptorSetLayoutBinding> bindings{};
        bool                                                 is_bindless = false;
        VkDescriptorSetLayoutBinding&                        operator[](uint _binding) { return bindings[_binding].binding; }
        auto                                                 TryEmplaceBinding(uint _binding, VkDescriptorSetLayoutBinding&& _binding_info) { return bindings.try_emplace(_binding, std::move(_binding_info)); }
    };

    struct VulkanDescriptorInfo {
        /** offset in current descriptor set */
        // uint offset;
        /** index in ArrayArguments */
        uint param_idx;
        /** index in descinfo arrays */
        uint info_idx;
    };
    struct DescBufferOffsetInfo {
        uint                set;
        VkPipelineBindPoint bind_point;
        VkPipelineLayout    layout;
        uint                buf_idx;
        uint64              offset;
    };
    struct VulkanDescriptorSetBinder {
        Array<VkWriteDescriptorSet>                        writers;
        Array<VulkanDescriptorInfo>                        bind_infos;
        Array<VkDescriptorImageInfo>                       image_infos;
        Array<VkDescriptorBufferInfo>                      buffer_infos;
        Array<VkWriteDescriptorSetAccelerationStructureNV> accel_structures;

        VkPushDescriptorSetInfoKHR push_info;
        VkPipelineBindPoint        bind_point;

        struct BindingInfo {
            uint64 offset;    //set offset in descriptor buffer
            uint64 src_handle;//cpu handle in global buffer

            uint binding;
        };
        Array<BindingInfo> binding_infos;
        uint64             size;//size in descriptor buffer
        uint64             pipeline_offset;
        uint               desc_idx;
        uint               offset_idx;
    };

    struct VulkanBindlessSetArray {
        //index in ArrayArguments
        uint param_idx;
        //descriptor buffer index
        uint desc_idx;
    };

    struct VulkanBindlessSetImage {
        //index in ArrayArguments
        uint param_idx;
        //descriptor buffer index
        uint desc_idx;
    };

    struct VulkanBindlessSetSampler {
        //index in ArrayArguments
        uint param_idx;
        //descriptor buffer index
        uint desc_idx;
    };

    struct VulkanPipelineParamBinder {
        UnorderedMap<uint, std::variant<VulkanDescriptorSetBinder, VulkanBindlessSetArray, VulkanBindlessSetImage, VulkanBindlessSetSampler>> set_binders;
        VkPushConstantsInfoKHR                                                                                                                push_constants_info;
        //descriptor buffer bind template
        Array<VkDescriptorBufferBindingInfoEXT> desc_buffers;
        //set offsets in descriptor buffers
        Array<DescBufferOffsetInfo> desc_buffer_offsets;
    };

    class VulkanEnumTranslator final {
    public:
        static VkIndexType           METoVKIndexType(EIndexElementType _type);
        static VkFormat              METoVKFormat(EPixelFormat _format);
        static VkImageType           METoVKImageType(ETextureDimension _dim);
        static VkImageUsageFlags     METoVKImageUsageFlags(ETextureUsageFlags _me_flags);
        static EPixelFormat          VKToMEFormat(VkFormat _format);
        static VkBufferUsageFlags    METoVKBufferUsageFlags(EBufferUsageFlags _me_flags);
        static VkDescriptorType      METoVkBufferDescriptorType(EBufferUsageFlags _type);
        static VkSampleCountFlagBits METoVKSampleCountFlagBits(uint32_t _me_count);
        static VkImageAspectFlags    METoVKImageAspectFlags(ETextureAspectFlags _flags);
        static VkImageViewType       METoVKImageViewType(ETextureDimension _dim);
        static VkImageLayout         METoVKImageLayout(ETextureLayout _layout);
        static VkAttachmentLoadOp    METoVKAttachmentLoadOp(EAttachmentLoadOp _load_op);
        static VkAttachmentStoreOp   METoVKAttachmentStoreOp(EAttachmentStoreOp _store_op);
        static VkFilter              METoVKImageFilter(ESamplerFilter _filter);

        static VkPipelineStageFlags2 METoVkPipelineStageFlags2(ERHIPipelineStageFlags _flags);
        static VkAccessFlags2        METoVkAccessFlags2(ERHIAccessFlags _flags);

        static VkCullModeFlags     METoVKCullModeFlags(ERasterizerCullMode _cull_mode);
        static VkPrimitiveTopology METoVKPrimitiveTopology(EPrimitiveTopology _primitive_type);
        static VkPolygonMode       METoVKPolygonMode(ERasterizerFillMode _fill_mode);

        static VkDescriptorType   METoVKDescriptorType(EShaderParameterType _type, EShaderCodeResourceBindingType _binding_type);
        static VkShaderStageFlags METoVKShaderStageFlags(EShaderType _type);

        static uint32_t METoVkQueueFamilyIndex(ECommandQueueType _type, const VulkanDevice* _device);
        static uint32_t METoVkQueueFamilyIndex(ECommandListType _type, const VulkanDevice* _device);

        static VkFilter             METoVKMinMagFilterMode(ESamplerFilter _filter);
        static VkSamplerMipmapMode  METoVKMipmapMode(ESamplerFilter _filter);
        static VkSamplerAddressMode METoVKWrapMode(ESamplerAddressMode _address_mode);
        static VkCompareOp          METoVKCompareOpSampler(ESamplerCompareFunction _compare_op);

        static VkCompareOp METoVKCompareOp(ECompareOption _compare_op);
        static VkStencilOp METoVKStencilOp(EStencilOp _stencil_op);

        static VkBlendOp     METoVKBlendOp(EBlendOperation _blend_op);
        static VkBlendFactor METoVKBlendFactor(EBlendFactor _blend_factor);

        static VkVertexInputRate METoVKVertexInputRate(EVertexInputRate _me_rate);

        static VkQueueFlagBits METoVKQueueFlagBits(EQueueType _type);

        //Raytracing
        static VkGeometryTypeKHR                    METoVKGeometryType(ERayTracingGeometryType _type);
        static VkGeometryFlagsKHR                   METoVKGeometryFlags(ERayTracingGeometryFlags _flags);
        static VkBuildAccelerationStructureFlagsKHR METoVKAccelerationStructureBuildType(ERayTracingAccelerationStructureBuildFlags _type);
        static VkBuildAccelerationStructureModeKHR  METoVKBuildAccelerationStructureMode(ERaytracingBuildMode _mode);
    };

#pragma endregion

    class VulkanRHISampler final : public RHISampler {
    public:
        explicit VulkanRHISampler() : RHISampler() {}

        void GenerateSamplerFromInitializer(const VulkanDevice* _device, const RHISamplerCreateInfo& _initializer);

        inline VkSampler GetHandle() const {
            return m_sampler;
        }

        inline VkImageLayout GetImageLayout() const {
            return m_image_layout;
        }

    private:
        VkFilter             METoVKMinMagFilterMode(ESamplerFilter _filter);
        VkSamplerMipmapMode  METoVKMipmapMode(ESamplerFilter _filter);
        VkSamplerAddressMode METoVKWrapMode(ESamplerAddressMode _address_mode);
        VkCompareOp          METoVKCompareOp(ESamplerCompareFunction _compare_op);

    private:
        VkSampler     m_sampler;
        VkImageLayout m_image_layout;
    };

#pragma region shader definitions

    class VulkanRHIGraphicsShader {
        friend VulkanRHIImpl;

    public:
        explicit VulkanRHIGraphicsShader() : m_shader_module(VK_NULL_HANDLE) {}

        inline VkShaderModule GetHandle() const {
            return m_shader_module;
        }

    protected:
        VkShaderModule m_shader_module;
    };

    class VulkanRHIVertexShader : public RHIVertexShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIVertexShader(const Shader* _meta_shader) : RHIVertexShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIFragmentShader : public RHIFragmentShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIFragmentShader(const Shader* _meta_shader) : RHIFragmentShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIGeometryShader : public RHIGeometryShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIGeometryShader(const Shader* _meta_shader) : RHIGeometryShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIComputeShader : public RHIComputeShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIComputeShader(const Shader* _meta_shader) : RHIComputeShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIMeshShader : public RHIMeshShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIMeshShader(const Shader* _meta_shader) : RHIMeshShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIAmplificationShader : public RHIAmplificationShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIAmplificationShader(const Shader* _meta_shader) : RHIAmplificationShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIRayGenShader : public RHIRayGenShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIRayGenShader(const Shader* _meta_shader) : RHIRayGenShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIRayMissShader : public RHIRayMissShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIRayMissShader(const Shader* _meta_shader) : RHIRayMissShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIRayClosestHitShader : public RHIRayClosestHitShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIRayClosestHitShader(const Shader* _meta_shader) : RHIRayClosestHitShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIRayCallableShader : public RHIRayCallableShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIRayCallableShader(const Shader* _meta_shader) : RHIRayCallableShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIRayIntersectionShader : public RHIRayIntersectionShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIRayIntersectionShader(const Shader* _meta_shader) : RHIRayIntersectionShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

    class VulkanRHIRayAnyhitShader : public RHIRayAnyhitShader, public VulkanRHIGraphicsShader {
    public:
        explicit VulkanRHIRayAnyhitShader(const Shader* _meta_shader) : RHIRayAnyhitShader(_meta_shader), VulkanRHIGraphicsShader() {}
    };

#pragma endregion

#pragma region pipeline states definitions

    class VulkanDeviceObject {
    public:
        VulkanDeviceObject(VulkanDevice* _device = nullptr);

    protected:
        VulkanDevice* m_device;
        uint          b_deferred_delete : 1 = true;
    };
    class VulkanPipelineState : public PipelineState, public VulkanDeviceObject {
        enum EType {
            GFX,
            Compute,
            RT
        };

    public:
        VulkanPipelineState(VulkanDevice* _device, EType _type = EType::GFX) : PipelineState(), VulkanDeviceObject(_device), m_pipeline(VK_NULL_HANDLE), m_pipeline_layout(VK_NULL_HANDLE), m_pipeline_state_cache(nullptr), m_type(_type){};
        virtual ~VulkanPipelineState();

        inline VkPipeline GetHandle() const {
            return m_pipeline;
        }

        inline const VkPipelineLayout GetPipelineLayout() const {
            return m_pipeline_layout;
        }

        inline VulkanPipelineResourceCache* GetPipelineResourceCache() const {
            return m_pipeline_state_cache;
        }

        inline const Moer::Render::VulkanDescriptorSetsLayout* GetDescriptorSetsLayout() const {
            return m_descriptor_sets_layout;
        }

        void                InitDescriptorSetLayouts(Moer::Array<Moer::Render::TDescriptorSetLayoutBindingArray>& _descriptor_bindings);
        void                InitPipelineResourceCache(const Moer::Array<Moer::Render::TDescriptorSetLayoutBindingArray>& _descriptor_bindings);
        void                CreatePipelineLayout(const VkPipelineLayoutCreateInfo& _pipeline_layout_ci);
        VkPipelineBindPoint GetPipelineBindPoint() {
            switch (m_type) {
                case GFX:
                    return VK_PIPELINE_BIND_POINT_GRAPHICS;
                case Compute:
                    return VK_PIPELINE_BIND_POINT_COMPUTE;
                case RT:
                    return VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
                default:
                    return VK_PIPELINE_BIND_POINT_GRAPHICS;
            }
        }
        void InitPipelineLayout(UnorderedMap<uint, struct VulkanDescriptorSetLayoutCreateInfo>&&, std::optional<VkPushConstantRange> _push_constant_range = std::nullopt);

    public:
        UniquePtr<struct VulkanPipelineParamBinder> bind_template;

    protected:
        friend VulkanDevice;
        VkPipeline                   m_pipeline;
        VkPipelineLayout             m_pipeline_layout;
        Array<VkDescriptorSetLayout> descriptor_set_layouts;
        // descriptor sets
        Moer::Render::VulkanDescriptorSetsLayout* m_descriptor_sets_layout = nullptr;
        // resource cache
        VulkanPipelineResourceCache* m_pipeline_state_cache = nullptr;
        EType                        m_type;
    };

    class VulkanRHIGraphicsPipelineState final : public VulkanPipelineState {
    public:
        VulkanRHIGraphicsPipelineState(VulkanDevice* _device)
            : VulkanPipelineState(_device) {}

        virtual ~VulkanRHIGraphicsPipelineState();

        static Moer::Array<VkPipelineShaderStageCreateInfo> METoVKShaderStageCreateInfo(const RHIShaderBoundStateInput& _shader_bound_state);
        static VkPipelineVertexInputStateCreateInfo         METoVKVertexInputStateCreateInfo(const RHIVertexInputInfo& _vertex_input_state);
        static Moer::Array<const Shader*>                   GetShaderInfoList(const RHIShaderBoundStateInput& _shader_bound_state);

        void CreateGraphicsPipeline(const VkGraphicsPipelineCreateInfo& _info);
    };

    class VulkanRHIComputePipelineState final : public VulkanPipelineState {
    public:
        VulkanRHIComputePipelineState(VulkanDevice* _device)
            : VulkanPipelineState(_device) {}

        void CreateComputePipeline(const VkComputePipelineCreateInfo& _info);
    };

    class VulkanRHIRayTracingPipelineState final : public VulkanPipelineState {
        friend VulkanRHIImpl;

    public:
        VulkanRHIRayTracingPipelineState(VulkanDevice* _device)
            : VulkanPipelineState(_device) {}

        const VkStridedDeviceAddressRegionKHR* GetRayGenSBT() { return &m_raygen_sbt; }
        const VkStridedDeviceAddressRegionKHR* GetRayMissSBT() { return &m_miss_sbt; }
        const VkStridedDeviceAddressRegionKHR* GetRayHitSBT() { return &m_hit_sbt; }
        const VkStridedDeviceAddressRegionKHR* GetRayCallableSBT() { return &m_callable_sbt; }

        void CreateRayTracingPipeline(const VkRayTracingPipelineCreateInfoKHR& _info);

    private:
        //SBT
        RHIBufferRef                    m_sbt_buffer;// MARK: should use RHIBufferRef instead of VkBuffer?
        VkStridedDeviceAddressRegionKHR m_raygen_sbt;
        VkStridedDeviceAddressRegionKHR m_miss_sbt;
        VkStridedDeviceAddressRegionKHR m_hit_sbt;
        VkStridedDeviceAddressRegionKHR m_callable_sbt;
    };
#pragma endregion

#pragma region global buffer definitions
#pragma endregion

#pragma region viewable resources definitions

    class VulkanRHIBuffer : public RHIBuffer {
        friend VulkanRHIImpl;

    public:
        VulkanRHIBuffer() = delete;
        VulkanRHIBuffer(const RHIBufferInfo& _info) : RHIBuffer(_info) {}

        inline const VmaAllocation GetAllocation() const {
            return m_alloc.alloc;
        }

        inline VkBuffer GetHandle() const {
            return m_alloc.buffer;
        }

        static VkIndexType        METoVKIndexType(EIndexElementType _type);
        static VkBufferUsageFlags METoVKBufferUsageFlags(VulkanDevice* _device, EBufferUsageFlags _me_flags);

    private:
        struct BufferAlloc {
            VkBuffer      buffer;
            VmaAllocation alloc;
        } m_alloc;
    };

    class VulkanStagingBuffer : public RHIBuffer {
        friend VulkanRHIImpl;
        ~VulkanStagingBuffer();

    public:
        VulkanStagingBuffer() = delete;
        VulkanStagingBuffer(VulkanRHIBuffer* _buffer);
    };

    class VulkanRHITexture final : public RHITexture, public VulkanDeviceObject {
        friend VulkanRHIImpl;

    public:
        VulkanRHITexture() = delete;
        ~VulkanRHITexture();

        explicit VulkanRHITexture(const RHITextureCreateInfo& _info, VulkanDevice* _device);

        //for inner usage only
        explicit VulkanRHITexture(const RHITextureCreateInfo& _info, VkImage _image, VulkanDevice* _device);

        inline const VmaAllocation GetAllocation() const {
            return m_alloc.alloc;
        }

        inline VkImage GetHandle() const {
            return m_alloc.image;
        }
        //for inner usage only
        inline void SetAttachedImageInner(VkImage _image) {
            m_alloc.image = _image;
        }

        static VkImageType       METoVKImageType(ETextureDimension _dim);
        static VkImageUsageFlags METoVKImageUsageFlags(ETextureUsageFlags _me_flags);

    private:
        struct TextureAlloc {
            VkImage       image;
            VmaAllocation alloc;
        } m_alloc;
    };

    class VulkanBuffer : public Buffer, VulkanDeviceObject {
        friend VulkanRHIImpl;

    public:
        struct BufferAlloc {
            VkBuffer      buffer;
            VmaAllocation alloc;
        };
        VulkanBuffer() = delete;
        virtual ~VulkanBuffer();
        virtual void Destroy() override;
        void         SetName(const std::string_view _name) override;

        VulkanBuffer(const BufferInfo& _info, VulkanDevice& _device);
        VulkanBuffer(const BufferInfo& _info, VulkanDevice& _device, VkBuffer _handle, VmaAllocation _alloc, bool _defer_destroy, bool _get_address = false);
        uint64                     DeviceAddress() const;
        inline const VmaAllocation GetAllocation() const {
            return m_alloc.alloc;
        }

        inline VkBuffer GetHandle() const {
            return m_alloc.buffer;
        }

        VkAccessFlags2             m_access_flags   = VK_ACCESS_2_NONE;
        VkPipelineStageFlags2      m_stage_flags    = VK_PIPELINE_STAGE_2_NONE;
        int                        m_descriptor_idx = -1;
        UnorderedMap<uint64, uint> m_descriptor_indices;

        VkDescriptorType GetDescriptorType() const {
            return m_descriptor_type;
        }

    private:
        friend class TempBufferAllocator;
        BufferAlloc      m_alloc;
        uint64           m_device_address = 0;
        VkDescriptorType m_descriptor_type;
    };

    struct VkTextureDescKey {
        VkImageLayout layout;
        uint8         mip_level;
        uint8         mip_cnt;

        bool operator==(const VkTextureDescKey& _other) const {
            return layout == _other.layout && mip_level == _other.mip_level && mip_cnt == _other.mip_cnt;
        }

        struct Hasher {
            size_t operator()(const VkTextureDescKey& _key) const {
                return std::hash<uint32_t>()(uint32_t(_key.layout)) ^ std::hash<uint32_t>()(uint32_t(_key.mip_level)) ^ std::hash<uint32_t>()(uint32_t(_key.mip_cnt));
            }
        };
    };
    class VulkanTexture final : public Texture, public VulkanDeviceObject {
        friend VulkanRHIImpl;

    public:
        VulkanTexture() = delete;
        ~VulkanTexture();
        //for inner usage only
        explicit VulkanTexture(const TextureInfo& _info, VulkanDevice* _device, VkImage _image = VK_NULL_HANDLE);

        inline const VmaAllocation GetAllocation() const {
            return m_alloc.alloc;
        }

        inline VkImage GetHandle() const {
            return m_alloc.image;
        }
        //for inner usage only
        inline void SetAttachedImageInner(VkImage _image) {
            m_alloc.image = _image;
        }

        static VkImageType       METoVKImageType(ETextureDimension _dim);
        static VkImageUsageFlags METoVKImageUsageFlags(ETextureUsageFlags _me_flags);

        uint        GetMipByteSize(uint _mip_idx) const override;
        VkImageView GetView(uint _mip_level = 0, uint _mip_cnt = 1);
        bool        IsGeneralRead(uint _mip_level = 0) const;

        void SetName(const std::string_view _name) override;
        struct SubResourceStates {
            uint8                 mip_level;
            uint8                 mip_cnt;
            VkAccessFlags2        access;
            VkImageLayout         layout;
            VkPipelineStageFlags2 stage;
        };
        Array<SubResourceStates> m_subresource_states;
        SubResourceStates        state;
        bool                     b_has_preferred_state : 1 = false;
        bool                     b_present : 1             = false;
        VkImageLayout            GetPreferredLayout() { return m_preferred_layout; };

        VkImageLayout GetQueuePreferredLayout(EQueueType _queue) {
            switch (_queue) {
                case EQueueType::Graphics:
                    return m_preferred_layout;
                case EQueueType::Compute:
                    return m_preferred_layout;
                case EQueueType::Copy:
                    return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                default:
                    return VK_IMAGE_LAYOUT_GENERAL;
            }
        }

        int32 GetDescriptorIndex(uint _mip_level, uint _mip_idx, VkImageLayout _layout) {
            VkTextureDescKey key = {_layout, uint8(_mip_level), uint8(_mip_idx)};
            auto             it  = m_descriptor_indices.find(key);
            if (it != m_descriptor_indices.end()) {
                return it->second;
            }
            return -1;
        }
        UnorderedMap<VkTextureDescKey, uint64, VkTextureDescKey::Hasher> m_descriptor_indices;

    private:
        void Destroy() override;
        struct TextureAlloc {
            VkImage       image;
            VmaAllocation alloc;
        } m_alloc;
        UnorderedMap<uint, VkImageView>                                           m_views;
        Moer::UnorderedMap<Moer::uint, std::tuple<ETextureStateFlags, EPassType>> mip_usages;
        VkImageLayout                                                             m_preferred_layout = VK_IMAGE_LAYOUT_GENERAL;
    };

    class VulkanBindlessArray final : public BindlessArray, public VulkanDeviceObject {
    public:
        enum EType : uint {
            Texture,
            Buffer
        };
        struct Handle {
            uint slot : 22;
            uint attrib : 8;
            uint type : 2;

            bool IsTexture() const { return type == Texture; }
            bool IsBuffer() const { return type == Buffer; }
        };

        VulkanBindlessArray(VulkanDevice* _device, uint32 _max_size);
        ~VulkanBindlessArray() override;

        uint AllocateTexture(const TextureView& _texture, Sampler _sampler) override;
        uint AllocateBuffer(BufferView _buffer) override;
        void FreeTexture(uint _slot) override;
        void FreeBuffer(uint _slot) override;
        bool IsResourceAllocated(uint64 _handle) const;
        //call on update
        void CmdUpdate(Array<TextureUpdateInfo>&& _textures_allocated, Array<BufferUpdateInfo>&& _buffers_allocated);
        //call on frame end free
        void OnFree(const Array<uint>& _slots_freed, const Array<uint>& _textures_freed, const Array<uint>& _buffers_freed);

        uint64 ArrayHandle() const override { return (uint64)bindless_array_buffer; }

        void Lock() { mtx.lock(); }
        void Unlock() { mtx.unlock(); }

    public:
        VulkanBuffer* bindless_array_buffer;
        VulkanBuffer* bindless_buffer_descs;
        VulkanBuffer* bindless_texture_descs;
        uint64        texture_offset_in_buffer;
        uint64        buffers_offset_in_set;
        Array<Handle> handles;

    private:
        std::mutex mtx;

    protected:
        UniquePtr<Command>          CreateUpdateCommand() override;
        class VulkanDescriptorHeap& g_heap;

        LockFreeQueueBase<uint, true> free_texture_slots;
        LockFreeQueueBase<uint, true> free_buffer_slots;
        LockFreeQueueBase<uint, true> free_slots;
        std::atomic_uint              texture_slot_offset;
        std::atomic_uint              buffer_slot_offset;
        std::atomic_uint              slot_offset;
        //frame resources
        Array<TextureUpdateInfo> textures_allocated;
        Array<BufferUpdateInfo>  buffers_allocated;

        Array<uint>          textures_freed;
        Array<uint>          buffers_freed;
        Array<uint>          slots_freed;
        UnorderedSet<uint64> resource_allocated_set;

        uint64 textures_offset_in_set;
    };

#pragma endregion

#pragma region[ raytracing ]
    static void GetBuildRaytracingGeometryInfo(
        Array<VkAccelerationStructureGeometryKHR>&       _build_info,
        Array<VkAccelerationStructureBuildRangeInfoKHR>& _build_ranges,
        const RaytracingGeometryInfo&                    _info);

    struct VulkanAccelerationStructure : RHIResource {
        VulkanAccelerationStructure(VulkanDevice& _device);
        virtual ~VulkanAccelerationStructure() override;
        void Destroy() override;

        VkAccelerationStructureKHR handle            = VK_NULL_HANDLE;
        VulkanBuffer*              underlying_buffer = nullptr;
        int                        m_descriptor_idx  = -1;
        VulkanDevice&              device;
    };

    using VulkanAccelRef = CountableRef<VulkanAccelerationStructure>;
    class VulkanRaytracingGeometry final : public RaytracingGeometry,
                                           public VulkanDeviceObject {
    public:
        VulkanRaytracingGeometry(const RaytracingGeometryInfo& _info, VulkanDevice* _device);
        virtual ~VulkanRaytracingGeometry();

        inline VkAccelerationStructureKHR GetHandle() const {
            return acc;
        }

        inline VulkanBuffer* GetUnderlyingBuffer() const {
            return underlying_buffer;
        }
        Array<VkAccelerationStructureGeometryKHR>       build_geometries;
        Array<VkAccelerationStructureBuildRangeInfoKHR> build_ranges;
        VkAccelerationStructureBuildSizesInfoKHR        build_sizes_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        uint64                                          blas_address;

    private:
    private:
        VkAccelerationStructureKHR acc;
        VulkanBuffer*              underlying_buffer;
    };

    class VulkanRaytracingScene final : public RaytracingScene, public VulkanDeviceObject {
    public:
        VulkanRaytracingScene(VulkanDevice* _device);
        virtual ~VulkanRaytracingScene();
        RaytracingInstance& AddInstance() override;

        void MarkModified(uint _array_idx) override;
        void FreeInstance(uint _array_idx) override;

        void RegisterGeometry(RaytracingGeometryRef _geometry) override;
        void UnregisterGeometry(RaytracingGeometryRef _geometry) override;

        UniquePtr<Command> UpdateScene() override;

    public:
        void                RefitInstanceBuffer();
        bool                RefitTLASAndScratchBuffer();
        RaytracingSizeInfos CalculateSizeInfos();
        // temperory
        Array<uint> temp_update_instance_ids;
        Set<uint>   temp_modified_instance_ids;

        Array<byte> temp_update_instances;

    public:
        RaytracingSizeInfos size_infos{};
        VulkanAccelRef      tlas           = nullptr;
        VulkanBufferRef     scratch_buffer = nullptr;

        VulkanBufferRef instance_buffer = nullptr;

        Array<VkAccelerationStructureInstanceKHR>
            vk_instances;

        Array<uint> free_instance_slots;

        std::mutex                 geom_mutex;
        UnorderedMap<uint64, uint> related_geometries;

    private:
        uint instance_capacity = 1000;
        uint exponent          = 2;
        uint instance_offset   = 1;
    };

#pragma endregion

#pragma region shader param
#pragma endregion

#pragma region synchronization

    class VulkanRHIFence final : public RHIFence {
    public:
        VulkanRHIFence(VulkanDevice* _device, EFenceUsageFlags _usage);
        virtual ~VulkanRHIFence();

        uint64_t GetValue() const override;

        void                    Wait(uint64_t value) override;
        inline VkSemaphore      GetSemaphoreHandle() { return m_timeline; }
        inline VkSemaphore      GetBinaryHandle() { return m_binary; }
        inline EFenceUsageFlags GetUsage() { return usage; }

    private:
        VulkanDevice*    m_device;
        VkSemaphore      m_timeline;
        VkSemaphore      m_binary;
        EFenceUsageFlags usage;
    };

#define RESOURCE_CAST(RHIType, VkType) \
    inline VkType* ResourceCast(RHIType* _val) { return static_cast<VkType*>(_val); }

    class VulkanFence final : public Fence, VulkanDeviceObject {
    public:
        VulkanFence(VulkanDevice&);
        virtual ~VulkanFence();

        uint64_t GetValue() const override;

        void Wait(uint64_t _value) override;
        // can be called on any thread to block current thread
        void Sync(uint64);
        // can be called on any thread to signal fence
        void        Notify(uint64);
        void        HostWait(uint64_t _value);
        void        SignalHost(uint64_t _value);
        auto&       GetFence() { return timeline; }
        VkSemaphore GetUnderlyingHandle() { return timeline; }

    public:
        uint64 current_value = 0;
        // uint64 last_value    = 0;

    private:
        VkSemaphore             timeline;
        std::condition_variable cv;
        std::mutex              cv_m;
    };

#pragma endregion

#pragma region[ resource cast ]
    RESOURCE_CAST(Buffer, VulkanBuffer)
    RESOURCE_CAST(Texture, VulkanTexture)
    RESOURCE_CAST(Fence, VulkanFence)
    RESOURCE_CAST(Swapchain, VkSwapchain)

    RESOURCE_CAST(RaytracingGeometry, VulkanRaytracingGeometry)
    RESOURCE_CAST(RaytracingScene, VulkanRaytracingScene)
#pragma endregion

#pragma region viewable resources view definitions
    class VulkanRHIViewport;
    class VulkanRHITextureUAV final : public RHIUAV, public VulkanDeviceObject {
        friend VulkanRHIImpl;
        friend Moer::Render::VulkanRHIViewport;

    public:
        virtual ~VulkanRHITextureUAV();
        explicit VulkanRHITextureUAV(VulkanDevice* _device, RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHIUAV(_resource, _viewInfo), VulkanDeviceObject(_device) {}

        inline VkImageView GetView() const { return m_view; }

    private:
        VkImageView m_view;
    };

    class VulkanRHICBV final : public RHICBV, public VulkanDeviceObject {
        friend VulkanRHIImpl;

    public:
        virtual ~VulkanRHICBV();
        explicit VulkanRHICBV(
            VulkanDevice*      _device,
            RHIBuffer*         _resource,
            const RHIViewInfo& _viewInfo) : RHICBV(_resource, _viewInfo), VulkanDeviceObject(_device) {}
    };
    class VulkanRHIBufferUAV final : public RHIUAV, public VulkanDeviceObject {
        friend VulkanRHIImpl;

    public:
        virtual ~VulkanRHIBufferUAV();
        explicit VulkanRHIBufferUAV(VulkanDevice* _device, RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHIUAV(_resource, _viewInfo), VulkanDeviceObject(_device) {}

        inline VkBufferView GetView() const { return m_view; }

    private:
        VkBufferView m_view;
    };

    class VulkanRHITextureSRV final : public RHISRV, public VulkanDeviceObject {
        friend VulkanRHIImpl;

    public:
        virtual ~VulkanRHITextureSRV();
        explicit VulkanRHITextureSRV(VulkanDevice* _device, RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHISRV(_resource, _viewInfo), VulkanDeviceObject(_device) {}
        inline VkImageView GetView() const { return m_view; }

    private:
        VkImageView m_view = VK_NULL_HANDLE;
    };

    class VulkanRHIBufferSRV final : public RHISRV, public VulkanDeviceObject {
        friend VulkanRHIImpl;

    public:
        virtual ~VulkanRHIBufferSRV();
        explicit VulkanRHIBufferSRV(VulkanDevice* _device, RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHISRV(_resource, _viewInfo), VulkanDeviceObject(_device) {}

        inline VkBufferView GetView() const { return m_view; }

    private:
        VkBufferView m_view = VK_NULL_HANDLE;
    };

    class VulkanRHIAccelerationStructureSRV final : public RHISRV, public VulkanDeviceObject {
        friend VulkanRHIImpl;

    public:
        virtual ~VulkanRHIAccelerationStructureSRV();
        explicit VulkanRHIAccelerationStructureSRV(VulkanDevice* _device, RHIViewableResource* _resource, const RHIViewInfo& _viewinfo) : RHISRV(_resource, _viewinfo), VulkanDeviceObject(_device){};
    };

    class VulkanImageView final : public RHIView {
        friend VulkanRHIImpl;

    public:
        explicit VulkanImageView(RHIViewableResource* _resource, const RHIViewInfo& _viewInfo) : RHIView(RRT_ATTACHMENT_VIEW, _resource, _viewInfo) {}

        explicit VulkanImageView(RHIViewableResource* _resource, VkImageView _view, const RHIViewInfo& _viewInfo) : RHIView(RRT_ATTACHMENT_VIEW, _resource, _viewInfo), m_view(_view) {
        }
        inline VkImageView GetView() const { return m_view; }

    private:
        VkImageView m_view = VK_NULL_HANDLE;
    };

#pragma endregion

#pragma region viewport

    class VulkanRHIViewport final : public RHIViewport {
        friend class VkCommandQueue;

    public:
        VulkanRHIViewport(class VulkanSwapChain* _swapchain, uint32_t _max_frame_in_flight);
        ~VulkanRHIViewport();
        virtual void OnResize(Extent2D _size) override;

        virtual void    Present(RHIFence* _render_finished) override;
        VulkanRHIFence* GetAcquireNextImageFence();

        RHIViewportNextBackBufferInfo GetNextFrameBackBufferInfo() override;

        VulkanRHITextureUAV* GetCurrentBackBuffer(uint32_t index);

        virtual void WaitForQueueComplete(class RHICommandQueue* _command_queue, RHIFence* _optional_fence) override;

        virtual ViewPort GetViewportExtent() const override;

    private:
        void                 InnerCreateResources();
        void                 InnerDestroyResources();
        void                 ResetResources();
        VulkanTexture*       GetSwapchainImage(uint32_t _index);
        VulkanFence*         GetBinaryFence(uint32_t _index);
        void                 Present(VkSemaphore _fence);
        VulkanRHITextureUAV* InnerCreateVulkanUAV(VulkanDevice* _device, VulkanRHITexture* texture, const RHIViewInfo& _view_info);

        class VulkanSwapChain* swapchain;

        Moer::Array<VulkanRHIFence*> image_aquire_fences;

        Moer::Array<VulkanRHITextureUAV*> swapchain_image_uavs;

        Moer::Array<VulkanRHITexture*> swapchain_images;
        //new resources
        Moer::Array<VulkanTexture*> swapchain_textures;
        Moer::Array<VulkanFence*>   frame_fences;

        uint32_t frame_offset = 0;

        // uint32_t max_frame_in_flight = 3;
    };

    class VulkanViewport final : public Viewport, VulkanDeviceObject {
    public:
        VulkanViewport(RHIViewportInitializer _init_info, VulkanDevice& _device);
        // ~VulkanViewport();
        void Resize(Extent2D _size) override;
        void Present(FenceRef _render_finished) override {};
        void Present(VkSemaphore _sem);
        // BackBufferInfo GetBackBuffer() override;
        void* GetNativeWindow() override;

    private:
        VulkanSwapChain m_swap_chain;
        void            CreateResources();

        Array<VulkanTexture*> m_back_buffers;
        uint32_t              frame_offset = 0;
        Array<VulkanFence*>   m_frame_fences;
    };
#pragma endregion

#pragma region acceleration structure definitions
    class VulkanRHIRayTracingAccelerationStructure {
    public:
        static VkGeometryTypeKHR                    METoVKGeometryTypeKHR(ERayTracingGeometryType _type);
        static VkGeometryFlagsKHR                   METoGeometryFlagsKHR(ERayTracingGeometryFlags _flag);
        static VkBuildAccelerationStructureFlagsKHR METoVKBuildAccelerationStructureFlagsKHR(ERayTracingAccelerationStructureBuildFlags _me_flags);
        static VkGeometryInstanceFlagsKHR           METoVKGeometryInstanceFlagsKHR(ERayTracingInstanceFlags _me_flags);
    };

    class VulkanRHIRayTracingBLAS final : public RHIRayTracingBLAS, public VulkanRHIRayTracingAccelerationStructure {
        friend VulkanRHIImpl;

    public:
        VulkanRHIRayTracingBLAS(const RHIRayTracingBLASInitializer& _init) : RHIRayTracingBLAS(_init) {
        }

    protected:
        VkAccelerationStructureKHR m_blas;
        RHIBufferRef               m_buffer;
    };
    class VulkanRHIRayTracingTLAS final : public RHIRayTracingTLAS, public VulkanRHIRayTracingAccelerationStructure {
        friend VulkanRHIImpl;
        friend VulkanPipelineResourceCache;

    public:
        VulkanRHIRayTracingTLAS(const RHIRayTracingTLASInitializer& _init) : RHIRayTracingTLAS(_init) {
        }

    protected:
        VkAccelerationStructureKHR m_tlas;
        RHIBufferRef               m_buffer;
    };
}// namespace Moer::Render
#pragma endregion

#pragma region graphic pipeline definitions
#pragma endregion

#pragma region raytracing
#pragma endregion

#pragma region render query
#pragma endregion

#pragma region RDG resource creater
#pragma endregion

#endif//VULKAN_RHI_RESOURCE_H
