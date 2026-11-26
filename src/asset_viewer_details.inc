// (2026-11-24, item 8 of the user's list: "os visualizadores com as opções da Unreal: LOD, Nanite, redução de
// triângulos, rig, textura, malha; UIs mais cheias") Included by asset_viewer.c. The viewers' Details as Unreal's: a
// search on top and sections that fold (av_details_search / av_section, also the Skeletal Mesh and animation editors'),
// and the Static Mesh's options that change the asset for real: its LODs (how many, each one's share of LOD 0's
// triangles; made again from LOD 0, the .rmesh's LOD table: no format change), Build (normals by a smoothing angle, a
// scale applied), each LOD's triangles, vertices and memory. Nothing is shown that the engine can't do.
#include "mesh_build.h"

// --- Details: the search and the sections ---
static const char* g_av_find; // the panel's search now ("": every section)

static bool av_has(const char* hay, const char* needle) {
    if (!needle || !needle[0]) return true;
    size_t n = strlen(needle);
    for (; hay && *hay; hay++)
        if (!_strnicmp(hay, needle, n)) return true;
    return false;
}

// (2026-11-25, the tester's block of item 8) the tests' hooks (asset_viewport_test.c): a search typed into the next panel
// drawn, and each panel's sections as drawn (shown by the search, open), the last whole panel kept
static char g_av_test_find[64];
static bool g_av_test_set;
static AvSectionSeen g_av_seen[AV_SEEN_MAX], g_av_last[AV_SEEN_MAX];
static int g_av_nseen, g_av_nlast;
static void av_seen_close(void) {
    if (!g_av_nseen) return;
    memcpy(g_av_last, g_av_seen, sizeof(AvSectionSeen) * (size_t)g_av_nseen);
    g_av_nlast = g_av_nseen, g_av_nseen = 0;
}
void av_test_find(const char* text) { snprintf(g_av_test_find, sizeof g_av_test_find, "%s", text ? text : ""), g_av_test_set = true; }
int av_test_sections(AvSectionSeen* out, int cap) { // (the last whole panel, then the one being drawn: an editor of two panels has both)
    int n = 0;
    for (int i = 0; i < g_av_nlast && n < cap; i++) out[n++] = g_av_last[i];
    for (int i = 0; i < g_av_nseen && n < cap; i++) out[n++] = g_av_seen[i];
    return n;
}
static void av_seen(const char* title, bool shown, bool open) {
    if (g_av_nseen >= AV_SEEN_MAX) return;
    AvSectionSeen* s = &g_av_seen[g_av_nseen++];
    snprintf(s->title, sizeof s->title, "%s", title);
    s->shown = shown, s->open = open;
}

void av_details_search(char* buf, size_t cap) {
    av_seen_close(); // (the panel before: whole)
    if (g_av_test_set) snprintf(buf, cap, "%s", g_av_test_find), g_av_test_set = false;
    igSetNextItemWidth(-1);
    igInputTextWithHint("##av_find", TR("Search Details"), buf, cap, 0, NULL, NULL);
    g_av_find = buf;
}

bool av_section(const char* title, const char* keys) {
    const char* t = TR(title);
    bool searching = g_av_find && g_av_find[0];
    if (searching && !av_has(t, g_av_find) && !av_has(title, g_av_find) && !(keys && av_has(keys, g_av_find))) return av_seen(title, false, false), false;
    if (searching) igSetNextItemOpen(true, ImGuiCond_Always);
    bool open = igCollapsingHeader_TreeNodeFlags(t, ImGuiTreeNodeFlags_DefaultOpen);
    av_seen(title, true, open);
    return open;
}

void av_details_end(void) { g_av_find = NULL, av_seen_close(); }

