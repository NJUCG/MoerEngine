#include "VulkanBindlessArray.h"
#include "VulkanCommandList.h"
#include "VulkanDevice.h"
#include "VulkanEnumTranslator.h"
#include "VulkanMacroUtils.h"
#include "VulkanPipeline.h"
#include "VulkanResourceState.h"
#include "shader/ShaderPipeline.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace Moer::Render {
namespace {
struct DescriptorWrite {
    VkWriteDescriptorSet                         write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    VkDescriptorBufferInfo                       buffer{};
    VkDescriptorImageInfo                        image{};
    VkBufferView                                 view                   = VK_NULL_HANDLE;
    VkAccelerationStructureKHR                   acceleration_structure = VK_NULL_HANDLE;
    VkWriteDescriptorSetAccelerationStructureKHR acceleration{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
    };
};

// Build pointer-bearing native writes only after the backing array stops growing.
Array<VkWriteDescriptorSet> ResolveWrites(Array<DescriptorWrite>& records) {
    Array<VkWriteDescriptorSet> writes;
    writes.reserve(records.size());
    for (auto& record : records) {
        auto write = record.write;
        switch (write.descriptorType) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                write.pBufferInfo = &record.buffer;
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                write.pTexelBufferView = &record.view;
                break;
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                record.acceleration.accelerationStructureCount = 1;
                record.acceleration.pAccelerationStructures    = &record.acceleration_structure;
                write.pNext                                    = &record.acceleration;
                break;
            default:
                write.pImageInfo = &record.image;
                break;
        }
        writes.push_back(write);
    }
    return writes;
}

template<class View, class Fn>
void ForDescriptorViews(const TArg& argument, uint count, Fn&& fn) {
    if (count == 1) {
        fn(std::get<View>(argument), 0);
        return;
    }
    const auto& views = std::get<Array<View>>(argument);
    if (views.empty() || views.size() > count)
        throw std::invalid_argument("Invalid descriptor array length");
    for (uint i = 0; i < count; ++i)
        fn(views[std::min<size_t>(i, views.size() - 1)], i);
}
} // namespace

void VulkanCommandList::ResetTransientDescriptors() {
    const auto device = command_pool.GetDevice().GetDevice();
    for (const auto& page : descriptor_pools)
        vkDestroyDescriptorPool(device, page.pool, nullptr);
    descriptor_pools.clear();
    for (auto view : descriptor_buffer_views)
        vkDestroyBufferView(device, view, nullptr);
    descriptor_buffer_views.clear();
}

VkDescriptorSet VulkanCommandList::AllocateDescriptorSet(
    VkDescriptorSetLayout                  layout,
    std::span<const VulkanResourceBinding> bindings
) {
    const auto                  device = command_pool.GetDevice().GetDevice();
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts        = &layout;
    UnorderedMap<VkDescriptorType, uint> counts;
    for (const auto& binding : bindings)
        if (binding.descriptor_count)
            counts[binding.descriptor_type] += binding.descriptor_count;
    const auto consume = [&](DescriptorPoolPage& page) {
        --page.remaining_sets;
        for (auto& remaining : page.remaining_descriptors)
            if (const auto found = counts.find(remaining.type); found != counts.end())
                remaining.descriptorCount -= found->second;
    };
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (auto& page : descriptor_pools) {
        if (!page.remaining_sets)
            continue;
        const bool fits = std::all_of(counts.begin(), counts.end(), [&](const auto& required) {
            return std::any_of(
                page.remaining_descriptors.begin(),
                page.remaining_descriptors.end(),
                [&](const auto& remaining) {
                    return remaining.type == required.first && remaining.descriptorCount >= required.second;
                }
            );
        });
        if (!fits)
            continue;
        allocate.descriptorPool = page.pool;
        const VkResult result   = vkAllocateDescriptorSets(device, &allocate, &set);
        if (result == VK_SUCCESS) {
            consume(page);
            return set;
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
            VK_CHECK_RESULT(result);
        // No individual frees occur, so a failed page is not retried this recording.
        page.remaining_sets = 0;
    }
    Array<VkDescriptorPoolSize> sizes;
    for (const auto& [type, count] : counts)
        sizes.push_back({type, std::max(64u, count * 64)});
    VkDescriptorPoolCreateInfo create{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    create.maxSets       = 64;
    create.poolSizeCount = static_cast<uint>(sizes.size());
    create.pPoolSizes    = sizes.data();
    VkDescriptorPool pool;
    VK_CHECK_RESULT(vkCreateDescriptorPool(device, &create, nullptr, &pool));
    try {
        descriptor_pools.push_back({pool, 64, std::move(sizes)});
    } catch (...) {
        vkDestroyDescriptorPool(device, pool, nullptr);
        throw;
    }
    allocate.descriptorPool = pool;
    VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocate, &set));
    consume(descriptor_pools.back());
    return set;
}

