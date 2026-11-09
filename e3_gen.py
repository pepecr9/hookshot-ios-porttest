"""E3 test data: two synthetic 512x512 RGBA images and their BC1 / BC3 / BC7 encodings (etcpak, BSD), raw blocks.
Writes <out>/{smooth,detail}.rgba and .bc1 / .bc3 / .bc7. Usage: python3 e3_gen.py <out dir>"""
import os, sys
import numpy as np
import etcpak

out = sys.argv[1]; os.makedirs(out, exist_ok=True)
N = 512

def synth(kind):
    y, x = np.mgrid[0:N, 0:N].astype(np.float32) / N
    rng = np.random.default_rng(7)
    if kind == "smooth":
        r = 255 * x; g = 255 * y; b = 128 + 127 * np.sin(8 * x + 5 * y); a = 255 * np.ones_like(x)
    else:
        r = 255 * ((np.floor(x * 64) + np.floor(y * 64)) % 2); g = 255 * (np.sin(60 * x) > 0)
        b = rng.integers(0, 256, (N, N)); a = 255 * (0.5 + 0.5 * np.cos(9 * y))
    return np.ascontiguousarray(np.clip(np.stack([r, g, b, a], -1), 0, 255).astype(np.uint8))

for kind in ("smooth", "detail"):
    img = synth(kind); raw = img.tobytes()
    open(f"{out}/{kind}.rgba", "wb").write(raw)
    for fmt, fn, blk in (("bc1", etcpak.compress_bc1, 8), ("bc3", etcpak.compress_bc3, 16), ("bc7", etcpak.compress_bc7, 16)):
        data = fn(raw, N, N)   # RGBA input: the order E2's transcode study measured as the right one
        assert len(data) == (N // 4) * (N // 4) * blk, (fmt, len(data))
        open(f"{out}/{kind}.{fmt}", "wb").write(data)
    print(kind, "written")
print(sorted(os.listdir(out)))
