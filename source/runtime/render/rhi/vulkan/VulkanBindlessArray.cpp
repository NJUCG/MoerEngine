#include "VulkanBindlessArray.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanResourceState.h"
#include "VulkanMacroUtils.h"
#include "log/LogSystem.h"
#include "misc/Alignment.h"
#include "misc/RAII.h"
#include "rhi/RHIBindlessUpdateLifecycle.h"
#include "rhi/RHIExecutor.h"

#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace Moer::Render {

static constexpr std::string_view s_bindless_index_buffer_name       = "VkBindless::IndexTable";
static constexpr std::string_view s_bindless_buffer_descriptors_name = "VkBindless::BufferDescriptors";
static constexpr std::string_view s_bindless_texture_descriptors_name =
    "VkBindless::TextureAndSamplerDescriptors";

struct VulkanBindlessArray::LegacyDescriptorSets {
    VkDevice              device;
    VkDescriptorPool      pool = VK_NULL_HANDLE;
    VkDescriptorSet       sets[3]{};
    VkDescriptorSetLayout layouts[3]{};
    explicit LegacyDescriptorSets(VkDevice native_device) : device(native_device) {}
    ~LegacyDescriptorSets() {
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        for (auto layout : layouts)
            if (layout)
                vkDestroyDescriptorSetLayout(device, layout, nullptr);
    }
};

VulkanBindlessArray::VulkanBindlessArray(VulkanDevice* _device, uint32 _max_size) :
    BindlessArray(),
    VulkanDeviceObject(_device),
    texture_offset_in_buffer(GetBindlessTextureDescriptorOffset(*_device)),
    handles(_max_size),
    g_heap(_device->GetGlobalDescriptorHeap()),
    texture_slot_offset(1),
    buffer_slot_offset(1),
    slot_offset(1),
    array_slot_generations(_max_size, 0),
    texture_slot_generations(_max_size, 0),
    buffer_slot_generations(_max_size, 0),
    array_slot_claim_tokens(_max_size, 0),
    texture_slot_claim_tokens(_max_size, 0),
    buffer_slot_claim_tokens(_max_size, 0),
    accepted_presentation_source_membership(std::make_shared<const PresentationSourceMembership>()),
    texture_view_infos(_max_size) {
    if (!_max_size || _max_size > RenderDevice::k_default_bindless_array_size)
        throw std::invalid_argument("Bindless array exceeds the shader table capacity");
    CreateLegacySets(_max_size);
    const auto create_buffer = [&](std::string_view name, uint64 bytes, VkBufferUsageFlags usage) {
        return std::make_unique<VulkanBuffer>(
            name,
            BufferInfo(bytes, 1, EBufferUsageFlags::UNORDERED_ACCESS),
            *m_device,
            VulkanBufferAllocationInfo{
                .vk_usage =
                    usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .needs_device_address = true,
            },
            false
        );
    };
    m_index_buffer = create_buffer(
        s_bindless_index_buffer_name, uint64(_max_size) * sizeof(uint32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    );
    if (_device->GetDescriptorMode() == VulkanDescriptorMode::Sets)
        return;
    const uint buffer_size = g_heap.GetDescriptorSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    const uint image_size  = g_heap.GetDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    if (_device->GetDescriptorMode() == VulkanDescriptorMode::Heap) {
        heap_lease = g_heap.CreateDescriptorBufferLease();
        try {
            const uint64 buffer_bytes =
                Moer::AlignUp(uint64(_max_size) * buffer_size, g_heap.GetStorageAlignment());
            const uint64 image_bytes =
                Moer::AlignUp(uint64(_max_size) * image_size, g_heap.GetStorageAlignment());
            auto offset = g_heap.ReserveDescriptorRange(heap_lease, buffer_bytes + image_bytes);
            if (!offset)
                throw std::runtime_error("Persistent bindless heap allocation exhausted");
            buffer_descriptors_byte_offset = *offset;
            texture_offset_in_buffer       = *offset + buffer_bytes;
            heap_buffer                    = g_heap.GetResourceHeapBuffer();
        } catch (...) {
            g_heap.RecycleDescriptorBufferLease(std::move(heap_lease));
            throw;
        }
        return;
    }
    // Query both layouts: implementations may add padding before binding 0.
    auto table_size = [&](VkDescriptorType type, uint count, uint64& binding_offset) {
        VkDescriptorSetLayoutBinding   binding{0, type, count, VK_SHADER_STAGE_ALL};
        const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
                                               VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                               VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO
        };
        binding_flags.bindingCount  = 1;
        binding_flags.pBindingFlags = &flags;
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        info.pNext        = &binding_flags;
        info.bindingCount = 1;
        info.pBindings    = &binding;
        VkDescriptorSetLayout layout;
        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(_device->GetDevice(), &info, nullptr, &layout));
        auto   release = OnScopeExit([&] {
            vkDestroyDescriptorSetLayout(_device->GetDevice(), layout, nullptr);
        });
        uint64 size    = 0;
        vkGetDescriptorSetLayoutSizeEXT(_device->GetDevice(), layout, &size);
        vkGetDescriptorSetLayoutBindingOffsetEXT(_device->GetDevice(), layout, 0, &binding_offset);
        return size;
    };
    const uint   table_capacity = RenderDevice::k_default_bindless_array_size;
    const uint64 buffer_bytes =
        table_size(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, table_capacity, buffer_descriptors_byte_offset);
    uint64       image_binding_offset = 0;
    const uint64 image_bytes =
        table_size(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, table_capacity, image_binding_offset);
    const uint64 image_set_offset = GetBindlessTextureDescriptorOffset(*_device);
    texture_offset_in_buffer      = image_set_offset + image_binding_offset;
    m_buffer_descriptors          = create_buffer(
        s_bindless_buffer_descriptors_name,
        buffer_bytes,
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    );
    m_texture_sampler_descriptors = create_buffer(
        s_bindless_texture_descriptors_name,
        image_set_offset + image_bytes,
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    );
    const uint  sampler_stride = g_heap.GetDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLER);
    Array<byte> sampler_data(VulkanDevice::bindless_sampler_count * sampler_stride);
    for (uint i = 0; i < VulkanDevice::bindless_sampler_count; ++i)
        g_heap.CopyDescriptorToMemory(
            {VK_DESCRIPTOR_TYPE_SAMPLER, i},
            std::span(sampler_data).subspan(i * sampler_stride, sampler_stride)
        );
    CommandList cmd;
    cmd.CopyFrom(
        std::span(sampler_data),
        m_texture_sampler_descriptors->GetView(g_heap.GetBindlessSamplerBindingOffset(), sampler_data.size())
    );
    RHIExecutor::Get().Submit(EQueueType::Graphics, cmd.Submit(), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
}

void VulkanBindlessArray::CreateLegacySets(uint max_size) {
    legacy_sets                    = std::make_unique<LegacyDescriptorSets>(m_device->GetDevice());
    const VkDescriptorType types[] = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER
    };
    const uint           counts[] = {max_size, max_size, VulkanDevice::bindless_sampler_count};
    auto&                layouts  = legacy_sets->layouts;
    VkDescriptorPoolSize sizes[3]{};
    for (uint i = 0; i < 3; ++i) {
        VkDescriptorSetLayoutBinding binding{
            0, types[i], i == 2 ? counts[i] : RenderDevice::k_default_bindless_array_size, VK_SHADER_STAGE_ALL
        };
        const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
                                               VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                               VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
                                               VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO
        };
        binding_flags.bindingCount  = 1;
        binding_flags.pBindingFlags = &flags;
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        info.pNext        = &binding_flags;
        info.bindingCount = 1;
        info.pBindings    = &binding;
        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(m_device->GetDevice(), &info, nullptr, &layouts[i]));
        sizes[i] = {types[i], counts[i]};
    }
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool.maxSets       = 3;
    pool.poolSizeCount = 3;
    pool.pPoolSizes    = sizes;
    VK_CHECK_RESULT(vkCreateDescriptorPool(m_device->GetDevice(), &pool, nullptr, &legacy_sets->pool));
    VkDescriptorSetVariableDescriptorCountAllocateInfo variable{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO
    };
    variable.descriptorSetCount = 3;
    variable.pDescriptorCounts  = counts;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.pNext              = &variable;
    allocate.descriptorPool     = legacy_sets->pool;
    allocate.descriptorSetCount = 3;
    allocate.pSetLayouts        = layouts;
    VK_CHECK_RESULT(vkAllocateDescriptorSets(m_device->GetDevice(), &allocate, legacy_sets->sets));
    Array<VkDescriptorImageInfo> samplers(counts[2]);
    for (uint i = 0; i < counts[2]; ++i)
        samplers[i].sampler = m_device->GetImmutableSamplers()[i < m_device->ImmutableSamplerCount() ? i : 0];
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet          = legacy_sets->sets[2];
    write.descriptorCount = counts[2];
    write.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo      = samplers.data();
    vkUpdateDescriptorSets(m_device->GetDevice(), 1, &write, 0, nullptr);
}