void VulkanCommandList::BindPipelineDescriptorSets(
    const VulkanPipelineState& pipeline,
    const ArrayArguments&      arguments,
    uint64                     resource_byte_offset
) {
    auto& device = command_pool.GetDevice();
    // Bind every set in ascending order, including push UBs. Binding lower sets
    // after higher ones can disturb the latter when pipeline layouts change.
    for (const auto& [set_index, set] : pipeline.binding_template->set_bindings) {
        if (pipeline.GetDescriptorMode() == VulkanDescriptorMode::Buffer && !set.push) {
            const VkDeviceSize byte_offset = set.byte_offset +
                (std::holds_alternative<VulkanResourceSetBindings>(set.resource_bindings) ?
                     resource_byte_offset : 0);
            vkCmdSetDescriptorBufferOffsetsEXT(
                command_buffer,
                pipeline.GetPipelineBindPoint(),
                pipeline.GetPipelineLayout(),
                set_index,
                1,
                &set.buffer_index,
                &byte_offset
            );
            continue;
        }
        const auto* resources = std::get_if<VulkanResourceSetBindings>(&set.resource_bindings);
        if (!resources) {
            VkDescriptorSet native_set = std::visit(
                Overload{
                    [&](const VulkanResourceSetBindings&) -> VkDescriptorSet {
                        return VK_NULL_HANDLE;
                    },
                    [&]<VkDescriptorType Type>(const VulkanBindlessBinding<Type>& binding) {
                        const auto& array = std::get<BindlessArrayRef>(arguments[binding.argument_index]);
                        if (!array)
                            throw std::invalid_argument("Null bindless array");
                        return static_cast<VulkanBindlessArray*>(array.Get())->GetLegacySet(Type);
                    }
                },
                set.resource_bindings
            );
            vkCmdBindDescriptorSets(
                command_buffer,
                pipeline.GetPipelineBindPoint(),
                pipeline.GetPipelineLayout(),
                set_index,
                1,
                &native_set,
                0,
                nullptr
            );
            continue;
        }
        Array<DescriptorWrite> records;
        for (const auto& binding : resources->bindings) {
            if (!binding.descriptor_count || binding.argument_index >= 64 ||
                !(pipeline.GetActiveArgumentBits() & (uint64(1) << binding.argument_index)))
                continue;
            const auto& argument = arguments[binding.argument_index];
            auto        add      = [&](uint element) -> DescriptorWrite& {
                auto& record                 = records.emplace_back();
                record.write.dstBinding      = binding.binding_index;
                record.write.dstArrayElement = element;
                record.write.descriptorType  = binding.descriptor_type;
                record.write.descriptorCount = 1;
                return record;
            };
            switch (binding.descriptor_type) {
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                    ForDescriptorViews<BufferView>(
                        argument,
                        binding.descriptor_count,
                        [&](const BufferView& view, uint element) {
                            auto& record  = add(element);
                            record.buffer = {
                                ResourceCast(view.GetBuffer())->GetHandle(),
                                view.byte_offset,
                                view.GetByteSize()
                            };
                            if (binding.descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                                binding.descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                                VkBufferViewCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
                                info.buffer = record.buffer.buffer;
                                info.offset = record.buffer.offset;
                                info.range  = record.buffer.range;
                                info.format = VulkanEnumTranslator::METoVKFormat(
                                    view.format == PF_UNDEFINED ?
                                        pipeline.GetArgumentInfo(binding.argument_index).texel_format :
                                        view.format
                                );
                                VK_CHECK_RESULT(
                                    vkCreateBufferView(device.GetDevice(), &info, nullptr, &record.view)
                                );
                                try {
                                    descriptor_buffer_views.push_back(record.view);
                                } catch (...) {
                                    vkDestroyBufferView(device.GetDevice(), record.view, nullptr);
                                    throw;
                                }
                            }
                        }
                    );
                    break;
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    ForDescriptorViews<TextureView>(
                        argument,
                        binding.descriptor_count,
                        [&](const TextureView& view, uint element) {
                            auto& record           = add(element);
                            record.image.imageView = ResourceCast(view.GetTexture())
                                                         ->GetView(
                                                             view.mip_level,
                                                             view.num_mips,
                                                             view.array_layer,
                                                             view.num_array ? view.num_array : 1
                                                         );
                            record.image.imageLayout =
                                binding.descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ?
                                    VK_IMAGE_LAYOUT_GENERAL :
                                    ResolveVulkanTextureState(
                                        ETextureState::SAMPLE,
                                        EPassType::Graphics,
                                        false,
                                        view.GetTexture()->GetAspectFlags()
                                    )
                                        .layout;
                        }
                    );
                    break;
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                    if (binding.descriptor_count != 1)
                        throw std::invalid_argument("Sampler arrays are unsupported by ArrayArguments");
                    add(0).image.sampler = device.GetSampler(std::get<Sampler>(argument));
                    break;
                case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                    if (binding.descriptor_count != 1)
                        throw std::invalid_argument(
                            "Acceleration structure arrays are unsupported by ArrayArguments"
                        );
                    add(0).acceleration_structure =
                        ResourceCast(std::get<RaytracingTlasRef>(argument).Get())->handle;
                    break;
                default:
                    throw std::invalid_argument("Unsupported native descriptor type");
            }
        }
        auto writes = ResolveWrites(records);
        if (set.push) {
            if (writes.empty())
                continue; // All descriptors in this set are statically unused.
            vkCmdPushDescriptorSetKHR(
                command_buffer,
                pipeline.GetPipelineBindPoint(),
                pipeline.GetPipelineLayout(),
                set_index,
                static_cast<uint>(writes.size()),
                writes.data()
            );
        } else {
            const VkDescriptorSet native_set =
                AllocateDescriptorSet(pipeline.GetDescriptorSetLayout(set_index), resources->bindings);
            for (auto& write : writes)
                write.dstSet = native_set;
            vkUpdateDescriptorSets(
                device.GetDevice(), static_cast<uint>(writes.size()), writes.data(), 0, nullptr
            );
            vkCmdBindDescriptorSets(
                command_buffer,
                pipeline.GetPipelineBindPoint(),
                pipeline.GetPipelineLayout(),
                set_index,
                1,
                &native_set,
                0,
                nullptr
            );
        }
    }
}

