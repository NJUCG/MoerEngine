#include "GaussianSplatPass.h"
#include "PixelFormat.h"
#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace Moer::Render::Raster {
namespace {
constexpr uint k_max_work_count = 2u * 1024u * 1024u;
uint           Groups(uint count) {
    return (count + 255u) / 256u;
}
class GaussianProjectPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GaussianProjectPipeline);
    DEFINE_SHADER_CONSTANT(GaussianDispatchParams, param);
    DEFINE_SHADER_BUFFER(camera);
    DEFINE_SHADER_BUFFER(gaussians);
    DEFINE_SHADER_BUFFER(instances);
    DEFINE_SHADER_BUFFER(projected);
    DEFINE_SHADER_BUFFER(tile_counts);
    DEFINE_SHADER_BUFFER(status);
    DEFINE_SHADER_ARGS(param, camera, gaussians, instances, projected, tile_counts, status);
};
class GaussianScanBlocksPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GaussianScanBlocksPipeline);
    DEFINE_SHADER_CONSTANT(GaussianDispatchParams, param);
    DEFINE_SHADER_BUFFER(input_values);
    DEFINE_SHADER_BUFFER(prefix);
    DEFINE_SHADER_BUFFER(block_sums);
    DEFINE_SHADER_ARGS(param, input_values, prefix, block_sums);
};
class GaussianScanAddPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GaussianScanAddPipeline);
    DEFINE_SHADER_CONSTANT(GaussianDispatchParams, param);
    DEFINE_SHADER_BUFFER(parent_prefix);
    DEFINE_SHADER_BUFFER(prefix);
    DEFINE_SHADER_ARGS(param, parent_prefix, prefix);
};
class GaussianFinalizePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GaussianFinalizePipeline);
    DEFINE_SHADER_CONSTANT(GaussianDispatchParams, param);
    DEFINE_SHADER_BUFFER(prefix);
    DEFINE_SHADER_BUFFER(status);
    DEFINE_SHADER_ARGS(param, prefix, status);
};
class GaussianEmitPairsPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GaussianEmitPairsPipeline);
    DEFINE_SHADER_CONSTANT(GaussianDispatchParams, param);
    DEFINE_SHADER_BUFFER(projected);
    DEFINE_SHADER_BUFFER(prefix);
    DEFINE_SHADER_BUFFER(status);
    DEFINE_SHADER_BUFFER(pairs);
    DEFINE_SHADER_ARGS(param, projected, prefix, status, pairs);
};
class GaussianRadixHistogramPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GaussianRadixHistogramPipeline);
    DEFINE_SHADER_CONSTANT(GaussianDispatchParams, param);
    DEFINE_SHADER_BUFFER(pairs);
    DEFINE_SHADER_BUFFER(status);
    DEFINE_SHADER_BUFFER(histogram);
    DEFINE_SHADER_ARGS(param, pairs, status, histogram);
};
class GaussianRadixOffsetsPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GaussianRadixOffsetsPipeline);
    DEFINE_SHADER_CONSTANT(GaussianDispatchParams, param);
    DEFINE_SHADER_BUFFER(histogram);
    DEFINE_SHADER_BUFFER(status);
    DEFINE_SHADER_BUFFER(offsets);
    DEFINE_SHADER_ARGS(param, histogram, status, offsets);
};
class GaussianRadixScatterPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GaussianRadixScatterPipeline);
    DEFINE_SHADER_CONSTANT(GaussianDispatchParams, param);
    DEFINE_SHADER_BUFFER(pairs);
    DEFINE_SHADER_BUFFER(status);
    DEFINE_SHADER_BUFFER(offsets);
    DEFINE_SHADER_BUFFER(sorted_pairs);
    DEFINE_SHADER_ARGS(param, pairs, status, offsets, sorted_pairs);
};
class GaussianTileRangesPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GaussianTileRangesPipeline);
    DEFINE_SHADER_CONSTANT(GaussianDispatchParams, param);
    DEFINE_SHADER_BUFFER(pairs);
    DEFINE_SHADER_BUFFER(status);
    DEFINE_SHADER_BUFFER(ranges);
    DEFINE_SHADER_ARGS(param, pairs, status, ranges);
};
class GaussianSplatPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GaussianSplatPipeline);
    DEFINE_SHADER_CONSTANT(GaussianDispatchParams, param);
    DEFINE_SHADER_BUFFER(camera);
    DEFINE_SHADER_BUFFER(projected);
    DEFINE_SHADER_BUFFER(pairs);
    DEFINE_SHADER_BUFFER(ranges);
    DEFINE_SHADER_BUFFER(status);
    DEFINE_SHADER_TEX(color_output);
    DEFINE_SHADER_TEX(transmittance_output);
    DEFINE_SHADER_TEX(opaque_depth);
    DEFINE_SHADER_TEX(reference_output);

    DEFINE_SHADER_ARGS(
        param,
        camera,
        projected,
        pairs,
        ranges,
        status,
        color_output,
        transmittance_output,
        opaque_depth,
        reference_output
    );
};
} // namespace
class GaussianDisplayPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GaussianDisplayPipeline);
    DEFINE_SHADER_CONSTANT(GaussianDispatchParams, param);
    DEFINE_SHADER_TEX(color_input);
    DEFINE_SHADER_TEX(transmittance_input);
    DEFINE_SHADER_BUFFER(ranges);
    DEFINE_SHADER_BUFFER(status);
    DEFINE_SHADER_TEX(display_output);
    DEFINE_SHADER_BUFFER(camera);
    DEFINE_SHADER_ARGS(param, color_input, transmittance_input, ranges, status, display_output, camera);
};
struct GaussianSplatPass::Impl {
    uint                           m_pair_capacity;
    GaussianProjectPipeline        m_project;
    GaussianScanBlocksPipeline     m_scanblocks;
    GaussianScanAddPipeline        m_scanadd;
    GaussianFinalizePipeline       m_finalize;
    GaussianEmitPairsPipeline      m_emitpairs;
    GaussianRadixHistogramPipeline m_radixhistogram;
    GaussianRadixOffsetsPipeline   m_radixoffsets;
    GaussianRadixScatterPipeline   m_radixscatter;
    GaussianTileRangesPipeline     m_tileranges;
    GaussianSplatPipeline          m_splat;
    GaussianDisplayPipeline        m_display_pipeline;
    BufferRef                      m_camera;
    uint2                          m_resolution{};
    uint                           m_work_capacity = 0;
    BufferRef m_projected, m_counts, m_status, m_pairs[2], m_histogram, m_offsets, m_ranges;
    struct ScanLevel {
        uint      count;
        BufferRef prefix;
        BufferRef sums;
    };
    std::vector<ScanLevel> m_scan;
    TextureRef             m_color, m_transmittance, m_display, m_reference, m_empty_depth;
    bool m_is_engine_linear = false;
    ReadbackFuture         m_status_readback;
    bool                   m_has_reported_overflow = false;
    GaussianRasterStatus   m_telemetry{};
    uint64                 m_readback_revision = 0;
    uint2                  m_readback_resolution{};
    QueryFuture            m_timing_future;
    void                   Poll() {
        if (m_status_readback.Valid() && m_status_readback.Status() != ReadbackStatus::Pending) {
            const auto result = m_status_readback.Get();
            const auto values = result.CopyAs<uint>();
            if (values && values->size() == 4 && (*values)[2] == m_telemetry.work_count &&
                m_readback_revision == m_telemetry.scene_revision && m_readback_resolution == m_resolution) {
                m_telemetry.tile_pair_count      = (*values)[0];
                m_telemetry.has_overflow         = (*values)[1] != 0;
                m_telemetry.visible_count        = (*values)[3];
                m_telemetry.has_completed_counts = true;
                if (m_telemetry.has_overflow && !m_has_reported_overflow)
                    LOG_ERROR(
                        "[GaussianRaster] Tile-pair capacity exceeded (limit={}); invalid magenta frame",
                        m_pair_capacity
                    );
                m_has_reported_overflow = m_telemetry.has_overflow;
            }
            m_status_readback = {};
        }
        if (m_timing_future.Valid() && m_timing_future.Status() != QueryStatus::Pending) {
            const auto result = m_timing_future.Get();
            if (const auto* timing = std::get_if<TimestampQueryResult>(&result.payload)) {
                m_telemetry.gpu_ms       = timing->duration_ns / 1000000.0;
                m_telemetry.has_gpu_time = result.status == QueryStatus::Ready;
            }
            m_timing_future = {};
        }
    }
    explicit Impl(ShaderManager& manager, GaussianRasterSettings settings) :
        m_pair_capacity(settings.tile_pair_capacity) {
        if (m_pair_capacity == 0 || m_pair_capacity > (1u << 24))
            throw std::invalid_argument("Gaussian pair capacity must be in [1, 2^24]");
        m_project    = manager.Compute<GaussianProjectPipeline>("features/gaussian/Project.comp.hlsl");
        m_scanblocks = manager.Compute<GaussianScanBlocksPipeline>("features/gaussian/ScanBlocks.comp.hlsl");
        m_scanadd    = manager.Compute<GaussianScanAddPipeline>("features/gaussian/ScanAdd.comp.hlsl");
        m_finalize   = manager.Compute<GaussianFinalizePipeline>("features/gaussian/Finalize.comp.hlsl");
        m_emitpairs  = manager.Compute<GaussianEmitPairsPipeline>("features/gaussian/EmitPairs.comp.hlsl");
        m_radixhistogram =
            manager.Compute<GaussianRadixHistogramPipeline>("features/gaussian/RadixHistogram.comp.hlsl");
        m_radixoffsets =
            manager.Compute<GaussianRadixOffsetsPipeline>("features/gaussian/RadixOffsets.comp.hlsl");
        m_radixscatter =
            manager.Compute<GaussianRadixScatterPipeline>("features/gaussian/RadixScatter.comp.hlsl");
        m_tileranges = manager.Compute<GaussianTileRangesPipeline>("features/gaussian/TileRanges.comp.hlsl");
        m_splat      = manager.Compute<GaussianSplatPipeline>("features/gaussian/Splat.comp.hlsl");
        m_display_pipeline = manager.Compute<GaussianDisplayPipeline>("features/gaussian/Display.comp.hlsl");
    }
    template<class T>
    BufferRef Buffer(const char* name, uint count) {
        return RenderDevice::Get().CreateBuffer<byte>(
            name,
            std::max(1u, count) * sizeof(T),
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
                EBufferUsageFlags::TRANSFER_SRC
        );
    }
    void Resize(uint2 resolution, uint work_count) {
        if (!m_empty_depth) {
            m_empty_depth = RenderDevice::Get().CreateTexture("Gaussian empty depth", Extent3D(1,1,1), PF_R32_SFLOAT,
                ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::UNORDERED_ACCESS);
        }
        if (!m_status) {
            m_status = Buffer<uint4>("Gaussian status", 1);
            for (auto& pairs : m_pairs)
                pairs = Buffer<uint4>("Gaussian tile-depth pairs", m_pair_capacity);
            m_histogram = Buffer<uint>("Gaussian radix histograms", Groups(m_pair_capacity) * 256u);
            m_offsets   = Buffer<uint>("Gaussian radix offsets", Groups(m_pair_capacity) * 256u);
        }
        if (resolution != m_resolution) {
            const auto     usage  = ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED |
                                    ETextureUsageFlags::TRANSFER_SRC | ETextureUsageFlags::TRANSFER_DST;
            auto&          device = RenderDevice::Get();
            const Extent3D extent(resolution.x, resolution.y, 1);
            m_color = device.CreateTexture(
                "Gaussian premultiplied RGB and alpha", extent, PF_R32G32B32A32_SFLOAT, usage
            );
            m_transmittance = device.CreateTexture("Gaussian transmittance", extent, PF_R32_SFLOAT, usage);
            m_display =
                device.CreateTexture("Gaussian display linear RGB", extent, PF_R32G32B32A32_SFLOAT, usage);
            m_reference = nullptr;
            m_ranges = Buffer<uint>(
                "Gaussian tile intervals", ((resolution.x + 15) / 16) * ((resolution.y + 15) / 16) * 2
            );
            m_resolution = resolution;
        }
        if (work_count > m_work_capacity || m_scan.empty()) {
            m_work_capacity = std::max(1u, work_count);
            m_projected     = Buffer<GProjectedGaussian>("Gaussian projected splats", m_work_capacity);
            m_counts        = Buffer<uint>("Gaussian tile counts", m_work_capacity);
            m_scan.clear();
            for (uint count = m_work_capacity;; count = Groups(count)) {
                m_scan.push_back(
                    {count,
                     Buffer<uint>("Gaussian scan prefix", count),
                     Buffer<uint>("Gaussian scan sums", Groups(count))}
                );
                if (Groups(count) == 1)
                    break;
            }
        }
    }
};
GaussianSplatPass::GaussianSplatPass(ShaderManager& manager, GaussianRasterSettings settings) :
    m_impl(std::make_unique<Impl>(manager, settings)) {}
