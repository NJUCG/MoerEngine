#include "VulkanRaytracingResource.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanEnumTranslator.h"
#include "VulkanMacroUtils.h"
#include "VulkanRaytracingInstance.h"
#include "VulkanVertexFormat.h"
#include "log/LogSystem.h"
#include "misc/Alignment.h"
#include "misc/MMemory.h"
#include "misc/Types.h"

#include <algorithm>
#include <format>

namespace Moer::Render {

static constexpr std::string_view s_tlas_underlying_buffer_name = "VkRaytracing::TLASUnderlyingBuffer";
static constexpr std::string_view s_blas_underlying_buffer_name = "VkRaytracing::BLASUnderlyingBuffer";
static constexpr std::string_view s_tlas_scratch_buffer_name    = "VkRaytracing::TLASScratchBuffer";
static constexpr std::string_view s_tlas_instance_buffer_name   = "VkRaytracing::TLASInstanceBuffer";

static void BuildRaytracingGeometryInfo(
    Array<VkAccelerationStructureGeometryKHR>&       _build_info,
    Array<uint32>&                                   _primitive_counts,
    Array<VkAccelerationStructureBuildRangeInfoKHR>& _build_ranges,
    const RaytracingGeometryInfo&                    _info
) {

    _build_info.reserve(_info.segments.size());
    _build_ranges.reserve(_info.segments.size());
    _primitive_counts.reserve(_info.segments.size());

    assert(_info.segments.size() > 0 && "No segment to build");

    const VkFormat vertex_format = ToVulkanVertexFormat(_info.vertex_format);
    if (vertex_format == VK_FORMAT_UNDEFINED) {
        throw std::invalid_argument("Invalid Vulkan ray tracing vertex format");
    }

    for (const auto& segment : _info.segments) {
        //TODO: currently only support triangle
        assert(segment.type == RTGT_TRIANGLES && "Unsupported geometry type");

        uint64 vtx_addr = ResourceCast(segment.vertex_buffer.Get())->DeviceAddress();
        uint64 idx_addr = ResourceCast(segment.index_buffer.Get())->DeviceAddress();
        assert(vtx_addr != 0 && idx_addr != 0 && "Invalid buffer address");

        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VulkanEnumTranslator::METoVKGeometryType(segment.type);
        geometry.flags        = VulkanEnumTranslator::METoVKGeometryFlags(segment.flags);
        geometry.geometry.triangles.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geometry.geometry.triangles.vertexFormat             = vertex_format;
        geometry.geometry.triangles.vertexStride             = segment.vertex_stride;
        geometry.geometry.triangles.vertexData.deviceAddress = vtx_addr + segment.vertex_offset;
        // maxVertex: Vulkan spec 要求为 vertexData 中可被寻址的最高顶点索引。
        // 由于 firstVertex 会加到每个 index 上，最高绝对索引 = first_vertex + vertex_count - 1。
        geometry.geometry.triangles.maxVertex = segment.first_vertex + segment.vertex_count - 1;
        geometry.geometry.triangles.indexType = VulkanEnumTranslator::METoVKIndexType(_info.index_type);
        geometry.geometry.triangles.indexData.deviceAddress     = idx_addr + segment.index_offset;
        geometry.geometry.triangles.transformData.deviceAddress = 0;

        _build_info.push_back(geometry);
        _primitive_counts.push_back(segment.primitive_count);

        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.firstVertex = segment.first_vertex;
        range.primitiveOffset =
            segment.first_primitive * 3 * (_info.index_type == EIndexElementType::IET_UINT16 ? 2 : 4);
        range.primitiveCount  = segment.primitive_count;
        range.transformOffset = 0;

        _build_ranges.push_back(range);
    }
}

VulkanRaytracingGeometry::VulkanRaytracingGeometry(
    const RaytracingGeometryInfo& _info,
    VulkanDevice*                 _device
) :
    VulkanDeviceObject(_device),
    RaytracingGeometry(_info) {

    VkAccelerationStructureBuildTypeKHR         build_type = VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR build_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
    };
    Array<uint32> primitive_counts;
    BuildRaytracingGeometryInfo(build_geometries, primitive_counts, build_ranges, _info);
    build_info.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build_info.flags         = VulkanEnumTranslator::METoVKAccelerationStructureBuildType(_info.build_flags);
    build_info.geometryCount = build_geometries.size();
    build_info.pGeometries   = build_geometries.data();
    vkGetAccelerationStructureBuildSizesKHR(
        m_device->GetDevice(), build_type, &build_info, primitive_counts.data(), &build_sizes_info
    );

