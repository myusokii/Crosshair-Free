#!/usr/bin/env python3
"""
CrosshairFree logo generator.
Renders the app icon procedurally (signed-distance shapes, supersampled) so it stays crisp at
every size, and writes a multi-size CrosshairFree.ico plus a preview PNG.

Usage: python3 make_logo.py            (needs: pillow, numpy)
"""
import numpy as np
from PIL import Image, ImageFilter

RED_TOP = np.array([255, 96, 88], np.float32)      # crosshair gradient, top
RED_BOT = np.array([214, 38, 40], np.float32)      # crosshair gradient, bottom
BG_A = np.array([40, 40, 48], np.float32)          # background gradient, top-left
BG_B = np.array([9, 9, 12], np.float32)            # background gradient, bottom-right
DOT = np.array([255, 244, 240], np.float32)


def sd_rrect(u, v, half, r):
    qx, qy = np.abs(u) - (half - r), np.abs(v) - (half - r)
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


def sd_box(u, v, cx, cy, hx, hy):
    qx, qy = np.abs(u - cx) - hx, np.abs(v - cy) - hy
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0)


# Geometry per detail level, in a 256-unit design space centred on (0, 0)
FULL = dict(ring=True, ring_r=66, ring_t=14, ring_gap=21, arm_in=30, arm_out=102, arm_t=20, dot=11, glow=1.0)
MID = dict(ring=True, ring_r=64, ring_t=18, ring_gap=24, arm_in=28, arm_out=104, arm_t=26, dot=14, glow=0.8)
SMALL = dict(ring=False, arm_in=26, arm_out=104, arm_t=32, dot=20, glow=0.0)


def params_for(size):
    if size >= 48:
        return FULL
    if size >= 32:
        return MID
    return SMALL


def render(size):
    p = params_for(size)
    ss = 8 if size <= 64 else 4
    n = size * ss
    ppu = n / 256.0                                    # supersampled pixels per design unit
    ys, xs = np.mgrid[0:n, 0:n]
    u = (xs + 0.5) / n * 256.0 - 128.0
    v = (ys + 0.5) / n * 256.0 - 128.0

    def cov(sd):                                       # signed distance (units) -> coverage 0..1
        return np.clip(0.5 - sd * ppu, 0, 1).astype(np.float32)

    # ---- tile background
    tile_sd = sd_rrect(u, v, 120, 58)
    tile = cov(tile_sd)
    t = np.clip((u + v) / 480.0 + 0.5, 0, 1)[..., None]
    bg = BG_A * (1 - t) + BG_B * t
    r = np.hypot(u, v)
    halo = (np.exp(-(r / 92.0) ** 2) * 0.20)[..., None]
    bg = bg * (1 - halo) + np.array([224, 49, 49], np.float32) * halo

    # ---- crosshair mask (arms + ring + dot)
    arms = np.full_like(u, 1e9)
    c = (p["arm_in"] + p["arm_out"]) / 2
    hl = (p["arm_out"] - p["arm_in"]) / 2
    ht = p["arm_t"] / 2
    for (cx, cy, hx, hy) in ((c, 0, hl, ht), (-c, 0, hl, ht), (0, c, ht, hl), (0, -c, ht, hl)):
        arms = np.minimum(arms, sd_box(u, v, cx, cy, hx, hy))
    shape = arms
    if p["ring"]:
        ring = np.abs(r - p["ring_r"]) - p["ring_t"] / 2
        ring = np.maximum(ring, p["ring_gap"] - np.minimum(np.abs(u), np.abs(v)))   # break the ring at the arms
        shape = np.minimum(shape, ring)
    ch = cov(shape)
    dot = cov(r - p["dot"])

    # ---- glow behind the crosshair
    if p["glow"] > 0:
        a = Image.fromarray((np.minimum(ch + dot, 1) * 255).astype(np.uint8))
        a = a.filter(ImageFilter.GaussianBlur(radius=9 * ppu))
        glow = (np.asarray(a, np.float32) / 255.0) * 0.60 * p["glow"]
    else:
        glow = np.zeros_like(ch)

    col_t = np.clip((v + 110) / 220.0, 0, 1)[..., None]
    red = RED_TOP * (1 - col_t) + RED_BOT * col_t

    rgb = bg
    g = glow[..., None] * tile[..., None]
    rgb = rgb * (1 - g) + np.array([224, 49, 49], np.float32) * g
    k = ch[..., None]
    rgb = rgb * (1 - k) + red * k
    d = dot[..., None]
    rgb = rgb * (1 - d) + DOT * d

    # ---- rim light on the tile edge
    rim = np.clip(1 - np.abs(tile_sd + 1.4) / 1.4, 0, 1) * tile
    rim_a = (0.20 * (1 - np.clip((v + 120) / 240.0, 0, 1)) + 0.04)[..., None] * rim[..., None]
    rgb = rgb * (1 - rim_a) + 255 * rim_a

    # ---- composite + downsample (premultiplied so edges stay clean)
    a = tile
    pre = np.dstack([rgb * a[..., None], a])
    pre = pre.reshape(size, ss, size, ss, 4).mean(axis=(1, 3))
    alpha = pre[..., 3]
    out = np.zeros((size, size, 4), np.float32)
    nz = alpha > 1e-4
    out[..., :3][nz] = pre[..., :3][nz] / alpha[nz][:, None]
    out[..., 3] = alpha * 255
    return Image.fromarray(np.clip(out + 0.5, 0, 255).astype(np.uint8), "RGBA")


SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]

if __name__ == "__main__":
    imgs = {s: render(s) for s in SIZES}
    imgs[256].save("CrosshairFree.ico", format="ICO", sizes=[(s, s) for s in SIZES], append_images=[imgs[s] for s in SIZES[:-1]])
    imgs[256].save("logo_256.png")
    # preview sheet on a dark + light backdrop
    sheet = Image.new("RGBA", (256 + 40 + 128 + 20 + 64 + 20 + 48 + 20 + 32 + 20 + 24 + 20 + 16 + 20, 296), (30, 30, 34, 255))
    light = Image.new("RGBA", (sheet.width, 296 // 2), (235, 236, 240, 255))
    x = 20
    for s in (256, 128, 64, 48, 32, 24, 16):
        sheet.alpha_composite(imgs[s], (x, 20))
        sheet.alpha_composite(imgs[s], (x, 20 + 256 + 6) if False else (x, 20))
        x += s + 20
    sheet.convert("RGB").save("logo_sheet.png")
    print("wrote CrosshairFree.ico, logo_256.png, logo_sheet.png")
