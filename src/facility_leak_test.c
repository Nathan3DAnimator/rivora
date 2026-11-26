// (2026-11-25, item 19 of the user's list: his video of the procedural facility) --scene facility_leak: the camera in the
// facility's deepest room. Without its lamps (REALENGINE_FACILITY_LIGHTS unset) nothing may light it: the scene's
// brightest point is checked by --max-scene-lum (the sky's light leaked along the walls' seams: 5.5 nits on average, 204 at
// a three-way corner). With them and Build Lighting (REALENGINE_FACILITY_BAKE=1: the test facility_leak_bake) the shot is
// read back: the room lit (its mean) and even (its blotches: the mean step between neighboring 16 px blocks of the walls and
// the ceiling over their mean, the patches of the video at 1:41-1:44)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "editor.h"
#include "render.h"

uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h); // mat_image.c

// The shot's blotchiness in the viewport's upper 3/4 (the walls and the ceiling: the floor's grid lines left out): the
// luma in 16 x 16 blocks, the mean |step| to the right and below over the mean luma
bool facility_leak_check(Editor* ed, const char* shot, double max_blotch, double min_mean) {
    if (!shot) {
        printf("[RealEngine] FAIL: facility_leak: no shot to read (--shot)\n");
        return false;
    }
    uint32_t w = 0, h = 0;
    uint8_t* px = mat_load_image_rgba(shot, &w, &h);
    if (!px) {
        printf("[RealEngine] FAIL: facility_leak: the shot couldn't be read\n");
        return false;
    }
    const RenderView* rv = &ed->render_view;
    uint32_t x0 = (uint32_t)rv->x, y0 = (uint32_t)rv->y, vw = (uint32_t)rv->w, vh = (uint32_t)rv->h;
    if (!vw || !vh || x0 + vw > w || y0 + vh > h) x0 = y0 = 0, vw = w, vh = h;
    y0 += vh / 20, vh = vh * 3 / 4 - vh / 20; // (below the viewport's toolbar, above the floor)
    enum { B = 16 };
    uint32_t bw = vw / B, bh = vh / B;
    double* blk = calloc((size_t)bw * bh, sizeof(double));
    double sum = 0.0;
    for (uint32_t by = 0; blk && by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            double s = 0.0;
            for (uint32_t y = 0; y < B; y++)
                for (uint32_t x = 0; x < B; x++) {
                    const uint8_t* a = &px[((size_t)(y0 + by * B + y) * w + x0 + bx * B + x) * 4];
                    s += 0.2126 * a[0] + 0.7152 * a[1] + 0.0722 * a[2];
                }
            blk[by * bw + bx] = s / (B * B), sum += s / (B * B);
        }
    double mean = bw && bh ? sum / (double)(bw * bh) : 0.0, step = 0.0;
    uint64_t n = 0;
    for (uint32_t by = 0; blk && by + 1 < bh; by++)
        for (uint32_t bx = 0; bx + 1 < bw; bx++) {
            double c = blk[by * bw + bx];
            step += fabs(blk[by * bw + bx + 1] - c) + fabs(blk[(by + 1) * bw + bx] - c), n += 2;
        }
    free(blk), free(px);
    double blotch = n ? step / (double)n / (mean + 1.0) : 0.0;
    printf("[RealEngine] facility_leak: the walls and ceiling's mean luma %.1f (0-255), blotches %.4f (the mean step between 16 px blocks over the mean)\n",
           mean, blotch);
    bool ok = true;
    // the scene's mean (the probe: nits, before the exposure) against REALENGINE_FACILITY_LUM_MEAN_MAX: the closed room
    // without its lamps (the air's light of the sky in it, 4 of its 5 nits)
    double lmax, lmean;
    render_probe_lum(&lmax, &lmean);
    const char* mm = getenv("REALENGINE_FACILITY_LUM_MEAN_MAX");
    if (mm && lmean > atof(mm)) printf("[RealEngine] FAIL: facility_leak: the closed room's mean %.3f nits > %.3f (light from outside)\n", lmean, atof(mm)), ok = false;
    if (max_blotch > 0.0 && blotch > max_blotch) printf("[RealEngine] FAIL: facility_leak: blotchy walls (%.4f > %.4f)\n", blotch, max_blotch), ok = false;
    if (min_mean > 0.0 && mean < min_mean) printf("[RealEngine] FAIL: facility_leak: the lit room is dark (%.1f < %.1f)\n", mean, min_mean), ok = false;
    return ok;
}
