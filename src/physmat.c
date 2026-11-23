// Physical Materials (physmat.h; notas-dev/fisica.md "Materiais físicos"): .rphysmat JSON files, read once into a small
// table by their path's hash (the same hash as the other assets'), read again when the file's time changed (an edit
// in its editor, a copy from Explorer); the editor a small window: presets, the values with their descriptions, Save.
#include "physmat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io_json.h"
#include "platform.h"
#include "platform_files.h"
#include "ui_widget.h"

const char* const g_phys_combine_names[PCOMB_COUNT] = { "Average", "Min", "Multiply", "Max" };

// name, friction, restitution, density g/cm3, combine (friction, restitution)
const PhysMatPreset g_physmat_presets[PHYSMAT_PRESETS] = {
    { "Default", { 0.6f, 0.2f, 0.1f, PCOMB_AVERAGE, PCOMB_AVERAGE, 0.0f } },
    { "Wood", { 0.5f, 0.25f, 0.6f, PCOMB_AVERAGE, PCOMB_AVERAGE, 0.0f } },
    { "Metal", { 0.4f, 0.15f, 7.8f, PCOMB_AVERAGE, PCOMB_AVERAGE, 0.0f } },
    { "Stone", { 0.7f, 0.1f, 2.5f, PCOMB_AVERAGE, PCOMB_AVERAGE, 0.0f } },
    { "Rubber", { 1.0f, 0.8f, 1.1f, PCOMB_MAX, PCOMB_MAX, 0.0f } },
    { "Ice", { 0.03f, 0.05f, 0.92f, PCOMB_MIN, PCOMB_AVERAGE, 0.0f } },
    { "Plastic", { 0.35f, 0.4f, 0.95f, PCOMB_AVERAGE, PCOMB_AVERAGE, 0.0f } },
    { "Glass", { 0.25f, 0.2f, 2.5f, PCOMB_AVERAGE, PCOMB_AVERAGE, 0.0f } },
};

#define PM_MAX 256
typedef struct Entry {
    uint32_t hash;
    int64_t mtime;
    bool ok;
    PhysMaterial m;
    char vpath[256];
} Entry;
static Entry g_pm[PM_MAX];
static uint32_t g_npm;

uint32_t physmat_vpath_hash(const char* vpath) { // (the same as the materials' and Blueprints')
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) {
        char ch = *c == '\\' ? '/' : *c;
        h = (h ^ (uint8_t)tolower((unsigned char)ch)) * 16777619u;
    }
    return h ? h : 1u;
}

bool physmat_is_path(const char* vpath) {
    const char* d = vpath ? strrchr(vpath, '.') : NULL;
    return d && !strcmp(d, ".rphysmat");
}

void physmat_defaults(PhysMaterial* m) { *m = g_physmat_presets[0].m; }

static bool to_abs(const char* vpath, char* out, size_t cap) {
    const char* root = ui_content_root();
    if (!root || !root[0] || strncmp(vpath, "/Content", 8)) return false;
    snprintf(out, cap, "%s%s", root, vpath + 8);
    return true;
}

bool physmat_load(const char* vpath, PhysMaterial* m) {
    physmat_defaults(m);
    char path[1100];
    if (!to_abs(vpath, path, sizeof path)) return false;
    FILE* f = re_fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = n > 0 && n < (1 << 20) ? malloc((size_t)n + 1) : NULL; // (a load, not a frame)
    bool ok = t && fread(t, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    JsonDoc doc;
    char err[160];
    if (!ok || !json_parse(t, (size_t)n, &doc, err, sizeof err)) {
        free(t);
        return false;
    }
    const JsonValue* r = &doc.root;
    m->friction = (float)json_get_num(r, "friction", m->friction);
    m->restitution = (float)json_get_num(r, "restitution", m->restitution);
    m->density = (float)json_get_num(r, "density", m->density);
    m->static_friction = (float)json_get_num(r, "static_friction", m->static_friction);
    m->friction_combine = (uint32_t)json_get_int(r, "friction_combine", (int)m->friction_combine) % PCOMB_COUNT;
    m->restitution_combine = (uint32_t)json_get_int(r, "restitution_combine", (int)m->restitution_combine) % PCOMB_COUNT;
    json_free(&doc);
    free(t);
    return true;
}

bool physmat_save(const char* vpath, const PhysMaterial* m) {
    char path[1100];
    if (!to_abs(vpath, path, sizeof path)) return false;
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1);
    jw_kstr(&w, "type", "PhysicalMaterial");
    jw_kfloat(&w, "friction", m->friction), jw_kfloat(&w, "restitution", m->restitution), jw_kfloat(&w, "density", m->density);
    jw_kfloat(&w, "static_friction", m->static_friction);
    jw_kint(&w, "friction_combine", m->friction_combine), jw_kint(&w, "restitution_combine", m->restitution_combine);
    jw_obj_end(&w);
    bool ok = false;
    if (!w.fail) {
        FILE* f = re_fopen(path, "wb");
        if (f) ok = fwrite(w.buf, 1, w.len, f) == w.len, ok = fclose(f) == 0 && ok;
    }
    jw_free(&w);
    physmat_reload();
    return ok;
}

