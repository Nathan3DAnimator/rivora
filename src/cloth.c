#include "cloth.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "cloth_mesh.h"
#include "components.h"
#include "gi.h"
#include "vsm.h"
#include "ikrig.h"
#include "physics.h"
#include "platform.h"
#include "render.h"
#include "softbody.h"
#include "vkctx.h"

static const uint32_t s_cloth_sim[] =
#include "cloth_sim.comp.inc"
    ;
static const uint32_t s_cloth_soft[] = // (Q9) the soft bodies' block pass: cloth_sim.comp with SOFT_BLOCK
#include "cloth_soft.comp.inc"
    ;
#define CLF_BLOCK 256u
#define SB_MAX 2048u // (cloth_sim.comp: a lattice up to this many vertices in shared memory)

#define MAX_CLOTH 64
#define MAX_VERTS (1u << 18)   // 262144: 64 cloths of 64 x 64
#define MAX_COLLIDERS 1024
#define PER_CLOTH_COLLIDERS 96
#define SELF_BUCKETS 65536u    // the self collision's hash
#define MAX_COLORS 32u         // the solver's colors (a grid's 9; a mesh's from its graph)
#define MAX_ADJ (1u << 20)     // the meshes' adjacency entries (8 MB)
#define MAX_TRIS (2u * MAX_VERTS) // the meshes' triangles
#define CLF_MESH 64u
#define CLF_SOFT 128u
#define MAX_EMBED (1u << 19) // (Q9) the soft bodies' mesh vertices (their lattice places), each half of the buffer
#define COL_FLOATS 12        // a collider: 3 vec4 (cloth_sim.comp collide)
#define FB_INTS (MAX_COLLIDERS * 8 + MAX_CLOTH * 8) // the feedback: 8 ints a collider, 8 a cloth
#define FB_SCALE 1024.0f

// (shaders/cloth.glsl ClothInfo, scalar)
typedef struct ClothGpu {
    float rows[3][4];
    uint32_t base, nx, ny, flags;
    float dx, dy, mass, inv_h;
    float k_stretch, k_shear, k_bend, damping;
    float friction, thickness, tear, gravity;
    float wind[4];
    uint32_t col_first, col_count, color, iter;
    float tint[4];
    float look[4];
    float soft[4]; // (Q9) a soft body's: its lattice's y count, asleep, let go, the plasticity's yield (m)
} ClothGpu;
_Static_assert(sizeof(ClothGpu) == 192, "ClothInfo layout");
typedef struct ClothHeaderGpu {
    VkDeviceAddress info, pos, prev, vel, target, owner, mask, colliders;
    uint32_t ncloth, nverts, ncolliders, color;
    float time[4];
    float cut[4];
    VkDeviceAddress order;
    uint32_t cfirst[MAX_COLORS], ccount[MAX_COLORS];
    VkDeviceAddress idx, head, next, selfnb;
    float cell;
    uint32_t nbuckets;
    VkDeviceAddress mrest, muv, madj_off, madj, mtris, mnormal; // the meshes' (cloth_mesh.h)
    float kick[4], kick_vel[4];   // (Q9) Add Soft Body Impulse
    VkDeviceAddress feedback;     // (Q9) read back by the CPU: the colliders' forces, the soft bodies' means
    uint32_t fb_cloth, pad_h;
} ClothHeaderGpu;
_Static_assert(sizeof(ClothHeaderGpu) == 416 + 48 + 48, "ClothHeader layout");

#define CLF_RESET 32u

static struct {
    VkPipeline pipe, pipe_block;
    VkPipelineLayout layout;
    // what the CPU writes every frame: one set per frame in flight (the GPU may still run the last frame's while this one's
    // is written: a single shared set raced it, a cloth's nx read as 0 mid-write, a read out of bounds, the device lost)
    GpuBuf header[RE_FRAMES_IN_FLIGHT], info[RE_FRAMES_IN_FLIGHT], colliders[RE_FRAMES_IN_FLIGHT], owner[RE_FRAMES_IN_FLIGHT], order[RE_FRAMES_IN_FLIGHT];
    GpuBuf pos, prev, vel, target, mask; // (the GPU's own: only the GPU writes them, in order)
    GpuBuf head, next, selfnb;           // self collision (the GPU's own)
    // the meshes' graphs (cloth_mesh.h): written on a layout change only, the GPU idle (rest, uv, adjacency, triangles);
    // their normals the GPU's own (pass 9)
    GpuBuf mrest, muv, madj_off, madj, mtris, mnormal;
    ClothMeshTopo topo[MAX_CLOTH];
    uint32_t ntri[MAX_CLOTH], key1[MAX_CLOTH]; // a mesh's triangles; the BLAS's size key (a grid's ny, a mesh's triangles | high bit)
    uint32_t ncolors;
    bool any_mesh; // (pass 9 only then)
    float self_cell;                     // m (0: no cloth collides with itself)
    uint32_t slot;                       // this frame's set
    ClothGpu cinfo[MAX_CLOTH];           // the cloths built here, copied whole into the frame's set
    uint32_t* order_cpu;                 // the vertices by color, built on a layout change ...
    uint32_t order_n, order_ver, order_ver_of[RE_FRAMES_IN_FLIGHT]; // ... and copied into each set that hasn't it yet
    GpuBuf idx;               // its triangles for the BLAS (a quad a 2 x 2 block of the grid; torn: degenerate)
    VkCtx* vk;
    GiDynBlas blas[MAX_CLOTH];
    uint32_t blas_size[MAX_CLOTH][2];
    bool shadow[MAX_CLOTH];   // casts one
    bool tris_ready[MAX_CLOTH]; // its triangles written (pass 5) for its current size: its BLAS can be built
    bool tears[MAX_CLOTH];      // it can tear by itself (its Tear Strain): its triangles may change any frame
    bool rebuild[MAX_CLOTH];    // its triangles moved in the buffers (a layout change): a full build next
    uint32_t cut_frames;        // a Blueprint cut: the next frames' BLAS built in full (its triangles changed)
    float box[MAX_CLOTH][2][3], box_prev[MAX_CLOTH][2][3]; // world (cm): where it can be (its rest place and its reach)
    uint32_t cfirst[MAX_COLORS], ccount[MAX_COLORS];
    uint32_t ncloth, nverts, ncol;
    Entity ents[MAX_CLOTH];
    uint32_t sizes[MAX_CLOTH][2];
    float time;
    uint32_t substeps, iterations;
    bool any, paused;
    float cut[4];
    bool cut_pending;
    ClothStats stats;
    // (Q9) the soft bodies (softbody.h): their lattices, their meshes' places in them (sembed: two halves, a layout writes
    // the one no frame in flight reads), what the renderer's skin reads (views, from the last fill), what the GPU gave back
    SoftTopo stopo[MAX_CLOTH];
    GpuBuf sembed, feedback[RE_FRAMES_IN_FLIGHT];
    uint32_t embed_half;
    ClothSoftView views[MAX_CLOTH];
    uint32_t nviews, sim_version[MAX_CLOTH], nblock;
    bool soft[MAX_CLOTH], asleep[MAX_CLOTH], held[MAX_CLOTH];
    uint32_t still[MAX_CLOTH];   // frames its fastest vertex stayed under the sleep speed
    uint32_t col_hash[MAX_CLOTH], cfg_hash[MAX_CLOTH];
    float soft_loc[MAX_CLOTH][3]; // where a free one's actor was put (its mean): moved by anyone else, it starts over there
    float soft_scale[MAX_CLOTH][3];
    VkDeviceAddress soft_embed[MAX_CLOTH]; // its mesh's places, written at the last layout
    float max_speed[MAX_CLOTH];
    // what each frame in flight's feedback belongs to: the colliders' physics bodies and the soft bodies, its time
    Entity fb_col[RE_FRAMES_IN_FLIGHT][MAX_COLLIDERS];
    Entity fb_cloth[RE_FRAMES_IN_FLIGHT][MAX_CLOTH];
    float fb_origin[RE_FRAMES_IN_FLIGHT][MAX_CLOTH][3];
    float fb_dt[RE_FRAMES_IN_FLIGHT];
    uint32_t fb_ncol[RE_FRAMES_IN_FLIGHT], fb_nsoft[RE_FRAMES_IN_FLIGHT];
    bool fb_play[RE_FRAMES_IN_FLIGHT];
    bool editor, was_editor;
    float cam[3];
    float kick[8];
    Entity kick_e;
    bool kick_pending;
    SoftStats sstats;
} C;

const char* const g_cloth_preset_names[CLOTH_PRESET_COUNT] = { "Custom", "Silk", "Cotton", "Denim", "Leather", "Canvas", "Flag", "Cape", "Shirt", "Skirt" };
const char* const g_cloth_pin_names[CLOTH_PIN_COUNT] = { "Top Edge", "Top Corners", "Left Edge", "Four Corners", "None" };

// density g/m2, stretch, shear, bend (N/m), damping, friction, color, roughness, sheen, translucency
static const float k_presets[CLOTH_PRESET_COUNT][12] = {
    { 0 },
    { 80, 3500, 200, 0.4f, 0.01f, 0.2f, 0.75f, 0.68f, 0.55f, 0.45f, 0.8f, 0.6f },  // silk: light, flowing, shiny
    { 150, 6000, 600, 2.0f, 0.02f, 0.4f, 0.55f, 0.08f, 0.07f, 0.8f, 0.25f, 0.3f }, // cotton
    { 400, 12000, 2500, 12.0f, 0.04f, 0.6f, 0.08f, 0.14f, 0.3f, 0.9f, 0.15f, 0.05f }, // denim: heavy, stiff
    { 700, 20000, 6000, 40.0f, 0.06f, 0.7f, 0.2f, 0.09f, 0.04f, 0.55f, 0.1f, 0.0f }, // leather: heavy, holds its shape
    { 350, 15000, 3000, 8.0f, 0.03f, 0.6f, 0.45f, 0.4f, 0.28f, 0.9f, 0.1f, 0.1f },   // canvas
    { 120, 5000, 400, 1.0f, 0.01f, 0.3f, 0.7f, 0.05f, 0.05f, 0.7f, 0.3f, 0.5f },     // flag (with wind)
    { 260, 9000, 1200, 5.0f, 0.03f, 0.5f, 0.08f, 0.06f, 0.2f, 0.75f, 0.35f, 0.15f }, // cape
    { 140, 7000, 700, 1.5f, 0.03f, 0.55f, 0.62f, 0.64f, 0.68f, 0.85f, 0.3f, 0.25f }, // (Q10) shirt: cotton on the body, grips it
    { 120, 5000, 450, 0.8f, 0.02f, 0.45f, 0.35f, 0.12f, 0.18f, 0.8f, 0.35f, 0.3f },  // (Q10) skirt: light, swings from the hips
};

