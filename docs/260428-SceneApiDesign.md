# Scene API Design

日期：2026-04-28

本文记录 MoerEngine 场景运行时修改 API 的设计约定。后续涉及 `Scene`、`LogicalScene`、`CpuScene`、`GpuScene`、ECS tag、dirty system 的修改，都应和本文保持同步。

## 目标结论

MoerEngine 不应该做一套“每个属性一个 setter”的笨重 Modify API，也不应该让外部自由拿 `entt::registry&` 修改后手动补 tag。

推荐设计是：

- 外部读 ECS 数据：只拿 `const entt::registry&`。
- 外部修改已有组件字段：必须走 `Scene::Patch<T>()` 或同等封装。
- 外部创建、销毁、增删组件：必须走专门的结构修改 API。
- 内部系统可以访问 mutable registry，但访问范围要收敛在 `Scene` / `LogicalScene` / loader / scene sync 系统内部。

简化成一句话：**读可以直接 registry，写必须走带 MoerEngine 场景语义的入口。**

## 为什么不直接暴露 mutable registry

直接暴露 `entt::registry&` 的问题不在于 EnTT 本身，而在于 MoerEngine 的场景修改不是纯 ECS 数据修改。

例如修改方向光：

```cpp
auto& light = scene.r().get<ecs::CLightDirectional>(entity);
light.intensity = exposure;
```

这段代码本身可以修改组件字段，但它不会自动表达这些后续语义：

- `CLightDirectional::is_dirty` 需要置位。
- 需要挂 `CTagNeedUpdateLight`。
- `CpuScene::UpdateLights()` 后续要同步 CPU render cache。
- `GpuScene::Update()` 后续要同步 GPU buffer。
- 如果修改的是 `CTransform`，还可能影响 light derived data、AABB、shadow cache、RT scene instance 等。

如果把这些责任交给调用方手动处理，调用点会很容易遗漏 tag，也会让修改规则散落在 renderer、editor、loader、debug tool 里。

## 为什么不做笨重 Modify API

不推荐这种 API：

```cpp
scene.SetDirectionalLightColor(entity, color);
scene.SetDirectionalLightIntensity(entity, intensity);
scene.SetDirectionalLightDirection(entity, direction);
scene.SetPointLightPosition(entity, position);
scene.SetMaterialRoughness(entity, roughness);
scene.SetMaterialAlbedo(entity, albedo);
```

原因是 API 数量会随着组件字段线性膨胀，并且会把 ECS 的灵活性重新包回一个很重的 OOP Scene 接口里。MoerEngine 的组件还在快速变化阶段，这种接口维护成本会很高。

推荐的是“组件级 patch”，不是“字段级 setter”。

## Patch API

### 推荐调用方式

```cpp
scene.Patch<ecs::CLightDirectional>(entity, [](auto& light) {
    light.color = float3(0.9f, 0.65f, 0.4f);
    light.intensity = exposure;
});

scene.Patch<ecs::CTransform>(entity, [](auto& transform) {
    transform.rotation = Quaternion(float3(0.f, 0.f, -1.f), sun_direction);
});
```

调用点只表达“我要修改哪个组件的数据”。dirty/tag、derived cache invalidation、render scene sync 由 `Patch<T>()` 内部统一处理。

### 推荐实现形态

`Patch<T>()` 可以很薄：

```cpp
template<typename T, typename Fn>
T& Scene::Patch(entt::entity entity, Fn&& fn) {
    auto& registry = MutableRegistry();
    registry.patch<T>(entity, std::forward<Fn>(fn));
    MarkDirty<T>(entity);
    return registry.get<T>(entity);
}
```

`MarkDirty<T>()` 负责把组件修改映射为 MoerEngine 自己的 dirty 语义：

```cpp
template<>
void Scene::MarkDirty<ecs::CLightDirectional>(entt::entity entity) {
    auto& registry = MutableRegistry();
    registry.get<ecs::CLightDirectional>(entity).is_dirty = true;
    registry.emplace_or_replace<ecs::CTagNeedUpdateLight>(entity);
}

template<>
void Scene::MarkDirty<ecs::CTransform>(entt::entity entity) {
    auto& registry = MutableRegistry();
    registry.get<ecs::CTransform>(entity).is_dirty = true;
    registry.emplace_or_replace<ecs::CTagNeedUpdateTransform>(entity);

    if (registry.all_of<ecs::CLightDirectional>(entity) || registry.all_of<ecs::CLightPoint>(entity)) {
        registry.emplace_or_replace<ecs::CTagNeedUpdateLight>(entity);
    }
}
```

当前实现采用 template specialization。`Scene::Patch<T>()` 保留在 `Scene.h` 中作为薄入口，`MarkDirty<ecs::CLightDirectional>`、`MarkDirty<ecs::CLightPoint>`、`MarkDirty<ecs::CTransform>` 的特化定义放在 `SceneDirty.cpp`，避免把具体 dirty 规则堆进头文件。关键点是：dirty 规则集中维护，不散落在调用点。

## 和 EnTT patch / on_update 的关系

EnTT 的 `registry.patch<T>(entity, fn)` 大致等价于：

