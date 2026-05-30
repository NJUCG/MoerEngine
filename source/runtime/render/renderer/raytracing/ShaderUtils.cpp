#include "ShaderUtils.h"
#include "PixelFormat.h"
#include "log/LogSystem.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/utils/Packing.h"
#include <type_traits>

namespace Moer::Render::Raytracing {

inline static uint DivCeil(uint _a, uint _b) {
    return (_a + _b - 1) / _b;
}

namespace {

TextureView MakeMipChainView(std::span<TextureView> mips) {
    assert(!mips.empty());
    Texture* texture = mips.front().GetTexture();
    assert(texture != nullptr);
    return texture->GetView(mips.front().format, mips.front().mip_level, static_cast<uint8>(mips.size()));
}

void FinalizeMipChainState(CommandList& cmd_list, std::span<TextureView> mips, ETextureState final_state) {
    if (mips.empty()) {
        return;
    }

    Array<TrackedTextureState> textures;
    textures.emplace_back(TrackedTextureState{
        .texture      = MakeMipChainView(mips),
        .state        = final_state,
        .owner_queue  = cmd_list.GetQueueType(),
        .access_write = false,
    });
    cmd_list.SetTrackedState(std::move(textures), Array<TrackedBufferState>{});
}

void EmitMipChainBarrier(
    CommandList&              cmd_list,
    std::span<TextureView>    mips,
    uint                      src_mip_level,
    uint                      write_begin,
    uint                      write_count
) {
    Array<BarrierCreateInfo> barriers;
    const BarrierState       uav_state = MakeBarrierState(ETextureState::UNORDERED_ACCESS, EPassType::Compute);

    if (src_mip_level < mips.size()) {
        barriers.emplace_back(BarrierCreateInfo::Transition(mips[src_mip_level], uav_state, uav_state));
    }
    for (uint write_index = 0; write_index < write_count; ++write_index) {
        const uint mip_index = write_begin + write_index;
        barriers.emplace_back(BarrierCreateInfo::Transition(mips[mip_index], uav_state, uav_state));
    }

    if (barriers.empty()) {
        return;
    }

    cmd_list.Barriers(barriers, ETrackedStateUpdateMode::Skip);
}

} // namespace

static constexpr EPixelFormat s_supported_formats[] = {
    PF_R32G32B32A32_SFLOAT,
    PF_R16G16B16A16_SFLOAT,
    PF_R16G16B16A16_UNORM,
    PF_R16G16B16A16_UINT,
    PF_R16G16B16A16_SNORM,
    PF_R8G8B8A8_SNORM,
    PF_R8G8B8A8_UNORM,
    PF_R8G8B8A8_SRGB,
    PF_B8G8R8A8_UNORM,
    PF_B8G8R8A8_SRGB
};

ShaderUtils::ShaderUtils(RenderDevice& _device, ShaderManager& _manager) :
    manager(_manager),
    device(_device) {
    gen_low_discrepancy_pipeline =
        std::move(manager.Compute<GenLowDiscrepancyPipeline>("core/utils/GenLowDiscrepancySequence.hlsl"));
    generate_mip_pdf_pipeline = std::move(manager.Compute<GenerateMipPdfPipeline>(
        "pipelines/raytracing/lighting/precompute/ProcessEnvironmentMap.hlsl"
    ));
    generate_mips_pipeline    = std::move(manager.Compute<GenerateMipsPipeline>("core/utils/BuildMips.hlsl"));

    for (auto format : s_supported_formats) {
        GfxPsoCreateInfo show_texture_pso_info(
            RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(format)}
        );
        show_texture_pipeline_map[format] = std::move(manager.Raster()
                                                          .Vertex("core/utils/FullScreenQuad.hlsl")
                                                          .Pixel("core/utils/ShowTexture.frag.hlsl")
                                                          .Build<ShowTexturePipeline>(std::move(show_texture_pso_info)));

        // GfxPsoCreateInfo sample_tex_pso_info(
        //     RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(format)}
        // );
        // sample_texture_pipeline_map[format] =
        //     std::move(manager.Raster()
        //                   .Vertex("core/utils/FullScreenQuad.hlsl")
        //                   .Pixel("core/utils/CopyTexture.frag.hlsl")
        //                   .Build<UtilsSampleTexturePipeline>(std::move(sample_tex_pso_info)));
    }

    sample_texture_cs_pipeline =
        std::move(manager.Compute<UtilsSampleTexturePipelineCS>("core/utils/CopyTexture.cs.hlsl"));
}

void ShaderUtils::GenerateLowDiscrepancySequence(
    CommandList&                   _cmd_list,
    GenLowDiscrepancySequenceParam _param,
    BufferView                     _output
) {
    assert(_param.num_dimensions == 2);
    assert(_param.num_samples * _param.num_dimensions <= _output.GetByteSize());
    // _cmd_list.Compute(gen_low_discrepancy_pipeline, _param, _output).Dispatch(uint3(DivCeil(_param.num_samples, 256), 1, 1), MOER_TEXT("GenerateLowDiscrepancySequence"));

    Array<int8> data(_param.num_samples * 2);
    int         R    = 250;
    const float phi2 = 1.0f / 1.3247179572447f;
    uint32_t    num  = 0;
    float       u    = 0.5f;
    float       v    = 0.5f;
    while (num < _param.num_samples * 2) {
        u += phi2;
        v += phi2 * phi2;
        if (u >= 1.0f)
            u -= 1.0f;
        if (v >= 1.0f)
            v -= 1.0f;

        float rSq = (u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f);
        if (rSq > 0.25f)
            continue;

        data[num++] = int8((u - 0.5f) * R);
        data[num++] = (v - 0.5f) * R;
    }

    _cmd_list.CopyFrom(std::span<byte>((byte*)data.data(), data.size()), _output);

    _cmd_list.AddCallback([data(std::move(data))]() {});
}