VkDescriptorSet VulkanBindlessArray::GetLegacySet(VkDescriptorType type) const {
    return legacy_sets->sets
        [type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? 0 :
         type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE  ? 1 :
                                                     2];
}

VkAccessFlags2 VulkanBindlessArray::GetDescriptorReadAccess() const {
    return m_device->GetDescriptorMode() == VulkanDescriptorMode::Heap ?
               VK_ACCESS_2_RESOURCE_HEAP_READ_BIT_EXT :
               VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT;
}

static uint SamplerToIndex(const Sampler& _samp) {
    uint idx = uint(_samp.filter) + uint(_samp.address_mode) * uint(SF_Num) +
               uint(_samp.compare_function) * uint(SAM_Num) * uint(SF_Num);
    return idx;
}

static bool RangeOverlap(uint32 _a_start, uint32 _a_count, uint32 _b_start, uint32 _b_count) {
    uint32 a_count = _a_count == 0 ? 1u : _a_count;
    uint32 b_count = _b_count == 0 ? 1u : _b_count;
    uint32 a_end   = _a_start + a_count;
    uint32 b_end   = _b_start + b_count;
    return _a_start < b_end && _b_start < a_end;
}

void VulkanBindlessArray::TrackResource(RHIResource* _resource) {
    const uint64 _handle = uint64(_resource);
    if (_handle == 0) {
        throw std::runtime_error("cannot track a null bindless resource");
    }
    auto [resource, inserted] = resource_allocation_counts.try_emplace(_handle);
    if (inserted) {
        resource->second.resource = CountableRef<RHIResource>(_resource);
    }
    if (resource->second.count == std::numeric_limits<uint32>::max()) {
        throw std::runtime_error("bindless resource reference count overflow");
    }
    ++resource->second.count;
}

void VulkanBindlessArray::UntrackResource(uint64 _handle) {
    const auto resource = resource_allocation_counts.find(_handle);
    if (resource == resource_allocation_counts.end() || resource->second.count == 0) {
        throw std::runtime_error("cannot untrack an unknown bindless resource");
    }
    if (resource->second.count == 1) {
        resource_allocation_counts.erase(resource);
    } else {
        --resource->second.count;
    }
}

void VulkanBindlessArray::TrackTextureView(uint _array_idx, const TextureView& _view) {
    if (_array_idx >= texture_view_infos.size()) {
        throw std::runtime_error("bindless texture view slot is outside capacity");
    }
    UntrackTextureView(_array_idx);
    auto*  vk_texture  = ResourceCast(_view.texture);
    uint64 texture_ptr = uint64(vk_texture);
    if (texture_ptr == 0) {
        return;
    }
    ValidateSubresourceRange(
        _view.texture, _view.mip_level, _view.num_mips, _view.array_layer, _view.num_array
    );
    TextureSubresourceKeyT<VulkanTexture> key{
        vk_texture, _view.mip_level, _view.num_mips, _view.array_layer, _view.num_array
    };
    texture_view_infos[_array_idx] = key;
    uint32& ref_count              = texture_view_ref_counts[texture_ptr][key];
    if (ref_count == std::numeric_limits<uint32>::max()) {
        throw std::runtime_error("bindless texture view reference count overflow");
    }
    ++ref_count;
}

void VulkanBindlessArray::UntrackTextureView(uint _array_idx) {
    if (_array_idx >= texture_view_infos.size()) {
        return;
    }
    const auto key = texture_view_infos[_array_idx];
    if (key.texture == nullptr) {
        return;
    }
    uint64     texture_ptr = uint64(key.texture);
    const auto texture     = texture_view_ref_counts.find(texture_ptr);
    if (texture == texture_view_ref_counts.end()) {
        throw std::runtime_error("cannot untrack an unknown bindless texture");
    }
    const auto view = texture->second.find(key);
    if (view == texture->second.end() || view->second == 0) {
        throw std::runtime_error("cannot untrack an unknown bindless texture view");
    }
    if (view->second == 1) {
        texture->second.erase(view);
        if (texture->second.empty()) {
            texture_view_ref_counts.erase(texture);
        }
    } else {
        --view->second;
    }
    texture_view_infos[_array_idx] = {};
}

