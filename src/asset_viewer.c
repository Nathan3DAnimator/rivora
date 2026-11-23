// The asset viewers and the Content Browser's thumbnails (asset_viewer.h).
#include "asset_viewer.h"
#include "asset_viewport.h"

#define THEME_SCENE // (overlays on the 3D scene: theme.h)
#include "theme.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <cimgui_impl.h>

#include "anim.h"
#include "bp_preview.h"
#include "content.h"
#include "i18n.h"
#include "mat_editor.h"
#include "tex_settings.h"
#include "material.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "render_gpu.h"
#include "ui_icons.h"
#include "vkctx.h"

uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h); // (mat_image.c)
float* mat_hdr_decode(const uint8_t* d, size_t n, uint32_t* ow, uint32_t* oh); // (Q23, mat_image.c) a Radiance .hdr, linear RGB
uint8_t* mat_hdr_rgba8(const float* rgb, uint32_t w, uint32_t h, float exposure);

#define THUMB 128          // a thumbnail's side (px)
#define THUMB_MAX ASSET_THUMB_MAX // the thumbnails kept (64 KB each on the GPU)
#define VIEW_MAX_SIDE 4096 // the viewer's texture at most (a bigger one shown shrunk)

// --- GPU images for ImGui ---

typedef struct AvTex {
    GpuImage img;
    VkDescriptorSet set;
    uint32_t w, h;
} AvTex;

static VkCtx* g_vk;