void cloth_apply_preset(CCloth* c, int preset) {
    if (preset <= CLOTH_CUSTOM || preset >= CLOTH_PRESET_COUNT) return;
    const float* p = k_presets[preset];
    c->preset = (uint32_t)preset;
    c->density = p[0], c->stretch = p[1], c->shear = p[2], c->bend = p[3], c->damping = p[4], c->friction = p[5];
    c->color[0] = p[6], c->color[1] = p[7], c->color[2] = p[8], c->roughness = p[9], c->sheen = p[10], c->translucency = p[11];
    if (preset == CLOTH_FLAG) c->wind[0] = 7.0f, c->gust = 0.6f, c->pin = CLOTH_PIN_LEFT;
    // (Q10) worn: thin against the body (its Physics Asset's bodies), two substeps of 4 iterations (a run's fast limbs swept), pinned at
    // its top band (the collar and shoulders, the waist) turning with the bone it follows
    if (preset == CLOTH_SHIRT || preset == CLOTH_SKIRT || preset == CLOTH_CAPE) {
        c->thickness = preset == CLOTH_CAPE ? 1.0f : 0.6f, c->substeps = 2, c->iterations = 4; // (2 x 4: the dispatches of 1 x 8, the limbs swept twice)
        if (preset != CLOTH_CAPE) c->follow_turn = 1, c->pin_band = c->pin_band > 0.0f ? c->pin_band : (preset == CLOTH_SHIRT ? 6.0f : 3.0f), c->self_collision = 1;
    }
}

void cloth_set_view(const float cam[3], bool editor) {
    memcpy(C.cam, cam, sizeof C.cam);
    C.editor = editor;
}
const SoftStats* soft_stats(void) { return &C.sstats; }

void cloth_soft_impulse(Entity e, const float center[3], float radius, const float vel[3]) {
    C.kick[0] = center[0] * 0.01f, C.kick[1] = center[1] * 0.01f, C.kick[2] = center[2] * 0.01f, C.kick[3] = fmaxf(radius, 1.0f) * 0.01f;
    C.kick[4] = vel[0] * 0.01f, C.kick[5] = vel[1] * 0.01f, C.kick[6] = vel[2] * 0.01f;
    C.kick_e = e, C.kick_pending = true;
}

bool cloth_soft_set_prop(World* w, Entity e, const char* name, float value) {
    CCloth* c = e ? ecs_get(w, e, COMP_CLOTH) : NULL;
    if (!c || c->kind != CLOTH_KIND_SOFT || !name) return false;
    char key[64];
    size_t k = 0;
    for (const char* p = name; *p && k + 1 < sizeof key; p++)
        if (*p != ' ' && *p != '(' && *p != ')') key[k++] = (char)(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
    key[k] = 0;
    static const struct { const char* key; size_t off; float mn, mx; } f[] = {
        { "stiffnessyoung", offsetof(CCloth, youngs), 0.2f, 5000.0f }, { "stiffness", offsetof(CCloth, youngs), 0.2f, 5000.0f },
        { "keepvolume", offsetof(CCloth, poisson), 0.0f, 0.49f },      { "softdensity", offsetof(CCloth, soft_density), 5.0f, 3000.0f },
        { "density", offsetof(CCloth, soft_density), 5.0f, 3000.0f },  { "softdamping", offsetof(CCloth, soft_damping), 0.0f, 1.0f },
        { "damping", offsetof(CCloth, soft_damping), 0.0f, 1.0f },     { "plasticity", offsetof(CCloth, plasticity), 0.0f, 1.0f },
        { "dentrecovery", offsetof(CCloth, recovery), 0.0f, 120.0f },  { "dentyield", offsetof(CCloth, yield), 0.0f, 1.0f },
        { "friction", offsetof(CCloth, friction), 0.0f, 1.0f },        { "thickness", offsetof(CCloth, thickness), 0.05f, 20.0f },
        { "gravityscale", offsetof(CCloth, gravity), -2.0f, 4.0f },    { "holdstiffness", offsetof(CCloth, attach_stiffness), 0.0f, 1e6f },
        { "holdfalloff", offsetof(CCloth, attach_falloff), 0.0f, 500.0f }, { "holdband", offsetof(CCloth, pin_band), 0.0f, 500.0f },
        { "pushphysicsbodies", offsetof(CCloth, rigid_push), 0.0f, 4.0f }, { "sleepdistance", offsetof(CCloth, sleep_distance), 0.0f, 10000.0f },
    };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++)
        if (!strcmp(key, f[i].key)) {
            *(float*)((char*)c + f[i].off) = fminf(fmaxf(value, f[i].mn), f[i].mx);
            c->soft_preset = SOFT_CUSTOM;
            return true;
        }
    if (!strcmp(key, "held")) return c->released = value != 0.0f ? 0u : 1u, true;
    if (!strcmp(key, "iterations")) return c->iterations = (uint32_t)fminf(fmaxf(value, 1.0f), 24.0f), true;
    if (!strcmp(key, "substeps")) return c->substeps = (uint32_t)fminf(fmaxf(value, 1.0f), 4.0f), true;
    if (!strcmp(key, "latticecells")) return c->soft_res = (uint32_t)fminf(fmaxf(value, 2.0f), (float)SOFT_MAX_RES), true;
    if (!strcmp(key, "sleepwhenstill")) return c->sleep = value != 0.0f, true;
    if (!strcmp(key, "collidewithothers")) return c->self_collision = value != 0.0f, true;
    return false;
}

bool cloth_soft_view(Entity e, ClothSoftView* out) {
    for (uint32_t i = 0; i < C.nviews; i++)
        if (C.views[i].e == e && C.views[i].nrender) {
            *out = C.views[i];
            return true;
        }
    return false;
}

void cloth_tear(const float center[3], float radius) {
    C.cut[0] = center[0] * 0.01f, C.cut[1] = center[1] * 0.01f, C.cut[2] = center[2] * 0.01f, C.cut[3] = radius * 0.01f;
    C.cut_pending = radius > 0.0f;
    if (C.cut_pending) C.cut_frames = 3;
}

const ClothStats* cloth_stats(void) { return &C.stats; }

bool cloth_init(VkCtx* vk, VkPipelineLayout layout) {
    memset(&C, 0, sizeof C);
    C.layout = layout;
    C.vk = vk;
    C.pipe = gpu_compute_pipeline(vk, layout, s_cloth_sim, sizeof s_cloth_sim);
    if (!C.pipe) return false;
    { // (Q9) the soft bodies' block pass needs clustered subgroup sums in compute (8 lanes a vertex); else they go by color
        VkPhysicalDeviceSubgroupProperties sg = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sg };
        vkGetPhysicalDeviceProperties2(vk->gpu, &p2);
        bool ok = (sg.supportedOperations & VK_SUBGROUP_FEATURE_CLUSTERED_BIT) && (sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) && sg.subgroupSize >= 8;
        C.pipe_block = ok ? gpu_compute_pipeline(vk, layout, s_cloth_soft, sizeof s_cloth_soft) : VK_NULL_HANDLE;
    }
    VkBufferUsageFlags u = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBufferUsageFlags as = vk->ray_query ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0;
    if (!gpu_buf_create(vk, &C.idx, 24u * MAX_VERTS, u | as, false)) return false;
    { // (zeros: every triangle empty until pass 5 writes a cloth's)
        VkCommandBuffer cmd = gpu_begin_once(vk);
        vkCmdFillBuffer(cmd, C.idx.buffer, 0, VK_WHOLE_SIZE, 0);
        gpu_end_once(vk, cmd);
    }
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) {
        if (!gpu_buf_create(vk, &C.header[f], sizeof(ClothHeaderGpu), u, true) || !gpu_buf_create(vk, &C.info[f], sizeof(ClothGpu) * MAX_CLOTH, u, true) ||
            !gpu_buf_create(vk, &C.colliders[f], sizeof(float) * COL_FLOATS * MAX_COLLIDERS, u, true) || !gpu_buf_create(vk, &C.owner[f], 4u * MAX_VERTS, u, true) ||
            !gpu_buf_create(vk, &C.order[f], 4u * MAX_VERTS, u, true) || !gpu_buf_create(vk, &C.feedback[f], 4u * FB_INTS, u, true))
            return false;
        memset(C.feedback[f].mapped, 0, 4u * FB_INTS);
    }
    if (!gpu_buf_create(vk, &C.sembed, 2u * 16u * MAX_EMBED, u, true)) return false;
    C.order_cpu = malloc(4u * MAX_VERTS);
    if (!C.order_cpu || !gpu_buf_create(vk, &C.pos, 16u * MAX_VERTS, u | as | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, getenv("REALENGINE_CLOTH_DEBUG") != NULL || getenv("REALENGINE_CLOTH_READBACK") != NULL) || !gpu_buf_create(vk, &C.prev, 16u * MAX_VERTS, u, false) ||
        !gpu_buf_create(vk, &C.vel, 16u * MAX_VERTS, u, false) || !gpu_buf_create(vk, &C.target, 16u * MAX_VERTS, u, false) ||
        !gpu_buf_create(vk, &C.mask, 4u * MAX_VERTS, u, false) || !gpu_buf_create(vk, &C.head, 4u * SELF_BUCKETS, u, false) ||
        !gpu_buf_create(vk, &C.next, 4u * MAX_VERTS, u, false) || !gpu_buf_create(vk, &C.selfnb, 16u * MAX_VERTS, u, false))
        return false;
    if (!gpu_buf_create(vk, &C.mrest, 16u * MAX_VERTS, u, true) || !gpu_buf_create(vk, &C.muv, 8u * MAX_VERTS, u, true) ||
        !gpu_buf_create(vk, &C.madj_off, 8u * MAX_VERTS, u, true) || !gpu_buf_create(vk, &C.madj, 8u * MAX_ADJ, u, true) ||
        !gpu_buf_create(vk, &C.mtris, 12u * MAX_TRIS, u, true) || !gpu_buf_create(vk, &C.mnormal, 16u * MAX_VERTS, u, false))
        return false;
    return true;
}

void cloth_shutdown(VkCtx* vk) {
    if (C.pipe) vkDestroyPipeline(vk->device, C.pipe, NULL);
    if (C.pipe_block) vkDestroyPipeline(vk->device, C.pipe_block, NULL);
    for (int i = 0; i < MAX_CLOTH; i++) gi_dyn_blas_free(vk, &C.blas[i]);
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) {
        GpuBuf* s[] = { &C.header[f], &C.info[f], &C.colliders[f], &C.owner[f], &C.order[f], &C.feedback[f] };
        for (int i = 0; i < 6; i++)
            if (s[i]->buffer) gpu_buf_destroy(vk, s[i]);
    }
    if (C.sembed.buffer) gpu_buf_destroy(vk, &C.sembed);
    for (int i = 0; i < MAX_CLOTH; i++) soft_topo_free(&C.stopo[i]);
    GpuBuf* b[] = { &C.pos, &C.prev, &C.vel, &C.target, &C.mask, &C.idx, &C.head, &C.next, &C.selfnb, &C.mrest, &C.muv, &C.madj_off, &C.madj, &C.mtris, &C.mnormal };
    for (int i = 0; i < 15; i++)
        if (b[i]->buffer) gpu_buf_destroy(vk, b[i]);
    for (int i = 0; i < MAX_CLOTH; i++) cloth_mesh_free(&C.topo[i]);
    free(C.order_cpu);
    memset(&C, 0, sizeof C);
}

