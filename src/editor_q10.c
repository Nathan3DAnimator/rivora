// (Q10, 2026-11-18) The test scenes of the realistic ragdoll's work in the game (notas-dev/ikrig.md):
//   --scene socket_offset   a Mannequin running in a circle with a sword on a socket of its right hand (the socket's
//                           offset and turn, the Skeleton tab's) and a lantern actor hung on its pelvis by Blueprint's
//                           Attach Actor To Socket: each frame both must sit exactly at bone x socket x their offset
//   --scene cloth_on_body   the same runner (MM_Run, a Physics Asset made from its bones) wearing a skirt (a mesh
//                           cloth, the Skirt preset, pinned at the waist) and a cape (a grid, the Cape preset): no
//                           vertex may sink into the body's capsules while it runs
//   --scene ragdoll_q10     N Mannequins (REALENGINE_Q10_N, 50) as Active Ragdolls taking hits, for the measures
//   --scene getup_q10       (2026-11-19) one Mannequin as an Active Ragdoll knocked forward, then backward: it must get up
//                           by the real get-up montages (AM_GetUp_Front / _Back, anim_getup.inc) - the right one for how it
//                           lies, no jump where the ragdoll hands over to the montage, the feet never under the floor, up
//                           where it lay, facing the right way
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "cloth.h"
#include "components.h"
#include "content.h"
#include "ecs.h"
#include "editor.h"
#include "ikrig.h"
#include "mesh_build.h"
#include "platform_files.h"
#include "render.h"
#include "vkctx.h"

#define GU_FALLS 4 // (getup_q10: forward, backward, forward, backward)
static struct {
    Entity runner, sword, lantern, skirt, cape, ground;
    Entity crowd[64];
    int ncrowd, hits;
    double t;
    float speed;
    int sock;                 // the socket made on hand_r
    float worst_sock, sock_dist, worst_lantern;
    int sock_frames, attached_frames;
    float cloth_depth[2];     // the deepest a vertex went into a body (cm): the skirt, the cape
    double cloth_inside[2];   // vertices inside by more than 1 cm, summed over the checks
    double cloth_verts[2];
    int cloth_through[2];     // vertices past half a body's radius (through it, not grazing it)
    int cloth_checks;
    bool cloth_mode, sock_mode, crowd_mode, lantern_on;
    // (getup_q10) per get-up: 0 face down (the forward hit), 1 on the back
    bool getup_mode;
    int gu_phase;              // 0 standing before the hit, 1 hit / down, 2 getting up (montage), 3 done
    int gu_round;              // which fall: even forward, odd backward
    double gu_t0, gu_hit_t, gu_last_t, gu_stand_t;
    int gu_face[GU_FALLS];            // phys_face_down when it started getting up
    char gu_montage[GU_FALLS][32];    // the montage that played
    float gu_heading[GU_FALLS];       // the lying body's heading (pelvis to chest, degrees) when it started getting up
    float gu_lie[GU_FALLS][3];        // the pelvis then
    float gu_jump[GU_FALLS];          // the fastest a bone moved in the hand-over (the first second of getting up), cm/s
    float gu_jump_cm[GU_FALLS];       //   and the most in one pose
    float gu_speed[GU_FALLS];         // the fastest during the whole get up
    float gu_foot[GU_FALLS];          // the lowest point of a sole's middle line once the montage plays (cm, the floor at 0)
    float gu_foot_lie[GU_FALLS];      //   and before, the ragdoll lying and handing over (its capsules against the ground)
    float gu_end_pelvis[GU_FALLS], gu_end_chest[GU_FALLS], gu_end_yaw[GU_FALLS], gu_end_dist[GU_FALLS];
    double gu_ms[GU_FALLS], gu_anim_ms[GU_FALLS];
    int gu_frames[GU_FALLS];
    float gu_prev[64][3];
    bool gu_have_prev;
    Entity clip;               // a second Mannequin without physics playing the two montages alone (the clips' own check)
    int clip_k;                // which it plays: 0 back, 1 front, 2 done
    float clip_foot[2];        // the lowest point of its soles in each
    bool clip_on;
} Q;

static Entity actor(World* w, const char* name, uint64_t comps) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | comps);
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE;
    return e;
}

static Entity floor_plane(World* w) {
    Entity e = actor(w, "Floor", COMP_BIT(COMP_STATIC_MESH));
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->scale[0] = t->scale[1] = 40.0f, t->flags = MOBILITY_STATIC;
    CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    ecs_component_defaults(COMP_STATIC_MESH, sm);
    sm->mesh = SHAPE_PLANE, sm->base_color[0] = sm->base_color[1] = sm->base_color[2] = 0.5f, sm->roughness = 0.8f;
    return e;
}

// The runner: the Mannequin playing MM_Run, a Physics Asset made from its bones (its capsules: the cloth's colliders)
static Entity runner(World* w, const char* name, float x, float y, int simulate) {
    Entity e = actor(w, name, COMP_BIT(COMP_SKELETAL_MESH));
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y;
    CSkeletalMesh* c = ecs_get(w, e, COMP_SKELETAL_MESH);
    const SkelMesh* m = anim_mesh_name("SK_Mannequin");
    static PhysicsAsset* pa;
    if (m && (!pa || pa->skeleton != m->hash)) pa = phys_temp(m->hash, "PHYS_Q10_Mannequin"); // (another project: its own Mannequin)
    const AnimSeq* run = anim_seq_name(simulate ? "MM_Idle" : "MM_Run");
    c->mesh = m ? m->hash : 0, c->anim = run ? run->hash : 0, c->play_rate = 1.0f, c->loop = 1, c->physics_blend = 1.0f;
    c->physics_asset = pa ? pa->hash : 0, c->simulate = (uint32_t)simulate;
    return e;
}