    underlying_buffer = MoerNew(VulkanBuffer)(
        s_blas_underlying_buffer_name,
        BufferInfo{build_sizes_info.accelerationStructureSize, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE},
        *m_device,
        VulkanBufferAllocationInfo{
            .vk_usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .needs_device_address = true,
        },
        false
    );

    VkAccelerationStructureCreateInfoKHR create_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR
    };
    create_info.deviceAddress = 0; //for capture replay
    create_info.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    create_info.createFlags   = 0;
    create_info.buffer        = underlying_buffer->GetHandle();
    create_info.offset        = 0;
    create_info.size          = underlying_buffer->GetByteSize();

    VK_CHECK_RESULT(vkCreateAccelerationStructureKHR(m_device->GetDevice(), &create_info, nullptr, &acc));
    VkAccelerationStructureDeviceAddressInfoKHR address_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR
    };
    address_info.accelerationStructure = acc;
    blas_address = vkGetAccelerationStructureDeviceAddressKHR(m_device->GetDevice(), &address_info);
}

VulkanAccelerationStructure::VulkanAccelerationStructure(VulkanDevice& device) : device(device) {}

VulkanRaytracingGeometry::~VulkanRaytracingGeometry() {
    if (acc != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(m_device->GetDevice(), acc, VK_NULL_HANDLE);
        acc = VK_NULL_HANDLE;
    }

    if (underlying_buffer) {
        underlying_buffer = nullptr;
    }
}

VulkanAccelerationStructure::~VulkanAccelerationStructure() {
    if (handle != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(device.GetDevice(), handle, VK_NULL_HANDLE);
        handle = VK_NULL_HANDLE;
    }

    if (underlying_buffer) {
        underlying_buffer = nullptr;
    }
    if (m_descriptor_cache_slot >= 0) {
        device.GetGlobalDescriptorHeap().FreeAccelerationStructureDescriptorSlot(m_descriptor_cache_slot);
    }
}

void VulkanAccelerationStructure::Destroy() {
    device.EnqueueDeferredRelease(this);
}

VulkanRaytracingScene::VulkanRaytracingScene(VulkanDevice* device) : VulkanDeviceObject(device) {
    instance_buffer = CreateInstanceBuffer(instance_capacity);
}

RaytracingInstance& VulkanRaytracingScene::AddInstance() {
    auto& instance = VulkanRaytracingDetail::AddInstance(instances, free_instance_slots);
    MarkModified(instance.array_idx);
    return instance;
}

void VulkanRaytracingScene::FreeInstance(uint32 array_index) {
    VulkanRaytracingDetail::FreeInstance(instances, free_instance_slots, array_index);
    MarkModified(array_index);
}

void VulkanRaytracingScene::MarkModified(uint32 _array_idx) {
    auto res = modified_instance_indices.emplace(_array_idx);
    if (res.second) {
        pending_instance_indices.emplace_back(_array_idx);
    }
}

static VkGeometryInstanceFlagsKHR
ToVulkanInstanceFlags(const RaytracingSegment& _segment) {
    VkGeometryInstanceFlagsKHR flags = 0;
    if ((EnumHasAllFlag(_segment.flags, ERayTracingGeometryFlags::GEOMETRY_OPAQUE)) &&
        _segment.b_force_opaque) {
        flags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
    }
    if (_segment.b_flip_face) {
        flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
    }
    if (!_segment.b_cull_back_face) {
        flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    }
    return flags;
}

