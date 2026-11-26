#include "volume.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "material.h"
#include "platform.h"
#include "platform_files.h"
#include "vdb.h"

static uint16_t f2h(float f) { // (round to nearest, no NaN in: our values are finite)
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t s = (x >> 16) & 0x8000u;
    int e = (int)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t m = x & 0x7FFFFFu;
    if (e <= 0) return (uint16_t)s;
    if (e >= 31) return (uint16_t)(s | 0x7BFFu);
    return (uint16_t)(s | (uint32_t)e << 10 | ((m + 0x1000u) >> 13));
}

uint32_t volume_key(const CVolume* v) {
    uint32_t h = 2166136261u;
    for (const char* p = v->path; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    const float f[] = { v->unit, v->flame_temperature, v->temperature_scale };
    for (size_t i = 0; i < 3; i++) {
        uint32_t b;
        memcpy(&b, &f[i], 4);
        h = (h ^ b) * 16777619u;
    }
    h = (h ^ v->frames) * 16777619u, h = (h ^ v->resolution) * 16777619u, h = (h ^ v->up_axis) * 16777619u;
    return h | 1u;
}

void volume_free(VolumeClip* c) {
    free(c->data);
    memset(c, 0, sizeof *c);
}

static void full_path(const char* p, char* out, size_t cap) {
    if (p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':')) snprintf(out, cap, "%s", p);
    else snprintf(out, cap, "%s/%s", mat_root(), p);
}
// the file's point into the engine's space (cm, Z up): 0 Z up, 1 Y up (Houdini, EmberGen)
static void to_engine(const CVolume* v, vec3 p, float o[3]) {
    float u = v->unit > 0.0f ? v->unit : 100.0f;
    if (v->up_axis == 1) o[0] = p.x * u, o[1] = -p.z * u, o[2] = p.y * u;
    else o[0] = p.x * u, o[1] = p.y * u, o[2] = p.z * u;
}
static void to_file(const CVolume* v, const float e[3], vec3* p) {
    float u = v->unit > 0.0f ? v->unit : 100.0f;
    if (v->up_axis == 1) *p = v3(e[0] / u, e[2] / u, -e[1] / u);
    else *p = v3(e[0] / u, e[1] / u, e[2] / u);
}

bool volume_load(const CVolume* v, VolumeClip* clip) {
    volume_free(clip);
    char first[1200];
    full_path(v->path, first, sizeof first);
    uint32_t nframes = v->frames < 1 ? 1 : v->frames > 240 ? 240 : v->frames;
    double t0 = re_time_seconds();
    VolumeGridSet* sets = calloc(nframes, sizeof *sets);
    if (!sets) return false;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    uint32_t got = 0;
    for (uint32_t f = 0; f < nframes; f++) {
        char path[1200], err[256] = "";
        if (nframes > 1) vdb_sequence_path(first, (int)f + 1, path, (int)sizeof path); // (frame 1 first, as Blender's _0001)
        else snprintf(path, sizeof path, "%s", first);
        if (!vdb_read(path, &sets[f], NULL, 0, err, (int)sizeof err)) {
            if (f == 0) printf("[RealEngine] aviso: Volume: %s: %s\n", path, err[0] ? err : "nao leu");
            break;
        }
        got = f + 1;
        for (int g = 0; g < sets[f].count; g++) {
            vec3 a, b;
            if (!volume_grid_world_bounds(&sets[f].grids[g], &a, &b)) continue;
            for (int c = 0; c < 8; c++) {
                float e[3];
                to_engine(v, v3((c & 1) ? b.x : a.x, (c & 2) ? b.y : a.y, (c & 4) ? b.z : a.z), e);
                for (int k = 0; k < 3; k++) lo[k] = fminf(lo[k], e[k]), hi[k] = fmaxf(hi[k], e[k]);
            }
        }
    }
    if (!got || lo[0] > hi[0]) {
        for (uint32_t f = 0; f < nframes; f++) volume_set_free(&sets[f]);
        free(sets);
        return false;
    }
    // the grid: res cells along the longest side, cubic cells
    float ext[3] = { hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] }, longest = fmaxf(ext[0], fmaxf(ext[1], ext[2]));
    uint32_t res = v->resolution < 16 ? 16 : v->resolution > 128 ? 128 : v->resolution;
    float cell = fmaxf(longest, 1.0f) / (float)res;
    uint32_t n[3];
    for (int k = 0; k < 3; k++) n[k] = (uint32_t)ceilf(ext[k] / cell) + 2u, n[k] = n[k] < 4 ? 4 : n[k];
    size_t cells = (size_t)n[0] * n[1] * n[2];
    clip->data = malloc(sizeof(uint16_t) * 2 * cells * got);
    if (!clip->data) {
        for (uint32_t f = 0; f < nframes; f++) volume_set_free(&sets[f]);
        free(sets);
        return false;
    }
    clip->nx = n[0], clip->ny = n[1], clip->nz = n[2], clip->frames = got, clip->cell = cell;
    float org[3] = { lo[0] - cell, lo[1] - cell, lo[2] - cell }; // (a cell of air around)
    clip->center[0] = org[0] + n[0] * cell * 0.5f, clip->center[1] = org[1] + n[1] * cell * 0.5f, clip->bottom = org[2];
    clip->center[2] = org[2];
    float fl = v->flame_temperature > 0.0f ? v->flame_temperature : 1500.0f, ts = v->temperature_scale > 0.0f ? v->temperature_scale : 1.0f;
    for (uint32_t f = 0; f < got; f++) {
        const VolumeGrid* gd = volume_set_find(&sets[f], "density");
        const VolumeGrid* gt = volume_set_find(&sets[f], "temperature");
        const VolumeGrid* gf = volume_set_find(&sets[f], "flame");
        if (!gf) gf = volume_set_find(&sets[f], "flames");
        if (!gd && sets[f].count && sets[f].grids[0].type == VG_FLOAT) gd = &sets[f].grids[0]; // (one unnamed grid: its density)
        uint16_t* out = clip->data + 2 * cells * f;
        for (uint32_t z = 0; z < n[2]; z++)
            for (uint32_t y = 0; y < n[1]; y++)
                for (uint32_t x = 0; x < n[0]; x++) {
                    float e[3] = { org[0] + (x + 0.5f) * cell, org[1] + (y + 0.5f) * cell, org[2] + (z + 0.5f) * cell };
                    vec3 p;
                    to_file(v, e, &p);
                    float d = 0.0f, t = 0.0f, fv = 0.0f;
                    if (gd) volume_grid_sample(gd, p, &d);
                    if (gt) volume_grid_sample(gt, p, &t), t = t * ts;
                    if (gf) volume_grid_sample(gf, p, &fv);
                    // the temperature: a grid in kelvin as it is; a Blender-style one (0..few) scaled; else from the flame
                    float T = gt ? (t > 50.0f ? t : t * fl) : fv * fl;
                    if (gf && fv > 0.0f) T = fmaxf(T, fv * fl);
                    size_t i = ((size_t)z * n[1] + y) * n[0] + x;
                    out[2 * i] = f2h(fmaxf(T, 0.0f)), out[2 * i + 1] = f2h(fmaxf(d, 0.0f));
                }
    }
    for (uint32_t f = 0; f < nframes; f++) volume_set_free(&sets[f]);
    free(sets);
    clip->key = volume_key(v);
    printf("[RealEngine] Volume: %u frame(s) of %ux%ux%u cells from %s (%.0f ms)\n", got, n[0], n[1], n[2], first, (re_time_seconds() - t0) * 1000.0);
    return true;
}

