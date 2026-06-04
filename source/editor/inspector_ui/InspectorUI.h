#pragma once

#include "Core.h"
#include "misc/Traits.h"

#include <entt/entity/entity.hpp>

namespace Moer {

class Scene;

/**
 * InspectorUI 负责编辑当前选中 CNode 的基础属性。
 *
 * 结构:
 * - 只关心一个 selected_node
 * - 当前支持 name / position / rotation / scale
 * - 内部缓存欧拉角，底层仍写回 quaternion
 *
 * 改这里:
 * - 加新的 inspector 字段: InspectorUI.cpp + 对应组件定义
 * - 改节点编辑规则: Scene 节点编辑 API / LogicalComponents.h / SceneModify.cpp
 * - 改显示文案或布局: InspectorUI.cpp
 *
 * 用法:
 * - 由 EditorUI::ShowInspector() 调用 ShowWindow()
 * - 外部传入 Scene 和当前选中 entt::entity，不自己持有场景
 *
 * 边界约束:
 * - 只通过 Scene query/edit API 读取和写回选中节点属性
 * - 不直接访问 registry / LogicalScene；如果能力不够，应先补 Scene 正式 API
 */
class InspectorUI {
public:
    void ShowWindow(bool* p_open, Scene* scene, entt::entity& selected_node);

private:
    entt::entity m_rotation_cache_entity = entt::null;
    float3       m_rotation_euler{};
};

} // namespace Moer