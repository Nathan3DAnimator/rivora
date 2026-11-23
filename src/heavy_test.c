#include "heavy_test.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mesh_build.h"
#include "platform.h"
#include "platform_files.h"

#define HEAVY_VERSION 3

static uint32_t oct(const float n[3]) {
    float l1 = fabsf(n[0]) + fabsf(n[1]) + fabsf(n[2]);
    if (l1 < 1e-12f) return 0;
    float x = n[0] / l1, y = n[1] / l1;
    if (n[2] < 0.0f) {
        float ox = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f), oy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox, y = oy;
    }
    int16_t ix = (int16_t)lroundf(fminf(fmaxf(x, -1.0f), 1.0f) * 32767.0f), iy = (int16_t)lroundf(fminf(fmaxf(y, -1.0f), 1.0f) * 32767.0f);
    return (uint16_t)ix | ((uint32_t)(uint16_t)iy << 16);
}

static bool exists(const char* path) {
    FILE* f = re_fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// a smooth bumpy field on the unit sphere (sums of sines: no tables, the same every run)
static float bumps(float x, float y, float z) {
    return 0.08f * sinf(3.1f * x + 1.3f) * sinf(2.7f * y + 0.4f) * sinf(3.3f * z + 2.1f) + 0.035f * sinf(9.0f * x + 5.0f * y) * sinf(8.0f * z - 3.0f * y) +
           0.012f * sinf(31.0f * x - 17.0f * z) * sinf(29.0f * y + 13.0f * x);
}

static bool write_built(const char* path, MeshBuilt* mb, const char* what) {
    bool ok = rmesh_write(path, mb);
    printf("[RealEngine] Heavy test: %s %u triangles, %u vertices, %u LODs (", what, mb->lods.index_count[0] / 3u, mb->nv, mb->lods.count);
    for (uint32_t l = 0; l < mb->lods.count; l++) printf("%s%u", l ? " / " : "", mb->lods.index_count[l] / 3u);
    printf(" triangles), %.0f ms\n", mb->ms);
    mesh_built_free(mb);
    return ok;
}

static bool make_rock(const char* path) {
    const uint32_t rings = 700, segs = 1100; // 1.54 M triangles
    uint32_t nv = (rings + 1) * (segs + 1), ni = rings * segs * 6;
    GpuVertex* v = calloc(nv, sizeof(GpuVertex));
    uint32_t* idx = malloc(sizeof(uint32_t) * ni);
    float(*p)[3] = malloc(sizeof(float) * 3 * nv);
    if (!v || !idx || !p) return free(v), free(idx), free(p), false;
    for (uint32_t i = 0; i <= rings; i++)
        for (uint32_t j = 0; j <= segs; j++) {
            float th = 3.14159265f * (float)i / (float)rings, ph = 6.2831853f * (float)j / (float)segs;
            float d[3] = { sinf(th) * cosf(ph), sinf(th) * sinf(ph), cosf(th) };
            float r = 100.0f * (1.0f + bumps(d[0], d[1], d[2]));
            float* q = p[i * (segs + 1) + j];
            q[0] = d[0] * r, q[1] = d[1] * r, q[2] = d[2] * r + 100.0f; // (resting on the ground)
        }
    for (uint32_t i = 0; i <= rings; i++)
        for (uint32_t j = 0; j <= segs; j++) {
            GpuVertex* g = &v[i * (segs + 1) + j];
            const float* q = p[i * (segs + 1) + j];
            memcpy(g->pos, q, 12);
            // the normal from the grid's neighbors (the poles: straight out)
            uint32_t i0 = i ? i - 1 : 0, i1 = i < rings ? i + 1 : rings, j0 = j ? j - 1 : segs - 1, j1 = j < segs ? j + 1 : 1;
            const float *a = p[i0 * (segs + 1) + j], *b = p[i1 * (segs + 1) + j], *c = p[i * (segs + 1) + j0], *e = p[i * (segs + 1) + j1];
            float t1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, t2[3] = { e[0] - c[0], e[1] - c[1], e[2] - c[2] };
            float n[3] = { t1[1] * t2[2] - t1[2] * t2[1], t1[2] * t2[0] - t1[0] * t2[2], t1[0] * t2[1] - t1[1] * t2[0] };
            float out[3] = { q[0], q[1], q[2] - 100.0f };
            if (n[0] * out[0] + n[1] * out[1] + n[2] * out[2] < 0.0f) n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
            if (i == 0 || i == rings) memcpy(n, out, 12);
            float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (int k = 0; k < 3; k++) n[k] = l > 0.0f ? n[k] / l : (k == 2 ? 1.0f : 0.0f);
            g->normal_oct = oct(n);
            g->uv[0] = 4.0f * (float)j / (float)segs, g->uv[1] = 2.0f * (float)i / (float)rings;
        }
    uint32_t* o = idx;
    for (uint32_t i = 0; i < rings; i++)
        for (uint32_t j = 0; j < segs; j++) {
            uint32_t a = i * (segs + 1) + j, b = a + 1, c = a + segs + 1, d = c + 1;
            *o++ = a, *o++ = c, *o++ = b, *o++ = b, *o++ = c, *o++ = d;
        }
    free(p);
    // as an importer hands it: a corner per triangle (mesh_build_corners welds them back: the import path's test)
    GpuVertex* corners = malloc(sizeof(GpuVertex) * (size_t)ni);
    MeshBuilt mb;
    bool ok = corners != NULL;
    if (ok) {
        for (uint32_t i = 0; i < ni; i++) corners[i] = v[idx[i]];
        ok = mesh_build_corners(corners, ni / 3u, true, &mb) && write_built(path, &mb, "rock (welded from corners)");
    }
    free(corners), free(v), free(idx);
    return ok;
}

static uint32_t rng_state = 12345u;
static float rnd(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return (float)(rng_state >> 8) / 16777216.0f;
}

static bool make_cards(const char* path) {
    const uint32_t cards = 300000; // 600 k triangles
    uint32_t nv = cards * 4, ni = cards * 6;
    GpuVertex* v = calloc(nv, sizeof(GpuVertex));
    uint32_t* idx = malloc(sizeof(uint32_t) * ni);
    if (!v || !idx) return free(v), free(idx), false;
    rng_state = 12345u;
    for (uint32_t c = 0; c < cards; c++) {
        // a point on a shell 104-140 cm around the rock's middle, the card lying across it: its length along a random
        // tangent, its width half out of the surface (layers over layers, like hair cards or a bush's leaves)
        float z = rnd() * 2.0f - 1.0f, a = rnd() * 6.2831853f, s = sqrtf(1.0f - z * z);
        float d[3] = { s * cosf(a), s * sinf(a), z };
        float r = 104.0f + 36.0f * rnd();
        float ctr[3] = { d[0] * r, d[1] * r, d[2] * r + 100.0f };
        float up[3] = { fabsf(d[2]) < 0.9f ? 0.0f : 1.0f, 0.0f, fabsf(d[2]) < 0.9f ? 1.0f : 0.0f };
        float t1[3] = { d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0] };
        float l1 = sqrtf(t1[0] * t1[0] + t1[1] * t1[1] + t1[2] * t1[2]);
        for (int k = 0; k < 3; k++) t1[k] /= l1;
        float t2[3] = { d[1] * t1[2] - d[2] * t1[1], d[2] * t1[0] - d[0] * t1[2], d[0] * t1[1] - d[1] * t1[0] };
        float ang = rnd() * 6.2831853f, ca = cosf(ang), sa = sinf(ang);
        float len[3], wid[3];
        for (int k = 0; k < 3; k++) len[k] = (t1[k] * ca + t2[k] * sa) * 12.0f, wid[k] = (d[k] * 0.6f + (-t1[k] * sa + t2[k] * ca) * 0.8f) * 3.0f;
        float n[3] = { len[1] * wid[2] - len[2] * wid[1], len[2] * wid[0] - len[0] * wid[2], len[0] * wid[1] - len[1] * wid[0] };
        float ln = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int k = 0; k < 3; k++) n[k] /= ln;
        uint32_t no = oct(n);
        static const float cu[4] = { 0, 1, 0, 1 }, cv[4] = { 0, 0, 1, 1 };
        for (int q = 0; q < 4; q++) {
            GpuVertex* g = &v[c * 4 + q];
            float su = cu[q] * 2.0f - 1.0f, sv = cv[q] * 2.0f - 1.0f;
            for (int k = 0; k < 3; k++) g->pos[k] = ctr[k] + wid[k] * su + len[k] * sv;
            g->normal_oct = no;
            g->uv[0] = cu[q], g->uv[1] = cv[q];
        }
        uint32_t b = c * 4;
        idx[c * 6 + 0] = b, idx[c * 6 + 1] = b + 2, idx[c * 6 + 2] = b + 1, idx[c * 6 + 3] = b + 1, idx[c * 6 + 4] = b + 2, idx[c * 6 + 5] = b + 3;
    }
    MeshBuilt mb;
    bool ok = mesh_build_indexed(v, nv, idx, ni, false, true, &mb) && write_built(path, &mb, "cards");
    free(v), free(idx);
    return ok;
}

