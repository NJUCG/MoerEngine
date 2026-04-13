#pragma once

#include "RasterConfig.h"
#include "RasterResource.h"
#include "rhi/RHI.h"

#include <algorithm>
#include <string>

namespace Moer::Render::Raster {

/**
 * Cooperative Ops scaffold pass.
 *
 * This pass reserves a stable insertion point for later cooperative experiments.
 * The current implementation is intentionally a no-op and preserves the existing
 * post-process chain output regardless of whether the UI toggle is enabled.
 */
class CooperativeOpsPass {
public:
    explicit CooperativeOpsPass(RasterContext& context) :
        m_cooperative_info(context.device.GetCooperativeExtensionInfo()) {}

    TextureWithHandle
    Process(RasterContext& context, RasterConfig& ui_config, TextureWithHandle input_image) {
        UpdateSnapshotStatus(ui_config.cooperative_ops_status);

        auto& status = ui_config.cooperative_ops_status;
        if (!ui_config.cooperative_ops_enabled) {
            status.matrix_runtime_status = "Inactive";
            status.vector_runtime_status = "Inactive";
            return input_image;
        }

        ++status.frames_evaluated;

        RunMatrixReadinessProbe(status);
        RunVectorConversionProbe(context, status);

        return input_image;
    }

private:
    static constexpr uint32_t s_compute_stage_bit   = 0x00000020;
    static constexpr uint32_t s_row_major_layout    = 0;
    static constexpr uint32_t s_column_major_layout = 1;
    static constexpr uint32_t s_float_e4m3_type     = 1000491002;
    static constexpr uint32_t s_float_e5m2_type     = 1000491003;

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

    static uint32_t ComponentTypeByteSize(uint32_t _component_type) {
        switch (_component_type) {
            case 0:
            case 4:
            case 8:
                return 2;
            case 1:
            case 5:
            case 9:
                return 4;
            case 2:
            case 6:
            case 10:
                return 8;
            case 3:
            case 7:
            case s_float_e4m3_type:
            case s_float_e5m2_type:
                return 1;
            default:
                return 0;
        }
    }

    static bool IsRowColumnRoundtripCapable(const CooperativeVectorModeInfo& _mode) {
        return _mode.matrix_interpretation != s_float_e4m3_type &&
               _mode.matrix_interpretation != s_float_e5m2_type;
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
            if (IsRowColumnRoundtripCapable(mode)) {
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
                           " memory_model=" + ToYesNo(m_cooperative_info.vulkan_memory_model_supported);

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

    void RunMatrixReadinessProbe(CooperativeOpsStatus& _status) const {
        if (!m_cooperative_info.matrix_supported) {
            _status.matrix_runtime_status = "Unavailable";
            return;
        }
        if ((m_cooperative_info.matrix_supported_stages & s_compute_stage_bit) == 0) {
            _status.matrix_runtime_status =
                "Driver did not advertise compute-stage cooperative matrix support.";
            return;
        }

        if (const auto* matrix_mode = FindPreferredMatrixMode()) {
            _status.matrix_runtime_status = "Compute-capable mode ready: " + FormatMatrixMode(*matrix_mode) +
                                            " (live shader probe pending).";
        } else {
            _status.matrix_runtime_status = "No preferred matrix mode selected.";
        }
    }

    void RunVectorConversionProbe(RasterContext& _context, CooperativeOpsStatus& _status) const {
        if (!m_cooperative_info.vector_supported) {
            _status.vector_runtime_status = "Unavailable";
            return;
        }

        const CooperativeVectorModeInfo* vector_mode = FindPreferredVectorMode();
        if (!vector_mode) {
            _status.vector_runtime_status = "No preferred vector mode selected.";
            return;
        }
        if (!IsRowColumnRoundtripCapable(*vector_mode)) {
            _status.vector_runtime_status =
                "Preferred vector mode requires opaque optimal layouts; row/column roundtrip probe skipped.";
            return;
        }

        const uint32_t element_size = ComponentTypeByteSize(vector_mode->matrix_interpretation);
        if (element_size == 0) {
            _status.vector_runtime_status = "Unsupported matrix component size for vector probe.";
            return;
        }

        const uint32_t cols       = std::max(1u, std::min(4u, m_cooperative_info.max_vector_components));
        const uint32_t rows       = 4;
        const size_t   row_stride = static_cast<size_t>(cols) * element_size;
        const size_t   col_stride = static_cast<size_t>(rows) * element_size;
        const size_t   byte_size  = static_cast<size_t>(rows) * cols * element_size;

        Array<byte> row_major(byte_size);
        Array<byte> column_major(byte_size);
        Array<byte> roundtrip(byte_size);

        for (size_t element_idx = 0; element_idx < static_cast<size_t>(rows) * cols; ++element_idx) {
            uint64_t value = 0x10ull + element_idx;
            for (uint32_t byte_idx = 0; byte_idx < element_size; ++byte_idx) {
                row_major[element_idx * element_size + byte_idx] =
                    static_cast<byte>((value >> (byte_idx * 8)) & 0xffu);
            }
        }

        CooperativeVectorConversionDesc to_column_major{};
        to_column_major.src_component_type = vector_mode->matrix_interpretation;
        to_column_major.dst_component_type = vector_mode->matrix_interpretation;
        to_column_major.num_rows           = rows;
        to_column_major.num_columns        = cols;
        to_column_major.src_layout         = s_row_major_layout;
        to_column_major.src_stride         = row_stride;
        to_column_major.dst_layout         = s_column_major_layout;
        to_column_major.dst_stride         = col_stride;

        CooperativeVectorConversionDesc to_row_major = to_column_major;
        to_row_major.src_layout                      = s_column_major_layout;
        to_row_major.src_stride                      = col_stride;
        to_row_major.dst_layout                      = s_row_major_layout;
        to_row_major.dst_stride                      = row_stride;

        if (!_context.device.TryConvertCooperativeVectorMatrix(to_column_major, row_major, column_major)) {
            _status.vector_runtime_status = "Host conversion failed.";
            return;
        }

        if (!_context.device.TryConvertCooperativeVectorMatrix(to_row_major, column_major, roundtrip)) {
            _status.vector_runtime_status = "Roundtrip conversion failed.";
            return;
        }

        if (roundtrip == row_major) {
            _status.vector_runtime_status =
                "Row/column roundtrip OK: " + std::to_string(rows) + "x" + std::to_string(cols) +
                " matrix, element_size=" + std::to_string(element_size) + " bytes.";
            return;
        }

        _status.vector_runtime_status =
            "Roundtrip conversion completed, but the resulting bytes did not match the input.";
    }

private:
    CooperativeExtensionInfo m_cooperative_info{};
};

} // namespace Moer::Render::Raster