// The skirt's mesh: a flared tube (17 cm around the waist, 36 at the hem, 58 long), written once into the project
static uint32_t skirt_mesh(Editor* ed) {
    char path[1100];
    snprintf(path, sizeof path, "%s/Meshes/SM_Q10Skirt.rmesh", content_root());
    uint32_t id = render_mesh_id_of("/Content/Meshes/SM_Q10Skirt.rmesh");
    if (id && render_mesh_slot(id) >= SHAPE_COUNT) return id;
    enum { SEG = 48, RING = 16 };
    GpuVertex* v = calloc((SEG + 1) * (RING + 1), sizeof(GpuVertex));
    uint32_t* idx = malloc(sizeof(uint32_t) * SEG * RING * 6);
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/Meshes", content_root());
    re_make_dirs(dir);
    if (v && idx) {
        uint32_t ni = 0;
        for (int r = 0; r <= RING; r++)
            for (int s = 0; s <= SEG; s++) {
                GpuVertex* g = &v[r * (SEG + 1) + s];
                float a = 6.2831853f * (float)s / SEG, f = (float)r / RING, rad = 17.0f + 19.0f * f * f * 0.6f + 19.0f * f * 0.4f;
                g->pos[0] = rad * cosf(a), g->pos[1] = rad * sinf(a), g->pos[2] = -58.0f * f;
                g->uv[0] = (float)s / SEG, g->uv[1] = f, g->uv2[0] = g->uv2[1] = -1.0f;
            }
        for (int r = 0; r < RING; r++)
            for (int s = 0; s < SEG; s++) {
                uint32_t a = (uint32_t)(r * (SEG + 1) + s), b = a + 1, c = a + SEG + 1, d = c + 1;
                idx[ni++] = a, idx[ni++] = c, idx[ni++] = b, idx[ni++] = b, idx[ni++] = c, idx[ni++] = d;
            }
        MeshBuilt mb;
        if (mesh_build_indexed(v, (SEG + 1) * (RING + 1), idx, ni, false, false, &mb)) {
            if (!rmesh_write(path, &mb)) printf("[RealEngine] FAIL: cloth_on_body: the skirt not written\n");
            mesh_built_free(&mb);
        }
    }
    free(v), free(idx);
    render_meshes_reload(ed->vk);
    return render_mesh_id_of("/Content/Meshes/SM_Q10Skirt.rmesh");
}

static Entity garment(World* w, Editor* ed, const char* name, int preset, const char* bone) {
    Entity e = actor(w, name, COMP_BIT(COMP_CLOTH));
    CCloth* c = ecs_get(w, e, COMP_CLOTH);
    ecs_component_defaults(COMP_CLOTH, c);
    cloth_apply_preset(c, preset);
    snprintf(c->follow_actor, sizeof c->follow_actor, "Runner"), snprintf(c->follow_bone, sizeof c->follow_bone, "%s", bone);
    if (preset == CLOTH_SKIRT) {
        c->mesh = skirt_mesh(ed), c->pin_band = 2.0f, c->follow_offset[2] = 8.0f, c->follow_turn = 0;
        c->color[0] = 0.12f, c->color[1] = 0.2f, c->color[2] = 0.55f;
        if (render_mesh_slot(c->mesh) < SHAPE_COUNT) printf("[RealEngine] FAIL: cloth_on_body: the skirt mesh not loaded\n");
    } else { // (a cape: a grid hanging from the shoulders behind the back)
        c->width = 52.0f, c->height = 95.0f, c->res_x = 20, c->res_y = 34, c->pin = CLOTH_PIN_TOP;
        c->follow_offset[0] = 0.0f, c->follow_offset[1] = -14.0f, c->follow_offset[2] = 8.0f;
    }
    return e;
}

