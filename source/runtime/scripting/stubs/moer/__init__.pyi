from __future__ import annotations

from enum import Enum


class EAlphaMode(Enum):
    Opaque: EAlphaMode
    Mask: EAlphaMode
    Blend: EAlphaMode


class EProceduralPrimitiveShape(Enum):
    Cube: EProceduralPrimitiveShape
    FacetedSphere: EProceduralPrimitiveShape

class EGaussianDisplayMode(Enum):
    Reference: EGaussianDisplayMode
    Engine: EGaussianDisplayMode

class EGaussianDebugView(Enum):
    Color: EGaussianDebugView
    Transmittance: EGaussianDebugView
    TileCoverage: EGaussianDebugView

class GaussianViewSettings:
    background: float3
    exposure_ev: float
    display_mode: EGaussianDisplayMode
    debug_view: EGaussianDebugView
    tile_pair_capacity: int
    def __init__(self) -> None: ...

class GaussianExportStatus:
    request_id: int
    is_busy: bool
    is_successful: bool
    message: str

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


class Transform:
    translation: float3
    scale: float3
    rotation: Quaternion

    def __init__(
        self,
        translation: float3 = ...,
        scale: float3 = ...,
        rotation: Quaternion = ...,
    ) -> None: ...
    def is_affine(self) -> bool: ...
    def __repr__(self) -> str: ...


class NodeLocalTransform:
    translation: float3
    rotation: Quaternion
    scale: float3

    def __init__(self) -> None: ...


class NodeSubtreeStats:
    node_count: int
    renderable_count: int
    camera_count: int
    light_count: int
    contains_main_camera: bool
    contains_main_light_tag: bool

    def __init__(self) -> None: ...


class EntityComponentFlags:
    is_valid_entity: bool
    is_node: bool
    is_root_node: bool
    is_renderable: bool
    is_camera: bool
    is_light: bool
    is_directional_light: bool
    is_point_light: bool
    is_main_camera: bool
    is_main_light: bool

    def __init__(self) -> None: ...


class SceneImportResult:
    success: bool
    error_message: str
    import_root_entity: Entity
    imported_entity_count: int

    def __init__(self) -> None: ...
    def __bool__(self) -> bool: ...


class PointLightCreateInfo:
    position: float3
    color: float3
    intensity: float
    name: str
    parent_node_entity: Entity
    should_set_main_light: bool

    def __init__(self) -> None: ...


class EntityWithNodeCreateInfo:
    parent_node_entity: Entity
    name: str
    translation: float3
    rotation: Quaternion
    scale: float3

    def __init__(self) -> None: ...


class MaterialCreateInfo:
    name: str
    albedo_factor: float4
    emissive_factor: float3
    metallic_factor: float
    roughness_factor: float
    alpha_mode: EAlphaMode
    alpha_cutoff: float

    def __init__(self) -> None: ...


class ProceduralMeshCreateInfo:
    shape: EProceduralPrimitiveShape
    parent_node_entity: Entity
    name: str
    translation: float3
    rotation: Quaternion
    scale: float3
    material: MaterialCreateInfo

    def __init__(self) -> None: ...


class CreateProceduralRenderableResult:
    material_entity: Entity
    primitive_entity: Entity
    mesh_entity: Entity
    renderable_entity: Entity

    def __init__(self) -> None: ...
    def __bool__(self) -> bool: ...


Entity = int


class SceneApi:
    def request_gaussian_image_export(self, path: str) -> bool: ...
    def get_gaussian_image_export_status(self) -> GaussianExportStatus: ...
    def get_gaussian_view_settings(self) -> GaussianViewSettings: ...
    def set_gaussian_view_settings(self, settings: GaussianViewSettings) -> bool: ...
    def save_gaussian_document(self, path: str) -> bool: ...
    def duplicate_gaussian_instance(self, entity: Entity) -> Entity: ...
    def get_gaussian_scene_content(self) -> EGaussianSceneContent: ...
    def get_gaussian_instance_settings(self, entity: Entity) -> GaussianInstanceSettings | None: ...
    def set_gaussian_instance_settings(self, entity: Entity, settings: GaussianInstanceSettings) -> bool: ...
    def destroy_gaussian_instance(self, entity: Entity) -> bool: ...
    def get_source_file_path(self) -> str: ...
    def is_start_loading(self) -> bool: ...
    def is_ready(self) -> bool: ...
    def get_root_node_entity(self) -> Entity: ...
    def is_valid_node_entity(self, entity: Entity) -> bool: ...
    def is_valid_entity(self, entity: Entity) -> bool: ...
    def is_root_node(self, entity: Entity) -> bool: ...
    def get_node_child_count(self, entity: Entity) -> int: ...
    def get_node_child_entity(self, entity: Entity, child_index: int) -> Entity: ...
    def list_node_children(self, entity: Entity) -> list[Entity]: ...
    def get_entity_component_flags(self, entity: Entity) -> EntityComponentFlags: ...
    def get_node_display_name(self, entity: Entity) -> str: ...
    def find_node_entity_by_name(self, name: str) -> Entity: ...
    def get_node_subtree_stats(self, entity: Entity) -> NodeSubtreeStats: ...
    def try_get_node_name(self, entity: Entity) -> str | None: ...
    def try_get_node_local_transform(self, entity: Entity) -> NodeLocalTransform | None: ...
    def import_scene_from_file(self, file_path: str) -> SceneImportResult: ...
    def get_main_camera_entity(self) -> Entity: ...
    def get_main_directional_light_entity(self) -> Entity: ...
    def get_main_point_light_entity(self) -> Entity: ...
    def create_entity(self, name: str = ...) -> Entity: ...
    def create_entity_with_node(self, create_info: EntityWithNodeCreateInfo) -> Entity: ...
    def create_point_light(self, create_info: PointLightCreateInfo) -> Entity: ...
    def create_procedural_renderable(
        self, create_info: ProceduralMeshCreateInfo
    ) -> CreateProceduralRenderableResult: ...
    def set_node_name(self, entity: Entity, name: str) -> bool: ...
    def set_node_translation(self, entity: Entity, value: float3) -> bool: ...
    def set_node_rotation(self, entity: Entity, value: Quaternion) -> bool: ...
    def set_node_scale(self, entity: Entity, value: float3) -> bool: ...
    def set_local_transform(self, entity: Entity, value: Transform) -> bool: ...
    def attach_to_parent(self, child_entity: Entity, parent_entity: Entity) -> bool: ...
    def detach_from_parent(self, child_entity: Entity) -> bool: ...
    def destroy_entity(self, entity: Entity) -> bool: ...
    def destroy_node_subtree(self, entity: Entity) -> bool: ...
    def destroy_renderable(self, entity: Entity) -> bool: ...
    def destroy_point_light(self, entity: Entity) -> bool: ...


def scene() -> SceneApi: ...


class EGaussianSceneContent(Enum):
    Empty: EGaussianSceneContent
    MeshOnly: EGaussianSceneContent
    GaussianOnly: EGaussianSceneContent
    Mixed: EGaussianSceneContent


class GaussianInstanceSettings:
    color_multiplier: float3
    opacity_multiplier: float
    enabled: bool

    def __init__(self) -> None: ...
