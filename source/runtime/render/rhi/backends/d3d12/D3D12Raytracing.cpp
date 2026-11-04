#include "D3D12Backend.h"
#include <cstring>

namespace Moer::Render::D3D12 {
namespace {
D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS BuildFlags(ERayTracingAccelerationStructureBuildFlags flags) {
    return D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(uint32(flags));
}
} // namespace

Geometry::Geometry(Context& context, const RaytracingGeometryInfo& info) :
    RaytracingGeometry(info) {
    for (const auto& segment : info.segments) {
        D3D12_RAYTRACING_GEOMETRY_DESC desc{};
        desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAGS(uint32(segment.flags));
        if (!segment.vertex_buffer)
            throw std::invalid_argument("D3D12 geometry has no vertex buffer");
        auto& vertices = *static_cast<GpuBuffer*>(segment.vertex_buffer.Get());
        if (segment.type == RTGT_TRIANGLES) {
            desc.Type              = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            auto& triangles        = desc.Triangles;
            triangles.VertexFormat = VertexFormat(info.vertex_format);
            triangles.VertexCount  = segment.vertex_count;
            triangles.VertexBuffer = {vertices.Address(segment.vertex_offset + uint64(segment.first_vertex) * segment.vertex_stride), segment.vertex_stride};
            if (segment.index_buffer) {
                const uint32 stride   = info.index_type == IET_UINT16 ? 2 : 4;
                triangles.IndexFormat = stride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
                triangles.IndexCount  = segment.primitive_count * 3;
                triangles.IndexBuffer = static_cast<GpuBuffer*>(segment.index_buffer.Get())->Address(segment.index_offset + uint64(segment.first_primitive) * 3 * stride);
            } else {
                triangles.VertexBuffer.StartAddress += uint64(segment.first_primitive) * 3 * segment.vertex_stride;
                triangles.VertexCount = segment.primitive_count * 3;
            }
        } else {
            desc.Type            = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
            desc.AABBs.AABBCount = segment.primitive_count;
            desc.AABBs.AABBs     = {vertices.Address(segment.vertex_offset + uint64(segment.first_primitive) * segment.vertex_stride), segment.vertex_stride};
        }
        descriptions.push_back(desc);
    }
    const auto inputs = Inputs();
    context.device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
    if (!sizes.ResultDataMaxSizeInBytes)
        throw std::invalid_argument("D3D12 geometry prebuild returned an empty result");
    storage = context.Buffer(sizes.ResultDataMaxSizeInBytes, EBufferUsageFlags::ACCELERATION_STRUCTURE);
    storage->SetName("D3D12 BLAS");
}
D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Geometry::Inputs() const {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs       = uint32(descriptions.size());
    inputs.pGeometryDescs = descriptions.data();
    inputs.Flags          = BuildFlags(info.build_flags);
    return inputs;
}
Scene::Scene(Context& context) :
    context(context),
    current(MoerNew(Tlas)()),
    previous(MoerNew(Tlas)()) {}
RaytracingInstance& Scene::AddInstance() {
    if (!free_instances.empty()) {
        const uint32 index = free_instances.back();
        free_instances.pop_back();
        auto& instance            = instances[index];
        instance.geom             = nullptr;
        instance.visible_mask     = RTVM_ALL;
        instance.flag.need_create = true;
        return instance;
    }
    const uint32 index = uint32(instances.size());
    instances.push_back(RaytracingInstance{.geom = nullptr, .array_idx = index, .instance_id = index});
    return instances.back();
}
void Scene::FreeInstance(uint32 index) {
    if (index >= instances.size())
        throw std::out_of_range("D3D12 scene instance index");
    if (std::find(free_instances.begin(), free_instances.end(), index) != free_instances.end())
        return;
    instances[index].geom         = nullptr;
    instances[index].visible_mask = RTVM_NONE;
    free_instances.push_back(index);
}
void Scene::MarkModified(uint32 index) {
    if (index >= instances.size())
        throw std::out_of_range("D3D12 scene instance index");
    instances[index].flag.need_update = true;
}
UniquePtr<Command> Scene::UpdateScene() {
    Array<byte>                  bytes(instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    Array<RaytracingGeometryRef> geometries;
    UnorderedMap<uint64, uint32> related;
    Array<uint32>                indices;
    for (uint32 i = 0; i < instances.size(); ++i) {
        const auto&                    instance = instances[i];
        D3D12_RAYTRACING_INSTANCE_DESC native{};
        std::memcpy(native.Transform, &instance.transform, sizeof(native.Transform));
        native.InstanceID                          = instance.custom_index;
        native.InstanceMask                        = instance.visible_mask;
        native.InstanceContributionToHitGroupIndex = uint32(instance.material_ref.sbt_offset);
        if (instance.geom) {
            auto*      geometry = static_cast<Geometry*>(instance.geom);
            const auto info     = geometry->GetInfo();
            if (instance.segment_idx >= info.segments.size())
                throw std::out_of_range("D3D12 ray tracing segment index");
            const auto& segment = info.segments[instance.segment_idx];
            if (!segment.b_cull_back_face)
                native.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            if (segment.b_flip_face)
                native.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
            if (segment.b_force_opaque)
                native.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
            native.AccelerationStructure = static_cast<GpuBuffer*>(geometry->storage.Get())->Address();
            if (related.emplace(uint64(geometry), 1).second)
                geometries.emplace_back(geometry);
        } else if (instance.visible_mask != RTVM_NONE) {
            throw std::invalid_argument("Visible D3D12 scene instance has no geometry");
        }
        std::memcpy(bytes.data() + i * sizeof(native), &native, sizeof(native));
        indices.push_back(i);
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type        = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs    = uint32(instances.size());
    inputs.Flags       = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
    context.device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
    // Each update publishes a new immutable TLAS identity. Recorded passes and
    // temporal history keep the exact generation they selected.
    current                                    = MoerNew(Tlas)();
    static_cast<Tlas*>(current.Get())->storage = context.Buffer(std::max<uint64>(256, sizes.ResultDataMaxSizeInBytes), EBufferUsageFlags::ACCELERATION_STRUCTURE);
    auto instance_buffer                       = context.Buffer(std::max<size_t>(sizeof(D3D12_RAYTRACING_INSTANCE_DESC), bytes.size()), EBufferUsageFlags::NONE, D3D12_HEAP_TYPE_UPLOAD);
    auto scratch                               = context.Buffer(std::max<uint64>(256, sizes.ScratchDataSizeInBytes), EBufferUsageFlags::UNORDERED_ACCESS);
    return MakeUnique<UpdateRaytracingSceneCmd>(std::move(related), RaytracingSceneRef{this}, instance_buffer, scratch, current, std::move(geometries), std::move(indices), std::move(bytes), uint32(instances.size()), true);
}
void Scene::AdvanceFrame() {
    previous = current;
}

void Recorder::Build(const BuildAccelerationStructuresCmd& command) {
    for (const auto& param : command.Params()) {
        auto& geometry = *static_cast<Geometry*>(param.geometry.Get());
        for (const auto& segment : geometry.GetInfo().segments) {
            Transition(*static_cast<GpuBuffer*>(segment.vertex_buffer.Get()), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (segment.index_buffer)
                Transition(*static_cast<GpuBuffer*>(segment.index_buffer.Get()), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        auto scratch = context.Buffer(std::max(geometry.sizes.ScratchDataSizeInBytes, geometry.sizes.UpdateScratchDataSizeInBytes), EBufferUsageFlags::UNORDERED_ACCESS);
        packet.buffers.push_back(scratch);
        Transition(*static_cast<GpuBuffer*>(scratch.Get()), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs                           = geometry.Inputs();
        build.DestAccelerationStructureData    = static_cast<GpuBuffer*>(geometry.storage.Get())->Address();
        build.ScratchAccelerationStructureData = static_cast<GpuBuffer*>(scratch.Get())->Address();
        if (param.mode == ERaytracingBuildMode::UPDATE && geometry.built &&
            (build.Inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE)) {
            build.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
            build.SourceAccelerationStructureData = build.DestAccelerationStructureData;
        }
        list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(static_cast<GpuBuffer*>(geometry.storage.Get())->resource.Get());
        list->ResourceBarrier(1, &barrier);
        packet.rollback.push_back([&geometry, built = geometry.built] {
            geometry.built = built;
        });
        geometry.built = true;
    }
}
void Recorder::Build(const UpdateRaytracingSceneCmd& command) {
    auto&             instances = *static_cast<GpuBuffer*>(command.InstanceBuffer().Get());
    void*             mapped    = nullptr;
    const D3D12_RANGE empty{0, 0};
    DX_CHECK_HRESULT(instances.resource->Map(0, &empty, &mapped));
    std::memcpy(mapped, command.InstanceData().data(), command.InstanceData().size());
    instances.resource->Unmap(0, nullptr);
    auto& tlas    = *static_cast<Tlas*>(command.Tlas().Get());
    auto& scratch = *static_cast<GpuBuffer*>(command.ScratchBuffer().Get());
    Transition(scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs.Type                      = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    build.Inputs.DescsLayout               = D3D12_ELEMENTS_LAYOUT_ARRAY;
    build.Inputs.NumDescs                  = command.InstanceCount();
    build.Inputs.InstanceDescs             = instances.Address();
    build.Inputs.Flags                     = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    build.DestAccelerationStructureData    = static_cast<GpuBuffer*>(tlas.storage.Get())->Address();
    build.ScratchAccelerationStructureData = scratch.Address();
    list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(static_cast<GpuBuffer*>(tlas.storage.Get())->resource.Get());
    list->ResourceBarrier(1, &barrier);
}
} // namespace Moer::Render::D3D12
