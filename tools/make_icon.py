# (2026-11-24, item 7) The program's icon: the blue R of the opening (src/brand_intro_win.inc), not the silver R on
# black of the first logo. The R and its orbit come out of data/branding/logo.png as alpha (the brightest channel, as
# li_logo / brand_mark do), tinted by Azul Real's gradient (lighter at the top, #2B6FE3 in the middle, deeper at the
# foot), on a transparent background with a soft dark shadow (legible on a light and a dark taskbar).
# Writes data/branding/icon.ico (16, 24, 32, 48, 64, 128, 256 px, PNG inside) and data/branding/icon.png (256 px).
#   python tools/make_icon.py            (needs Pillow and numpy)
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BRAND = os.path.join(ROOT, "data", "branding")
SIZES = [256, 128, 64, 48, 32, 24, 16]

TOP = np.array([0.60, 0.76, 1.0])
MID = np.array([43 / 255.0, 111 / 255.0, 227 / 255.0])
FOOT = np.array([0.10, 0.29, 0.70])


def mask_of_logo(path):
    im = np.asarray(Image.open(path).convert("RGBA"), dtype=np.float32) / 255.0
    a = im[..., :3].max(axis=2) * im[..., 3] * 1.25  # (the logo's grays: a little more opaque, as brand_mark)
    a = np.clip(a, 0.0, 1.0)
    a[a < 0.04] = 0.0  # (the black background's noise and the glow's far edge: out)
    return a


def crop_square(a, margin):
    ys, xs = np.nonzero(a > 0.08)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    side = int(max(y1 - y0, x1 - x0) * (1.0 + margin))
    cy, cx = (y0 + y1) // 2, (x0 + x1) // 2
    out = np.zeros((side, side), np.float32)
    sy, sx = cy - side // 2, cx - side // 2
    for y in range(side):
        yy = sy + y
        if 0 <= yy < a.shape[0]:
            lo, hi = max(sx, 0), min(sx + side, a.shape[1])
            out[y, lo - sx:hi - sx] = a[yy, lo:hi]
    return out


def render(mask, size):
    big = size * 4  # (drawn 4x, then filtered down: smooth edges at every size)
    m = np.asarray(Image.fromarray((mask * 255).astype(np.uint8)).resize((big, big), Image.LANCZOS), np.float32) / 255.0
    if size <= 24:  # (tiny: a touch bolder, the orbit's thin tail would vanish)
        m = np.asarray(Image.fromarray((m * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3)), np.float32) / 255.0
    v = np.linspace(0.0, 1.0, big)[:, None, None]
    col = np.where(v < 0.5, TOP + (MID - TOP) * (v * 2.0), MID + (FOOT - MID) * (v * 2.0 - 1.0))
    col = np.broadcast_to(col, (big, big, 3))
    # the shadow: the mask blurred, a little down, dark
    sh = Image.fromarray((m * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(big / 40.0))
    sh = np.roll(np.asarray(sh, np.float32) / 255.0, big // 64, axis=0) * 0.55
    # "over": the R over its shadow
    a = m + sh * (1.0 - m)
    rgb = np.where(a[..., None] > 1e-4, col * m[..., None] / np.maximum(a[..., None], 1e-4), 0.0)
    out = np.dstack([np.clip(rgb, 0, 1), np.clip(a, 0, 1)])
    img = Image.fromarray((out * 255 + 0.5).astype(np.uint8), "RGBA")
    return img.resize((size, size), Image.LANCZOS)


def main():
    mask = crop_square(mask_of_logo(os.path.join(BRAND, "logo.png")), 0.06)
    imgs = {s: render(mask, s) for s in SIZES}
    imgs[256].save(os.path.join(BRAND, "icon.png"))
    imgs[256].save(os.path.join(BRAND, "icon.ico"), format="ICO", sizes=[(s, s) for s in SIZES],
                   append_images=[imgs[s] for s in SIZES[1:]])
    print("icon: %s (%s px)" % (os.path.join(BRAND, "icon.ico"), ", ".join(str(s) for s in sorted(SIZES))))


if __name__ == "__main__":
    sys.exit(main())