// --- The colliders around a cloth (meters) ---
// (Q9: 3 vec4 a collider, the third an oriented box's turn; the physics body each one is, for its contact forces)
static Entity* s_col_ent; // (this frame's: fb_col of the slot)
static void add_col(float* out, uint32_t* n, int type, const float a[3], const float b[3], float radius) {
    if (*n >= MAX_COLLIDERS) return;
    if (s_col_ent) s_col_ent[*n] = ENTITY_NONE;
    float* c = out + COL_FLOATS * (*n)++;
    c[0] = a[0] * 0.01f, c[1] = a[1] * 0.01f, c[2] = a[2] * 0.01f, c[3] = (float)type;
    c[4] = b[0] * 0.01f, c[5] = b[1] * 0.01f, c[6] = b[2] * 0.01f, c[7] = radius * 0.01f;
    c[8] = c[9] = c[10] = 0.0f, c[11] = 1.0f;
}

// A turn's quaternion from its axes (rotator_axes: ax[k] the local axis k in the world)
static void axes_to_quat(const float ax[3][3], float q[4]) {
    float m00 = ax[0][0], m11 = ax[1][1], m22 = ax[2][2], tr = m00 + m11 + m22; // (m[r][c] = ax[c][r])
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s, q[0] = (ax[1][2] - ax[2][1]) / s, q[1] = (ax[2][0] - ax[0][2]) / s, q[2] = (ax[0][1] - ax[1][0]) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (ax[1][2] - ax[2][1]) / s, q[0] = 0.25f * s, q[1] = (ax[1][0] + ax[0][1]) / s, q[2] = (ax[2][0] + ax[0][2]) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (ax[2][0] - ax[0][2]) / s, q[0] = (ax[1][0] + ax[0][1]) / s, q[1] = 0.25f * s, q[2] = (ax[2][1] + ax[1][2]) / s;
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (ax[0][1] - ax[1][0]) / s, q[0] = (ax[2][0] + ax[0][2]) / s, q[1] = (ax[2][1] + ax[1][2]) / s, q[2] = 0.25f * s;
    }
}

static bool near(const float p[3], const float c[3], float r) {
    float d[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] };
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < r * r;
}

// A Skeletal Mesh part's capsule radius: its Physics Asset's body for the bone, else from the bone's length
static float part_radius(World* w, Entity owner, const char* bone, float len) {
    const CSkeletalMesh* sk = ecs_get(w, owner, COMP_SKELETAL_MESH);
    const PhysicsAsset* pa = sk && sk->physics_asset ? phys_find(sk->physics_asset) : NULL;
    for (uint32_t i = 0; pa && i < pa->nbodies; i++)
        if (!strcmp(pa->bodies[i].bone, bone)) return pa->bodies[i].shape == PHYS_BOX ? fmaxf(pa->bodies[i].extent[1], pa->bodies[i].extent[2]) : pa->bodies[i].radius;
    return fminf(fmaxf(len * 0.2f, 4.0f), 14.0f);
}

// (Q10) A capsule that moves (a body of a Physics Asset): its place now, then last frame's in the next slot (type 7),
// the shader sweeps it over the substeps and keeps a vertex on the side it was on (a fast arm never jumps over it)
static void add_moving(float* out, uint32_t* n, const PhysCapsule* c) {
    if (*n + 2 > MAX_COLLIDERS) return;
    add_col(out, n, 6, c->a, c->b, c->r); // (6 and 7: Q9's oriented box is 4)
    add_col(out, n, 7, c->pa, c->pb, c->r);
}
// (Q10) A Skeletal Mesh that publishes its Physics Asset's capsules (ikrig.c): its parts' rough capsules are not needed
static bool has_bodies(World* w, Entity mesh) {
    const CSkeletalMesh* sk = ecs_get(w, mesh, COMP_SKELETAL_MESH);
    return sk && sk->physics_asset && phys_find(sk->physics_asset);
}
static void gather_colliders(World* w, const float center[3], float reach, float* out, uint32_t* n, Entity self, Entity skip, Entity skip_mesh, const char* skip_bone) {
    if (!getenv("REALENGINE_CLOTH_NO_BODIES")) { // (Q10) the characters' Physics Asset bodies, skinned or not, as they moved this frame (not the body it hangs from; _NO_BODIES: as before, the measure's)
        PhysCapsule caps[96];
        uint32_t nc = phys_capsules_near(center, reach, caps, 96);
        for (uint32_t i = 0; i < nc && *n + 2 <= MAX_COLLIDERS; i++) {
            if (skip_mesh && caps[i].owner == skip_mesh && skip_bone && skip_bone[0]) {
                const CSkeletalMesh* sk = ecs_get(w, skip_mesh, COMP_SKELETAL_MESH);
                const PhysicsAsset* pa = sk ? phys_find(sk->physics_asset) : NULL;
                if (pa && caps[i].body >= 0 && caps[i].body < (int)pa->nbodies && !_stricmp(pa->bodies[caps[i].body].bone, skip_bone)) continue;
            }
            add_moving(out, n, &caps[i]);
        }
    }
    for (uint32_t ai = 0; ai < w->archetype_count && *n < MAX_COLLIDERS; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_TRANSFORM)) || (a->mask & (COMP_BIT(COMP_CLOTH) | COMP_BIT(COMP_WATER)))) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        const CAttach* at = (a->mask & COMP_BIT(COMP_ATTACH)) ? ecs_arch_column(a, COMP_ATTACH) : NULL;
        for (uint32_t r = 0; r < a->count && *n < MAX_COLLIDERS; r++) {
            if (ents[r] == self || ents[r] == skip || (tr[r].flags & TRANSFORM_HIDDEN_GAME)) continue; // (skip: the bone it hangs from)
            const float* p = tr[r].location;
            if (at && at[r].kind == ATTACH_SK_PART) { // a bone of a Skeletal Mesh: a capsule to its parent bone
                if (!near(p, center, reach)) continue;
                if (has_bodies(w, (Entity)at[r].owner) && !getenv("REALENGINE_CLOTH_NO_BODIES")) continue; // (Q10: its Physics Asset's bodies stand for it, above)
                Entity par = (Entity)at[r].parent;
                const CAttach* pat = par ? ecs_get(w, par, COMP_ATTACH) : NULL;
                const CTransform* pt = par ? ecs_get(w, par, COMP_TRANSFORM) : NULL;
                const CName* nm = ecs_get(w, ents[r], COMP_NAME);
                if (pat && pat->kind == ATTACH_SK_PART && pt) {
                    float d[3] = { p[0] - pt->location[0], p[1] - pt->location[1], p[2] - pt->location[2] };
                    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    add_col(out, n, 1, pt->location, p, part_radius(w, (Entity)at[r].owner, nm ? nm->text : "", len));
                } else if (a->mask & COMP_BIT(COMP_STATIC_MESH)) { // (a part hung from the actor, as anim.c makes them: a
                    // capsule along its mesh's longest side, as wide as the others; else the legs never held a skirt)
                    const CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
                    float lmn[3], lmx[3], rows[3][4];
                    render_mesh_bounds(sm[r].mesh, lmn, lmx);
                    render_transform_rows(&tr[r], rows);
                    float half[3], ctr[3], axl[3];
                    for (int k = 0; k < 3; k++) {
                        float cx = (lmn[k] + lmx[k]) * 0.5f;
                        axl[k] = sqrtf(rows[0][k] * rows[0][k] + rows[1][k] * rows[1][k] + rows[2][k] * rows[2][k]); // (the scale along k)
                        half[k] = (lmx[k] - lmn[k]) * 0.5f * axl[k];
                        (void)cx;
                    }
                    float lc[3] = { (lmn[0] + lmx[0]) * 0.5f, (lmn[1] + lmx[1]) * 0.5f, (lmn[2] + lmx[2]) * 0.5f };
                    for (int j = 0; j < 3; j++) ctr[j] = rows[j][0] * lc[0] + rows[j][1] * lc[1] + rows[j][2] * lc[2] + rows[j][3];
                    int k = half[0] >= half[1] && half[0] >= half[2] ? 0 : half[1] >= half[2] ? 1 : 2;
                    float rad = fmaxf(half[(k + 1) % 3], half[(k + 2) % 3]), seg = fmaxf(half[k] - rad, 0.0f);
                    float a0[3], a1[3];
                    for (int j = 0; j < 3; j++) {
                        float dir = axl[k] > 1e-6f ? rows[j][k] / axl[k] : 0.0f;
                        a0[j] = ctr[j] - dir * seg, a1[j] = ctr[j] + dir * seg;
                    }
                    if (rad > 0.5f) add_col(out, n, 1, a0, a1, rad);
                }
                continue;
            }
            if (at) continue; // (other components: their actor's shapes stand for them)
            if (a->mask & COMP_BIT(COMP_CHARACTER)) {
                const CCharacter* ch = ecs_arch_column(a, COMP_CHARACTER);
                if (!near(p, center, reach + ch[r].half_height)) continue;
                float hh = fmaxf(ch[r].half_height - ch[r].radius, 0.0f);
                float a0[3] = { p[0], p[1], p[2] - hh }, a1[3] = { p[0], p[1], p[2] + hh };
                if (!(a->mask & COMP_BIT(COMP_SKELETAL_MESH))) add_col(out, n, 1, a0, a1, ch[r].radius); // (a skeletal one: its bones)
                continue;
            }
            if (a->mask & COMP_BIT(COMP_STATIC_MESH)) { // a mesh: its box in the world (rigid bodies too: their shapes' boxes)
                const CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
                float lmn[3], lmx[3], rows[3][4], mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
                render_mesh_bounds(sm[r].mesh, lmn, lmx);
                render_transform_rows(&tr[r], rows);
                for (int k = 0; k < 8; k++) {
                    float q[3] = { k & 1 ? lmx[0] : lmn[0], k & 2 ? lmx[1] : lmn[1], k & 4 ? lmx[2] : lmn[2] };
                    for (int j = 0; j < 3; j++) {
                        float v = rows[j][0] * q[0] + rows[j][1] * q[1] + rows[j][2] * q[2] + rows[j][3];
                        mn[j] = fminf(mn[j], v), mx[j] = fmaxf(mx[j], v);
                    }
                }
                // (near enough: the box's nearest point within reach of the cloth)
                float q[3], dd = 0.0f;
                for (int j = 0; j < 3; j++) q[j] = fminf(fmaxf(center[j], mn[j]), mx[j]), dd += (q[j] - center[j]) * (q[j] - center[j]);
                if (dd > reach * reach) continue;
                // (Q9) a physics body (simulating): an oriented box (its turn kept), its contact forces wanted back; a
                // turned mesh too (a tilted board); the level's straight ones stay boxes in the world's axes
                const CRigidBody* rb = (a->mask & COMP_BIT(COMP_RIGID_BODY)) ? &((const CRigidBody*)ecs_arch_column(a, COMP_RIGID_BODY))[r] : NULL;
                bool body = rb && rb->simulate;
                bool turned = fabsf(tr[r].rotation[0]) > 0.01f || fabsf(tr[r].rotation[1]) > 0.01f || fabsf(tr[r].rotation[2]) > 0.01f;
                if (sm[r].mesh == SHAPE_SPHERE) {
                    float c[3] = { (mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f };
                    uint32_t at = *n;
                    add_col(out, n, body ? 16 : 0, c, c, (mx[0] - mn[0]) * 0.5f);
                    if (body && s_col_ent && at < *n) s_col_ent[at] = ents[r];
                } else if (body || turned) {
                    float ax[3][3], q[4], ctr[3], half[3];
                    rotator_axes(tr[r].rotation, ax);
                    axes_to_quat(ax, q);
                    float lc[3] = { (lmn[0] + lmx[0]) * 0.5f, (lmn[1] + lmx[1]) * 0.5f, (lmn[2] + lmx[2]) * 0.5f };
                    for (int j = 0; j < 3; j++) ctr[j] = rows[j][0] * lc[0] + rows[j][1] * lc[1] + rows[j][2] * lc[2] + rows[j][3];
                    for (int k = 0; k < 3; k++) half[k] = (lmx[k] - lmn[k]) * 0.5f * fabsf(tr[r].scale[k]);
                    if (!body && half[2] < 50.0f) { // (a tilted plane: a slab under its top too)
                        for (int j = 0; j < 3; j++) ctr[j] -= ax[2][j] * (50.0f - half[2]);
                        half[2] = 50.0f;
                    }
                    uint32_t at = *n;
                    add_col(out, n, body ? 4 + 16 : 4, ctr, half, 0.0f);
                    if (at < *n) {
                        float* cc = out + COL_FLOATS * at;
                        memcpy(&cc[8], q, 16);
                        if (body && s_col_ent) s_col_ent[at] = ents[r];
                    }
                } else {
                    if (mx[2] - mn[2] < 100.0f) mn[2] = mx[2] - 100.0f; // (a plane, a thin slab: solid below its top, else what slips under falls)
                    add_col(out, n, 2, mn, mx, 0.0f);
                }
            }
        }
    }
}

