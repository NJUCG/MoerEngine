#pragma once

#include "LogicalComponents.h"

#include <cctype>
#include <entt/entt.hpp>
#include <string>
#include <string_view>

namespace Moer::ecs {

enum class ENodeNameKind {
    Root,
    Node,
    Mesh,
    Camera,
    Light,
    DirectionalLight,
    PointLight,
};

inline uint32 GetEntityNumericId(entt::entity entity) {
    return static_cast<uint32>(entt::to_integral(entity));
}

inline bool IsBlankName(std::string_view name) {
    for (unsigned char ch : name) {
        if (!std::isspace(ch)) {
            return false;
        }
    }
    return true;
}

inline std::string MakeIndexedName(std::string_view prefix, entt::entity entity) {
    return std::string(prefix) + std::to_string(GetEntityNumericId(entity));
}

inline ENodeNameKind GetNodeNameKind(const entt::registry& registry, entt::entity entity) {
    if (registry.all_of<CTagRootNode>(entity)) {
        return ENodeNameKind::Root;
    }
    if (registry.all_of<CCamera>(entity)) {
        return ENodeNameKind::Camera;
    }
    if (registry.all_of<CLightPoint>(entity)) {
        return ENodeNameKind::PointLight;
    }
    if (registry.all_of<CLightDirectional>(entity)) {
        return ENodeNameKind::DirectionalLight;
    }
    if (registry.all_of<CLight>(entity)) {
        return ENodeNameKind::Light;
    }
    if (registry.all_of<CRenderable>(entity)) {
        return ENodeNameKind::Mesh;
    }
    return ENodeNameKind::Node;
}

inline std::string MakeDefaultNodeName(ENodeNameKind kind, entt::entity entity) {
    switch (kind) {
        case ENodeNameKind::Root:
            return "RootNode";
        case ENodeNameKind::Mesh:
            return MakeIndexedName("Mesh", entity);
        case ENodeNameKind::Camera:
            return MakeIndexedName("Camera", entity);
        case ENodeNameKind::Light:
            return MakeIndexedName("Light", entity);
        case ENodeNameKind::DirectionalLight:
            return MakeIndexedName("DirectionalLight", entity);
        case ENodeNameKind::PointLight:
            return MakeIndexedName("PointLight", entity);
        case ENodeNameKind::Node:
        default:
            return MakeIndexedName("Node", entity);
    }
}

inline std::string MakeDefaultNodeName(const entt::registry& registry, entt::entity entity) {
    return MakeDefaultNodeName(GetNodeNameKind(registry, entity), entity);
}

inline void EnsureNodeName(entt::registry& registry, entt::entity entity, std::string_view preferred = {}) {
    if (!registry.all_of<CNode>(entity)) {
        return;
    }

    auto& node = registry.get<CNode>(entity);
    if (!IsBlankName(preferred)) {
        node.name = std::string(preferred);
    }
    if (IsBlankName(node.name)) {
        node.name = MakeDefaultNodeName(registry, entity);
    }
}

inline std::string GetNodeDisplayName(const CNode& node, entt::entity entity) {
    return IsBlankName(node.name) ? MakeIndexedName("Node", entity) : node.name;
}

inline std::string MakeDebugName(std::string_view prefix, entt::entity entity) {
    return MakeIndexedName(prefix, entity);
}

} // namespace Moer::ecs