// a row: the label dim on the left, the value on the right (Unreal's Details look, read-only)
void av_row(const char* label, const char* fmt, ...) {
    char v[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    igTextDisabled("%s", label);
    igSameLine(igGetFontSize() * 10.0f, -1);
    igTextUnformatted(v, NULL);
}

// --- the mesh's file: its LODs, vertices, memory ---
typedef struct MeshInfo {
    bool ok;
    uint32_t flags, nv, ni, nlods, tris[RMESH_MAX_LODS], verts[RMESH_MAX_LODS];
    float err[RMESH_MAX_LODS];
    double mem_kb[RMESH_MAX_LODS], total_kb;
    uint64_t file_bytes;
    uint32_t morphs;
} MeshInfo;
static MeshInfo g_mi;
static char g_mi_for[600];

static bool mesh_disk(const char* vpath, char* out, size_t cap) {
    const char* root = mat_root();
    if (strncmp(vpath, "/Content", 8) || !root || !root[0]) return false;
    snprintf(out, cap, "%s%s", root, vpath + 8);
    return true;
}

static void mesh_info_load(const char* vpath, MeshInfo* mi) {
    memset(mi, 0, sizeof *mi);
    char disk[1100];
    if (!mesh_disk(vpath, disk, sizeof disk)) return;
    uint64_t sz = 0;
    int64_t mt = 0;
    bool dir = false;
    if (re_file_stat(disk, &sz, &mt, &dir)) mi->file_bytes = sz;
    MeshBuilt mb;
    memset(&mb, 0, sizeof mb);
    if (!rmesh_read(disk, &mb, 1u << 26, 1u << 28)) return;
    mi->ok = true, mi->flags = mb.flags | (mb.col ? RMESH_COLORS : 0u), mi->nv = mb.nv, mi->ni = mb.ni, mi->morphs = mb.morph.count;
    mi->nlods = mb.lods.count ? mb.lods.count : 1u;
    for (uint32_t l = 0; l < mi->nlods && l < RMESH_MAX_LODS; l++) {
        uint32_t f = mb.lods.count ? mb.lods.first_index[l] : 0u, c = mb.lods.count ? mb.lods.index_count[l] : mb.ni;
        mi->tris[l] = c / 3u, mi->err[l] = mb.lods.count ? mb.lods.error[l] : 0.0f;
        mi->verts[l] = asset_mesh_used_verts(mb.idx, f, c);
        mi->mem_kb[l] = ((double)mi->verts[l] * sizeof(GpuVertex) + (double)c * 4.0) / 1024.0;
    }
    mi->total_kb = ((double)mb.nv * sizeof(GpuVertex) + (double)mb.ni * 4.0) / 1024.0;
    mesh_built_free(&mb);
}

int asset_mesh_lod_info(const char* vpath, unsigned tris[4]) {
    MeshInfo mi;
    mesh_info_load(vpath, &mi);
    for (int l = 0; l < 4; l++) tris[l] = l < (int)mi.nlods ? mi.tris[l] : 0u;
    return mi.ok ? (int)mi.nlods : 0;
}

// --- Build: the normals made again by a smoothing angle, a scale applied ---
static uint32_t av_oct(const float n[3]) { // (the octahedral normal, as the meshes keep it)
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

// Each vertex's normal: the faces around its place (every vertex at the same spot) whose normal is within the smoothing
// angle of its own face's, weighted by their area. Splits the mesh already has (UV seams, hard edges) stay
static bool recompute_normals(GpuVertex* v, uint32_t nv, const uint32_t* idx, uint32_t ni, float smoothing_deg) {
    uint32_t nt = ni / 3u, cap = 1;
    while (cap < nv * 2u) cap <<= 1;
    float* fn = malloc(sizeof(float) * 3 * (size_t)nt);
    float* acc = calloc((size_t)nv * 3, sizeof(float));
    uint32_t* grp = malloc(sizeof(uint32_t) * (size_t)nv);
    uint32_t* slot_v = malloc(sizeof(uint32_t) * (size_t)cap);
    uint32_t* cnt = NULL;
    uint32_t* faces = NULL;
    bool ok = fn && acc && grp && slot_v;
    uint32_t ng = 0;
    if (ok) { // the places: a hash of the position in 0.01 cm steps
        memset(slot_v, 0xFF, sizeof(uint32_t) * (size_t)cap);
        for (uint32_t i = 0; i < nv; i++) {
            int32_t q[3] = { (int32_t)lroundf(v[i].pos[0] * 100.0f), (int32_t)lroundf(v[i].pos[1] * 100.0f), (int32_t)lroundf(v[i].pos[2] * 100.0f) };
            uint32_t h = ((uint32_t)q[0] * 73856093u ^ (uint32_t)q[1] * 19349663u ^ (uint32_t)q[2] * 83492791u) & (cap - 1);
            for (;;) {
                uint32_t s = slot_v[h];
                if (s == UINT32_MAX) {
                    slot_v[h] = i, grp[i] = ng++;
                    break;
                }
                int32_t r[3] = { (int32_t)lroundf(v[s].pos[0] * 100.0f), (int32_t)lroundf(v[s].pos[1] * 100.0f), (int32_t)lroundf(v[s].pos[2] * 100.0f) };
                if (r[0] == q[0] && r[1] == q[1] && r[2] == q[2]) {
                    grp[i] = grp[s];
                    break;
                }
                h = (h + 1) & (cap - 1);
            }
        }
        cnt = calloc((size_t)ng + 1, sizeof(uint32_t));
        faces = malloc(sizeof(uint32_t) * (size_t)ni);
        ok = cnt && faces;
    }
    if (ok) {
        for (uint32_t t = 0; t < nt; t++) { // the faces' normals (by area) and, per place, the faces touching it
            const float* a = v[idx[t * 3]].pos;
            const float* b = v[idx[t * 3 + 1]].pos;
            const float* c = v[idx[t * 3 + 2]].pos;
            float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            fn[t * 3] = e1[1] * e2[2] - e1[2] * e2[1], fn[t * 3 + 1] = e1[2] * e2[0] - e1[0] * e2[2], fn[t * 3 + 2] = e1[0] * e2[1] - e1[1] * e2[0];
            for (int k = 0; k < 3; k++) cnt[grp[idx[t * 3 + k]] + 1]++;
        }
        for (uint32_t g = 0; g < ng; g++) cnt[g + 1] += cnt[g];
        uint32_t* fill = malloc(sizeof(uint32_t) * (size_t)ng);
        if (!fill) ok = false;
        else {
            memcpy(fill, cnt, sizeof(uint32_t) * (size_t)ng);
            for (uint32_t t = 0; t < nt; t++)
                for (int k = 0; k < 3; k++) faces[fill[grp[idx[t * 3 + k]]]++] = t;
            free(fill);
        }
    }
    if (ok) {
        float cosa = cosf(fminf(fmaxf(smoothing_deg, 0.0f), 180.0f) * 0.017453293f);
        for (uint32_t t = 0; t < nt; t++) {
            float* f = &fn[t * 3];
            float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            if (fl < 1e-12f) continue;
            for (int k = 0; k < 3; k++) { // its corner: the faces at this place close enough to this face
                uint32_t vi = idx[t * 3 + k], g = grp[vi];
                for (uint32_t j = cnt[g]; j < cnt[g + 1]; j++) {
                    const float* o = &fn[faces[j] * 3];
                    float ol = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
                    if (ol < 1e-12f || (f[0] * o[0] + f[1] * o[1] + f[2] * o[2]) < cosa * fl * ol) continue;
                    acc[vi * 3] += o[0], acc[vi * 3 + 1] += o[1], acc[vi * 3 + 2] += o[2];
                }
            }
        }
        for (uint32_t i = 0; i < nv; i++) {
            float* n = &acc[i * 3];
            float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (l > 1e-12f) n[0] /= l, n[1] /= l, n[2] /= l, v[i].normal_oct = av_oct(n);
        }
    }
    free(fn), free(acc), free(grp), free(slot_v), free(cnt), free(faces);
    return ok;
}

// --- the LOD Settings saved beside the mesh (.<file>.rlod): what the panel shows next time ---
static void lod_side(const char* disk, char* out, size_t cap) {
    const char* bn = disk + strlen(disk);
    while (bn > disk && bn[-1] != '/' && bn[-1] != '\\') bn--;
    snprintf(out, cap, "%.*s.%s.rlod", (int)(bn - disk), disk, bn);
}
static void lod_settings_default(AssetMeshBuild* b) {
    memset(b, 0, sizeof *b);
    b->lods = 4, b->percent[0] = 100.0f, b->percent[1] = 50.0f, b->percent[2] = 25.0f, b->percent[3] = 12.5f;
    b->min_tris = 64, b->smoothing_deg = 60.0f, b->scale = 1.0f;
    b->nanite = -1, b->fallback_pct = 100.0f, b->collision = 0; // (2026-11-24)
}
static bool lod_settings_load(const char* vpath, AssetMeshBuild* b) {
    lod_settings_default(b);
    char disk[1100], side[1200];
    if (!mesh_disk(vpath, disk, sizeof disk)) return false;
    lod_side(disk, side, sizeof side);
    FILE* f = re_fopen(side, "rb");
    if (!f) return false;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    JsonDoc d;
    if (!json_parse(buf, n, &d, NULL, 0)) return false;
    b->lods = json_get_int(&d.root, "lods", b->lods);
    json_get_floats(&d.root, "percent", b->percent, 4);
    b->min_tris = (unsigned)json_get_int(&d.root, "min_tris", (int)b->min_tris);
    b->smoothing_deg = (float)json_get_num(&d.root, "smoothing", b->smoothing_deg);
    b->nanite = json_get_int(&d.root, "nanite", b->nanite); // (2026-11-24: version 2's; absent in a version 1 file: the defaults)
    b->fallback_pct = (float)json_get_num(&d.root, "fallback", b->fallback_pct);
    b->collision = json_get_int(&d.root, "collision", b->collision);
    json_free(&d);
    if (b->lods < 1) b->lods = 1;
    if (b->lods > RMESH_MAX_LODS) b->lods = RMESH_MAX_LODS;
    return true;
}
static void lod_settings_save(const char* vpath, const AssetMeshBuild* b) {
    char disk[1100], side[1200];
    if (!mesh_disk(vpath, disk, sizeof disk)) return;
    lod_side(disk, side, sizeof side);
    FILE* f = re_fopen(side, "wb");
    if (!f) return;
    fprintf(f, "{ \"version\": 2, \"lods\": %d, \"percent\": [%g, %g, %g, %g], \"min_tris\": %u, \"smoothing\": %g, \"nanite\": %d, \"fallback\": %g, \"collision\": %d }\n",
            b->lods, b->percent[0], b->percent[1], b->percent[2], b->percent[3], b->min_tris, b->smoothing_deg, b->nanite, b->fallback_pct, b->collision);
    fclose(f);
}

// The mesh made again: its LODs from LOD 0 (b->lods 0: the automatic rule), and, asked for, its normals and its scale.
// Written over its file and every mesh read again (the GPU's copy, the level's actors). msg: what happened
bool asset_mesh_rebuild(const char* vpath, const AssetMeshBuild* b, bool normals, bool scale, char* msg, size_t cap) {
    char disk[1100];
    if (!mesh_disk(vpath, disk, sizeof disk)) return snprintf(msg, cap, "%s", TR("Not a mesh of this project.")), false;
    MeshBuilt mb, nb;
    memset(&mb, 0, sizeof mb), memset(&nb, 0, sizeof nb);
    if (!rmesh_read(disk, &mb, 1u << 26, 1u << 28)) return snprintf(msg, cap, TR("Could not read %s."), vpath), false;
    double t0 = re_time_seconds();
    bool ok = false;
    MeshLodOptions o;
    memset(&o, 0, sizeof o);
    o.lods = (uint32_t)(b->lods > 0 ? b->lods : 0);
    for (int l = 0; l < RMESH_MAX_LODS; l++) o.percent[l] = b->percent[l];
    o.min_tris = b->min_tris;
    if (mb.flags & RMESH_SKINNED) { // a Skeletal Mesh's skin: its LODs only (its vertices carry the bones)
        if (normals || scale) snprintf(msg, cap, "%s", TR("A skin keeps its normals and size (they come from its import)."));
        else if (b->lods < 2) snprintf(msg, cap, "%s", TR("A skin keeps at least 2 LODs (it gets the automatic ones back with 1)."));
        else {
            mesh_skinned_lods_opts(&mb, &o);
            ok = rmesh_write(disk, &mb);
            if (!ok) snprintf(msg, cap, TR("Could not write %s."), vpath);
        }
        mesh_built_free(&mb);
    } else if (mb.col || mb.morph.count) {
        snprintf(msg, cap, "%s", TR("Its LODs come from its import (it has vertex colors, morph targets or a skin)."));
        mesh_built_free(&mb);
    } else {
        uint32_t f0 = mb.lods.count ? mb.lods.first_index[0] : 0u, n0 = mb.lods.count ? mb.lods.index_count[0] : mb.ni;
        if (scale && b->scale > 0.0f && b->scale != 1.0f)
            for (uint32_t i = 0; i < mb.nv; i++)
                for (int k = 0; k < 3; k++) mb.v[i].pos[k] *= b->scale;
        if (normals) recompute_normals(mb.v, mb.nv, mb.idx + f0, n0, b->smoothing_deg);
        bool atlas = !(mb.flags & RMESH_NO_ATLAS) || b->lightmap; // (2026-11-24: Generate Lightmap UVs: the UV2 unwrap made)
        ok = b->lods > 0 ? mesh_build_indexed_lods(mb.v, mb.nv, mb.idx + f0, n0, atlas, &o, &nb) : mesh_build_indexed(mb.v, mb.nv, mb.idx + f0, n0, atlas, true, &nb);
        if (!ok) snprintf(msg, cap, "%s", TR("Could not simplify it."));
        else if (!rmesh_write(disk, &nb)) ok = false, snprintf(msg, cap, TR("Could not write %s."), vpath);
        mesh_built_free(&mb), mesh_built_free(&nb);
    }
    if (!ok) return false;
    if (b->lods > 0) lod_settings_save(vpath, b);
    if (g_vk) render_meshes_reload(g_vk); // (the GPU's copy, its bounds, the actors: the file read again)
    MeshInfo mi;
    mesh_info_load(vpath, &mi);
    snprintf(msg, cap, TR("Made again in %.0f ms: %u LODs, LOD 0 %u triangles."), (re_time_seconds() - t0) * 1000.0, mi.nlods, mi.tris[0]);
    g_mi_for[0] = 0; // (the panel reads it again)
    return true;
}
