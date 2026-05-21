import time

import moer

scene = moer.scene()

MIZUKI_PATH = "asset/scenes/mizuki/mizuki.gltf"
EPSILON = 1.0e-4


def nearly_equal(lhs: float, rhs: float, epsilon: float = EPSILON) -> bool:
    return abs(lhs - rhs) <= epsilon


def assert_float3(actual: moer.float3, expected: moer.float3, label: str) -> None:
    assert nearly_equal(actual.x, expected.x), f"{label}.x mismatch: {actual.x} != {expected.x}"
    assert nearly_equal(actual.y, expected.y), f"{label}.y mismatch: {actual.y} != {expected.y}"
    assert nearly_equal(actual.z, expected.z), f"{label}.z mismatch: {actual.z} != {expected.z}"


def assert_quaternion(actual: moer.Quaternion, expected: moer.Quaternion, label: str) -> None:
    assert nearly_equal(actual.w, expected.w), f"{label}.w mismatch: {actual.w} != {expected.w}"
    assert nearly_equal(actual.x, expected.x), f"{label}.x mismatch: {actual.x} != {expected.x}"
    assert nearly_equal(actual.y, expected.y), f"{label}.y mismatch: {actual.y} != {expected.y}"
    assert nearly_equal(actual.z, expected.z), f"{label}.z mismatch: {actual.z} != {expected.z}"


def dump_transform(label: str, transform: moer.NodeLocalTransform | None) -> None:
    print(f"[py] {label} = {transform}")
    if transform is None:
        return

    print(f"[py] {label}.translation = {transform.translation}")
    print(f"[py] {label}.rotation = {transform.rotation}")
    print(f"[py] {label}.scale = {transform.scale}")


def dump_stats(label: str, stats: moer.NodeSubtreeStats) -> None:
    print(
        f"[py] {label}: node_count={stats.node_count}, renderable_count={stats.renderable_count}, "
        f"camera_count={stats.camera_count}, light_count={stats.light_count}, "
        f"contains_main_camera={stats.contains_main_camera}, "
        f"contains_main_light_tag={stats.contains_main_light_tag}"
    )


print(f"[py] source path = {scene.get_source_file_path()}")
print(f"[py] is_start_loading = {scene.is_start_loading()}")
print(f"[py] is_ready = {scene.is_ready()}")
assert scene.is_ready(), "Scene should be ready before running the all-in-one script test"
assert scene.get_source_file_path() != "", "Source file path should not be empty"

root = scene.get_root_node_entity()
assert scene.is_valid_node_entity(root), "Root node should be valid"
assert scene.is_root_node(root), "Root node should report is_root_node == True"

baseline_root_child_count = scene.get_node_child_count(root)
baseline_root_stats = scene.get_node_subtree_stats(root)
root_name = scene.try_get_node_name(root)
root_transform = scene.try_get_node_local_transform(root)
print(f"[py] root entity = {root}")
print(f"[py] root name = {root_name}")
dump_transform("root_transform", root_transform)
dump_stats("root_stats_before", baseline_root_stats)
assert root_name is not None, "Root node name should be available"
assert root_transform is not None, "Root node transform should be available"

main_camera = scene.get_main_camera_entity()
main_directional_light = scene.get_main_directional_light_entity()
assert scene.is_valid_node_entity(main_camera), "Main camera entity should be a valid node"
assert (
    scene.is_valid_node_entity(main_directional_light)
), "Main directional light entity should be a valid node"
print(f"[py] main camera entity = {main_camera}")
print(f"[py] main directional light entity = {main_directional_light}")

plain_entity = scene.create_entity("Py Plain Entity")
assert scene.try_get_node_name(plain_entity) is None, "Plain entity should not expose node name"
assert (
    scene.try_get_node_local_transform(plain_entity) is None
), "Plain entity should not expose node local transform"
print(f"[py] plain entity = {plain_entity}")

parent_a_info = moer.EntityWithNodeCreateInfo()
parent_a_info.name = "Py Parent A"
parent_a_info.translation = moer.float3(-2.0, 0.0, 0.0)
parent_a = scene.create_entity_with_node(parent_a_info)
assert scene.is_valid_node_entity(parent_a), "Parent A should be a valid node"

parent_b_info = moer.EntityWithNodeCreateInfo()
parent_b_info.name = "Py Parent B"
parent_b_info.translation = moer.float3(2.0, 0.0, 0.0)
parent_b = scene.create_entity_with_node(parent_b_info)
assert scene.is_valid_node_entity(parent_b), "Parent B should be a valid node"

child_info = moer.EntityWithNodeCreateInfo()
child_info.parent_node_entity = parent_a
child_info.name = "Py Child"
child_info.translation = moer.float3(0.0, 1.0, 0.0)
child = scene.create_entity_with_node(child_info)
assert scene.is_valid_node_entity(child), "Child should be a valid node"

assert (
    scene.get_node_child_count(root) == baseline_root_child_count + 2
), "Root child count should increase by two after creating two root children"
assert scene.get_node_child_count(parent_a) == 1, "Parent A should own one child"
assert scene.get_node_child_count(parent_b) == 0, "Parent B should start with zero children"
parent_a_stats = scene.get_node_subtree_stats(parent_a)
dump_stats("parent_a_stats_after_create", parent_a_stats)
assert parent_a_stats.node_count == 2, "Parent A subtree should contain parent and child"
assert scene.try_get_node_name(parent_a) == "Py Parent A", "Parent A name mismatch"
assert scene.try_get_node_name(child) == "Py Child", "Child name mismatch"