```cpp
auto& component = registry.get<T>(entity);
fn(component);
registry.on_update<T>().publish(entity);
```

它是一个显式的“修改组件并触发 update signal”的入口，不是字段级自动脏检查。下面这种普通引用修改不会触发 `on_update<T>`：

```cpp
auto& light = registry.get<ecs::CLightDirectional>(entity);
light.intensity = exposure;
```

因此，只使用 EnTT `patch + on_update` 仍然需要工程约定：所有写操作都必须走 patch。既然需要统一入口，MoerEngine 应该封装自己的 `Scene::Patch<T>()`。

`on_update<T>` 可以作为辅助机制，但不建议作为主架构。原因是：

- `on_update` 带隐式副作用，调用点不容易看出后续会挂哪些 tag 或 invalidation。
- MoerEngine 的 dirty 语义经常跨组件，例如 `CTransform` 修改会影响 `CLightDirectional` / `CLightPoint` derived cache。
- 后续可能需要统计修改、调试日志、线程校验、scene ready 校验、SceneUpdatePacket 收集，这些更适合放在 `Scene::Patch<T>()` 这一层。

所以推荐关系是：**`Scene::Patch<T>()` 内部可以使用 `entt::registry::patch<T>()`，但外部不直接调用 EnTT patch。**

## Public Registry 约定

目标 API：

```cpp
class Scene {
public:
    const entt::registry& r() const;
    const entt::registry& GetRegistry() const;

    template<typename T, typename Fn>
    void Patch(entt::entity entity, Fn&& fn);

    template<typename T, typename... Args>
    T& AddComponent(entt::entity entity, Args&&... args);

    template<typename T>
    void RemoveComponent(entt::entity entity);

    entt::entity CreateEntity();
    void DestroyEntity(entt::entity entity);

private:
    entt::registry& MutableRegistry();
};
```

外部 pass、editor UI、工具代码如果只是读取组件，使用 const registry：

```cpp
const auto& registry = scene.r();
const auto& light = registry.get<ecs::CLightDirectional>(entity);
```

如果要修改组件字段，使用 `Patch<T>()`：

```cpp
scene.Patch<ecs::CLightDirectional>(entity, [&](auto& light) {
    light.intensity = exposure;
});
```

不推荐外部代码这样写：

```cpp
auto& light = scene.r().get<ecs::CLightDirectional>(entity);
light.intensity = exposure;
scene.r().emplace_or_replace<ecs::CTagNeedUpdateLight>(entity);
```

## Patch 不能覆盖的情况

`Patch<T>()` 只适合修改已有组件字段。它不能覆盖 ECS 结构变化。

这些操作需要单独 API：

- 创建 entity。
- 销毁 entity。
- 添加组件。
- 删除组件。
- 创建 light / camera / renderable / material / texture。
- 释放或失效 render-side slot。
- 触发局部 cache rebuild。

结构变化 API 负责挂 `CTagNeedCreateXXX`、`CTagNeedDestroyXXX`、`CTagNeedRebuildXXX` 等同步标记。

当前 Light 创建先落地 `Scene::CreatePointLight(const PointLightCreateInfo&)`，实现位于 `SceneLightApi.cpp`。该 API 负责创建 entity，挂 `CNode`、`CTransform`、`CLight`、`CLightPoint`、`CName`，接入父节点或 root node，并挂 `CTagNeedCreateLight` 与 `CTagNeedUpdateTransform`。调用方不需要手动补 render-side tag。

## Dirty / Tag 语义

当前约定：

- `CTransform::is_dirty`：逻辑侧 derived data dirty，用于更新 `d_world_transform` 和 `d_aabb`。
- `CLightDirectional::is_dirty` / `CLightPoint::is_dirty`：light derived data dirty，用于更新 `d_direction` / `d_position`。
- `CTagNeedUpdateXXX`：已有 render-side slot 的原地更新，不改变数组布局。
- `CTagNeedCreateXXX`：Logical entity 尚无 render-side slot，需要创建缓存项并分配索引。
- `CTagNeedDestroyXXX`：未来用于释放或失效 render-side slot。
- `CTagNeedRebuildXXX`：未来用于复杂结构变化的局部重建兜底。

`NeedUpdate` 不处理新增。新增必须走 `NeedCreate`，避免数据更新和结构变化混在一起。

## Light 和 Transform 的特殊关系

Light 的权威 transform 数据来自 `CTransform`。Light component 里只保存渲染侧常用 derived cache：

- `CLightDirectional::d_direction`
- `CLightPoint::d_position`

修改 light 参数时：

```cpp
scene.Patch<ecs::CLightDirectional>(entity, [](auto& light) {
    light.color = color;
    light.intensity = intensity;
});
```

修改 light transform 时：

```cpp
scene.Patch<ecs::CTransform>(entity, [](auto& transform) {
    transform.rotation = rotation;
});
```

`Patch<CTransform>` 应负责：

