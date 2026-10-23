#include "VulkanBindlessArray.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "log/LogSystem.h"
#include "misc/Alignment.h"
#include "misc/MMemory.h"
#include "rhi/RHIBindlessUpdateLifecycle.h"
#include "rhi/RHIExecutor.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace Moer::Render {

static constexpr std::string_view s_bindless_index_buffer_name       = "VkBindless::IndexTable";
static constexpr std::string_view s_bindless_buffer_descriptors_name = "VkBindless::BufferDescriptors";
static constexpr std::string_view s_bindless_texture_descriptors_name =
    "VkBindless::TextureAndSamplerDescriptors";

VulkanBindlessArray::VulkanBindlessArray(VulkanDevice* _device, uint32 _max_size) :
    BindlessArray(),
    VulkanDeviceObject(_device),
    bindless_buffer_descs(nullptr),
    bindless_texture_descs(nullptr),
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
    handles(_max_size),
    texture_view_infos(_max_size),
    texture_offset_in_buffer(GetBindlessTextureDescriptorOffset(*_device)),
    accepted_presentation_source_membership(std::make_shared<const PresentationSourceMembership>()) {
    const uint storage_buffer_descriptor_size = g_heap.GetDescriptorSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    const uint sampled_image_descriptor_size  = g_heap.GetDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);

    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
                                           VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfoEXT binding_flags{};
    binding_flags.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT;
    binding_flags.bindingCount  = 1;
    binding_flags.pBindingFlags = &flags;

    VkDescriptorSetLayoutBinding buffer_binding{};
    buffer_binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    buffer_binding.binding         = 0;
    buffer_binding.descriptorCount = _max_size;
    buffer_binding.stageFlags      = VK_SHADER_STAGE_ALL;

    VkDescriptorSetLayoutCreateInfo buffer_desc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    buffer_desc_info.bindingCount = 1;
    buffer_desc_info.pBindings    = &buffer_binding;
    buffer_desc_info.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    buffer_desc_info.pNext        = &binding_flags;

    VkDescriptorSetLayout buffer_desc_layout = VK_NULL_HANDLE;
    VK_CHECK_RESULT(vkCreateDescriptorSetLayout(
        m_device->GetDevice(), &buffer_desc_info, VK_NULL_HANDLE, &buffer_desc_layout
    ));
    VkDeviceSize bindless_buffer_descriptor_bytes = 0;
    vkGetDescriptorSetLayoutSizeEXT(
        m_device->GetDevice(), buffer_desc_layout, &bindless_buffer_descriptor_bytes
    );
    vkGetDescriptorSetLayoutBindingOffsetEXT(
        m_device->GetDevice(), buffer_desc_layout, 0, &buffer_descriptors_byte_offset
    );
    vkDestroyDescriptorSetLayout(m_device->GetDevice(), buffer_desc_layout, VK_NULL_HANDLE);

    const uint64 required_buffer_descriptor_bytes =
        buffer_descriptors_byte_offset + static_cast<uint64>(_max_size) * storage_buffer_descriptor_size;
    if (bindless_buffer_descriptor_bytes < required_buffer_descriptor_bytes) {
        throw std::runtime_error("bindless buffer descriptor layout is smaller than its bindings");
    }
    const uint64 bindless_texture_descriptor_bytes =
        texture_offset_in_buffer + static_cast<uint64>(_max_size) * sampled_image_descriptor_size;

    BufferInfo buffer_info(uint64(_max_size), uint(sizeof(uint)), EBufferUsageFlags::UNORDERED_ACCESS);
    VkBuffer   current_handle    = VK_NULL_HANDLE;
    VkBufferCreateInfo buffer_ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

    VmaAllocation alloc = VK_NULL_HANDLE;
    buffer_ci.size      = _max_size * sizeof(uint32);
    buffer_ci.usage     = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_ci.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
    buffer_ci.queueFamilyIndexCount = 0;
    buffer_ci.pQueueFamilyIndices   = nullptr;
    buffer_ci.flags                 = 0;

