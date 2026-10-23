#include "VulkanRaytracingResource.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanEnumTranslator.h"
#include "VulkanMacroUtils.h"
#include "VulkanVertexFormat.h"
#include "log/LogSystem.h"
#include "misc/MMemory.h"

#include <algorithm>
#include <format>

namespace Moer::Render {

static constexpr std::string_view s_tlas_underlying_buffer_name = "VkRaytracing::TLASUnderlyingBuffer";
static constexpr std::string_view s_blas_underlying_buffer_name = "VkRaytracing::BLASUnderlyingBuffer";
static constexpr std::string_view s_blas_scratch_buffer_name    = "VkRaytracing::BLASScratchBuffer";
static constexpr std::string_view s_tlas_scratch_buffer_name    = "VkRaytracing::TLASScratchBuffer";
static constexpr std::string_view s_tlas_instance_buffer_name   = "VkRaytracing::TLASInstanceBuffer";

static void GetBuildRaytracingGeometryInfo(
    Array<VkAccelerationStructureGeometryKHR>&       _build_info,
    Array<uint>&                                     _primitive_counts,
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

    VkGeometryTypeKHR geometry_type = VulkanEnumTranslator::METoVKGeometryType(_info.segments[0].type);

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
    Array<uint> primitive_counts;
    GetBuildRaytracingGeometryInfo(build_geometries, primitive_counts, build_ranges, _info);
    build_info.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build_info.flags         = VulkanEnumTranslator::METoVKAccelerationStructureBuildType(_info.build_flags);
    build_info.geometryCount = build_geometries.size();
    build_info.pGeometries   = build_geometries.data();
    vkGetAccelerationStructureBuildSizesKHR(
        m_device->GetDevice(), build_type, &build_info, primitive_counts.data(), &build_sizes_info
    );

    {
        BufferInfo buffer_info(
            build_sizes_info.accelerationStructureSize, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE
        );

        VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_ci.size  = buffer_info.size;
        buffer_ci.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

#if WITH_CUDA
        buffer_ci.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.flags = 0;
        alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VkBuffer      buffer = VK_NULL_HANDLE;
        VmaAllocation alloc  = VK_NULL_HANDLE;

        vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &buffer, &alloc, nullptr);

        underlying_buffer = MoerNew(VulkanBuffer)(
            s_blas_underlying_buffer_name, buffer_info, *m_device, buffer, alloc, false, true
        );
    }

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

VulkanAccelerationStructure::VulkanAccelerationStructure(
    VulkanDevice&          _device,
    VulkanRaytracingScene& _scene
) :
    device(_device),
    src_scene(_scene),
    RaytracingTlas() {}

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

VulkanRaytracingScene::VulkanRaytracingScene(VulkanDevice* _device) :
    VulkanDeviceObject(_device),
    RaytracingScene() {

    VkBuffer      current_handle = VK_NULL_HANDLE;
    VmaAllocation alloc          = VK_NULL_HANDLE;

    // TLAS instance buffer must be 16-byte aligned (Vulkan spec requirement)
    // Add extra padding for alignment and use 256-byte alignment for AMD GPU compatibility
    constexpr uint64 kInstanceBufferAlignment     = 256;
    const uint64     base_instance_buffer_size    = sizeof(VkAccelerationStructureInstanceKHR) * 1000;
    const uint64     aligned_instance_buffer_size = base_instance_buffer_size + kInstanceBufferAlignment;

    VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_ci.size  = aligned_instance_buffer_size;
    buffer_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    buffer_ci.flags       = 0;

#if WITH_CUDA
    buffer_ci.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VK_CHECK_RESULT(
        vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr)
    );

    instance_buffer = MoerNew(VulkanBuffer)(
        s_tlas_instance_buffer_name,
        BufferInfo{buffer_ci.size, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE},
        *m_device,
        current_handle,
        alloc,
        true,
        true
    );

    // Initialize aligned address for AMD GPU compatibility
    UpdateAlignedInstanceBufferAddress();
}

VulkanRaytracingScene::~VulkanRaytracingScene() {}

RaytracingInstance& VulkanRaytracingScene::AddInstance() {
    // uint idx = instances.size();
    uint idx = 0;
    if (free_instance_slots.empty()) {
        idx = instance_offset++;
    } else {
        idx = free_instance_slots.back();
        free_instance_slots.pop_back();
    }
    RaytracingInstance instance{.array_idx = idx - 1, .instance_id = idx - 1};
    VkAccelerationStructureInstanceKHR vk_instance{

        };
    instances.emplace_back(instance);
    vk_instances.emplace_back(vk_instance);
    MarkModified(idx - 1);

    if (idx >= instance_capacity) {
        //resize the buffer
    }
    return instances.back();
}

void VulkanRaytracingScene::FreeInstance(uint _array_idx) {
    instances[_array_idx].visible_mask = RTVM_NONE;
    MarkModified(_array_idx); //Is not correct, because maybe the instance has invalid BLAS
    free_instance_slots.push_back(_array_idx + 1);
    //TODO: use default blas as place holder for invalid rtgeometry
}

