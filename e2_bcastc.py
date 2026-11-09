"""E2 side study: BC1 / BC3 / BC7 -> ASTC 4x4 transcoding with public tools, on synthetic images only.
encode BC (etcpak, BSD), decode BC (texture2ddecoder, MIT), encode ASTC 4x4 (ARM astcenc, Apache-2.0), decode ASTC (astcenc).
Reports time, megapixels per second and PSNR against the original. Usage: python3 e2_bcastc.py <astcenc binary> <work dir>"""
import os, subprocess, sys, time
import numpy as np
from PIL import Image
import etcpak, texture2ddecoder

astcenc, work = sys.argv[1], sys.argv[2]
os.makedirs(work, exist_ok=True)
N = 1024

def synth(kind):
    y, x = np.mgrid[0:N, 0:N].astype(np.float32) / N
    rng = np.random.default_rng(7)
    if kind == "smooth":   # gradients and soft shapes, like terrain or sky
        r = 255 * x; g = 255 * y; b = 128 + 127 * np.sin(8 * x + 5 * y); a = 255 * np.ones_like(x)
    else:                  # detail: stripes, text-like edges, noise, varying alpha
        r = 255 * ((np.floor(x * 64) + np.floor(y * 64)) % 2); g = 255 * (np.sin(60 * x) > 0)
        b = rng.integers(0, 256, (N, N)); a = 255 * (0.5 + 0.5 * np.cos(9 * y))
    return np.clip(np.stack([r, g, b, a], -1), 0, 255).astype(np.uint8)

def psnr(a, b, ch=slice(0, 4)):
    d = a[..., ch].astype(np.float64) - b[..., ch].astype(np.float64)
    m = np.mean(d * d)
    return 99.0 if m == 0 else 10 * np.log10(255 * 255 / m)

def as_rgba(raw):  # texture2ddecoder returns BGRA
    v = np.frombuffer(raw, np.uint8).reshape(N, N, 4)
    return v[..., [2, 1, 0, 3]]

def best_order(enc_fn, img):
    """etcpak's channel order is checked by measurement, not assumed: try RGBA and BGRA input, keep the closer result"""
    out = []
    for name, src in (("RGBA", img), ("BGRA", img[..., [2, 1, 0, 3]])):
        t = time.perf_counter(); blob = enc_fn(np.ascontiguousarray(src).tobytes(), N, N); te = time.perf_counter() - t
        out.append((name, blob, te))
    return out

def run(cmd):
    t = time.perf_counter(); subprocess.run(cmd, check=True, capture_output=True); return time.perf_counter() - t

ver = (subprocess.run([astcenc, "-version"], capture_output=True, text=True).stdout.strip().splitlines() or ["?"])[0]
print(f"images {N}x{N} ({N*N/1e6:.2f} MPix), synthetic; astcenc: {ver}")
print(f"{'image':8} {'fmt':4} {'bytes':>8} {'BC enc ms':>9} {'BC dec ms':>9} {'PSNR BC':>8} | {'ASTC preset':11} {'enc ms':>8} {'MPix/s':>7} {'PSNR BC->ASTC':>13} {'PSNR direct ASTC':>16}")
DEC = {"BC1": texture2ddecoder.decode_bc1, "BC3": texture2ddecoder.decode_bc3, "BC7": texture2ddecoder.decode_bc7}
ENC = {"BC1": etcpak.compress_bc1, "BC3": etcpak.compress_bc3, "BC7": etcpak.compress_bc7}
for kind in ("smooth", "detail"):
    img = synth(kind); Image.fromarray(img, "RGBA").save(f"{work}/{kind}.png")
    direct = {}
    for preset in ("-fast", "-medium"):
        run([astcenc, "-cl", f"{work}/{kind}.png", f"{work}/{kind}{preset}.astc", "4x4", preset])
        run([astcenc, "-dl", f"{work}/{kind}{preset}.astc", f"{work}/{kind}{preset}.out.png"])
        direct[preset] = psnr(img, np.array(Image.open(f"{work}/{kind}{preset}.out.png").convert("RGBA")))
    for fmt in ("BC1", "BC3", "BC7"):
        ch = slice(0, 3) if fmt == "BC1" else slice(0, 4)   # BC1 here is opaque: compare colour only
        cands = []
        for order, blob, te in best_order(ENC[fmt], img):
            t = time.perf_counter(); dec = as_rgba(DEC[fmt](blob, N, N)); td = time.perf_counter() - t
            cands.append((psnr(img, dec, ch), order, blob, te, td, dec))
        p_bc, order, blob, te, td, dec = max(cands, key=lambda c: c[0])
        Image.fromarray(np.ascontiguousarray(dec), "RGBA").save(f"{work}/{kind}_{fmt}.png")
        for preset in ("-fast", "-medium"):
            tenc = run([astcenc, "-cl", f"{work}/{kind}_{fmt}.png", f"{work}/{kind}_{fmt}{preset}.astc", "4x4", preset])
            run([astcenc, "-dl", f"{work}/{kind}_{fmt}{preset}.astc", f"{work}/{kind}_{fmt}{preset}.out.png"])
            back = np.array(Image.open(f"{work}/{kind}_{fmt}{preset}.out.png").convert("RGBA"))
            print(f"{kind:8} {fmt:4} {len(blob):8d} {te*1000:9.1f} {td*1000:9.1f} {p_bc:8.2f} | {preset:11} {tenc*1000:8.0f} {N*N/1e6/tenc:7.1f} "
                  f"{psnr(img, back, ch):13.2f} {direct[preset]:16.2f}   (etcpak input {order})")
print("ASTC 4x4 and BC7/BC3 are 8 bits a pixel, BC1 4: the transcode keeps memory the same as BC3/BC7 and doubles BC1.")
print("Times include astcenc process start and PNG reading; all threads of this runner.")