static bool tex_upload(AvTex* t, const uint8_t* px, uint32_t w, uint32_t h) {
    VkCtx* vk = g_vk;
    if (!vk || !w || !h) return false;
    memset(t, 0, sizeof *t);
    if (!gpu_image_create(vk, &t->img, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    GpuBuf st = { 0 };
    VkDeviceSize n = (VkDeviceSize)w * h * 4;
    if (!gpu_buf_create(vk, &st, n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) {
        gpu_image_destroy(vk, &t->img);
        return false;
    }
    memcpy(st.mapped, px, (size_t)n);
    VkCommandBuffer cmd = gpu_begin_once(vk);
    gpu_transition(cmd, &t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
    VkBufferImageCopy r = { 0 };
    r.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageExtent = (VkExtent3D){ w, h, 1 };
    vkCmdCopyBufferToImage(cmd, st.buffer, t->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    gpu_transition(cmd, &t->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    gpu_end_once(vk, cmd);
    gpu_buf_destroy(vk, &st);
    t->set = ImGui_ImplVulkan_AddTexture(t->img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t->w = w, t->h = h;
    return t->set != VK_NULL_HANDLE;
}

// (a texture the UI may still draw this frame: freed a few frames later)
#define DEAD_MAX 64
static struct {
    AvTex t;
    uint64_t after;
} g_dead[DEAD_MAX];
static void tex_release(AvTex* t) {
    if (!t->img.image) return;
    for (int i = 0; i < DEAD_MAX; i++)
        if (!g_dead[i].t.img.image) {
            g_dead[i].t = *t, g_dead[i].after = g_vk ? g_vk->frame_number + 4 : 0;
            memset(t, 0, sizeof *t);
            return;
        }
    vkDeviceWaitIdle(g_vk->device); // (all slots waiting: rare)
    if (t->set) ImGui_ImplVulkan_RemoveTexture(t->set);
    gpu_image_destroy(g_vk, &t->img);
    memset(t, 0, sizeof *t);
}
static void dead_collect(bool all) {
    for (int i = 0; g_vk && i < DEAD_MAX; i++) {
        AvTex* t = &g_dead[i].t;
        if (!t->img.image || (!all && g_vk->frame_number < g_dead[i].after)) continue;
        if (t->set && igGetCurrentContext()) ImGui_ImplVulkan_RemoveTexture(t->set);
        gpu_image_destroy(g_vk, &t->img);
        memset(t, 0, sizeof *t);
    }
}
static ImTextureRef_c tex_ref(const AvTex* t) { return (ImTextureRef_c){ NULL, (ImTextureID)(uintptr_t)t->set }; }

static bool vpath_disk(const char* vpath, char* out, size_t cap) { // (backslashes: the material cache's key, as its editor's)
    if (strncmp(vpath, "/Content", 8)) return false;
    snprintf(out, cap, "%s%s", content_root(), vpath + 8);
    for (char* c = out; *c; c++)
        if (*c == '/') *c = '\\';
    return true;
}

// RGBA shrunk by a box filter to fit side x side (aspect kept); *ow, *oh its size
static uint8_t* shrink(const uint8_t* px, uint32_t w, uint32_t h, uint32_t side, uint32_t* ow, uint32_t* oh) {
    uint32_t nw = w, nh = h;
    if (w > side || h > side) {
        if (w >= h) nw = side, nh = (uint32_t)fmax(1.0, (double)h * side / w);
        else nh = side, nw = (uint32_t)fmax(1.0, (double)w * side / h);
    }
    uint8_t* o = malloc((size_t)nw * nh * 4);
    if (!o) return NULL;
    for (uint32_t y = 0; y < nh; y++)
        for (uint32_t x = 0; x < nw; x++) {
            uint32_t x0 = x * w / nw, x1 = (x + 1) * w / nw, y0 = y * h / nh, y1 = (y + 1) * h / nh;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            uint32_t sum[4] = { 0 }, cnt = 0;
            for (uint32_t sy = y0; sy < y1 && sy < h; sy += (y1 - y0 > 8 ? (y1 - y0) / 8 : 1))
                for (uint32_t sx = x0; sx < x1 && sx < w; sx += (x1 - x0 > 8 ? (x1 - x0) / 8 : 1)) {
                    const uint8_t* p = &px[((size_t)sy * w + sx) * 4];
                    for (int c = 0; c < 4; c++) sum[c] += p[c];
                    cnt++;
                }
            for (int c = 0; c < 4; c++) o[((size_t)y * nw + x) * 4 + c] = (uint8_t)(sum[c] / (cnt ? cnt : 1));
        }
    *ow = nw, *oh = nh;
    return o;
}

// --- Thumbnails ---

enum { TH_FREE = 0, TH_WANTED, TH_DECODING, TH_READY, TH_DONE, TH_FAILED };
typedef struct Thumb {
    uint32_t hash;
    volatile int32_t state;
    int kind;
    char disk[600];
    uint8_t* px; // TH_READY: the shrunk pixels
    uint32_t w, h;
    AvTex tex;
    int64_t mtime; // (Q23) the file's date and size when it was made: changed on disk, it is made again
    uint64_t fsize;
} Thumb;
static Thumb* g_th;
static int g_nth;
static int g_watch;                    // (Q23) the next thumbnail whose file is looked at (one a frame)
static volatile int32_t g_th_refreshes; // (the worker's)
static unsigned g_av_reloads;
static double g_watch_us, g_view_checked;
static ReThread* g_worker;
static volatile int32_t g_quit, g_wanted;

static uint32_t hash_str(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1u;
}

// (Q23) The thumbnails follow their files: a texture painted again in another program, a mesh exported again, showed the
// old picture until the editor restarted (each thumbnail made once a session). The worker, when it has nothing to
// decode, looks at one thumbnail's file every 15 ms (a stat: 30-150 µs in a folder an antivirus or OneDrive watches;
// none of it on the main thread); changed, the thumbnail is made again (a texture decoded by the worker, a mesh
// rasterized again by the main thread). Its old image stays drawn until the new one is uploaded.
static void thumb_watch(void) {
    int n = re_atomic_load((volatile int32_t*)&g_nth);
    if (!g_th || n <= 0) return;
    double t0 = re_time_seconds();
    for (int tries = 0; tries < 4; tries++) { // (the next one shown or failed: the others are on their way)
        Thumb* t = &g_th[g_watch % n];
        g_watch = (g_watch + 1) % n;
        int st = re_atomic_load(&t->state);
        if (st != TH_DONE && st != TH_FAILED) continue;
        uint64_t sz = 0;
        int64_t mt = 0;
        bool dir = false;
        if (re_file_stat(t->disk, &sz, &mt, &dir) && (mt != t->mtime || sz != t->fsize)) {
            bool mesh = t->kind == ASSET_STATIC_MESH || t->kind == ASSET_SKELETAL_MESH || t->kind == ASSET_ANIMATION;
            if (re_atomic_cas(&t->state, st, mesh ? TH_FREE : TH_WANTED)) t->mtime = mt, t->fsize = sz, re_atomic_add(&g_th_refreshes, 1);
        }
        break;
    }
    g_watch_us = (re_time_seconds() - t0) * 1e6;
}

// The worker: the wanted textures decoded and shrunk (WIC; the main thread uploads them)
static int worker(void* arg) {
    (void)arg;
    re_thread_set_low_priority(true);
    while (!re_atomic_load(&g_quit)) {
        bool any = false;
        for (int i = 0; i < g_nth && !re_atomic_load(&g_quit); i++) {
            Thumb* t = &g_th[i];
            if (!re_atomic_cas(&t->state, TH_WANTED, TH_DECODING)) continue;
            any = true;
            uint32_t w = 0, h = 0;
            uint8_t* px = mat_load_image_rgba(t->disk, &w, &h);
            uint8_t* shrunk = px ? shrink(px, w, h, THUMB, &t->w, &t->h) : NULL;
            free(px);
            t->px = shrunk;
            re_atomic_store(&t->state, shrunk ? TH_READY : TH_FAILED);
        }
        if (!any) thumb_watch(), Sleep(15); // (Q23: idle, a file looked at)
    }
    return 0;
}

// The meshes' thumbnail: their triangles (the Engine's space) rasterized at 2x with a depth buffer, lit by a key light
// and the sky, from the front-left and above; then halved (smooth edges). tri(i, p[3][3]) gives triangle i.
typedef struct RasterSrc {
    uint32_t mesh;   // a .rmesh: its id; or a Skeletal Mesh:
    SkelMesh* sk;
    const float (*pose)[3][4]; // its bones posed (an animation's frame; NULL: the bind pose)
    bool framed;               // the view's center and radius given (an animation's frames all framed alike)
    float center[3], radius;
    bool view;                 // the view's yaw and pitch given (else from the front-left and above)
    float yaw, pitch;          // degrees
    uint32_t material;         // (2026-11-10) a .rmesh's own material (content_mesh_material): its mean color tints it
} RasterSrc;
// A triangle's color in the thumbnail: its material's baked mean (once baked), else the part's color, else light gray
static struct { uint32_t key[16]; float rgb[16][3]; int n; } g_tc; // (per thumbnail: a material looked up once, not per triangle)
static void tri_color(uint32_t mat, const float* color, float out[3]) {
    if (mat) {
        for (int k = 0; k < g_tc.n; k++)
            if (g_tc.key[k] == mat) {
                if (g_tc.rgb[k][0] >= 0.0f) {
                    memcpy(out, g_tc.rgb[k], 12);
                    return;
                }
                mat = 0; // (looked up: none)
                break;
            }
    }
    const Material* m = mat ? mat_find_hash(mat) : NULL;
    if (mat && g_tc.n < 16) {
        g_tc.key[g_tc.n] = mat;
        if (m && (m->avg_base[0] + m->avg_base[1] + m->avg_base[2]) > 0.0f) memcpy(g_tc.rgb[g_tc.n], m->avg_base, 12);
        else g_tc.rgb[g_tc.n][0] = -1.0f;
        g_tc.n++;
    }
    if (m && (m->avg_base[0] + m->avg_base[1] + m->avg_base[2]) > 0.0f) memcpy(out, m->avg_base, 12);
    else if (color) memcpy(out, color, 12);
    else out[0] = out[1] = out[2] = 0.78f;
}
static uint32_t src_tris(const RasterSrc* s) {
    const GpuVertex* v;
    const uint32_t* idx;
    uint32_t first, count;
    int32_t off;
    if (!s->sk) {
        render_mesh_cpu(s->mesh, &v, &idx, &first, &count, &off);
        return count / 3;
    }
    uint32_t n = 0;
    for (uint32_t p = 0; p < s->sk->nparts; p++) {
        render_mesh_cpu(s->sk->parts[p].mesh, &v, &idx, &first, &count, &off);
        n += count / 3;
    }
    for (uint32_t k = 0; k < s->sk->nskins; k++) { // (the skins of a v2 import: bent by the pose)
        render_mesh_cpu(s->sk->skins[k].mesh, &v, &idx, &first, &count, &off);
        n += count / 3;
    }
    return n;
}

// A skin's vertex at the pose (skin.comp's: its 4 bones' palette rows, pose x bind inverse, weighted)
static void skin_point(const SkelMesh* sk, const float (*pose)[3][4], const GpuVertex* gv, float out[3]) {
    if (!pose) {
        memcpy(out, gv->pos, 12);
        return;
    }
    uint32_t bits[2];
    memcpy(bits, gv->uv2, 8);
    float acc[3] = { 0, 0, 0 }, ws = 0.0f;
    for (int k = 0; k < 4; k++) {
        float w = (float)((bits[1] >> (k * 8)) & 255u) / 255.0f;
        uint32_t b = (bits[0] >> (k * 8)) & 255u;
        if (w <= 0.0f || b >= sk->nbones) continue;
        float l[3], p[3];
        for (int a = 0; a < 3; a++) // (into the bone's bind space, then out by its pose)
            l[a] = sk->bind_inv[b][a][0] * gv->pos[0] + sk->bind_inv[b][a][1] * gv->pos[1] + sk->bind_inv[b][a][2] * gv->pos[2] + sk->bind_inv[b][a][3];
        for (int a = 0; a < 3; a++) p[a] = pose[b][a][0] * l[0] + pose[b][a][1] * l[1] + pose[b][a][2] * l[2] + pose[b][a][3];
        for (int a = 0; a < 3; a++) acc[a] += p[a] * w;
        ws += w;
    }
    if (ws <= 0.0f) memcpy(out, gv->pos, 12);
    else for (int a = 0; a < 3; a++) out[a] = acc[a] / ws;
}
// Triangle i in the Engine's space (a Skeletal Mesh's parts at its bind)
static bool src_tri(const RasterSrc* s, uint32_t i, float out[3][3], float col[3]) {
    const GpuVertex* v;
    const uint32_t* idx;
    uint32_t first, count;
    int32_t off;
    float rows[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    if (!s->sk) {
        render_mesh_cpu(s->mesh, &v, &idx, &first, &count, &off);
        if (col) tri_color(s->material, NULL, col);
    } else {
        uint32_t p = 0;
        for (; p < s->sk->nparts; p++) {
            render_mesh_cpu(s->sk->parts[p].mesh, &v, &idx, &first, &count, &off);
            if (i < count / 3) break;
            i -= count / 3;
        }
        if (p >= s->sk->nparts) { // past the parts: a skin's triangle
            for (uint32_t k = 0; k < s->sk->nskins; k++) {
                render_mesh_cpu(s->sk->skins[k].mesh, &v, &idx, &first, &count, &off);
                if (i < count / 3) {
                    if (i * 3 + 2 >= count) return false;
                    if (col) tri_color(s->sk->skins[k].material, s->sk->skins[k].color, col);
                    for (int c = 0; c < 3; c++) skin_point(s->sk, s->pose, &v[(int64_t)idx[first + i * 3 + c] + off], out[c]);
                    return true;
                }
                i -= count / 3;
            }
            return false;
        }
        const SkPart* part = &s->sk->parts[p];
        if (col) tri_color(part->material, part->color, col);
        anim_part_rows(part, s->pose ? s->pose[part->bone] : s->sk->bind_world[part->bone], rows); // (cm: a shape's scale 1 is 100 cm)
    }
    if (i * 3 + 2 >= count) return false;
    for (int k = 0; k < 3; k++) {
        const float* p = v[(int64_t)idx[first + i * 3 + k] + off].pos;
        for (int r = 0; r < 3; r++) out[k][r] = rows[r][0] * p[0] + rows[r][1] * p[1] + rows[r][2] * p[2] + rows[r][3];
    }
    return true;
}

static uint8_t* raster_thumb(const RasterSrc* s);

// --- An animation's thumbnail: its skeleton's Skeletal Mesh posed (the static one at 40% of the clip); hovered, the
// clip plays (LIVE_FRAMES frames rasterized one per UI frame, kept while the pointer stays, released after) ---
#define LIVE_FRAMES 24
static struct {
    char vpath[600];
    AvTex tex[LIVE_FRAMES];
    int nready;
    bool ok;
    float len, center[3], radius;
    double last_hover;
} L;
static float g_pose[ANIM_MAX_BONES][3][4];

// The clip's view: the bind pose's bounds moved along the root's travel (so every frame is framed alike)
static bool anim_view(AnimSeq* q, SkelMesh* sk, float c[3], float* rad) {
    static AnimXf local[ANIM_MAX_BONES];
    static const float id[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    RasterSrc b = { 0 };
    b.sk = sk;
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f }, t[3][3];
    for (uint32_t i = 0, nt = src_tris(&b); i < nt; i++)
        if (src_tri(&b, i, t, NULL))
            for (int k = 0; k < 3; k++)
                for (int a = 0; a < 3; a++) mn[a] = fminf(mn[a], t[k][a]), mx[a] = fmaxf(mx[a], t[k][a]);
    if (mn[0] > mx[0]) return false;
    float rmn[3] = { 1e30f, 1e30f, 1e30f }, rmx[3] = { -1e30f, -1e30f, -1e30f };
    for (int k = 0; k <= 8; k++) { // the root's way through the clip
        anim_sample(q, sk, q->length * k / 8.0f, local);
        anim_world(sk, local, id, g_pose);
        for (int a = 0; a < 3; a++) rmn[a] = fminf(rmn[a], g_pose[0][a][3] - sk->bind_world[0][a][3]), rmx[a] = fmaxf(rmx[a], g_pose[0][a][3] - sk->bind_world[0][a][3]);
    }
    float d2 = 0.0f;
    for (int a = 0; a < 3; a++) {
        c[a] = (mn[a] + mx[a]) * 0.5f + (rmn[a] + rmx[a]) * 0.5f;
        float h = (mx[a] - mn[a]) * 0.5f + (rmx[a] - rmn[a]) * 0.5f;
        d2 += h * h;
    }
    *rad = sqrtf(d2) * 1.05f; // (a little room: arms swing past the bind bounds)
    return true;
}
static bool anim_src(const char* vpath, float t01, RasterSrc* src, const float c[3], float rad) {
    static AnimXf local[ANIM_MAX_BONES];
    static const float id[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    AnimSeq* q = anim_seq_load(vpath);
    SkelMesh* sk = q ? anim_mesh_name(q->skeleton) : NULL;
    if (!sk) return false;
    anim_sample(q, sk, q->length * t01, local);
    anim_world(sk, local, id, g_pose);
    memset(src, 0, sizeof *src);
    src->sk = sk, src->pose = (const float (*)[3][4])g_pose, src->framed = true, src->radius = rad;
    memcpy(src->center, c, sizeof src->center);
    return true;
}
static uint8_t* raster_anim(const char* vpath, float t01) {
    AnimSeq* q = anim_seq_load(vpath);
    SkelMesh* sk = q ? anim_mesh_name(q->skeleton) : NULL;
    float c[3], rad;
    RasterSrc src;
    if (!sk || !anim_view(q, sk, c, &rad) || !anim_src(vpath, t01, &src, c, rad)) return NULL;
    return raster_thumb(&src);
}

// An animation's frames side by side in a PNG (the command line's --anim-sheet: the animations' reviews): n frames
// seen from yaw (90: from the side), each THUMB pixels, on a dark gray
bool asset_anim_sheet(const char* vpath, const char* png, int n, float yaw) {
    AnimSeq* q = anim_seq_load(vpath);
    SkelMesh* sk = q ? anim_mesh_name(q->skeleton) : NULL;
    float c[3], rad;
    if (!sk || n < 1 || !anim_view(q, sk, c, &rad)) return false;
    uint8_t* rgb = malloc((size_t)THUMB * n * THUMB * 3);
    if (!rgb) return false;
    for (int f = 0; f < n; f++) {
        RasterSrc src;
        anim_src(vpath, q->loop ? (float)f / n : (float)f / (n > 1 ? n - 1 : 1), &src, c, rad);
        src.view = true, src.yaw = yaw, src.pitch = 8.0f;
        uint8_t* px = raster_thumb(&src);
        for (int y = 0; y < THUMB; y++)
            for (int x = 0; x < THUMB; x++) {
                const uint8_t* p = px ? &px[(y * THUMB + x) * 4] : NULL;
                uint8_t* o = &rgb[((size_t)y * THUMB * n + (size_t)f * THUMB + x) * 3];
                float a = p ? p[3] / 255.0f : 0.0f, bg = (x == 0 || ((y / 16) & 1) == 0 && x % 32 == 0) ? 60.0f : 40.0f;
                for (int k = 0; k < 3; k++) o[k] = (uint8_t)(bg + ((p ? p[k] : 0) - bg) * a);
            }
        free(px);
    }
    bool ok = vkctx_png_write_rgb(png, rgb, (uint32_t)(THUMB * n), THUMB);
    free(rgb);
    return ok;
}

void asset_thumb_hover(const char* vpath) {
    if (!vpath) return;
    L.last_hover = igGetTime();
    if (!strcmp(L.vpath, vpath)) return;
    for (int i = 0; i < LIVE_FRAMES; i++) tex_release(&L.tex[i]);
    snprintf(L.vpath, sizeof L.vpath, "%s", vpath);
    L.nready = 0, L.ok = false;
    AnimSeq* q = anim_seq_load(vpath);
    SkelMesh* sk = q ? anim_mesh_name(q->skeleton) : NULL;
    if (sk && q->length > 0.0f && anim_view(q, sk, L.center, &L.radius)) L.ok = true, L.len = q->length;
}
// Every frame (asset_viewer_draw): the hovered clip's next frame rasterized; the frames dropped once it isn't hovered
static void live_step(void) {
    if (!L.vpath[0]) return;
    if (igGetTime() - L.last_hover > 0.25 || !L.ok) {
        for (int i = 0; i < LIVE_FRAMES; i++) tex_release(&L.tex[i]);
        L.vpath[0] = 0, L.nready = 0;
        return;
    }
    if (L.nready >= LIVE_FRAMES) return;
    RasterSrc src;
    double t0 = re_time_seconds();
    while (L.nready < LIVE_FRAMES && anim_src(L.vpath, (float)L.nready / LIVE_FRAMES, &src, L.center, L.radius)) {
        uint8_t* px = raster_thumb(&src);
        if (!px || !tex_upload(&L.tex[L.nready], px, THUMB, THUMB)) {
            free(px);
            L.ok = false;
            return;
        }
        free(px);
        L.nready++;
        if (re_time_seconds() - t0 > 0.004) break; // (4 ms a frame at most: a dense character takes a few frames)
    }
}

static uint8_t* raster_thumb(const RasterSrc* s) {
    enum { S2 = THUMB * 2 };
    g_tc.n = 0;
    uint32_t nt = src_tris(s);
    if (!nt) return NULL;
    float c[3], rad, t[3][3];
    if (s->framed) {
        memcpy(c, s->center, sizeof c), rad = s->radius;
    } else {
        float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        for (uint32_t i = 0; i < nt; i++)
            if (src_tri(s, i, t, NULL))
                for (int k = 0; k < 3; k++)
                    for (int a = 0; a < 3; a++) mn[a] = fminf(mn[a], t[k][a]), mx[a] = fmaxf(mx[a], t[k][a]);
        if (mn[0] > mx[0]) return NULL;
        for (int a = 0; a < 3; a++) c[a] = (mn[a] + mx[a]) * 0.5f;
        rad = 0.5f * sqrtf((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) + (mx[2] - mn[2]) * (mx[2] - mn[2]));
    }
    if (rad < 1e-4f) rad = 1.0f;
    // the view: from the front-left and above (yaw 45, pitch 25), orthographic, the bounds filling 90%
    float yaw = (s->view ? s->yaw : 45.0f) * 0.0174533f, pitch = (s->view ? s->pitch : 25.0f) * 0.0174533f;
    float f[3] = { cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), -sinf(pitch) }, r[3] = { -sinf(yaw), cosf(yaw), 0.0f };
    float u[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
    float L[3] = { -f[0] + 0.3f * r[0] + 0.8f * u[0], -f[1] + 0.3f * r[1] + 0.8f * u[1], -f[2] + 0.3f * r[2] + 0.8f * u[2] };
    float ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    for (int k = 0; k < 3; k++) L[k] /= ll;
    float scale = 0.45f * S2 / rad;
    float* zb = malloc(sizeof(float) * S2 * S2);
    uint8_t* big = calloc((size_t)S2 * S2, 4);
    uint8_t* out = malloc((size_t)THUMB * THUMB * 4);
    if (!zb || !big || !out) {
        free(zb), free(big), free(out);
        return NULL;
    }
    for (int i = 0; i < S2 * S2; i++) zb[i] = 1e30f;
    for (uint32_t i = 0; i < nt; i++) {
        float tc[3];
        if (!src_tri(s, i, t, tc)) continue;
        float sx[3], sy[3], sz[3];
        for (int k = 0; k < 3; k++) {
            float d[3] = { t[k][0] - c[0], t[k][1] - c[1], t[k][2] - c[2] };
            sx[k] = S2 * 0.5f + (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) * scale;
            sy[k] = S2 * 0.5f - (d[0] * u[0] + d[1] * u[1] + d[2] * u[2]) * scale;
            sz[k] = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
        }
        float e1[3] = { t[1][0] - t[0][0], t[1][1] - t[0][1], t[1][2] - t[0][2] }, e2[3] = { t[2][0] - t[0][0], t[2][1] - t[0][1], t[2][2] - t[0][2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl < 1e-12f) continue;
        for (int k = 0; k < 3; k++) n[k] /= nl;
        if (n[0] * f[0] + n[1] * f[1] + n[2] * f[2] > 0.0f) // (two-sided: the side facing the view)
            for (int k = 0; k < 3; k++) n[k] = -n[k];
        float diff = fmaxf(n[0] * L[0] + n[1] * L[1] + n[2] * L[2], 0.0f), sky = 0.5f + 0.5f * (n[0] * u[0] + n[1] * u[1] + n[2] * u[2]);
        float lit = fminf(0.18f + 0.62f * diff + 0.25f * sky, 1.0f);
        // (the color: linear base to the screen's sRGB, roughly)
        uint8_t cr = (uint8_t)fminf(255.0f, powf(fminf(tc[0] * lit * 1.15f, 1.0f), 1.0f / 2.2f) * 255.0f);
        uint8_t cg = (uint8_t)fminf(255.0f, powf(fminf(tc[1] * lit * 1.15f, 1.0f), 1.0f / 2.2f) * 255.0f);
        uint8_t cb = (uint8_t)fminf(255.0f, powf(fminf(tc[2] * lit * 1.15f, 1.0f), 1.0f / 2.2f) * 255.0f);
        float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (fabsf(area) < 1e-6f) continue;
        int x0 = (int)fmaxf(floorf(fminf(fminf(sx[0], sx[1]), sx[2])), 0.0f), x1 = (int)fminf(ceilf(fmaxf(fmaxf(sx[0], sx[1]), sx[2])), S2 - 1.0f);
        int y0 = (int)fmaxf(floorf(fminf(fminf(sy[0], sy[1]), sy[2])), 0.0f), y1 = (int)fminf(ceilf(fmaxf(fmaxf(sy[0], sy[1]), sy[2])), S2 - 1.0f);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
                float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                int at = y * S2 + x;
                if (z >= zb[at]) continue;
                zb[at] = z;
                uint8_t* p = &big[at * 4];
                p[0] = cr, p[1] = cg, p[2] = cb, p[3] = 255;
            }
    }
    for (int y = 0; y < THUMB; y++) // (halved: 2x2 averaged, edges smooth)
        for (int x = 0; x < THUMB; x++)
            for (int k = 0; k < 4; k++) {
                int s4 = big[((y * 2) * S2 + x * 2) * 4 + k] + big[((y * 2) * S2 + x * 2 + 1) * 4 + k] + big[((y * 2 + 1) * S2 + x * 2) * 4 + k] +
                         big[((y * 2 + 1) * S2 + x * 2 + 1) * 4 + k];
                out[(y * THUMB + x) * 4 + k] = (uint8_t)(s4 / 4);
            }
    // (the colors of half-covered pixels un-darkened: they were averaged with transparent black)
    for (int i = 0; i < THUMB * THUMB; i++) {
        uint8_t* p = &out[i * 4];
        if (p[3] && p[3] < 255)
            for (int k = 0; k < 3; k++) p[k] = (uint8_t)fminf(p[k] * 255.0f / p[3], 255.0f);
    }
    free(zb), free(big);
    return out;
}

static Thumb* thumb_find(const char* vpath, int kind, bool add) {
    uint32_t h = hash_str(vpath);
    for (int i = 0; i < g_nth; i++)
        if (g_th[i].hash == h) return &g_th[i];
    if (!add || !g_th || g_nth >= THUMB_MAX) return NULL;
    Thumb* t = &g_th[g_nth];
    memset(t, 0, sizeof *t);
    t->hash = h, t->kind = kind;
    if (!vpath_disk(vpath, t->disk, sizeof t->disk)) return NULL;
    bool dir = false;
    if (!re_file_stat(t->disk, &t->fsize, &t->mtime, &dir)) t->fsize = 0, t->mtime = 0; // (Q23)
    // (published last: the worker scans up to g_nth)
    re_atomic_store(&t->state, kind == ASSET_TEXTURE_SOURCE ? TH_WANTED : TH_FREE);
    re_atomic_add((volatile int32_t*)&g_nth, 1);
    return t;
}

// A material's preview as an ImGui texture (the views change when it is baked again)
static struct {
    const Material* m;
    VkImageView view;
    VkDescriptorSet set;
} g_mats[256];
static ImTextureID mat_tex(const Material* m) {
    VkImageView v = mat_preview_view(m, 0);
    int slot = -1, free_slot = -1;
    for (int i = 0; i < 256; i++) {
        if (g_mats[i].m == m) slot = i;
        if (!g_mats[i].m && free_slot < 0) free_slot = i;
    }
    if (slot < 0) slot = free_slot;
    if (slot < 0 || !v) return 0;
    if (g_mats[slot].view != v) {
        if (g_mats[slot].set) { // (drawn by a frame in flight, maybe: the GPU waited for, once per bake)
            vkDeviceWaitIdle(g_vk->device);
            ImGui_ImplVulkan_RemoveTexture(g_mats[slot].set);
        }
        g_mats[slot].set = ImGui_ImplVulkan_AddTexture(v, VK_IMAGE_LAYOUT_GENERAL);
        g_mats[slot].view = v, g_mats[slot].m = m;
    }
    return (ImTextureID)(uintptr_t)g_mats[slot].set;
}

static int g_raster_budget; // the meshes rasterized this frame (one: it takes a few ms)

bool asset_thumb(struct ImDrawList* dl, float x0, float y0, float x1, float y1, const char* vpath, int kind) {
    if (!g_vk || !g_th) return false;
    if (kind == ASSET_MATERIAL || kind == ASSET_MATERIAL_INSTANCE) {
        char disk[600];
        Material* m = vpath_disk(vpath, disk, sizeof disk) ? mat_load(disk) : NULL;
        ImTextureID tex = m ? mat_tex(m) : 0;
        if (!tex) return false;
        float size = fminf(x1 - x0, y1 - y0);
        ImVec2 o = { x0 + (x1 - x0 - size) * 0.5f, y0 + (y1 - y0 - size) * 0.5f };
        ImDrawList_PushClipRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, true);
        mat_sphere_draw(dl, (ImVec2){ o.x - size * 0.05f, o.y - size * 0.05f }, size * 1.1f, m, tex, 0, -30.0f, 15.0f, 24, 12);
        ImDrawList_PopClipRect(dl);
        return true;
    }
    if (kind == ASSET_ANIMATION && !strcmp(L.vpath, vpath) && L.nready == LIVE_FRAMES) { // hovered: the clip plays
        int fr = (int)(fmod(igGetTime(), (double)L.len) / L.len * LIVE_FRAMES) % LIVE_FRAMES;
        float bw = x1 - x0, bh = y1 - y0, sz = fminf(bw, bh), ox = x0 + (bw - sz) * 0.5f, oy = y0 + (bh - sz) * 0.5f;
        ImDrawList_AddImage(dl, tex_ref(&L.tex[fr]), (ImVec2){ ox, oy }, (ImVec2){ ox + sz, oy + sz }, (ImVec2){ 0, 0 }, (ImVec2){ 1, 1 }, THEME_IMAGE_TINT);
        return true;
    }
    if (kind != ASSET_TEXTURE_SOURCE && kind != ASSET_STATIC_MESH && kind != ASSET_SKELETAL_MESH && kind != ASSET_ANIMATION) return false;
    Thumb* t = thumb_find(vpath, kind, true);
    if (!t) return false;
    int st = re_atomic_load(&t->state);
    if (st == TH_FREE && g_raster_budget > 0) { // a mesh: rasterized here (the renderer's CPU copy is the main thread's)
        g_raster_budget--;
        RasterSrc src = { 0 };
        uint8_t* px = NULL;
        if (kind == ASSET_ANIMATION) px = raster_anim(vpath, 0.4f);
        else {
            if (kind == ASSET_STATIC_MESH) src.mesh = !strncmp(vpath, "#mesh/", 6) ? (uint32_t)strtoul(vpath + 6, NULL, 16) : render_mesh_id_of(vpath); // ("#mesh/<id>": a mesh by its id, a basic shape too)
            if (kind == ASSET_STATIC_MESH && strncmp(vpath, "#mesh/", 6) != 0) src.material = content_mesh_material(vpath);
            else src.sk = anim_mesh_load(vpath);
            px = kind == ASSET_STATIC_MESH || src.sk ? raster_thumb(&src) : NULL;
        }
        t->px = px, t->w = t->h = THUMB;
        re_atomic_store(&t->state, px ? TH_READY : TH_FAILED);
        st = re_atomic_load(&t->state);
    }
    if (st == TH_READY) { // (uploaded where it is drawn: a few a frame at most, 64 KB each)
        tex_release(&t->tex); // (Q23: made again, the old image after the frames in flight)
        bool ok = tex_upload(&t->tex, t->px, t->w, t->h);
        free(t->px), t->px = NULL;
        re_atomic_store(&t->state, ok ? TH_DONE : TH_FAILED);
        st = re_atomic_load(&t->state);
    }
    if (st == TH_WANTED || st == TH_DECODING || (st == TH_FREE && kind != ASSET_TEXTURE_SOURCE)) { // (on its way: a spinner)
        float sz = fminf(x1 - x0, y1 - y0) * 0.22f;
        ui_spinner(dl, x1 - sz - 2.0f, y0 + 2.0f, sz, THA(TK_CYAN, 192), igGetTime());
    }
    if (st != TH_DONE) return false;
    // the image fit in the rectangle, a checker under a texture (its alpha)
    float bw = x1 - x0, bh = y1 - y0, s = fminf(bw / t->tex.w, bh / t->tex.h);
    float w = t->tex.w * s, h = t->tex.h * s, ox = x0 + (bw - w) * 0.5f, oy = y0 + (bh - h) * 0.5f;
    if (kind == ASSET_TEXTURE_SOURCE) {
        float cs = fmaxf(w, h) / 8.0f;
        for (int y = 0; y * cs < h; y++)
            for (int x = 0; x * cs < w; x++)
                ImDrawList_AddRectFilled(dl, (ImVec2){ ox + x * cs, oy + y * cs }, (ImVec2){ fminf(ox + (x + 1) * cs, ox + w), fminf(oy + (y + 1) * cs, oy + h) },
                                         (x + y) & 1 ? TH(TK_N8) : TH(TK_N7), 0, 0);
    }
    ImDrawList_AddImage(dl, tex_ref(&t->tex), (ImVec2){ ox, oy }, (ImVec2){ ox + w, oy + h }, (ImVec2){ 0, 0 }, (ImVec2){ 1, 1 }, THEME_IMAGE_TINT);
    return true;
}

// --- The viewer ---

static struct {
    int kind; // 0 closed, ASSET_TEXTURE_SOURCE or ASSET_STATIC_MESH
    char vpath[600], name[160];
    // a texture
    uint8_t* px; // the shown pixels (at most VIEW_MAX_SIDE)
    uint32_t w, h, src_w, src_h;
    long long bytes;
    bool alpha, ch[4];
    AvTex tex;
    float zoom, pan[2];
    int mip, mips;          // (Q23) the mip shown (0: the image) and how many the texture has (down to 1 x 1)
    float* hdr;             // (Q23) a .hdr's linear RGB at the shown size (its exposure redoes px)
    float exposure;         // stops
    char error[200];        // (Q23) why it didn't open (a format nothing here decodes): said, not a window that never shows
    int64_t mtime;          // (Q23) the file's: changed on disk (painted in another program), it is read again
    uint64_t fsize;
    // a mesh
    uint32_t mesh, tris, verts, material; // material: the preview's (a hash; 0: gray)
    int view_mode; // PREVIEW_ (render.h)
    float mn[3], mx[3];
    BpPreviewCam cam;
    bool grid;
    AssetViewport vp; // (Q7) the asset viewport
} V;
static uint32_t guess_material(const char* mesh_name);

static void view_upload(void) {
    tex_release(&V.tex);
    if (!V.px) return;
    // (Q23) a mip: the image box-filtered to its size, drawn at the image's (how it looks that far away)
    uint32_t mw = V.w, mh = V.h;
    uint8_t* src = V.px;
    if (V.mip > 0) {
        uint32_t side = (V.w > V.h ? V.w : V.h) >> V.mip;
        src = shrink(V.px, V.w, V.h, side ? side : 1, &mw, &mh);
        if (!src) return;
    }
    uint8_t* show = malloc((size_t)mw * mh * 4);
    if (!show) {
        if (src != V.px) free(src);
        return;
    }
    bool only_a = V.ch[3] && !V.ch[0] && !V.ch[1] && !V.ch[2];
    for (size_t i = 0; i < (size_t)mw * mh; i++) {
        const uint8_t* s = &src[i * 4];
        uint8_t* d = &show[i * 4];
        if (only_a) d[0] = d[1] = d[2] = s[3], d[3] = 255; // (the alpha as gray)
        else
            for (int c = 0; c < 3; c++) d[c] = V.ch[c] ? s[c] : 0;
        if (!only_a) d[3] = V.ch[3] ? s[3] : 255;
    }
    tex_upload(&V.tex, show, mw, mh);
    free(show);
    if (src != V.px) free(src);
}

// (Q23) A .hdr's floats box-filtered to fit side x side (as shrink)
static float* hdr_shrink(const float* px, uint32_t w, uint32_t h, uint32_t side, uint32_t* ow, uint32_t* oh) {
    uint32_t nw = w, nh = h;
    if (w > side || h > side) {
        if (w >= h) nw = side, nh = (uint32_t)fmax(1.0, (double)h * side / w);
        else nh = side, nw = (uint32_t)fmax(1.0, (double)w * side / h);
    }
    float* o = malloc((size_t)nw * nh * 3 * sizeof(float));
    if (!o) return NULL;
    for (uint32_t y = 0; y < nh; y++)
        for (uint32_t x = 0; x < nw; x++) {
            uint32_t x0 = x * w / nw, x1 = (x + 1) * w / nw, y0 = y * h / nh, y1 = (y + 1) * h / nh;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            float sum[3] = { 0 };
            uint32_t cnt = 0;
            for (uint32_t sy = y0; sy < y1 && sy < h; sy++)
                for (uint32_t sx = x0; sx < x1 && sx < w; sx++, cnt++)
                    for (int c = 0; c < 3; c++) sum[c] += px[((size_t)sy * w + sx) * 3 + c];
            for (int c = 0; c < 3; c++) o[((size_t)y * nw + x) * 3 + c] = sum[c] / (float)(cnt ? cnt : 1);
        }
    *ow = nw, *oh = nh;
    return o;
}

// (Q23) The mesh's vertices its triangles use (each counted once): not the span of its indices, which counted the
// vertices of the meshes packed between them and the ones no triangle uses. A bit per vertex of the span.
uint32_t asset_mesh_used_verts(const uint32_t* idx, uint32_t first, uint32_t count) {
    if (!count) return 0;
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = idx[first + i];
        lo = v < lo ? v : lo, hi = v > hi ? v : hi;
    }
    size_t words = ((size_t)(hi - lo) >> 6) + 1;
    uint64_t* bits = calloc(words, 8);
    if (!bits) return hi - lo + 1;
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = idx[first + i] - lo;
        uint64_t b = 1ull << (v & 63);
        if (!(bits[v >> 6] & b)) bits[v >> 6] |= b, n++;
    }
    free(bits);
    return n;
}

bool asset_viewer_handles(const char* vpath) {
    const char* dot = strrchr(vpath, '.');
    if (!dot) return false;
    static const char* exts[] = { ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".tif", ".tiff", ".gif", ".hdr", ".exr", ".dds", ".rudim", ".rmesh" }; // (Q23: .rudim, its first tile)
    for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++)
        if (!_stricmp(dot, exts[i])) return true;
    return false;
}

void asset_viewer_open(const char* vpath) {
    if (!asset_viewer_handles(vpath)) return;
    char disk[600];
    if (!vpath_disk(vpath, disk, sizeof disk)) return;
    bool same = !strcmp(V.vpath, vpath) && V.kind == ASSET_TEXTURE_SOURCE; // (read again: the view, channels and mip stay)
    free(V.px), V.px = NULL;
    free(V.hdr), V.hdr = NULL;
    tex_release(&V.tex);
    if (!same) memset(V.ch, 1, sizeof V.ch), V.mip = 0;
    V.error[0] = 0;
    { // (Q23) its date and size: read again when they change
        bool dir = false;
        uint64_t sz = 0;
        int64_t mt = 0;
        V.mtime = 0, V.fsize = 0;
        if (re_file_stat(disk, &sz, &mt, &dir)) V.mtime = mt, V.fsize = sz;
    }
    snprintf(V.vpath, sizeof V.vpath, "%s", vpath);
    const char* b = strrchr(vpath, '/');
    snprintf(V.name, sizeof V.name, "%s", b ? b + 1 : vpath);
    const char* dot = strrchr(vpath, '.');
    if (!_stricmp(dot, ".rmesh")) {
        V.kind = ASSET_STATIC_MESH;
        V.mesh = render_mesh_id_of(vpath);
        render_mesh_bounds(V.mesh, V.mn, V.mx);
        const GpuVertex* v;
        const uint32_t* idx;
        uint32_t first, count;
        int32_t off;
        render_mesh_cpu(V.mesh, &v, &idx, &first, &count, &off);
        V.tris = count / 3;
        V.verts = asset_mesh_used_verts(idx, first, count); // (Q23) the vertices used, each once
        memset(&V.cam, 0, sizeof V.cam);
        bp_preview_frame_bounds(&V.cam, V.mn, V.mx);
        V.grid = true;
        V.vp.cam.framed = false;
        V.material = content_mesh_material(vpath); // (its import's material; else one named like it)
        if (!V.material) V.material = guess_material(V.name);
        return;
    }
    uint32_t w = 0, h = 0, sw = 0, sh = 0;
    uint8_t* px = NULL;
    if (!_stricmp(dot, ".hdr")) { // (Q23) its floats kept (at the shown size): the exposure slider
        size_t n = 0;
        uint8_t* data = NULL;
        FILE* hf = re_fopen(disk, "rb");
        if (hf) {
            _fseeki64(hf, 0, SEEK_END), n = (size_t)_ftelli64(hf), _fseeki64(hf, 0, SEEK_SET);
            data = malloc(n ? n : 1);
            if (data && fread(data, 1, n, hf) != n) free(data), data = NULL;
            fclose(hf);
        }
        float* rgb = data ? mat_hdr_decode(data, n, &w, &h) : NULL;
        free(data);
        if (rgb) {
            V.hdr = w > VIEW_MAX_SIDE || h > VIEW_MAX_SIDE ? hdr_shrink(rgb, w, h, VIEW_MAX_SIDE, &sw, &sh) : rgb;
            if (V.hdr != rgb) free(rgb);
            else sw = w, sh = h;
            if (!same) V.exposure = 0.0f;
            px = V.hdr ? mat_hdr_rgba8(V.hdr, sw, sh, V.exposure) : NULL;
            if (!px) free(V.hdr), V.hdr = NULL;
        }
    } else
        px = mat_load_image_rgba(disk, &w, &h);
    V.kind = ASSET_TEXTURE_SOURCE;
    if (!px) { // (Q23) said in the window (before: no window at all, as if the double click had missed)
        snprintf(V.error, sizeof V.error, "%s",
                 !_stricmp(dot, ".exr") ? TR("OpenEXR isn't read yet: save it as .hdr (Radiance) or PNG/TGA.")
                                        : TR("Couldn't read this image (damaged, or a format Windows can't decode)."));
        V.w = V.h = V.src_w = V.src_h = 0, V.mips = 0;
        return;
    }
    V.src_w = w, V.src_h = h;
    V.alpha = false;
    for (size_t i = 0; i < (size_t)w * h && !V.alpha; i++) V.alpha = px[i * 4 + 3] != 255;
    if (V.hdr) V.px = px, V.w = sw, V.h = sh; // (already at the shown size)
    else if (w > VIEW_MAX_SIDE || h > VIEW_MAX_SIDE) {
        V.px = shrink(px, w, h, VIEW_MAX_SIDE, &V.w, &V.h);
        free(px);
    } else
        V.px = px, V.w = w, V.h = h;
    V.mips = 1; // (Q23) the full chain down to 1 x 1, as the GPU's
    for (uint32_t m = w > h ? w : h; m > 1; m >>= 1) V.mips++;
    if (V.mip >= V.mips) V.mip = V.mips - 1;
    V.bytes = 0;
    FILE* f = re_fopen(disk, "rb");
    if (f) _fseeki64(f, 0, SEEK_END), V.bytes = _ftelli64(f), fclose(f);
    if (!same) V.zoom = 0.0f, V.pan[0] = V.pan[1] = 0.0f; // (0: fit, set when drawn)
    view_upload();
}

// (2026-11-23, the user: "opened, a texture set as RGB, non-color, linear; its quality") Texture Settings: its color
// space (sRGB: a color; Linear: data - roughness, metallic, masks; Normal Map), the largest size the materials read it
// at, its compression, filter and wrap. Saved beside it at once; Apply to Materials sets every Image Texture using it
typedef struct ApplyWalk { const char* vpath; const TexSettings* s; char dir[1100]; int mats, nodes; } ApplyWalk;
static void apply_walk(void* user, const ReDirEntry* e) {
    ApplyWalk* w = user;
    if (e->name[0] == '.') return;
    char full[1200];
    snprintf(full, sizeof full, "%s/%s", w->dir, e->name);
    if (e->is_dir) {
        ApplyWalk sub = *w;
        snprintf(sub.dir, sizeof sub.dir, "%s", full);
        re_dir_walk(full, apply_walk, &sub);
        w->mats = sub.mats, w->nodes = sub.nodes;
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (!dot || _stricmp(dot, ".rmat")) return;
    const char* root = mat_root();
    size_t rl = strlen(root);
    char vp[1200];
    snprintf(vp, sizeof vp, "/Content%s", full + rl);
    Material* m = mat_load(vp);
    if (!m || !m->graph) return;
    int n = 0;
    for (int k = 0; k < m->graph->node_count; k++)
        if (m->graph->nodes[k].type == NODE_TEX_IMAGE && !_stricmp(m->graph->nodes[k].image, w->vpath)) {
            MatNode* nd = &m->graph->nodes[k];
            nd->prop[0] = w->s->filter, nd->prop[1] = w->s->wrap, nd->prop[2] = w->s->color == TEXC_SRGB ? 0 : 1;
            n++;
        }
    if (n) mat_mark_dirty(m), mat_save(m), w->mats++, w->nodes += n;
}
int tex_settings_apply(const char* vpath) {
    TexSettings s;
    tex_settings_get(vpath, &s);
    ApplyWalk w = { vpath, &s, "", 0, 0 };
    snprintf(w.dir, sizeof w.dir, "%s", mat_root());
    re_dir_walk(w.dir, apply_walk, &w);
    return w.mats;
}
static void tex_settings_panel(const char* vpath, uint32_t sw, uint32_t sh) {
    if (!vpath || !vpath[0]) return;
    static char shown[600];
    static TexSettings s;
    static char note[160];
    if (strcmp(shown, vpath)) snprintf(shown, sizeof shown, "%s", vpath), tex_settings_get(vpath, &s), note[0] = 0;
    igSeparatorText(TR("Texture Settings"));
    bool ch = false;
    // the fields as wide as the column less its longest label (the labels on their right, whole in every language)
    static const char* const LB[5] = { "Color Space", "Max Size", "Compression", "Filter", "Wrap" };
    float lw = 0.0f;
    for (int k = 0; k < 5; k++) {
        ImVec2_c ts = igCalcTextSize(TR(LB[k]), NULL, false, -1.0f);
        if (ts.x > lw) lw = ts.x;
    }
    float fw = -(lw + igGetStyle()->ItemInnerSpacing.x + 4.0f);
    igPushTextWrapPos(0.0f);
    static const char* const CS[TEXC_COUNT] = { "sRGB (Color)", "Linear (Non-Color)", "Normal Map" };
    int c = s.color;
    igSetNextItemWidth(fw);
    if (igCombo_Str_arr(TR("Color Space"), &c, CS, TEXC_COUNT, -1)) s.color = (uint8_t)c, ch = true;
    if (!s.saved) igTextDisabled("%s", TR("Set by its name (save a change to keep yours)."));
    static const int SIZES[] = { 0, 256, 512, 1024, 2048, 4096, 8192 };
    char cur[32];
    if (s.max_size) snprintf(cur, sizeof cur, "%d", s.max_size);
    else snprintf(cur, sizeof cur, "%s", TR("As it is"));
    igSetNextItemWidth(fw);
    if (igBeginCombo(TR("Max Size"), cur, 0)) {
        for (size_t k = 0; k < sizeof SIZES / sizeof SIZES[0]; k++) {
            char lb[32];
            if (SIZES[k]) snprintf(lb, sizeof lb, "%d", SIZES[k]);
            else snprintf(lb, sizeof lb, "%s", TR("As it is"));
            if (igSelectable_Bool(lb, s.max_size == SIZES[k], 0, (ImVec2){ 0, 0 })) s.max_size = (uint16_t)SIZES[k], ch = true; // tips: none (a size in the Max Size list)
        }
        igEndCombo();
    }
    static const char* const CZ[TEXZ_COUNT] = { "BC7", "Uncompressed" };
    int z = s.compression;
    igSetNextItemWidth(fw);
    if (igCombo_Str_arr(TR("Compression"), &z, CZ, TEXZ_COUNT, -1)) s.compression = (uint8_t)z, ch = true;
    static const char* const FI[2] = { "Linear", "Closest" };
    int fi = s.filter;
    igSetNextItemWidth(fw);
    if (igCombo_Str_arr(TR("Filter"), &fi, FI, 2, -1)) s.filter = (uint8_t)fi, ch = true;
    static const char* const WR[4] = { "Repeat", "Extend", "Clip", "Mirror" };
    int wr = s.wrap;
    igSetNextItemWidth(fw);
    if (igCombo_Str_arr(TR("Wrap"), &wr, WR, 4, -1)) s.wrap = (uint8_t)wr, ch = true;
    // its memory as the materials read it (the mips a third more)
    uint32_t w = sw, h = sh;
    while (s.max_size && (w > s.max_size || h > s.max_size) && w > 1 && h > 1) w /= 2, h /= 2;
    double mb = (double)w * h * (s.compression == TEXZ_NONE ? 4.0 : 1.0) * 1.333 / (1024.0 * 1024.0);
    igTextDisabled(TR("Read by the materials at %u x %u: %.1f MB"), w, h, mb);
    if (ch) s.saved = tex_settings_save(vpath, &s), note[0] = 0;
    if (igButton(TR("Apply to Materials"), (ImVec2){ 0, 0 })) {
        int n = tex_settings_apply(vpath);
        snprintf(note, sizeof note, TR("%d material(s) updated."), n);
    }
    if (note[0]) igTextDisabled("%s", note);
    igPopTextWrapPos();
}

static void draw_texture(void) {
    if (!V.px) { // (Q23) it couldn't be read: why, in its window
        igTextWrapped("%s", V.name);
        igTextWrapped("%s", V.error);
        return;
    }
    ImVec2 avail = igGetContentRegionAvail();
    float side = igGetFontSize() * 15.0f, vw = avail.x - side - igGetStyle()->ItemSpacing.x, vh = avail.y;
    if (vw < 64 || vh < 64) return;
    ImVec2 o = igGetCursorScreenPos();
    igInvisibleButton("##tex_view", (ImVec2){ vw, vh }, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | // tips: none (a canvas or a tile drawn with its own name: no description over the work)
                                                           ImGuiButtonFlags_MouseButtonMiddle);
    bool hov = igIsItemHovered(0);
    ImGuiIO* io = igGetIO_Nil();
    float fit = fminf(vw / V.w, vh / V.h);
    if (V.zoom <= 0.0f) V.zoom = fminf(fit, 1.0f);
    if (hov && io->MouseWheel != 0.0f) { // (about the cursor)
        float nz = fminf(fmaxf(V.zoom * powf(1.15f, io->MouseWheel), 0.02f), 64.0f);
        float mx = io->MousePos.x - (o.x + vw * 0.5f + V.pan[0]), my = io->MousePos.y - (o.y + vh * 0.5f + V.pan[1]);
        V.pan[0] -= mx * (nz / V.zoom - 1.0f), V.pan[1] -= my * (nz / V.zoom - 1.0f);
        V.zoom = nz;
    }
    if (igIsItemActive() && (igIsMouseDragging(ImGuiMouseButton_Left, 0) || igIsMouseDragging(ImGuiMouseButton_Right, 0) ||
                             igIsMouseDragging(ImGuiMouseButton_Middle, 0)))
        V.pan[0] += io->MouseDelta.x, V.pan[1] += io->MouseDelta.y;
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_PushClipRect(dl, o, (ImVec2){ o.x + vw, o.y + vh }, true);
    ImDrawList_AddRectFilled(dl, o, (ImVec2){ o.x + vw, o.y + vh }, TH(TK_N3), 0, 0);
    float w = V.w * V.zoom, h = V.h * V.zoom, x0 = o.x + vw * 0.5f + V.pan[0] - w * 0.5f, y0 = o.y + vh * 0.5f + V.pan[1] - h * 0.5f;
    float cs = 16.0f; // the checker (alpha) under it
    for (float y = fmaxf(y0, o.y); y < fminf(y0 + h, o.y + vh); y += cs)
        for (float x = fmaxf(x0, o.x); x < fminf(x0 + w, o.x + vw); x += cs) {
            int ix = (int)floorf((x - x0) / cs), iy = (int)floorf((y - y0) / cs);
            ImDrawList_AddRectFilled(dl, (ImVec2){ x, y }, (ImVec2){ fminf(x + cs, x0 + w), fminf(y + cs, y0 + h) }, (ix + iy) & 1 ? TH(TK_N8) : TH(TK_N7), 0, 0);
        }
    if (V.tex.set) ImDrawList_AddImage(dl, tex_ref(&V.tex), (ImVec2){ x0, y0 }, (ImVec2){ x0 + w, y0 + h }, (ImVec2){ 0, 0 }, (ImVec2){ 1, 1 }, THEME_IMAGE_TINT);
    ImDrawList_AddRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x0 + w, y0 + h }, THA(TK_N15, 96), 0.0f, 1.0f, 0);
    // the pixel under the cursor
    if (hov) {
        int px = (int)floorf((io->MousePos.x - x0) / V.zoom), py = (int)floorf((io->MousePos.y - y0) / V.zoom);
        if (px >= 0 && py >= 0 && (uint32_t)px < V.w && (uint32_t)py < V.h) {
            const uint8_t* p = &V.px[((size_t)py * V.w + px) * 4];
            char tip[96];
            snprintf(tip, sizeof tip, "%d, %d   R %d  G %d  B %d  A %d", px, py, p[0], p[1], p[2], p[3]);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 8, o.y + vh - igGetTextLineHeight() - 6 }, TH(TK_N14), tip, NULL);
        }
    }
    ImDrawList_PopClipRect(dl);
    igSameLine(0, -1);
    igBeginGroup();
    igText("%s", V.name);
    igTextDisabled("%u x %u%s", V.src_w, V.src_h, V.w != V.src_w ? TR(" (shown smaller)") : "");
    igTextDisabled(TR("%.1f KB on disk"), (double)V.bytes / 1024.0);
    igTextDisabled("%s", V.alpha ? TR("Has alpha") : TR("No alpha"));
    igSeparator();
    igText("%s", TR("Channels"));
    static const char* names[4] = { "R", "G", "B", "A" };
    bool changed = false;
    for (int c = 0; c < 4; c++) {
        if (c) igSameLine(0, -1);
        changed |= igCheckbox(names[c], &V.ch[c]);
    }
    // (Q23) the mips (as the Texture Editor's Mip Level: how it looks far away, its size)
    if (V.mips > 1) {
        igSetNextItemWidth(-1);
        uint32_t ms_w = V.src_w >> V.mip, ms_h = V.src_h >> V.mip;
        char mf[64];
        snprintf(mf, sizeof mf, "%%d (%u x %u)", ms_w ? ms_w : 1, ms_h ? ms_h : 1);
        changed |= igSliderInt(TR("Mip Level"), &V.mip, 0, V.mips - 1, mf, 0);
    }
    // (Q23) a .hdr: its exposure (stops), as the Texture Editor's for HDR images
    if (V.hdr) {
        igSetNextItemWidth(-1);
        if (igSliderFloat(TR("HDR Exposure"), &V.exposure, -8.0f, 8.0f, "%+.1f EV", 0)) {
            uint8_t* px = mat_hdr_rgba8(V.hdr, V.w, V.h, V.exposure);
            if (px) free(V.px), V.px = px, changed = true;
        }
        igTextDisabled("%s", TR("HDR (Radiance): a texture reads it at 0 EV."));
    }
    if (changed) view_upload();
    tex_settings_panel(V.vpath, V.src_w, V.src_h);
    igSeparator();
    igText(TR("Zoom: %.0f%%"), V.zoom * 100.0f);
    if (igButton(TR("Fit"), (ImVec2){ 0, 0 })) V.zoom = fit, V.pan[0] = V.pan[1] = 0;
    igSameLine(0, -1);
    if (igButton("1:1", (ImVec2){ 0, 0 })) V.zoom = 1.0f, V.pan[0] = V.pan[1] = 0;
    igTextDisabled("%s", TR("Wheel: zoom | drag: pan"));
    igEndGroup();
}

// The preview's material: the one named like the mesh's slot (the importers write SM_<object>_<material>), or picked
static char g_found[256][512];
static int g_nfound = -1;
static uint32_t material_hash(const char* vpath) {
    char disk[600];
    Material* m = vpath_disk(vpath, disk, sizeof disk) ? mat_load(disk) : NULL;
    return m ? m->hash : 0u;
}
static uint32_t guess_material(const char* mesh_name) {
    char stem[160];
    snprintf(stem, sizeof stem, "%s", mesh_name);
    char* dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    g_nfound = content_search(".rmat", g_found, 256);
    size_t sl = strlen(stem), best = 0;
    int pick = -1;
    for (int i = 0; i < g_nfound; i++) {
        const char* b = strrchr(g_found[i], '/');
        char ms[160];
        snprintf(ms, sizeof ms, "%s", b ? b + 1 : g_found[i]);
        char* d = strrchr(ms, '.');
        if (d) *d = 0;
        size_t ml = strlen(ms);
        if (ml && ml <= sl && !_stricmp(stem + sl - ml, ms) && (ml == sl || stem[sl - ml - 1] == '_') && ml > best) best = ml, pick = i;
    }
    return pick >= 0 ? material_hash(g_found[pick]) : 0u;
}
// (2026-11-23, the user: "change a mesh's material in the viewer and save it, always used") the mesh's material saved
// (.<mesh>.mat: what a drag into the level puts on it) and the level's actors that had its old one told (editor.c)
static void (*g_mesh_mat_hook)(const char* mesh_vpath, uint32_t old_mat, uint32_t new_mat);
void asset_viewer_on_mesh_material(void (*fn)(const char* mesh_vpath, uint32_t old_mat, uint32_t new_mat)) { g_mesh_mat_hook = fn; }

// (2026-11-23, "add sockets") the Static Mesh's sockets: a skeleton of one bone (the mesh's space) holding them, so the
// Skeletal Mesh's socket panel and the viewport's gizmo edit them; saved beside the mesh (anim_meshsock.inc)
static SkelMesh* g_sock_skel;
static char g_sock_for[600];
static SkelMesh* mesh_socket_skel(void) {
    if (!g_sock_skel) g_sock_skel = calloc(1, sizeof(SkelMesh));
    if (!g_sock_skel) return NULL;
    if (strcmp(g_sock_for, V.vpath)) {
        snprintf(g_sock_for, sizeof g_sock_for, "%s", V.vpath);
        SkelMesh* m = g_sock_skel;
        m->nbones = 1, m->nparts = 0;
        snprintf(m->bones[0].name, ANIM_NAME, "%s", "root");
        m->bones[0].parent = -1;
        memset(&m->bones[0].bind, 0, sizeof m->bones[0].bind);
        m->bones[0].bind.q[3] = 1.0f, m->bones[0].bind.s[0] = m->bones[0].bind.s[1] = m->bones[0].bind.s[2] = 1.0f;
        memset(m->bind_world[0], 0, sizeof m->bind_world[0]);
        m->bind_world[0][0][0] = m->bind_world[0][1][1] = m->bind_world[0][2][2] = 1.0f;
        m->nsockets = anim_mesh_sockets(V.vpath, true, m->sockets, ANIM_MAX_SOCKETS);
        for (uint32_t i = 0; i < m->nsockets; i++) m->sockets[i].bone = 0;
        m->dirty = false;
        V.vp.socket = -1;
    }
    return g_sock_skel;
}

static void material_combo(void) {
    Material* cur = V.material ? mat_find_hash(V.material) : NULL;
    char now[160] = "";
    if (cur && mat_vpath(cur, now, sizeof now)) {
        const char* b = strrchr(now, '/');
        if (b) memmove(now, b + 1, strlen(b));
    }
    igTextDisabled("%s", TR("Material"));
    igSetNextItemWidth(-1);
    if (igBeginCombo("##sm_mat", now[0] ? now : TR("(none: gray)"), 0)) {
        if (g_nfound < 0) g_nfound = content_search(".rmat", g_found, 256);
        if (igSelectable_Bool(TR("(none: gray)"), !V.material, 0, (ImVec2){ 0, 0 })) V.material = 0;
        for (int i = 0; i < g_nfound; i++) {
            const char* b = strrchr(g_found[i], '/');
            igPushID_Int(i);
            if (asset_selectable_material(b ? b + 1 : g_found[i], false, g_found[i])) V.material = material_hash(g_found[i]);
            igPopID();
        }
        igEndCombo();
    }
    uint32_t saved = content_mesh_material(V.vpath);
    igPushTextWrapPos(0.0f);
    if (V.material != saved) { // (picked another: kept for good with a click)
        if (igButton(TR("Save as the Mesh's Material"), (ImVec2){ -1, 0 })) {
            char disk[1100];
            const char* root = mat_root();
            snprintf(disk, sizeof disk, "%s%s", root, V.vpath + 8);
            content_mesh_material_set_disk(disk, V.material);
            if (g_mesh_mat_hook) g_mesh_mat_hook(V.vpath, saved, V.material);
        }
        igTextDisabled("%s", TR("Not saved: only this preview shows it."));
    } else if (saved)
        igTextDisabled("%s", TR("The mesh's material: placed in a level it comes with it."));
    igPopTextWrapPos();
}

static void draw_mesh(void) {
    ImVec2 avail = igGetContentRegionAvail();
    float side = igGetFontSize() * 15.0f, vw = avail.x - side - igGetStyle()->ItemSpacing.x, vh = avail.y;
    if (vw < 64 || vh < 64) return;
    ImVec2 o = igGetCursorScreenPos();
    // (Q7) the asset viewport: the view modes (UV with its layout, LOD by the distance or forced), the light, the camera
    static const float grey[3] = { 0.72f, 0.72f, 0.72f };
    PreviewDraw d;
    avp_mesh_draw(&d, V.mesh, V.material, grey, 0.45f, 0.0f);
    AvpScene sc;
    memset(&sc, 0, sizeof sc);
    sc.draws = &d, sc.ndraws = 1, sc.sel_bone = sc.sel_body = sc.sel_constraint = -1;
    sc.mn = V.mn, sc.mx = V.mx;
    static const float id_rows[1][3][4] = { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } } };
    SkelMesh* sk = mesh_socket_skel(); // (2026-11-23) its sockets: drawn, picked, moved by the gizmo
    if (sk && strncmp(V.vpath, "#mesh/", 6)) sc.skel = sk, sc.edit_sockets = sk, sc.bones = id_rows;
    V.vp.grid = V.grid;
    AvpResult res;
    memset(&res, 0, sizeof res);
    avp_draw(&V.vp, BP_PV_KEY_STATIC_MESH, o.x, o.y, vw, vh, &sc, &res);
    V.grid = V.vp.grid;
    if (res.socket >= 0) V.vp.socket = res.socket;
    igSetCursorScreenPos((ImVec2){ o.x + vw + igGetStyle()->ItemSpacing.x, o.y });
    igBeginGroup();
    igText("%s", V.name);
    igTextDisabled(TR("%u triangles, %u vertices"), V.tris, V.verts);
    igTextDisabled(TR("Size: %.0f x %.0f x %.0f cm"), V.mx[0] - V.mn[0], V.mx[1] - V.mn[1], V.mx[2] - V.mn[2]);
    igSeparator();
    igCheckbox(TR("Grid"), &V.grid);
    material_combo();
    uint32_t lt[4];
    float le[4];
    uint32_t nl = render_mesh_lods(V.mesh, lt, le);
    if (nl > 1) { // (Q7) its LODs (the viewport's LOD menu forces one)
        igSeparatorText(TR("LODs"));
        for (uint32_t l = 0; l < nl && l < 4; l++) igTextDisabled(TR("LOD %u: %u triangles, error %.2f cm"), l, lt[l], le[l]);
    }
    const PreviewStats* ps = render_preview_stats();
    if (ps->views) igTextDisabled(TR("GPU %.2f ms | CPU %.2f ms | %ux MSAA"), ps->gpu_ms, ps->last_cpu_ms, ps->msaa);
    if (igButton(TR("Frame (F)"), (ImVec2){ 0, 0 })) avp_frame(&V.vp, V.mn, V.mx);
    if (sk && sc.skel) { // (2026-11-23) the sockets: Add Socket, name, place, rotation, preview mesh, delete; saved at once
        avp_socket_panel(&V.vp, sk, -1);
        if (sk->dirty && !res.busy) anim_mesh_sockets_save(V.vpath, sk->sockets, sk->nsockets), sk->dirty = false;
    }
    igEndGroup();
}