void ShaderUtils::GenerateMipPdf(
    CommandList&           _cmd_list,
    const TextureView&     _env_map,
    std::span<TextureView> _integrated_mips,
    ETextureState          _final_state
) {
    if (_integrated_mips.empty()) {
        return;
    }

    PreprocessEnvironmentMapParams param;
    uint                           width  = _env_map.extent.x;
    uint                           height = _env_map.extent.y;

    for (uint i = 0; i < _integrated_mips.size(); i += 5) {
        param.src_mip_level  = i;
        param.num_mip_levels = _integrated_mips.size();
        param.src_size       = uint2(width, height);

        const uint write_begin = i == 0 ? 0u : i + 1;
        const uint write_count = i == 0 ? std::min<uint>(6u, uint(_integrated_mips.size()))
                                        : std::min<uint>(5u, uint(_integrated_mips.size() - i - 1));
        EmitMipChainBarrier(_cmd_list, _integrated_mips, i, write_begin, write_count);

        _cmd_list.Compute(generate_mip_pdf_pipeline, _env_map, _integrated_mips, param)
            .Dispatch(uint3(DivCeil(width, 32), DivCeil(height, 32), 1), MOER_TEXT("GenerateMipPdf"));
        width  = std::max(1u, width >> 5);
        height = std::max(1u, height >> 5);
    }

    FinalizeMipChainState(_cmd_list, _integrated_mips, _final_state);
}

void ShaderUtils::GenerateMips(
    CommandList&           _cmd_list,
    std::span<TextureView> _mips,
    ETextureState          _final_state
) {
    if (_mips.empty()) {
        return;
    }

    BuildMipsParam param;

    uint width  = _mips[0].extent.x;
    uint height = _mips[0].extent.y;

    if (_mips.size() > 5) {
        static bool b_first_time = true;
        if (b_first_time) {
            b_first_time = false;
            LOG_WARNING(
                MOER_TEXT("Here may be a bug when total mips > 5. GenerateMips for {} will be called. mip_level={}, ")
                MOER_TEXT("extent={},{}"),
                String(_mips[0].GetTexture()->GetName()),
                _mips[0].mip_level,
                _mips[0].extent.x,
                _mips[0].extent.y
            );
        }
    }

    for (uint i = 0; i < _mips.size(); i += 5) {
        param.num_mip_levels = _mips.size();
        param.src_mip_level  = i;
        param.src_size       = uint2(width, height);

        const uint write_begin = i + 1;
        const uint write_count = i + 1 >= _mips.size() ? 0u : std::min<uint>(5u, uint(_mips.size() - i - 1));
        EmitMipChainBarrier(_cmd_list, _mips, i, write_begin, write_count);

        _cmd_list.Compute(generate_mips_pipeline, _mips, param)
            .Dispatch(uint3(DivCeil(width, 32), DivCeil(height, 32), 1), MOER_TEXT("GenerateMips"));
        width  = std::max(1u, width >> 5);
        height = std::max(1u, height >> 5);
    }

    FinalizeMipChainState(_cmd_list, _mips, _final_state);
}

void ShaderUtils::ShowTexture(
    CommandList&             _cmd_list,
    BindlessArrayRef         _bdls,
    const ShowTextureParams& _param,
    TextureRef               _src_tex,
    TextureRef               _dst_texture
) {
    const EPixelFormat dst_format = _dst_texture->GetFormat();
    assert(show_texture_pipeline_map.contains(dst_format) && "Unsupported ShowTexture destination format");

    _cmd_list.Gfx(
                show_texture_pipeline_map.at(dst_format),
                _param,
                _src_tex->GetView(0, _src_tex->GetNumMips()),
                _bdls
            )
        .Draw(MOER_TEXT("ShowTexture"),
            Rect2D(0, 0, _dst_texture->GetExtent().x, _dst_texture->GetExtent().y),
            {},
            3,
            {SingleDrawParam(3, 1, 0, 0, 0)},
            ColorAttachment(_dst_texture)
        );
}

void ShaderUtils::SampleTextureRaster(
    CommandList& _cmd_list,
    TextureView  _src_tex,
    TextureView  _dst_texture,
    EPixelFormat _format
) {
    assert(0 && "SampleTextureRaster not implemented");
    Sampler linear_clamp_sampler{SF_LINEAR, SAM_CLAMP_TO_EDGE};
    //find dst pso
    if (!sample_texture_pipeline_map.contains(_format)) {
        assert(false && "Unsupported format");
    }

    auto& sample_texture_pipeline = sample_texture_pipeline_map[_format];

    _cmd_list.Gfx(sample_texture_pipeline, _src_tex, linear_clamp_sampler)
        .Draw(MOER_TEXT("SampleTexture"),
            Rect2D(0, 0, _dst_texture.extent.x, _dst_texture.extent.y),
            {},
            3,
            {SingleDrawParam(3, 1, 0, 0, 0)},
            ColorAttachment{_dst_texture.GetTexture()}
        );
}

void ShaderUtils::SampleTextureCS(
    CommandList& _cmd_list,
    TextureView  _src_tex,
    TextureView  _dst_texture,
    EPixelFormat _format
) {
    Sampler linear_clamp_sampler{SF_LINEAR, SAM_CLAMP_TO_EDGE};
    //find dst pso
    _cmd_list.Compute(sample_texture_cs_pipeline, _src_tex, linear_clamp_sampler, _dst_texture)
        .Dispatch(
            uint3(DivCeil(_dst_texture.extent.x, 16), DivCeil(_dst_texture.extent.y, 16), 1),
            MOER_TEXT("SampleTextureCS")
        );
}

} // namespace Moer::Render::Raytracing