// (Q23, notas-dev/auditoria-artista.md) The artist tools' fixes, tested.
// --artist-selftest (no window): the Radiance .hdr decoder (run-length and flat scanlines, values, a broken file), the
// Content Browser's kinds (TIFF, GIF, .hdr and .rudim are textures), the viewer taking .rudim, the Static Mesh viewer's
// vertex count (the vertices used, not the index span) and its cost on 3 M indices.
// --artist-test (the editor): a .hdr made in /Content/Q23Test opened in the Texture viewer (its size, mips, exposure),
// its Content Browser thumbnail made; the file written again bigger: the thumbnail and the open viewer follow it (the
// file watch, once a frame); an .exr shows why it doesn't open. The folder is removed at the end.
#include "asset_viewer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "content.h"
#include "editor.h"
#include "platform.h"
#include "platform_files.h"

float* mat_hdr_decode(const uint8_t* d, size_t n, uint32_t* ow, uint32_t* oh); // (mat_image.c)

#define FAIL(...) (printf("[RealEngine] FAIL: artist: " __VA_ARGS__), printf("\n"), fails++)

// A .hdr of w x h: pixel (x, y) is RGBE (x, y, 200 - x, 129) (= (x + 0.5) * 2 / 256...); rle: the new run-length
// scanlines (a run of 4 equal values at the start of each channel, then literals), else flat
static uint8_t* make_hdr(int w, int h, bool rle, size_t* n) {
    uint8_t* d = malloc(256 + (size_t)w * h * 8);
    if (!d) return NULL;
    size_t p = (size_t)sprintf((char*)d, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n", h, w);
    for (int y = 0; y < h; y++) {
        uint8_t px[4][2048];
        for (int x = 0; x < w; x++) px[0][x] = (uint8_t)(x < 4 ? 7 : x), px[1][x] = (uint8_t)y, px[2][x] = (uint8_t)(200 - x), px[3][x] = 129;
        if (rle) {
            d[p++] = 2, d[p++] = 2, d[p++] = (uint8_t)(w >> 8), d[p++] = (uint8_t)(w & 255);
            for (int c = 0; c < 4; c++) {
                int x = 0;
                if (c == 0) d[p++] = 128 + 4, d[p++] = 7, x = 4; // (a run)
                while (x < w) {
                    int k = w - x < 128 ? w - x : 128;
                    d[p++] = (uint8_t)k;
                    for (int i = 0; i < k; i++) d[p++] = px[c][x + i];
                    x += k;
                }
            }
        } else
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 4; c++) d[p++] = px[c][x];
    }
    *n = p;
    return d;
}

static bool check_hdr(const float* f, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float* q = f + ((size_t)y * w + x) * 3;
            float e = 2.0f / 256.0f; // 2^(129-136)
            float r = ((x < 4 ? 7 : x) + 0.5f) * e, g = (y + 0.5f) * e, b = (200 - x + 0.5f) * e;
            if (fabsf(q[0] - r) > 1e-5f || fabsf(q[1] - g) > 1e-5f || fabsf(q[2] - b) > 1e-5f) return false;
        }
    return true;
}

