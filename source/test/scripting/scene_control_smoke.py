import moer

# ScriptHost bootstraps `scene` before user code runs, but binding it here keeps
# static analysis aligned with the runtime contract.
scene = moer.scene()


def dump_transform(label, transform):
    if transform is None:
        print(f"[py] {label}: <none>")
        return

    print(f"[py] {label}.translation = {transform.translation}")
    print(f"[py] {label}.rotation = {transform.rotation}")
    print(f"[py] {label}.scale = {transform.scale}")


root = scene.get_root_node_entity()
print(f"[py] root entity = {root}")
print(f"[py] root valid = {scene.is_valid_node_entity(root)}")

before_name = scene.get_node_display_name(root)
before_transform = scene.try_get_node_local_transform(root)
print(f"[py] before name = {before_name}")
dump_transform("before", before_transform)

assert before_transform is not None, "Root node local transform is not available"
assert scene.set_node_name(root, "ScriptedRoot"), "set_node_name failed"
assert scene.set_node_translation(root, moer.float3(1.0, 2.0, 3.0)), "set_node_translation failed"
assert scene.set_node_rotation(root, moer.Quaternion(1.0, 0.0, 0.0, 0.0)), "set_node_rotation failed"
assert scene.set_node_scale(root, moer.float3(1.25, 1.5, 1.75)), "set_node_scale failed"

after_name = scene.get_node_display_name(root)
after_transform = scene.try_get_node_local_transform(root)
print(f"[py] after name = {after_name}")
dump_transform("after", after_transform)
print("[py] scene control smoke done")