static void q10_spawn(Editor* ed, const char* name);
static char g_q10_name[32];
void editor_q10_scene(Editor* ed, const char* name) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    snprintf(g_q10_name, sizeof g_q10_name, "%s", name);
    memset(&Q, 0, sizeof Q);
    if (ed->pending_project[0]) return; // (a project opening: made in its level once it is open - its Content, not the build's)
    q10_spawn(ed, name);
}
static bool runner_here(World* w) { // (the scene's runner in this world: a level opened since has another entity at its slot)
    const CName* n = ecs_alive(w, Q.runner) ? ecs_get(w, Q.runner, COMP_NAME) : NULL;
    return n && !strcmp(n->text, "Runner");
}
// (made again into the world there is: with --new-project the project opens after the scene and replaces it)
static void q10_spawn(Editor* ed, const char* name) {
    World* w = &ed->world;
    memset(&Q, 0, sizeof Q);
    Q.sock = -1, Q.speed = 380.0f;
    Q.ground = floor_plane(w);
    Q.sock_mode = !strcmp(name, "socket_offset"), Q.cloth_mode = !strcmp(name, "cloth_on_body"), Q.crowd_mode = !strcmp(name, "ragdoll_q10");
    if (Q.crowd_mode) {
        const char* env = getenv("REALENGINE_Q10_N");
        Q.ncrowd = env ? atoi(env) : 50;
        if (Q.ncrowd < 1) Q.ncrowd = 1;
        if (Q.ncrowd > 64) Q.ncrowd = 64;
        for (int i = 0; i < Q.ncrowd; i++) {
            char nm[32];
            snprintf(nm, sizeof nm, "Ragdoll%d", i);
            Q.crowd[i] = runner(w, nm, (float)(i % 10) * 160.0f - 720.0f, (float)(i / 10) * 220.0f - 440.0f, PHYS_ACTIVE);
            ((CTransform*)ecs_get(w, Q.crowd[i], COMP_TRANSFORM))->rotation[1] = (float)(i * 37 % 360);
        }
        ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -1500.0f, ed->cam.pos[2] = 500.0f, ed->cam.yaw = 90.0f, ed->cam.pitch = -18.0f;
        editor_sky_preset(ed, 0, 0.0);
        return;
    }
    Q.getup_mode = !strcmp(name, "getup_q10");
    if (Q.getup_mode) {
        Q.runner = runner(w, "Runner", 0.0f, 0.0f, PHYS_ACTIVE);
        ((CSkeletalMesh*)ecs_get(w, Q.runner, COMP_SKELETAL_MESH))->physics_asset = 0; // (its own Physics Asset, made now: the project's montages exist)
        const SkelMesh* m = anim_mesh_name("SK_Mannequin");
        PhysicsAsset* pa = m ? phys_temp(m->hash, "PHYS_Q10_GetUp") : NULL;
        ((CSkeletalMesh*)ecs_get(w, Q.runner, COMP_SKELETAL_MESH))->physics_asset = pa ? pa->hash : 0;
        if (!pa || !pa->getup_front[0] || !pa->getup_back[0])
            printf("[RealEngine] FAIL: getup_q10: the Physics Asset made for the Mannequin has no get-up montages (front '%s', back '%s')\n", pa ? pa->getup_front : "-", pa ? pa->getup_back : "-");
        Q.clip = runner(w, "ClipCheck", 0.0f, 400.0f, 0);
        CSkeletalMesh* cc = ecs_get(w, Q.clip, COMP_SKELETAL_MESH);
        const AnimSeq* idle = anim_seq_name("MM_Idle");
        cc->physics_asset = 0, cc->simulate = 0, cc->anim = idle ? idle->hash : 0;
        Q.clip_foot[0] = Q.clip_foot[1] = 1e9f;
        editor_sky_preset(ed, 0, 0.0);
        ed->cam.pos[0] = 200.0f, ed->cam.pos[1] = -470.0f, ed->cam.pos[2] = 140.0f, ed->cam.yaw = 90.0f, ed->cam.pitch = -12.0f; // (where the falls land: the screenshots)
        return;
    }
    Q.runner = runner(w, "Runner", 300.0f, 0.0f, 0);
    if (Q.sock_mode) {
        // a socket on the right hand, 12 cm along the bone and 3 off it, turned 90 deg around X: the sword's grip
        SkelMesh* m = (SkelMesh*)anim_mesh_name("SK_Mannequin");
        int hand = -1;
        for (uint32_t b = 0; m && b < m->nbones; b++)
            if (!strcmp(m->bones[b].name, "hand_r")) hand = (int)b;
        int at = m && hand >= 0 ? anim_socket_index(m, "Q10_Grip") : -1;
        if (m && hand >= 0 && at < 0) {
            AnimSocket* k = anim_socket_add(m, hand);
            if (k) {
                snprintf(k->name, sizeof k->name, "Q10_Grip");
                k->local.t[0] = 3.0f, k->local.t[1] = 12.0f, k->local.t[2] = 2.0f;
                k->local.q[0] = 0.70710678f, k->local.q[3] = 0.70710678f; // (90 deg around X)
                at = (int)(k - m->sockets);
            }
            m->dirty = false; // (the test's: never saved)
        }
        Q.sock = at;
        Q.sword = actor(w, "Sword", COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_ATTACH));
        CStaticMesh* sm = ecs_get(w, Q.sword, COMP_STATIC_MESH);
        ecs_component_defaults(COMP_STATIC_MESH, sm);
        sm->mesh = SHAPE_CUBE, sm->base_color[0] = 0.8f, sm->base_color[1] = 0.8f, sm->base_color[2] = 0.85f, sm->metallic = 1.0f, sm->roughness = 0.3f;
        CTransform* st = ecs_get(w, Q.sword, COMP_TRANSFORM);
        st->flags = MOBILITY_MOVABLE | TRANSFORM_NO_COLLISION;
        CAttach* a = ecs_get(w, Q.sword, COMP_ATTACH);
        a->owner = Q.runner, a->parent = Q.runner, a->kind = ATTACH_SCS;
        a->location[2] = 30.0f, a->scale[0] = 0.04f, a->scale[1] = 0.04f, a->scale[2] = 0.7f; // (the blade along the socket's Z)
        Q.lantern = actor(w, "Lantern", COMP_BIT(COMP_STATIC_MESH));
        sm = ecs_get(w, Q.lantern, COMP_STATIC_MESH);
        ecs_component_defaults(COMP_STATIC_MESH, sm);
        sm->mesh = SHAPE_CYLINDER, sm->base_color[0] = 0.9f, sm->base_color[1] = 0.6f, sm->base_color[2] = 0.2f;
        CTransform* lt = ecs_get(w, Q.lantern, COMP_TRANSFORM);
        lt->scale[0] = lt->scale[1] = lt->scale[2] = 0.12f, lt->flags = MOBILITY_MOVABLE | TRANSFORM_NO_COLLISION;
    }
    if (Q.cloth_mode) {
        Q.skirt = garment(w, ed, "Skirt", CLOTH_SKIRT, "pelvis");
        Q.cape = garment(w, ed, "Cape", CLOTH_CAPE, "spine_03");
    }
    editor_sky_preset(ed, 0, 0.0);
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -650.0f, ed->cam.pos[2] = 260.0f, ed->cam.yaw = 90.0f, ed->cam.pitch = -18.0f;
}