void VulkanRaytracingScene::MarkModified(uint _array_idx) {
    auto res = temp_modified_instance_ids.emplace(_array_idx);
    if (res.second) {
        temp_update_instance_ids.emplace_back(_array_idx);
    }
}

void VulkanRaytracingScene::RegisterGeometry(RaytracingGeometryRef _geometry) {
    VulkanRaytracingGeometry* geometry = ResourceCast(_geometry.Get());
    assert(geometry && "Invalid geometry");

    auto pair = related_geometries.try_emplace(uint64(geometry), 1);
    if (!pair.second) {
        related_geometries[uint64(geometry)]++;
    }
}

void VulkanRaytracingScene::UnregisterGeometry(RaytracingGeometryRef _geometry) {
    VulkanRaytracingGeometry* geometry = ResourceCast(_geometry.Get());
    assert(geometry && "Invalid geometry");
    auto iter = related_geometries.find(uint64(geometry));
    if (iter == related_geometries.end()) {
        return;
    }
    if (iter->second > 1) {
        --iter->second;
    } else {
        related_geometries.erase(iter);
    }
}

static VkGeometryInstanceFlagsKHR
METoRTInstanceFlags(const RaytracingSegment& _segment, const RaytracingInstance& _instance) {
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

UniquePtr<Command> VulkanRaytracingScene::UpdateScene() {

    related_geometries.clear();
    temp_update_instances.clear();
    if (instances.empty()) {
        temp_update_instance_ids.clear();
        temp_modified_instance_ids.clear();
        prev_modified_instance_ids.clear();
        return {};
    }

    //update gpu buffer
    bool b_full_refit = false;
    if (instance_capacity < instances.size()) {
        RefitInstanceBuffer();
        b_full_refit = true;
    }
    bool b_need_force_build = RefitTLASAndScratchBuffer();

    // for(auto prev_idx : prev_modified_instance_ids){
    //     if(!temp_modified_instance_ids.contains(prev_idx)){
    //         temp_update_instance_ids.push_back(prev_idx);
    //     }
    // }
    if (!prev_modified_instance_ids.empty()) {
        b_need_force_build = true;
    }

    prev_modified_instance_ids.clear();

    // b_current_full_refit |= b_full_refit;

    // if(b_prev_full_refit){
    //     b_full_refit = true;
    //     b_prev_full_refit = false;
    // }

    if (b_full_refit) {
        temp_update_instances.reserve(instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
        std::span<byte> temp_span(
            (byte*)vk_instances.data(), vk_instances.size() * sizeof(VkAccelerationStructureInstanceKHR)
        );

        temp_update_instance_ids.clear();
        for (uint i = 0; i < instances.size(); i++) {
            temp_update_instance_ids.push_back(i);
        }
    }

    {

        temp_update_instances.resize(
            temp_update_instance_ids.size() * sizeof(VkAccelerationStructureInstanceKHR)
        );
        //update cpu size vk_instance datas
        for (const auto& idx : temp_update_instance_ids) {
            VulkanRaytracingGeometry* geometry = ResourceCast(instances[idx].geom);
            assert(geometry && "Invalid geometry");
            const Array<RaytracingSegment>&     segments       = geometry->GetInfo().segments;
            VkAccelerationStructureInstanceKHR& vk_instance    = vk_instances[idx];
            const RaytracingInstance&           instance       = instances[idx];
            vk_instance.instanceCustomIndex                    = instance.custom_index;
            vk_instance.mask                                   = instance.visible_mask;
            vk_instance.instanceShaderBindingTableRecordOffset = instance.material_ref.sbt_offset;
            vk_instance.flags = METoRTInstanceFlags(segments[instance.segment_idx], instance);
            vk_instance.accelerationStructureReference = geometry->blas_address;
            std::memcpy(&vk_instance.transform, &instance.transform, sizeof(vk_instance.transform));

            uint indice = &idx - &temp_update_instance_ids[0];
            std::memcpy(
                temp_update_instances.data() + indice * sizeof(VkAccelerationStructureInstanceKHR),
                &vk_instance,
                sizeof(VkAccelerationStructureInstanceKHR)
            );
        }
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

    //maybe we should calculate relative blas here
    // Array<uint> instance_ids_to_update = temp_update_instance_ids;

    // RefitTLASAndScratchBuffer();
    auto cmd = MakeUnique<UpdateRaytracingSceneCmd>(
        std::move(related_geometries),
        RaytracingSceneRef(this),
        BufferRef(instance_buffer.Get()),
        BufferRef(scratch_buffer.Get()),
        RaytracingTlasRef(tlas.Get()),
        std::move(geometry_refs),
        std::move(temp_update_instance_ids),
        std::move(temp_update_instances),
        vk_instances.size(),
        b_full_refit || b_need_force_build
    );
    temp_modified_instance_ids.clear();
    return cmd;
}

void VulkanRaytracingScene::AdvanceFrame() {

    assert(
        prev_modified_instance_ids.empty() &&
        "There are still modified instances not updated, call UpdateScene() first"
    );

    prev_modified_instance_ids.swap(temp_modified_instance_ids);
    b_current_full_refit = false;

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

void VulkanRaytracingScene::RefitInstanceBuffer() {
    VkBuffer      current_handle = VK_NULL_HANDLE;
    VmaAllocation alloc          = VK_NULL_HANDLE;

    instance_capacity *= exponent;

    // TLAS instance buffer must be 16-byte aligned (Vulkan spec requirement)
    // Add extra padding for alignment and use 256-byte alignment for AMD GPU compatibility
    constexpr uint64 kInstanceBufferAlignment = 256;
    const uint64 base_instance_buffer_size = sizeof(VkAccelerationStructureInstanceKHR) * instance_capacity;
    const uint64 aligned_instance_buffer_size = base_instance_buffer_size + kInstanceBufferAlignment;

    VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_ci.size  = aligned_instance_buffer_size;
    buffer_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    buffer_ci.flags       = 0;

#if WITH_CUDA
    buffer_ci.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VK_CHECK_RESULT(
        vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr)
    );
    instance_buffer = MoerNew(VulkanBuffer)(
        s_tlas_instance_buffer_name,
        BufferInfo{buffer_ci.size, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE},
        *m_device,
        current_handle,
        alloc,
        true,
        true
    );

    // Update aligned address after buffer recreation
    UpdateAlignedInstanceBufferAddress();
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

    uint instance_cnt = instances.size();
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

bool VulkanRaytracingScene::RefitTLASAndScratchBuffer() {
    //calculate prebuild info

    RaytracingSizeInfos build_sizes_info = CalculateSizeInfos();

    if (build_sizes_info.result_size > size_infos.result_size ||
        build_sizes_info.build_scratch_size > size_infos.build_scratch_size) {
        //recreate buffers
        VkBuffer      current_handle = VK_NULL_HANDLE;
        VmaAllocation alloc          = VK_NULL_HANDLE;

        VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

        buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        buffer_ci.flags       = 0;

#if WITH_CUDA
        buffer_ci.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.flags = 0;
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        if (build_sizes_info.build_scratch_size > size_infos.build_scratch_size) {
            //scratch buffer
            buffer_ci.size  = build_sizes_info.build_scratch_size;
            buffer_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            VK_CHECK_RESULT(vmaCreateBuffer(
                m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr
            ));

            scratch_buffer = MoerNew(VulkanBuffer)(
                s_tlas_scratch_buffer_name,
                BufferInfo{buffer_ci.size, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE},
                *m_device,
                current_handle,
                alloc,
                true,
                true
            );
        }
        //TLAS
        if (build_sizes_info.result_size > size_infos.result_size) {
            buffer_ci.size  = build_sizes_info.result_size;
            buffer_ci.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            VK_CHECK_RESULT(vmaCreateBuffer(
                m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr
            ));

            tlas = MoerNew(VulkanAccelerationStructure)(*m_device, *this);

            tlas->underlying_buffer = MoerNew(VulkanBuffer)(
                s_tlas_underlying_buffer_name,
                BufferInfo{buffer_ci.size, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE},
                *m_device,
                current_handle,
                alloc,
                true,
                true
            );

            //create new TLAS
            VkAccelerationStructureCreateInfoKHR create_info{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR
            };
            create_info.deviceAddress = 0; //for capture replay
            create_info.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            create_info.createFlags =
                VK_ACCELERATION_STRUCTURE_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT;
            create_info.buffer = tlas->underlying_buffer->GetHandle();
            create_info.offset = 0;
            create_info.size   = tlas->underlying_buffer->GetByteSize();
            VK_CHECK_RESULT(
                vkCreateAccelerationStructureKHR(m_device->GetDevice(), &create_info, nullptr, &tlas->handle)
            );

            // //create previous tlas
            // if(prev_size_infos.result_size == 0){

            //     VK_CHECK_RESULT(vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr));
            //     prev_tlas = MoerNew(VulkanAccelerationStructure)(*m_device, *this);
            //     prev_tlas->underlying_buffer = MoerNew(VulkanBuffer)(BufferInfo{buffer_ci.size, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE}, *m_device, current_handle, alloc, false, true);
            //     prev_tlas->underlying_buffer->SetName("TLAS underly buffer Pong");
            //     create_info.buffer = prev_tlas->underlying_buffer->GetHandle();
            //     VK_CHECK_RESULT(vkCreateAccelerationStructureKHR(m_device->GetDevice(), &create_info, nullptr, &prev_tlas->handle));
            //     prev_size_infos = build_sizes_info;
            // }
        }
        size_infos = build_sizes_info;
        return true;
    }
    return false;
}

} // namespace Moer::Render