#if WITH_CUDA
    buffer_ci.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

    VmaAllocationCreateInfo alloc_ci = {};
    alloc_ci.usage                   = VMA_MEMORY_USAGE_AUTO;
    alloc_ci.flags                   = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

    VK_CHECK_RESULT(
        vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr)
    );
    bindless_index_buffer = MoerNew(VulkanBuffer)(
        s_bindless_index_buffer_name, buffer_info, *m_device, current_handle, alloc, false, true
    );

    buffer_ci.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_ci.size = bindless_buffer_descriptor_bytes;
    current_handle = VK_NULL_HANDLE;
    alloc          = VK_NULL_HANDLE;

    VK_CHECK_RESULT(
        vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr)
    );
    buffer_info.size   = bindless_buffer_descriptor_bytes;
    buffer_info.stride = 1;

    bindless_buffer_descs = MoerNew(VulkanBuffer)(
        s_bindless_buffer_descriptors_name, buffer_info, *m_device, current_handle, alloc, false, true
    );

    buffer_ci.usage = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                      VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_ci.size = bindless_texture_descriptor_bytes;
    current_handle = VK_NULL_HANDLE;
    alloc          = VK_NULL_HANDLE;
    VK_CHECK_RESULT(
        vmaCreateBuffer(m_device->GetVmaAllocator(), &buffer_ci, &alloc_ci, &current_handle, &alloc, nullptr)
    );

    buffer_info.size       = bindless_texture_descriptor_bytes;
    buffer_info.stride     = 1;
    bindless_texture_descs = MoerNew(VulkanBuffer)(
        s_bindless_texture_descriptors_name, buffer_info, *m_device, current_handle, alloc, false, true
    );

    CommandList cmd_list{};
    //fill sampler data to descriptor buffer
    {
        uint sampler_stride =
            m_device->GetOptionalProperties().descriptor_buffer_properties.samplerDescriptorSize;
        // void* mapped_data;
        Array<byte> data_array(
            m_device->GetOptionalProperties().descriptor_buffer_properties.samplerDescriptorSize *
            VulkanDevice::bindless_sampler_count
        );
        // vmaMapMemory(m_device->GetVmaAllocator(), bindless_texture_descs->GetAllocation(), &mapped_data);
        const auto* samplers = m_device->GetImmutableSamplers();
        // byte* mapped_data_byte = reinterpret_cast<byte*>(mapped_data);
        VkDescriptorGetInfoEXT descriptor_info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        descriptor_info.type = VK_DESCRIPTOR_TYPE_SAMPLER;

        VkDescriptorDataEXT& descriptor_data = descriptor_info.data;
        for (uint i = 0; i < VulkanDevice::bindless_sampler_count; i++) {
            descriptor_data.pSampler = i >= m_device->ImmutableSamplerCount() ? &samplers[0] : &samplers[i];
            vkGetDescriptorEXT(
                m_device->GetDevice(),
                &descriptor_info,
                sampler_stride,
                data_array.data() + i * sampler_stride
            );
        }
        // std::memcpy(mapped_data_byte, data_array.data(), data_array.size());
        // vmaUnmapMemory(m_device->GetVmaAllocator(), bindless_texture_descs->GetAllocation());
        // vmaFlushAllocation(m_device->GetVmaAllocator(), bindless_texture_descs->GetAllocation(), 0, VulkanDevice::bindless_sampler_count * sampler_stride);
        cmd_list.CopyFrom(
            std::span<byte>(data_array.data(), data_array.size()),
            bindless_texture_descs->GetView(0, data_array.size())
        );
        RHIExecutor::Get().Submit(EQueueType::Graphics, cmd_list.Submit(), ERHIExecSubmitFlags::FlushGPU);
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    }
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

                            VulkanCachedDescriptor descriptor;
                            if (uint(vk_texture->GetAspectFlags() & ETextureAspectFlags::DEPTH_SLICE) != 0) {
                                // view.aspect_flags = ETextureAspectFlags::COLOR;
                                descriptor = heap.GetOrCreateImageDescriptor(
                                    &view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                );

                            } else {
                                descriptor = heap.GetOrCreateImageDescriptor(
                                    &view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                );
                            }

                            heap.CopyDescriptorToMemory(
                                descriptor,
                                std::span<byte>(texture_dat)
                                    .subspan(texture_cnt * texture_handle_stride, texture_handle_stride)
                            );
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
                            VulkanCachedDescriptor descriptor =
                                heap.GetOrCreateBufferDescriptor(vk_buffer->GetView(_cmd.format), type);

                            heap.CopyDescriptorToMemory(
                                descriptor,
                                std::span<byte>(buffer_dat)
                                    .subspan(buffer_cnt * buffer_handle_stride, buffer_handle_stride)
                            );
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

