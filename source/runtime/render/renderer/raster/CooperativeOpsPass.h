#pragma once

#include "RasterConfig.h"
#include "RasterResource.h"

#include <algorithm>
#include <string>

namespace Moer::Render::Raster {

/**
 * Cooperative Ops Pass。
 *
 * Moer 目前的 Shader 编译路径是 HLSL -> DXC -> SPIR-V -> Vulkan -> 硬件。
 * 当前 DXC 在编译到 SPIR-V 的路径上不支持 cooperative vector，因此暂时无法在
 * Moer 中进行 cooperative vector 试验。这个类现阶段仅保留占位，暂时废弃。
 */
class CooperativeOpsPass {
public:
    explicit CooperativeOpsPass(RasterContext& context) :
        m_cooperative_info(context.device.GetCooperativeExtensionInfo()) {}

    TextureWithHandle
    Process(RasterContext& context, RasterConfig& ui_config, TextureWithHandle input_image) {
        (void)context;
        UpdateSnapshotStatus(ui_config.cooperative_ops_status);

        auto& status = ui_config.cooperative_ops_status;
        if (!ui_config.cooperative_ops_enabled) {
            status.matrix_runtime_status = "Inactive";
            status.vector_runtime_status = "Inactive";
            return input_image;
        }

        ++status.frames_evaluated;

        UpdateRuntimeStatus(status);

        return input_image;
    }

private:
    static constexpr uint32_t s_compute_stage_bit = 0x00000020;
    static constexpr uint32_t s_float_e4m3_type   = 1000491002;
    static constexpr uint32_t s_float_e5m2_type   = 1000491003;

    static const char* ToYesNo(bool _enabled) {
        return _enabled ? "Yes" : "No";
    }

    static std::string ComponentTypeName(uint32_t _component_type) {
        switch (_component_type) {
            case 0:
                return "float16";
            case 1:
                return "float32";
            case 2:
                return "float64";
            case 3:
                return "sint8";
            case 4:
                return "sint16";
            case 5:
                return "sint32";
            case 6:
                return "sint64";
            case 7:
                return "uint8";
            case 8:
                return "uint16";
            case 9:
                return "uint32";
            case 10:
                return "uint64";
            case s_float_e4m3_type:
                return "float8_e4m3";
            case s_float_e5m2_type:
                return "float8_e5m2";
            default:
                return "type(" + std::to_string(_component_type) + ")";
        }
    }

    static std::string ScopeName(uint32_t _scope) {
        switch (_scope) {
            case 1:
                return "Device";
            case 2:
                return "Workgroup";
            case 3:
                return "Subgroup";
            case 5:
                return "QueueFamily";
            default:
                return "Scope(" + std::to_string(_scope) + ")";
        }
    }

    static std::string FormatMatrixMode(const CooperativeMatrixModeInfo& _mode) {
        return std::to_string(_mode.m_size) + "x" + std::to_string(_mode.n_size) + "x" +
               std::to_string(_mode.k_size) + " scope=" + ScopeName(_mode.scope) +
               " A=" + ComponentTypeName(_mode.a_type) + " B=" + ComponentTypeName(_mode.b_type) +
               " C=" + ComponentTypeName(_mode.c_type) + " R=" + ComponentTypeName(_mode.result_type) +
               " sat=" + ToYesNo(_mode.saturating_accumulation);
    }

    static std::string FormatVectorMode(const CooperativeVectorModeInfo& _mode) {
        return "input=" + ComponentTypeName(_mode.input_type) +
               " input_interp=" + ComponentTypeName(_mode.input_interpretation) +
               " matrix_interp=" + ComponentTypeName(_mode.matrix_interpretation) +
               " bias_interp=" + ComponentTypeName(_mode.bias_interpretation) +
               " result=" + ComponentTypeName(_mode.result_type) + " transpose=" + ToYesNo(_mode.transpose);
    }

    const CooperativeMatrixModeInfo* FindPreferredMatrixMode() const {
        if (m_cooperative_info.matrix_modes.empty()) {
            return nullptr;
        }

        for (const auto& mode : m_cooperative_info.matrix_modes) {
            if (mode.scope == 3) {
                return &mode;
            }
        }

        return &m_cooperative_info.matrix_modes.front();
    }

    const CooperativeVectorModeInfo* FindPreferredVectorMode() const {
        if (m_cooperative_info.vector_modes.empty()) {
            return nullptr;
        }

        for (const auto& mode : m_cooperative_info.vector_modes) {
            if (mode.matrix_interpretation != s_float_e4m3_type &&
                mode.matrix_interpretation != s_float_e5m2_type) {
                return &mode;
            }
        }

        return &m_cooperative_info.vector_modes.front();
    }