static bool write_text(const char* path, const char* text) {
    FILE* f = re_fopen(path, "wb");
    bool ok = f && fputs(text, f) >= 0;
    if (f) ok = fclose(f) == 0 && ok;
    return ok;
}

// the rock: noise for the color and the bump, 4096; the cards: voronoi cells kept under a distance (round leaves, ~55%
// of each card), their color from the cells, Masked and Two Sided, 4096
static const char* const k_rock_mat =
    "{\"format\":\"RealEngine Material\",\"version\":1,\"resolution\":4096,\"blend_mode\":\"Opaque\",\"opacity_mask_clip\":0.333,"
    "\"two_sided\":false,\"parameters\":[],\"nodes\":["
    "{\"idname\":\"ShaderNodeTexNoise\",\"x\":-500,\"y\":300,\"inputs\":{\"Scale\":[14,0,0,1],\"Detail\":[8,0,0,1]}},"
    "{\"idname\":\"ShaderNodeBsdfPrincipled\",\"x\":0,\"y\":300,\"inputs\":{\"Roughness\":[0.8,0,0,1]}},"
    "{\"idname\":\"ShaderNodeOutputMaterial\",\"x\":300,\"y\":300},"
    "{\"idname\":\"ShaderNodeBump\",\"x\":-200,\"y\":0,\"inputs\":{\"Strength\":[0.5,0,0,1]}}],"
    "\"links\":[{\"from\":1,\"from_socket\":\"BSDF\",\"to\":2,\"to_socket\":\"Surface\"},"
    "{\"from\":0,\"from_socket\":\"Color\",\"to\":1,\"to_socket\":\"Base Color\"},"
    "{\"from\":0,\"from_socket\":\"Fac\",\"to\":3,\"to_socket\":\"Height\"},"
    "{\"from\":3,\"from_socket\":\"Normal\",\"to\":1,\"to_socket\":\"Normal\"}]}";
