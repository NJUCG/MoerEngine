#include "shader/ShaderPipeline.h"
#include "../rhi/vulkan/VulkanDevice.h"
#include "rhi/RHIResource.h"
#include "../rhi/vulkan/VulkanPipelineResourceCache.h"

namespace Moer::Render {
    void ShaderPipeline::SetTexture(uint32_t _index, TextureRef _texture) {
        SetTexture(_index, std::move(_texture->GetView()));
    }

    void ShaderPipeline::SetBuffer(uint32_t _index, BufferRef _buffer) {

        SetBuffer(_index, std::move(_buffer->GetView()));
    }

    void ShaderPipeline::SetBuffer(uint32_t _index, BufferView _view) {
        uint64 binding_info = handle.binding_infos[_index];
        std::visit([&](auto&& _arg) {
            using T = std::decay_t<decltype(_arg)>;
            if constexpr (std::is_same_v<T, VkPipelineHandle>) {
                VulkanPipelineState& pso            = *reinterpret_cast<VulkanPipelineState*>(_arg.handle);
                auto*                resource_cache = pso.GetPipelineResourceCache();
                auto [set, binding, stage_flags]    = DecodeReflectInfo(binding_info);

                resource_cache->SetBuffer(set, binding, std::move(_view));
            }
        },
                   handle.handle);
    }

    void ShaderPipeline::SetTexture(uint32_t _index, TextureView _texture) {
        uint64 binding_info = handle.binding_infos[_index];
        std::visit([&](auto&& _arg) {
            using T = std::decay_t<decltype(_arg)>;
            if constexpr (std::is_same_v<T, VkPipelineHandle>) {
                VulkanPipelineState& pso            = *reinterpret_cast<VulkanPipelineState*>(_arg.handle);
                auto*                resource_cache = pso.GetPipelineResourceCache();
                auto [set, binding, stage_flags]    = DecodeReflectInfo(binding_info);

                resource_cache->SetTexture(set, binding, std::move(_texture));
            }
        },
                   handle.handle);
    }

    void ShaderPipeline::SetConstantInner(uint _index, std::span<uint> _data) {
        uint64 binding_info = handle.binding_infos[_index];
        std::visit([&](auto&& _arg) {
            using T = std::decay_t<decltype(_arg)>;
            if constexpr (std::is_same_v<T, VkPipelineHandle>) {
                VulkanPipelineState& pso            = *reinterpret_cast<VulkanPipelineState*>(_arg.handle);
                auto*                resource_cache = pso.GetPipelineResourceCache();
                auto [offset, size, stage_flags]    = DecodeReflectPushConstant(binding_info);

                resource_cache->PushConstant(stage_flags, _data);
            }
        },
                   handle.handle);
    }

    void ShaderPipeline::SetBufferHash(uint64 _hash, BufferView _buffer) {
        uint idx = handle.hash_2_info_index[_hash];
        SetBuffer(idx, std::move(_buffer));
    }

    void ShaderPipeline::SetTextureHash(uint64 _hash, TextureView _texture) {
        uint idx = handle.hash_2_info_index[_hash];
        SetTexture(idx, std::move(_texture));
    }
};// namespace Moer::Render