assert scene.set_node_name(child, "Py Child Renamed"), "set_node_name should succeed for child"
assert (
    scene.set_node_translation(child, moer.float3(0.25, 1.5, -0.5))
), "set_node_translation should succeed for child"
assert (
    scene.set_node_rotation(child, moer.Quaternion(0.9238795, 0.0, 0.3826834, 0.0))
), "set_node_rotation should succeed for child"
assert scene.set_node_scale(child, moer.float3(0.8, 1.2, 1.4)), "set_node_scale should succeed for child"

child_transform = scene.try_get_node_local_transform(child)
assert child_transform is not None, "Child local transform should be available after direct setters"
assert scene.try_get_node_name(child) == "Py Child Renamed", "Child renamed value mismatch"
assert_float3(child_transform.translation, moer.float3(0.25, 1.5, -0.5), "child_transform.translation")
assert_quaternion(
    child_transform.rotation,
    moer.Quaternion(0.9238795, 0.0, 0.3826834, 0.0),
    "child_transform.rotation",
)
assert_float3(child_transform.scale, moer.float3(0.8, 1.2, 1.4), "child_transform.scale")

composite_transform = moer.Transform(
    moer.float3(0.5, 2.0, -0.25),
    moer.float3(0.75, 1.25, 1.5),
    moer.Quaternion(0.70710677, 0.0, 0.70710677, 0.0),
)
assert composite_transform.is_affine(), "Composite transform should be affine"
assert scene.set_local_transform(child, composite_transform), "set_local_transform should succeed for child"

child_transform = scene.try_get_node_local_transform(child)
assert child_transform is not None, "Child local transform should be available after set_local_transform"
assert_float3(child_transform.translation, moer.float3(0.5, 2.0, -0.25), "child_transform_after.translation")
assert_float3(child_transform.scale, moer.float3(0.75, 1.25, 1.5), "child_transform_after.scale")
assert_quaternion(
    child_transform.rotation,
    moer.Quaternion(0.70710677, 0.0, 0.70710677, 0.0),
    "child_transform_after.rotation",
)

assert scene.attach_to_parent(child, parent_b), "attach_to_parent should succeed"
assert scene.get_node_child_count(parent_a) == 0, "Parent A should have zero children after reparent"
assert scene.get_node_child_count(parent_b) == 1, "Parent B should have one child after reparent"

assert scene.detach_from_parent(child), "detach_from_parent should succeed"
assert scene.get_node_child_count(parent_b) == 0, "Parent B should have zero children after detach"
assert (
    scene.get_node_child_count(root) == baseline_root_child_count + 3
), "Root child count should reflect detached child reattached to root"

point_light_info = moer.PointLightCreateInfo()
point_light_info.name = "Py Point Light"
point_light_info.position = moer.float3(0.0, 3.0, 0.0)
point_light_info.color = moer.float3(1.0, 0.8, 0.6)
point_light_info.intensity = 16.0
point_light_info.should_set_main_light = True
point_light = scene.create_point_light(point_light_info)
assert scene.is_valid_node_entity(point_light), "Created point light should be a valid node"
main_point_light = scene.get_main_point_light_entity()
assert scene.is_valid_node_entity(main_point_light), "Main point light entity should be valid after creation"
print(f"[py] point light entity = {point_light}, main point light entity = {main_point_light}")

procedural_info = moer.ProceduralMeshCreateInfo()
procedural_info.shape = moer.EProceduralPrimitiveShape.Cube
procedural_info.parent_node_entity = parent_b
procedural_info.name = "Py Procedural Renderable"
procedural_info.translation = moer.float3(0.0, 0.5, 0.0)
procedural_info.material.name = "Py Procedural Material"
procedural_info.material.albedo_factor = moer.float4(0.2, 0.4, 0.8, 1.0)
procedural_result = scene.create_procedural_renderable(procedural_info)
assert bool(procedural_result), "create_procedural_renderable should succeed"
assert (
    scene.is_valid_node_entity(procedural_result.renderable_entity)
), "Procedural renderable entity should be a valid node"
parent_b_stats = scene.get_node_subtree_stats(parent_b)
dump_stats("parent_b_stats_after_procedural", parent_b_stats)
assert parent_b_stats.renderable_count >= 1, "Parent B subtree should contain a renderable"
assert (
    scene.destroy_renderable(procedural_result.renderable_entity)
), "destroy_renderable should succeed for the procedural renderable"
time.sleep(0.1)

import_result = scene.import_scene_from_file(MIZUKI_PATH)
print(
    f"[py] import success = {import_result.success}, root = {import_result.import_root_entity}, "
    f"entities = {import_result.imported_entity_count}"
)
assert bool(import_result), f"Import failed: {import_result.error_message}"
import_root = import_result.import_root_entity
assert scene.is_valid_node_entity(import_root), "Imported root should be a valid node"
import_stats = scene.get_node_subtree_stats(import_root)
dump_stats("imported_root_stats", import_stats)
assert import_stats.node_count > 0, "Imported subtree should contain at least one node"
assert scene.destroy_node_subtree(import_root), "destroy_node_subtree should succeed for imported root"
assert not scene.is_valid_node_entity(import_root), "Imported root should be invalid after subtree destruction"

assert scene.destroy_point_light(point_light), "destroy_point_light should succeed"
time.sleep(0.1)

assert scene.destroy_entity(plain_entity), "destroy_entity should succeed for the plain entity"
assert scene.destroy_entity(parent_a), "destroy_entity should succeed for Parent A"
assert scene.destroy_entity(parent_b), "destroy_entity should succeed for Parent B"
assert scene.destroy_entity(child), "destroy_entity should succeed for detached child"

final_root_stats = scene.get_node_subtree_stats(root)
dump_stats("root_stats_after", final_root_stats)
assert scene.is_valid_node_entity(root), "Root node should remain valid after the full script test"
print("[py] scene api all 260514 done")