static void rows_of(const CTransform* t, float r[3][4]) { render_transform_rows(t, r); }
static void mul_rows(const float a[3][4], const float b[3][4], float o[3][4]) {
    float r[3][4];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0.0f);
    memcpy(o, r, sizeof r);
}

// The checks of a frame (after the editor's frame posed them): the sockets' places, the cloths against the body
static void check_frame(Editor* ed) {
    World* w = &ed->world;
    if (Q.sock_mode && Q.sock >= 0 && Q.attached_frames > 4) { // (a few poses after both were hung: the kept pose is this frame's)
        const SkelMesh* m = anim_mesh_name("SK_Mannequin");
        float bone[3][4], sock[3][4], loc[3][4], want[3][4], got[3][4];
        if (m && anim_socket_world(w, Q.runner, "hand_r", bone)) {
            anim_xf_rows(&m->sockets[Q.sock].local, (const float(*)[4])bone, sock);
            const CAttach* a = ecs_get(w, Q.sword, COMP_ATTACH);
            CTransform l = { { a->location[0], a->location[1], a->location[2] }, { a->rotation[0], a->rotation[1], a->rotation[2] }, { a->scale[0], a->scale[1], a->scale[2] }, 0 };
            rows_of(&l, loc);
            mul_rows(sock, loc, want);
            rows_of(ecs_get(w, Q.sword, COMP_TRANSFORM), got);
            float err = 0.0f;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 4; j++) err = fmaxf(err, fabsf(want[i][j] - got[i][j]) * (j == 3 ? 1.0f : 30.0f)); // (a turn's error x 30 cm)
            Q.worst_sock = fmaxf(Q.worst_sock, err);
            float d[3] = { sock[0][3] - bone[0][3], sock[1][3] - bone[1][3], sock[2][3] - bone[2][3] };
            Q.sock_dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (getenv("REALENGINE_Q10_DEBUG") && err > 0.05f) printf("[RealEngine] dbg: frame %d (t %.3f) sword off %.2f\n", Q.sock_frames, Q.t, err);
            Q.sock_frames++;
        }
        float pel[3][4];
        if (anim_socket_world(w, Q.runner, "pelvis", pel)) { // the lantern: 15 cm out from the pelvis along its X
            const CTransform* lt = ecs_get(w, Q.lantern, COMP_TRANSFORM);
            float wx = pel[0][3] + pel[0][0] * 15.0f, wy = pel[1][3] + pel[1][0] * 15.0f, wz = pel[2][3] + pel[2][0] * 15.0f;
            float e = sqrtf((lt->location[0] - wx) * (lt->location[0] - wx) + (lt->location[1] - wy) * (lt->location[1] - wy) + (lt->location[2] - wz) * (lt->location[2] - wz));
            Q.worst_lantern = fmaxf(Q.worst_lantern, e);
            if (getenv("REALENGINE_Q10_DEBUG") && e > 0.05f)
                printf("[RealEngine] dbg: lantern %.1f %.1f %.1f want %.1f %.1f %.1f; sword %.1f %.1f %.1f want %.1f %.1f %.1f\n", lt->location[0], lt->location[1], lt->location[2], wx, wy, wz,
                       ((CTransform*)ecs_get(w, Q.sword, COMP_TRANSFORM))->location[0], ((CTransform*)ecs_get(w, Q.sword, COMP_TRANSFORM))->location[1], ((CTransform*)ecs_get(w, Q.sword, COMP_TRANSFORM))->location[2], want[0][3], want[1][3], want[2][3]);
        }
    }
    if (Q.cloth_mode && Q.t > 1.5 && ((int)(Q.t * 60.0) % 6) == 0) {
        // (the GPU done: the vertices are the last frame's, simulated against the capsules published by its pose - this
        // frame's pose runs after this check)
        vkDeviceWaitIdle(ed->vk->device);
        const CTransform* rt = ecs_get(w, Q.runner, COMP_TRANSFORM);
        PhysCapsule caps[64];
        uint32_t nc = rt ? phys_capsules_near(rt->location, 250.0f, caps, 64) : 0;
        const CSkeletalMesh* sk = ecs_get(w, anim_mesh_entity(w, Q.runner), COMP_SKELETAL_MESH);
        const PhysicsAsset* pa = sk ? phys_find(sk->physics_asset) : NULL;
        for (int g = 0; g < 2; g++) {
            const float* pos;
            uint32_t n = cloth_debug_vertices(g ? Q.cape : Q.skirt, &pos);
            if (!n) continue;
            const char* skip = g == 0 ? "pelvis" : "spine_03";
            uint32_t inside = 0;
            // (the pinned ones left out: they sit where the bone carries them, the collar inside the neck by design - the
            // cape's top row, the skirt's waist band: its top 2.5 cm)
            float ztop = -1e30f;
            for (uint32_t v = 0; v < n; v++) ztop = fmaxf(ztop, pos[4 * v + 2] * 100.0f);
            uint32_t counted = 0;
            for (uint32_t v = g ? 20u : 0u; v < n; v++) {
                float p[3] = { pos[4 * v] * 100.0f, pos[4 * v + 1] * 100.0f, pos[4 * v + 2] * 100.0f };
                if (g == 0 && p[2] > ztop - 2.5f) continue;
                counted++;
                for (uint32_t k = 0; k < nc; k++) {
                    if (pa && caps[k].body >= 0 && caps[k].body < (int)pa->nbodies && !strcmp(pa->bodies[caps[k].body].bone, skip)) continue;
                    float ab[3] = { caps[k].b[0] - caps[k].a[0], caps[k].b[1] - caps[k].a[1], caps[k].b[2] - caps[k].a[2] };
                    float ap[3] = { p[0] - caps[k].a[0], p[1] - caps[k].a[1], p[2] - caps[k].a[2] };
                    float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2], tt = l2 > 1e-6f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2 : 0.0f;
                    tt = fminf(fmaxf(tt, 0.0f), 1.0f);
                    float q[3] = { ap[0] - ab[0] * tt, ap[1] - ab[1] * tt, ap[2] - ab[2] * tt };
                    float depth = caps[k].r - sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
                    if (depth > Q.cloth_depth[g]) Q.cloth_depth[g] = depth;
                    if (depth > 0.5f * caps[k].r) Q.cloth_through[g]++;
                    if (depth > 1.0f) {
                        inside++;
                        break;
                    }
                }
            }
            Q.cloth_inside[g] += inside, Q.cloth_verts[g] += counted;
        }
        Q.cloth_checks++;
    }
}