int artist_selftest(void) {
    int fails = 0;
    // 1. the .hdr decoder
    for (int rle = 0; rle < 2; rle++) {
        size_t n = 0;
        uint8_t* d = make_hdr(200, 3, rle != 0, &n);
        uint32_t w = 0, h = 0;
        double t0 = re_time_seconds();
        float* f = d ? mat_hdr_decode(d, n, &w, &h) : NULL;
        double ms = (re_time_seconds() - t0) * 1000.0;
        if (!f || w != 200 || h != 3 || !check_hdr(f, 200, 3)) FAIL(".hdr (%s scanlines) decoded wrong (%ux%u)", rle ? "run-length" : "flat", w, h);
        else printf("[RealEngine] Artist: .hdr %s 200x3 decoded right (%.3f ms)\n", rle ? "run-length" : "flat", ms);
        free(f);
        if (d && rle) { // a broken one: cut in the middle, and its header's size wrong
            if ((f = mat_hdr_decode(d, n / 2, &w, &h)) != NULL) FAIL("a cut .hdr decoded"), free(f);
            memcpy(d, "#?NOTRADIANCE", 13);
            if ((f = mat_hdr_decode(d, n, &w, &h)) != NULL) FAIL("a file that isn't a .hdr decoded"), free(f);
        }
        free(d);
    }
    { // a big one: 2048 x 1024, the cost
        size_t n = 0;
        uint8_t* d = make_hdr(2048, 1024, true, &n);
        uint32_t w = 0, h = 0;
        double t0 = re_time_seconds();
        float* f = d ? mat_hdr_decode(d, n, &w, &h) : NULL;
        double ms = (re_time_seconds() - t0) * 1000.0;
        if (!f || w != 2048 || h != 1024) FAIL("a 2048x1024 .hdr didn't decode");
        else printf("[RealEngine] Artist: .hdr 2048x1024 run-length decoded in %.1f ms (%.1f MB)\n", ms, n / 1048576.0);
        free(f), free(d);
    }
    // 2. the Content Browser's kinds and the viewer's files
    static const char* const tex[] = { "a.tif", "a.TIFF", "a.gif", "a.hdr", "a.rudim", "a.png" };
    for (size_t i = 0; i < sizeof tex / sizeof tex[0]; i++)
        if (content_kind_of(tex[i]) != ASSET_TEXTURE_SOURCE) FAIL("%s is not a texture in the Content Browser", tex[i]);
    if (!asset_viewer_handles("/Content/T.rudim") || !asset_viewer_handles("/Content/T.tiff")) FAIL("the viewer doesn't take .rudim/.tiff");
    // 3. the Static Mesh viewer's vertex count: the vertices used, not the indices' span
    static const uint32_t idx[] = { 99, 99, 99, 10, 11, 12, 12, 11, 13, 20, 21, 22 };
    uint32_t used = asset_mesh_used_verts(idx, 3, 9);
    if (used != 7) FAIL("the vertices used: %u, not 7 (the span says 13)", used);
    {
        uint32_t cnt = 3000000, span = 1000000;
        uint32_t* big = malloc(cnt * sizeof(uint32_t));
        uint32_t s = 12345u;
        for (uint32_t i = 0; big && i < cnt; i++) s = s * 1664525u + 1013904223u, big[i] = 500000u + (s >> 8) % span;
        double t0 = re_time_seconds();
        uint32_t u = big ? asset_mesh_used_verts(big, 0, cnt) : 0;
        double ms = (re_time_seconds() - t0) * 1000.0;
        printf("[RealEngine] Artist: vertex count of 1 M triangles: %u used (span %u) in %.2f ms\n", u, span, ms);
        if (!u || u > span) FAIL("the big mesh's vertex count");
        free(big);
    }
    printf("[RealEngine] Artist selftest: %s\n", fails ? "FAIL" : "OK");
    return fails == 0;
}

// --- --artist-test: in the editor ---

enum { A_START, A_THUMB, A_GROW, A_EXR, A_DONE };
static struct {
    bool running;
    int state, wait, fails;
    unsigned refreshes0, reloads0;
    double watch_sum;
    int watch_n;
    char dir[600];
} T;

static bool write_hdr(const char* path, int w, int h) {
    size_t n = 0;
    uint8_t* d = make_hdr(w, h, true, &n);
    FILE* f = d ? re_fopen(path, "wb") : NULL;
    bool ok = f && fwrite(d, 1, n, f) == n;
    if (f) fclose(f);
    free(d);
    return ok;
}

void artist_test_start(void) {
    memset(&T, 0, sizeof T);
    T.running = true;
    snprintf(T.dir, sizeof T.dir, "%s/Q23Test", content_root());
    re_remove_tree(T.dir);
    re_make_dirs(T.dir);
}

#define HDR_V "/Content/Q23Test/T_Q23.hdr"
#define EXR_V "/Content/Q23Test/T_Q23.exr"