// the example: a puff of smoke rising and spreading from a hot core that dims (24 frames, meters, Z up)
bool volume_examples_present(void) {
    char p[1200];
    snprintf(p, sizeof p, "%s/Volumes/Example_Puff_0001.vdb", mat_root());
    return re_file_stat(p, NULL, NULL, NULL);
}
bool volume_examples(void) {
    char dir[1100], p[1200];
    snprintf(dir, sizeof dir, "%s/Volumes", mat_root());
    snprintf(p, sizeof p, "%s/Example_Puff_0001.vdb", dir);
    if (re_file_stat(p, NULL, NULL, NULL)) return false;
    re_make_dirs(dir);
    bool ok = true;
    for (int f = 0; f < 24 && ok; f++) {
        VolumeGridSet set;
        memset(&set, 0, sizeof set);
        float vs = 0.05f; // (5 cm voxels)
        VolumeGrid* gd = volume_set_add(&set, "density", VG_FLOAT, vs);
        VolumeGrid* gt = volume_set_add(&set, "temperature", VG_FLOAT, vs);
        if (!gd || !gt) { volume_set_free(&set); return false; }
        float t = (float)f / 23.0f, rise = 0.4f + 1.6f * t, rad = 0.35f + 0.5f * t;
        int R = (int)((rad + 0.3f) / vs) + 2;
        for (int k = 0; k <= (int)((rise + rad + 0.4f) / vs); k++)
            for (int j = -R; j <= R; j++)
                for (int i = -R; i <= R; i++) {
                    float x = i * vs, y = j * vs, z = k * vs;
                    float r2 = x * x + y * y, cz = rise * 0.6f;
                    float dz = (z - cz) / (rad * 1.4f), d = 1.0f - (r2 / (rad * rad) + dz * dz);
                    float nse = 0.5f + 0.5f * sinf(x * 9.0f + t * 3.0f) * cosf(y * 7.0f - z * 5.0f) * sinf(z * 6.0f + x * 4.0f);
                    d = d * (0.6f + 0.6f * nse);
                    if (d > 0.02f) {
                        float dv = fminf(d * 2.0f, 1.5f) * (1.0f - 0.5f * t);
                        volume_grid_set(gd, i, j, k, &dv);
                        float core = 1.0f - (r2 + (z - 0.35f) * (z - 0.35f)) / (0.3f * 0.3f);
                        if (core > 0.0f) {
                            float T = (900.0f + 900.0f * core) * (1.0f - t * 0.8f);
                            volume_grid_set(gt, i, j, k, &T);
                        }
                    }
                }
        char path[1200], err[128];
        snprintf(path, sizeof path, "%s/Example_Puff_%04d.vdb", dir, f + 1);
        VdbWriteOptions o = { true, true };
        ok = vdb_write(path, &set, &o, err, (int)sizeof err);
        if (!ok) printf("[RealEngine] aviso: Volume: o exemplo nao foi gravado: %s\n", err);
        volume_set_free(&set);
    }
    return ok;
}
