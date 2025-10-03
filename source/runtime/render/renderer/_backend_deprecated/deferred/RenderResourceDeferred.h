// #ifndef MOER_ENGINE_RENDER_RESOURCE_DEFERRED_H
// #define MOER_ENGINE_RENDER_RESOURCE_DEFERRED_H

// #include "misc/STL.h"
// #include "rendergraph/RenderGraph.h"
// #include "resources/AsyncResources.h"
// #include "rhi/RHICommand.h"
// #include "rhi/RHIResource.h"
// #include <string_view>
// namespace Moer {
//     struct ViewportResources {
//         Moer::Array<RHITextureRef> g_buffer_depth;
//         Moer::Array<RHIUAVRef>     g_buffer_depth_uav;
//     };
//     struct RenderResourceDeferred {
//         RHIBufferRef packed_vertex_buffer;
//         RHIBufferRef packed_index_buffer;

//         RHIBufferRef instance_buffer;
//         RHIBufferRef meshlet_info_buffer;
//         RHIBufferRef instance_mesh_info_buffer;
//         RHIBufferRef meshlet_bounds_buffer;

//         RHITextureRef g_buffer_albedo;
//         RHITextureRef g_buffer_normal;
//         RHITextureRef g_buffer_material;

//         Moer::Array<ViewportResources> viewports;
//     };
//     struct RenderContextInitInfo {
//         uint32_t         back_buffer_cnt;
//         VirtualViewport& main_viewport;
//     };
//     struct RenderContext {
//         struct Impl;
//         Impl* impl;

//         RenderContext();
//         ~RenderContext();
//         void Init(RenderContextInitInfo _init_info);
//         void ShutDown();
//         void BeginFrame();

//         uint32_t                GetFrameOffset() const;
//         uint32_t                GetMaxFrameInFlight() const;
//         RenderGraph&            GetRenderGraph();
//         RHIGraphicsCommandList& GetCommandList();
//         RHICommandQueue&        GetCommandQueue();
//         VirtualViewport&        GetMainViewport();
//         void                    EndFrame(const VirtualViewport* _viewport);
//         void                    Present(VirtualViewport* _viewport);

//         template<typename TResourceRef>
//             requires std::is_same_v<TResourceRef, RHITextureRef> || std::is_same_v<TResourceRef, RHIBufferRef>
//         void RegisterResource(std::string_view _resource_name, TResourceRef _texture) {
//         }
//         RHITextureRef GetTexture(std::string_view _resource_name);
//         RHIBufferRef  GetBuffer(std::string_view _resource_name, RHIBufferRef& _buffer);

//     private:
//         template<>
//         void RegisterResource<RHITextureRef>(std::string_view _resource_name, RHITextureRef _texture) {
//             RegisterTexture(_resource_name, _texture);
//         }
//         template<>
//         void RegisterResource<RHIBufferRef>(std::string_view _resource_name, RHIBufferRef _buffer) {
//             RegisterBuffer(_resource_name, _buffer);
//         }
//         void RegisterBuffer(std::string_view _resource_name, RHIBufferRef _buffer);
//         void RegisterTexture(std::string_view _resource_name, RHITextureRef _texture);
//     };
// }// namespace Moer
// #endif