// A Skeletal Mesh actor's bone (a part named so; Q10: else the bone of its skinned mesh, anim_socket_world, or a
// socket): its world place and turn
static bool find_bone(World* w, const char* actor, const char* bone, const CTransform** out, Entity* part, Entity* mesh) {
    Entity act = ENTITY_NONE;
    for (uint32_t i = 1; i < w->slot_count && act == ENTITY_NONE; i++) {
        Entity e = ecs_entity_at(w, i);
        const CName* nm = e ? ecs_get(w, e, COMP_NAME) : NULL;
        if (nm && !ecs_get(w, e, COMP_ATTACH) && !strcmp(nm->text, actor)) act = e;
    }
    if (act == ENTITY_NONE) return false;
    for (uint32_t i = 1; i < w->slot_count; i++) {
        Entity e = ecs_entity_at(w, i);
        const CAttach* at = e ? ecs_get(w, e, COMP_ATTACH) : NULL;
        const CName* nm = at && (Entity)at->owner == act ? ecs_get(w, e, COMP_NAME) : NULL;
        if (nm && !strcmp(nm->text, bone)) {
            *out = ecs_get(w, e, COMP_TRANSFORM);
            *part = e;
            *mesh = (Entity)at->owner;
            return *out != NULL;
        }
    }
    static CTransform skinned[MAX_CLOTH]; // (Q10: the main thread's; one a cloth this frame)
    static uint32_t next;
    float rows[3][4];
    if (!anim_socket_world(w, act, bone, rows)) return false;
    CTransform* t = &skinned[next++ % MAX_CLOTH];
    anim_rows_transform(rows, t);
    *out = t, *part = ENTITY_NONE, *mesh = anim_mesh_entity(w, act);
    return true;
}

uint32_t cloth_debug_vertices(Entity cloth, const float** pos) {
    uint32_t i = 0;
    while (i < C.ncloth && C.ents[i] != cloth) i++;
    if (i >= C.ncloth || !C.pos.mapped) return 0;
    *pos = (const float*)C.pos.mapped + 4u * C.cinfo[i].base;
    return C.cinfo[i].nx * C.cinfo[i].ny;
}

static float fb_push[RE_FRAMES_IN_FLIGHT][MAX_COLLIDERS]; // (each collider's cloth's Rigid Push)
static uint32_t fb_sub[RE_FRAMES_IN_FLIGHT];

