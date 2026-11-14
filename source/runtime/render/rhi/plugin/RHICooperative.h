#ifndef MOER_RHI_COOPERATIVE_H
#define MOER_RHI_COOPERATIVE_H

// TODO: Implement this cooperative surface as a true RuntimePlugin instead of a plain shared header.

#include "misc/STL.h"

#include "misc/Types.h"
#include <cstddef>

namespace Moer::Render {

struct CooperativeMatrixModeInfo {
    uint32   m_size                  = 0;
    uint32   n_size                  = 0;
    uint32   k_size                  = 0;
    uint32   a_type                  = 0;
    uint32   b_type                  = 0;
    uint32   c_type                  = 0;
    uint32   result_type             = 0;
    bool     saturating_accumulation = false;
    uint32   scope                   = 0;
};

struct CooperativeVectorModeInfo {
    uint32   input_type            = 0;
    uint32   input_interpretation  = 0;
    uint32   matrix_interpretation = 0;
    uint32   bias_interpretation   = 0;
    uint32   result_type           = 0;
    bool     transpose             = false;
};

struct CooperativeExtensionInfo {
    bool                             extension_enabled                     = false;
    bool                             matrix_supported                      = false;
    bool                             matrix_robust_buffer_access_supported = false;
    bool                             vector_supported                      = false;
    bool                             vector_training_supported             = false;
    bool                             inference_ready                       = false;
    bool                             low_precision_supported               = false;
    bool                             storage_supported                     = false;
    bool                             vulkan_memory_model_supported         = false;
    bool                             shader_float16_supported              = false;
    bool                             shader_int8_supported                 = false;
    uint32                           matrix_supported_stages               = 0;
    uint32                           vector_supported_stages               = 0;
    bool                             vector_training_float16_accumulation  = false;
    bool                             vector_training_float32_accumulation  = false;
    uint32                           max_vector_components                 = 0;
    Array<CooperativeMatrixModeInfo> matrix_modes{};
    Array<CooperativeVectorModeInfo> vector_modes{};
};

struct CooperativeVectorConversionDesc {
    uint32   src_component_type = 0;
    uint32   dst_component_type = 0;
    uint32   num_rows           = 0;
    uint32   num_columns        = 0;
    uint32   src_layout         = 0;
    size_t   src_stride         = 0;
    uint32   dst_layout         = 0;
    size_t   dst_stride         = 0;
};

} // namespace Moer::Render

#endif