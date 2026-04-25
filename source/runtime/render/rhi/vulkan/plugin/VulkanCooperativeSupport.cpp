#include "VulkanCooperativeSupport.h"

#include "log/LogSystem.h"

#include <string>

namespace Moer::Render {

namespace {

CooperativeMatrixModeInfo BuildCooperativeMatrixModeInfo(const VkCooperativeMatrixPropertiesKHR& _mode) {
    CooperativeMatrixModeInfo info{};
    info.m_size                  = _mode.MSize;
    info.n_size                  = _mode.NSize;
    info.k_size                  = _mode.KSize;
    info.a_type                  = static_cast<uint32_t>(_mode.AType);
    info.b_type                  = static_cast<uint32_t>(_mode.BType);
    info.c_type                  = static_cast<uint32_t>(_mode.CType);
    info.result_type             = static_cast<uint32_t>(_mode.ResultType);
    info.saturating_accumulation = (_mode.saturatingAccumulation == VK_TRUE);
    info.scope                   = static_cast<uint32_t>(_mode.scope);
    return info;
}

CooperativeVectorModeInfo BuildCooperativeVectorModeInfo(const VkCooperativeVectorPropertiesNV& _mode) {
    CooperativeVectorModeInfo info{};
    info.input_type            = static_cast<uint32_t>(_mode.inputType);
    info.input_interpretation  = static_cast<uint32_t>(_mode.inputInterpretation);
    info.matrix_interpretation = static_cast<uint32_t>(_mode.matrixInterpretation);
    info.bias_interpretation   = static_cast<uint32_t>(_mode.biasInterpretation);
    info.result_type           = static_cast<uint32_t>(_mode.resultType);
    info.transpose             = (_mode.transpose == VK_TRUE);
    return info;
}

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
            MOER_TEXT("VulkanRHI: Cooperative extensions are not supported: {}. Cooperative-related passes in raster ")
            "renderer will be disabled.",
            unsupported_extensions
        );
        return;
    }

    LOG_INFO(
        MOER_TEXT("VulkanRHI: Cooperative support - matrix={} robust={} matrix_modes={} vector={} training={} ")
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

CooperativeExtensionInfo BuildCooperativeExtensionInfo(
    const VulkanOptionalDeviceExtensions& _optional_extensions,
    const VulkanOptionalDeviceProperties& _optional_properties
) {
    CooperativeExtensionInfo info{};

    info.extension_enabled = _optional_extensions.IsExtensionCooperativeEnabled();
    info.matrix_supported  = _optional_extensions.SupportsCooperativeMatrix();
    info.matrix_robust_buffer_access_supported =
        _optional_extensions.SupportsCooperativeMatrixRobustBufferAccess();
    info.vector_supported              = _optional_extensions.SupportsCooperativeVector();
    info.vector_training_supported     = _optional_extensions.SupportsCooperativeVectorTraining();
    info.inference_ready               = _optional_extensions.HasCooperativeInferenceEnabled();
    info.low_precision_supported       = _optional_extensions.HasCooperativeLowPrecisionSupport();
    info.storage_supported             = _optional_extensions.HasCooperativeStorageSupport();
    info.vulkan_memory_model_supported = _optional_extensions.m_has_vulkan_memory_model;
    info.shader_float16_supported      = _optional_extensions.m_has_shader_float16;
    info.shader_int8_supported         = _optional_extensions.m_has_shader_int8;

    info.matrix_supported_stages =
        _optional_properties.cooperative_matrix_properties.cooperativeMatrixSupportedStages;
    info.vector_supported_stages =
        _optional_properties.cooperative_vector_properties.cooperativeVectorSupportedStages;
    info.vector_training_float16_accumulation =
        (_optional_properties.cooperative_vector_properties.cooperativeVectorTrainingFloat16Accumulation ==
         VK_TRUE);
    info.vector_training_float32_accumulation =
        (_optional_properties.cooperative_vector_properties.cooperativeVectorTrainingFloat32Accumulation ==
         VK_TRUE);
    info.max_vector_components =
        _optional_properties.cooperative_vector_properties.maxCooperativeVectorComponents;

    info.matrix_modes.reserve(_optional_properties.cooperative_matrix_supports.size());
    for (const auto& mode : _optional_properties.cooperative_matrix_supports) {
        info.matrix_modes.push_back(BuildCooperativeMatrixModeInfo(mode));
    }

    info.vector_modes.reserve(_optional_properties.cooperative_vector_supports.size());
    for (const auto& mode : _optional_properties.cooperative_vector_supports) {
        info.vector_modes.push_back(BuildCooperativeVectorModeInfo(mode));
    }

    return info;
}

} // namespace Moer::Render