- 置位 `CTransform::is_dirty`。
- 挂 `CTagNeedUpdateTransform`。
- 如果 entity 上有 light 组件，挂 `CTagNeedUpdateLight`。
- 后续由 `LogicalScene::Update()` 统一更新 world transform、AABB 和 light derived data。
- 如果父节点 transform dirty，`LogicalScene` 会把 dirty 向子节点传播，并为受影响节点挂 `CTagNeedUpdateTransform`，确保子 renderable instance 也能同步。
- 如果子节点 transform dirty，祖先节点的 AABB 也会重新合并，避免层级 AABB 滞后。
- 如果 entity 有 `CTransform` 但没有 `CNode`，这是当前 scene hierarchy 的非法状态，`LogicalScene` 只记录 `LOG_ERROR` 并跳过该 transform update。

## 内部系统例外

以下代码可以访问 mutable registry，但应尽量收敛在内部边界：

- `LogicalScene` 自身的 ECS system，例如 transform/AABB/light derived data 更新。
- loader / scene builder，用于批量构建初始场景。
- `Scene` 的 create / destroy / add / remove / patch API 实现。
- `CpuScene` / `GpuScene` 同步系统在必要时清理 sync tag。更理想的方式是由 `LogicalScene` 或 `Scene` 提供清理 tag 的封装，逐步减少跨层 mutable registry 访问。

普通 renderer pass 不应修改 registry。renderer pass 读取 scene 数据时，优先使用 const registry 或 `Scene` 提供的 const getter。

## 当前代码状态与迁移方向

当前已经落地：

- `Scene::Patch<T>()`：组件级修改入口，内部调用 EnTT `registry.patch<T>()` 后执行 `MarkDirty<T>()`。
- `SceneDirty.cpp`：集中保存 Light / Transform 的 `MarkDirty<T>()` 特化。
- `SceneLightApi.cpp`：提供 `Scene::CreatePointLight()`，用于运行时创建 point light。
- `CpuScene::CreateNeededLights()`：处理 `CTagNeedCreateLight`，为新增 light 分配 CPU render cache slot。
- `CpuScene::UpdateMeshes()`：处理 `CTagNeedUpdateTransform`，根据 transform entity 到 instance slot 的映射更新 `GInstance`，从而同步已有 renderable 的实例矩阵。
- `GpuScene::UpdateLightBuffer()`：同步 CPU light cache 到 GPU light buffer。debug 阶段 light 数量很小，当前采用全量上传；buffer 不足时重建并更新 bindless handle。后续需要替换为 capacity/chunk 策略和局部更新。
- `GpuScene::UpdateInstanceBuffer()`：同步 CPU instance cache 到 GPU instance buffer，使 GeometryPass 和 GPU culling 使用最新实例矩阵。
- `GpuScene::UpdateRaytracingScene()`：使用更新后的 CPU instance 数据刷新 RT TLAS instance transform；本阶段只更新 transform，不 rebuild BLAS。
- `Scene::Tick()`：设计为每帧可调用的 guarded sync 入口。内部先采样 `NeedUpdate/NeedCreate` tag 生成 `TickState`，再在有同步需求时更新 `LogicalScene -> CpuScene -> GpuScene`。外部通过返回值或 `GetLastTickState()` 读取本帧是否同步、是否更新 transform 等信息，不直接窥探 registry tag。
- `RasterTestCase`：集中保存 Raster 调试用例。当前包含 `TestCase Add Light`、`TestCase Modify Material`、`TestCase Move Renderables`、`TestCase Move Point Lights`。Transform motion 测试只通过 `Scene::Patch<ecs::CTransform>()` 写入，不直接修改 registry。
- `RaytracingRenderer` 中方向光参数和 transform 的外部修改已迁移到 `Scene::Patch<T>()`。

当前代码中 `Scene::r()` 和 `LogicalScene::r()` 仍然公开 mutable registry，这是历史接口。后续迁移目标是：

1. 给结构变化增加 `CreateEntity`、`DestroyEntity`、`AddComponent<T>`、`RemoveComponent<T>` 或更高层 create/destroy API。
2. 将 public `Scene::r()` 收敛为 const 版本；mutable registry 仅留在内部 API。
3. 检查 renderer pass，确保只读路径不依赖 mutable registry。
4. 将 material create / texture handle rebuild、mesh / renderable create/update/destroy 路径补齐到同一套 `Patch + NeedCreate/NeedUpdate/NeedRebuild` 语义。
5. 后续实现 `NeedDestroy` / `NeedRebuild` 时，同步更新本文档。

## 文档同步要求

之后如果修改以下内容，必须同步更新本文：

- `Scene` / `LogicalScene` 暴露的 registry 访问策略。
- `Patch<T>()` / `MarkDirty<T>()` 的语义。
- `CTagNeedUpdateXXX`、`CTagNeedCreateXXX`、`CTagNeedDestroyXXX`、`CTagNeedRebuildXXX` 的使用规则。
- Light / Transform / Material 等组件的 dirty 规则。
- `LogicalScene -> CpuScene -> GpuScene` 的同步流程。
- 哪些模块允许访问 mutable registry。

本文作为 Scene API 和 scene update 设计的基准文档。代码可以渐进迁移，但新设计不应和本文冲突。