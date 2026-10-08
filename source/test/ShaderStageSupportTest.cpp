#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/ShaderStageUtils.h"
#include "shader/DXC/DXCUtils.h"

#include <algorithm>
#include <cassert>
#include <iostream>

namespace {

Moer::Array<Moer::Render::SingleShaderInfo> MakeShaders(std::initializer_list<EShaderType> shader_types) {
    Moer::Array<Moer::Render::SingleShaderInfo> shaders;
    for (const auto shader_type : shader_types) {
        shaders.push_back({.shader_type = shader_type});
    }
    return shaders;
}

void CheckShaderStageComposition() {
    using namespace Moer::Render;
    const std::initializer_list<EShaderType> k_valid_graphics[] = {
        {ST_VERTEX, ST_FRAGMENT},
        {ST_VERTEX, ST_GEOMETRY, ST_FRAGMENT},
        {ST_VERTEX, ST_HULL, ST_DOMAIN, ST_FRAGMENT},
        {ST_MESH, ST_FRAGMENT},
        {ST_AMPLIFICATION, ST_MESH, ST_FRAGMENT}
    };
    const auto compare_type = [](const SingleShaderInfo& lhs, const SingleShaderInfo& rhs) {
        return lhs.shader_type < rhs.shader_type;
    };
    for (const auto shader_types : k_valid_graphics) {
        auto shaders = MakeShaders(shader_types);
        std::sort(shaders.begin(), shaders.end(), compare_type);
        do {
            assert(ValidateGraphicsShaderStages(shaders).empty());
            const auto ordered = GetGraphicsShadersInStageOrder(shaders);
            assert(ordered.size() == shader_types.size());
            size_t shader_id = 0;
            for (const auto shader_type : shader_types) {
                assert(ordered[shader_id]->shader_type == shader_type);
                assert(ordered[shader_id] == FindShaderStage(shaders, shader_type));
                ++shader_id;
            }
        } while (std::next_permutation(shaders.begin(), shaders.end(), compare_type));
    }

    const std::initializer_list<EShaderType> k_invalid_graphics[] = {
        {},
        {ST_FRAGMENT},
        {ST_VERTEX},
        {ST_VERTEX, ST_VERTEX, ST_FRAGMENT},
        {ST_VERTEX, ST_COMPUTE, ST_FRAGMENT},
        {ST_VERTEX, ST_RAY_GEN, ST_FRAGMENT},
        {ST_VERTEX, ST_NONE, ST_FRAGMENT},
        {ST_VERTEX, static_cast<EShaderType>(255), ST_FRAGMENT},
        {ST_VERTEX, ST_MESH, ST_FRAGMENT},
        {ST_VERTEX, ST_AMPLIFICATION, ST_FRAGMENT},
        {ST_AMPLIFICATION, ST_FRAGMENT},
        {ST_MESH, ST_HULL, ST_DOMAIN, ST_FRAGMENT},
        {ST_MESH, ST_GEOMETRY, ST_FRAGMENT},
        {ST_VERTEX, ST_HULL, ST_FRAGMENT},
        {ST_VERTEX, ST_DOMAIN, ST_FRAGMENT},
        {ST_VERTEX, ST_HULL, ST_DOMAIN, ST_GEOMETRY, ST_FRAGMENT}
    };
    for (const auto shader_types : k_invalid_graphics) {
        assert(!ValidateGraphicsShaderStages(MakeShaders(shader_types)).empty());
    }
    assert(ValidateComputeShaderStages(MakeShaders({ST_COMPUTE})).empty());
    const std::initializer_list<EShaderType> k_invalid_compute[] = {
        {}, {ST_VERTEX}, {ST_RAY_GEN}, {ST_COMPUTE, ST_COMPUTE}, {ST_COMPUTE, ST_FRAGMENT}
    };
    for (const auto shader_types : k_invalid_compute) {
        assert(!ValidateComputeShaderStages(MakeShaders(shader_types)).empty());
    }

    const auto shaders = MakeShaders({ST_VERTEX, ST_FRAGMENT});
    assert(FindShaderStage(shaders, ST_HULL) == nullptr);
    // The generic storage also permits repeated RT stages; only graphics/compute
    // validators impose uniqueness for their own creation entry points.
    PipelineShaderInfo ray_shaders{.shaders = MakeShaders({ST_RAY_GEN, ST_RAY_GEN})};
    assert(ray_shaders.shaders.size() == 2);
}

} // namespace

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

    CheckShaderStageComposition();

    Moer::Render::GfxPsoCreateInfo pipeline_info(
        ::RHIRasterizeInfo::Preset(),
        {},
        {}
    );
    pipeline_info.primitive_topology = EPrimitiveTopology::PATCH_LIST;
    pipeline_info.SetPatchControlPoints(3);
    assert(pipeline_info.patch_control_points == 3);

    std::cout << "TestShaderStageSupport: all checks passed\n";
    return 0;
}
