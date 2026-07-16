#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/DXC/DXCUtils.h"

#include <cassert>
#include <iostream>
#include <variant>

int main() {
    static_assert(ST_HULL > ST_RAY_ANYHIT, "New stages must not renumber serialized shader types.");
    static_assert(ST_DOMAIN == ST_HULL + 1);
    static_assert(ST_Num <= (1 << ST_NumBits));

    assert(GetPlatform(ST_HULL, SP_VULKAN_SM6) == L"hs_6_7");
    assert(GetPlatform(ST_DOMAIN, SP_VULKAN_SM6) == L"ds_6_7");
    assert(
        ToPipelineStageFlag(spv::ExecutionModelTessellationControl) ==
        ERHIPipelineStageFlags::PS_TESSELLATION_CONTROL_SHADER
    );
    assert(
        ToPipelineStageFlag(spv::ExecutionModelTessellationEvaluation) ==
        ERHIPipelineStageFlags::PS_TESSELLATION_EVALUATION_SHADER
    );

    Moer::Render::ShaderOutputGroup shader_group = Moer::Render::ShaderVsHsDsPs{};
    assert(std::holds_alternative<Moer::Render::ShaderVsHsDsPs>(shader_group));

    Moer::Render::GfxPsoCreateInfo pipeline_info(
        RHIRasterizeInfo::Preset(),
        {},
        {}
    );
    pipeline_info.primitive_topology = EPrimitiveTopology::PATCH_LIST;
    pipeline_info.SetPatchControlPoints(3);
    assert(pipeline_info.patch_control_points == 3);

    std::cout << "TestShaderStageSupport: all checks passed\n";
    return 0;
}
