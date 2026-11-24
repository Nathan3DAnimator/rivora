#!/usr/bin/env python3
"""The loading screen's picture (data/branding/splash.jpg) with the engine's new name (2026-11-23, Real Engine ->
Rivora Engine): the old name painted into the picture is erased (each of its bright pixels filled from the dark
background just above and below it, column by column) and "RIVORA ENGINE" written in its place with the same look:
44 px capitals, a wide stance, the same silver gradient (247 at the top to 184 at the foot) and a soft shadow, centered
where the old name was. Bahnschrift (Windows) stretched to the old letters' width. Writes the file in place; the old
picture stays in git.
    python tools/splash_rename.py [--preview out.png]
"""
import os, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATH = os.path.join(ROOT, "data", "branding", "splash.jpg")
TEXT = "RIVORA ENGINE"
BAND = (440, 526, 1240, 612)      # x0, y0, x1, y1: where the old name lies (its letters 472..1199 x 534..577)
CAP_TOP, CAP = 534, 44           # its capitals' top and height
CENTER_X = 835.5
TOP, FOOT = (247, 248, 253), (184, 184, 196)


def erase(im):
    px = im.load()
    x0, y0, x1, y1 = BAND
    lum = lambda p: (p[0] + p[1] + p[2]) / 3.0
    for x in range(x0, x1):
        bright = [y for y in range(y0, y1) if lum(px[x, y]) > (28 if y < 579 else 11)]  # (below: the old name's faint reflection)
        if not bright:
            continue
        a, b = max(min(bright) - 3, y0 - 4), min(max(bright) + 3, y1 + 4)  # (the letters' run in this column, padded)
        ca, cb = px[x, a - 1], px[x, b + 1]
        for y in range(a, b + 1):
            t = (y - a) / max(b - a, 1)
            px[x, y] = tuple(int(ca[k] + (cb[k] - ca[k]) * t) for k in range(3))
    # (a light blur over the filled band hides the columns' seams)
    box = (x0, y0 - 4, x1, y1 + 4)
    region = im.crop(box).filter(ImageFilter.GaussianBlur(1.2))
    im.paste(region, box)


def name_layer(size):
    font = ImageFont.truetype(r"C:\Windows\Fonts\bahnschrift.ttf", 10)
    try:
        font.set_variation_by_axes([600, 100])  # (weight, width)
    except Exception:
        pass
    # the font size whose capitals are CAP px tall (at 4x, then shrunk: smooth edges)
    S = 4
    fs = 10
    while True:
        f = ImageFont.truetype(r"C:\Windows\Fonts\bahnschrift.ttf", fs * S)
        try:
            f.set_variation_by_axes([700, 100])
        except Exception:
            pass
        bb = f.getbbox("E")
        if bb[3] - bb[1] >= CAP * S:
            break
        fs += 1
    stretch, track, word = 1.62, 15 * S, 48 * S  # (the old letters: ~57 px wide, ~17 px apart, ~50 px between the words)
    glyphs = []
    for ch in TEXT:
        if ch == " ":
            glyphs.append(None)
            continue
        bb = f.getbbox(ch)
        g = Image.new("L", (bb[2] - bb[0] + 4, bb[3] - bb[1] + 4), 0)
        ImageDraw.Draw(g).text((2 - bb[0], 2 - bb[1]), ch, font=f, fill=255)
        g = g.resize((int(g.width * stretch), g.height), Image.LANCZOS)
        glyphs.append(g)
    width = sum(g.width for g in glyphs if g) + track * (sum(1 for g in glyphs if g) - 1) + word - track
    hmax = max(g.height for g in glyphs if g)
    mask = Image.new("L", (width, hmax), 0)
    x = 0
    for g in glyphs:
        if g is None:
            x += word - track
            continue
        mask.paste(g, (x, hmax - g.height))
        x += g.width + track
    mask = mask.resize((width // S, hmax // S), Image.LANCZOS)
    return mask


def main():
    im = Image.open(PATH).convert("RGB")
    erase(im)
    mask = name_layer(CAP)
    w, h = mask.size
    x0 = int(round(CENTER_X - w / 2))
    y0 = CAP_TOP - (h - CAP) - 1  # (the glyphs' top padding above the capitals)
    grad = Image.new("RGB", (w, h))
    gp = grad.load()
    for y in range(h):
        t = min(max((y - (h - CAP)) / CAP, 0.0), 1.0)
        c = tuple(int(TOP[k] + (FOOT[k] - TOP[k]) * t) for k in range(3))
        for x in range(w):
            gp[x, y] = c
    shadow = mask.filter(ImageFilter.GaussianBlur(3)).point(lambda v: int(v * 0.55))
    im.paste((0, 0, 0), (x0 + 1, y0 + 3), shadow)
    im.paste(grad, (x0, y0), mask)
    out = sys.argv[sys.argv.index("--preview") + 1] if "--preview" in sys.argv else PATH
    im.save(out, quality=93)
    print("%s: \"%s\" %d x %d px at (%d, %d)" % (out, TEXT, w, h, x0, y0))


if __name__ == "__main__":
    main()