// (Q17) the same rock with its noise as the Displacement too (Enable Tessellation, 8 cm): Nanite Tessellation moves its
// surface for real (--scene rocks_tess, tess_seams)
static const char* const k_rock_tess_mat =
    "{\"format\":\"RealEngine Material\",\"version\":1,\"resolution\":4096,\"blend_mode\":\"Opaque\",\"opacity_mask_clip\":0.333,"
    "\"two_sided\":false,\"tessellation\":true,\"displacement_magnitude\":8,\"displacement_center\":0.5,\"parameters\":[],\"nodes\":["
    "{\"idname\":\"ShaderNodeTexNoise\",\"x\":-500,\"y\":300,\"inputs\":{\"Scale\":[14,0,0,1],\"Detail\":[8,0,0,1]}},"
    "{\"idname\":\"ShaderNodeBsdfPrincipled\",\"x\":0,\"y\":300,\"inputs\":{\"Roughness\":[0.8,0,0,1]}},"
    "{\"idname\":\"ShaderNodeOutputMaterial\",\"x\":300,\"y\":300},"
    "{\"idname\":\"ShaderNodeBump\",\"x\":-200,\"y\":0,\"inputs\":{\"Strength\":[0.5,0,0,1]}},"
    "{\"idname\":\"ShaderNodeDisplacement\",\"x\":0,\"y\":-100,\"inputs\":{\"Midlevel\":[0.5,0,0,1],\"Scale\":[0.1,0,0,1]}}],"
    "\"links\":[{\"from\":1,\"from_socket\":\"BSDF\",\"to\":2,\"to_socket\":\"Surface\"},"
    "{\"from\":0,\"from_socket\":\"Color\",\"to\":1,\"to_socket\":\"Base Color\"},"
    "{\"from\":0,\"from_socket\":\"Fac\",\"to\":3,\"to_socket\":\"Height\"},"
    "{\"from\":3,\"from_socket\":\"Normal\",\"to\":1,\"to_socket\":\"Normal\"},"
    "{\"from\":0,\"from_socket\":\"Fac\",\"to\":4,\"to_socket\":\"Height\"},"
    "{\"from\":4,\"from_socket\":\"Displacement\",\"to\":2,\"to_socket\":\"Displacement\"}]}";
