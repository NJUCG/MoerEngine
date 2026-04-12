#include "RHIImpl.h"
#include "PixelFormat.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"

#include "rhi/plugin/NrdPlugin.h"
namespace Moer::Render {
PipelineHandle RenderDevice::CreatePipeline(GfxPsoCreateInfo&& _pso_info, PipelineShaderInfo&& _shaders) {
    return impl->CreatePipeline(std::move(_pso_info), std::move(_shaders));
}

PipelineHandle RenderDevice::CreatePipeline(PipelineShaderInfo&& _shaders) {
    return impl->CreatePipeline(std::move(_shaders));
}

TextureRef RenderDevice::CreateTexture(
    Extent2D           _size,
    EPixelFormat       _format,
    ETextureUsageFlags _usage,
    uint32_t           _mip_cnt,
    uint32_t           _array_size
) {
    ETextureDimension dim = _array_size > 1 ? ETextureDimension::TEX_2D_ARRAY : ETextureDimension::TEX_2D;

    return impl->CreateTexture("User2DTexture", dim, _size, _format, _usage, _mip_cnt, _array_size);
}

TextureRef RenderDevice::CreateTexture(
    Extent3D           _size,
    EPixelFormat       _format,
    ETextureUsageFlags _usage,
    uint32_t           _mip_cnt,
    uint32_t           _array_size
) {
    ETextureDimension dim =
        _size.z > 1 ? ETextureDimension::TEX_3D :
                      (_array_size > 1 ? ETextureDimension::TEX_2D_ARRAY : ETextureDimension::TEX_2D);

    return impl->CreateTexture("UserTexture", dim, _size, _format, _usage, _mip_cnt, _array_size);
}

TextureRef RenderDevice::CreateTexture(
    std::string_view   _name,
    Extent3D           _size,
    EPixelFormat       _format,
    ETextureUsageFlags _usage,
    uint32_t           _mip_cnt,
    uint32_t           _array_size
) {
    ETextureDimension dim =
        _size.z > 1 ? ETextureDimension::TEX_3D :
                      (_array_size > 1 ? ETextureDimension::TEX_2D_ARRAY : ETextureDimension::TEX_2D);

    return impl->CreateTexture(_name, dim, _size, _format, _usage, _mip_cnt, _array_size);
}

TextureRef RenderDevice::CreateCubeMap(
    std::string_view   _name,
    Extent2D           _size,
    EPixelFormat       _format,
    ETextureUsageFlags _usage,
    uint32_t           _mip_cnt
) {
    ETextureDimension dim = ETextureDimension::TEX_CUBE; // TODO:未来支持CUBE_ARRAY
    return impl->CreateTexture(_name, dim, _size, _format, _usage, _mip_cnt, 1);
}

DepthBufferRef RenderDevice::CreateDepthBuffer(
    std::string_view   _name,
    Extent2D           _size,
    EPixelFormat       _format,
    uint32_t           _array_size,
    ETextureUsageFlags _usage
) {
    return impl->CreateDepthBuffer(_name, _size, _format, _array_size, _usage);
}

BindlessArrayRef RenderDevice::CreateBindlessArray(uint _max_size) {
    return impl->CreateBindlessArray(_max_size);
}

FenceRef RenderDevice::CreateFence() {
    return impl->CreateFence();
}

SwapchainRef RenderDevice::CreateSwapchain(const SwapchainCreateInfo& _info) {
    return impl->CreateSwapchain(_info);
}

BufferRef RenderDevice::CreateBuffer(
    std::string_view  _name,
    uint              _element_cnt,
    uint              _stride,
    EBufferUsageFlags _usage,
    EPixelFormat      _format
) {
    return impl->CreateBuffer(_name, _element_cnt, _stride, _usage, _format);
}

IOInterfaceRef RenderDevice::CreateIOInterface(CopyQueue& _copy_queue) {
    return impl->CreateIOInterface(_copy_queue);
}

const EShaderPlatform RenderDevice::GetShaderPlatform() const {
    switch (rhi_type) {
        case ERHIType::Vulkan:
            return EShaderPlatform::SP_VULKAN_SM6;
        case ERHIType::D3D12:
            return EShaderPlatform::SP_WIN_D3D_SM6;
        default:
            return EShaderPlatform::SP_Num;
    }
}

RaytracingGeometryRef RenderDevice::CreateRaytracingGeometry(const RaytracingGeometryInfo& _init) {
    return impl->CreateRaytracingGeometry(_init);
}

RaytracingSceneRef RenderDevice::CreateRaytracingScene() {
    return impl->CreateRaytracingScene();
}

template<DeviceExt Ext>
Ext* RenderDevice::LoadPlugin() const {
    return static_cast<Ext*>(impl->LoadPlugin(Ext::name));
}

void RenderDevice::FlushDebugMessages() const {
    impl->FlushDebugMessages();
}

void RenderDevice::WaitIdle() {
    impl->WaitIdle();
}

template RENDER_API Ext::NRDPlugin* RenderDevice::LoadPlugin<Ext::NRDPlugin>() const;

} // namespace Moer::Render