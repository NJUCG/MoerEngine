"""Compare the GPU integration test's 256x256 SH3 PLY output with a double precision
CPU implementation. Requires numpy and Pillow. The CPU projects covariance matrices
and composites depth-sorted pixel patches without GPU scan, radix sorting or buffers.
Usage: python ReferenceGaussianPly.py asset.ply capture.ppm
This is an independent numeric reference, not an invocation of INRIA's renderer.
"""
import numpy as np
from pathlib import Path
from PIL import Image
import json, sys, time
asset = Path(sys.argv[1])
image = Path(sys.argv[2])
start = time.monotonic()
with asset.open('rb') as f:
    names = []
    while True:
        line = f.readline().decode().strip()
        if line.startswith('element vertex '):
            count = int(line.split()[-1])
        if line.startswith('property float '):
            names.append(line.split()[-1])
        if line == 'end_header':
            break
    data = np.fromfile(f, dtype='<f4', count=count * len(names)).reshape(count, len(names)).astype(np.float64)

def field(name):
    return data[:, names.index(name)]
points = np.stack([field(a) for a in ('x', 'y', 'z')], axis=-1)
values = np.loadtxt(str(image) + '.camera.txt', max_rows=2)
view = values[0].reshape(4, 4)
projection = values[1].reshape(4, 4)
eye = np.loadtxt(str(image) + '.camera.txt', skiprows=2)
w = h = 256
xyz = points @ view[:3, :3].T + view[:3, 3]
depth = -xyz[:, 2]
clip = np.c_[xyz, np.ones(count)] @ projection.T
centers = clip[:, :2] / clip[:, 3:] * np.array([w / 2, -h / 2]) + np.array([(w - 1) / 2, (h - 1) / 2])
q = np.stack([field('rot_' + str(gaussian_id)) for gaussian_id in range(4)], axis=-1)
q /= np.linalg.norm(q, axis=1)[:, None]
qw, qx, qy, qz = q.T
rotation = np.stack([1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw), 2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw), 2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)], axis=-1).reshape(count, 3, 3)
scales = np.exp(np.stack([field('scale_' + str(gaussian_id)) for gaussian_id in range(3)], axis=-1))
cov_world = rotation * scales[:, None, :] @ np.transpose(rotation * scales[:, None, :], (0, 2, 1))
cov_view = view[:3, :3] @ cov_world @ view[:3, :3].T
linear = xyz.copy()
linear[:, 0] = np.clip(xyz[:, 0] / depth, -1.3 / abs(projection[0, 0]), 1.3 / abs(projection[0, 0])) * depth
linear[:, 1] = np.clip(xyz[:, 1] / depth, -1.3 / abs(projection[1, 1]), 1.3 / abs(projection[1, 1])) * depth
j = np.zeros((count, 2, 3))
fx = projection[0, 0] * w / 2
fy = -projection[1, 1] * h / 2
j[:, 0, 0] = fx / depth
j[:, 0, 2] = fx * linear[:, 0] / depth ** 2
j[:, 1, 1] = fy / depth
j[:, 1, 2] = fy * linear[:, 1] / depth ** 2
cov = j @ cov_view @ np.transpose(j, (0, 2, 1)) + np.eye(2) * 0.3
inverse = np.linalg.inv(cov)
mid = (cov[:, 0, 0] + cov[:, 1, 1]) * 0.5
det = np.linalg.det(cov)
radius = np.ceil(3 * np.sqrt(mid + np.sqrt(np.maximum(0.1, mid * mid - det))))
rect_min = np.clip(np.floor((centers - radius[:, None]) / 16), 0, [w // 16, h // 16]).astype(int) * 16
rect_max = np.clip(np.ceil((centers + radius[:, None]) / 16), 0, [w // 16, h // 16]).astype(int) * 16
opacity = 1 / (1 + np.exp(-field('opacity')))
direction = points - eye
direction /= np.linalg.norm(direction, axis=1)[:, None]
x, y, z = direction.T
basis = np.stack([np.ones(count) * 0.28209479177387814, -0.4886025119029199 * y, 0.4886025119029199 * z, -0.4886025119029199 * x, 1.0925484305920792 * x * y, -1.0925484305920792 * y * z, 0.31539156525252005 * (2 * z * z - x * x - y * y), -1.0925484305920792 * x * z, 0.5462742152960396 * (x * x - y * y), -0.5900435899266435 * y * (3 * x * x - y * y), 2.890611442640554 * x * y * z, -0.4570457994644658 * y * (4 * z * z - x * x - y * y), 0.3731763325901154 * z * (2 * z * z - 3 * x * x - 3 * y * y), -0.4570457994644658 * x * (4 * z * z - x * x - y * y), 1.445305721320277 * z * (x * x - y * y), -0.5900435899266435 * x * (x * x - 3 * y * y)], axis=-1)
sh = np.zeros((count, 16, 3))
sh[:, 0, :] = np.stack([field('f_dc_' + str(gaussian_id)) for gaussian_id in range(3)], axis=-1)
for channel_id in range(3):
    for coefficient_id in range(15):
        sh[:, coefficient_id + 1, channel_id] = field('f_rest_' + str(channel_id * 15 + coefficient_id))
colors = np.maximum(0.5 + np.einsum('ni,nic->nc', basis, sh), 0)
output = np.zeros((h, w, 3))
transmittance = np.ones((h, w))
is_finished = np.zeros((h, w), dtype=bool)
for gaussian_id in np.argsort(depth, kind='stable'):
    if depth[gaussian_id] <= 0.001 or depth[gaussian_id] >= 100:
        continue
    x0, y0 = rect_min[gaussian_id]
    x1, y1 = rect_max[gaussian_id]
    if x1 <= x0 or y1 <= y0:
        continue
    yy, xx = np.mgrid[y0:y1, x0:x1]
    delta = np.stack([xx - centers[gaussian_id, 0], yy - centers[gaussian_id, 1]], axis=-1)
    power = -0.5 * np.einsum('...i,ij,...j->...', delta, inverse[gaussian_id], delta)
    alpha = np.minimum(0.99, opacity[gaussian_id] * np.exp(power))
    remaining = transmittance[y0:y1, x0:x1]
    active = (alpha >= 1 / 255) & (remaining * (1 - alpha) >= 0.0001) & ~is_finished[y0:y1, x0:x1]
    terminated = (alpha >= 1 / 255) & (remaining * (1 - alpha) < 0.0001)
    contribution = np.where(active, alpha * remaining, 0)
    output[y0:y1, x0:x1] += contribution[..., None] * colors[gaussian_id]
    remaining[:] = np.where(active, remaining * (1 - alpha), remaining)
    is_finished[y0:y1, x0:x1] |= terminated
gpu = np.fromfile(str(image) + '.rgba', dtype='<f4').reshape(h, w, 4)
difference = np.abs(gpu[:, :, :3] - output)
rmse = np.sqrt(np.mean(difference ** 2))
max_error = float(difference.max())
psnr = float(-20 * np.log10(max(rmse, 1e-15)))
Image.fromarray(np.rint(np.clip(output, 0, 1) * 255).astype(np.uint8)).save(str(image) + '.reference.png')
Image.fromarray(np.rint(np.clip(gpu[:, :, :3], 0, 1) * 255).astype(np.uint8)).save(str(image) + '.png')
report = {'asset': str(asset), 'gaussian_count': count, 'max_abs_rgb_error': max_error, 'rmse': float(rmse), 'psnr_db': psnr, 'seconds': time.monotonic() - start, 'passed': bool(max_error < 0.003 and psnr > 65)}
Path(str(image) + '.comparison.json').write_text(json.dumps(report, indent=2))
print(report)
if not report['passed']:
    sys.exit(1)