// (getup_q10) the actor's heading (its X axis on the ground, degrees)
static float actor_heading(const CTransform* t) {
    float r[3][4];
    render_transform_rows(t, r);
    return atan2f(r[1][0], r[0][0]) * 57.29578f;
}
static float wrap180(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}
// the lowest point of an actor's soles in its last pose (cm over its feet's level), 1e9 when it has no pose yet
static float soles_low(World* w, Entity actor) {
    const CTransform* t = ecs_get(w, actor, COMP_TRANSFORM);
    float lo = 1e9f;
    static const char* const feet[2] = { "foot_l", "foot_r" };
    static const float sole[3][3] = { { 18, 0, -8.5f }, { 5, 0, -8.5f }, { -8, 0, -8.5f } };
    for (int f = 0; f < 2 && t; f++) {
        float rows[3][4];
        if (!anim_socket_world(w, actor, feet[f], rows)) return 1e9f;
        for (int c = 0; c < 3; c++) lo = fminf(lo, rows[2][0] * sole[c][0] + rows[2][1] * sole[c][1] + rows[2][2] * sole[c][2] + rows[2][3] - t->location[2]);
    }
    return lo;
}
static void clip_tick(Editor* ed) {
    World* w = &ed->world;
    if (Q.clip_k > 1 || !ecs_alive(w, Q.clip)) return;
    static const char* const mo[2] = { "AM_GetUp_Back", "AM_GetUp_Front" };
    float mt = 0.0f;
    const char* playing = anim_montage_playing(w, Q.clip, &mt);
    if (!Q.clip_on) {
        if (Q.t > 0.5 && anim_play_montage(w, Q.clip, mo[Q.clip_k], 1.0f)) Q.clip_on = true;
        return;
    }
    if (playing && mt > 0.25f) Q.clip_foot[Q.clip_k] = fminf(Q.clip_foot[Q.clip_k], soles_low(w, Q.clip)); // (blended in from the idle)
    if (!playing) Q.clip_on = false, Q.clip_k++;
}
static void getup_tick(Editor* ed) {
    World* w = &ed->world;
    clip_tick(ed);
    Entity sk = anim_mesh_entity(w, Q.runner);
    const SkelMesh* m = anim_mesh_name("SK_Mannequin");
    CTransform* t = ecs_get(w, Q.runner, COMP_TRANSFORM);
    if (!m || !t || Q.gu_round >= GU_FALLS) return;
    int r = Q.gu_round, bal = phys_balance_state(sk);
    // the last frame's pose: every bone's place, the soles' corners
    float pos[64][3], foot_lo = 1e9f;
    uint32_t nb = m->nbones < 64 ? m->nbones : 64;
    int pelvis = -1, chest = -1;
    bool ok = nb > 0;
    for (uint32_t b = 0; b < nb && ok; b++) {
        float rows[3][4];
        if (!anim_socket_world(w, Q.runner, m->bones[b].name, rows)) {
            ok = false;
            break;
        }
        for (int k = 0; k < 3; k++) pos[b][k] = rows[k][3];
        if (!strcmp(m->bones[b].name, "pelvis")) pelvis = (int)b;
        if (!strcmp(m->bones[b].name, "spine_03")) chest = (int)b;
        if (!strcmp(m->bones[b].name, "foot_l") || !strcmp(m->bones[b].name, "foot_r")) {
            // (the sole's middle line, heel to toe, of the foot's box (anim.c): its corners stick out of any capsule)
            static const float sole[3][3] = { { 18, 0, -8.5f }, { 5, 0, -8.5f }, { -8, 0, -8.5f } };
            for (int c = 0; c < 3; c++) foot_lo = fminf(foot_lo, rows[2][0] * sole[c][0] + rows[2][1] * sole[c][1] + rows[2][2] * sole[c][2] + rows[2][3]);
        }
    }
    if (!ok || pelvis < 0 || chest < 0) return;
    if (Q.gu_phase == 2) { // getting up: the fastest bone between two poses (a jump), the soles, the cost
        float worst = 0.0f;
        int wb = -1;
        for (uint32_t b = 0; Q.gu_have_prev && b < nb; b++) {
            if (m->bones[b].parent < 0) continue; // (the root: the actor's place, nothing drawn on it - the actor moves under the body)
            float dx = pos[b][0] - Q.gu_prev[b][0], dy = pos[b][1] - Q.gu_prev[b][1], dz = pos[b][2] - Q.gu_prev[b][2], d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > worst) worst = d, wb = (int)b;
        }
        if (!Q.gu_have_prev || worst > 1e-4f) { // (a new pose)
            if (Q.gu_have_prev && Q.t > Q.gu_last_t) {
                float v = worst / (float)(Q.t - Q.gu_last_t);
                Q.gu_speed[r] = fmaxf(Q.gu_speed[r], v);
                if (Q.t - Q.gu_t0 < 1.0) Q.gu_jump[r] = fmaxf(Q.gu_jump[r], v), Q.gu_jump_cm[r] = fmaxf(Q.gu_jump_cm[r], worst);
                if (getenv("REALENGINE_Q10_DEBUG") && v > 300.0f)
                    printf("[RealEngine] dbg: getup %d: t %.2f, %s %.1f cm in %.4f s (%.0f cm/s), state %d\n", r, Q.t - Q.gu_t0, wb >= 0 ? m->bones[wb].name : "-", worst, Q.t - Q.gu_last_t, v, bal);
            }
            memcpy(Q.gu_prev, pos, sizeof(float) * 3 * nb);
            Q.gu_have_prev = true, Q.gu_last_t = Q.t;
        }
        float mt = 0.0f;
        bool clip = anim_montage_playing(w, Q.runner, &mt) && mt > 0.55f; // (the hand-over: 0.5 s from the alignment, where the montage starts)
        float* lo = clip ? &Q.gu_foot[r] : &Q.gu_foot_lie[r];
        if (getenv("REALENGINE_Q10_DEBUG") && foot_lo - t->location[2] < fminf(*lo, -1.0f))
            printf("[RealEngine] dbg: getup %d: t %.2f, a sole %.1f cm (state %d, %s)\n", r, Q.t - Q.gu_t0, foot_lo - t->location[2], bal, clip ? "montage" : "lying");
        *lo = fminf(*lo, foot_lo - t->location[2]);
        Q.gu_ms[r] += ikphys_stats()->ms, Q.gu_anim_ms[r] += anim_stats()->ms, Q.gu_frames[r]++;
        const char* mo = anim_montage_playing(w, Q.runner, NULL);
        if (mo && !Q.gu_montage[r][0]) snprintf(Q.gu_montage[r], sizeof Q.gu_montage[r], "%s", mo);
    }
    switch (Q.gu_phase) {
    case 0: // standing a second, then a strong hit forward (the first time) or backward
        if (Q.t - Q.gu_t0 < 1.0) break;
        {
            float a = actor_heading(t) * 0.017453293f, sg = (r & 1) == 0 ? 1.0f : -1.0f;
            float imp[3] = { cosf(a) * 1000.0f * sg, sinf(a) * 1000.0f * sg, 100.0f };
            phys_hit_reaction(sk, "spine_03", imp, 0.1f, 0.8f);
        }
        Q.gu_phase = 1, Q.gu_hit_t = Q.t;
        break;
    case 1: // down: the moment it starts getting up, how it lies
        if (getenv("REALENGINE_Q10_DEBUG") && fmod(Q.t - Q.gu_hit_t, 0.1) < 0.006) {
            float cr[3][4];
            if (anim_socket_world(w, Q.runner, "spine_03", cr))
                printf("[RealEngine] dbg: fall %d: t %.2f, state %d, chest front z %.2f, chest %.0f %.0f %.0f\n", r, Q.t - Q.gu_hit_t, bal, cr[2][0], cr[0][3], cr[1][3], cr[2][3]);
        }
        if (bal == 3) {
            Q.gu_face[r] = phys_face_down(sk);
            memcpy(Q.gu_lie[r], pos[pelvis], 12);
            Q.gu_heading[r] = atan2f(pos[chest][1] - pos[pelvis][1], pos[chest][0] - pos[pelvis][0]) * 57.29578f;
            Q.gu_phase = 2, Q.gu_t0 = Q.t, Q.gu_have_prev = false, Q.gu_foot[r] = Q.gu_foot_lie[r] = 1e9f, Q.gu_stand_t = 0.0;
            if (getenv("REALENGINE_Q10_DEBUG"))
                printf("[RealEngine] dbg: getup %d starts: face %d, heading %.0f (actor %.0f), pelvis %.0f %.0f %.0f\n", r, Q.gu_face[r], Q.gu_heading[r], actor_heading(t), pos[pelvis][0], pos[pelvis][1], pos[pelvis][2]);
        } else if (Q.t - Q.gu_hit_t > 14.0) {
            printf("[RealEngine] FAIL: getup_q10: fall %d never started getting up (state %d)\n", r, bal);
            Q.gu_round++, Q.gu_phase = 0, Q.gu_t0 = Q.t;
        }
        break;
    case 2: // up when the montage is over and it stands: how it ended
        if (bal == 0 && !anim_montage_playing(w, Q.runner, NULL) && Q.t - Q.gu_t0 > 0.6) {
            if (Q.gu_stand_t == 0.0) Q.gu_stand_t = Q.t;
            if (Q.t - Q.gu_stand_t < 0.5) break;
            Q.gu_end_pelvis[r] = pos[pelvis][2] - t->location[2];
            Q.gu_end_chest[r] = pos[chest][2] - pos[pelvis][2];
            float want = Q.gu_heading[r] + (Q.gu_face[r] == 1 ? 0.0f : 180.0f); // (face down: it rises toward its head; on its back: toward its feet)
            Q.gu_end_yaw[r] = fabsf(wrap180(actor_heading(t) - want));
            Q.gu_end_dist[r] = sqrtf((t->location[0] - Q.gu_lie[r][0]) * (t->location[0] - Q.gu_lie[r][0]) + (t->location[1] - Q.gu_lie[r][1]) * (t->location[1] - Q.gu_lie[r][1]));
            Q.gu_round++, Q.gu_phase = 0, Q.gu_t0 = Q.t;
        } else
            Q.gu_stand_t = 0.0;
        if (Q.gu_phase == 2 && Q.t - Q.gu_t0 > 12.0) {
            printf("[RealEngine] FAIL: getup_q10: get up %d never ended (state %d)\n", r, bal);
            Q.gu_round++, Q.gu_phase = 0, Q.gu_t0 = Q.t;
        }
        break;
    }
}