UniquePtr<Command> VulkanBindlessArray::CreateUpdateCommand() {
    std::unique_lock<std::mutex> lk(mtx);

    Array<std::pair<uint, uint>> array_indices_dat;
    Array<byte>                  array_dat;
    Array<std::pair<uint, uint>> texture_indices_dat;
    Array<byte>                  texture_dat;
    Array<std::pair<uint, uint>> buffer_indices_dat;
    Array<byte>                  buffer_dat;

    {
        VulkanDescriptorHeap& heap   = m_device->GetGlobalDescriptorHeap();
        uint64 texture_handle_stride = heap.GetDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        uint64 buffer_handle_stride  = heap.GetDescriptorSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        uint64 array_handle_stride   = sizeof(uint);

        //update all cpu data here instead of during execution

        //handle update data first
        uint   array_idx            = 0;
        uint   texture_cnt          = 0;
        uint   buffer_cnt           = 0;
        uint64 update_command_token = 0;
        if (!update_cmds.empty()) {
            if (next_update_command_token == std::numeric_limits<uint64>::max()) {
                throw std::runtime_error("bindless update command token exhausted");
            }
            update_command_token = ++next_update_command_token;
        }

        // Snapshot each pending mutation under one command identity.
        for (UpdateCmd& cmd : update_cmds) {
            std::visit(
                [&](auto& _cmd) {
                    using Tcmd = std::decay_t<decltype(_cmd)>;
                    if constexpr (std::is_same_v<Tcmd, TextureUpdateInfo>) {
                        _cmd.command_token = update_command_token;
                        if (_cmd.free) {
                            auto& handle = handles[_cmd.array_idx];
                            assert(
                                handle.type == VulkanBindlessArray::Texture && handle.Ptr() &&
                                "Invalid texture handle"
                            );
                            assert(
                                uint64(_cmd.texture.Get()) == handle.Ptr() &&
                                "Bindless texture release lost resource ownership"
                            );

                            handle = Handle(0ull, 0, 0, 0);

                        } else {
                            ++array_idx;
                            ++texture_cnt;
                        }

                    } else if constexpr (std::is_same_v<Tcmd, BufferUpdateInfo>) {
                        _cmd.command_token = update_command_token;
                        if (_cmd.free) {
                            auto& handle = handles[_cmd.array_idx];
                            assert(
                                handle.type == VulkanBindlessArray::Buffer && handle.Ptr() &&
                                "Invalid buffer handle"
                            );
                            assert(
                                uint64(_cmd.buffer.Get()) == handle.Ptr() &&
                                "Bindless buffer release lost resource ownership"
                            );

                            handle = Handle(0ull, 0, 0, 0);

                        } else {
                            ++array_idx;
                            ++buffer_cnt;
                        }
                    }
                },
                cmd
            );
        }

        array_indices_dat.resize(array_idx);
        texture_indices_dat.resize(texture_cnt);
        buffer_indices_dat.resize(buffer_cnt);

        array_dat.resize(array_idx * array_handle_stride);
        texture_dat.resize(texture_cnt * texture_handle_stride);
        buffer_dat.resize(buffer_cnt * buffer_handle_stride);

        array_idx   = 0;
        texture_cnt = 0;
        buffer_cnt  = 0;

        // Buffer binding 0 is addressed through a view that starts at the
        // implementation-provided byte offset. Slots therefore remain
        // relative to that binding and never truncate byte padding into a
        // descriptor index.
        // The index table stores uint handles; the separate buffer stores descriptors.
        for (const UpdateCmd& cmd : update_cmds) {
            std::visit(
                [&](auto&& _cmd) {
                    using Tcmd = std::decay_t<decltype(_cmd)>;
                    if constexpr (std::is_same_v<Tcmd, TextureUpdateInfo>) {
                        if (!_cmd.free) {
                            handles[_cmd.array_idx] = Handle(
                                (uint64)_cmd.texture.Get(), _cmd.slot, 1, VulkanBindlessArray::Texture
                            );
                            VulkanTexture* vk_texture = ResourceCast(_cmd.texture.Get());
                            TextureView    view(vk_texture, _cmd.format, _cmd.mip_level, _cmd.num_mips);
                            if (_cmd.array_count > 0) {
                                view = view.Slice(_cmd.array_layer, _cmd.array_count);
                            }

                            VkDescriptorImageInfo native_image{};
                            native_image.imageView = vk_texture->GetView(
                                view.mip_level,
                                view.num_mips,
                                view.array_layer,
                                view.num_array ? view.num_array : 1
                            );
                            native_image.imageLayout = ResolveVulkanTextureState(
                                ETextureState::SAMPLE, EPassType::Graphics, false, vk_texture->GetAspectFlags()
                            ).layout;
                            VkWriteDescriptorSet native_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                            native_write.dstSet          = GetLegacySet(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
                            native_write.dstArrayElement = _cmd.slot;
                            native_write.descriptorCount = 1;
                            native_write.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                            native_write.pImageInfo      = &native_image;
                            // Slots are unpublished until the GPU index-table update; retirement
                            // prevents overwriting descriptors still reachable by earlier draws.
                            vkUpdateDescriptorSets(m_device->GetDevice(), 1, &native_write, 0, nullptr);
                            if (m_device->GetDescriptorMode() != VulkanDescriptorMode::Sets) {
                                const auto descriptor =
                                    heap.GetOrCreateImageDescriptor(&view, native_image.imageLayout);
                                heap.CopyDescriptorToMemory(
                                    descriptor,
                                    std::span<byte>(texture_dat)
                                        .subspan(texture_cnt * texture_handle_stride, texture_handle_stride)
                                );
                            }
                            uint indirect_handle = (m_device->GetSamplerIndex(_cmd.sampler) & 0xff) |
                                                   (_cmd.slot & 0xffffff) << 8;
                            memcpy(
                                array_dat.data() + array_idx * array_handle_stride,
                                &indirect_handle,
                                array_handle_stride
                            );
                            array_indices_dat[array_idx]     = {array_idx, _cmd.array_idx};
                            texture_indices_dat[texture_cnt] = {texture_cnt, _cmd.slot};

                            ++array_idx;
                            ++texture_cnt;
                        }

                    } else if constexpr (std::is_same_v<Tcmd, BufferUpdateInfo>) {
                        if (!_cmd.free) {
                            handles[_cmd.array_idx] =
                                Handle((uint64)_cmd.buffer.Get(), _cmd.slot, 0, VulkanBindlessArray::Buffer);
                            VulkanBuffer*    vk_buffer = ResourceCast(_cmd.buffer.Get());
                            VkDescriptorType type      = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                            if (_cmd.format != PF_UNDEFINED) {
                                LOG_WARNING(
                                    "Bindless buffer with format is not supported yet, current buffer: {}",
                                    _cmd.buffer->GetName()
                                );
                            }
                            VkDescriptorBufferInfo native_buffer{
                                vk_buffer->GetHandle(), 0, vk_buffer->GetByteSize()
                            };
                            VkWriteDescriptorSet native_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                            native_write.dstSet          = GetLegacySet(type);
                            native_write.dstArrayElement = _cmd.slot;
                            native_write.descriptorCount = 1;
                            native_write.descriptorType  = type;
                            native_write.pBufferInfo     = &native_buffer;
                            vkUpdateDescriptorSets(m_device->GetDevice(), 1, &native_write, 0, nullptr);
                            if (m_device->GetDescriptorMode() != VulkanDescriptorMode::Sets) {
                                const auto descriptor =
                                    heap.GetOrCreateBufferDescriptor(vk_buffer->GetView(_cmd.format), type);
                                heap.CopyDescriptorToMemory(
                                    descriptor,
                                    std::span<byte>(buffer_dat)
                                        .subspan(buffer_cnt * buffer_handle_stride, buffer_handle_stride)
                                );
                            }
                            memcpy(
                                array_dat.data() + array_idx * array_handle_stride, &_cmd.slot, sizeof(uint)
                            );
                            array_indices_dat[array_idx]   = {array_idx, _cmd.array_idx};
                            buffer_indices_dat[buffer_cnt] = {buffer_cnt, _cmd.slot};

                            ++array_idx;
                            ++buffer_cnt;
                        }
                    }
                },
                cmd
            );
        }
    }
    if (m_device->GetDescriptorMode() == VulkanDescriptorMode::Sets) {
        texture_indices_dat.clear();
        texture_dat.clear();
        buffer_indices_dat.clear();
        buffer_dat.clear();
    }
    temp_slot_to_cmd.clear();
    UniquePtr<Command> cmd = MakeUnique<UpdateBindlessArrayCmd>(
        this,
        std::move(update_cmds),
        std::move(array_dat),
        std::move(array_indices_dat),
        std::move(buffer_dat),
        std::move(buffer_indices_dat),
        std::move(texture_dat),
        std::move(texture_indices_dat)
    );

    update_cmds.clear();
    return cmd;
}

uint VulkanBindlessArray::AllocateTexture(const TextureView& _texture, Sampler _sampler) {
    std::unique_lock<std::mutex> lk(mtx);
    const uint                   capacity              = static_cast<uint>(handles.size());
    const uint                   reused_array_slot     = free_slots.Pop();
    const uint                   reused_texture_slot   = free_texture_slots.Pop();
    auto                         rollback_reused_slots = [&] {
        if (reused_array_slot != 0 && reused_array_slot < capacity) {
            free_slots.Push(reused_array_slot);
        }
        if (reused_texture_slot != 0 && reused_texture_slot < capacity) {
            free_texture_slots.Push(reused_texture_slot);
        }
    };
    if ((reused_array_slot != 0 && reused_array_slot >= capacity) ||
        (reused_texture_slot != 0 && reused_texture_slot >= capacity)) {
        rollback_reused_slots();
        throw std::runtime_error("bindless texture free-list slot is outside capacity");
    }
    if ((reused_array_slot == 0 && slot_offset >= capacity) ||
        (reused_texture_slot == 0 && texture_slot_offset >= capacity)) {
        rollback_reused_slots();
        throw std::runtime_error("bindless texture capacity exhausted");
    }

    const uint slot_idx     = reused_array_slot == 0 ? slot_offset++ : reused_array_slot;
    const uint texture_slot = reused_texture_slot == 0 ? texture_slot_offset++ : reused_texture_slot;
    if (array_slot_generations[slot_idx] == std::numeric_limits<uint64>::max() ||
        texture_slot_generations[texture_slot] == std::numeric_limits<uint64>::max()) {
        free_slots.Push(slot_idx);
        free_texture_slots.Push(texture_slot);
        throw std::runtime_error("bindless texture slot generation exhausted");
    }
    const uint64 array_generation           = ++array_slot_generations[slot_idx];
    const uint64 texture_generation         = ++texture_slot_generations[texture_slot];
    array_slot_claim_tokens[slot_idx]       = 0;
    texture_slot_claim_tokens[texture_slot] = 0;

    update_cmds.emplace_back(
        TextureUpdateInfo{
            _texture.texture,
            _sampler,
            _texture.format,
            slot_idx,
            texture_slot,
            _texture.mip_level,
            _texture.num_mips,
            _texture.array_layer,
            _texture.num_array,
            array_generation,
            texture_generation,
            0,
            false
        }
    );
    temp_slot_to_cmd[slot_idx] = update_cmds.size() - 1;
    // textures_allocated.emplace_back(_texture.texture, _sampler, _texture.format, slot_idx, texture_slot, _texture.mip_level, _texture.num_mips);
    TrackResource(_texture.texture);
    TrackTextureView(slot_idx, _texture);
    return slot_idx;
}

uint VulkanBindlessArray::AllocateBuffer(BufferView _buffer) {
    std::unique_lock<std::mutex> lk(mtx);
    const uint                   capacity              = static_cast<uint>(handles.size());
    const uint                   reused_array_slot     = free_slots.Pop();
    const uint                   reused_buffer_slot    = free_buffer_slots.Pop();
    auto                         rollback_reused_slots = [&] {
        if (reused_array_slot != 0 && reused_array_slot < capacity) {
            free_slots.Push(reused_array_slot);
        }
        if (reused_buffer_slot != 0 && reused_buffer_slot < capacity) {
            free_buffer_slots.Push(reused_buffer_slot);
        }
    };
    if ((reused_array_slot != 0 && reused_array_slot >= capacity) ||
        (reused_buffer_slot != 0 && reused_buffer_slot >= capacity)) {
        rollback_reused_slots();
        throw std::runtime_error("bindless buffer free-list slot is outside capacity");
    }
    if ((reused_array_slot == 0 && slot_offset >= capacity) ||
        (reused_buffer_slot == 0 && buffer_slot_offset >= capacity)) {
        rollback_reused_slots();
        throw std::runtime_error("bindless buffer capacity exhausted");
    }

    const uint slot_idx    = reused_array_slot == 0 ? slot_offset++ : reused_array_slot;
    const uint buffer_slot = reused_buffer_slot == 0 ? buffer_slot_offset++ : reused_buffer_slot;
    if (array_slot_generations[slot_idx] == std::numeric_limits<uint64>::max() ||
        buffer_slot_generations[buffer_slot] == std::numeric_limits<uint64>::max()) {
        free_slots.Push(slot_idx);
        free_buffer_slots.Push(buffer_slot);
        throw std::runtime_error("bindless buffer slot generation exhausted");
    }
    const uint64 array_generation         = ++array_slot_generations[slot_idx];
    const uint64 buffer_generation        = ++buffer_slot_generations[buffer_slot];
    array_slot_claim_tokens[slot_idx]     = 0;
    buffer_slot_claim_tokens[buffer_slot] = 0;
    update_cmds.emplace_back(
        BufferUpdateInfo{
            _buffer.buffer,
            slot_idx,
            buffer_slot,
            _buffer.format,
            array_generation,
            buffer_generation,
            0,
            false
        }
    );
    temp_slot_to_cmd[slot_idx] = update_cmds.size() - 1;
    // buffers_allocated.emplace_back(_buffer.buffer, slot_idx, buffer_slot);
    TrackResource(_buffer.buffer);
    return slot_idx;
}
static const Sampler s_spl = {ESamplerFilter::SF_LINEAR, SAM_CLAMP_TO_BORDER};
void                 VulkanBindlessArray::UnbindTexture(uint _array_idx) {
    std::unique_lock<std::mutex> lk(mtx);
    // Entries in this map have not been captured by CreateUpdateCommand yet;
    // cancelling an allocation here is normal while the initial extent settles.
    if (auto iter = temp_slot_to_cmd.find(_array_idx); iter != temp_slot_to_cmd.end()) {
        const UpdateCmd& src_cmd = update_cmds[iter->second];

        std::visit(
            [&](auto&& _cmd) {
                using TCmd = std::decay_t<decltype(_cmd)>;
                if constexpr (std::is_same_v<TCmd, TextureUpdateInfo>) {
                    if (_cmd.free) {
                        LOG_WARNING(
                            "You are releasing a bindless texture that's pending releasing, array index: {}",
                            _array_idx
                        );
                        return;
                    } else {
                        if (array_slot_generations[_cmd.array_idx] != _cmd.array_generation ||
                            texture_slot_generations[_cmd.slot] != _cmd.slot_generation ||
                            array_slot_claim_tokens[_cmd.array_idx] != 0 ||
                            texture_slot_claim_tokens[_cmd.slot] != 0) {
                            throw std::runtime_error(
                                "pending bindless texture cancellation lost slot ownership"
                            );
                        }
                        if (array_slot_generations[_cmd.array_idx] == std::numeric_limits<uint64>::max() ||
                            texture_slot_generations[_cmd.slot] == std::numeric_limits<uint64>::max()) {
                            throw std::runtime_error(
                                "pending bindless texture cancellation generation exhausted"
                            );
                        }
                        ++array_slot_generations[_cmd.array_idx];
                        ++texture_slot_generations[_cmd.slot];
                        UntrackResource((uint64)_cmd.texture.Get());
                        UntrackTextureView(_array_idx);
                        free_slots.Push(_cmd.array_idx);
                        free_texture_slots.Push(_cmd.slot);

                        update_cmds[iter->second] = InvalidUpdateInfo{_array_idx};
                        LOG_DEBUG(
                            "Cancelled a pending bindless texture allocation before submission, array index: "
                                            "{}",
                            _array_idx
                        );
                    }
                } else if constexpr (std::is_same_v<TCmd, BufferUpdateInfo>) {
                    if (_cmd.free) {
                        LOG_WARNING(
                            "You are releasing a bindless buffer that's pending releasing, array index: {}",
                            _array_idx
                        );
                        return;
                    } else {
                        if (array_slot_generations[_cmd.array_idx] != _cmd.array_generation ||
                            buffer_slot_generations[_cmd.slot] != _cmd.slot_generation ||
                            array_slot_claim_tokens[_cmd.array_idx] != 0 ||
                            buffer_slot_claim_tokens[_cmd.slot] != 0) {
                            throw std::runtime_error(
                                "pending bindless buffer cancellation lost slot ownership"
                            );
                        }
                        if (array_slot_generations[_cmd.array_idx] == std::numeric_limits<uint64>::max() ||
                            buffer_slot_generations[_cmd.slot] == std::numeric_limits<uint64>::max()) {
                            throw std::runtime_error(
                                "pending bindless buffer cancellation generation exhausted"
                            );
                        }
                        ++array_slot_generations[_cmd.array_idx];
                        ++buffer_slot_generations[_cmd.slot];
                        UntrackResource((uint64)_cmd.buffer.Get());
                        free_slots.Push(_cmd.array_idx);
                        free_buffer_slots.Push(_cmd.slot);
                        update_cmds[iter->second] = InvalidUpdateInfo{_array_idx};
                        LOG_DEBUG(
                            "Cancelled a pending bindless buffer allocation before submission, array index: "
                                            "{}",
                            _array_idx
                        );
                    }
                } else if constexpr (std::is_same_v<TCmd, InvalidUpdateInfo>) {
                }
            },
            src_cmd
        );
        return;
    }

    if (_array_idx >= handles.size()) {
        LOG_WARNING("You are releasing an out-of-range bindless texture, array index: {}", _array_idx);
        return;
    }
    const auto& handle = handles[_array_idx];
    if (handle.Ptr() == 0) {
        LOG_WARNING("You are releasing an empty bindless texture slot, array index: {}", _array_idx);
        return;
    }
    if (handle.type == Texture) {
        update_cmds.emplace_back(
            TextureUpdateInfo{
                TextureRef(reinterpret_cast<VulkanTexture*>(handle.Ptr())),
                s_spl,
                PF_UNDEFINED,
                _array_idx,
                handle.slot,
                0,
                0,
                0,
                0,
                array_slot_generations[_array_idx],
                texture_slot_generations[handle.slot],
                0,
                true
            }
        );
    } else if (handle.type == Buffer) {
        update_cmds.emplace_back(
            BufferUpdateInfo{
                BufferRef(reinterpret_cast<VulkanBuffer*>(handle.Ptr())),
                _array_idx,
                handle.slot,
                PF_UNDEFINED,
                array_slot_generations[_array_idx],
                buffer_slot_generations[handle.slot],
                0,
                true
            }
        );
    } else {
        LOG_WARNING("You are releasing an empty bindless texture slot, array index: {}", _array_idx);
        return;
    }
    temp_slot_to_cmd[_array_idx] = update_cmds.size() - 1;
}

void VulkanBindlessArray::UnbindBuffer(uint _array_idx) {
    std::unique_lock<std::mutex> lk(mtx);

    if (auto iter = temp_slot_to_cmd.find(_array_idx); iter != temp_slot_to_cmd.end()) {
        const UpdateCmd& src_cmd = update_cmds[iter->second];

        std::visit(
            [&](auto&& _cmd) {
                using TCmd = std::decay_t<decltype(_cmd)>;
                if constexpr (std::is_same_v<TCmd, TextureUpdateInfo>) {
                    if (_cmd.free) {
                        LOG_WARNING(
                            "You are releasing a bindless texture that's pending releasing, array index: {}",
                            _array_idx
                        );
                        return;
                    } else {
                        if (array_slot_generations[_cmd.array_idx] != _cmd.array_generation ||
                            texture_slot_generations[_cmd.slot] != _cmd.slot_generation ||
                            array_slot_claim_tokens[_cmd.array_idx] != 0 ||
                            texture_slot_claim_tokens[_cmd.slot] != 0) {
                            throw std::runtime_error(
                                "pending bindless texture cancellation lost slot ownership"
                            );
                        }
                        if (array_slot_generations[_cmd.array_idx] == std::numeric_limits<uint64>::max() ||
                            texture_slot_generations[_cmd.slot] == std::numeric_limits<uint64>::max()) {
                            throw std::runtime_error(
                                "pending bindless texture cancellation generation exhausted"
                            );
                        }
                        ++array_slot_generations[_cmd.array_idx];
                        ++texture_slot_generations[_cmd.slot];
                        UntrackResource((uint64)_cmd.texture.Get());
                        UntrackTextureView(_array_idx);
                        free_slots.Push(_cmd.array_idx);
                        free_texture_slots.Push(_cmd.slot);
                        update_cmds[iter->second] = InvalidUpdateInfo{_array_idx};
                        LOG_DEBUG(
                            "Cancelled a pending bindless texture allocation before submission, array index: "
                            "{}",
                            _array_idx
                        );
                    }
                } else if constexpr (std::is_same_v<TCmd, BufferUpdateInfo>) {
                    if (_cmd.free) {
                        LOG_WARNING(
                            "You are releasing a bindless buffer that's pending releasing, array index: {}",
                            _array_idx
                        );
                        return;
                    } else {
                        if (array_slot_generations[_cmd.array_idx] != _cmd.array_generation ||
                            buffer_slot_generations[_cmd.slot] != _cmd.slot_generation ||
                            array_slot_claim_tokens[_cmd.array_idx] != 0 ||
                            buffer_slot_claim_tokens[_cmd.slot] != 0) {
                            throw std::runtime_error(
                                "pending bindless buffer cancellation lost slot ownership"
                            );
                        }
                        if (array_slot_generations[_cmd.array_idx] == std::numeric_limits<uint64>::max() ||
                            buffer_slot_generations[_cmd.slot] == std::numeric_limits<uint64>::max()) {
                            throw std::runtime_error(
                                "pending bindless buffer cancellation generation exhausted"
                            );
                        }
                        ++array_slot_generations[_cmd.array_idx];
                        ++buffer_slot_generations[_cmd.slot];
                        UntrackResource((uint64)_cmd.buffer.Get());
                        free_slots.Push(_cmd.array_idx);
                        free_buffer_slots.Push(_cmd.slot);
                        update_cmds[iter->second] = InvalidUpdateInfo{_array_idx};
                        LOG_DEBUG(
                            "Cancelled a pending bindless buffer allocation before submission, array index: "
                            "{}",
                            _array_idx
                        );
                    }
                } else if constexpr (std::is_same_v<TCmd, InvalidUpdateInfo>) {
                }
            },
            src_cmd
        );
        return;
    }

    if (_array_idx >= handles.size()) {
        LOG_WARNING("You are releasing an out-of-range bindless buffer, array index: {}", _array_idx);
        return;
    }
    const auto& handle = handles[_array_idx];
    if (handle.Ptr() == 0) {
        LOG_WARNING("You are releasing an empty bindless buffer slot, array index: {}", _array_idx);
        return;
    }
    if (handle.type == Texture) {
        // textures_freed.push_back(handle.slot);
        update_cmds.emplace_back(
            TextureUpdateInfo{
                TextureRef(reinterpret_cast<VulkanTexture*>(handle.Ptr())),
                s_spl,
                PF_UNDEFINED,
                _array_idx,
                handle.slot,
                0,
                0,
                0,
                0,
                array_slot_generations[_array_idx],
                texture_slot_generations[handle.slot],
                0,
                true
            }
        );
    } else if (handle.type == Buffer) {
        // buffers_freed.push_back(handle.slot);
        update_cmds.emplace_back(
            BufferUpdateInfo{
                BufferRef(reinterpret_cast<VulkanBuffer*>(handle.Ptr())),
                _array_idx,
                handle.slot,
                PF_UNDEFINED,
                array_slot_generations[_array_idx],
                buffer_slot_generations[handle.slot],
                0,
                true
            }
        );
    } else {
        LOG_WARNING("You are releasing an empty bindless buffer slot, array index: {}", _array_idx);
        return;
    }
    temp_slot_to_cmd[_array_idx] = update_cmds.size() - 1;
}

bool VulkanBindlessArray::IsResourceAllocated(uint64 _resource) const {
    return resource_allocation_counts.find(_resource) != resource_allocation_counts.end();
}

VulkanBindlessArray::PresentationSourceMembershipSnapshot
VulkanBindlessArray::SnapshotAcceptedPresentationSourceMembership() const noexcept {
    return std::atomic_load_explicit(&accepted_presentation_source_membership, std::memory_order_acquire);
}

void VulkanBindlessArray::PublishAcceptedPresentationSourceMembership(
    PresentationSourceMembershipSnapshot _membership
) noexcept {
    assert(_membership);
    std::atomic_store_explicit(
        &accepted_presentation_source_membership, std::move(_membership), std::memory_order_release
    );
}

bool VulkanBindlessArray::IsTextureViewAllocated(
    uint64 _texture,
    uint8  _mip_level,
    uint8  _mip_count,
    uint8  _array_layer,
    uint8  _array_count
) const {
    auto it = texture_view_ref_counts.find(_texture);
    if (it == texture_view_ref_counts.end()) {
        return false;
    }
    auto*  vk_texture = reinterpret_cast<VulkanTexture*>(_texture);
    uint32 mip_level  = _mip_level;
    uint32 mip_count =
        _mip_count == kRemainingSubresource ? (vk_texture->GetNumMips() - mip_level) : _mip_count;
    uint32 array_layer = _array_layer;
    uint32 array_count =
        _array_count == kRemainingSubresource ? (vk_texture->GetNumArray() - array_layer) : _array_count;
    for (const auto& [view, ref_count] : it->second) {
        (void)ref_count;
        if (RangeOverlap(view.mip_level, view.mip_count, mip_level, mip_count) &&
            RangeOverlap(view.array_layer, view.array_count, array_layer, array_count)) {
            return true;
        }
    }
    return false;
}

namespace {

uint64 ValidateUpdateCommandToken(const Array<BindlessArray::UpdateCmd>& updates) {
    using TextureUpdateInfo = BindlessArray::TextureUpdateInfo;
    using BufferUpdateInfo  = BindlessArray::BufferUpdateInfo;
    uint64 command_token    = 0;
    for (const BindlessArray::UpdateCmd& update : updates) {
        std::visit(
            [&](const auto& _cmd) {
                using TCmd = std::decay_t<decltype(_cmd)>;
                if constexpr (std::is_same_v<TCmd, TextureUpdateInfo> ||
                              std::is_same_v<TCmd, BufferUpdateInfo>) {
                    if (_cmd.command_token == 0) {
                        throw std::runtime_error("bindless update command has no lifecycle token");
                    }
                    if (command_token == 0) {
                        command_token = _cmd.command_token;
                    } else if (command_token != _cmd.command_token) {
                        throw std::runtime_error("bindless update command mixes lifecycle tokens");
                    }
                }
            },
            update
        );
    }
    return command_token;
}

} // namespace

bool VulkanBindlessArray::BeginUpdateCommand(const Array<BindlessArray::UpdateCmd>& _updates) {
    std::unique_lock<std::mutex> lk(mtx);

    const uint64 command_token = ValidateUpdateCommandToken(_updates);

    auto generation_matches = [&](const auto& _cmd) {
        using TCmd = std::decay_t<decltype(_cmd)>;
        if constexpr (std::is_same_v<TCmd, TextureUpdateInfo>) {
            return _cmd.texture && _cmd.array_idx != 0 && _cmd.array_idx < array_slot_generations.size() &&
                   _cmd.slot != 0 && _cmd.slot < texture_slot_generations.size() &&
                   ClassifyBindlessUpdateSlot(
                       array_slot_generations[_cmd.array_idx],
                       texture_slot_generations[_cmd.slot],
                       array_slot_claim_tokens[_cmd.array_idx],
                       texture_slot_claim_tokens[_cmd.slot],
                       _cmd.array_generation,
                       _cmd.slot_generation,
                       _cmd.command_token
                   ) == BindlessUpdateSlotState::Original;
        } else if constexpr (std::is_same_v<TCmd, BufferUpdateInfo>) {
            return _cmd.buffer && _cmd.array_idx != 0 && _cmd.array_idx < array_slot_generations.size() &&
                   _cmd.slot != 0 && _cmd.slot < buffer_slot_generations.size() &&
                   ClassifyBindlessUpdateSlot(
                       array_slot_generations[_cmd.array_idx],
                       buffer_slot_generations[_cmd.slot],
                       array_slot_claim_tokens[_cmd.array_idx],
                       buffer_slot_claim_tokens[_cmd.slot],
                       _cmd.array_generation,
                       _cmd.slot_generation,
                       _cmd.command_token
                   ) == BindlessUpdateSlotState::Original;
        } else {
            return true;
        }
    };

    bool all_current = true;
    for (const BindlessArray::UpdateCmd& update : _updates) {
        std::visit(
            [&](const auto& _cmd) {
                all_current &= generation_matches(_cmd);
            },
            update
        );
    }

    UnorderedSet<uint> global_claims;
    UnorderedSet<uint> texture_claims;
    UnorderedSet<uint> buffer_claims;
    for (const BindlessArray::UpdateCmd& update : _updates) {
        std::visit(
            [&](const auto& _cmd) {
                using TCmd = std::decay_t<decltype(_cmd)>;
                if constexpr (std::is_same_v<TCmd, TextureUpdateInfo>) {
                    if (!generation_matches(_cmd) || (!_cmd.free && all_current)) {
                        return;
                    }
                    if (array_slot_generations[_cmd.array_idx] == std::numeric_limits<uint64>::max() ||
                        texture_slot_generations[_cmd.slot] == std::numeric_limits<uint64>::max()) {
                        throw std::runtime_error("bindless texture update generation exhausted");
                    }
                    if (array_slot_claim_tokens[_cmd.array_idx] != 0 ||
                        texture_slot_claim_tokens[_cmd.slot] != 0) {
                        throw std::runtime_error("bindless texture slot already has a lifecycle claim");
                    }
                    if (!global_claims.emplace(_cmd.array_idx).second ||
                        !texture_claims.emplace(_cmd.slot).second) {
                        throw std::runtime_error("duplicate bindless texture update generation claim");
                    }
                } else if constexpr (std::is_same_v<TCmd, BufferUpdateInfo>) {
                    if (!generation_matches(_cmd) || (!_cmd.free && all_current)) {
                        return;
                    }
                    if (array_slot_generations[_cmd.array_idx] == std::numeric_limits<uint64>::max() ||
                        buffer_slot_generations[_cmd.slot] == std::numeric_limits<uint64>::max()) {
                        throw std::runtime_error("bindless buffer update generation exhausted");
                    }
                    if (array_slot_claim_tokens[_cmd.array_idx] != 0 ||
                        buffer_slot_claim_tokens[_cmd.slot] != 0) {
                        throw std::runtime_error("bindless buffer slot already has a lifecycle claim");
                    }
                    if (!global_claims.emplace(_cmd.array_idx).second ||
                        !buffer_claims.emplace(_cmd.slot).second) {
                        throw std::runtime_error("duplicate bindless buffer update generation claim");
                    }
                }
            },
            update
        );
    }
    ApplySlotClaims(global_claims, texture_claims, buffer_claims, command_token);
    return all_current;
}

struct VulkanBindlessArray::UpdateCompletionPlan {
    Array<std::pair<const TextureUpdateInfo*, bool>> texture_restores;
    Array<std::pair<const BufferUpdateInfo*, bool>>  buffer_restores;
    Array<const TextureUpdateInfo*>                  texture_retirements;
    Array<const BufferUpdateInfo*>                   buffer_retirements;
    UnorderedMap<uint64, uint32>                     resource_decrements;
    UnorderedMap<TextureSubresourceKeyT<VulkanTexture>, uint32, TextureSubresourceKeyHashT<VulkanTexture>>
                       view_decrements;
    UnorderedSet<uint> global_claims;
    UnorderedSet<uint> texture_claims;
    UnorderedSet<uint> buffer_claims;
};

void VulkanBindlessArray::FinalizeUpdateCommand(
    const Array<UpdateCmd>& _updates,
    bool                    _gpu_update_succeeded
) {
    std::unique_lock<std::mutex> lock(mtx);
    const uint64                 command_token = ValidateUpdateCommandToken(_updates);
    if (command_token == 0) {
        return;
    }
    const UpdateCompletionPlan plan = BuildFinalizationPlan(_updates, _gpu_update_succeeded);
    ValidateReferenceDecrements(plan);
    ApplyCompletionPlan(plan, command_token);
}

void VulkanBindlessArray::DiscardUpdateCommand(const Array<UpdateCmd>& _updates) {
    std::unique_lock<std::mutex> lock(mtx);
    const uint64                 command_token = ValidateUpdateCommandToken(_updates);
    if (command_token == 0) {
        return;
    }
    const UpdateCompletionPlan plan = BuildDiscardPlan(_updates);
    ValidateReferenceDecrements(plan);
    ApplyCompletionPlan(plan, command_token);
}

VulkanBindlessArray::UpdateCompletionPlan
VulkanBindlessArray::BuildFinalizationPlan(const Array<UpdateCmd>& updates, bool gpu_update_succeeded) const {
    UpdateCompletionPlan plan;
    UnorderedSet<uint>   global_slots;
    UnorderedSet<uint>   texture_slots;
    UnorderedSet<uint>   buffer_slots;

    auto reserve_slot_mutation = [&](const auto& _cmd, bool _texture) {
        if (!global_slots.emplace(_cmd.array_idx).second) {
            throw std::runtime_error("duplicate pending bindless global-slot mutation");
        }
        if (_texture) {
            if (!texture_slots.emplace(_cmd.slot).second) {
                throw std::runtime_error("duplicate pending bindless texture-slot mutation");
            }
        } else if (!buffer_slots.emplace(_cmd.slot).second) {
            throw std::runtime_error("duplicate pending bindless buffer-slot mutation");
        }
    };

    auto reserve_claim = [&](const auto& _cmd, bool _texture) {
        if (array_slot_generations[_cmd.array_idx] == std::numeric_limits<uint64>::max()) {
            throw std::runtime_error("bindless global-slot generation exhausted");
        }
        if (!plan.global_claims.emplace(_cmd.array_idx).second) {
            throw std::runtime_error("duplicate pending bindless global-slot claim");
        }
        if (_texture) {
            if (texture_slot_generations[_cmd.slot] == std::numeric_limits<uint64>::max()) {
                throw std::runtime_error("bindless texture-slot generation exhausted");
            }
            if (!plan.texture_claims.emplace(_cmd.slot).second) {
                throw std::runtime_error("duplicate pending bindless texture-slot claim");
            }
        } else {
            if (buffer_slot_generations[_cmd.slot] == std::numeric_limits<uint64>::max()) {
                throw std::runtime_error("bindless buffer-slot generation exhausted");
            }
            if (!plan.buffer_claims.emplace(_cmd.slot).second) {
                throw std::runtime_error("duplicate pending bindless buffer-slot claim");
            }
        }
    };

    // Validate the whole retirement transaction before changing membership
    // or exposing any slot to another producer.
    for (const BindlessArray::UpdateCmd& update : updates) {
        std::visit(
            [&](const auto& _cmd) {
                using TCmd = std::decay_t<decltype(_cmd)>;
                if constexpr (std::is_same_v<TCmd, TextureUpdateInfo>) {
                    if (!_cmd.texture || _cmd.array_idx == 0 || _cmd.array_idx >= handles.size() ||
                        _cmd.slot == 0 || _cmd.slot >= texture_slot_generations.size()) {
                        throw std::runtime_error("invalid pending bindless texture retirement");
                    }
                    const BindlessUpdateSlotState state = ClassifyBindlessUpdateSlot(
                        array_slot_generations[_cmd.array_idx],
                        texture_slot_generations[_cmd.slot],
                        array_slot_claim_tokens[_cmd.array_idx],
                        texture_slot_claim_tokens[_cmd.slot],
                        _cmd.array_generation,
                        _cmd.slot_generation,
                        _cmd.command_token
                    );
                    const BindlessUpdateFinalizationAction action =
                        GetBindlessUpdateFinalizationAction(state, _cmd.free, gpu_update_succeeded);
                    if (action == BindlessUpdateFinalizationAction::Ignore) {
                        return;
                    }

                    const Handle& handle = handles[_cmd.array_idx];
                    if (action == BindlessUpdateFinalizationAction::Restore) {
                        if (handle.Ptr() != 0 &&
                            (handle.Ptr() != uint64(_cmd.texture.Get()) || handle.slot != _cmd.slot ||
                             handle.type != VulkanBindlessArray::Texture)) {
                            throw std::runtime_error("rejected bindless texture release lost slot ownership");
                        }
                        const auto allocation = resource_allocation_counts.find(uint64(_cmd.texture.Get()));
                        if (allocation == resource_allocation_counts.end() || allocation->second.count == 0) {
                            throw std::runtime_error(
                                "rejected bindless texture release lost resource ownership"
                            );
                        }
                        const auto& key = texture_view_infos[_cmd.array_idx];
                        if (key.texture != ResourceCast(_cmd.texture.Get())) {
                            throw std::runtime_error("rejected bindless texture release lost its view");
                        }
                        const auto texture = texture_view_ref_counts.find(uint64(key.texture));
                        if (texture == texture_view_ref_counts.end()) {
                            throw std::runtime_error(
                                "rejected bindless texture release has no texture entry"
                            );
                        }
                        const auto view = texture->second.find(key);
                        if (view == texture->second.end() || view->second == 0) {
                            throw std::runtime_error(
                                "rejected bindless texture release has no view ownership"
                            );
                        }
                        reserve_slot_mutation(_cmd, true);
                        plan.texture_restores.emplace_back(
                            &_cmd, state == BindlessUpdateSlotState::ClaimedByCommand
                        );
                        return;
                    }
                    if (_cmd.free) {
                        if (handle.Ptr() != 0) {
                            throw std::runtime_error("pending bindless texture release slot is not empty");
                        }
                    } else if (handle.Ptr() != 0 &&
                               (handle.Ptr() != uint64(_cmd.texture.Get()) || handle.slot != _cmd.slot ||
                                handle.type != VulkanBindlessArray::Texture)) {
                        throw std::runtime_error("pending bindless texture cancellation lost slot ownership");
                    }
                    if (state == BindlessUpdateSlotState::Original) {
                        reserve_claim(_cmd, true);
                    }
                    reserve_slot_mutation(_cmd, true);
                    ++plan.resource_decrements[uint64(_cmd.texture.Get())];
                    const auto& key = texture_view_infos[_cmd.array_idx];
                    if (key.texture != ResourceCast(_cmd.texture.Get())) {
                        throw std::runtime_error("pending bindless texture release lost its view");
                    }
                    ++plan.view_decrements[key];
                    plan.texture_retirements.push_back(&_cmd);
                } else if constexpr (std::is_same_v<TCmd, BufferUpdateInfo>) {
                    if (!_cmd.buffer || _cmd.array_idx == 0 || _cmd.array_idx >= handles.size() ||
                        _cmd.slot == 0 || _cmd.slot >= buffer_slot_generations.size()) {
                        throw std::runtime_error("invalid pending bindless buffer retirement");
                    }
                    const BindlessUpdateSlotState state = ClassifyBindlessUpdateSlot(
                        array_slot_generations[_cmd.array_idx],
                        buffer_slot_generations[_cmd.slot],
                        array_slot_claim_tokens[_cmd.array_idx],
                        buffer_slot_claim_tokens[_cmd.slot],
                        _cmd.array_generation,
                        _cmd.slot_generation,
                        _cmd.command_token
                    );
                    const BindlessUpdateFinalizationAction action =
                        GetBindlessUpdateFinalizationAction(state, _cmd.free, gpu_update_succeeded);
                    if (action == BindlessUpdateFinalizationAction::Ignore) {
                        return;
                    }

                    const Handle& handle = handles[_cmd.array_idx];
                    if (action == BindlessUpdateFinalizationAction::Restore) {
                        if (handle.Ptr() != 0 &&
                            (handle.Ptr() != uint64(_cmd.buffer.Get()) || handle.slot != _cmd.slot ||
                             handle.type != VulkanBindlessArray::Buffer)) {
                            throw std::runtime_error("rejected bindless buffer release lost slot ownership");
                        }
                        const auto allocation = resource_allocation_counts.find(uint64(_cmd.buffer.Get()));
                        if (allocation == resource_allocation_counts.end() || allocation->second.count == 0) {
                            throw std::runtime_error(
                                "rejected bindless buffer release lost resource ownership"
                            );
                        }
                        reserve_slot_mutation(_cmd, false);
                        plan.buffer_restores.emplace_back(
                            &_cmd, state == BindlessUpdateSlotState::ClaimedByCommand
                        );
                        return;
                    }
                    if (_cmd.free) {
                        if (handle.Ptr() != 0) {
                            throw std::runtime_error("pending bindless buffer release slot is not empty");
                        }
                    } else if (handle.Ptr() != 0 &&
                               (handle.Ptr() != uint64(_cmd.buffer.Get()) || handle.slot != _cmd.slot ||
                                handle.type != VulkanBindlessArray::Buffer)) {
                        throw std::runtime_error("pending bindless buffer cancellation lost slot ownership");
                    }
                    if (state == BindlessUpdateSlotState::Original) {
                        reserve_claim(_cmd, false);
                    }
                    reserve_slot_mutation(_cmd, false);
                    ++plan.resource_decrements[uint64(_cmd.buffer.Get())];
                    plan.buffer_retirements.push_back(&_cmd);
                }
            },
            update
        );
    }

    return plan;
}

VulkanBindlessArray::UpdateCompletionPlan
VulkanBindlessArray::BuildDiscardPlan(const Array<UpdateCmd>& updates) const {
    UpdateCompletionPlan plan;
    UnorderedSet<uint>   global_slots;
    UnorderedSet<uint>   texture_slots;
    UnorderedSet<uint>   buffer_slots;

    auto reserve_slot = [&](const auto& _cmd, bool _texture) {
        if (!global_slots.emplace(_cmd.array_idx).second) {
            throw std::runtime_error("duplicate discarded bindless global-slot mutation");
        }
        if (_texture) {
            if (!texture_slots.emplace(_cmd.slot).second) {
                throw std::runtime_error("duplicate discarded bindless texture-slot mutation");
            }
        } else if (!buffer_slots.emplace(_cmd.slot).second) {
            throw std::runtime_error("duplicate discarded bindless buffer-slot mutation");
        }
    };

    auto reserve_claim = [&](const auto& _cmd, bool _texture) {
        if (array_slot_generations[_cmd.array_idx] == std::numeric_limits<uint64>::max()) {
            throw std::runtime_error("discarded bindless global-slot generation exhausted");
        }
        if (!plan.global_claims.emplace(_cmd.array_idx).second) {
            throw std::runtime_error("duplicate discarded bindless global-slot claim");
        }
        if (_texture) {
            if (texture_slot_generations[_cmd.slot] == std::numeric_limits<uint64>::max()) {
                throw std::runtime_error("discarded bindless texture-slot generation exhausted");
            }
            if (!plan.texture_claims.emplace(_cmd.slot).second) {
                throw std::runtime_error("duplicate discarded bindless texture-slot claim");
            }
        } else {
            if (buffer_slot_generations[_cmd.slot] == std::numeric_limits<uint64>::max()) {
                throw std::runtime_error("discarded bindless buffer-slot generation exhausted");
            }
            if (!plan.buffer_claims.emplace(_cmd.slot).second) {
                throw std::runtime_error("duplicate discarded bindless buffer-slot claim");
            }
        }
    };

    for (const BindlessArray::UpdateCmd& update : updates) {
        std::visit(
            [&](const auto& _cmd) {
                using TCmd = std::decay_t<decltype(_cmd)>;
                if constexpr (std::is_same_v<TCmd, TextureUpdateInfo>) {
                    if (!_cmd.texture || _cmd.array_idx == 0 || _cmd.array_idx >= handles.size() ||
                        _cmd.slot == 0 || _cmd.slot >= texture_slot_generations.size()) {
                        throw std::runtime_error("invalid discarded bindless texture update");
                    }
                    const BindlessUpdateSlotState state = ClassifyBindlessUpdateSlot(
                        array_slot_generations[_cmd.array_idx],
                        texture_slot_generations[_cmd.slot],
                        array_slot_claim_tokens[_cmd.array_idx],
                        texture_slot_claim_tokens[_cmd.slot],
                        _cmd.array_generation,
                        _cmd.slot_generation,
                        _cmd.command_token
                    );
                    if (state == BindlessUpdateSlotState::Stale) {
                        return;
                    }

                    const Handle& handle = handles[_cmd.array_idx];
                    if (_cmd.free) {
                        if (state != BindlessUpdateSlotState::Original) {
                            return;
                        }
                        if (handle.Ptr() != 0 &&
                            (handle.Ptr() != uint64(_cmd.texture.Get()) || handle.slot != _cmd.slot ||
                             handle.type != VulkanBindlessArray::Texture)) {
                            throw std::runtime_error(
                                "discarded bindless texture release lost slot ownership"
                            );
                        }
                        reserve_slot(_cmd, true);
                        plan.texture_restores.emplace_back(&_cmd, false);
                        return;
                    }

                    if (handle.Ptr() != 0 &&
                        (handle.Ptr() != uint64(_cmd.texture.Get()) || handle.slot != _cmd.slot ||
                         handle.type != VulkanBindlessArray::Texture)) {
                        throw std::runtime_error("discarded bindless texture allocation lost slot ownership");
                    }
                    reserve_slot(_cmd, true);
                    if (state == BindlessUpdateSlotState::Original) {
                        reserve_claim(_cmd, true);
                    }
                    ++plan.resource_decrements[uint64(_cmd.texture.Get())];
                    const auto& key = texture_view_infos[_cmd.array_idx];
                    if (key.texture != ResourceCast(_cmd.texture.Get())) {
                        throw std::runtime_error("discarded bindless texture allocation lost its view");
                    }
                    ++plan.view_decrements[key];
                    plan.texture_retirements.push_back(&_cmd);
                } else if constexpr (std::is_same_v<TCmd, BufferUpdateInfo>) {
                    if (!_cmd.buffer || _cmd.array_idx == 0 || _cmd.array_idx >= handles.size() ||
                        _cmd.slot == 0 || _cmd.slot >= buffer_slot_generations.size()) {
                        throw std::runtime_error("invalid discarded bindless buffer update");
                    }
                    const BindlessUpdateSlotState state = ClassifyBindlessUpdateSlot(
                        array_slot_generations[_cmd.array_idx],
                        buffer_slot_generations[_cmd.slot],
                        array_slot_claim_tokens[_cmd.array_idx],
                        buffer_slot_claim_tokens[_cmd.slot],
                        _cmd.array_generation,
                        _cmd.slot_generation,
                        _cmd.command_token
                    );
                    if (state == BindlessUpdateSlotState::Stale) {
                        return;
                    }

                    const Handle& handle = handles[_cmd.array_idx];
                    if (_cmd.free) {
                        if (state != BindlessUpdateSlotState::Original) {
                            return;
                        }
                        if (handle.Ptr() != 0 &&
                            (handle.Ptr() != uint64(_cmd.buffer.Get()) || handle.slot != _cmd.slot ||
                             handle.type != VulkanBindlessArray::Buffer)) {
                            throw std::runtime_error("discarded bindless buffer release lost slot ownership");
                        }
                        reserve_slot(_cmd, false);
                        plan.buffer_restores.emplace_back(&_cmd, false);
                        return;
                    }

                    if (handle.Ptr() != 0 &&
                        (handle.Ptr() != uint64(_cmd.buffer.Get()) || handle.slot != _cmd.slot ||
                         handle.type != VulkanBindlessArray::Buffer)) {
                        throw std::runtime_error("discarded bindless buffer allocation lost slot ownership");
                    }
                    reserve_slot(_cmd, false);
                    if (state == BindlessUpdateSlotState::Original) {
                        reserve_claim(_cmd, false);
                    }
                    ++plan.resource_decrements[uint64(_cmd.buffer.Get())];
                    plan.buffer_retirements.push_back(&_cmd);
                }
            },
            update
        );
    }

    return plan;
}

void VulkanBindlessArray::ValidateReferenceDecrements(const UpdateCompletionPlan& plan) const {
    for (const auto& [resource, decrement] : plan.resource_decrements) {
        const auto allocation = resource_allocation_counts.find(resource);
        if (allocation == resource_allocation_counts.end() || allocation->second.count < decrement) {
            throw std::runtime_error("bindless resource retirement exceeds its reference count");
        }
    }
    for (const auto& [key, decrement] : plan.view_decrements) {
        const auto texture = texture_view_ref_counts.find(uint64(key.texture));
        if (texture == texture_view_ref_counts.end()) {
            throw std::runtime_error("pending bindless texture retirement has no texture entry");
        }
        const auto view = texture->second.find(key);
        if (view == texture->second.end() || view->second < decrement) {
            throw std::runtime_error("bindless texture retirement exceeds its view reference count");
        }
    }
}

void VulkanBindlessArray::ApplySlotClaims(
    const UnorderedSet<uint>& array_slots,
    const UnorderedSet<uint>& texture_slots,
    const UnorderedSet<uint>& buffer_slots,
    uint64                    command_token
) {
    for (const uint slot : array_slots) {
        ++array_slot_generations[slot];
        array_slot_claim_tokens[slot] = command_token;
    }
    for (const uint slot : texture_slots) {
        ++texture_slot_generations[slot];
        texture_slot_claim_tokens[slot] = command_token;
    }
    for (const uint slot : buffer_slots) {
        ++buffer_slot_generations[slot];
        buffer_slot_claim_tokens[slot] = command_token;
    }
}

void VulkanBindlessArray::ApplyCompletionPlan(const UpdateCompletionPlan& plan, uint64 command_token) {
    // All entries and reference decrements have been validated under mtx.
    ApplySlotClaims(plan.global_claims, plan.texture_claims, plan.buffer_claims, command_token);

    // A rejected free never reached a GPU ordering point. Roll it back to
    // an active CPU binding instead of recycling the descriptor while
    // older submissions may still read it. This command is the only owner
    // allowed to undo its generation claim.
    for (const auto& [restore, claimed] : plan.texture_restores) {
        if (claimed && !RollbackBindlessUpdateSlotClaim(
                           array_slot_generations[restore->array_idx],
                           texture_slot_generations[restore->slot],
                           array_slot_claim_tokens[restore->array_idx],
                           texture_slot_claim_tokens[restore->slot],
                           restore->array_generation,
                           restore->slot_generation,
                           restore->command_token
                       )) {
            throw std::runtime_error("rejected bindless texture release lost its lifecycle claim");
        }
        handles[restore->array_idx] =
            Handle(uint64(restore->texture.Get()), restore->slot, 1, VulkanBindlessArray::Texture);
    }
    for (const auto& [restore, claimed] : plan.buffer_restores) {
        if (claimed && !RollbackBindlessUpdateSlotClaim(
                           array_slot_generations[restore->array_idx],
                           buffer_slot_generations[restore->slot],
                           array_slot_claim_tokens[restore->array_idx],
                           buffer_slot_claim_tokens[restore->slot],
                           restore->array_generation,
                           restore->slot_generation,
                           restore->command_token
                       )) {
            throw std::runtime_error("rejected bindless buffer release lost its lifecycle claim");
        }
        handles[restore->array_idx] =
            Handle(uint64(restore->buffer.Get()), restore->slot, 0, VulkanBindlessArray::Buffer);
    }

    for (const TextureUpdateInfo* retirement : plan.texture_retirements) {
        handles[retirement->array_idx] = Handle{};
        UntrackResource(uint64(retirement->texture.Get()));
        UntrackTextureView(retirement->array_idx);
    }
    for (const BufferUpdateInfo* retirement : plan.buffer_retirements) {
        handles[retirement->array_idx] = Handle{};
        UntrackResource(uint64(retirement->buffer.Get()));
    }
    // Publish free slots only after every membership change is complete.
    for (const TextureUpdateInfo* retirement : plan.texture_retirements) {
        free_slots.Push(retirement->array_idx);
        free_texture_slots.Push(retirement->slot);
    }
    for (const BufferUpdateInfo* retirement : plan.buffer_retirements) {
        free_slots.Push(retirement->array_idx);
        free_buffer_slots.Push(retirement->slot);
    }
}

VulkanBindlessArray::~VulkanBindlessArray() {
    if (heap_lease)
        g_heap.RecycleDescriptorBufferLease(std::move(heap_lease));
}

} // namespace Moer::Render