AssetViewerState asset_viewer_state(void) {
    AssetViewerState s;
    memset(&s, 0, sizeof s);
    s.open = V.kind != 0, s.is_hdr = V.hdr != NULL;
    s.w = V.w, s.h = V.h, s.src_w = V.src_w, s.src_h = V.src_h, s.mips = V.mips;
    s.error = V.error;
    s.thumb_refreshes = (unsigned)re_atomic_load(&g_th_refreshes), s.reloads = g_av_reloads, s.watch_us = g_watch_us;
    return s;
}
int asset_thumb_state(const char* vpath) {
    uint32_t h = hash_str(vpath);
    for (int i = 0; g_th && i < g_nth; i++)
        if (g_th[i].hash == h) return re_atomic_load(&g_th[i].state);
    return -1;
}

void asset_viewer_draw(void) {
    g_raster_budget = 1;
    dead_collect(false);
    live_step();
    if (!V.kind) return;
    char title[300];
    snprintf(title, sizeof title, "%s - %s###asset_viewer", V.name, V.kind == ASSET_STATIC_MESH ? TR("Static Mesh") : TR("Texture"));
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 64, igGetFontSize() * 40 }, ImGuiCond_FirstUseEver);
    bool open = true;
    if (igBegin(title, &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        if (V.kind == ASSET_TEXTURE_SOURCE) draw_texture();
        else draw_mesh();
        if (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && igIsKeyPressed_Bool(ImGuiKey_Escape, false)) open = false;
    }
    igEnd();
    if (!open) {
        V.kind = 0;
        free(V.px), V.px = NULL;
        free(V.hdr), V.hdr = NULL;
        tex_release(&V.tex);
        V.vpath[0] = 0;
    } else if (V.kind == ASSET_TEXTURE_SOURCE && igGetTime() - g_view_checked > 0.5) { // (Q23) changed on disk: read again
        g_view_checked = igGetTime();
        char disk[600];
        uint64_t sz = 0;
        int64_t mt = 0;
        bool dir = false;
        if (vpath_disk(V.vpath, disk, sizeof disk) && re_file_stat(disk, &sz, &mt, &dir) && (mt != V.mtime || sz != V.fsize)) {
            char vp[600];
            snprintf(vp, sizeof vp, "%s", V.vpath);
            asset_viewer_open(vp);
            g_av_reloads++;
        }
    }
}