void editor_q10_tick(Editor* ed, double dt) {
    if (!ed->playing) return;
    World* w = &ed->world;
    if (!Q.crowd_mode && !runner_here(w) && (!strcmp(g_q10_name, "socket_offset") || !strcmp(g_q10_name, "cloth_on_body") || !strcmp(g_q10_name, "getup_q10"))) { // (the project's level came in over the scene: made in it)
        q10_spawn(ed, g_q10_name);
        return;
    }
    check_frame(ed); // (the last frame's poses and cloths)
    if (Q.getup_mode) getup_tick(ed);
    Q.t += dt;
    if (Q.getup_mode) return;
    if (Q.crowd_mode) { // one hit every 0.4 s, light and strong in turn
        int k = Q.t > 1.0 ? (int)((Q.t - 1.0) / 0.4) : -1;
        while (Q.hits <= k && Q.hits < 6 * Q.ncrowd) {
            int i = (Q.hits * 7) % Q.ncrowd;
            bool strong = Q.hits % 2 == 1;
            float a = (float)(Q.hits % 4) * 1.5707963f, imp[3] = { cosf(a) * (strong ? 1000.0f : 160.0f), sinf(a) * (strong ? 1000.0f : 160.0f), strong ? 100.0f : 0.0f };
            phys_hit_reaction(anim_mesh_entity(w, Q.crowd[i]), "spine_03", imp, strong ? 0.1f : 0.5f, 0.8f);
            Q.hits++;
        }
        return;
    }
    // the runner: a circle of 3 m at a run (3.8 m/s), its yaw along it
    CTransform* t = ecs_get(w, Q.runner, COMP_TRANSFORM);
    if (!t) return;
    float a = (float)Q.t * Q.speed / 300.0f;
    t->location[0] = 300.0f * cosf(a), t->location[1] = 300.0f * sinf(a), t->location[2] = 0.0f;
    t->rotation[1] = a * 57.29578f + 90.0f;
    ecs_touch(w, Q.runner);
    if (Q.sock_mode && Q.t > 0.2 && Q.sock >= 0) { // (the first frames: the Mannequin's instance made)
        CAttach* at = ecs_get(w, Q.sword, COMP_ATTACH);
        if (at && at->kind != ATTACH_BONE && !anim_attach_to_socket(w, Q.sword, Q.runner, "Q10_Grip")) printf("[RealEngine] FAIL: socket_offset: the sword not attached\n");
        float keep[3][4];
        anim_socket_world(w, Q.runner, "hand_r", keep); // (its pose kept from the next frame on: the check's reference)
        if (!Q.lantern_on) Q.lantern_on = anim_attach_actor_to_socket(w, Q.lantern, Q.runner, "pelvis", (float[3]){ 15.0f, 0.0f, 0.0f }, (float[3]){ 0, 0, 0 });
        if (Q.lantern_on && at && at->kind == ATTACH_BONE) Q.attached_frames++;
    }
}

