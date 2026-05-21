import math
import time

import moer

scene = moer.scene()

MIZUKI_PATH = "asset/scenes/mizuki/mizuki.gltf"
PHASE_DURATION_SECONDS = 5.0
PHASE_2_DURATION_SECONDS = 60.0
FRAME_INTERVAL_SECONDS = 1.0 / 60.0
ANGULAR_SPEED = math.radians(90.0)
BOB_AMPLITUDE = 0.75
BOB_OMEGA = math.tau / 2.0


def make_y_rotation(angle_radians: float) -> moer.Quaternion:
    half_angle = 0.5 * angle_radians
    return moer.Quaternion(math.cos(half_angle), 0.0, math.sin(half_angle), 0.0)


def multiply_quaternion(lhs: moer.Quaternion, rhs: moer.Quaternion) -> moer.Quaternion:
    return moer.Quaternion(
        lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
        lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
        lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


print(f"[py] importing scene from: {MIZUKI_PATH}")
import_result = scene.import_scene_from_file(MIZUKI_PATH)
print(
    f"[py] import success = {import_result.success}, "
    f"root = {import_result.import_root_entity}, entities = {import_result.imported_entity_count}"
)
require(bool(import_result), f"Import failed: {import_result.error_message}")

target = import_result.import_root_entity
require(scene.is_valid_node_entity(target), "Imported root entity is invalid")

target_name = scene.get_node_display_name(target)
base_transform = scene.try_get_node_local_transform(target)
if base_transform is None:
    raise RuntimeError("Imported root local transform is unavailable")

base_translation = moer.float3(
    base_transform.translation.x,
    base_transform.translation.y,
    base_transform.translation.z,
)
base_rotation = moer.Quaternion(
    base_transform.rotation.w,
    base_transform.rotation.x,
    base_transform.rotation.y,
    base_transform.rotation.z,
)

print(f"[py] target name = {target_name}")
print(f"[py] base translation = {base_translation}")
print(f"[py] base rotation = {base_rotation}")
print("[py] phase 1 start: rotate around Y axis")

animation_start = time.monotonic()
phase2_start = animation_start + PHASE_DURATION_SECONDS
animation_end = phase2_start + PHASE_2_DURATION_SECONDS
phase2_started = False

while True:
    now = time.monotonic()
    total_elapsed = now - animation_start
    if now >= animation_end:
        break

    angle = ANGULAR_SPEED * total_elapsed
    animated_rotation = multiply_quaternion(make_y_rotation(angle), base_rotation)
    require(scene.set_node_rotation(target, animated_rotation), "set_node_rotation failed")

    if now < phase2_start:
        require(scene.set_node_translation(target, base_translation), "set_node_translation failed")
    else:
        if not phase2_started:
            phase2_started = True
            print("[py] phase 2 start: keep rotating and add sinusoidal Y translation")

        phase2_elapsed = now - phase2_start
        bob_offset = BOB_AMPLITUDE * math.sin(BOB_OMEGA * phase2_elapsed)
        translation = moer.float3(
            base_translation.x,
            base_translation.y + bob_offset,
            base_translation.z,
        )
        require(scene.set_node_translation(target, translation), "set_node_translation failed")

    time.sleep(FRAME_INTERVAL_SECONDS)

final_transform = scene.try_get_node_local_transform(target)
print("[py] animation finished")
print(f"[py] final transform = {final_transform}")