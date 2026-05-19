import moer

print("[py] remote engine loopback demo started")

scene = moer.scene()
root = scene.get_root_node_entity()

print(f"[py] root entity = {root}")
print(f"[py] root valid = {scene.is_valid_node_entity(root)}")
print(f"[py] before name = {scene.get_node_display_name(root)}")

assert scene.set_node_name(root, "RemoteLoopbackRoot")
assert scene.set_node_translation(root, moer.float3(4.0, 5.0, 6.0))

print(f"[py] after name = {scene.get_node_display_name(root)}")
print("[py] remote engine loopback demo finished")