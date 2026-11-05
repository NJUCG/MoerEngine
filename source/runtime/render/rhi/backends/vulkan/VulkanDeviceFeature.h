#pragma once

#include "VulkanCommon.h"

namespace Moer::Render {

// Value snapshot. Native query/create chains are local to their Vulkan calls.
class VulkanDeviceFeatures {
public:
    static VulkanDeviceFeatures QuerySupportedFeatures(VkPhysicalDevice gpu);
    bool SupportsRequiredFeatures() const;

    const VkPhysicalDeviceFeatures& GetCore10Features() const { return core_1_0; }
    const VkPhysicalDeviceVulkan11Features& GetCore11Features() const { return core_1_1; }
    const VkPhysicalDeviceVulkan12Features& GetCore12Features() const { return core_1_2; }
    const VkPhysicalDeviceVulkan13Features& GetCore13Features() const { return core_1_3; }

private:
    VkPhysicalDeviceFeatures         core_1_0{};
    VkPhysicalDeviceVulkan11Features core_1_1{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features core_1_2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features core_1_3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
};

// Enabled optional capabilities, with no native chain or separate lifetime.
class VulkanDeviceCapabilities final {
public:
    inline bool HasRaytracingExtensions() const {
        return m_has_khr_acceleration_structure && (m_has_khr_ray_tracing_pipeline || m_has_khr_ray_query);
    }

    // cooperative 扩展与 feature bit 的原始支持状态。
    inline bool SupportsCooperativeMatrix() const {
        return m_has_khr_cooperative_matrix;
    }

    inline bool SupportsCooperativeMatrixRobustBufferAccess() const {
        return m_has_khr_cooperative_matrix_robust_buffer_access;
    }

    inline bool SupportsCooperativeVector() const {
        return m_has_nv_cooperative_vector;
    }

    inline bool SupportsCooperativeVectorTraining() const {
        return m_has_nv_cooperative_vector_training;
    }

    inline bool HasCooperativeMatrixEnabled() const {
        return SupportsCooperativeMatrix();
    }

    inline bool HasCooperativeVectorEnabled() const {
        return SupportsCooperativeVector();
    }

    inline bool HasCooperativeVectorTrainingEnabled() const {
        return SupportsCooperativeVectorTraining();
    }

    // cooperative bundle 依赖的 core feature 前置条件。
    inline bool HasCooperativeLowPrecisionSupport() const {
        return m_has_shader_float16 || m_has_shader_int8;
    }

    inline bool HasCooperativeStorageSupport() const {
        return m_has_storage_buffer_16bit_access || m_has_uniform_and_storage_buffer_16bit_access ||
               m_has_storage_buffer_8bit_access || m_has_uniform_and_storage_buffer_8bit_access;
    }

    inline bool HasCooperativeInferenceSupport() const {
        return SupportsCooperativeMatrix() && SupportsCooperativeVector() && m_has_vulkan_memory_model &&
               HasCooperativeLowPrecisionSupport() && HasCooperativeStorageSupport();
    }

    inline bool HasCooperativeInferenceEnabled() const {
        return HasCooperativeMatrixEnabled() && HasCooperativeVectorEnabled() && m_has_vulkan_memory_model &&
               HasCooperativeLowPrecisionSupport() && HasCooperativeStorageSupport();
    }

    inline bool IsExtensionCooperativeEnabled() const {
        return HasCooperativeInferenceEnabled();
    }

    // optional extensions
    bool m_has_ext_descriptor_heap                         = false;
    bool m_has_khr_maintenance5                            = false;
    bool m_has_descriptor_buffer_push                      = false;
    bool m_has_ext_descriptor_buffer                       = false;
    bool m_has_khr_acceleration_structure                  = false;
    bool m_has_khr_ray_tracing_pipeline                    = false;
    bool m_has_khr_ray_query                               = false;
    bool m_has_ext_mesh_shader                             = false;
    bool m_allow_mesh_primitive_shading                    = false;
    bool m_has_khr_cooperative_matrix                      = false;
    bool m_has_khr_cooperative_matrix_robust_buffer_access = false;
    bool m_has_nv_cooperative_vector                       = false;
    bool m_has_nv_cooperative_vector_training              = false;

    bool m_has_memory_priority              = false;
    bool m_has_pageable_device_local_memory = false;

    // cooperative bundle 依赖的 core feature 前置条件。
    bool m_has_shader_float16                          = false;
    bool m_has_shader_int8                             = false;
    bool m_has_vulkan_memory_model                     = false;
    bool m_has_storage_buffer_16bit_access             = false;
    bool m_has_uniform_and_storage_buffer_16bit_access = false;
    bool m_has_storage_buffer_8bit_access              = false;
    bool m_has_uniform_and_storage_buffer_8bit_access  = false;
};

} // namespace Moer::Render
