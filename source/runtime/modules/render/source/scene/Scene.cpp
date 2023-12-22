#include "scene/Scene.h"

#include "config/ConfigManager.h"
// #include "loader/gltf/Parser.h"
#include "scene/EntityManager.h"
#include "scene/RenderableManager.h"
#include "rhi/RHI.h"


namespace Moer{
// Scene * Scene::default_scene = nullptr;    
Scene * g_scene = nullptr;
    
Scene::Scene() noexcept {
    // Entity triangle = EntityManager::Get().Create();
    //
    // const uint16_t      indices[] = {1, 2, 3};
    // RHIBufferCreateInfo buffer_info;
    // buffer_info.SetUsage(EBufferUsageFlags::INDEX_BUFFER | EBufferUsageFlags::CPU_VISIBLE)
    //     .SetStride(sizeof(uint16_t))
    //     .SetSize(sizeof(indices));
    //
    // RHIBufferRef index_buffer = CreateBufferFromData(buffer_info, sizeof(indices), (void*)indices);
    //
    // RHIBufferCreateInfo v_info;
    // v_info.SetSize(16).SetStride(4).SetUsage(EBufferUsageFlags::VERTEX_BUFFER  | EBufferUsageFlags::CPU_VISIBLE );
    //
    // const float  vertex_data[] = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 1};
    // RHIBufferRef vertex_buffer = CreateBufferFromData(v_info, sizeof(vertex_data), (void*)vertex_data);
    //
    //
    // RenderableManager::Builder().Geometry(EPrimitiveType::TRIANGLES,nullptr,nullptr,0,3).Build(triangle);
    //
    // AddEntity(triangle);
}


void Scene::AddEntity(Entity entity) noexcept {
    entities.insert(entity);
}
void Scene::RemoveEntity(Entity entity) noexcept {
    entities.erase(entity);
}

Array<Entity> Scene::GetEntities() const noexcept {
    Array<Entity> result;
    result.reserve(entities.size());
    for(auto & entity : entities) {
        result.push_back(entity);
    }
    return result;
}


Scene* Scene::GetDefaultScene() noexcept {
    return g_scene;
}
void   Scene::SetDefaultScene(Scene* scene) noexcept {
    g_scene = scene;
}


}
