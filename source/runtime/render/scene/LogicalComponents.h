#pragma once

#include "PixelFormat.h"
#include "entt/entity/fwd.hpp"
#include "misc/BoundingBox.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "scene/camera/Camera.h"
#include "shaderheaders/shared/raster/SharedEnum.h"

#include <entt/entity/entity.hpp>
#include <string>

/**
 * 这里存了所有ECS Component, Context的定义
 * - Component: 作为ECS的组成部分，附加在Entity上，存储数据
 *   - 所有Component的类型名 均以C开头
 *   - 一些特殊的Component会以CTag开头，表示它只是一个Tag，不储存任何数据，只是便于ECS索引
 *   - 如果一个变量以 d_ 开头，则表示它是一个derived数据，不会被序列化存储
 * - Context: 作为全局数据存储，不附加在Entity上
 *   - 所有Context的类型名 均以Ctx开头
 */
namespace Moer::ecs {

// MARK: Camera
struct CTagMainCamera {};
struct CCamera {
    // camera之前重写过，执行稳定，这里直接复用
    // FIXME: 所以，目前camera会再存一份position等数据，没有和CNode接入
    // TODO: 统一为ECS格式
    Camera camera;
};

/**
 * MARK: Light
 *
 * 通过CLight表示一个Entity是Light
 *
 * Light的Transform权威数据来自CNode，渲染侧常用的世界空间数据缓存在Light组件的derived字段中。
 */
struct CTagMainLight {};
struct CLight {
    ELightType type = ELightType::None;
};
struct CLightDirectional {
    float3 color     = float3(1.f, 1.f, 1.f);
    float  intensity = 1.f;

    bool   is_dirty    = true;
    float3 d_direction = float3(0.f, 0.f, -1.f); // derived from CNode
};
struct CLightPoint {
    float3 color     = float3(1.f, 1.f, 1.f);
    float  intensity = 1.f;

    bool   is_dirty   = true;
    float3 d_position = float3(0.f, 0.f, 0.f); // derived from CNode
};
struct CLightAmbient {
    float3 color     = float3(1.f, 1.f, 1.f);
    float  intensity = 1.f;
};
struct CLightSpot { // 聚光灯
    // TODO
};
struct CLightEnvironment { // IBL
    entt::entity env_map_entt = entt::null;
    // TODO
};

// MARK: Node & Hieray

struct CTagRootNode {};

// CNode应该被UEmplaceNodeToParent正确设置
// CNode包含了Transform信息
struct CNode {
    entt::entity parent_entt       = entt::null;
    entt::entity prev_sibling_entt = entt::null;
    entt::entity next_sibling_entt = entt::null;
    entt::entity first_child_entt  = entt::null;
    entt::entity last_child_entt   = entt::null;
    uint32       child_count       = 0;
    uint32       depth             = 0;

    float3     translation = float3(0.f, 0.f, 0.f);
    Quaternion rotation    = Quaternion();
    float3     scale       = float3(1.f, 1.f, 1.f);

    bool is_dirty = true; // 是否需要更新 变换矩阵 & AABB

    float4x4 d_world_transform = float4x4::Identity(); // derived
    Box3D    d_aabb            = Box3D(); // derived，AABB = 儿子CNode的AABB * 自己的变换 + 挂载的CMesh的AABB
};

struct CSceneMetaData {
    entt::entity root_node_entt = entt::null;

    std::string scene_path;
};

struct CName {
    std::string name;
};

/**
 * MARK: Mesh etc.
 * 
 * - Primitive: 表示最小的可渲染单元
 * - Mesh: 可以包含多个Primitive
 * - Renderable: 绑定Mesh到Entity上，表示该Entity是可渲染的
 * 
 * - Primitive是MoerEngine最小的去重单元
 * - Mesh也是一个去重单元：多个Node可以引用同一个Mesh，比如森林中有1k棵树
 * - Renderable是为了让多个Entity引用同一个Mesh，而设计的中间层
 *   - 每个可渲染的Entity绑定一个Renderable，其中一些Renderable引用同一个Mesh
 *   - 如果不设立中间层，那么每个Entity都直接绑定Mesh，无法去重
 * 
 * 我们使用纯GPU Driven的方式进行绘制
 */

struct CPrimitive {
    struct BufferView {
        uint32 start_idx = 0;     // in element (not bytes)
        uint32 stride    = 0;     // in bytes
        bool   is_valid  = false; // 该CPrimitive是否拥有该顶点属性
    };

