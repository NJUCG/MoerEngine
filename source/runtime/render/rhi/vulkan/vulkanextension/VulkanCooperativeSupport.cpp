#include "VulkanCooperativeSupport.h"

#include "log/LogSystem.h"

#include <string>

namespace Moer::Render {

namespace {

std::string
BuildUnsupportedCooperativeExtensionList(const VulkanOptionalDeviceExtensions& _optional_extensions) {
    std::string unsupported_extensions;

    const auto append_extension = [&](std::string_view _extension_name) {
        if (!unsupported_extensions.empty()) {
            unsupported_extensions += ", ";
        }
        unsupported_extensions += _extension_name;
    };

    if (!_optional_extensions.SupportsCooperativeMatrix()) {
        append_extension(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    }
    if (!_optional_extensions.SupportsCooperativeVector()) {
        append_extension(VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME);
    }

    return unsupported_extensions;
}

} // namespace

// 把 cooperative 依赖的 core feature 映射到引擎自己的 capability 状态里。
void UpdateCooperativePrerequisites(
    const VulkanDeviceFeatures&     _core_features,
    VulkanOptionalDeviceExtensions& _optional_extensions
) {
    const auto& core_1_1 = _core_features.GetCore11Features();
    const auto& core_1_2 = _core_features.GetCore12Features();

    _optional_extensions.m_has_shader_float16              = (core_1_2.shaderFloat16 == VK_TRUE);
    _optional_extensions.m_has_shader_int8                 = (core_1_2.shaderInt8 == VK_TRUE);
    _optional_extensions.m_has_vulkan_memory_model         = (core_1_2.vulkanMemoryModel == VK_TRUE);
    _optional_extensions.m_has_storage_buffer_16bit_access = (core_1_1.storageBuffer16BitAccess == VK_TRUE);
    _optional_extensions.m_has_uniform_and_storage_buffer_16bit_access =
        (core_1_1.uniformAndStorageBuffer16BitAccess == VK_TRUE);
    _optional_extensions.m_has_storage_buffer_8bit_access = (core_1_2.storageBuffer8BitAccess == VK_TRUE);
    _optional_extensions.m_has_uniform_and_storage_buffer_8bit_access =
        (core_1_2.uniformAndStorageBuffer8BitAccess == VK_TRUE);
}

// 把 cooperative 当前的支持情况集中打印出来，便于后续做 shader probe。
void LogCooperativeSupportSummary(
    const VulkanOptionalDeviceExtensions& _optional_extensions,
    const VulkanOptionalDeviceProperties& _optional_properties
) {
    const std::string unsupported_extensions = BuildUnsupportedCooperativeExtensionList(_optional_extensions);
    if (!unsupported_extensions.empty()) {
        LOG_INFO(
            "VulkanRHI: Cooperative extensions are not supported: {}. Cooperative-related passes in raster "
            "renderer will be disabled.",
            unsupported_extensions
        );
        return;
    }

    LOG_INFO(
        "VulkanRHI: Cooperative support - matrix={} robust={} matrix_modes={} vector={} training={} "
        "vector_modes={} inference_ready={} float16={} int8={} vulkan_memory_model={}",
        _optional_extensions.SupportsCooperativeMatrix(),
        _optional_extensions.SupportsCooperativeMatrixRobustBufferAccess(),
        _optional_properties.cooperative_matrix_supports.size(),
        _optional_extensions.SupportsCooperativeVector(),
        _optional_extensions.SupportsCooperativeVectorTraining(),
        _optional_properties.cooperative_vector_supports.size(),
        _optional_extensions.HasCooperativeInferenceEnabled(),
        _optional_extensions.m_has_shader_float16,
        _optional_extensions.m_has_shader_int8,
        _optional_extensions.m_has_vulkan_memory_model
    );
}

} // namespace Moer::Render