static uint32_t fnv(uint32_t h, const void* p, size_t n) {
    const uint8_t* b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

// (Q9) What the GPU gave back from this slot's last frame (its fence waited): the soft bodies' pushes on the physics bodies
// (an impulse over that frame at the push's line through each body), each soft body's mean place (a free one's actor
// follows it) and its fastest vertex (sleep)
static void soft_feedback(World* w, uint32_t slot) {
    int32_t* fb = C.feedback[slot].mapped;
    if (!fb) return;
    C.sstats.rigid_pushes = 0, C.sstats.rigid_force = 0.0f;
    float subs = (float)(fb_sub[slot] ? fb_sub[slot] : 1u);
    const float* cols = C.colliders[slot].mapped;
    if (C.fb_play[slot] && C.fb_dt[slot] > 0.0f && cols)
        for (uint32_t k = 0; k < C.fb_ncol[slot]; k++) {
            Entity e = C.fb_col[slot][k];
            const int32_t* f = fb + 8 * k;
            if (e == ENTITY_NONE || !f[6] || !ecs_alive(w, e)) continue;
            float F[3], M[3], ff = 0.0f;
            for (int j = 0; j < 3; j++) F[j] = (float)f[j] / FB_SCALE / subs, M[j] = (float)f[3 + j] / FB_SCALE / subs, ff += F[j] * F[j];
            if (ff < 1e-8f) continue;
            const float* cc = cols + COL_FLOATS * k;
            float ctr[3];
            for (int j = 0; j < 3; j++) ctr[j] = ((uint32_t)cc[3] & 15u) == 2u ? 0.5f * (cc[j] + cc[4 + j]) : cc[j];
            // the point of the push's line nearest the body's center: (F x M) / |F|^2 (m)
            float pt[3] = { (F[1] * M[2] - F[2] * M[1]) / ff, (F[2] * M[0] - F[0] * M[2]) / ff, (F[0] * M[1] - F[1] * M[0]) / ff };
            float J[3], P[3];
            for (int j = 0; j < 3; j++) J[j] = F[j] * C.fb_dt[slot] * 100.0f * fb_push[slot][k], P[j] = (ctr[j] + pt[j]) * 100.0f; // (kg cm/s, cm)
            physics_add_impulse_at(w, e, J, P);
            // the contact's damping on the body, from its velocity now (no lag): along the push it loses 20/s of its speed
            // into or out of the cushion, across it 8/s (the foam's grip); a resting body (asleep) left alone
            const CRigidBody* rb = ecs_get(w, e, COMP_RIGID_BODY);
            if (rb && !rb->sleeping && fb_push[slot][k] > 0.0f) {
                float fl = sqrtf(ff), fh[3] = { F[0] / fl, F[1] / fl, F[2] / fl };
                float vn = rb->velocity[0] * fh[0] + rb->velocity[1] * fh[1] + rb->velocity[2] * fh[2], dv[3];
                float dn = fminf(30.0f * C.fb_dt[slot], 0.9f), dtg = fminf(8.0f * C.fb_dt[slot], 0.9f);
                for (int j = 0; j < 3; j++) dv[j] = -dn * vn * fh[j] - dtg * (rb->velocity[j] - vn * fh[j]);
                physics_add_impulse(w, e, dv, true);
            }
            C.sstats.rigid_pushes++;
            C.sstats.rigid_force = fmaxf(C.sstats.rigid_force, sqrtf(ff));
        }
    const int32_t* fs = fb + MAX_COLLIDERS * 8;
    for (uint32_t i = 0; i < C.fb_nsoft[slot]; i++) {
        Entity e = C.fb_cloth[slot][i];
        const int32_t* f = fs + 8 * i;
        if (e == ENTITY_NONE || !f[3]) continue;
        uint32_t n = i < C.ncloth && C.ents[i] == e ? i : MAX_CLOTH;
        for (uint32_t j = 0; j < C.ncloth && n == MAX_CLOTH; j++)
            if (C.ents[j] == e) n = j;
        if (n == MAX_CLOTH || !C.soft[n]) continue;
        float sp;
        memcpy(&sp, &f[4], 4);
        C.max_speed[n] = sp;
        // a free one (nothing holds it): its actor where its mass is (the gameplay, the culling, its sleep by distance)
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        const CCloth* c = ecs_get(w, e, COMP_CLOTH);
        if (!t || !c || C.held[n] || C.editor || !C.fb_play[slot]) continue;
        float mean[3], ax[3][3];
        for (int j = 0; j < 3; j++) mean[j] = (C.fb_origin[slot][i][j] + (float)f[j] / FB_SCALE / (float)f[3]) * 100.0f;
        rotator_axes(t->rotation, ax);
        const float* rc = C.stopo[n].center;
        for (int j = 0; j < 3; j++) t->location[j] = mean[j] - (ax[0][j] * rc[0] + ax[1][j] * rc[1] + ax[2][j] * rc[2]);
        memcpy(C.soft_loc[n], t->location, 12);
    }
    memset(fb, 0, 4u * FB_INTS);
}

// (Q9) A soft body into the frame (cloth n): its lattice (built again when its mesh or settings change), its info, its
// colliders, sleeping. Its vertex count (0: none this frame)
static uint32_t soft_fill_one(World* w, uint32_t n, Entity e, CCloth* c, CTransform* tr, const CStaticMesh* sm, uint32_t nv, float dt, float* cols,
                              uint32_t* ncol, uint32_t* owner, bool* relayout, float* self_r) {
    if (!sm) return 0;
    if (C.topo[n].nv) cloth_mesh_free(&C.topo[n]);
    SoftTopo* t = &C.stopo[n];
    float sc[3];
    for (int k = 0; k < 3; k++) sc[k] = fabsf(tr->scale[k]) > 1e-4f ? fabsf(tr->scale[k]) : 1.0f;
    uint32_t key[12];
    soft_topo_key(key, sm->mesh, sc, c);
    memcpy(C.soft_scale[n], sc, 12);
    bool built = false;
    if (!t->nv || memcmp(key, t->key, sizeof key)) {
        built = soft_topo_build(t, sm->mesh, sc, c);
        if (built) {
            printf("[RealEngine] Soft body: %ux%ux%u lattice, %u active vertices, %u cells, cell %.1f cm, %u pinned, %u held, %u mesh vertices (%.1f ms)\n", t->nx, t->ny,
                   t->nz, t->nactive, t->filled, t->cell, t->pinned, t->held, t->nrender, t->build_ms);
            C.sstats.build_ms = t->build_ms;
        } else
            memcpy(t->key, key, sizeof key); // (not a mesh: tried once for this key)
    }
    if (!t->nv || !t->nactive || nv + t->nv > MAX_VERTS) return 0;
    uint32_t nx = t->nx, ny = t->ny * t->nz;
    const CTransform* bone;
    Entity bone_part = ENTITY_NONE, bone_mesh = ENTITY_NONE;
    bool follows = c->follow_actor[0] && c->follow_bone[0] && find_bone(w, c->follow_actor, c->follow_bone, &bone, &bone_part, &bone_mesh);
    if (follows) { // belly fat on the pelvis: the bone carries the actor (its held vertices with it)
        float ax[3][3];
        rotator_axes(bone->rotation, ax);
        for (int k = 0; k < 3; k++)
            tr->location[k] = bone->location[k] + ax[0][k] * c->follow_offset[0] + ax[1][k] * c->follow_offset[1] + ax[2][k] * c->follow_offset[2];
        memcpy(tr->rotation, bone->rotation, sizeof tr->rotation);
    }
    bool held = (t->pinned + t->held) > 0 && !c->released;
    C.held[n] = held || follows;
    bool same = C.ents[n] == e && C.soft[n];
    bool moved = same && (fabsf(tr->location[0] - C.soft_loc[n][0]) > 0.5f || fabsf(tr->location[1] - C.soft_loc[n][1]) > 0.5f ||
                          fabsf(tr->location[2] - C.soft_loc[n][2]) > 0.5f);
    // starts over: new, rebuilt, its size changed, reset, play began or ended, or (free, or in the editor) moved by someone
    bool fresh = !same || C.sizes[n][0] != nx || C.sizes[n][1] != ny || c->reset || built || C.editor != C.was_editor || (moved && (!C.held[n] || C.editor));
    if (built || C.sizes[n][0] != nx || C.sizes[n][1] != ny || C.ents[n] != e) *relayout = true;
    c->reset = 0;
    C.ents[n] = e, C.sizes[n][0] = nx, C.sizes[n][1] = ny;
    C.soft[n] = true, C.shadow[n] = false, C.tears[n] = false, C.tris_ready[n] = false, C.ntri[n] = 0, C.key1[n] = 0x40000000u | ny;
    memcpy(C.soft_loc[n], tr->location, 12);
    ClothGpu* g = &C.cinfo[n];
    memset(g, 0, sizeof *g);
    CTransform rigid = *tr;
    rigid.scale[0] = rigid.scale[1] = rigid.scale[2] = 1.0f; // (its lattice's rest already scaled)
    render_transform_rows(&rigid, g->rows);
    g->base = nv, g->nx = nx, g->ny = ny, g->flags = CLF_SOFT | (fresh ? CLF_RESET : 0u);
    if (C.pipe_block && t->nv <= SB_MAX && !getenv("REALENGINE_SOFT_BY_COLOR")) g->flags |= CLF_BLOCK; // (its lattice in shared memory: one dispatch a substep)
    float cell = t->cell * 0.01f;
    g->dx = g->dy = cell;
    g->mass = fmaxf(c->soft_density, 1.0f) * cell * cell * cell / 8.0f; // (a cell's mass to each of its 8 corners)
    float E = fmaxf(c->youngs, 0.01f) * 1000.0f, nu = fminf(fmaxf(c->poisson, 0.0f), 0.49f);
    float mu = E / (2.0f * (1.0f + nu)), lam = E * nu / ((1.0f + nu) * (1.0f - 2.0f * nu));
    g->k_stretch = 4.0f / 3.0f * mu, g->k_shear = lam + 5.0f / 6.0f * mu; // (Smith et al.'s Lame parameters for stable Neo-Hookean)
    g->k_bend = fmaxf(c->attach_stiffness, 0.0f);
    g->damping = fminf(fmaxf(c->soft_damping, 0.0f), 1.0f), g->friction = fminf(fmaxf(c->friction, 0.0f), 1.0f);
    g->thickness = fmaxf(c->thickness, 0.05f) * 0.01f, g->gravity = c->gravity;
    // plasticity: only what is held (a free body has no frame to keep a dent in)
    g->tint[0] = C.held[n] ? fminf(fmaxf(c->plasticity, 0.0f), 1.0f) * 6.0f : 0.0f;
    g->tint[1] = C.held[n] && c->recovery > 0.0f ? 1.0f / c->recovery : 0.0f;
    g->look[3] = C.time;
    if (c->self_collision) { // (vertex against vertex with the other soft bodies and the cloths)
        g->look[2] = c->self_distance > 0.0f ? c->self_distance * 0.005f : 0.3f * cell;
        *self_r = fmaxf(*self_r, g->look[2]);
    }
    // its colliders: what is within its reach
    float ax[3][3], center[3];
    rotator_axes(tr->rotation, ax);
    for (int k = 0; k < 3; k++) center[k] = tr->location[k] + ax[0][k] * t->center[0] + ax[1][k] * t->center[1] + ax[2][k] * t->center[2];
    float reach = t->radius + 40.0f; // (tight: the block pass keeps 32 in shared memory; a collider tested is ALU on one SM)
    g->col_first = *ncol;
    uint32_t before = *ncol;
    if (!getenv("REALENGINE_CLOTH_NO_COLLIDERS")) gather_colliders(w, center, reach, cols, ncol, e, bone_part, follows ? bone_mesh : ENTITY_NONE, follows ? c->follow_bone : NULL); // (Q10: not its own bone's Physics Asset body: its held vertices sit in it)
    { // only what comes within 15 cm of its lattice's box (world): fewer tests a vertex, and what moves farther away never
        // wakes it (someone sitting on the next cushion)
        float bmn[3] = { 1e30f, 1e30f, 1e30f }, bmx[3] = { -1e30f, -1e30f, -1e30f };
        for (int q = 0; q < 8; q++) {
            float l[3] = { t->origin[0] + ((q & 1) ? (float)(t->nx - 1) * t->cell : 0.0f), t->origin[1] + ((q & 2) ? (float)(t->ny - 1) * t->cell : 0.0f),
                           t->origin[2] + ((q & 4) ? (float)(t->nz - 1) * t->cell : 0.0f) };
            for (int k = 0; k < 3; k++) {
                float wv = (tr->location[k] + ax[0][k] * l[0] + ax[1][k] * l[1] + ax[2][k] * l[2]) * 0.01f;
                bmn[k] = fminf(bmn[k], wv - 0.15f), bmx[k] = fmaxf(bmx[k], wv + 0.15f);
            }
        }
        uint32_t keep = before;
        for (uint32_t k = before; k < *ncol; k++) {
            const float* cc = cols + COL_FLOATS * k;
            uint32_t type = (uint32_t)cc[3] & 15u;
            float cmn[3], cmx[3];
            for (int j = 0; j < 3; j++) {
                if (type == 2u) cmn[j] = cc[j], cmx[j] = cc[4 + j];
                else if (type == 4u) { float e = sqrtf(cc[4] * cc[4] + cc[5] * cc[5] + cc[6] * cc[6]); cmn[j] = cc[j] - e, cmx[j] = cc[j] + e; }
                else if (type == 1u) cmn[j] = fminf(cc[j], cc[4 + j]) - cc[7], cmx[j] = fmaxf(cc[j], cc[4 + j]) + cc[7];
                else if (type == 0u) cmn[j] = cc[j] - cc[7], cmx[j] = cc[j] + cc[7];
                else cmn[j] = -1e30f, cmx[j] = 1e30f;
            }
            bool overlap = true;
            for (int j = 0; j < 3; j++) overlap &= cmn[j] <= bmx[j] && cmx[j] >= bmn[j];
            if (!overlap) continue;
            if (keep != k) memcpy(cols + COL_FLOATS * keep, cc, sizeof(float) * COL_FLOATS), s_col_ent[keep] = s_col_ent[k];
            keep++;
        }
        *ncol = keep;
    }
    if (*ncol - before > 32u) *ncol = before + 32u; // (the block pass keeps 32 in shared memory)
    g->col_count = *ncol - before;
    for (uint32_t k = before; k < *ncol; k++) fb_push[C.slot][k] = fmaxf(c->rigid_push, 0.0f);
    // sleep: still (its fastest vertex under 1 cm/s), nothing around it moved, its settings the same; or far; or the editor
    uint32_t ch = fnv(2166136261u, &before, 0);
    for (uint32_t k = before * COL_FLOATS; k < *ncol * COL_FLOATS; k++) {
        int32_t q = (int32_t)lrintf(cols[k] * 400.0f); // (2.5 mm)
        ch = fnv(ch, &q, 4);
    }
    uint32_t cfg = fnv(2166136261u, &c->kind, sizeof *c - offsetof(CCloth, kind));
    cfg = fnv(cfg, &c->friction, 4), cfg = fnv(cfg, &c->thickness, 4), cfg = fnv(cfg, &c->gravity, 4);
    bool kicked = C.kick_pending && (C.kick_e == ENTITY_NONE || C.kick_e == e);
    bool wake = fresh || ch != C.col_hash[n] || cfg != C.cfg_hash[n] || kicked || !c->sleep;
    C.col_hash[n] = ch, C.cfg_hash[n] = cfg;
    if (wake) C.still[n] = 0;
    else if (C.max_speed[n] < 0.01f) C.still[n]++;
    else C.still[n] = 0;
    float dc[3] = { center[0] - C.cam[0], center[1] - C.cam[1], center[2] - C.cam[2] };
    bool far = c->sleep && c->sleep_distance > 0.0f && sqrtf(dc[0] * dc[0] + dc[1] * dc[1] + dc[2] * dc[2]) * 0.01f > c->sleep_distance;
    C.asleep[n] = C.editor || dt <= 0.0f || (c->sleep && (far || C.still[n] > 45u) && !kicked);
    g->soft[0] = (float)t->ny, g->soft[1] = C.asleep[n] ? 1.0f : 0.0f, g->soft[2] = c->released ? 1.0f : 0.0f;
    g->soft[3] = fmaxf(c->yield, 0.0f) * cell;
    if (!C.asleep[n] || fresh) C.sim_version[n]++;
    for (uint32_t k = 0; k < t->nv; k++) owner[nv + k] = n;
    return t->nv;
}

void cloth_fill(World* w, float dt, uint32_t slot) {
    double t0 = re_time_seconds();
    static uint32_t dbg_n;
    if (C.pos.mapped && C.ncloth && (dbg_n++ % 120u) == 0u) { // (REALENGINE_CLOTH_DEBUG: a vertex of each cloth)
        const float* pp = C.pos.mapped;
        const ClothGpu* in = C.cinfo;
        for (uint32_t i = 0; i < C.ncloth; i++) { uint32_t bad = 0; float zmin = 1e9f, zmax = -1e9f; for (uint32_t v = in[i].base; v < in[i].base + in[i].nx * in[i].ny; v++) { float z = pp[4 * v + 2]; if (!(z == z) || fabsf(z) > 1e4f) bad++; else zmin = fminf(zmin, z), zmax = fmaxf(zmax, z); } double mx = 0, my = 0; for (uint32_t v = in[i].base; v < in[i].base + in[i].nx * in[i].ny; v++) mx += pp[4 * v], my += pp[4 * v + 1]; mx /= in[i].nx * in[i].ny, my /= in[i].nx * in[i].ny; fprintf(stderr, "[cloth %u] bad %u z %.2f..%.2f mean %.2f %.2f actor %.2f %.2f\n", i, bad, zmin, zmax, mx, my, in[i].rows[0][3] * 0.01f, in[i].rows[1][3] * 0.01f); }
    }
    if (dt > 1.0f / 20.0f) dt = 1.0f / 20.0f;
    C.time += dt;
    C.slot = slot % RE_FRAMES_IN_FLIGHT;
    soft_feedback(w, C.slot); // (Q9: this slot's last frame, done on the GPU)
    ClothGpu* info = C.cinfo; // (built here, in plain memory: copied whole into the frame's set below)
    float* cols = C.colliders[C.slot].mapped;
    uint32_t* owner = C.owner[C.slot].mapped;
    uint32_t n = 0, nv = 0, ncol = 0, sub = 1, it = 4, ntri_all = 0, nadj_all = 0;
    float self_r = 0.0f;
    bool relayout = false;
    s_col_ent = C.fb_col[C.slot];
    C.sstats.bodies = C.sstats.awake = C.sstats.vertices = C.sstats.tets = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_CLOTH)) || !(a->mask & COMP_BIT(COMP_TRANSFORM)) || getenv("REALENGINE_NO_CLOTH")) continue; // (the env: measuring without)
        CCloth* cl = ecs_arch_column(a, COMP_CLOTH);
        CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        const CStaticMesh* smc = (a->mask & COMP_BIT(COMP_STATIC_MESH)) ? ecs_arch_column(a, COMP_STATIC_MESH) : NULL;
        for (uint32_t r = 0; r < a->count && n < MAX_CLOTH; r++) {
            CCloth* c = &cl[r];
            if (c->kind == CLOTH_KIND_SOFT) { // (Q9) a soft body: its lattice, the same solver
                if (getenv("REALENGINE_NO_SOFT")) continue; // (measuring without)
                uint32_t add = soft_fill_one(w, n, ents[r], c, &tr[r], smc ? &smc[r] : NULL, nv, dt, cols, &ncol, owner, &relayout, &self_r);
                if (!add) continue;
                C.sstats.bodies++, C.sstats.awake += !C.asleep[n], C.sstats.vertices += C.stopo[n].nactive, C.sstats.tets += 6u * C.stopo[n].filled;
                nv += add;
                if (c->substeps > sub) sub = c->substeps;
                if (c->iterations > it) it = c->iterations;
                n++;
                continue;
            }
            if (C.stopo[n].nv) soft_topo_free(&C.stopo[n]), relayout = true;
            if (C.soft[n]) C.soft[n] = false, C.ents[n] = ENTITY_NONE; // (the slot was a soft body's)
            uint32_t nx = c->res_x < 2 ? 2 : c->res_x > 128 ? 128 : c->res_x, ny = c->res_y < 2 ? 2 : c->res_y > 128 ? 128 : c->res_y;
            // a mesh of its own: its graph (built again when the mesh or its pins change)
            ClothMeshTopo* topo = &C.topo[n];
            bool mesh = false, built = false;
            if (c->mesh & 0x80000000u) {
                float sc[3] = { c->mesh_scale[0] > 0.0f ? c->mesh_scale[0] : 1.0f, c->mesh_scale[1] > 0.0f ? c->mesh_scale[1] : 1.0f,
                                c->mesh_scale[2] > 0.0f ? c->mesh_scale[2] : 1.0f };
                float ax[3] = { c->pin_axis[0], c->pin_axis[1], c->pin_axis[2] };
                if (ax[0] == 0.0f && ax[1] == 0.0f && ax[2] == 0.0f) ax[2] = 1.0f;
                uint32_t key[8];
                cloth_mesh_key(key, c->mesh, sc, c->pin_band, ax, c->pin != CLOTH_PIN_NONE);
                if (!topo->nv || memcmp(key, topo->key, sizeof key)) {
                    double tb = re_time_seconds();
                    built = cloth_mesh_build(topo, c->mesh, sc, c->pin_band, ax, c->pin != CLOTH_PIN_NONE);
                    if (built) printf("[RealEngine] Cloth: mesh %08x: %u vertices, %u triangles, %u colors, %u pinned (%.1f ms)\n", c->mesh, topo->nv, topo->nt,
                                      topo->ncolors, topo->pinned, (re_time_seconds() - tb) * 1000.0);
                    else memcpy(topo->key, key, sizeof key); // (not a cloth: tried once for this key)
                }
                mesh = topo->nv >= 3;
                if (!mesh) continue;
                if (ntri_all + topo->nt > MAX_TRIS || nadj_all + topo->nadj > MAX_ADJ) continue;
                nx = topo->nv, ny = 1;
            } else if (topo->nv)
                cloth_mesh_free(topo);
            if (nv + nx * ny > MAX_VERTS) break;
            const CTransform* bone;
            Entity bone_part = ENTITY_NONE, bone_mesh = ENTITY_NONE;
            if (c->follow_actor[0] && c->follow_bone[0] && find_bone(w, c->follow_actor, c->follow_bone, &bone, &bone_part, &bone_mesh)) { // a cape: the bone carries it
                float ax[3][3];
                rotator_axes(bone->rotation, ax);
                for (int k = 0; k < 3; k++)
                    tr[r].location[k] = bone->location[k] + ax[0][k] * c->follow_offset[0] + ax[1][k] * c->follow_offset[1] + ax[2][k] * c->follow_offset[2];
                tr[r].rotation[1] = bone->rotation[1];
                if (c->follow_turn) memcpy(tr[r].rotation, bone->rotation, sizeof tr[r].rotation); // (a garment made of a part: the bone's whole turn)
            }
            bool fresh = C.ents[n] != ents[r] || C.sizes[n][0] != nx || C.sizes[n][1] != ny || c->reset || built;
            if (built) relayout = true;
            if (C.sizes[n][0] != nx || C.sizes[n][1] != ny || C.ents[n] != ents[r]) C.tris_ready[n] = false;
            if (fresh && (C.ents[n] != ents[r] || C.sizes[n][0] != nx || C.sizes[n][1] != ny)) relayout = true;
            C.ents[n] = ents[r], C.sizes[n][0] = nx, C.sizes[n][1] = ny;
            ClothGpu* g = &info[n];
            memset(g, 0, sizeof *g);
            CTransform rigid = tr[r];
            rigid.scale[0] = rigid.scale[1] = rigid.scale[2] = 1.0f; // (its size is width and height)
            render_transform_rows(&rigid, g->rows);
            g->base = nv, g->nx = nx, g->ny = ny;
            static const uint32_t pins[CLOTH_PIN_COUNT] = { 1u, 2u, 4u, 8u, 0u };
            g->flags = pins[c->pin < CLOTH_PIN_COUNT ? c->pin : 0] | (fresh ? CLF_RESET : 0u);
            c->reset = 0;
            g->dx = fmaxf(c->width, 1.0f) * 0.01f / (float)(nx - 1), g->dy = fmaxf(c->height, 1.0f) * 0.01f / (float)(ny - 1);
            g->mass = fmaxf(c->density, 1.0f) * 0.001f * (c->width * c->height * 1e-4f) / (float)(nx * ny);
            C.ntri[n] = 0, C.key1[n] = ny;
            if (mesh) { // (its spacing: the mean edge; its mass: its area's; its triangles after the others')
                g->flags |= CLF_MESH;
                g->dx = g->dy = fmaxf(topo->edge, 1e-3f);
                g->mass = fmaxf(c->density, 1.0f) * 0.001f * fmaxf(topo->area, 1e-4f) / (float)topo->nv;
                g->color = ntri_all, g->iter = topo->nt; // (tri_first, ntris: cloth.glsl)
                C.ntri[n] = topo->nt, C.key1[n] = 0x80000000u | topo->nt;
                ntri_all += topo->nt, nadj_all += topo->nadj;
            }
            g->k_stretch = c->stretch, g->k_shear = c->shear, g->k_bend = c->bend;
            g->damping = fminf(fmaxf(c->damping, 0.0f), 1.0f), g->friction = fminf(fmaxf(c->friction, 0.0f), 1.0f);
            g->thickness = fmaxf(c->thickness, 0.1f) * 0.01f, g->tear = c->tear, g->gravity = c->gravity;
            memcpy(g->wind, c->wind, 12), g->wind[3] = c->gust;
            memcpy(g->tint, c->color, 12), g->tint[3] = c->roughness;
            g->look[0] = c->sheen, g->look[1] = c->translucency, g->look[3] = C.time;
            if (c->self_collision) { // its radius (m): half the distance kept; auto: twice the thickness, 3/4 of a cell (a layer
                // can't slip between the vertices of another: a quad's middle is within reach of its corners)
                float dist = c->self_distance > 0.0f ? c->self_distance * 0.01f : fmaxf(2.0f * g->thickness, 0.75f * fminf(g->dx, g->dy));
                g->look[2] = 0.5f * dist;
                self_r = fmaxf(self_r, g->look[2]);
            }
            // its colliders: what is within its size of it
            float reach = fmaxf(c->width, c->height) * 1.2f + 50.0f;
            float center[3] = { tr[r].location[0], tr[r].location[1], tr[r].location[2] - c->height * 0.5f };
            if (mesh) reach = topo->radius * 1.2f + 50.0f, center[2] = tr[r].location[2]; // (a mesh: around its origin)
            g->col_first = ncol;
            uint32_t before = ncol;
            if (!getenv("REALENGINE_CLOTH_NO_COLLIDERS")) gather_colliders(w, center, reach, cols, &ncol, ents[r], bone_part, bone_mesh, c->follow_bone); // (not the bone it hangs from: its pins sit in it)
            if (ncol - before > PER_CLOTH_COLLIDERS) ncol = before + PER_CLOTH_COLLIDERS;
            g->col_count = ncol - before;
            for (uint32_t k = 0; k < nx * ny; k++) owner[nv + k] = n;
            { // where it can be: its top edge's place, as far as it reaches (a box around the hanging point)
                float reach2 = mesh ? topo->radius * 1.6f + 30.0f : sqrtf(c->width * c->width + c->height * c->height) + 30.0f;
                memcpy(C.box_prev[n], C.box[n], sizeof C.box[n]);
                for (int k = 0; k < 3; k++) C.box[n][0][k] = tr[r].location[k] - reach2, C.box[n][1][k] = tr[r].location[k] + reach2;
                if (fresh) memcpy(C.box_prev[n], C.box[n], sizeof C.box[n]);
                C.shadow[n] = c->cast_shadow != 0 && !getenv("REALENGINE_NO_CLOTH_SHADOW"); // (the env: measuring without)
                C.tears[n] = c->tear > 1.0f;
            }
            nv += nx * ny;
            if (c->substeps > sub) sub = c->substeps;
            if (c->iterations > it) it = c->iterations;
            n++;
        }
    }
    if (relayout || n != C.ncloth) { // (a cloth came, went or changed its size: every block starts over; the color lists anew)
        for (uint32_t i = 0; i < MAX_CLOTH; i++) C.tris_ready[i] = false, C.rebuild[i] = true; // (the blocks moved: their triangles too)
        for (uint32_t i = 0; i < n; i++) info[i].flags |= CLF_RESET;
        uint32_t* ord = C.order_cpu;
        uint32_t at = 0;
        C.ncolors = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t k = (info[i].flags & CLF_SOFT) ? ((info[i].flags & CLF_BLOCK) ? 0u : 8u) : (info[i].flags & CLF_MESH) ? C.topo[i].ncolors : 9u;
            if (k > C.ncolors) C.ncolors = k;
        }
        for (uint32_t col = 0; col < MAX_COLORS; col++) {
            C.cfirst[col] = at;
            for (uint32_t i = 0; i < n; i++) {
                if (info[i].flags & CLF_SOFT) { // (a soft body: its active vertices by (x, y, z) mod 2; a block's: the block pass)
                    const SoftTopo* st = &C.stopo[i];
                    if (col < 8 && !(info[i].flags & CLF_BLOCK))
                        for (uint32_t v = 0; v < st->nv; v++)
                            if (st->cells[v] && st->color[v] == col) ord[at++] = info[i].base + v;
                    continue;
                }
                if (info[i].flags & CLF_MESH) { // (a mesh: its graph's colors)
                    for (uint32_t v = 0; v < C.topo[i].nv; v++)
                        if (C.topo[i].color[v] == col) ord[at++] = info[i].base + v;
                    continue;
                }
                if (col >= 9) continue;
                for (uint32_t y = col / 3; y < info[i].ny; y += 3)
                    for (uint32_t x = col % 3; x < info[i].nx; x += 3) ord[at++] = info[i].base + y * info[i].nx + x;
            }
            C.ccount[col] = at - C.cfirst[col];
        }
        C.order_n = at, C.order_ver++;
        // the meshes' graphs into their buffers: the GPU idle (a frame in flight reads them; a layout change is rare)
        bool any_mesh = false;
        for (uint32_t i = 0; i < n; i++) any_mesh |= (info[i].flags & (CLF_MESH | CLF_SOFT)) != 0;
        if (any_mesh && C.mrest.mapped && C.muv.mapped && C.madj_off.mapped && C.madj.mapped && C.mtris.mapped) {
            vkDeviceWaitIdle(C.vk->device);
            uint32_t adj_at = 0, emb_at = 0;
            C.embed_half ^= 1u; // (Q9: the other half: this frame's skins still read the last layout's)
            for (uint32_t i = 0; i < n; i++) {
                if (info[i].flags & CLF_SOFT) { // (Q9) its lattice's rest and hold, offsets and cells; its mesh's places
                    const SoftTopo* st = &C.stopo[i];
                    memcpy((float*)C.mrest.mapped + 4u * info[i].base, st->rest, 16u * st->nv);
                    memcpy((float*)C.muv.mapped + 2u * info[i].base, st->aux, 8u * st->nv);
                    C.soft_embed[i] = 0;
                    if (emb_at + st->nrender <= MAX_EMBED && C.sembed.mapped) {
                        uint32_t off = C.embed_half * MAX_EMBED + emb_at;
                        memcpy((float*)C.sembed.mapped + 4u * off, st->embed, 16u * st->nrender);
                        C.soft_embed[i] = C.sembed.addr + (VkDeviceAddress)off * 16u;
                        emb_at += st->nrender;
                    }
                    continue;
                }
                if (!(info[i].flags & CLF_MESH)) continue;
                const ClothMeshTopo* t = &C.topo[i];
                uint32_t base = info[i].base;
                memcpy((float*)C.mrest.mapped + 4u * base, t->rest, 16u * t->nv);
                memcpy((float*)C.muv.mapped + 2u * base, t->uv, 8u * t->nv);
                uint32_t* off = (uint32_t*)C.madj_off.mapped + 2u * base;
                uint32_t* adj = (uint32_t*)C.madj.mapped + 2u * adj_at;
                for (uint32_t v = 0; v < t->nv; v++) off[2 * v] = adj_at + t->adj_off[2 * v], off[2 * v + 1] = t->adj_off[2 * v + 1];
                for (uint32_t e = 0; e < t->nadj; e++) { // (the springs' other vertex global; a triangle stays the cloth's)
                    uint32_t x = t->adj[2 * e], k = x >> 30;
                    adj[2 * e] = k == CLOTH_ADJ_TRI ? x : (k << 30) | ((x & 0x3FFFFFFFu) + base);
                    adj[2 * e + 1] = t->adj[2 * e + 1];
                }
                memcpy((uint32_t*)C.mtris.mapped + 3u * info[i].color, t->tris, 12u * t->nt);
                adj_at += t->nadj;
            }
        }
    }
    for (uint32_t i = n; i < MAX_CLOTH; i++) {
        C.ents[i] = ENTITY_NONE, C.sizes[i][0] = C.sizes[i][1] = 0, C.soft[i] = false;
        if (C.topo[i].nv) cloth_mesh_free(&C.topo[i]);
        if (C.stopo[i].nv) soft_topo_free(&C.stopo[i]);
    }
    C.ncloth = n, C.nverts = nv, C.ncol = ncol;
    C.any_mesh = ntri_all > 0;
    if (getenv("REALENGINE_CLOTH_SUBSTEPS")) sub = (uint32_t)atoi(getenv("REALENGINE_CLOTH_SUBSTEPS")); // (measuring)
    if (getenv("REALENGINE_CLOTH_ITERATIONS")) it = (uint32_t)atoi(getenv("REALENGINE_CLOTH_ITERATIONS"));
    C.substeps = sub > 4 ? 4 : sub < 1 ? 1 : sub, C.iterations = it > 24 ? 24 : it;
    C.any = n > 0 && dt > 0.0f;
    C.paused = dt <= 0.0f;
    { // (Q9) every one a soft body asleep (or the editor's): no simulation at all (a reset still runs below)
        bool all_asleep = n > 0;
        for (uint32_t i = 0; i < n && all_asleep; i++) all_asleep = C.soft[i] && C.asleep[i];
        if (all_asleep) C.any = false, C.paused = true;
    }
    float h = dt > 0.0f ? dt / (float)C.substeps : 1.0f / 120.0f;
    C.nblock = 0;
    for (uint32_t i = 0; i < n; i++) {
        info[i].inv_h = 1.0f / h;
        if (info[i].flags & CLF_SOFT) info[i].iter = C.iterations; // (the block pass's loop)
        if ((info[i].flags & CLF_BLOCK) && !C.asleep[i]) C.nblock = i + 1u; // (its workgroups: up to the last awake block)
    }
    // into this frame's set (the GPU is done with it: the frame's fence waited)
    if (n) memcpy(C.info[C.slot].mapped, info, n * sizeof *info);
    if (C.order_ver_of[C.slot] != C.order_ver) {
        memcpy(C.order[C.slot].mapped, C.order_cpu, C.order_n * sizeof(uint32_t));
        C.order_ver_of[C.slot] = C.order_ver;
    }
    ClothHeaderGpu* hd = C.header[C.slot].mapped;
    hd->info = C.info[C.slot].addr, hd->pos = C.pos.addr, hd->prev = C.prev.addr, hd->vel = C.vel.addr, hd->target = C.target.addr;
    hd->owner = C.owner[C.slot].addr, hd->mask = C.mask.addr, hd->colliders = C.colliders[C.slot].addr;
    hd->ncloth = n, hd->nverts = nv, hd->ncolliders = ncol, hd->color = 0;
    hd->time[0] = C.time, hd->time[1] = h;
    memcpy(hd->cut, C.cut, sizeof C.cut);
    hd->order = C.order[C.slot].addr, hd->idx = C.idx.addr;
    C.self_cell = self_r > 0.0f ? 2.0f * self_r : 0.0f;
    hd->head = C.head.addr, hd->next = C.next.addr, hd->selfnb = C.selfnb.addr, hd->cell = C.self_cell, hd->nbuckets = C.self_cell > 0.0f ? SELF_BUCKETS : 0u;
    memcpy(hd->cfirst, C.cfirst, sizeof C.cfirst), memcpy(hd->ccount, C.ccount, sizeof C.ccount);
    hd->mrest = C.mrest.addr, hd->muv = C.muv.addr, hd->madj_off = C.madj_off.addr, hd->madj = C.madj.addr, hd->mtris = C.mtris.addr, hd->mnormal = C.mnormal.addr;
    if (!C.cut_pending) hd->cut[3] = 0.0f;
    // (Q9) the impulse (once), the feedback and what it belongs to (read when this slot comes round again)
    memset(hd->kick, 0, sizeof hd->kick), memset(hd->kick_vel, 0, sizeof hd->kick_vel);
    if (C.kick_pending && !C.editor) {
        memcpy(hd->kick, C.kick, 16), memcpy(hd->kick_vel, &C.kick[4], 12);
        hd->kick_vel[3] = -1.0f;
        for (uint32_t i = 0; i < n && C.kick_e != ENTITY_NONE; i++)
            if (C.ents[i] == C.kick_e) hd->kick_vel[3] = (float)i;
        if (C.kick_e != ENTITY_NONE && hd->kick_vel[3] < 0.0f) hd->kick[3] = 0.0f; // (not a soft body: none)
    }
    C.kick_pending = false;
    hd->feedback = C.feedback[C.slot].addr, hd->fb_cloth = MAX_COLLIDERS * 8u, hd->pad_h = 0;
    C.fb_ncol[C.slot] = ncol, C.fb_nsoft[C.slot] = n, C.fb_dt[C.slot] = dt, C.fb_play[C.slot] = !C.editor;
    fb_sub[C.slot] = C.substeps;
    for (uint32_t i = 0; i < n; i++) {
        C.fb_cloth[C.slot][i] = C.soft[i] ? C.ents[i] : ENTITY_NONE;
        for (int k = 0; k < 3; k++) C.fb_origin[C.slot][i][k] = info[i].rows[k][3] * 0.01f;
    }
    // (Q9) what the renderer's skins read next frame (this layout's: the GPU's positions then are this frame's)
    C.nviews = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!C.soft[i]) continue;
        ClothSoftView* sv = &C.views[C.nviews];
        VkDeviceAddress emb = C.soft_embed[i]; // (written at the layout: kept by index until the next)
        const SoftTopo* st = &C.stopo[i];
        sv->e = C.ents[i], sv->pos = C.pos.addr, sv->embed = emb, sv->base = info[i].base, sv->nx = st->nx, sv->ny = st->ny, sv->nz = st->nz;
        sv->nrender = emb ? st->nrender : 0u;
        for (int k = 0; k < 3; k++) sv->scale_over_cell[k] = C.soft_scale[i][k] / (st->cell > 0.0f ? st->cell : 1.0f);
        sv->version = C.sim_version[i];
        C.nviews++;
    }
    C.was_editor = C.editor;
    C.stats.cloths = n, C.stats.vertices = nv, C.stats.colliders = ncol;
    // its shadow in the virtual shadow maps: drawn again where it is and was while it moves (a still one stays cached)
    if (C.any && vsm_enabled())
        for (uint32_t i = 0; i < n; i++)
            if (C.shadow[i]) vsm_dirty_box(C.box[i][0], C.box[i][1]), vsm_dirty_box(C.box_prev[i][0], C.box_prev[i][1]);
    C.stats.cpu_ms = (re_time_seconds() - t0) * 1000.0;
    // (a first frame of a new cloth still draws: the reset pass runs even when paused)
    if (n && dt <= 0.0f)
        for (uint32_t i = 0; i < n; i++)
            if (info[i].flags & CLF_RESET) C.any = true;
}