static VkAccelerationStructureInstanceKHR BuildNativeInstance(const RaytracingInstance& instance) {
    const auto* geometry = ResourceCast(instance.geom);
    if (!geometry) {
        if (instance.visible_mask != RTVM_NONE) {
            throw std::invalid_argument("Visible ray tracing instance has no geometry");
        }
        return {};
    }
    const auto& segments = geometry->GetInfo().segments;
    if (instance.segment_idx >= segments.size()) {
        throw std::invalid_argument("Ray tracing instance segment is out of range");
    }
    VkAccelerationStructureInstanceKHR native{};
    native.instanceCustomIndex = instance.custom_index;
    native.mask = instance.visible_mask;
    native.instanceShaderBindingTableRecordOffset = instance.material_ref.sbt_offset;
    native.flags = ToVulkanInstanceFlags(segments[instance.segment_idx]);
    native.accelerationStructureReference = geometry->blas_address;
    std::memcpy(&native.transform, &instance.transform, sizeof(native.transform));
    return native;
}

UniquePtr<Command> VulkanRaytracingScene::UpdateScene() {

    UnorderedMap<uint64, uint32> related_geometries;
    Array<byte> instance_update_data;
    if (instances.empty()) {
        pending_instance_indices.clear();
        modified_instance_indices.clear();
        previous_modified_instance_indices.clear();
        return {};
    }

    //update gpu buffer
    bool instance_buffer_recreated = false;
    if (instance_capacity < instances.size()) {
        GrowInstanceBuffer();
        instance_buffer_recreated = true;
    }
    bool force_build = EnsureAccelerationStructureStorage();

    if (!previous_modified_instance_indices.empty()) {
        force_build = true;
    }

    previous_modified_instance_indices.clear();

    if (instance_buffer_recreated) {
        pending_instance_indices.clear();
        for (uint32 i = 0; i < instances.size(); i++) {
            pending_instance_indices.push_back(i);
        }
    }

    instance_update_data.resize(pending_instance_indices.size() * sizeof(VkAccelerationStructureInstanceKHR));
    for (size_t update_index = 0; update_index < pending_instance_indices.size(); ++update_index) {
        const auto native_instance = BuildNativeInstance(instances[pending_instance_indices[update_index]]);
        std::memcpy(
            instance_update_data.data() + update_index * sizeof(native_instance),
            &native_instance, sizeof(native_instance)
        );
    }

    Array<RaytracingGeometryRef> geometry_refs;
    geometry_refs.reserve(instances.size());
    for (const RaytracingInstance& instance : instances) {
        if (!instance.geom) {
            continue;
        }
        auto pair = related_geometries.try_emplace(uint64(instance.geom), 1);
        if (pair.second) {
            geometry_refs.emplace_back(instance.geom);
        } else {
            ++pair.first->second;
        }
    }

    auto cmd = MakeUnique<UpdateRaytracingSceneCmd>(
        std::move(related_geometries),
        RaytracingSceneRef(this),
        BufferRef(instance_buffer.Get()),
        BufferRef(scratch_buffer.Get()),
        RaytracingTlasRef(tlas.Get()),
        std::move(geometry_refs),
        std::move(pending_instance_indices),
        std::move(instance_update_data),
        instances.size(),
        instance_buffer_recreated || force_build
    );
    modified_instance_indices.clear();
    return cmd;
}

void VulkanRaytracingScene::AdvanceFrame() {

    assert(
        previous_modified_instance_indices.empty() &&
        "There are still modified instances not updated, call UpdateScene() first"
    );

    previous_modified_instance_indices.swap(modified_instance_indices);

    std::swap(prev_instance_capacity, instance_capacity);
    prev_size_infos.build_scratch_size  = size_infos.build_scratch_size;
    prev_size_infos.update_scratch_size = size_infos.update_scratch_size;
    std::swap(prev_size_infos, size_infos);

    std::swap(prev_tlas, tlas);
}

RaytracingTlasRef VulkanRaytracingScene::GetTlas() const {
    return RaytracingTlasRef(tlas.Get());
}

RaytracingTlasRef VulkanRaytracingScene::GetPrevTlas() const {
    return RaytracingTlasRef(prev_tlas.Get());
}

VulkanBufferRef VulkanRaytracingScene::CreateInstanceBuffer(uint32 capacity) {
    // Preserve the existing 256-byte address alignment policy and padding.
    const uint64 byte_size = uint64(capacity) * sizeof(VkAccelerationStructureInstanceKHR) + 256;
    return MoerNew(VulkanBuffer)(
        s_tlas_instance_buffer_name,
        BufferInfo{byte_size, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE},
        *m_device,
        VulkanBufferAllocationInfo{
            .vk_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            .allocation_flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .needs_device_address = true,
        },
        true
    );
}