    void UpdateSnapshotStatus(CooperativeOpsStatus& _status) const {
        _status.extension_enabled = m_cooperative_info.extension_enabled;
        _status.inference_ready   = m_cooperative_info.inference_ready;
        _status.matrix_supported  = m_cooperative_info.matrix_supported;
        _status.matrix_robust_buffer_access_supported =
            m_cooperative_info.matrix_robust_buffer_access_supported;
        _status.vector_supported              = m_cooperative_info.vector_supported;
        _status.vector_training_supported     = m_cooperative_info.vector_training_supported;
        _status.low_precision_supported       = m_cooperative_info.low_precision_supported;
        _status.storage_supported             = m_cooperative_info.storage_supported;
        _status.vulkan_memory_model_supported = m_cooperative_info.vulkan_memory_model_supported;
        _status.matrix_mode_count             = static_cast<uint>(m_cooperative_info.matrix_modes.size());
        _status.vector_mode_count             = static_cast<uint>(m_cooperative_info.vector_modes.size());
        _status.matrix_supported_stages       = m_cooperative_info.matrix_supported_stages;
        _status.vector_supported_stages       = m_cooperative_info.vector_supported_stages;
        _status.max_vector_components         = m_cooperative_info.max_vector_components;

        _status.overview = std::string("Enabled=") + ToYesNo(m_cooperative_info.extension_enabled) +
                           " inference_ready=" + ToYesNo(m_cooperative_info.inference_ready) +
                           " low_precision=" + ToYesNo(m_cooperative_info.low_precision_supported) +
                           " storage=" + ToYesNo(m_cooperative_info.storage_supported) +
                           " memory_model=" + ToYesNo(m_cooperative_info.vulkan_memory_model_supported) +
                           " | Shader-side cooperative vector is parked on the current Vulkan path.";

        if (const auto* matrix_mode = FindPreferredMatrixMode()) {
            _status.matrix_summary = "Preferred mode: " + FormatMatrixMode(*matrix_mode);
        } else if (m_cooperative_info.matrix_supported) {
            _status.matrix_summary =
                "KHR cooperative matrix is enabled, but the driver reported zero matrix modes.";
        } else {
            _status.matrix_summary = "KHR cooperative matrix is unavailable on this device.";
        }

        if (const auto* vector_mode = FindPreferredVectorMode()) {
            _status.vector_summary = "Preferred mode: " + FormatVectorMode(*vector_mode);
        } else if (m_cooperative_info.vector_supported) {
            _status.vector_summary =
                "NV cooperative vector is enabled, but the driver reported zero vector modes.";
        } else {
            _status.vector_summary = "NV cooperative vector is unavailable on this device.";
        }
    }

    std::string DescribeMatrixCapability() const {
        if (!m_cooperative_info.matrix_supported) {
            return "Cooperative matrix unavailable.";
        }
        if ((m_cooperative_info.matrix_supported_stages & s_compute_stage_bit) == 0) {
            return "Driver did not advertise compute-stage cooperative matrix support.";
        }

        if (const auto* matrix_mode = FindPreferredMatrixMode()) {
            return "Compute-capable mode ready: " + FormatMatrixMode(*matrix_mode) +
                   " (cooperative shader probe pending).";
        }

        return "No preferred matrix mode selected.";
    }

    void UpdateRuntimeStatus(CooperativeOpsStatus& _status) const {
        _status.matrix_runtime_status =
            DescribeMatrixCapability() + " No active matrix experiment is running in this pass.";

        if (!m_cooperative_info.vector_supported) {
            _status.vector_runtime_status = "NV cooperative vector is unavailable on this device.";
            return;
        }

        const CooperativeVectorModeInfo* vector_mode = FindPreferredVectorMode();
        const std::string                preferred_mode =
            vector_mode ? (" Preferred mode: " + FormatVectorMode(*vector_mode)) : std::string();

        _status.vector_runtime_status =
            "Shader-side cooperative vector is parked on the current Vulkan path: MoerEngine compiles "
            "Vulkan shaders as HLSL -> DXC -> SPIR-V, but dx::linalg cooperative vector authoring is "
            "disabled under __spirv__. The repo currently only has capability enumeration and host-side "
            "layout conversion, not a usable shader execution path." +
            preferred_mode;
    }

private:
    CooperativeExtensionInfo m_cooperative_info{};
};

} // namespace Moer::Render::Raster