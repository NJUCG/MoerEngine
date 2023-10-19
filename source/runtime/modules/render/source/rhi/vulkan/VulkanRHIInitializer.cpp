#include "VulkanRHIInitializer.h"
#include "VulkanDevice.h"
#include "log/LogSystem.h"
//
//VkFilter VulkanRHISamplerInitializer::TranslateMinMagFilterMode(ESamplerFilter _filter) {
//    switch (_filter) {
//        case SF_NEAREST:
//            return VK_FILTER_NEAREST;
//        case SF_LINEAR:
//        case SF_CUBIC:
//            return VK_FILTER_LINEAR;
//        case SF_ANISOTROPIC_NEAREST:
//        case SF_ANISOTROPIC_LINEAR:
//            return VK_FILTER_LINEAR;
//        default:
//            LOG_CRITICAL("Unknown ESamplerFilter {:d}", static_cast<uint8_t>(_filter));
//            return VK_FILTER_MAX_ENUM;
//    }
//}
//
//VkSamplerMipmapMode VulkanRHISamplerInitializer::TranslateMipmapMode(ESamplerFilter _filter) {
//    switch (_filter) {
//        case SF_NEAREST:
//        case SF_LINEAR:
//            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
//        case SF_CUBIC:
//            return VK_SAMPLER_MIPMAP_MODE_LINEAR;
//        case SF_ANISOTROPIC_NEAREST:
//            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
//        case SF_ANISOTROPIC_LINEAR:
//            return VK_SAMPLER_MIPMAP_MODE_LINEAR;
//        default:
//            LOG_CRITICAL("Unknown Mipmap ESamplerFilter {:d}", static_cast<uint8_t>(_filter));
//            return VK_SAMPLER_MIPMAP_MODE_MAX_ENUM;
//    }
//}
//
//VkSamplerAddressMode VulkanRHISamplerInitializer::TranslateWrapMode(ESamplerAddressMode _address_mode) {
//    switch (_address_mode) {
//        case SAM_REPEAT:
//            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
//        case SAM_MIRRORED_REPEAT:
//            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
//        case SAM_CLAMP_TO_EDGE:
//            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
//        case SAM_CLAMP_TO_BORDER:
//            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
//        default:
//            LOG_CRITICAL("Unknown ESamplerAddressMode {:d}", static_cast<uint8_t>(_address_mode));
//            return VK_SAMPLER_ADDRESS_MODE_MAX_ENUM;
//    }
//}
//
//VkCompareOp VulkanRHISamplerInitializer::TranslateCompareOp(ESamplerCompareFunction _compare_op) {
//    switch (_compare_op) {
//        case SCF_NEVER:
//            return VK_COMPARE_OP_NEVER;
//        case SCF_LESS:
//            return VK_COMPARE_OP_LESS;
//        case SCF_EQUAL:
//            return VK_COMPARE_OP_EQUAL;
//        case SCF_LESS_OR_EQUAL:
//            return VK_COMPARE_OP_LESS_OR_EQUAL;
//        case SCF_GREATER:
//            return VK_COMPARE_OP_GREATER;
//        case SCF_NOT_EQUAL:
//            return VK_COMPARE_OP_NOT_EQUAL;
//        case SCF_GREATER_OR_EQUAL:
//            return VK_COMPARE_OP_GREATER_OR_EQUAL;
//        case SCF_ALWAYS:
//            return VK_COMPARE_OP_ALWAYS;
//        default:
//            LOG_CRITICAL("Unknown ESamplerCompareFunction {:d}", static_cast<uint8_t>(_compare_op));
//            return VK_COMPARE_OP_MAX_ENUM;
//    }
//}
//
//VkSamplerCreateInfo VulkanRHISamplerInitializer::FromRHISamplerInitializer(const VulkanDevice* _device, const RHISamplerInitializer& _initializer) {
//    VkSamplerCreateInfo sampler_create_info{};
//
//    sampler_create_info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
//    sampler_create_info.flags        = 0;
//    sampler_create_info.magFilter    = TranslateMinMagFilterMode(_initializer.filter);
//    sampler_create_info.minFilter    = TranslateMinMagFilterMode(_initializer.filter);
//    sampler_create_info.mipmapMode   = TranslateMipmapMode(_initializer.filter);
//    sampler_create_info.addressModeU = TranslateWrapMode(_initializer.address_mode_u);
//    sampler_create_info.addressModeV = TranslateWrapMode(_initializer.address_mode_v);
//    sampler_create_info.addressModeW = TranslateWrapMode(_initializer.address_mode_w);
//    sampler_create_info.mipLodBias   = _initializer.mip_lod_bias;
//
//    sampler_create_info.maxAnisotropy = 1.0f;
//    if (_initializer.filter == SF_ANISOTROPIC_NEAREST || _initializer.filter == SF_ANISOTROPIC_LINEAR) {
//        sampler_create_info.maxAnisotropy = std::clamp(static_cast<float>(_initializer.max_anisotropy), 1.0f, _device->GetProperties().limits.maxSamplerAnisotropy);
//    }
//    sampler_create_info.anisotropyEnable = sampler_create_info.maxAnisotropy > 1.0f ? VK_TRUE : VK_FALSE;
//
//    sampler_create_info.compareEnable = _initializer.compare_op != SCF_NEVER ? VK_TRUE : VK_FALSE;
//    sampler_create_info.compareOp     = TranslateCompareOp(_initializer.compare_op);
//    sampler_create_info.minLod        = _initializer.min_mip_level;
//    sampler_create_info.maxLod        = _initializer.max_mip_level;
//    sampler_create_info.borderColor   = _initializer.border_color == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
//
//    return sampler_create_info;
//}