// Each editor frame after the UI: true when it finished (*ok: passed)
bool artist_test_frame(Editor* ed, bool* ok) {
    (void)ed;
    if (!T.running) return false;
    int fails = 0;
    char disk[700];
    AssetViewerState st = asset_viewer_state();
    switch (T.state) {
    case A_START:
        snprintf(disk, sizeof disk, "%s/T_Q23.hdr", T.dir);
        if (!write_hdr(disk, 128, 64)) {
            FAIL("could not write %s", disk);
            T.state = A_DONE;
            break;
        }
        asset_viewer_open(HDR_V);
        st = asset_viewer_state();
        if (!st.open || !st.is_hdr || st.src_w != 128 || st.src_h != 64 || st.mips != 8)
            FAIL("the .hdr in the viewer: open %d hdr %d %ux%u, %d mips (128x64, 8)", st.open, st.is_hdr, st.src_w, st.src_h, st.mips);
        else printf("[RealEngine] Artist: the .hdr opens in the Texture viewer (%ux%u, %d mips)\n", st.src_w, st.src_h, st.mips);
        T.state = A_THUMB, T.wait = 0;
        break;
    case A_THUMB: // its thumbnail made (drawn off screen: the Content Browser's call)
        asset_thumb(igGetForegroundDrawList_ViewportPtr(igGetMainViewport()), -200, -200, -72, -72, HDR_V, ASSET_TEXTURE_SOURCE);
        if (asset_thumb_state(HDR_V) == 4) {
            printf("[RealEngine] Artist: its thumbnail made in %d frames\n", T.wait);
            T.refreshes0 = st.thumb_refreshes, T.reloads0 = st.reloads;
            snprintf(disk, sizeof disk, "%s/T_Q23.hdr", T.dir);
            if (!write_hdr(disk, 256, 64)) FAIL("could not write the .hdr again");
            T.state = A_GROW, T.wait = 0;
        } else if (++T.wait > 300) FAIL("the .hdr's thumbnail was never made (state %d)", asset_thumb_state(HDR_V)), T.state = A_DONE;
        break;
    case A_GROW: // written again (256 wide): the thumbnail made again, the open viewer read again
        asset_thumb(igGetForegroundDrawList_ViewportPtr(igGetMainViewport()), -200, -200, -72, -72, HDR_V, ASSET_TEXTURE_SOURCE);
        T.watch_sum += st.watch_us, T.watch_n++;
        if (st.thumb_refreshes > T.refreshes0 && asset_thumb_state(HDR_V) == 4 && st.reloads > T.reloads0 && st.src_w == 256) {
            printf("[RealEngine] Artist: the file changed: thumbnail and viewer followed in %d frames (now %ux%u); a file looked at by the worker in %.1f us (0 on the main thread)\n", T.wait,
                   st.src_w, st.src_h, T.watch_sum / T.watch_n);
            T.state = A_EXR, T.wait = 0;
        } else if (++T.wait > 600) {
            FAIL("the changed file wasn't followed: thumbnail refreshes %u -> %u (state %d), viewer reloads %u -> %u (%u wide)", T.refreshes0, st.thumb_refreshes,
                 asset_thumb_state(HDR_V), T.reloads0, st.reloads, st.src_w);
            T.state = A_DONE;
        }
        break;
    case A_EXR: { // an .exr: the window says why
        snprintf(disk, sizeof disk, "%s/T_Q23.exr", T.dir);
        FILE* f = re_fopen(disk, "wb");
        if (f) fputs("v/1\x01 not really an OpenEXR", f), fclose(f);
        asset_viewer_open(EXR_V);
        st = asset_viewer_state();
        if (!st.open || !st.error || !st.error[0]) FAIL("the .exr: no window or no reason (open %d)", st.open);
        else printf("[RealEngine] Artist: the .exr says: %s\n", st.error);
        T.state = A_DONE;
        break;
    }
    case A_DONE:
        asset_viewer_close();
        re_remove_tree(T.dir);
        T.running = false;
        *ok = T.fails == 0;
        printf("[RealEngine] Artist test: %s\n", T.fails ? "FAIL" : "OK");
        return true;
    }
    T.fails += fails;
    return false;
}