void VulkanRaytracingScene::GrowInstanceBuffer() {
    const uint32 capacity = std::max<size_t>(instances.size(), size_t(instance_capacity) * 2);
    instance_buffer = CreateInstanceBuffer(capacity);
    instance_capacity = capacity;
}

RaytracingSizeInfos VulkanRaytracingScene::CalculateSizeInfos() {
    VkAccelerationStructureBuildGeometryInfoKHR build_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
    };

    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType             = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.flags                    = 0;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.arrayOfPointers = VK_FALSE;
    // Use 16-byte aligned device address for TLAS instance data (Vulkan spec requirement)
    constexpr uint64 kInstanceDataAlignment = 256; // 256-byte for AMD GPU compatibility
    uint64 aligned_device_address = Moer::AlignUp(instance_buffer->DeviceAddress(), kInstanceDataAlignment);
    geometry.geometry.instances.data.deviceAddress = aligned_device_address;

    build_info.type                      = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build_info.flags                     = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build_info.mode                      = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build_info.srcAccelerationStructure  = VK_NULL_HANDLE;
    build_info.geometryCount             = 1;
    build_info.pGeometries               = &geometry;
    build_info.scratchData.deviceAddress = 0;
    build_info.dstAccelerationStructure  = VK_NULL_HANDLE;

    VkAccelerationStructureBuildSizesInfoKHR build_sizes_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
    };

    uint32 instance_cnt = instances.size();
    vkGetAccelerationStructureBuildSizesKHR(
        m_device->GetDevice(),
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &build_info,
        &instance_cnt,
        &build_sizes_info
    );

    // reserve more space for buffer device address alignment
    const uint64 alignment =
        m_device->GetOptionalProperties()
            .acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment;
    build_sizes_info.buildScratchSize =
        Moer::AlignUp(build_sizes_info.buildScratchSize + alignment, alignment);
    build_sizes_info.updateScratchSize =
        Moer::AlignUp(build_sizes_info.updateScratchSize + alignment, alignment);
    return {
        build_sizes_info.buildScratchSize,
        build_sizes_info.updateScratchSize,
        build_sizes_info.accelerationStructureSize
    };
}

bool VulkanRaytracingScene::EnsureAccelerationStructureStorage() {
    const RaytracingSizeInfos required = CalculateSizeInfos();
    const bool grow_tlas = required.result_size > size_infos.result_size;
    const bool grow_scratch = required.build_scratch_size > size_infos.build_scratch_size;
    if (!grow_tlas && !grow_scratch) {
        return false;
    }

    // Publish replacements only after every allocation and native creation succeeds.
    VulkanBufferRef next_scratch = scratch_buffer;
    VulkanAccelRef next_tlas = tlas;
    if (grow_scratch) {
        next_scratch = MoerNew(VulkanBuffer)(
            s_tlas_scratch_buffer_name,
            BufferInfo{required.build_scratch_size, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE},
            *m_device,
            VulkanBufferAllocationInfo{
                .vk_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                .needs_device_address = true,
            },
            true
        );
    }
    if (grow_tlas) {
        UniquePtr<VulkanAccelerationStructure> replacement = MakeUnique<VulkanAccelerationStructure>(*m_device);
        replacement->underlying_buffer = MoerNew(VulkanBuffer)(
            s_tlas_underlying_buffer_name,
            BufferInfo{required.result_size, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE},
            *m_device,
            VulkanBufferAllocationInfo{
                .vk_usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                .memory_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                .needs_device_address = true,
            },
            true
        );
        const VkAccelerationStructureCreateInfoKHR create_info{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .createFlags = VK_ACCELERATION_STRUCTURE_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT,
            .buffer = replacement->underlying_buffer->GetHandle(),
            .size = replacement->underlying_buffer->GetByteSize(),
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        };
        VK_CHECK_RESULT(vkCreateAccelerationStructureKHR(
            m_device->GetDevice(), &create_info, nullptr, &replacement->handle
        ));
        next_tlas = replacement.release();
    }
    scratch_buffer = std::move(next_scratch);
    tlas = std::move(next_tlas);
    size_infos = required;
    return true;
}

} // namespace Moer::Render