void VulkanCommandList::BindHeapDescriptors(
    const VulkanPipelineState& pipeline,
    const ArrayArguments&      arguments,
    uint64                     allocation_offset
) {
    auto&       device  = command_pool.GetDevice();
    auto&       heap    = device.GetGlobalDescriptorHeap();
    const auto& binding = *pipeline.binding_template;
    Array<byte> push(binding.heap_push_size, byte{});
    const auto  store = [&](uint offset, const auto& value) {
        if (offset + sizeof(value) > push.size())
            throw std::out_of_range("Heap push data overflow");
        memcpy(push.data() + offset, &value, sizeof(value));
    };
    const auto& constants = binding.push_constants_info;
    if (arguments.constants.size() * sizeof(uint) < constants.size)
        throw std::invalid_argument("Push data source is too small");
    if (constants.size)
        memcpy(push.data() + constants.offset, arguments.constants.data(), constants.size);
    if (binding.bindless_index_address) {
        const auto& address_binding = *binding.bindless_index_address;
        const auto& array           = std::get<BindlessArrayRef>(arguments[address_binding.argument_index]);
        if (!array)
            throw std::invalid_argument("Null bindless index table");
        store(
            address_binding.byte_offset,
            static_cast<VulkanBindlessArray*>(array.Get())->GetIndexBuffer()->DeviceAddress()
        );
    }
    for (const auto& [set_index, set] : binding.set_bindings) {
        if (const auto* resources = std::get_if<VulkanResourceSetBindings>(&set.resource_bindings)) {
            store(set.heap_push_offset, static_cast<uint>(allocation_offset + set.byte_offset));
            for (const auto& resource : resources->bindings) {
                if (resource.heap_push_offset == UINT32_MAX || resource.argument_index >= 64 ||
                    !(pipeline.GetActiveArgumentBits() & (uint64(1) << resource.argument_index)))
                    continue;
                const auto& argument = arguments[resource.argument_index];
                if (resource.descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                    store(resource.heap_push_offset, device.GetSamplerIndex(std::get<Sampler>(argument)));
                } else {
                    const auto& buffer = std::get<BufferView>(argument);
                    store(
                        resource.heap_push_offset,
                        ResourceCast(buffer.GetBuffer())->DeviceAddress() + buffer.byte_offset
                    );
                }
            }
        } else {
            std::visit(
                Overload{
                    [&](const VulkanResourceSetBindings&) {},
                    [&]<VkDescriptorType Type>(const VulkanBindlessBinding<Type>& table) {
                        const auto& array = std::get<BindlessArrayRef>(arguments[table.argument_index]);
                        if (!array)
                            throw std::invalid_argument("Null bindless array");
                        auto*      native = static_cast<VulkanBindlessArray*>(array.Get());
                        const uint base   = Type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ?
                                                static_cast<uint>(native->buffer_descriptors_byte_offset) :
                                            Type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ?
                                                static_cast<uint>(native->texture_offset_in_buffer) :
                                                0;
                        store(set.heap_push_offset, base);
                    }
                },
                set.resource_bindings
            );
        }
    }
    const auto& functions = device.GetDescriptorHeapFunctions();
    functions.bind_resources(command_buffer, &heap.GetResourceHeapBinding());
    functions.bind_samplers(command_buffer, &heap.GetSamplerHeapBinding());
    if (!push.empty()) {
        VkPushDataInfoEXT info{VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT};
        info.data = {push.data(), push.size()};
        functions.push_data(command_buffer, &info);
    }
}
} // namespace Moer::Render