bool VulkanBindlessArray::BeginUpdateCommand(const Array<BindlessArray::UpdateCmd>& _updates) {
    std::unique_lock<std::mutex> lk(mtx);

    uint64 command_token = 0;
    bool   token_valid   = true;
    for (const BindlessArray::UpdateCmd& update : _updates) {
        std::visit(
            [&](const auto& _cmd) {
                using TCmd = std::decay_t<decltype(_cmd)>;
                if constexpr (std::is_same_v<TCmd, TextureUpdateInfo> ||
                              std::is_same_v<TCmd, BufferUpdateInfo>) {
                    if (_cmd.command_token == 0) {
                        token_valid = false;
                    } else if (command_token == 0) {
                        command_token = _cmd.command_token;
                    } else if (command_token != _cmd.command_token) {
                        token_valid = false;
                    }
                }
            },
            update
        );
    }
    if (!token_valid) {
        throw std::runtime_error("bindless update command has an invalid lifecycle token");
    }

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

    bool all_current = token_valid;
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
    for (const uint slot : global_claims) {
        ++array_slot_generations[slot];
        array_slot_claim_tokens[slot] = command_token;
    }
    for (const uint slot : texture_claims) {
        ++texture_slot_generations[slot];
        texture_slot_claim_tokens[slot] = command_token;
    }
    for (const uint slot : buffer_claims) {
        ++buffer_slot_generations[slot];
        buffer_slot_claim_tokens[slot] = command_token;
    }
    return all_current;
}