static void dispatch(VkCommandBuffer cmd, Push p, uint32_t pass, uint32_t color, uint32_t groups) {
    p.pass = pass, p.pad = color; // (Q10: the substep and their count above: bits 8-11 and 12-15, the moving colliders')
    vkCmdPushConstants(cmd, C.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(cmd, groups, 1, 1);
    C.stats.dispatches++;
}

void cloth_record(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push) {
    C.stats.dispatches = 0;
    if (!C.any || !C.nverts) return;
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VS = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, C.layout, 0, 1, &set, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, C.pipe);
    VkPipelineStageFlags2 AS = C.vk && C.vk->ray_query ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR : 0;
    gpu_barrier(cmd, VS | CS | AS, RW, CS, RW); // (this frame's BLAS read the vertices before they move)
    Push p = *push;
    p.instances = C.header[C.slot].addr;
    uint32_t groups = (C.nverts + 63) / 64;
    dispatch(cmd, p, 4, 0, groups); // the new cloths at their rest pose
    gpu_barrier(cmd, CS, RW, CS, RW);
    if (C.cut_pending) {
        dispatch(cmd, p, 3, 0, groups);
        gpu_barrier(cmd, CS, RW, CS, RW);
        C.cut_pending = false;
    }
    for (uint32_t s = 0; s < (C.paused ? 0u : C.substeps); s++) {
        uint32_t subk = (s << 16) | (C.substeps << 20); // (Q10: the moving capsules' sweep, cloth_sim.comp; 0x100 is Q9's last iteration)
        dispatch(cmd, p, 0, s, groups); // (pad: the substep; a soft body's impulse in the first)
        gpu_barrier(cmd, CS, RW, CS, RW);
        if (C.self_cell > 0.0f) { // self collision: the hash of the predicted places, then each vertex's contacts (once a substep)
            dispatch(cmd, p, 6, 0, SELF_BUCKETS / 64u);
            gpu_barrier(cmd, CS, RW, CS, RW);
            dispatch(cmd, p, 7, 0, groups);
            gpu_barrier(cmd, CS, RW, CS, RW);
            dispatch(cmd, p, 8, 0, groups);
            gpu_barrier(cmd, CS, RW, CS, RW);
        }
        if (C.nblock && C.pipe_block) { // (Q9) the soft bodies' block pass: every iteration and color of the substep in one
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, C.pipe_block);
            dispatch(cmd, p, 10, s + 1u == C.substeps ? 1u : 0u, C.nblock); // (a workgroup a cloth: the blocks work, the rest return)
            gpu_barrier(cmd, CS, RW, CS, RW);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, C.pipe);
        }
        for (uint32_t it = 0; it < C.iterations; it++)
            for (uint32_t col = 0; col < C.ncolors; col++) {
                if (!C.ccount[col]) continue;
                dispatch(cmd, p, 1, col | (it + 1u == C.iterations ? 0x100u : 0u) | subk, (C.ccount[col] + 63) / 64); // (0x100: the last: the soft bodies' pushes)
                gpu_barrier(cmd, CS, RW, CS, RW);
            }
        dispatch(cmd, p, 2, s + 1u == C.substeps ? 1u : 0u, groups); // (pad 1: the last substep, the soft bodies' feedback)
        gpu_barrier(cmd, CS, RW, CS, RW);
    }
    if (C.any_mesh) { // the meshes' normals (their wind next frame, their drawing)
        dispatch(cmd, p, 9, 0, groups);
        gpu_barrier(cmd, CS, RW, CS, RW);
    }
    dispatch(cmd, p, 5, 0, groups); // the triangles (the BLAS's, next frame), torn ones empty
    for (uint32_t i = 0; i < C.ncloth; i++) C.tris_ready[i] = true;
    gpu_barrier(cmd, CS, RW, VS | AS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | (AS ? VK_ACCESS_2_SHADER_READ_BIT : 0));
}

