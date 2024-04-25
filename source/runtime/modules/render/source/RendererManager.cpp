#include "RendererManager.h"

#include "config/ConfigManager.h"
#include "misc/STL.h"
#include "renderer/BackendRenderer.h"
#include "renderer/backend/DeferredRenderer.h"
#include "renderer/backend/MeshDebugRenderer.h"
#include "renderer/backend/RenderGraphTestRender.h"
#include "renderer/backend/RenderGraphTestRender.h"
#include "rhi/RHIResource.h"
#include "renderer/backend/3dGs/SplattingRender.h"
#include <string>
#include "renderer/backend/Common.h"
namespace Moer {
    struct RendererManagerData {
        Moer::UnorderedMap<std::string, TRendererID> renderer_ids;

        Moer::UnorderedMap<TRendererID, BackendRenderer*> backend_renderers;
    };

    BackendRenderer* GetRender(std::string renderer_name) {
        if (renderer_name == MOER_3D_GAUSSIAN_SPLATTING_RENDERER_NAME) {
            return MoerNew(SplattingRender);
        }
        if (renderer_name == MOER_DEFERRED_RENDERER_NAME) {
            return MoerNew(DeferredRenderer);
        }
        if (renderer_name == MOER_MESH_RENDERER_NAME) {
            return MoerNew(MeshDebugRenderer);
        }
        return nullptr;
    }

    void RendererManager::Init() {
        data = new RendererManagerData();

        std::string render_name = ConfigManager::GetInstance().GetInitConfig().default_render_name;
        RegisterRenderer(render_name, GetRender(render_name));
        //  RegisterRenderer(MOER_DEFAULT_RENDERER_NAME, MoerNew(DeferredRenderer));
        // RegisterRenderer(MOER_MESH_RENDERER_NAME, MoerNew(MeshDebugRenderer));
        //  RegisterRenderer(MOER_RENDER_GRAPH_DEFERED_RENDERER_NAME, MoerNew(DeferedRenderingRenderGraphRender));
        BackendRendererInitInfo init_info;
        init_info.width  = 1280;
        init_info.height = 720;
        init_info.format = PF_R8G8B8A8_SRGB;

        for (auto& it : data->backend_renderers) {
            it.second->Init(init_info);
        }
    }

    void RendererManager::RegisterRenderer(const std::string& _name, BackendRenderer* _renderer) {

        assert(_renderer != nullptr && "Renderer is nullptr");
        TRendererID id = data->backend_renderers.size();

        if (data->renderer_ids.count(_name) != 0) {
            return;
        }

        data->renderer_ids.insert(std::make_pair(_name, id));
        data->backend_renderers.insert(std::make_pair(id, _renderer));
    }

    void RendererManager::UnregisterRenderer(const std::string& _name) {
        assert(0 && "Not supported");
        TRendererID id = GetRendererID(_name);
        if (id < 0) {
            return;
        }
        data->backend_renderers.erase(id);
        data->renderer_ids.erase(_name);
    }

    TRendererID RendererManager::GetRendererID(const std::string& _renderer_name) {
        if (data->renderer_ids.count(_renderer_name) == 0) {
            return -1;
        }
        return data->renderer_ids[_renderer_name];
    }

    TRendererOutput RendererManager::GetRendererOutput(TRendererID _renderer_id) {
        BackendRenderer* renderer = GetRenderer(_renderer_id);
        if (renderer == nullptr) {
            return {};
        }
        return renderer->GetRendererOutput();
    }

    BackendRenderer* RendererManager::GetRenderer(TRendererID _renderer_id) {
        if (_renderer_id < 0 || data->backend_renderers.count(_renderer_id) == 0) {
            return nullptr;
        }
        return data->backend_renderers[_renderer_id];
    }

    void RendererManager::ShutDown() {
        for (auto& it : data->backend_renderers) {
            it.second->ShutDown();
            delete it.second;
        }
        delete data;
    }

    void RendererManager::DrawFrame() {
        for (auto& it : data->backend_renderers) {
            it.second->DrawFrame();
        }
    }

    void RendererManager::Present() {
        for (auto& it : data->backend_renderers) {
            it.second->Present();
        }
    }

    void RendererManager::SetRendererOriginResolution(TRendererID _renderer_id, uint32_t _width, uint32_t _height) {
        BackendRenderer* renderer = GetRenderer(_renderer_id);
        if (renderer == nullptr) {
            return;
        }
        renderer->SetOriginResolution(_width, _height);
    }

    void RendererManager::SetRendererPresentResolution(TRendererID _renderer_id, uint32_t _width, uint32_t _height) {
        BackendRenderer* renderer = GetRenderer(_renderer_id);
        if (renderer == nullptr) {
            return;
        }
        renderer->SetPresentResolution(_width, _height);
    }

}// namespace Moer