GaussianSplatPass::~GaussianSplatPass() = default;
const TextureRef& GaussianSplatPass::GetColor() const {
    return m_impl->m_color;
}
const TextureRef& GaussianSplatPass::GetTransmittance() const {
    return m_impl->m_transmittance;
}
GaussianRasterStatus GaussianSplatPass::GetStatus() const {
    m_impl->Poll();
    return m_impl->m_telemetry;
}
uint GaussianSplatPass::GetPairCapacity() const {
    return m_impl->m_pair_capacity;
}
void GaussianSplatPass::SetPairCapacity(uint capacity) {
    if (!capacity || capacity > (1u << 24))
        throw std::invalid_argument("Invalid Gaussian pair capacity");
    if (capacity == m_impl->m_pair_capacity)
        return;
    // Keep PSOs alive; previous submissions retain their own buffer owners until GPU completion.
    m_impl->m_pair_capacity                  = capacity;
    m_impl->m_status                         = nullptr;
    m_impl->m_status_readback                = {};
    m_impl->m_telemetry.has_completed_counts = false;
    m_impl->m_telemetry.has_overflow         = false;
}
BufferView GaussianSplatPass::GetStatusBuffer() const {
    return m_impl->m_status->GetView();
}
TextureRef GaussianSplatPass::RecordAccumulation(
    CommandList&            commands,
    const GaussianGpuScene* scene,
    const Camera&           camera,
    uint2                   resolution,
    bool                    is_editor_view,
    bool                    is_unsupported_mixed,
    GaussianViewSettings    view_settings,
    TextureRef opaque_depth,
    bool is_engine_linear,
    bool needs_reference
) {
    auto& state = *m_impl;
    if (resolution.x == 0 || resolution.y == 0)
        throw std::runtime_error("Gaussian viewport is empty");
    uint64     work_count_64   = 0;
    const uint visibility_mask = is_editor_view ? 2u : 1u;
    if (scene)
        for (const auto& instance : scene->GetInstances())
            if (instance.range.w & visibility_mask)
                work_count_64 += instance.range.y;
    const bool is_work_overflow = work_count_64 > k_max_work_count;
    state.m_telemetry.has_work_overflow = is_work_overflow;
    const uint work_count       = is_work_overflow ? 0u : static_cast<uint>(work_count_64);
    if (!IsValidGaussianViewSettings(view_settings))
        throw std::invalid_argument("Invalid Gaussian view settings");
    const uint64 revision = scene ? scene->GetRevision() : 0;
    if (state.m_telemetry.scene_revision != revision || state.m_resolution != resolution)
        state.m_telemetry.has_completed_counts = false;
    state.m_telemetry.is_active             = true;
    state.m_telemetry.scene_revision        = revision;
    state.m_telemetry.unique_gaussian_count = scene ? scene->GetGaussianCount() : 0;
    state.m_telemetry.instance_count        = scene ? scene->GetInstanceCount() : 0;
    state.m_telemetry.work_count            = work_count;
    state.m_telemetry.geometry_bytes = uint64(state.m_telemetry.unique_gaussian_count) * sizeof(GGaussian) +
                                       uint64(state.m_telemetry.instance_count) * sizeof(GGaussianInstance);
    state.Resize(resolution, work_count);
    state.m_is_engine_linear = is_engine_linear;
    if (!state.m_reference || (needs_reference && state.m_reference->GetWidth() != resolution.x)) {
        state.m_reference = RenderDevice::Get().CreateTexture("Gaussian reference RGB",
            needs_reference ? Extent3D(resolution.x, resolution.y, 1) : Extent3D(1,1,1),
            PF_R32G32B32A32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
    }
    if (!opaque_depth) commands.ClearResource(state.m_empty_depth->GetView(), float4(0));
    // Commands retain descriptor resources through translation. Keep owners explicitly until
    // GPU completion too: camera uploads, resize and instance replacement can overlap frames.
    Array<BufferRef> buffer_owners{
        state.m_projected,
        state.m_counts,
        state.m_status,
        state.m_pairs[0],
        state.m_pairs[1],
        state.m_histogram,
        state.m_offsets,
        state.m_ranges
    };
    for (const auto& level : state.m_scan) {
        buffer_owners.push_back(level.prefix);
        buffer_owners.push_back(level.sums);
    }
    state.m_telemetry.workspace_bytes = uint64(resolution.x) * resolution.y * 36u;
    for (const auto& buffer : buffer_owners)
        if (buffer)
            state.m_telemetry.workspace_bytes += buffer->GetByteSize();
    if (scene) {
        buffer_owners.push_back(scene->GetGaussianBuffer());
        buffer_owners.push_back(scene->GetInstanceBuffer());
    }
    commands.AddCallback([buffer_owners = std::move(buffer_owners),
                          color         = state.m_color,
                          transmittance = state.m_transmittance,
                          display       = state.m_display, reference = state.m_reference, depth = opaque_depth,
                          empty_depth = state.m_empty_depth]() {});
    state.Poll();
    QueryToken timing_token;
    if (!state.m_timing_future.Valid()) {
        timing_token          = commands.BeginTimestampQuery("Gaussian Raster Total");
        state.m_timing_future = timing_token.GetFuture();
    }
    commands.PushScopeWithTimeScope("Gaussian Raster");
    auto camera_data           = std::make_shared<GGaussianCamera>();
    camera_data->world_to_view = camera.GetViewMatrix();
    camera_data->view_to_clip  = camera.GetProjectionMatrix();
    camera_data->clip_to_view  = camera.GetProjectionMatrixInv();
    const auto position        = camera.GetPosition();
    camera_data->position      = float4(position.x, position.y, position.z, 1);
    camera_data->viewport =
        uint4(resolution.x, resolution.y, (resolution.x + 15) / 16, (resolution.y + 15) / 16);
    camera_data->filter     = float4(0.3f, 1.f / 255.f, camera.GetNearClip(), camera.GetFarClip());
    camera_data->background = float4(
        view_settings.background.x,
        view_settings.background.y,
        view_settings.background.z,
        view_settings.exposure_ev
    );
    auto camera_buffer = RenderDevice::Get().CreateBuffer<byte>(
        "Gaussian camera",
        sizeof(GGaussianCamera),
        EBufferUsageFlags::CONSTANT_BUFFER | EBufferUsageFlags::TRANSFER_DST
    );
    commands.CopyFrom(
        std::span<byte>(reinterpret_cast<byte*>(camera_data.get()), sizeof(GGaussianCamera)),
        camera_buffer->GetView()
    );
    commands.AddCallback([camera_data, camera_buffer]() {});
    state.m_camera = camera_buffer;
    if (is_unsupported_mixed || is_work_overflow || work_count == 0) {
        const bool is_invalid                  = is_unsupported_mixed || is_work_overflow;
        state.m_telemetry.has_overflow         = is_work_overflow;
        state.m_telemetry.has_completed_counts = true;
        state.m_telemetry.tile_pair_count = state.m_telemetry.visible_count = 0;
        commands.ClearResource(state.m_status->GetView(), is_invalid ? 1u : 0u);
        commands.ClearResource(state.m_color->GetView(), is_invalid ? float4(1, 0, 1, 0) : float4(0));
        commands.ClearResource(state.m_transmittance->GetView(), float4(1));
        commands.ClearResource(state.m_ranges->GetView(), 0u);
        commands.ClearResource(state.m_reference->GetView(), float4(0));
        commands.PopScopeWithTimeScope();
        if (timing_token.Valid())
            commands.EndTimestampQuery(timing_token);
        return state.m_color;
    }
    GaussianDispatchParams param{};
    param.info = uint4(work_count, state.m_pair_capacity, camera_data->viewport.z, camera_data->viewport.w);
    commands.ClearResource(state.m_status->GetView(), 0u);
    uint work_offset = 0;
    for (uint instance_id = 0; instance_id < scene->GetInstances().size(); ++instance_id) {
        const auto& instance = scene->GetInstances()[instance_id];
        if (!(instance.range.w & visibility_mask) || !instance.range.y)
            continue;
        param.job = uint4(instance.range.x, instance_id, work_offset, instance.range.y);
        commands
            .Compute(
                state.m_project,
                param,
                camera_buffer->GetView(),
                scene->GetGaussianBuffer()->GetView(),
                scene->GetInstanceBuffer()->GetView(),
                state.m_projected->GetView(),
                state.m_counts->GetView(),
                state.m_status->GetView()
            )
            .Dispatch(uint3(Groups(instance.range.y), 1, 1), "Gaussian project");
        work_offset += instance.range.y;
    }
    uint   scan_count       = work_count;
    auto   input            = state.m_counts;
    size_t scan_level_count = 0;
    for (auto& level : state.m_scan) {
        level.count = scan_count;
        param.job   = uint4(scan_count, 0, 0, 0);
        commands
            .Compute(
                state.m_scanblocks, param, input->GetView(), level.prefix->GetView(), level.sums->GetView()
            )
            .Dispatch(uint3(Groups(scan_count), 1, 1), "Gaussian scan blocks");
        ++scan_level_count;
        if (Groups(scan_count) == 1)
            break;
        input      = level.sums;
        scan_count = Groups(scan_count);
    }
    for (size_t level_id = scan_level_count - 1; level_id > 0; --level_id) {
        auto& child = state.m_scan[level_id - 1];
        param.job   = uint4(child.count, 0, 0, 0);
        commands
            .Compute(
                state.m_scanadd, param, state.m_scan[level_id].prefix->GetView(), child.prefix->GetView()
            )
            .Dispatch(uint3(Groups(child.count), 1, 1), "Gaussian scan add");
    }
    commands.Compute(state.m_finalize, param, state.m_scan[0].prefix->GetView(), state.m_status->GetView())
        .Dispatch(uint3(1, 1, 1), "Gaussian finalize counts");
    // Count and emit use identical pixel-center bounds on partially clipped tiles.
    param.job = uint4(resolution.x, resolution.y, 0, 0);
    commands
        .Compute(
            state.m_emitpairs,
            param,
            state.m_projected->GetView(),
            state.m_scan[0].prefix->GetView(),
            state.m_status->GetView(),
            state.m_pairs[0]->GetView()
        )
        .Dispatch(uint3(Groups(work_count), 1, 1), "Gaussian emit pairs");
    for (uint digit_id = 0; digit_id < 8; ++digit_id) {
        param.job          = uint4(digit_id, 0, 0, 0);
        auto& pairs        = state.m_pairs[digit_id & 1];
        auto& sorted_pairs = state.m_pairs[(digit_id + 1) & 1];
        commands
            .Compute(
                state.m_radixhistogram,
                param,
                pairs->GetView(),
                state.m_status->GetView(),
                state.m_histogram->GetView()
            )
            .Dispatch(uint3(Groups(state.m_pair_capacity), 1, 1), "Gaussian radix histogram");
        commands
            .Compute(
                state.m_radixoffsets,
                param,
                state.m_histogram->GetView(),
                state.m_status->GetView(),
                state.m_offsets->GetView()
            )
            .Dispatch(uint3(1, 1, 1), "Gaussian radix offsets");
        commands
            .Compute(
                state.m_radixscatter,
                param,
                pairs->GetView(),
                state.m_status->GetView(),
                state.m_offsets->GetView(),
                sorted_pairs->GetView()
            )
            .Dispatch(uint3(Groups(state.m_pair_capacity), 1, 1), "Gaussian radix scatter");
    }
    commands.ClearResource(state.m_ranges->GetView(), 0u);
    commands
        .Compute(
            state.m_tileranges,
            param,
            state.m_pairs[0]->GetView(),
            state.m_status->GetView(),
            state.m_ranges->GetView()
        )
        .Dispatch(uint3(Groups(state.m_pair_capacity), 1, 1), "Gaussian tile ranges");
    param.job = uint4(
        uint(is_engine_linear), uint(needs_reference), uint(bool(opaque_depth)), 0
    );
    commands
        .Compute(
            state.m_splat,
            param,
            camera_buffer->GetView(),
            state.m_projected->GetView(),
            state.m_pairs[0]->GetView(),
            state.m_ranges->GetView(),
            state.m_status->GetView(),
            state.m_color->GetView(),
            state.m_transmittance->GetView(),
            opaque_depth ? opaque_depth->GetView() : state.m_empty_depth->GetView(),
            state.m_reference->GetView()
        )
        .Dispatch(uint3(camera_data->viewport.z, camera_data->viewport.w, 1), "Gaussian splat");
    if (!state.m_status_readback.Valid()) {
        state.m_status_readback     = commands.Readback(state.m_status->GetView(), "Gaussian pair status");
        state.m_readback_revision   = revision;
        state.m_readback_resolution = resolution;
    }
    commands.PopScopeWithTimeScope();
    if (timing_token.Valid())
        commands.EndTimestampQuery(timing_token);
    return state.m_color;
}
TextureRef GaussianSplatPass::RecordDisplay(CommandList& commands, GaussianViewSettings settings) {
    auto& state = *m_impl;
    GaussianDispatchParams param{};
    param.job = uint4(static_cast<uint>(settings.display_mode), static_cast<uint>(settings.debug_view), 0, 0);
    commands.Compute(state.m_display_pipeline, param, (state.m_is_engine_linear ? state.m_reference : state.m_color)->GetView(),
        state.m_transmittance->GetView(), state.m_ranges->GetView(), state.m_status->GetView(),
        state.m_display->GetView(), state.m_camera->GetView())
        .Dispatch(uint3((state.m_resolution.x + 15) / 16, (state.m_resolution.y + 15) / 16, 1), "Gaussian reference display");
    commands.AddCallback([display = state.m_display, camera = state.m_camera]() {});
    return state.m_display;
}
TextureRef GaussianSplatPass::Record(CommandList& commands, const GaussianGpuScene* scene,
    const Camera& camera, uint2 resolution, bool is_editor_view, bool is_invalid, GaussianViewSettings settings) {
    RecordAccumulation(commands, scene, camera, resolution, is_editor_view, is_invalid, settings);
    return RecordDisplay(commands, settings);
}
const TextureRef& GaussianSplatPass::GetDisplay() const { return m_impl->m_display; }
const BufferRef& GaussianSplatPass::GetRanges() const { return m_impl->m_ranges; }
GaussianSplatPass::RecordParameters GaussianSplatPass::Prepare(const GaussianGpuScene* scene,
    const Camera& camera, uint2 resolution, bool is_editor_view, GaussianViewSettings settings,
    TextureRef opaque_depth, bool needs_reference) {
    uint64 work_count = 0;
    if (scene) for (const auto& instance : scene->GetInstances())
        if (instance.range.w & (is_editor_view ? 2u : 1u)) work_count += instance.range.y;
    m_impl->Resize(resolution, work_count > k_max_work_count ? 0 : uint(work_count));
    if (!m_impl->m_reference || (needs_reference && m_impl->m_reference->GetWidth() != resolution.x))
        m_impl->m_reference = RenderDevice::Get().CreateTexture("Gaussian reference RGB",
            needs_reference ? Extent3D(resolution.x,resolution.y,1) : Extent3D(1,1,1),
            PF_R32G32B32A32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
    return {scene, camera, resolution, is_editor_view, settings, std::move(opaque_depth), true, needs_reference};
}
void GaussianSplatPass::Record(CommandList& commands, const RecordParameters& parameters) {
    RecordAccumulation(commands, parameters.scene, parameters.camera, parameters.resolution,
        parameters.is_editor_view, false, parameters.settings, parameters.opaque_depth,
        parameters.is_engine_linear, parameters.needs_reference);
    if (parameters.needs_reference) RecordDisplay(commands, parameters.settings);
}
GaussianSplatPass::GraphResources GaussianSplatPass::ImportGraphResources(RenderGraph& graph) const {
    auto& state = *m_impl;
    return {graph.ImportTexture("gs_color", state.m_color, {}),
        graph.ImportTexture("gs_transmittance", state.m_transmittance, {}),
        graph.ImportTexture("gs_reference", state.m_reference, {}),
        graph.ImportTexture("gs_display", state.m_display, {}),
        graph.ImportBuffer("gs_status", state.m_status, {.byte_size = state.m_status->GetByteSize()}),
        graph.ImportBuffer("gs_ranges", state.m_ranges, {.byte_size = state.m_ranges->GetByteSize()})};
}
RenderGraph::PassHandle GaussianSplatPass::AddToGraph(RenderGraph& graph, RecordParameters parameters,
    GraphResources resources, RenderGraph::TextureHandle opaque_depth) {
    Array<RenderGraph::BufferHandle> scratch;
    auto add_buffer = [&](const BufferRef& buffer) {
        scratch.push_back(graph.ImportBuffer("gs_workspace_" + std::to_string(scratch.size()), buffer, {.byte_size = buffer->GetByteSize()}));
    };
    auto& state = *m_impl;
    for (const auto& buffer : {state.m_projected, state.m_counts, state.m_pairs[0], state.m_pairs[1],
                              state.m_histogram, state.m_offsets}) add_buffer(buffer);
    for (const auto& level : state.m_scan) { add_buffer(level.prefix); add_buffer(level.sums); }
    Array<RenderGraph::BufferHandle> inputs;
    if (parameters.scene && parameters.scene->GetGaussianBuffer()) {
        for (const auto& buffer : {parameters.scene->GetGaussianBuffer(), parameters.scene->GetInstanceBuffer()})
            inputs.push_back(graph.ImportBuffer("gs_scene_" + std::to_string(inputs.size()), buffer, {.byte_size = buffer->GetByteSize()}));
    }
    return graph.AddRecordPass("GaussianRaster", [=](RenderGraph::PassBuilder& builder) {
        builder.Read(opaque_depth).Write(resources.color).Write(resources.transmittance)
            .Write(resources.reference).Write(resources.display).ReadWrite(resources.status).ReadWrite(resources.ranges).SideEffect();
        for (const auto buffer : scratch) builder.ReadWrite(buffer);
        for (const auto buffer : inputs) builder.Read(buffer);
    }, [this, parameters](CommandList& commands) { Record(commands, parameters); },
        RenderGraph::PassExecutionClass::SerialRecord);
}
} // namespace Moer::Render::Raster