// --- Its shadow ---
void cloth_blas_record(VkCommandBuffer cmd) {
    if (!C.vk || !gi_available() || !C.ncloth) return;
    bool any = false;
    for (uint32_t i = 0; i < C.ncloth; i++) {
        uint32_t nx = C.sizes[i][0], ny = C.sizes[i][1];
        if (!C.shadow[i] || !C.tris_ready[i]) continue;
        uint32_t k1 = C.key1[i], ntris = (k1 & 0x80000000u) ? C.ntri[i] : (nx / 2u) * (ny / 2u) * 2u; // (a grid's: half the grid, pass 5)
        if (C.blas_size[i][0] != nx || C.blas_size[i][1] != k1 || !C.blas[i].as) { // (a new size: a new BLAS; the old one may be in flight)
            if (C.blas[i].as) vkDeviceWaitIdle(C.vk->device);
            gi_dyn_blas_free(C.vk, &C.blas[i]);
            if (!ntris || !gi_dyn_blas_create(C.vk, &C.blas[i], ntris, nx * ny - 1u)) continue;
            C.blas[i].stride = 16;
            C.blas_size[i][0] = nx, C.blas_size[i][1] = k1;
        }
        const ClothGpu* g = &C.cinfo[i];
        // a refit (cheap: 12 cloths built in full every frame cost 0.32 ms) unless its triangles changed: a refit needs
        // the same indices as the last build
        C.blas[i].always_build = C.tears[i] || C.cut_frames > 0 || C.rebuild[i];
        C.rebuild[i] = false;
        gi_dyn_blas_record(cmd, &C.blas[i], C.pos.addr + (VkDeviceAddress)g->base * 16u, nx * ny - 1u, C.idx.addr + (VkDeviceAddress)g->base * 24u);
        any = true;
    }
    if (C.cut_frames) C.cut_frames--;
    if (any)
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                    VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
}

