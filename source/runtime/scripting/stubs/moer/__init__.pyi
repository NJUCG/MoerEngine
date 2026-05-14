from __future__ import annotations

class float2:
    x: float
    y: float

    def __init__(self, x: float = 0.0, y: float = 0.0) -> None: ...
    def __repr__(self) -> str: ...


class float3:
    x: float
    y: float
    z: float

    def __init__(self, x: float = 0.0, y: float = 0.0, z: float = 0.0) -> None: ...
    def __repr__(self) -> str: ...


class float4:
    x: float
    y: float
    z: float
    w: float

    def __init__(self, x: float = 0.0, y: float = 0.0, z: float = 0.0, w: float = 0.0) -> None: ...
    def __repr__(self) -> str: ...


class Quaternion:
    vec: float4
    x: float
    y: float
    z: float
    w: float

    def __init__(self, w: float = 0.0, x: float = 0.0, y: float = 0.0, z: float = 0.0) -> None: ...
    def __repr__(self) -> str: ...


class NodeLocalTransform:
    translation: float3
    rotation: Quaternion
    scale: float3

    def __init__(self) -> None: ...


Entity = int


class SceneApi:
    def get_root_node_entity(self) -> Entity: ...
    def is_valid_node_entity(self, entity: Entity) -> bool: ...
    def is_valid_entity(self, entity: Entity) -> bool: ...
    def get_node_display_name(self, entity: Entity) -> str: ...
    def try_get_node_local_transform(self, entity: Entity) -> NodeLocalTransform | None: ...
    def set_node_name(self, entity: Entity, name: str) -> bool: ...
    def set_node_translation(self, entity: Entity, value: float3) -> bool: ...
    def set_node_rotation(self, entity: Entity, value: Quaternion) -> bool: ...
    def set_node_scale(self, entity: Entity, value: float3) -> bool: ...


def scene() -> SceneApi: ...