void asset_viewer_init(struct VkCtx* vk) {
    g_vk = vk;
    if (!g_th) g_th = calloc(THUMB_MAX, sizeof(Thumb));
    if (g_th && !g_worker) g_worker = re_thread_start(worker, NULL);
}

void asset_viewer_shutdown(struct VkCtx* vk) {
    re_atomic_store(&g_quit, 1);
    if (g_worker) re_thread_join(g_worker), g_worker = NULL;
    for (int i = 0; g_th && i < g_nth; i++) {
        free(g_th[i].px);
        if (g_th[i].tex.set && igGetCurrentContext()) ImGui_ImplVulkan_RemoveTexture(g_th[i].tex.set);
        gpu_image_destroy(vk, &g_th[i].tex.img);
    }
    free(g_th), g_th = NULL, g_nth = 0;
    for (int i = 0; i < LIVE_FRAMES; i++) tex_release(&L.tex[i]);
    L.vpath[0] = 0;
    dead_collect(true);
    if (V.tex.img.image) {
        if (V.tex.set && igGetCurrentContext()) ImGui_ImplVulkan_RemoveTexture(V.tex.set);
        gpu_image_destroy(vk, &V.tex.img);
    }
    free(V.px), V.px = NULL;
    for (int i = 0; i < 256; i++)
        if (g_mats[i].set && igGetCurrentContext()) ImGui_ImplVulkan_RemoveTexture(g_mats[i].set);
    memset(g_mats, 0, sizeof g_mats);
    V.kind = 0;
    g_vk = NULL;
}