static const char* const k_cards_mat =
    "{\"format\":\"RealEngine Material\",\"version\":1,\"resolution\":4096,\"blend_mode\":\"Masked\",\"opacity_mask_clip\":0.333,"
    "\"two_sided\":true,\"parameters\":[],\"nodes\":["
    "{\"idname\":\"ShaderNodeTexVoronoi\",\"x\":-600,\"y\":300,\"inputs\":{\"Scale\":[7,0,0,1]}},"
    "{\"idname\":\"ShaderNodeMath\",\"x\":-300,\"y\":100,\"props\":[15,0,0,0],\"inputs\":{\"Value_001\":[0.42,0,0,1]}},"
    "{\"idname\":\"ShaderNodeBsdfPrincipled\",\"x\":0,\"y\":300,\"inputs\":{\"Roughness\":[0.55,0,0,1]}},"
    "{\"idname\":\"ShaderNodeOutputMaterial\",\"x\":300,\"y\":300}],"
    "\"links\":[{\"from\":2,\"from_socket\":\"BSDF\",\"to\":3,\"to_socket\":\"Surface\"},"
    "{\"from\":0,\"from_socket\":\"Color\",\"to\":2,\"to_socket\":\"Base Color\"},"
    "{\"from\":0,\"from_socket\":\"Distance\",\"to\":1,\"to_socket\":\"Value\"},"
    "{\"from\":1,\"from_socket\":\"Value\",\"to\":2,\"to_socket\":\"Alpha\"}]}";

bool heavy_test_assets(const char* content_root, HeavyAssets* out) {
    memset(out, 0, sizeof *out);
    char dir[700], rock[800], cards[800];
    snprintf(dir, sizeof dir, "%s/HeavyTest", content_root);
    re_make_dirs(dir);
    snprintf(out->rock_mesh, sizeof out->rock_mesh, "/Content/HeavyTest/SM_HeavyRock_v%d.rmesh", HEAVY_VERSION);
    snprintf(out->cards_mesh, sizeof out->cards_mesh, "/Content/HeavyTest/SM_HeavyCards_v%d.rmesh", HEAVY_VERSION);
    snprintf(rock, sizeof rock, "%s/SM_HeavyRock_v%d.rmesh", dir, HEAVY_VERSION);
    snprintf(cards, sizeof cards, "%s/SM_HeavyCards_v%d.rmesh", dir, HEAVY_VERSION);
    snprintf(out->rock_mat, sizeof out->rock_mat, "%s/M_HeavyRock_v%d.rmat", dir, HEAVY_VERSION);
    snprintf(out->cards_mat, sizeof out->cards_mat, "%s/M_HeavyCards_v%d.rmat", dir, HEAVY_VERSION);
    snprintf(out->rock_tess_mat, sizeof out->rock_tess_mat, "%s/M_HeavyRockTess_v1.rmat", dir);
    out->tris = 700u * 1100u * 2u + 300000u * 2u;
    double t0 = re_time_seconds();
    bool ok = true, made = false;
    if (!exists(rock)) ok = make_rock(rock) && ok, made = true;
    if (!exists(cards)) ok = make_cards(cards) && ok, made = true;
    if (!exists(out->rock_mat)) ok = write_text(out->rock_mat, k_rock_mat) && ok;
    if (!exists(out->cards_mat)) ok = write_text(out->cards_mat, k_cards_mat) && ok;
    if (!exists(out->rock_tess_mat)) ok = write_text(out->rock_tess_mat, k_rock_tess_mat) && ok;
    if (made) out->ms = (re_time_seconds() - t0) * 1000.0;
    return ok;
}