void VulkanBindlessArray::FinalizeUpdateCommand(
    const Array<BindlessArray::UpdateCmd>& _updates,
    bool                                   _gpu_update_succeeded
) {
    std::unique_lock<std::mutex> lk(mtx);

    uint64 command_token = 0;
    for (const BindlessArray::UpdateCmd& update : _updates) {
        std::visit(
            [&](const auto& _cmd) {
                using TCmd = std::decay_t<decltype(_cmd)>;
                if constexpr (std::is_same_v<TCmd, TextureUpdateInfo> ||
                              std::is_same_v<TCmd, BufferUpdateInfo>) {
                    if (_cmd.command_token == 0) {
                        throw std::runtime_error("bindless update finalization has no lifecycle token");
                    }
                    if (command_token == 0) {
                        command_token = _cmd.command_token;
                    } else if (command_token != _cmd.command_token) {
                        throw std::runtime_error("bindless update finalization mixes lifecycle tokens");
                    }
                }
            },
            update
        );
    }
    if (command_token == 0) {
        return;
    }

    Array<std::pair<const TextureUpdateInfo*, bool>> texture_restores;
    Array<std::pair<const BufferUpdateInfo*, bool>>  buffer_restores;
    Array<const TextureUpdateInfo*>                  texture_retirements;
    Array<const BufferUpdateInfo*>                   buffer_retirements;
    UnorderedMap<uint64, uint32>                     resource_decrements;
    UnorderedMap<TextureSubresourceKeyT<VulkanTexture>, uint32, TextureSubresourceKeyHashT<VulkanTexture>>
                       view_decrements;
    UnorderedSet<uint> global_slots;
    UnorderedSet<uint> texture_slots;
    UnorderedSet<uint> buffer_slots;
    UnorderedSet<uint> global_claims;
    UnorderedSet<uint> texture_claims;
    UnorderedSet<uint> buffer_claims;

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
        if (!global_claims.emplace(_cmd.array_idx).second) {
            throw std::runtime_error("duplicate pending bindless global-slot claim");
        }
        if (_texture) {
            if (texture_slot_generations[_cmd.slot] == std::numeric_limits<uint64>::max()) {
                throw std::runtime_error("bindless texture-slot generation exhausted");
            }
            if (!texture_claims.emplace(_cmd.slot).second) {
                throw std::runtime_error("duplicate pending bindless texture-slot claim");
            }
        } else {
            if (buffer_slot_generations[_cmd.slot] == std::numeric_limits<uint64>::max()) {
                throw std::runtime_error("bindless buffer-slot generation exhausted");
            }
            if (!buffer_claims.emplace(_cmd.slot).second) {
                throw std::runtime_error("duplicate pending bindless buffer-slot claim");
            }
        }
    };

    // Validate the whole retirement transaction before changing membership
    // or exposing any slot to another producer.
    for (const BindlessArray::UpdateCmd& update : _updates) {
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
                        GetBindlessUpdateFinalizationAction(state, _cmd.free, _gpu_update_succeeded);
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
                        texture_restores.emplace_back(
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
                    ++resource_decrements[uint64(_cmd.texture.Get())];
                    const auto& key = texture_view_infos[_cmd.array_idx];
                    if (key.texture != ResourceCast(_cmd.texture.Get())) {
                        throw std::runtime_error("pending bindless texture release lost its view");
                    }
                    ++view_decrements[key];
                    texture_retirements.push_back(&_cmd);
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
                        GetBindlessUpdateFinalizationAction(state, _cmd.free, _gpu_update_succeeded);
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
                        buffer_restores.emplace_back(
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
                    ++resource_decrements[uint64(_cmd.buffer.Get())];
                    buffer_retirements.push_back(&_cmd);
                }
            },
            update
        );
    }

    for (const auto& [resource, decrement] : resource_decrements) {
        const auto allocation = resource_allocation_counts.find(resource);
        if (allocation == resource_allocation_counts.end() || allocation->second.count < decrement) {
            throw std::runtime_error("bindless resource retirement exceeds its reference count");
        }
    }
    for (const auto& [key, decrement] : view_decrements) {
        const auto texture = texture_view_ref_counts.find(uint64(key.texture));
        if (texture == texture_view_ref_counts.end()) {
            throw std::runtime_error("pending bindless texture retirement has no texture entry");
        }
        const auto view = texture->second.find(key);
        if (view == texture->second.end() || view->second < decrement) {
            throw std::runtime_error("bindless texture retirement exceeds its view reference count");
        }
    }

    // Claim every still-current cancellation only after the complete
    // transaction has validated. A later command then observes a stale
    // generation and cannot restore or upload this retired binding.
    for (const uint slot : global_claims) {
        ++array_slot_generations[slot];
        array_slot_claim_tokens[slot] = command_token;
    }
    for (const uint slot : texture_claims) {
        ++texture_slot_generations[slot];
        texture_slot_claim_tokens[slot] = command_token;
    }
    for (const uint slot : buffer_claims) {
        ++buffer_slot_generations[slot];
        buffer_slot_claim_tokens[slot] = command_token;
    }

    // A rejected free never reached a GPU ordering point. Roll it back to
    // an active CPU binding instead of recycling the descriptor while
    // older submissions may still read it. This command is the only owner
    // allowed to undo its generation claim.
    for (const auto& [restore, claimed] : texture_restores) {
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
    for (const auto& [restore, claimed] : buffer_restores) {
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

    for (const TextureUpdateInfo* retirement : texture_retirements) {
        handles[retirement->array_idx] = Handle{};
        UntrackResource(uint64(retirement->texture.Get()));
        UntrackTextureView(retirement->array_idx);
    }
    for (const BufferUpdateInfo* retirement : buffer_retirements) {
        handles[retirement->array_idx] = Handle{};
        UntrackResource(uint64(retirement->buffer.Get()));
    }
    for (const TextureUpdateInfo* retirement : texture_retirements) {
        free_slots.Push(retirement->array_idx);
        free_texture_slots.Push(retirement->slot);
    }
    for (const BufferUpdateInfo* retirement : buffer_retirements) {
        free_slots.Push(retirement->array_idx);
        free_buffer_slots.Push(retirement->slot);
    }
}

void VulkanBindlessArray::DiscardUpdateCommand(const Array<BindlessArray::UpdateCmd>& _updates) {
    std::unique_lock<std::mutex> lk(mtx);

    uint64 command_token = 0;
    for (const BindlessArray::UpdateCmd& update : _updates) {
        std::visit(
            [&](const auto& _cmd) {
                using TCmd = std::decay_t<decltype(_cmd)>;
                if constexpr (std::is_same_v<TCmd, TextureUpdateInfo> ||
                              std::is_same_v<TCmd, BufferUpdateInfo>) {
                    if (_cmd.command_token == 0) {
                        throw std::runtime_error("discarded bindless update has no lifecycle token");
                    }
                    if (command_token == 0) {
                        command_token = _cmd.command_token;
                    } else if (command_token != _cmd.command_token) {
                        throw std::runtime_error("discarded bindless update mixes lifecycle tokens");
                    }
                }
            },
            update
        );
    }
    if (command_token == 0) {
        return;
    }

    Array<const TextureUpdateInfo*> texture_restores;
    Array<const BufferUpdateInfo*>  buffer_restores;
    Array<const TextureUpdateInfo*> texture_retirements;
    Array<const BufferUpdateInfo*>  buffer_retirements;
    UnorderedMap<uint64, uint32>    resource_decrements;
    UnorderedMap<TextureSubresourceKeyT<VulkanTexture>, uint32, TextureSubresourceKeyHashT<VulkanTexture>>
                       view_decrements;
    UnorderedSet<uint> global_slots;
    UnorderedSet<uint> texture_slots;
    UnorderedSet<uint> buffer_slots;
    UnorderedSet<uint> global_claims;
    UnorderedSet<uint> texture_claims;
    UnorderedSet<uint> buffer_claims;

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
        if (!global_claims.emplace(_cmd.array_idx).second) {
            throw std::runtime_error("duplicate discarded bindless global-slot claim");
        }
        if (_texture) {
            if (texture_slot_generations[_cmd.slot] == std::numeric_limits<uint64>::max()) {
                throw std::runtime_error("discarded bindless texture-slot generation exhausted");
            }
            if (!texture_claims.emplace(_cmd.slot).second) {
                throw std::runtime_error("duplicate discarded bindless texture-slot claim");
            }
        } else {
            if (buffer_slot_generations[_cmd.slot] == std::numeric_limits<uint64>::max()) {
                throw std::runtime_error("discarded bindless buffer-slot generation exhausted");
            }
            if (!buffer_claims.emplace(_cmd.slot).second) {
                throw std::runtime_error("duplicate discarded bindless buffer-slot claim");
            }
        }
    };

    for (const BindlessArray::UpdateCmd& update : _updates) {
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

                    Handle& handle = handles[_cmd.array_idx];
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
                        texture_restores.push_back(&_cmd);
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
                    ++resource_decrements[uint64(_cmd.texture.Get())];
                    const auto& key = texture_view_infos[_cmd.array_idx];
                    if (key.texture != ResourceCast(_cmd.texture.Get())) {
                        throw std::runtime_error("discarded bindless texture allocation lost its view");
                    }
                    ++view_decrements[key];
                    texture_retirements.push_back(&_cmd);
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

                    Handle& handle = handles[_cmd.array_idx];
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
                        buffer_restores.push_back(&_cmd);
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
                    ++resource_decrements[uint64(_cmd.buffer.Get())];
                    buffer_retirements.push_back(&_cmd);
                }
            },
            update
        );
    }

    for (const auto& [resource, decrement] : resource_decrements) {
        const auto allocation = resource_allocation_counts.find(resource);
        if (allocation == resource_allocation_counts.end() || allocation->second.count < decrement) {
            throw std::runtime_error("discarded bindless update exceeds its resource reference count");
        }
    }
    for (const auto& [key, decrement] : view_decrements) {
        const auto texture = texture_view_ref_counts.find(uint64(key.texture));
        if (texture == texture_view_ref_counts.end()) {
            throw std::runtime_error("discarded bindless texture update has no texture entry");
        }
        const auto view = texture->second.find(key);
        if (view == texture->second.end() || view->second < decrement) {
            throw std::runtime_error("discarded bindless texture update exceeds its view reference count");
        }
    }

    for (const uint slot : global_claims) {
        ++array_slot_generations[slot];
        array_slot_claim_tokens[slot] = command_token;
    }
    for (const uint slot : texture_claims) {
        ++texture_slot_generations[slot];
        texture_slot_claim_tokens[slot] = command_token;
    }
    for (const uint slot : buffer_claims) {
        ++buffer_slot_generations[slot];
        buffer_slot_claim_tokens[slot] = command_token;
    }

    for (const TextureUpdateInfo* restore : texture_restores) {
        handles[restore->array_idx] =
            Handle(uint64(restore->texture.Get()), restore->slot, 1, VulkanBindlessArray::Texture);
    }
    for (const BufferUpdateInfo* restore : buffer_restores) {
        handles[restore->array_idx] =
            Handle(uint64(restore->buffer.Get()), restore->slot, 0, VulkanBindlessArray::Buffer);
    }
    for (const TextureUpdateInfo* retirement : texture_retirements) {
        handles[retirement->array_idx] = Handle{};
        UntrackResource(uint64(retirement->texture.Get()));
        UntrackTextureView(retirement->array_idx);
    }
    for (const BufferUpdateInfo* retirement : buffer_retirements) {
        handles[retirement->array_idx] = Handle{};
        UntrackResource(uint64(retirement->buffer.Get()));
    }
    for (const TextureUpdateInfo* retirement : texture_retirements) {
        free_slots.Push(retirement->array_idx);
        free_texture_slots.Push(retirement->slot);
    }
    for (const BufferUpdateInfo* retirement : buffer_retirements) {
        free_slots.Push(retirement->array_idx);
        free_buffer_slots.Push(retirement->slot);
    }
}

VulkanBindlessArray::~VulkanBindlessArray() {
    if (bindless_index_buffer) {
        MoerDelete(bindless_index_buffer);
        bindless_index_buffer = nullptr;
    }
    if (bindless_buffer_descs) {
        MoerDelete(bindless_buffer_descs);
        bindless_buffer_descs = nullptr;
    }
    if (bindless_texture_descs) {
        MoerDelete(bindless_texture_descs);
        bindless_texture_descs = nullptr;
    }
}

} // namespace Moer::Render