// --- Rows with a thumbnail (sugestão #60: every list of meshes, animations, materials and textures shows what each is) ---
bool re_imgui_combo_filter_pass(const char* label); // (imgui_widgets.cpp: the open combo's search)

// A disk path under the project's Content folder as "/Content/..." (false: elsewhere)
static bool disk_vpath(const char* disk, char* out, size_t cap) {
    const char* root = content_root();
    size_t n = strlen(root);
    const char bs = 0x5C; // (a backslash)
    while (n && (root[n - 1] == '/' || root[n - 1] == bs)) n--;
    if (!n) return false;
    for (size_t i = 0; i < n; i++) {
        char a = disk[i], b = root[i];
        if (!a) return false;
        if (a == bs) a = '/';
        if (b == bs) b = '/';
        if (tolower((unsigned char)a) != tolower((unsigned char)b)) return false;
    }
    if (disk[n] != '/' && disk[n] != bs) return false;
    snprintf(out, cap, "/Content%s", disk + n);
    for (char* c = out; *c; c++)
        if (*c == bs) *c = '/';
    return true;
}

static UiIcon kind_icon(int kind) { return ui_asset_icon(kind); } // (Q5: every kind its own)

bool asset_selectable(const char* label, bool selected, const char* path, int kind) {
    if (!re_imgui_combo_filter_pass(label)) return false; // (the combo's search: not this one)
    char vp[600] = "";
    if (path && path[0]) {
        if (path[0] == '/' || path[0] == '#') snprintf(vp, sizeof vp, "%s", path);
        else disk_vpath(path, vp, sizeof vp);
    }
    float fs = igGetFontSize(), h = fs * 1.8f;
    ImVec2 p = igGetCursorScreenPos();
    igSetCursorScreenPos((ImVec2){ p.x + h + 6.0f, p.y });
    igPushStyleVar_Vec2(ImGuiStyleVar_SelectableTextAlign, (ImVec2){ 0.0f, 0.5f });
    bool click = igSelectable_Bool(label, selected, 0, (ImVec2){ 0.0f, h });
    igPopStyleVar(1);
    bool hov = igIsItemHovered(0), shown = igIsItemVisible();
    if (shown) { // the thumbnail on its dark tile (the kind's icon until it is ready)
        ImDrawList* dl = igGetWindowDrawList();
        ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + h, p.y + h }, TH(TK_N4), 3.0f, 0);
        if (!vp[0] || !asset_thumb(dl, p.x + 1, p.y + 1, p.x + h - 1, p.y + h - 1, vp, kind))
            ui_icon(dl, kind_icon(kind), p.x + h * 0.2f, p.y + h * 0.2f, h * 0.6f, TH(TK_N13));
    }
    if (hov && vp[0]) { // the big preview beside the list; an animation plays (Unreal's asset tooltip)
        if (kind == ASSET_ANIMATION) asset_thumb_hover(vp);
        if (igBeginTooltip()) {
            float s = fs * 10.0f;
            ImVec2 q = igGetCursorScreenPos();
            igDummy((ImVec2){ s, s });
            ImDrawList* tl = igGetWindowDrawList();
            ImDrawList_AddRectFilled(tl, q, (ImVec2){ q.x + s, q.y + s }, TH(TK_N4), 4.0f, 0);
            if (!asset_thumb(tl, q.x + 2, q.y + 2, q.x + s - 2, q.y + s - 2, vp, kind))
                ui_icon(tl, kind_icon(kind), q.x + s * 0.3f, q.y + s * 0.3f, s * 0.4f, TH(TK_N11));
            igTextUnformatted(label, NULL);
            igEndTooltip();
        }
    }
    return click;
}

bool asset_selectable_mesh(const char* label, bool selected, unsigned int mesh_id) {
    char ref[32];
    snprintf(ref, sizeof ref, "#mesh/%x", mesh_id);
    return asset_selectable(label, selected, ref, ASSET_STATIC_MESH);
}

bool asset_selectable_material(const char* label, bool selected, const char* path) {
    const char* dot = path ? strrchr(path, '.') : NULL;
    return asset_selectable(label, selected, path, dot && !_stricmp(dot, ".rmi") ? ASSET_MATERIAL_INSTANCE : ASSET_MATERIAL);
}

void asset_viewer_close(void) {
    V.kind = 0;
    free(V.px), V.px = NULL;
    tex_release(&V.tex);
}