bool physmat_create(const char* folder_vpath, const char* name, char* out_vpath, uint32_t cap) {
    char vp[600], abs[1100];
    for (int i = 0; i < 1000; i++) {
        if (i) snprintf(vp, sizeof vp, "%s/%s%d.rphysmat", folder_vpath, name, i + 1);
        else snprintf(vp, sizeof vp, "%s/%s.rphysmat", folder_vpath, name);
        if (!to_abs(vp, abs, sizeof abs)) return false;
        if (!re_file_stat(abs, NULL, NULL, NULL)) break;
    }
    PhysMaterial m;
    physmat_defaults(&m);
    if (!physmat_save(vp, &m)) return false;
    if (out_vpath) snprintf(out_vpath, cap, "%s", vp);
    return true;
}

void physmat_reload(void) {
    for (uint32_t i = 0; i < g_npm; i++) g_pm[i].mtime = -1; // (each read again when asked for)
}

// The material of a hash: found among the ones read; one not read yet is looked for among the Content's .rphysmat
// files once (its path is unknown from the hash alone), then remembered
static Entry* find(uint32_t hash) {
    for (uint32_t i = 0; i < g_npm; i++)
        if (g_pm[i].hash == hash) return &g_pm[i];
    return NULL;
}

typedef struct Scan { const char* abs; const char* vdir; int depth; } Scan;
static void scan_dir(const char* abs_dir, const char* vdir, int depth);
static void scan_one(void* user, const ReDirEntry* d) {
    const Scan* s = user;
    if (g_npm >= PM_MAX) return;
    char a[1100], v[600];
    snprintf(a, sizeof a, "%s/%s", s->abs, d->name);
    snprintf(v, sizeof v, "%s/%s", s->vdir, d->name);
    if (d->is_dir) scan_dir(a, v, s->depth + 1);
    else if (physmat_is_path(v) && !find(physmat_vpath_hash(v))) {
        Entry* e = &g_pm[g_npm++];
        memset(e, 0, sizeof *e);
        e->hash = physmat_vpath_hash(v), e->mtime = -1;
        snprintf(e->vpath, sizeof e->vpath, "%s", v);
    }
}
static void scan_dir(const char* abs_dir, const char* vdir, int depth) {
    if (depth > 8 || g_npm >= PM_MAX) return;
    Scan s = { abs_dir, vdir, depth };
    re_dir_walk(abs_dir, scan_one, &s);
}

static uint32_t g_scanned_for; // (the hashes not found: the Content looked through once each frame at most)
const PhysMaterial* physmat_get(uint32_t hash) {
    if (!hash) return NULL;
    Entry* e = find(hash);
    if (!e && g_scanned_for != hash) {
        g_scanned_for = hash;
        const char* root = ui_content_root();
        if (root && root[0]) scan_dir(root, "/Content", 0);
        e = find(hash);
    }
    if (!e) return NULL;
    if (e->mtime < 0) {
        char path[1100];
        int64_t mt = 0;
        e->ok = to_abs(e->vpath, path, sizeof path) && re_file_stat(path, NULL, &mt, NULL) && physmat_load(e->vpath, &e->m);
        e->mtime = e->ok ? mt : 0;
    }
    return e->ok ? &e->m : NULL;
}

const char* physmat_vpath(uint32_t hash) {
    const Entry* e = hash ? find(hash) : NULL;
    return e ? e->vpath : "";
}

void physmat_remember(const char* vpath) {
    if (find(physmat_vpath_hash(vpath)) || g_npm >= PM_MAX) return;
    Entry* e = &g_pm[g_npm++];
    memset(e, 0, sizeof *e);
    e->hash = physmat_vpath_hash(vpath), e->mtime = -1;
    snprintf(e->vpath, sizeof e->vpath, "%s", vpath);
}