    uint32     vertex_count = 0;
    BufferView position;
    BufferView packed_normal;
    BufferView packed_tangent;
    BufferView texcoord0;

    uint32     index_count = 0;
    BufferView index; // index buffer的stride是uint，而非uint3

    Box3D aabb = Box3D(); // 以Mesh为单位的AABB

    entt::entity material_entt = entt::null;

    uint64 d_primitive_hash = 0; // derived
};

struct CMesh {
    Array<entt::entity> primitive_entts;

    uint64 d_mesh_hash = 0;

    Box3D d_aabb = Box3D(); // derived，AABB = 所有Primitive的AABB的并集
};

struct CRenderable {
    entt::entity mesh_entt = entt::null;
};

// MARK: Material & Texture

struct CMaterial {
    entt::entity normal_map_entt             = entt::null;
    entt::entity ao_map_entt                 = entt::null;
    entt::entity albedo_map_entt             = entt::null;
    entt::entity emissive_map_entt           = entt::null;
    entt::entity metallic_roughness_map_entt = entt::null;

    float4 albedo_factor    = float4(1.f, 1.f, 1.f, 1.f);
    float3 emissive_factor  = float3(0.f, 0.f, 0.f);
    float  metallic_factor  = 0.f;
    float  roughness_factor = 1.f;

    EAlphaMode alpha_mode   = EAlphaMode::Opaque;
    float      alpha_cutoff = 0.5f;
};

/**
 * Component: Texture
 * 
 * gemini说不用担心CPU内存碎片的问题，所以这里不用TextureLibrary进行优化
 * 
 * 为了简化，此处直接在CTexture中存储带Mipmap的纹理数据
 * 
 * TODO: 将纹理资源从LogicalScene中拆分出去，塞到一个专门的TextureLibrary中
 *       同时，LogicalScene Cache中也不带纹理；Texture放单独的Cache中
 * TODO: 将数据Upload到Gpu之后，释放Cpu内存
 * TODO: 纹理压缩
 * TODO: 是否有需求在CTexture中添加Sampler?
 */
struct CTexture {
    Array<uint8> data;
    EPixelFormat format            = PF_UNDEFINED;
    uint32       width             = 0;
    uint32       height            = 0;
    uint32       mip_level_count   = 1;
    uint32       array_layer_count = 1;
};

// MARK: [Context]

// CtxMegaBuffers是LogicalScene和CpuScene共享的
struct CtxMegaBuffers {
    Array<float3> position;
    Array<uint32> packed_normal;
    Array<uint32> packed_tangent;
    Array<float2> texcoord0;

    Array<uint32> index;
};

/**
 * Render Scene Sync Tag Components
 *
 * 用于标记 LogicalScene 到 CpuScene/GpuScene 的同步需求，按操作类型区分：
 * - NeedUpdate：已有 render-side slot 的原地数据更新，不改变数组布局
 * - NeedCreate：Logical entity 还没有 render-side slot，需要分配索引并创建缓存项
 * - NeedDestroy：释放/失效 render-side slot
 * - NeedRebuild：未来用于复杂结构变化的局部 cache 重建兜底
 *
 * 约束：NeedUpdate 不处理新增；新增必须走 NeedCreate，避免数据更新和结构变化混在一起。
 */
struct CTagNeedUpdateLight {};
struct CTagNeedUpdateMaterial {};
struct CTagNeedUpdateTransform {};

struct CTagNeedCreateLight {};
struct CTagNeedCreateMaterial {};
struct CTagNeedCreateTransform {};

struct CTagNeedDestroyLight {};

struct CTagNeedRebuildMesh {};

} // namespace Moer::ecs