uint32_t cloth_tlas_instances(VkAccelerationStructureInstanceKHR* out, uint32_t max) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < C.ncloth && k < max; i++) {
        if (!C.shadow[i] || !C.tris_ready[i] || !C.blas[i].as || !C.blas[i].updates || C.blas_size[i][0] != C.sizes[i][0] || C.blas_size[i][1] != C.key1[i]) continue;
        VkAccelerationStructureInstanceKHR a;
        memset(&a, 0, sizeof a);
        a.transform.matrix[0][0] = a.transform.matrix[1][1] = a.transform.matrix[2][2] = 100.0f; // (its vertices in meters)
        a.instanceCustomIndex = 0;
        a.mask = 0x02u; // (shadow rays only: no ray shades a cloth's hit)
        a.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        a.accelerationStructureReference = C.blas[i].addr;
        memcpy(&out[k++], &a, sizeof a);
    }
    return k;
}

bool cloth_bounds(float mn[3], float mx[3]) {
    bool any = false;
    for (uint32_t i = 0; i < C.ncloth; i++) {
        if (!C.shadow[i]) continue;
        for (int k = 0; k < 3; k++) mn[k] = fminf(mn[k], C.box[i][0][k]), mx[k] = fmaxf(mx[k], C.box[i][1][k]);
        any = true;
    }
    return any;
}

uint32_t cloth_draw_shadow(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout, VkPipeline pipe, uint32_t level) {
    if (!C.ncloth || !pipe) return 0;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    Push p = *push;
    p.instances = C.header[C.slot].addr;
    p.pad = level | 0x10000u; // (cloth.vert: the level's matrix and this job's rectangle)
    uint32_t n = 0;
    for (uint32_t i = 0; i < C.ncloth; i++) {
        if (!C.shadow[i]) continue;
        p.pass = i;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        vkCmdDraw(cmd, C.ntri[i] ? 3u * C.ntri[i] : 6u * (C.sizes[i][0] - 1u) * (C.sizes[i][1] - 1u), 1, 0, 0);
        n++;
    }
    return n;
}

uint32_t cloth_count(void) { // (the sheets: a soft body draws as its mesh, the renderer's skin)
    uint32_t k = 0;
    for (uint32_t i = 0; i < C.ncloth; i++) k += !C.soft[i];
    return k;
}
uint32_t cloth_draw(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout, VkPipeline pipe) {
    if (!C.ncloth || !pipe) return 0;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    Push p = *push;
    p.instances = C.header[C.slot].addr;
    p.pad &= ~0x10000u; // (not the shadow's)
    uint32_t n = 0;
    for (uint32_t i = 0; i < C.ncloth; i++) {
        if (C.soft[i]) continue;
        p.pass = i;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        vkCmdDraw(cmd, C.ntri[i] ? 3u * C.ntri[i] : 6u * (C.sizes[i][0] - 1u) * (C.sizes[i][1] - 1u), 1, 0, 0);
        n++;
    }
    return n;
}

uint32_t cloth_soft_readback(Entity e, float* out, uint32_t cap, const SoftTopo** topo) {
    uint32_t n = MAX_CLOTH;
    for (uint32_t i = 0; i < C.ncloth && n == MAX_CLOTH; i++)
        if (C.soft[i] && C.ents[i] == e) n = i;
    if (n == MAX_CLOTH || !C.vk) return 0;
    const SoftTopo* st = &C.stopo[n];
    uint32_t cnt = st->nv < cap ? st->nv : cap;
    if (topo) *topo = st;
    GpuBuf hb = { 0 };
    if (!cnt || !gpu_buf_create(C.vk, &hb, 16u * (VkDeviceSize)cnt, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return 0;
    vkDeviceWaitIdle(C.vk->device);
    VkCommandBuffer cmd = gpu_begin_once(C.vk);
    VkBufferCopy r = { 16u * (VkDeviceSize)C.cinfo[n].base, 0, 16u * (VkDeviceSize)cnt };
    vkCmdCopyBuffer(cmd, C.pos.buffer, hb.buffer, 1, &r);
    gpu_end_once(C.vk, cmd);
    memcpy(out, hb.mapped, 16u * cnt);
    gpu_buf_destroy(C.vk, &hb);
    return cnt;
}
