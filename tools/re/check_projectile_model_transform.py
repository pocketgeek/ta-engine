#!/usr/bin/env python3
"""Compare authored projectile model poses with retail's native XYZ transform.

Retail's 0x535d50 transforms a model-space vertex from the loader's mirrored
X/Z basis with the shot's roll/yaw/pitch BAM words. TAK's model_transform_test
applies its projectile pose to the authored coordinates. This checks the core
mesh orientation independently of the final Glide triangle raster and textures.
"""
import random
import struct
import subprocess
import sys

from emu import HEAP, Icd


binary = sys.argv[1] if len(sys.argv) > 1 else "build-o2/model_transform_test"

p = Icd()
p.freeze_hooks()
source, result, angles = [HEAP + i * 0x10000 for i in range(1, 4)]
rng = random.Random(0x535d50)
rows = []
native_points = []

for _ in range(4096):
    vertex = [rng.randrange(-20 * 65536, 20 * 65536) for _ in range(3)]
    roll, yaw, pitch = (rng.randrange(65536) for _ in range(3))

    # Retail's model loader mirrors the authored X/Z vertex basis before the
    # renderer applies the shot's BAM angles.
    p.uc.mem_write(source, struct.pack("<3i", -vertex[0], vertex[1], -vertex[2]))
    p.uc.mem_write(angles, struct.pack("<3H", roll, yaw, pitch))
    _, error = p.call(0x535d50, (source, result, angles))
    assert error is None, error
    native = struct.unpack("<3i", p.uc.mem_read(result, 12))
    native_points.append(tuple(value / 65536.0 for value in native))

    # model_transform_test takes authored fixed-point vertex followed by the
    # pitch/yaw/roll BAM words used by modelEmissionPoint.
    rows.append(" ".join(map(str, [0] * 9 + vertex + [pitch, yaw, roll])))

run = subprocess.run([binary, "--points"], input="\n".join(rows) + "\n",
                     text=True, capture_output=True, check=True)
tak_points = [tuple(map(float, line.split())) for line in run.stdout.splitlines()]
assert len(tak_points) == len(native_points), (len(tak_points), len(native_points))
errors = [max(abs(a - b) for a, b in zip(tak, native))
          for tak, native in zip(tak_points, native_points)]
worst = max(range(len(errors)), key=errors.__getitem__)
assert errors[worst] < 0.0005, (worst, rows[worst], tak_points[worst],
                                native_points[worst], errors[worst])
print(f"PASS: {len(errors)} retail projectile-model XYZ poses; "
      f"maximum vertex error {errors[worst]:.8f} world units")
