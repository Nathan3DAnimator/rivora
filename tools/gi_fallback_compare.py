#!/usr/bin/env python3
"""(Q27.12, notas-dev/gi.md "GI de fallback") The GI fallback against the RealGI by image.

Three screenshots of the same scene's GI-only view (--view gi): the reference (RealGI: Medium, software rays with
--no-rt), Low without the fallback (REALENGINE_NO_GI_FALLBACK=1) and Low with it. Their viewports (the left 80 % of the
window, under the toolbars) in luminance, each scaled to the reference's mean (the exposure differs per preset), the
relative RMSE of each against the reference: the fallback should come closer.

    python tools/gi_fallback_compare.py ref.png low.png fallback.png
"""
import sys

import numpy as np
from PIL import Image


def lum(path):
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.float64) / 255.0
    h, w = a.shape[:2]
    a = a[int(h * 0.07):int(h * 0.96), int(w * 0.005):int(w * 0.79)]  # (the viewport)
    a = a ** 2.2  # (back to linear, roughly)
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def main():
    ref, low, fb = (lum(p) for p in sys.argv[1:4])
    out = []
    for name, x in (('low', low), ('fallback', fb)):
        x = x * (ref.mean() / max(x.mean(), 1e-9))
        e = np.sqrt(np.mean((x - ref) ** 2)) / max(ref.mean(), 1e-9)
        out.append(e)
        print('gi_fallback_compare: %s against the RealGI: relative RMSE %.1f %%' % (name, e * 100.0))
    print('gi_fallback_compare: the fallback %s' % ('closer' if out[1] < out[0] else 'NOT closer'))
    return 0 if out[1] < out[0] else 1


if __name__ == '__main__':
    sys.exit(main())