bool editor_q10_check(Editor* ed) {
    (void)ed;
    bool ok = true;
    if (Q.sock_mode) {
        printf("[RealEngine] socket_offset: %d frames: the sword %.3f cm from bone x socket x its offset at worst (the socket %.1f cm from the hand bone), the lantern %.3f cm from its place on the pelvis\n",
               Q.sock_frames, Q.worst_sock, Q.sock_dist, Q.worst_lantern);
        if (Q.sock_frames < 30) printf("[RealEngine] FAIL: socket_offset: only %d frames checked\n", Q.sock_frames), ok = false;
        if (Q.worst_sock > 0.05f) printf("[RealEngine] FAIL: socket_offset: the sword is %.2f cm off its socket\n", Q.worst_sock), ok = false;
        if (Q.sock_dist < 10.0f) printf("[RealEngine] FAIL: socket_offset: the socket's offset was not applied (%.1f cm)\n", Q.sock_dist), ok = false;
        if (Q.worst_lantern > 0.05f) printf("[RealEngine] FAIL: socket_offset: the lantern (Attach Actor To Socket) is %.2f cm off\n", Q.worst_lantern), ok = false;
    }
    if (Q.cloth_mode) {
        const ClothStats* cs = cloth_stats();
        for (int g = 0; g < 2; g++) {
            double frac = Q.cloth_verts[g] > 0.0 ? Q.cloth_inside[g] / Q.cloth_verts[g] : 1.0;
            printf("[RealEngine] cloth_on_body: the %s at a run: %d checks, the deepest vertex %.2f cm into the body, %.3f%% of the vertices over 1 cm in, %d past half a body's radius\n",
                   g ? "cape" : "skirt", Q.cloth_checks, Q.cloth_depth[g], frac * 100.0, Q.cloth_through[g]);
            if (Q.cloth_verts[g] <= 0.0) printf("[RealEngine] FAIL: cloth_on_body: the %s never simulated\n", g ? "cape" : "skirt"), ok = false;
            else if (frac > 0.0005 || Q.cloth_through[g] > 0) printf("[RealEngine] FAIL: cloth_on_body: the %s goes through the body\n", g ? "cape" : "skirt"), ok = false;
        }
        printf("[RealEngine] cloth_on_body: %u cloths, %u vertices, %u colliders, %.3f ms CPU\n", cs->cloths, cs->vertices, cs->colliders, cs->cpu_ms);
    }
    if (Q.getup_mode) {
        static const char* const want_mo[2] = { "AM_GetUp_Front", "AM_GetUp_Back" };
        int lay[2] = { 0, 0 }, as_hit = 0;
        for (int r = 0; r < GU_FALLS; r++) {
            const char* how = (r & 1) ? "knocked backward" : "knocked forward";
            if (r >= Q.gu_round) {
                printf("[RealEngine] FAIL: getup_q10: %s: never got up (%.1f s played)\n", how, Q.t);
                ok = false;
                continue;
            }
            double n = Q.gu_frames[r] > 0 ? (double)Q.gu_frames[r] : 1.0;
            printf("[RealEngine] getup_q10: %s, lying %s -> %s; hand-over: fastest bone %.0f cm/s (%.1f cm in a pose); whole get up: %.0f cm/s; lowest sole %.1f cm lying, %.1f cm in the montage; "
                   "up: pelvis %.1f cm, chest %.1f cm over it, facing %.1f deg off, %.0f cm from where it lay; %.3f ms physics + %.3f ms poses a frame (%d frames)\n",
                   how, Q.gu_face[r] == 1 ? "face down" : "on its back", Q.gu_montage[r][0] ? Q.gu_montage[r] : "(no montage)", Q.gu_jump[r], Q.gu_jump_cm[r], Q.gu_speed[r], Q.gu_foot_lie[r], Q.gu_foot[r],
                   Q.gu_end_pelvis[r], Q.gu_end_chest[r], Q.gu_end_yaw[r], Q.gu_end_dist[r], Q.gu_ms[r] / n, Q.gu_anim_ms[r] / n, Q.gu_frames[r]);
            if (r == 0) {
                printf("[RealEngine] getup_q10: the clips alone (no physics): lowest sole %.1f cm (AM_GetUp_Back), %.1f cm (AM_GetUp_Front)\n", Q.clip_foot[0], Q.clip_foot[1]);
                if (Q.clip_k < 2 || Q.clip_foot[0] < -1.5f || Q.clip_foot[1] < -1.5f) printf("[RealEngine] FAIL: getup_q10: a clip puts a foot under the floor (or did not play)\n"), ok = false;
            }
            lay[Q.gu_face[r] == 1 ? 0 : 1]++, as_hit += Q.gu_face[r] == ((r & 1) ? 0 : 1);
            if (strcmp(Q.gu_montage[r], want_mo[Q.gu_face[r] == 1 ? 0 : 1])) printf("[RealEngine] FAIL: getup_q10: %s: the montage played was '%s'\n", how, Q.gu_montage[r]), ok = false;
            // (a jump: a limb faster than 10 m/s - a pop of 17 cm in a 60 Hz frame; the blend from the lying ragdoll into the
            // clip's first pose moves a hand at ~5-7 m/s at most)
            if (Q.gu_jump[r] > 1000.0f) printf("[RealEngine] FAIL: getup_q10: %s: the body jumps where the ragdoll hands over to the montage\n", how), ok = false;
            if (Q.gu_speed[r] > 1500.0f) printf("[RealEngine] FAIL: getup_q10: %s: a bone pops while getting up\n", how), ok = false;
            // (lying: the ragdoll at rest - its ground 1 cm under the feet, the joints solved after the contacts - sinks a foot 2-4
            // cm (a ball at the ankle sank the toes 11 cm); in the montage: the clip, under 1.5 cm)
            if (Q.gu_foot_lie[r] < -5.0f || Q.gu_foot[r] < -1.5f)
                printf("[RealEngine] FAIL: getup_q10: %s: a foot goes under the floor (%.1f cm lying, %.1f cm in the montage)\n", how, -Q.gu_foot_lie[r], -Q.gu_foot[r]), ok = false;
            if (Q.gu_end_pelvis[r] < 85.0f || Q.gu_end_chest[r] < 35.0f) printf("[RealEngine] FAIL: getup_q10: %s: not standing at the end\n", how), ok = false;
            if (Q.gu_end_yaw[r] > 35.0f) printf("[RealEngine] FAIL: getup_q10: %s: it stood up facing the wrong way\n", how), ok = false;
            if (Q.gu_end_dist[r] > 150.0f) printf("[RealEngine] FAIL: getup_q10: %s: it stood up away from where it lay\n", how), ok = false;
        }
        // (pushed hard a ragdoll may roll over once down: each get up is checked against how it lies; both must be seen)
        printf("[RealEngine] getup_q10: %d falls: %d face down, %d on its back, %d lying the way the hit pushed it\n", Q.gu_round, lay[0], lay[1], as_hit);
        if (!lay[0] || !lay[1]) printf("[RealEngine] FAIL: getup_q10: only one of the two get-up montages was seen\n"), ok = false;
        if (as_hit < Q.gu_round / 2) printf("[RealEngine] FAIL: getup_q10: most falls didn't lie the way the hit pushed\n"), ok = false;
    }
    if (Q.crowd_mode) {
        const IkPhysStats* s = ikphys_stats();
        int counts[4] = { 0 };
        for (int i = 0; i < Q.ncrowd; i++) {
            int b = phys_balance_state(anim_mesh_entity(&ed->world, Q.crowd[i]));
            if (b >= 0 && b < 4) counts[b]++;
        }
        printf("[RealEngine] ragdoll_q10: %d Active Ragdolls, %d hits; now %d standing, %d stumbling, %d down, %d getting up; %u bodies, %u contacts, %.3f ms CPU (the pose jobs summed)\n",
               Q.ncrowd, Q.hits, counts[0], counts[1], counts[2], counts[3], s->bodies, s->contacts, s->ms);
    }
    return ok;
}
