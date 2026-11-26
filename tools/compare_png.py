# (2026-11-25, item 22) A render compared to its reference (the suite's render_ref_* tests):
#   python tools/compare_png.py <shot.png> <reference.png> [--max-mean M] [--max-bad P] [--crop x0,y0,x1,y1] [--update]
#                               [--max-dark D]
# Both brought to the reference's size (640 px wide: the references stay small in the repository; --update writes the
# shot so), then per pixel the largest channel difference: its mean (0..255) and the % of pixels over 24. Over the
# limits fails, and <shot>_diff.png shows where (brighter = more different), beside the shot.
# --max-dark D (2026-11-25): the shot alone (in its crop) fails with more than D % of its pixels almost black (every
# channel under 12): the Low preset's house came out black, a picture no reference had yet. Reference "-": that check alone
import os, sys

def main():
    a = [x for i, x in enumerate(sys.argv[1:], 1) if not x.startswith('--') and sys.argv[i - 1] not in ('--crop', '--max-mean', '--max-bad', '--max-dark')]
    if len(a) < 2:
        print('usage: compare_png.py <shot.png> <reference.png|-> [--max-mean M] [--max-bad P] [--max-dark D] [--update]')
        return 2
    shot, ref = a[0], a[1]
    def opt(name, default):
        return float(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else default
    max_mean, max_bad = opt('--max-mean', 4.0), opt('--max-bad', 2.0)
    from PIL import Image, ImageChops
    if not os.path.exists(shot):
        print('FAIL: render ref: no shot %s (the engine wrote none)' % shot)
        return 1
    im = Image.open(shot).convert('RGB')
    name = os.path.basename(shot if ref == '-' else ref)
    if '--max-dark' in sys.argv:
        sd = im
        if '--crop' in sys.argv:
            c = [float(v) for v in sys.argv[sys.argv.index('--crop') + 1].split(',')]
            sd = im.crop((round(c[0] * im.width), round(c[1] * im.height), round(c[2] * im.width), round(c[3] * im.height)))
        r, g, b = sd.split()
        hist = ImageChops.lighter(ImageChops.lighter(r, g), b).histogram()
        dark = 100.0 * sum(hist[:12]) / max(1, sd.width * sd.height)
        max_dark = opt('--max-dark', 100.0)
        print('render ref %s: %.1f%% of the view almost black (at most %.1f%%)' % (name, dark, max_dark))
        if dark > max_dark:
            print('FAIL: render ref %s: the view is dark (%.1f%% of it almost black)' % (name, dark))
            return 1
    if ref == '-':
        print('render ref %s: OK' % name)
        return 0
    if '--update' in sys.argv:
        w = 640
        h = max(1, round(im.height * w / im.width))
        os.makedirs(os.path.dirname(os.path.abspath(ref)), exist_ok=True)
        im.resize((w, h), Image.LANCZOS).save(ref, optimize=True)
        print('render ref: reference written %s (%dx%d)' % (ref, w, h))
        return 0
    if not os.path.exists(ref):
        print('FAIL: render ref: no reference %s (the suite: -GravarReferencias)' % ref)
        return 1
    rf = Image.open(ref).convert('RGB')
    im = im.resize(rf.size, Image.LANCZOS)
    if '--crop' in sys.argv:  # (fractions x0,y0,x1,y1: the viewport alone, not the editor's panels and their fps numbers)
        c = [float(v) for v in sys.argv[sys.argv.index('--crop') + 1].split(',')]
        box = (round(c[0] * rf.width), round(c[1] * rf.height), round(c[2] * rf.width), round(c[3] * rf.height))
        im, rf = im.crop(box), rf.crop(box)
    d = ImageChops.difference(im, rf)
    r, g, b = d.split()
    m = ImageChops.lighter(ImageChops.lighter(r, g), b)  # the largest channel's difference
    hist = m.histogram()
    n = rf.width * rf.height
    mean = sum(i * c for i, c in enumerate(hist)) / n
    bad = 100.0 * sum(hist[25:]) / n
    ok = mean <= max_mean and bad <= max_bad
    print('render ref %s: mean difference %.2f (at most %.1f), %.2f%% of the pixels over 24 (at most %.1f%%)' % (name, mean, max_mean, bad, max_bad))
    if not ok:
        diff = os.path.splitext(shot)[0] + '_diff.png'
        m.point(lambda v: min(255, v * 4)).save(diff)
        print('FAIL: render ref %s differs from its reference (the difference: %s)' % (name, diff))
        return 1
    print('render ref %s: OK' % name)
    return 0

if __name__ == '__main__':
    sys.exit(main())
