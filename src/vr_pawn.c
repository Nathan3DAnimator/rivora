// (Q25) The VR pawn (vr_pawn.h, notas-dev/xr.md): the play space, locomotion and turning, the comfort vignette, the hands,
// the laser, grabbing, throwing and the sockets, the VR menu panel's pointer. The headset's data comes from xr.h (the
// simulated mode's from the tests). Engine space: X forward, Y right, Z up, cm.
#include "vr_pawn.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "components.h"
#include "gameplay_tags.h"
#include "interaction.h"
#include "landscape.h"
#include "physics.h"
#include "platform.h"
#include "project.h"
#include "render.h"
#include "xr.h"
#include "anim.h"
#include "vr_hand.h"

#define VR_DEG 0.017453292f
#define VR_ARC_DOTS 20
#define VR_FINGERS 5
#define VR_HAND_PARTS (1 + VR_FINGERS * 2) // the palm, two boxes a finger
#define VR_VEL_HIST 8
#define VR_GRAB_REACH 10.0f    // cm from the hand to a Grabbable's box
#define VR_SOCKET_REACH 20.0f  // cm from a released actor to a Grab Socket
#define VR_HAND_JUMP 15.0f     // cm a hand moves in one frame at most (more: the tracking lost and found it elsewhere)

typedef struct VrHandState {
    Entity held;
    bool held_body;             // it simulated physics before (it does again when let go)
    bool held_nocol;            // it had no collision before (kept so when let go)
    float rel_pos[3], rel_axes[3][3]; // the held actor in the hand's frame (snapped: identity)
    float hist[VR_VEL_HIST][3]; // the hand's world places, the last frames (the throw's speed)
    float hist_t[VR_VEL_HIST];
    int hist_n, hist_i;
    bool grip_was, trig_was;
    Entity parts[VR_HAND_PARTS];
    Entity laser;
    bool laser_hit;
    float laser_end[3];
    Entity laser_actor;
    float world_pos[3], world_axes[3][3]; // this frame's grip pose in the world (fwd, right, up)
    float real_pos[3];                    // ... where the real hand is (before the physical hand stops it): the drag's
    float aim_o[3], aim_d[3];             // the aim's ray in the world
    bool dragging;                        // (drag locomotion) the closed hand holds the world: the play space follows it
    float anchor[3];                      // ... where it closed (world)
    float local_prev[3], local_vel[3];    // the hand in the play space a frame ago, its speed there (the drag's fling)
    Entity poked;                         // the Interactable the index tip is in (used on entering)
    bool was_active;                      // seen last frame (a hand found again: its speed starts over)
    float solved[3];                      // (physical hands) where the drawn hand is: the real one's place, the walls allowing
    bool solved_ok;
    float shift[3];                       // ... solved minus the real place (the tracked joints drawn moved by it)
    Entity mesh_ent;                      // the project's own hand mesh (Project Settings > VR > Hand Meshes)
    // (2026-11-20) a Skeletal Mesh as the hand: its actor, its parts (anim.c's, found again now and then: shown / hidden
    // with it), whether it is shown; its bones as a hand (vr_hand.h)
    Entity sk_ent;
    Entity sk_parts[24];
    uint32_t sk_nparts;
    int sk_shown;                         // (-1: not yet)
    bool sk_rig_ok;
    bool jumped;                          // this frame it moved more than a hand can (a tracking glitch)
} VrHandState;

static struct {
    bool playing;
    World* w;
    Entity pawn;
    float origin[3], yaw;   // the play space: its origin on the floor, its yaw (degrees)
    float off[2];           // where the pawn stands in the play space (its feet; the room's walking leaves it, a teleport puts it under the head)
    float seat;             // cm added to the play space's heights (seated)
    int locomotion, turn;
    float snap_angle, turn_speed, move_speed, tp_range, vignette_max;
    bool hands, laser, hand_tracking, drag;
    float fling[2];         // (drag locomotion) the play space's glide after a drag, cm/s (slowing down)
    bool drag_move;         // the play space moved by a drag or its glide (the comfort vignette's)
    bool physical, hand_objects, push, held_collide, hide_body;
    int head_mode;          // VR_HEAD_*
    float fade, fade_want;  // the head in a wall: the view to black
    uint32_t frame;
#define VR_BODY_MAX 256
    Entity body[VR_BODY_MAX];   // the pawn's own meshes hidden in VR (find_body)
    uint32_t nbody;
    float safe[3], safe_time, fall_time; // the safety net: the last ground, how long since, how long falling
#define VR_SAFE_N 8
    float safes[VR_SAFE_N][3]; // ... the last firm places, a meter apart (a rescue soon after another goes further back)
    int nsafe, safe_head, rescue_depth;
    float since_rescue, start[3];
    bool start_ok;
    float last_origin[3];   // (the jump detector: the play space a frame ago)
    uint32_t teleports_seen;
    bool safe_ok;
    int dom;                // the dominant hand (1 right): the laser's and the turn's; the other moves and teleports
    float grab_reach, laser_len, haptics;
    float blink;            // a blink to black now (a teleport, a snap turn), going out
    VrHandState h[2];
    bool tp_aiming;
    int tp_hand;
    bool tp_valid;
    float tp_target[3];
    Entity arc[VR_ARC_DOTS], ring;
    bool snap_ready;
    float vignette, vig_turn;
    bool menu, menu_was, ui_hit, ui_down;
    float ui_uv[2];
    float time;
    VrStats st;
} V;

const VrStats* vr_pawn_stats(void) { return &V.st; }

// the project's VR settings, read every frame: Project Settings > VR at the start, the nodes and the console variables
// (vr.*) while playing write them there
static void sync_settings(void) {
    const ProjectVr* p = &project()->vr;
    V.locomotion = p->locomotion, V.turn = p->turn;
    V.snap_angle = p->snap_angle > 1.0f ? p->snap_angle : 45.0f;
    V.turn_speed = p->turn_speed > 1.0f ? p->turn_speed : 90.0f;
    V.move_speed = p->move_speed > 1.0f ? p->move_speed : 200.0f;
    V.tp_range = p->teleport_range > 50.0f ? p->teleport_range : 900.0f;
    V.vignette_max = fminf(fmaxf(p->vignette, 0.0f), 1.0f);
    V.hands = p->hands, V.laser = p->laser, V.hand_tracking = p->hand_tracking, V.drag = p->drag_locomotion;
    V.physical = p->physical_hands, V.hand_objects = p->hand_objects, V.push = p->hand_push, V.held_collide = p->held_collide, V.hide_body = p->hide_body;
    V.head_mode = p->head_collision;
    V.dom = p->dominant_hand ? 0 : 1; // (the dominant hand: right 1, left 0)
    V.grab_reach = p->grab_reach > 1.0f ? p->grab_reach : 10.0f;
    V.laser_len = p->laser_length > 50.0f ? p->laser_length : 1000.0f;
    V.haptics = fminf(fmaxf(p->haptics, 0.0f), 1.0f);
}
// a buzz at the project's haptics strength
static void buzz(int h, float amplitude, float seconds) {
    if (V.haptics > 0.0f) xr_haptic(h, amplitude * V.haptics, seconds);
}
bool vr_pawn_active(void) { return V.playing; }
float vr_pawn_vignette(void) { return V.vignette; }
float vr_pawn_fade(void) { return fmaxf(V.fade, V.blink); }

// --- The play space ---
static void rotz(float yaw_deg, const float in[3], float out[3]) {
    float s = sinf(yaw_deg * VR_DEG), c = cosf(yaw_deg * VR_DEG), x = in[0], y = in[1];
    out[0] = x * c - y * s, out[1] = x * s + y * c, out[2] = in[2];
}
void vr_pawn_origin(float pos[3], float* yaw_deg) {
    memcpy(pos, V.origin, 12);
    if (yaw_deg) *yaw_deg = V.yaw;
}
void vr_pawn_to_world(const float l[3], float out[3]) {
    float r[3];
    rotz(V.yaw, l, r);
    out[0] = V.origin[0] + r[0], out[1] = V.origin[1] + r[1], out[2] = V.origin[2] + r[2] + V.seat;
}
void vr_pawn_dir_to_world(const float l[3], float out[3]) { rotz(V.yaw, l, out); }

static void cross(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}
static float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float norm(float v[3]) {
    float l = sqrtf(dot(v, v));
    if (l > 1e-6f) v[0] /= l, v[1] /= l, v[2] /= l;
    return l;
}

// the pawn the play space follows: the possessed one (a Character: its feet), else a free origin
static CCharacter* pawn_char(World* w) { return V.pawn && ecs_alive(w, V.pawn) ? ecs_get(w, V.pawn, COMP_CHARACTER) : NULL; }
// the pawn's feet in the world (a Character: its capsule's bottom)
static bool pawn_feet(World* w, float p[3]) {
    if (!V.pawn || !ecs_alive(w, V.pawn)) return false;
    const CTransform* t = ecs_get(w, V.pawn, COMP_TRANSFORM);
    if (!t) return false;
    const CCharacter* c = pawn_char(w);
    p[0] = t->location[0], p[1] = t->location[1], p[2] = t->location[2] - (c ? c->half_height : 0.0f);
    return true;
}
// the play space follows the pawn: origin = its feet less where it stands in the play space
static void origin_from_pawn(World* w) {
    float f[3];
    if (!pawn_feet(w, f)) return;
    float o[3] = { V.off[0], V.off[1], 0.0f }, r[3];
    rotz(V.yaw, o, r);
    V.origin[0] = f[0] - r[0], V.origin[1] = f[1] - r[1], V.origin[2] = f[2];
}
// where the pawn stands, in the play space, from the world (after the origin moved)
static void off_from_pawn(World* w) {
    float f[3];
    if (!pawn_feet(w, f)) return;
    float d[3] = { f[0] - V.origin[0], f[1] - V.origin[1], 0.0f }, l[3];
    rotz(-V.yaw, d, l);
    V.off[0] = l[0], V.off[1] = l[1];
}
// a turn by d degrees about a play-space point (the head): it stays where it is in the world, the pawn too
static void turn_about(World* w, const float pivot_local[3], float d) {
    float before[3], after[3];
    vr_pawn_to_world(pivot_local, before);
    V.yaw += d;
    vr_pawn_to_world(pivot_local, after);
    V.origin[0] += before[0] - after[0], V.origin[1] += before[1] - after[1];
    off_from_pawn(w);
    if (V.pawn && ecs_alive(w, V.pawn)) {
        CTransform* t = ecs_get(w, V.pawn, COMP_TRANSFORM);
        if (t) t->rotation[1] = V.yaw;
        CCharacter* c = pawn_char(w);
        if (c) c->control_yaw = V.yaw;
    }
}

// (Q25b) mixed reality turns the level to the room's walls: about the head, as a snap turn (nothing seen moves for the player)
void vr_pawn_turn_play_space(World* w, float delta_deg) {
    float hp[3], hf[3], hu[3];
    xr_head(hp, hf, hu);
    turn_about(w, hp, delta_deg);
}

// --- The visuals: a few entities (no collision, not saved, hidden until needed) ---
static Entity make_part(World* w, const char* name, uint32_t shape, const float color[3], float emissive, bool shadow) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    if (e == ENTITY_NONE) return e;
    CName* n = ecs_get(w, e, COMP_NAME);
    if (n) snprintf(n->text, sizeof n->text, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->scale[0] = t->scale[1] = t->scale[2] = 0.01f;
    t->flags = MOBILITY_MOVABLE | TRANSFORM_NO_COLLISION | TRANSFORM_HIDDEN_GAME | TRANSFORM_NO_ICON;
    CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    sm->mesh = shape, sm->roughness = 0.6f, sm->uv_scale = 1.0f;
    memcpy(sm->base_color, color, 12);
    for (int k = 0; k < 3; k++) sm->emissive[k] = color[k] * emissive;
    sm->cast_shadow = shadow ? 1u : 0u;
    return e;
}
static void show(World* w, Entity e, bool on) {
    CTransform* t = e && ecs_alive(w, e) ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
    if (t) t->flags = on ? t->flags & ~TRANSFORM_HIDDEN_GAME : t->flags | TRANSFORM_HIDDEN_GAME;
}
// a box from a to b, w thick, its up toward up_hint
static void place_segment(World* w, Entity e, const float a[3], const float b[3], float thick, const float up_hint[3]) {
    CTransform* t = e && ecs_alive(w, e) ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
    if (!t) return;
    float x[3][3] = { { b[0] - a[0], b[1] - a[1], b[2] - a[2] } };
    float len = norm(x[0]);
    if (len < 0.01f) x[0][0] = 1.0f, x[0][1] = x[0][2] = 0.0f;
    cross(up_hint, x[0], x[1]);
    if (norm(x[1]) < 1e-4f) {
        float z[3] = { 0, 0, 1 }, yy[3] = { 0, 1, 0 };
        cross(fabsf(x[0][2]) < 0.9f ? z : yy, x[0], x[1]), norm(x[1]);
    }
    cross(x[0], x[1], x[2]);
    axes_rotator(x, t->rotation);
    for (int k = 0; k < 3; k++) t->location[k] = (a[k] + b[k]) * 0.5f;
    t->scale[0] = fmaxf(len, 0.2f) * 0.01f, t->scale[1] = t->scale[2] = thick * 0.01f;
}
static void place_point(World* w, Entity e, const float p[3], float size_cm, const float scale_z) {
    CTransform* t = e && ecs_alive(w, e) ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
    if (!t) return;
    memcpy(t->location, p, 12);
    t->rotation[0] = t->rotation[1] = t->rotation[2] = 0.0f;
    t->scale[0] = t->scale[1] = size_cm * 0.01f, t->scale[2] = scale_z * 0.01f;
}
static void tint(World* w, Entity e, const float c[3], float emissive) {
    CStaticMesh* sm = e && ecs_alive(w, e) ? ecs_get(w, e, COMP_STATIC_MESH) : NULL;
    if (!sm) return;
    memcpy(sm->base_color, c, 12);
    for (int k = 0; k < 3; k++) sm->emissive[k] = c[k] * emissive;
}

// the pawn's own meshes hidden (its body, a mannequin, a capsule's cylinder): in VR the eyes are inside them. Not the
// pawn itself (its overlaps, its collision go on): every mesh whose owners lead to it (a Skeletal Mesh's parts belong to
// the Mesh component, which belongs to the pawn). The components take their owner's flags every frame (bp_components.c):
// the list is found every 30 frames and hidden every frame, after the movement and before the render
static uint32_t owner_depth(World* w, Entity e, Entity pawn) {
    for (int k = 0; k < 4; k++) {
        const CAttach* at = ecs_alive(w, e) ? ecs_get(w, e, COMP_ATTACH) : NULL;
        if (!at) return 0;
        if (at->owner == pawn || at->parent == pawn) return 1;
        if (at->owner == e) return 0;
        e = at->owner;
    }
    return 0;
}
static void find_body(World* w) {
    V.nbody = 0;
    if (!V.hide_body || !V.pawn || !ecs_alive(w, V.pawn)) return;
    if (ecs_get(w, V.pawn, COMP_STATIC_MESH) && V.nbody < VR_BODY_MAX) V.body[V.nbody++] = V.pawn; // (its own mesh: drawn only, see hide_body)
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_ATTACH)) || !(a->mask & COMP_BIT(COMP_TRANSFORM)) || !a->count) continue;
        if (!(a->mask & (COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_SKELETAL_MESH)))) continue;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count && V.nbody < VR_BODY_MAX; i++)
            if (owner_depth(w, ents[i], V.pawn)) V.body[V.nbody++] = ents[i];
    }
}
static void hide_body(World* w) {
    if (!V.hide_body) return;
    for (uint32_t i = 0; i < V.nbody; i++) {
        Entity e = V.body[i];
        CTransform* t = ecs_alive(w, e) ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (!t) continue;
        if (e == V.pawn) { // (the pawn's own mesh: taken off it, not the pawn hidden: a hidden pawn stops its overlaps; the game only)
            ecs_remove(w, e, COMP_STATIC_MESH);
            continue;
        }
        t->flags |= TRANSFORM_HIDDEN_GAME;
    }
}

// --- (2026-11-20) a Skeletal Mesh as the hand (vr_hand.h: the base hand, or any hand rigged on OpenXR's joint names) ---
static VrHandRig g_rig[2];
static const SkelMesh* g_rig_mesh[2];
static void sk_hand_make(World* w, int h, const char* vpath) {
    SkelMesh* m = anim_mesh_load(vpath);
    if (!m) {
        printf("[RealEngine] VR: the hand mesh %s was not found (the engine's hands instead)\n", vpath);
        return;
    }
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SKELETAL_MESH));
    if (e == ENTITY_NONE) return;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", h ? "VR_HandSkeletal_R" : "VR_HandSkeletal_L");
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    t->flags = MOBILITY_MOVABLE | TRANSFORM_NO_COLLISION | TRANSFORM_NO_ICON;
    CSkeletalMesh* sk = ecs_get(w, e, COMP_SKELETAL_MESH);
    sk->mesh = m->hash, sk->play_rate = 1.0f;
    VrHandState* s = &V.h[h];
    s->sk_ent = e, s->sk_shown = -1, s->sk_nparts = 0;
    s->sk_rig_ok = vrh_rig_build(&g_rig[h], m, h);
    g_rig_mesh[h] = m;
    printf("[RealEngine] VR: %s hand %s: %u bones, %s%s\n", h ? "right" : "left", vpath, m->nbones,
           s->sk_rig_ok ? "fingers found" : "no OpenXR finger bones (shown still)",
           s->sk_rig_ok ? (g_rig[h].open ? ", its Open/Fist animations" : ", curled by its bones' axes") : "");
}
// the hand actor's parts (anim.c makes them on its first pose)
static void sk_hand_parts(World* w, int h) {
    VrHandState* s = &V.h[h];
    s->sk_nparts = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_ATTACH)) || !(a->mask & COMP_BIT(COMP_TRANSFORM)) || !a->count) continue;
        const Entity* ents = ecs_arch_entities(a);
        const CAttach* at = ecs_arch_column(a, COMP_ATTACH);
        for (uint32_t i = 0; i < a->count && s->sk_nparts < 24; i++)
            if (at[i].owner == s->sk_ent) s->sk_parts[s->sk_nparts++] = ents[i];
    }
}
static void sk_hand_show(World* w, int h, bool on) {
    VrHandState* s = &V.h[h];
    uint32_t before = s->sk_nparts;
    if (!s->sk_nparts || (V.frame % 30u) == 0u) sk_hand_parts(w, h);
    if (s->sk_shown == (int)on && before == s->sk_nparts) return;
    s->sk_shown = on;
    show(w, s->sk_ent, on);
    for (uint32_t i = 0; i < s->sk_nparts; i++) show(w, s->sk_parts[i], on);
}
static void sk_hand_destroy(World* w, int h) {
    VrHandState* s = &V.h[h];
    if (!s->sk_ent) return;
    anim_pose_override(s->sk_ent, NULL, 0);
    sk_hand_parts(w, h);
    for (uint32_t i = 0; i < s->sk_nparts; i++)
        if (ecs_alive(w, s->sk_parts[i])) ecs_destroy(w, s->sk_parts[i]);
    if (ecs_alive(w, s->sk_ent)) ecs_destroy(w, s->sk_ent);
    s->sk_ent = ENTITY_NONE, s->sk_nparts = 0;
}
// placed and posed: tracked, by the real joints (the hand where the wrist and the knuckles are); a controller, at the
// grip like the boxes (Hand Mesh Rotation / Scale) with each finger curled by its trigger, grip and buttons
static void sk_hand_pose(World* w, int h, const XrHand* x) {
    VrHandState* s = &V.h[h];
    const SkelMesh* m = g_rig_mesh[h];
    CTransform* mt = ecs_alive(w, s->sk_ent) ? ecs_get(w, s->sk_ent, COMP_TRANSFORM) : NULL;
    if (!mt || !m) return;
    const ProjectVr* pv = &project()->vr;
    float sc = pv->hand_mesh_scale > 0.0f ? pv->hand_mesh_scale : 1.0f, rot[3][3], pos[3];
    static AnimXf pose[ANIM_MAX_BONES];
    bool tracked = x->tracked && V.hand_tracking && s->sk_rig_ok && g_rig[h].tracked_ok;
    if (tracked) {
        float jw[VRH_JOINTS][3];
        for (int k = 0; k < VRH_JOINTS; k++) vr_pawn_to_world(x->joint[k], jw[k]), jw[k][0] += s->shift[0], jw[k][1] += s->shift[1], jw[k][2] += s->shift[2];
        tracked = vrh_pose_tracked(&g_rig[h], m, (const float(*)[3])jw, sc, rot, pos, pose);
    }
    if (!tracked) {
        float r0[3][3];
        rotator_axes(pv->hand_mesh_rot, r0);
        for (int a = 0; a < 3; a++)
            for (int k = 0; k < 3; k++) rot[a][k] = r0[a][0] * s->world_axes[0][k] + r0[a][1] * s->world_axes[1][k] + r0[a][2] * s->world_axes[2][k];
        // the palm on the grip, 2 cm over it (the boxes' place): the palm joint's bind under it
        int pb = s->sk_rig_ok ? g_rig[h].bone_of[VRH_PALM] : -1;
        const float* pp = pb >= 0 ? g_rig[h].pm[pb] : NULL;
        for (int k = 0; k < 3; k++)
            pos[k] = s->world_pos[k] + s->world_axes[2][k] * 2.0f - (pp ? (rot[0][k] * pp[0] + rot[1][k] * pp[1] + rot[2][k] * pp[2]) * sc : 0.0f);
        float curl[5];
        if (x->tracked) memcpy(curl, x->curl, sizeof curl); // (tracked, the rig without the knuckles: the fingers' curls)
        else curl[0] = x->primary || x->secondary ? 0.8f : x->grip * 0.4f, curl[1] = x->trigger, curl[2] = curl[3] = curl[4] = x->grip;
        if (s->sk_rig_ok) vrh_pose_curl(&g_rig[h], m, curl, pose);
        else
            for (uint32_t b = 0; b < m->nbones; b++) pose[b] = m->bones[b].bind;
    }
    axes_rotator(rot, mt->rotation);
    memcpy(mt->location, pos, 12);
    mt->scale[0] = mt->scale[1] = mt->scale[2] = sc;
    anim_pose_override(s->sk_ent, pose, m->nbones);
}

void vr_pawn_begin_play(World* w) {
    memset(&V, 0, sizeof V);
    const ProjectVr* p = &project()->vr;
    V.playing = true, V.w = w;
    sync_settings();
    V.seat = p->tracking == VR_TRACK_SEATED ? (p->seated_height > 0.0f ? p->seated_height : 120.0f) : 0.0f;
    V.snap_ready = true;
    V.pawn = bp_player(w);
    if (V.pawn) {
        const CTransform* t = ecs_get(w, V.pawn, COMP_TRANSFORM);
        const CCharacter* c = pawn_char(w);
        V.yaw = c ? c->control_yaw : t ? t->rotation[1] : 0.0f;
        origin_from_pawn(w);
    } else {
        float pos[3] = { 0, 0, 170.0f }, yaw = 0.0f, pitch = 0.0f;
        if (bp_player_camera(w, pos, &yaw, &pitch)) V.yaw = yaw;
        V.origin[0] = pos[0], V.origin[1] = pos[1], V.origin[2] = pos[2] - 170.0f;
    }
    // the visuals: a skin-colored hand each, a laser each, the teleport's arc and its ring
    const float skin[3] = { 0.62f, 0.45f, 0.36f }, cyan[3] = { 0.1f, 0.65f, 1.0f };
    for (int h = 0; h < 2; h++) {
        for (int i = 0; i < VR_HAND_PARTS; i++) V.h[h].parts[i] = make_part(w, h ? "VR_Hand_R" : "VR_Hand_L", SHAPE_CUBE, skin, 0.0f, true);
        V.h[h].laser = make_part(w, h ? "VR_Laser_R" : "VR_Laser_L", SHAPE_CUBE, cyan, 4.0f, false);
    }
    for (int h = 0; h < 2; h++) { // the project's own hand meshes, in place of the boxes
        const char* hm = p->hand_mesh[h];
        size_t hl = strlen(hm);
        if (hl > 5 && !strcmp(hm + hl - 5, ".rskm")) { // (2026-11-20) a Skeletal Mesh: its fingers moved (vr_hand.h)
            sk_hand_make(w, h, hm);
            continue;
        }
        uint32_t id = hm[0] ? render_mesh_id_of(hm) : 0;
        if (hm[0] && !id) printf("[RealEngine] VR: the hand mesh %s was not found (the engine's hands instead)\n", hm);
        if (id) {
            const float white[3] = { 1, 1, 1 };
            V.h[h].mesh_ent = make_part(w, h ? "VR_HandMesh_R" : "VR_HandMesh_L", id, white, 0.0f, true);
        }
    }
    for (int i = 0; i < VR_ARC_DOTS; i++) V.arc[i] = make_part(w, "VR_TeleportArc", SHAPE_SPHERE, cyan, 3.0f, false);
    V.ring = make_part(w, "VR_TeleportTarget", SHAPE_CYLINDER, cyan, 2.0f, false);
    find_body(w), hide_body(w);
    printf("[RealEngine] VR pawn: %s, locomotion %d, turn %d (%.0f deg), vignette %.2f, play space at (%.0f, %.0f, %.0f) yaw %.0f\n",
           V.pawn ? "the possessed pawn" : "a free origin", V.locomotion, V.turn, V.snap_angle, V.vignette_max, V.origin[0], V.origin[1], V.origin[2], V.yaw);
}

void vr_pawn_end_play(World* w) {
    if (!V.playing) return;
    for (int h = 0; h < 2; h++) vr_pawn_release(w, h);
    for (int h = 0; h < 2; h++) {
        for (int i = 0; i < VR_HAND_PARTS; i++)
            if (V.h[h].parts[i] && ecs_alive(w, V.h[h].parts[i])) ecs_destroy(w, V.h[h].parts[i]);
        if (V.h[h].laser && ecs_alive(w, V.h[h].laser)) ecs_destroy(w, V.h[h].laser);
        if (V.h[h].mesh_ent && ecs_alive(w, V.h[h].mesh_ent)) ecs_destroy(w, V.h[h].mesh_ent);
        sk_hand_destroy(w, h);
    }
    for (int i = 0; i < VR_ARC_DOTS; i++)
        if (V.arc[i] && ecs_alive(w, V.arc[i])) ecs_destroy(w, V.arc[i]);
    if (V.ring && ecs_alive(w, V.ring)) ecs_destroy(w, V.ring);
    interaction_vr_pointer(false, NULL, NULL, false);
    V.playing = false;
}

// --- Traces: the level's collision boxes and the landscapes (the self-test's own scene instead) ---
static float (*g_test_trace)(const float a[3], const float b[3]);
static Entity g_test_pawn;
static float trace(const float a[3], const float b[3]) {
    if (g_test_trace) return g_test_trace(a, b);
    float f = bp_trace_boxes(a, b, 0.0f);
    float l = landscape_trace(a, b);
    return l < f ? l : f;
}
// a sphere of radius r along a -> b (the hands, the held things, the head): the fixed level only (moving bodies don't stop it)
static float trace_r(const float a[3], const float b[3], float r) {
    if (g_test_trace) return g_test_trace(a, b);
    float f = bp_trace_boxes(a, b, r);
    float l = landscape_trace(a, b);
    return l < f ? l : f;
}
// a floor under p: a short trace down from 30 cm above it lands within 8 cm of it
static bool floor_at(const float p[3]) {
    float a[3] = { p[0], p[1], p[2] + 30.0f }, b[3] = { p[0], p[1], p[2] - 10.0f };
    float f = trace(a, b);
    return f < 1.0f && fabsf(a[2] + (b[2] - a[2]) * f - p[2]) < 8.0f;
}

// --- The hands in the world ---
static void hand_world(int h) {
    const XrHand* x = xr_hand(h);
    VrHandState* s = &V.h[h];
    vr_pawn_to_world(x->pos, s->world_pos);
    memcpy(s->real_pos, s->world_pos, 12);
    float f[3], u[3], r[3];
    vr_pawn_dir_to_world(x->fwd, f), vr_pawn_dir_to_world(x->up, u);
    norm(f), norm(u);
    cross(u, f, r), norm(r);
    cross(f, r, u);
    memcpy(s->world_axes[0], f, 12), memcpy(s->world_axes[1], r, 12), memcpy(s->world_axes[2], u, 12);
    bool aim = x->aim_fwd[0] != 0.0f || x->aim_fwd[1] != 0.0f || x->aim_fwd[2] != 0.0f;
    vr_pawn_to_world(aim ? x->aim_pos : x->pos, s->aim_o);
    vr_pawn_dir_to_world(aim ? x->aim_fwd : x->fwd, s->aim_d);
    norm(s->aim_d);
    // the throw's speed: the places of the last frames
    memcpy(s->hist[s->hist_i], s->world_pos, 12), s->hist_t[s->hist_i] = V.time;
    s->hist_i = (s->hist_i + 1) % VR_VEL_HIST;
    if (s->hist_n < VR_VEL_HIST) s->hist_n++;
}
// (physical hands) the drawn hand where the real one is, the fixed level allowing: a 3 cm sphere from where it was drawn
// (a jump past 60 cm: from the head) to the real place, stopped at the first wall, floor or table
static void world_box(World* w, Entity e, float mn[3], float mx[3]);
static bool ray_box(const float o[3], const float d[3], const float mn[3], const float mx[3], float* s_in);
// the hand's points that touch: the palm and the fingertips (tracked: the joints' tips; a controller: where its fingers
// reach around the grip), world space, the hand's shift so far applied
static int hand_points(int h, float pts[6][3]) {
    const XrHand* x = xr_hand(h);
    const VrHandState* s = &V.h[h];
    memcpy(pts[0], s->world_pos, 12);
    if (x->tracked && V.hand_tracking) {
        static const int tips[5] = { 5, 10, 15, 20, 25 };
        for (int f = 0; f < 5; f++) {
            vr_pawn_to_world(x->joint[tips[f]], pts[1 + f]);
            for (int k = 0; k < 3; k++) pts[1 + f][k] += s->shift[k];
        }
        return 6;
    }
    const float *F = s->world_axes[0], *R = s->world_axes[1], *U = s->world_axes[2];
    const float side = h ? 1.0f : -1.0f;
    const float off[4][3] = { { 13.0f, 0.0f, -2.0f }, { 11.0f, 3.5f * side, 0.0f }, { 9.0f, -3.5f * side, -1.0f }, { 6.0f, 4.5f * side, 1.0f } };
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++) pts[1 + i][k] = s->world_pos[k] + F[k] * off[i][0] + R[k] * off[i][1] + U[k] * off[i][2];
    return 5;
}
// the boxes of what moves near the hand (bodies with physics, Interactables: doors, levers, Grabbables), not what a hand
// holds nor a Grab Socket; the fixed level is the trace's
static int near_boxes(World* w, const float p[3], float (*mn)[3], float (*mx)[3], bool* grab, int cap) {
    int n = 0;
    for (uint32_t ai = 0; ai < w->archetype_count && n < cap; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & (COMP_BIT(COMP_RIGID_BODY) | COMP_BIT(COMP_INTERACTABLE))) || !(a->mask & COMP_BIT(COMP_TRANSFORM)) ||
            !(a->mask & COMP_BIT(COMP_STATIC_MESH)) || !a->count)
            continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CInteractable* ia = (a->mask & COMP_BIT(COMP_INTERACTABLE)) ? ecs_arch_column(a, COMP_INTERACTABLE) : NULL;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count && n < cap; i++) {
            if (ents[i] == V.h[0].held || ents[i] == V.h[1].held || (tr[i].flags & (TRANSFORM_HIDDEN_GAME | TRANSFORM_NO_COLLISION))) continue;
            if (ia && (ia[i].flags & IA_GRAB_SOCKET)) continue;
            float d[3] = { tr[i].location[0] - p[0], tr[i].location[1] - p[1], tr[i].location[2] - p[2] };
            if (dot(d, d) > 150.0f * 150.0f) continue; // (a quick cut: its pivot within 1.5 m)
            world_box(w, ents[i], mn[n], mx[n]);
            grab[n] = ia && (ia[i].flags & IA_GRAB);
            n++;
        }
    }
    return n;
}

static void solve_hand(World* w, int h) {
    VrHandState* s = &V.h[h];
    s->shift[0] = s->shift[1] = s->shift[2] = 0.0f;
    if (!V.physical || !xr_hand(h)->active) {
        s->solved_ok = false;
        return;
    }
    float from[3], d[3];
    for (int k = 0; k < 3; k++) d[k] = s->world_pos[k] - s->solved[k];
    if (s->solved_ok && dot(d, d) < 60.0f * 60.0f) memcpy(from, s->solved, 12);
    else {
        float hp[3], hf[3], hu[3];
        xr_head(hp, hf, hu), vr_pawn_to_world(hp, from);
    }
    float f = trace_r(from, s->world_pos, 3.0f), to[3];
    for (int k = 0; k < 3; k++) to[k] = from[k] + (s->world_pos[k] - from[k]) * (f < 1.0f ? fmaxf(f - 0.002f, 0.0f) : 1.0f);
    for (int k = 0; k < 3; k++) s->shift[k] = to[k] - s->world_pos[k], s->world_pos[k] = to[k], s->solved[k] = to[k];
    s->solved_ok = true;
    if (f < 1.0f) V.st.hand_blocked++;
    // the whole hand, not only its palm (the fingers went into the cubes through the gaps, Q25's headset test): the palm and
    // the fingertips against the fixed level (from the palm) and the boxes of what moves near; the hand moved back by the
    // deepest, a few times (one push can put another point in)
    float bmn[24][3], bmx[24][3];
    bool bgrab[24];
    int nb = V.hand_objects ? near_boxes(w, s->world_pos, bmn, bmx, bgrab, 24) : 0; // (off: the level only)
    // (a hand closing or holding: its fingers may wrap a Grabbable; open, nothing goes in)
    const bool closing = xr_hand(h)->grip >= 0.3f || s->held;
    for (int it = 0; it < 3; it++) {
        float pts[6][3], best[3] = { 0, 0, 0 }, bl = 0.0f;
        int np = hand_points(h, pts);
        for (int i = 0; i < np; i++) {
            // the way a point came: a fingertip from the palm, the palm from where it was drawn last frame
            const float* o = i ? s->world_pos : from;
            const float r = i ? 1.2f : 3.0f;
            if (i) { // a fingertip through the level: back to where the trace from the palm met it
                float ft = trace_r(s->world_pos, pts[i], r);
                if (ft < 1.0f) {
                    float c[3];
                    for (int k = 0; k < 3; k++) c[k] = (s->world_pos[k] + (pts[i][k] - s->world_pos[k]) * ft) - pts[i][k];
                    if (dot(c, c) > bl) bl = dot(c, c), memcpy(best, c, 12);
                }
            }
            for (int b = 0; b < nb; b++) { // in a box: back to where its way entered it (from inside already: out by its shallowest side)
                if (i && closing && bgrab[b]) continue;
                float in[3][2], m = 1e9f, emn[3], emx[3];
                int ax = -1, sg = 0;
                bool inside = true;
                for (int k = 0; k < 3 && inside; k++) {
                    in[k][0] = pts[i][k] - (bmn[b][k] - r), in[k][1] = (bmx[b][k] + r) - pts[i][k];
                    if (in[k][0] <= 0.0f || in[k][1] <= 0.0f) inside = false;
                    emn[k] = bmn[b][k] - r, emx[k] = bmx[b][k] + r;
                }
                if (!inside) continue;
                float way[3] = { pts[i][0] - o[0], pts[i][1] - o[1], pts[i][2] - o[2] }, te = 0.0f;
                if (dot(way, way) > 1e-4f && ray_box(o, way, emn, emx, &te) && te > 0.0f && te < 1.0f) {
                    float c[3];
                    for (int k = 0; k < 3; k++) c[k] = (o[k] + way[k] * fmaxf(te - 0.01f, 0.0f)) - pts[i][k];
                    if (dot(c, c) > bl) bl = dot(c, c), memcpy(best, c, 12);
                    continue;
                }
                for (int k = 0; k < 3; k++)
                    for (int q = 0; q < 2; q++)
                        if (in[k][q] < m) m = in[k][q], ax = k, sg = q ? 1 : -1;
                float c[3] = { 0, 0, 0 };
                c[ax] = (float)sg * m;
                if (dot(c, c) > bl) bl = dot(c, c), memcpy(best, c, 12);
            }
        }
        if (bl <= 1e-6f) break;
        for (int k = 0; k < 3; k++) s->world_pos[k] += best[k], s->shift[k] += best[k], s->solved[k] = s->world_pos[k];
        V.st.hand_blocked++;
    }
}

// how closed a hand is for a gesture choice (VR_GESTURE_*): a controller's grip; a tracked hand's fist, its thumb on the
// index, any finger on the index, or the most of the fist and the touches
static float hand_closed(const XrHand* x, int mode) {
    if (!x->tracked) return x->grip;
    float any = fmaxf(fmaxf(x->touch[0], x->touch[1]), fmaxf(x->touch[2], x->touch[3]));
    switch (mode) {
    case VR_GESTURE_PINCH: return x->touch[0];
    case VR_GESTURE_ANY_TO_INDEX: return any;
    case VR_GESTURE_FIST_OR_TOUCH: return fmaxf(x->grip, any);
    default: return x->grip;
    }
}

// the hand's speed at the throw (cm/s): the fastest of its two-frame speeds in the last ~80 ms (a hand slows as it lets
// go: the mean over the window, or the last frame alone, threw short)
static void hand_velocity(int h, float v[3]) {
    const VrHandState* s = &V.h[h];
    v[0] = v[1] = v[2] = 0.0f;
    if (s->hist_n < 3) return;
    int newest = (s->hist_i + VR_VEL_HIST - 1) % VR_VEL_HIST;
    float best = -1.0f;
    for (int k = 0; k + 2 < s->hist_n; k++) {
        int a = (newest + VR_VEL_HIST - k) % VR_VEL_HIST, b = (newest + VR_VEL_HIST - k - 2) % VR_VEL_HIST;
        if (s->hist_t[newest] - s->hist_t[b] > 0.08f) break;
        float dt = s->hist_t[a] - s->hist_t[b];
        if (dt < 1e-4f) continue;
        float c[3] = { (s->hist[a][0] - s->hist[b][0]) / dt, (s->hist[a][1] - s->hist[b][1]) / dt, (s->hist[a][2] - s->hist[b][2]) / dt };
        float sp = dot(c, c);
        if (sp > best) best = sp, memcpy(v, c, 12);
    }
}

static void world_box(World* w, Entity e, float mn[3], float mx[3]) {
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    const CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    float lmn[3] = { -50, -50, -50 }, lmx[3] = { 50, 50, 50 };
    if (sm) {
        render_mesh_bounds(sm->mesh, lmn, lmx);
        if (!(lmx[0] > lmn[0])) lmn[0] = lmn[1] = lmn[2] = -50.0f, lmx[0] = lmx[1] = lmx[2] = 50.0f;
    }
    float rows[3][4];
    render_transform_rows(t, rows);
    for (int k = 0; k < 3; k++) {
        float c = rows[k][3], hh = 0.0f;
        for (int j = 0; j < 3; j++) c += rows[k][j] * (lmn[j] + lmx[j]) * 0.5f, hh += fabsf(rows[k][j]) * (lmx[j] - lmn[j]) * 0.5f;
        hh = fmaxf(hh, 1.0f);
        mn[k] = c - hh, mx[k] = c + hh;
    }
}
static float box_dist(const float p[3], const float mn[3], const float mx[3]) {
    float d2 = 0.0f;
    for (int k = 0; k < 3; k++) {
        float v = p[k] < mn[k] ? mn[k] - p[k] : p[k] > mx[k] ? p[k] - mx[k] : 0.0f;
        d2 += v * v;
    }
    return sqrtf(d2);
}
static bool ray_box(const float o[3], const float d[3], const float mn[3], const float mx[3], float* s_in) {
    float t0 = 0.0f, t1 = 1e30f;
    for (int k = 0; k < 3; k++) {
        if (fabsf(d[k]) < 1e-8f) {
            if (o[k] < mn[k] || o[k] > mx[k]) return false;
            continue;
        }
        float inv = 1.0f / d[k], a = (mn[k] - o[k]) * inv, b = (mx[k] - o[k]) * inv;
        if (a > b) { float t = a; a = b; b = t; }
        t0 = fmaxf(t0, a), t1 = fminf(t1, b);
        if (t0 > t1) return false;
    }
    *s_in = t0;
    return true;
}

// the Interactables with a flag, one pass (no allocation): the nearest to p within reach, or the first the ray meets
static Entity find_interactable(World* w, uint16_t flag, const float p[3], float reach, const float* ro, const float* rd, float* best_out, Entity skip) {
    Entity best = ENTITY_NONE;
    float bd = ro ? V.laser_len : reach;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_INTERACTABLE)) || !(a->mask & COMP_BIT(COMP_TRANSFORM)) || !a->count) continue;
        const CInteractable* ia = ecs_arch_column(a, COMP_INTERACTABLE);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            if (flag && !(ia[i].flags & flag)) continue;
            if (!(ia[i].flags & IA_ENABLED) || ents[i] == skip) continue;
            float mn[3], mx[3], d;
            world_box(w, ents[i], mn, mx);
            if (ro) {
                if (!ray_box(ro, rd, mn, mx, &d)) continue;
            } else
                d = box_dist(p, mn, mx);
            if (d < bd) bd = d, best = ents[i];
        }
    }
    if (best_out) *best_out = bd;
    return best;
}

// --- Grabbing ---
static void mat_from_t(const CTransform* t, float m[3][3]) { rotator_axes(t->rotation, m); }
bool vr_pawn_grab(World* w, int hand, Entity target) {
    hand = hand ? 1 : 0;
    if (!V.playing || !target || !ecs_alive(w, target)) return false;
    CTransform* t = ecs_get(w, target, COMP_TRANSFORM);
    if (!t) return false;
    for (int k = 0; k < 2; k++)
        if (V.h[k].held == target) vr_pawn_release(w, k); // (from one hand to the other: let go first, no throw)
    if (V.h[hand].held) vr_pawn_release(w, hand);
    VrHandState* s = &V.h[hand];
    const CRigidBody* rb = ecs_get(w, target, COMP_RIGID_BODY);
    s->held_body = rb && rb->simulate;
    if (s->held_body) physics_set_simulate(w, target, false);
    // held: out of the player's way (its capsule pushed by what it holds threw the player around, Q25's headset test)
    s->held_nocol = (t->flags & TRANSFORM_NO_COLLISION) != 0;
    t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE | TRANSFORM_NO_COLLISION;
    const CInteractable* ia = ecs_get(w, target, COMP_INTERACTABLE);
    float m[3][3];
    mat_from_t(t, m);
    if (ia && (ia->flags & IA_GRAB_SNAP)) { // held as made: its pivot in the hand, its axes the hand's
        memset(s->rel_pos, 0, 12);
        memset(s->rel_axes, 0, sizeof s->rel_axes);
        s->rel_axes[0][0] = s->rel_axes[1][1] = s->rel_axes[2][2] = 1.0f;
    } else { // where it was taken: its place and axes in the hand's frame
        float d[3] = { t->location[0] - s->world_pos[0], t->location[1] - s->world_pos[1], t->location[2] - s->world_pos[2] };
        for (int a = 0; a < 3; a++) s->rel_pos[a] = dot(d, s->world_axes[a]);
        for (int r = 0; r < 3; r++)
            for (int a = 0; a < 3; a++) s->rel_axes[r][a] = dot(m[r], s->world_axes[a]);
    }
    s->held = target;
    V.st.grabs++;
    buzz(hand, 0.4f, 0.05f);
    BpValue ev[2];
    memset(ev, 0, sizeof ev);
    ev[0].v[0] = (float)hand;
    bp_fire_actor_event(w, target, BPK_EVENT_VR_GRABBED, ev, 1);
    return true;
}
static void follow_hand(World* w, int h) {
    VrHandState* s = &V.h[h];
    if (!s->held || !ecs_alive(w, s->held)) {
        s->held = ENTITY_NONE;
        return;
    }
    CTransform* t = ecs_get(w, s->held, COMP_TRANSFORM);
    if (!t) return;
    float m[3][3], was[3] = { t->location[0], t->location[1], t->location[2] }, want[3];
    for (int k = 0; k < 3; k++)
        want[k] = s->world_pos[k] + s->world_axes[0][k] * s->rel_pos[0] + s->world_axes[1][k] * s->rel_pos[1] + s->world_axes[2][k] * s->rel_pos[2];
    if (V.held_collide) { // it stops at the fixed level: a sphere of its smallest half size from where it was
        float mn[3], mx[3];
        world_box(w, s->held, mn, mx);
        float r = fminf(fminf(mx[0] - mn[0], mx[1] - mn[1]), mx[2] - mn[2]) * 0.5f;
        r = fminf(fmaxf(r, 1.0f), 15.0f);
        float f = trace_r(was, want, r);
        if (f < 1.0f)
            for (int k = 0; k < 3; k++) want[k] = was[k] + (want[k] - was[k]) * fmaxf(f - 0.002f, 0.0f);
    }
    memcpy(t->location, want, 12);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++)
            m[r][k] = s->world_axes[0][k] * s->rel_axes[r][0] + s->world_axes[1][k] * s->rel_axes[r][1] + s->world_axes[2][k] * s->rel_axes[r][2];
    axes_rotator(m, t->rotation);
}
void vr_pawn_release(World* w, int hand) {
    hand = hand ? 1 : 0;
    VrHandState* s = &V.h[hand];
    Entity e = s->held;
    s->held = ENTITY_NONE;
    if (!e || !ecs_alive(w, e)) return;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (!s->held_nocol) t->flags &= ~TRANSFORM_NO_COLLISION; // (it blocks again)
    // a Grab Socket near it (the tag it asks for, when it asks): snapped into its place, its rotation
    float c[3] = { t->location[0], t->location[1], t->location[2] }, sd = 0.0f;
    Entity sock = find_interactable(w, IA_GRAB_SOCKET, c, VR_SOCKET_REACH, NULL, NULL, &sd, e);
    if (sock) {
        const CInteractable* si = ecs_get(w, sock, COMP_INTERACTABLE);
        if (si && si->require[0] && !gt_has(w, e, si->require, false)) sock = ENTITY_NONE;
    }
    BpValue ev[3];
    memset(ev, 0, sizeof ev);
    ev[0].v[0] = (float)hand;
    if (sock) {
        const CTransform* st = ecs_get(w, sock, COMP_TRANSFORM);
        memcpy(t->location, st->location, 12), memcpy(t->rotation, st->rotation, 12);
        V.st.socketed++;
        buzz(hand, 0.6f, 0.08f);
        memcpy(&ev[2].v[0], &sock, sizeof sock);
    } else {
        float v[3];
        hand_velocity(hand, v);
        float ts = project()->vr.throw_scale > 0.0f ? project()->vr.throw_scale : 1.0f;
        v[0] *= ts, v[1] *= ts, v[2] *= ts;
        if (s->held_body) {
            physics_set_simulate(w, e, true);
            physics_set_linear_velocity(w, e, v, false);
            // (continuous collision: a throw at a few meters a second crossed a 1 cm floor between two steps and fell out of
            // the level, Q25's played test; what a VR hand let go keeps it)
            physics_set_ccd(w, e, true);
        }
        float speed = sqrtf(dot(v, v));
        if (speed > 100.0f) V.st.throws++, V.st.last_throw_speed = speed;
        memcpy(ev[1].v, v, 12);
    }
    bp_fire_actor_event(w, e, BPK_EVENT_VR_RELEASED, ev, 3);
}
Entity vr_pawn_held(int hand) { return V.h[hand ? 1 : 0].held; }

// --- Drag locomotion: the play space moved by d (cm, xy), a Character's capsule with it, stopped short of a wall ---
static void move_by(World* w, float d[2]) {
    if (fabsf(d[0]) < 1e-4f && fabsf(d[1]) < 1e-4f) return;
    float f[3];
    if (pawn_feet(w, f)) {
        // the whole capsule: its radius at the knee, the waist and the chest (a table's top at 80 cm let the waist's trace
        // pass and the capsule into it, the Character pushed out under the floor: the player fell out of the level, Q25)
        CCharacter* c = pawn_char(w);
        float hh = c ? c->half_height : 0.0f, r = c ? c->radius : 20.0f;
        float len = sqrtf(d[0] * d[0] + d[1] * d[1]), k = 1.0f;
        const float heights[3] = { 30.0f, hh, hh * 1.6f };
        for (int i = 0; i < 3; i++) {
            float a[3] = { f[0], f[1], f[2] + fminf(heights[i], 2.0f * hh - 5.0f) };
            float b[3] = { a[0] + d[0] / len * (len + r), a[1] + d[1] / len * (len + r), a[2] };
            float t = trace(a, b);
            k = fminf(k, fmaxf(fminf((t * (len + r) - r) / len, 1.0f), 0.0f));
        }
        d[0] *= k, d[1] *= k;
        // never into the void: some floor below (down off a table or a step is fine: only 60 cm under the feet stranded the
        // player on the table, Q25's headset test), the edge of the level stops the drag
        float to[3] = { f[0] + d[0], f[1] + d[1], f[2] };
        float a2[3] = { to[0], to[1], to[2] + 40.0f }, b2[3] = { to[0], to[1], to[2] - 1500.0f };
        if (trace(a2, b2) >= 1.0f) d[0] = d[1] = 0.0f;
        CTransform* t2 = ecs_get(w, V.pawn, COMP_TRANSFORM);
        if (t2) t2->location[0] += d[0], t2->location[1] += d[1];
    }
    V.origin[0] += d[0], V.origin[1] += d[1];
}

// (the safety net) the last place the pawn stood on the ground; fallen out of the level (5 m under it) or falling for 2 s:
// back there, still. Any VR level with holes or thin floors: the player never lost in the void
static void safety_net(World* w, float dt) {
    CCharacter* c = pawn_char(w);
    CTransform* t = c ? ecs_get(w, V.pawn, COMP_TRANSFORM) : NULL;
    if (!t || !project()->vr.safety_net) return;
    if (!V.start_ok) memcpy(V.start, t->location, 12), V.start_ok = true;
    V.since_rescue += dt;
    if (c->on_ground) {
        V.fall_time = 0.0f;
        V.safe_time += dt;
        // a firm place: stood on for half a second, a floor right under its feet, a meter from the last one kept
        if (V.safe_time > 0.5f) {
            V.safe_time = 0.0f;
            float a[3] = { t->location[0], t->location[1], t->location[2] }, b[3] = { a[0], a[1], a[2] - c->half_height - 30.0f };
            bool floor = trace(a, b) < 1.0f;
            const float* last = V.nsafe ? V.safes[(V.safe_head + VR_SAFE_N - 1) % VR_SAFE_N] : NULL;
            float d2 = last ? (a[0] - last[0]) * (a[0] - last[0]) + (a[1] - last[1]) * (a[1] - last[1]) + (a[2] - last[2]) * (a[2] - last[2]) : 1e9f;
            if (floor && d2 > 100.0f * 100.0f) {
                memcpy(V.safes[V.safe_head], a, 12);
                V.safe_head = (V.safe_head + 1) % VR_SAFE_N;
                if (V.nsafe < VR_SAFE_N) V.nsafe++;
            }
            if (floor) memcpy(V.safe, a, 12), V.safe_ok = true;
        }
        return;
    }
    V.fall_time += dt;
    if (!V.safe_ok) return;
    // fallen out of the level (5 m under the last ground) or falling for 6 s (a long drop is a fall, not a void)
    if (t->location[2] < V.safe[2] - 500.0f || V.fall_time > 6.0f) {
        // where to: the last firm place; rescued again soon after: an older one each time, at last where the play began
        // (a place that drops the player again was a loop of rescues, Q25's headset test)
        float to[3];
        V.rescue_depth = V.since_rescue < 3.0f ? V.rescue_depth + 1 : 0;
        if (V.rescue_depth == 0) memcpy(to, V.safe, 12);
        else if (V.rescue_depth <= V.nsafe) memcpy(to, V.safes[(V.safe_head + VR_SAFE_N - V.rescue_depth) % VR_SAFE_N], 12);
        else memcpy(to, V.start, 12);
        printf("[RealEngine] VR: the pawn fell out of the level at (%.0f, %.0f, %.0f): back to (%.0f, %.0f, %.0f) (%s)\n", t->location[0], t->location[1],
               t->location[2], to[0], to[1], to[2], V.rescue_depth == 0 ? "the last ground" : V.rescue_depth <= V.nsafe ? "an older ground" : "the start");
        fflush(stdout);
        memcpy(t->location, to, 12);
        memcpy(V.safe, to, 12);
        c->velocity[0] = c->velocity[1] = c->velocity[2] = 0.0f;
        V.fling[0] = V.fling[1] = 0.0f;
        for (int h = 0; h < 2; h++) V.h[h].dragging = false;
        V.fall_time = 0.0f, V.blink = 1.0f, V.st.rescues++, V.since_rescue = 0.0f;
        origin_from_pawn(w);
    }
}
// --- Teleport ---
void vr_pawn_teleport(World* w, const float location[3], float yaw_deg, bool keep_yaw) {
    float head[3], hf[3], hu[3];
    xr_head(head, hf, hu);
    if (!keep_yaw) {
        float pivot[3] = { head[0], head[1], 0.0f };
        turn_about(w, pivot, yaw_deg - V.yaw);
    }
    // the head (its place over the floor) lands over location, the pawn under it
    float hl[3] = { head[0], head[1], 0.0f }, hw[3];
    rotz(V.yaw, hl, hw);
    V.origin[0] = location[0] - hw[0], V.origin[1] = location[1] - hw[1], V.origin[2] = location[2];
    V.off[0] = head[0], V.off[1] = head[1];
    if (V.pawn && ecs_alive(w, V.pawn)) {
        CTransform* t = ecs_get(w, V.pawn, COMP_TRANSFORM);
        CCharacter* c = pawn_char(w);
        if (t) t->location[0] = location[0], t->location[1] = location[1], t->location[2] = location[2] + (c ? c->half_height + 2.0f : 0.0f);
        if (c) c->velocity[0] = c->velocity[1] = c->velocity[2] = 0.0f;
    }
    V.st.teleports++;
    if (V.pawn) {
        BpValue ev[1];
        memset(ev, 0, sizeof ev);
        memcpy(ev[0].v, location, 12);
        bp_fire_actor_event(w, V.pawn, BPK_EVENT_VR_TELEPORTED, ev, 1);
    }
}
// the arc from the aiming hand: a projectile's path until it hits; a floor there: valid
static void teleport_arc(World* w, int h, bool draw) {
    VrHandState* s = &V.h[h];
    const float g = 980.0f, v0 = sqrtf(V.tp_range * g); // (45 degrees over a flat floor reaches the range)
    float p[3] = { s->aim_o[0], s->aim_o[1], s->aim_o[2] }, v[3] = { s->aim_d[0] * v0, s->aim_d[1] * v0, s->aim_d[2] * v0 };
    float pts[VR_ARC_DOTS][3];
    int n = 0;
    V.tp_valid = false;
    const float step = 0.035f;
    for (int i = 0; i < 80; i++) {
        float q[3] = { p[0] + v[0] * step, p[1] + v[1] * step, p[2] + v[2] * step - 0.5f * g * step * step };
        float f = trace(p, q);
        if (i % 2 == 0 && n < VR_ARC_DOTS) memcpy(pts[n++], p, 12);
        if (f < 1.0f) {
            float hit[3] = { p[0] + (q[0] - p[0]) * f, p[1] + (q[1] - p[1]) * f, p[2] + (q[2] - p[2]) * f };
            // a floor: met falling and level under it (a wall or a ceiling is refused)
            V.tp_valid = q[2] < p[2] && floor_at(hit);
            memcpy(V.tp_target, hit, 12);
            break;
        }
        memcpy(p, q, 12);
        v[2] -= g * step;
        if (p[2] < V.origin[2] - 5000.0f) break;
    }
    if (!draw) return;
    const float ok[3] = { 0.1f, 0.65f, 1.0f }, bad[3] = { 1.0f, 0.15f, 0.1f };
    for (int i = 0; i < VR_ARC_DOTS; i++) {
        show(w, V.arc[i], i < n);
        if (i < n) place_point(w, V.arc[i], pts[i], 3.0f, 3.0f), tint(w, V.arc[i], V.tp_valid ? ok : bad, 3.0f);
    }
    show(w, V.ring, V.tp_valid);
    if (V.tp_valid) place_point(w, V.ring, V.tp_target, 60.0f, 1.0f), tint(w, V.ring, ok, 2.0f);
}
static void hide_arc(World* w) {
    for (int i = 0; i < VR_ARC_DOTS; i++) show(w, V.arc[i], false);
    show(w, V.ring, false);
}

// --- The frame ---
void vr_pawn_set_locomotion(int locomotion, int turn) { // (the project's: what the next frames read, vr.Locomotion's too)
    if (locomotion >= 0 && locomotion <= VR_LOCO_BOTH) project()->vr.locomotion = V.locomotion = locomotion;
    if (turn >= 0 && turn <= VR_TURN_NONE) project()->vr.turn = V.turn = turn;
}
void vr_pawn_set_vignette(float s) { project()->vr.vignette = V.vignette_max = fminf(fmaxf(s, 0.0f), 1.0f); }
void vr_pawn_set_hand_collision(int physical, int objects, int push) {
    ProjectVr* p = &project()->vr;
    if (physical >= 0) p->physical_hands = V.physical = physical != 0;
    if (objects >= 0) p->hand_objects = V.hand_objects = objects != 0;
    if (push >= 0) p->hand_push = V.push = push != 0;
}
void vr_pawn_set_menu(bool shown) { V.menu = shown; }

void vr_pawn_tick(World* w, float dt) {
    if (!V.playing) return;
    double t0 = re_time_seconds();
    V.time += dt;
    sync_settings();
    Entity p = g_test_pawn ? g_test_pawn : bp_player(w);
    if (p != V.pawn) V.pawn = p;
    origin_from_pawn(w);
    for (int h = 0; h < 2; h++) hand_world(h);
    const int mh = 1 - V.dom, th = V.dom; // the moving hand (teleport, walk) and the turning one: swapped for the left-handed
    const XrHand* L = xr_hand(mh);
    const XrHand* Rh = xr_hand(th);
    float head[3], hf[3], hu[3];
    xr_head(head, hf, hu);
    bool moving = false, turning = false;
    // turning: the right stick's sideways push (Both: its forward push teleports)
    float rx = Rh->stick[0];
    if (V.turn == VR_TURN_SNAP) {
        if (fabsf(rx) > 0.7f && V.snap_ready) {
            float pivot[3] = { head[0], head[1], 0.0f };
            turn_about(w, pivot, rx > 0.0f ? V.snap_angle : -V.snap_angle); // (right: the yaw up, toward +Y)
            V.snap_ready = false, V.vig_turn = 1.0f, V.st.snap_turns++;
            if (project()->vr.turn_fade) V.blink = 1.0f;
        } else if (fabsf(rx) < 0.3f)
            V.snap_ready = true;
    } else if (V.turn == VR_TURN_SMOOTH && fabsf(rx) > 0.15f) {
        float pivot[3] = { head[0], head[1], 0.0f };
        turn_about(w, pivot, rx * V.turn_speed * dt);
        turning = true;
    }
    // teleport: the left stick (Teleport) or the right stick's forward push (Both); released: go when the arc found a floor
    int aim_hand = -1;
    if (V.locomotion == VR_LOCO_TELEPORT && L->stick[1] > 0.6f) aim_hand = mh;
    if (V.locomotion == VR_LOCO_TELEPORT && Rh->stick[1] > 0.6f && aim_hand < 0) aim_hand = th;
    if (V.locomotion == VR_LOCO_BOTH && Rh->stick[1] > 0.6f) aim_hand = th;
    if (aim_hand >= 0 && !V.menu) {
        V.tp_aiming = true, V.tp_hand = aim_hand;
        teleport_arc(w, aim_hand, true);
    } else if (V.tp_aiming) {
        const XrHand* ah = xr_hand(V.tp_hand);
        if (fabsf(ah->stick[1]) < 0.3f && fabsf(ah->stick[0]) < 0.5f && V.tp_valid) {
            float tgt[3];
            memcpy(tgt, V.tp_target, 12);
            vr_pawn_teleport(w, tgt, V.yaw, true);
            buzz(V.tp_hand, 0.3f, 0.04f);
            if (project()->vr.teleport_fade > 0.0f) V.blink = 1.0f;
            V.vig_turn = 1.0f;
        }
        V.tp_aiming = false;
        hide_arc(w);
    }
    // smooth: the left stick walks where the head looks (the Character moves it: collision, gravity, stairs)
    if ((V.locomotion == VR_LOCO_SMOOTH || V.locomotion == VR_LOCO_BOTH) && !V.menu) {
        float sx = L->stick[0], sy = L->stick[1];
        float mag = sqrtf(sx * sx + sy * sy);
        if (mag > 0.15f) {
            float hfw[3];
            if (project()->vr.move_direction == 1 && L->active) memcpy(hfw, V.h[mh].aim_d, 12); // (toward where the moving hand points)
            else vr_pawn_dir_to_world(hf, hfw);
            float f[3] = { hfw[0], hfw[1], 0.0f };
            if (norm(f) < 1e-3f) f[0] = cosf(V.yaw * VR_DEG), f[1] = sinf(V.yaw * VR_DEG);
            float r[3] = { -f[1], f[0], 0.0f };
            float dir[3] = { f[0] * sy + r[0] * sx, f[1] * sy + r[1] * sx, 0.0f };
            float k = fminf(mag, 1.0f) / fmaxf(mag, 1e-4f);
            CCharacter* c = pawn_char(w);
            if (c) {
                float ks = c->max_walk_speed > 1.0f ? fminf(V.move_speed / c->max_walk_speed, 1.0f) : 1.0f;
                for (int i = 0; i < 2; i++) c->input[i] += dir[i] * k * ks;
            } else
                for (int i = 0; i < 2; i++) V.origin[i] += dir[i] * k * V.move_speed * dt;
            moving = true;
        }
    }
    // the comfort vignette: in while moving or turning smoothly, a pulse on a snap or a teleport, out over 0.25 s
    float want = (moving || turning || V.drag_move) ? 1.0f : 0.0f;
    V.drag_move = false;
    V.vig_turn = fmaxf(V.vig_turn - dt * 4.0f, 0.0f);
    float target = fmaxf(want, V.vig_turn) * V.vignette_max;
    V.vignette += (target - V.vignette) * fminf(dt * 10.0f, 1.0f);
    if (V.vignette < 0.002f) V.vignette = 0.0f;
    V.st.tick_ms = (re_time_seconds() - t0) * 1000.0;
}

// the hand's boxes: the tracked joints, else the controller's grip curled by the trigger (index) and the grip (the rest)
static void pose_hand(World* w, int h) {
    VrHandState* s = &V.h[h];
    const XrHand* x = xr_hand(h);
    bool on = V.hands && x->active && !s->held;
    if (s->sk_ent) { // (2026-11-20) the Skeletal Mesh hand: shown while the hand is, its fingers posed
        for (int i = 0; i < VR_HAND_PARTS; i++) show(w, s->parts[i], false);
        sk_hand_show(w, h, V.hands && x->active);
        if (V.hands && x->active) sk_hand_pose(w, h, x);
        return;
    }
    if (s->mesh_ent) { // the project's own hand mesh at the grip, turned by its rotation
        for (int i = 0; i < VR_HAND_PARTS; i++) show(w, s->parts[i], false);
        show(w, s->mesh_ent, V.hands && x->active);
        CTransform* mt = ecs_alive(w, s->mesh_ent) ? ecs_get(w, s->mesh_ent, COMP_TRANSFORM) : NULL;
        if (!mt || !x->active) return;
        const ProjectVr* pv = &project()->vr;
        float r0[3][3], ax[3][3];
        rotator_axes(pv->hand_mesh_rot, r0);
        for (int a = 0; a < 3; a++)
            for (int k = 0; k < 3; k++) ax[a][k] = r0[a][0] * s->world_axes[0][k] + r0[a][1] * s->world_axes[1][k] + r0[a][2] * s->world_axes[2][k];
        axes_rotator(ax, mt->rotation);
        memcpy(mt->location, s->world_pos, 12);
        float sc = pv->hand_mesh_scale > 0.0f ? pv->hand_mesh_scale : 1.0f;
        mt->scale[0] = mt->scale[1] = mt->scale[2] = sc;
        return;
    }
    for (int i = 0; i < VR_HAND_PARTS; i++) show(w, s->parts[i], on || (V.hands && x->active && s->held && i == 0));
    if (!V.hands || !x->active) return;
    const float* F = s->world_axes[0];
    const float* Rr = s->world_axes[1];
    const float* U = s->world_axes[2];
    float side = h ? 1.0f : -1.0f; // (the thumb toward the body's middle)
    if (x->tracked && V.hand_tracking) { // the joints as they are: palm wrist->middle knuckle, each finger two boxes
        float jw[26][3];
        for (int k = 0; k < 26; k++) vr_pawn_to_world(x->joint[k], jw[k]), jw[k][0] += s->shift[0], jw[k][1] += s->shift[1], jw[k][2] += s->shift[2];
        place_segment(w, s->parts[0], jw[1], jw[11], 6.0f, U);
        static const int fing[5][3] = { { 3, 4, 5 }, { 7, 8, 10 }, { 12, 13, 15 }, { 17, 18, 20 }, { 22, 23, 25 } };
        for (int f = 0; f < 5; f++) {
            place_segment(w, s->parts[1 + f * 2], jw[fing[f][0]], jw[fing[f][1]], 1.6f, U);
            place_segment(w, s->parts[2 + f * 2], jw[fing[f][1]], jw[fing[f][2]], 1.4f, U);
        }
        return;
    }
    // the controller: the palm 9 cm long along the grip, the fingers wrapped around it as the trigger and the grip close
    float pa[3], pb[3];
    for (int k = 0; k < 3; k++) pa[k] = s->world_pos[k] - F[k] * 4.0f + U[k] * 2.0f - Rr[k] * side * 1.0f, pb[k] = pa[k] + F[k] * 9.0f;
    place_segment(w, s->parts[0], pa, pb, 7.0f, U);
    for (int f = 0; f < 5; f++) {
        float curl = f == 0 ? (x->primary || x->secondary ? 0.8f : x->grip * 0.4f) : f == 1 ? x->trigger : x->grip;
        float a1 = curl * 80.0f * VR_DEG, a2 = curl * 170.0f * VR_DEG;
        float base[3], d1[3], d2[3], m1[3], m2[3];
        if (f == 0) { // the thumb: from the palm's side, forward and in
            for (int k = 0; k < 3; k++) base[k] = pa[k] + F[k] * 3.0f + Rr[k] * side * 3.5f;
            for (int k = 0; k < 3; k++) d1[k] = F[k] * cosf(a1) - Rr[k] * side * sinf(a1) * 0.6f - U[k] * sinf(a1) * 0.4f;
            for (int k = 0; k < 3; k++) d2[k] = F[k] * cosf(a2 * 0.6f) - Rr[k] * side * sinf(a2 * 0.6f) * 0.6f - U[k] * sinf(a2 * 0.6f) * 0.4f;
        } else { // the four: from the palm's front edge, across it, curling down around the grip
            float across = (2.5f - (float)f) * 1.8f;
            for (int k = 0; k < 3; k++) base[k] = pb[k] - Rr[k] * side * across * -1.0f;
            for (int k = 0; k < 3; k++) d1[k] = F[k] * cosf(a1) - U[k] * sinf(a1);
            for (int k = 0; k < 3; k++) d2[k] = F[k] * cosf(a2) - U[k] * sinf(a2);
        }
        float l1 = f == 0 ? 3.5f : 4.5f, l2 = f == 0 ? 3.0f : 3.5f;
        for (int k = 0; k < 3; k++) m1[k] = base[k] + d1[k] * l1, m2[k] = m1[k] + d2[k] * l2;
        place_segment(w, s->parts[1 + f * 2], base, m1, 1.7f, U);
        place_segment(w, s->parts[2 + f * 2], m1, m2, 1.5f, U);
    }
}

void vr_pawn_late_tick(World* w, float dt) {
    (void)dt;
    if (!V.playing) return;
    double t0 = re_time_seconds();
    safety_net(w, dt);
    origin_from_pawn(w); // (the Character moved: the play space with it)
    for (int h = 0; h < 2; h++) hand_world(h), solve_hand(w, h);
    if (++V.frame % 30u == 0u) find_body(w);
    hide_body(w);
    // grabbing: the grip's edge takes the nearest Grabbable under the hand; its release lets go (thrown, or socketed)
    for (int h = 0; h < 2; h++) {
        const XrHand* x = xr_hand(h);
        VrHandState* s = &V.h[h];
        // closed by the project's gesture (Grab Gesture; while dragging, Drag Gesture's keeps it); a controller: its grip
        const ProjectVr* pg = &project()->vr;
        float closed = s->dragging ? hand_closed(x, pg->drag_gesture) : s->held ? hand_closed(x, pg->grab_gesture)
                                   : fmaxf(hand_closed(x, pg->grab_gesture), V.drag ? hand_closed(x, pg->drag_gesture) : 0.0f);
        bool grip = closed > (s->grip_was ? 0.35f : 0.7f);
        // the hand's speed in the play space (the drag's fling: the world moves the other way); a jump past what a hand
        // moves in a frame, or a hand just found again, is the tracking's: no speed from it
        float lp[3] = { x->pos[0], x->pos[1], x->pos[2] };
        float jd[3] = { lp[0] - s->local_prev[0], lp[1] - s->local_prev[1], lp[2] - s->local_prev[2] };
        s->jumped = x->active && s->was_active && sqrtf(dot(jd, jd)) > VR_HAND_JUMP;
        if (!x->active || !s->was_active || s->jumped) memset(s->local_vel, 0, 12);
        else if (dt > 1e-4f)
            for (int k = 0; k < 3; k++) s->local_vel[k] += (jd[k] / dt - s->local_vel[k]) * 0.5f;
        memcpy(s->local_prev, lp, 12);
        s->was_active = x->active;
        if (!x->active) { // the hand lost: the drag ends without a glide, what it held is let go where it is
            s->dragging = false;
            if (s->held) {
                Entity e = s->held;
                bool body = s->held_body;
                s->held = ENTITY_NONE;
                if (body && ecs_alive(w, e)) physics_set_simulate(w, e, true);
                CTransform* lt = ecs_alive(w, e) ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
                if (lt && !s->held_nocol) lt->flags &= ~TRANSFORM_NO_COLLISION;
            }
            s->grip_was = false;
            continue;
        }
        if (s->jumped && s->dragging) memcpy(s->anchor, s->real_pos, 12); // (held where it is now: no jump of the player)
        if (grip && !s->grip_was && !s->held && x->active) {
            float d = 0.0f;
            Entity e = find_interactable(w, IA_GRAB, s->world_pos, V.grab_reach, NULL, NULL, &d, ENTITY_NONE);
            if (!e) { // (the fingertips too: an open hand stopped by them at the thing's surface still takes it)
                float pts[6][3], bd = 1e9f;
                int np = hand_points(h, pts);
                for (int i = 1; i < np; i++) {
                    float dd = 0.0f;
                    Entity f = find_interactable(w, IA_GRAB, pts[i], V.grab_reach * 0.5f, NULL, NULL, &dd, ENTITY_NONE);
                    if (f && dd < bd) bd = dd, e = f;
                }
            }
            if (e) vr_pawn_grab(w, h, e);
            else if (project()->vr.distance_grab && s->laser_actor && ecs_alive(w, s->laser_actor)) { // what the laser points at, into the hand
                const CInteractable* li = ecs_get(w, s->laser_actor, COMP_INTERACTABLE);
                if (li && (li->flags & IA_GRAB) && (li->flags & IA_ENABLED) && vr_pawn_grab(w, h, s->laser_actor)) {
                    memset(s->rel_pos, 0, 12), e = s->held, V.st.distance_grabs++;
                    CTransform* gt = ecs_get(w, e, COMP_TRANSFORM);
                    if (gt) memcpy(gt->location, s->world_pos, 12);
                }
            }
            if (!s->held && V.drag && !V.menu && hand_closed(x, pg->drag_gesture) > 0.7f) s->dragging = true, memcpy(s->anchor, s->real_pos, 12), V.fling[0] = V.fling[1] = 0.0f, V.st.drags++;
            if (V.st.grabs + V.st.drags <= 3) { // (the first few, in the log: the headset's tests read them)
                printf("[RealEngine] VR: hand %d closed (%s, grip %.2f): %s\n", h, x->tracked ? "tracked" : "controller", x->grip,
                       s->held ? "grabbed" : s->dragging ? "dragging the world" : "nothing");
                fflush(stdout);
            }
        } else if (!grip && s->grip_was && s->held)
            vr_pawn_release(w, h);
        if (!grip && s->dragging) { // let go: a glide the other way of the hand's last move
            s->dragging = false;
            float v[3], lv[3] = { -s->local_vel[0], -s->local_vel[1], 0.0f };
            vr_pawn_dir_to_world(lv, v);
            float sp = sqrtf(v[0] * v[0] + v[1] * v[1]), cap = 400.0f;
            if (sp > cap) v[0] *= cap / sp, v[1] *= cap / sp;
            V.fling[0] = v[0], V.fling[1] = v[1];
        }
        s->grip_was = grip;
        follow_hand(w, h);
    }
    { // drag locomotion: the closed hands pinned where they closed, the play space moved under them (both: their mean)
        float d[2] = { 0, 0 };
        int n = 0;
        for (int h = 0; h < 2; h++)
            if (V.h[h].dragging) d[0] += V.h[h].anchor[0] - V.h[h].real_pos[0], d[1] += V.h[h].anchor[1] - V.h[h].real_pos[1], n++; // (the real hand: the physical one stuck on the floor it grabs pulled nothing, Q25)
        if (n) {
            d[0] /= (float)n, d[1] /= (float)n;
            float dl = sqrtf(d[0] * d[0] + d[1] * d[1]);
            if (dl > VR_HAND_JUMP) // (a glitch the jump test missed: both hands re-anchored, nobody thrown)
                for (int h = 0; h < 2; h++) {
                    if (V.h[h].dragging) memcpy(V.h[h].anchor, V.h[h].real_pos, 12);
                    d[0] = d[1] = 0.0f;
                }
            move_by(w, d);
            V.drag_move = true;
        } else if (V.fling[0] != 0.0f || V.fling[1] != 0.0f) {
            float g[2] = { V.fling[0] * dt, V.fling[1] * dt };
            move_by(w, g);
            float k = fmaxf(1.0f - dt * 4.0f, 0.0f); // (it slows in about a quarter of a second)
            V.fling[0] *= k, V.fling[1] *= k;
            if (V.fling[0] * V.fling[0] + V.fling[1] * V.fling[1] < 25.0f) V.fling[0] = V.fling[1] = 0.0f;
            V.drag_move = true;
        }
        if (n || V.drag_move)
            for (int h = 0; h < 2; h++) hand_world(h), solve_hand(w, h), follow_hand(w, h); // (the hands' and the held things' places after the move)
    }
    if (V.push) // the hands push the bodies with physics they touch (not what they hold)
        for (int h = 0; h < 2; h++) {
            const VrHandState* s = &V.h[h];
            const XrHand* x = xr_hand(h);
            if (!x->active || s->held) continue;
            float v[3];
            hand_velocity(h, v);
            float sp = sqrtf(dot(v, v));
            // the real hand's points (the physical hand's drawn ones stop at the surface: the shift taken back)
            float hpts[6][3];
            int hn = hand_points(h, hpts);
            for (int q = 0; q < hn; q++)
                for (int k = 0; k < 3; k++) hpts[q][k] -= s->shift[k];
            bool closing = hand_closed(x, project()->vr.grab_gesture) > 0.35f;
            for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
                const EcsArchetype* a = &w->archetypes[ai];
                const uint64_t need = COMP_BIT(COMP_RIGID_BODY) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH);
                if ((a->mask & need) != need || !a->count) continue;
                const CRigidBody* rb = ecs_arch_column(a, COMP_RIGID_BODY);
                const Entity* ents = ecs_arch_entities(a);
                for (uint32_t i = 0; i < a->count; i++) {
                    if (!rb[i].simulate || ents[i] == V.h[0].held || ents[i] == V.h[1].held) continue;
                    float mn[3], mx[3];
                    world_box(w, ents[i], mn, mx);
                    const float* bv = rb[i].velocity;
                    // the penetration: each real point inside the box (1 cm skin) pushes it out through the nearest face, as
                    // deep as it went, at any speed (only the hand's speed pushed: an open hand laid on a cube slowly did
                    // nothing, Q25's headset test); opposite points cancel (a pinch or fist closing around a thing doesn't
                    // shove it), and a Grabbable isn't pushed by a hand closing to grab it
                    float acc[3] = { 0.0f, 0.0f, 0.0f };
                    const float r = 1.0f;
                    for (int q = 0; q < hn; q++) {
                        const float* P = hpts[q];
                        if (P[0] < mn[0] - r || P[0] > mx[0] + r || P[1] < mn[1] - r || P[1] > mx[1] + r || P[2] < mn[2] - r || P[2] > mx[2] + r) continue;
                        int best = 0;
                        float dep = 1e9f, sg = 1.0f;
                        for (int k = 0; k < 3; k++) {
                            float lo = P[k] - (mn[k] - r), hi = (mx[k] + r) - P[k]; // (in through the low face: out to +k)
                            if (lo < dep) dep = lo, best = k, sg = 1.0f;
                            if (hi < dep) dep = hi, best = k, sg = -1.0f;
                        }
                        acc[best] += sg * dep;
                    }
                    float depth = sqrtf(dot(acc, acc));
                    // (a Grabbable: only a tracked open hand's penetration pushes it; a controller's hand reaches into it
                    // open and grips there, the cube fled from it in the played test)
                    const CInteractable* ia = ecs_get(w, ents[i], COMP_INTERACTABLE);
                    const bool grabbable = ia && (ia->flags & IA_GRAB);
                    if (depth > 0.2f && !(grabbable && (closing || !x->tracked))) {
                        float dir[3] = { acc[0] / depth, acc[1] / depth, acc[2] / depth };
                        float want = fminf(depth / fmaxf(dt, 1e-3f) * 0.8f, 500.0f);
                        float along = dot(bv, dir);
                        if (along >= want) continue;
                        float add[3] = { dir[0] * (want - along), dir[1] * (want - along), dir[2] * (want - along) };
                        physics_set_linear_velocity(w, ents[i], add, true);
                        V.st.pushes++;
                        continue;
                    }
                    // a quick hand near it: up to the hand's speed along its move, never past what the body has already
                    // (a body that moves the hand back can't feed itself: Q25's played test threw a cube 1.5 km up)
                    if (sp < 30.0f) continue;
                    float near_ = 1e9f;
                    for (int q = 0; q < hn; q++) near_ = fminf(near_, box_dist(hpts[q], mn, mx));
                    if (near_ > 3.5f) continue;
                    float dir[3] = { v[0] / sp, v[1] / sp, v[2] / sp };
                    float along = dot(bv, dir);
                    if (along >= sp * 0.95f) continue;
                    float add[3] = { dir[0] * (sp - along), dir[1] * (sp - along), dir[2] * (sp - along) };
                    physics_set_linear_velocity(w, ents[i], add, true);
                    V.st.pushes++;
                }
            }
        }
    { // the head into a wall: from the pawn (at the head's height) to the head, the fixed level in between: fade or push back
        float hp[3], hf[3], hu[3], H[3], R[3], o[3] = { V.off[0], V.off[1], 0.0f };
        xr_head(hp, hf, hu), vr_pawn_to_world(hp, H);
        o[2] = hp[2];
        vr_pawn_to_world(o, R);
        float want = 0.0f;
        if (V.head_mode != VR_HEAD_NONE) {
            float f = trace_r(R, H, 8.0f);
            if (f < 1.0f) {
                float hit[3], pen[3];
                for (int k = 0; k < 3; k++) hit[k] = R[k] + (H[k] - R[k]) * f, pen[k] = H[k] - hit[k];
                float depth = sqrtf(dot(pen, pen));
                if (V.head_mode == VR_HEAD_PUSH) { // the play space moved back: the head stays outside
                    V.origin[0] -= pen[0], V.origin[1] -= pen[1];
                    off_from_pawn(w);
                } else
                    want = fminf(depth / 15.0f + 0.3f, 1.0f); // (a little in: dimmed; 15 cm: black)
                V.st.head_blocked++;
            }
        }
        V.fade_want = want;
        V.fade += (want - V.fade) * fminf(dt * 15.0f, 1.0f);
        if (V.fade < 0.01f) V.fade = 0.0f;
        float tf = project()->vr.teleport_fade > 0.0f ? project()->vr.teleport_fade : 0.12f;
        V.blink = fmaxf(V.blink - dt / tf, 0.0f);
    }
    for (int h = 0; h < 2; h++) { // poke: the index tip into a door's, a lever's, a button's box uses it (on entering)
        const XrHand* x = xr_hand(h);
        VrHandState* s = &V.h[h];
        pose_hand(w, h);
        if (!x->active || s->held) { s->poked = ENTITY_NONE; continue; }
        float pts[6][3], tip[3];
        hand_points(h, pts); // (the index tip as the physical hand has it: tracked, its joint; a controller, its reach)
        memcpy(tip, pts[x->tracked && V.hand_tracking ? 2 : 1], 12);
        float pd = 0.0f;
        Entity e = find_interactable(w, 0, tip, 2.5f, NULL, NULL, &pd, ENTITY_NONE); // (touching: the physical hand stops the tip at the surface)
        const CInteractable* ia = e ? ecs_get(w, e, COMP_INTERACTABLE) : NULL;
        if (ia && (ia->flags & (IA_GRAB | IA_GRAB_SOCKET))) e = ENTITY_NONE; // (grabbing, not poking)
        if (e && e != s->poked && interaction_interact(w, V.pawn, e)) V.st.pokes++, buzz(h, 0.5f, 0.04f);
        s->poked = e;
    }
    // the laser: the hand that last pulled its trigger (the right one first); it ends on the menu panel, an Interactable,
    // or what the trace meets
    int lh = V.dom;
    if (!xr_hand(lh)->active && xr_hand(1 - lh)->active) lh = 1 - lh;
    V.ui_hit = false;
    for (int h = 0; h < 2; h++) {
        VrHandState* s = &V.h[h];
        const XrHand* x = xr_hand(h);
        bool want = V.laser && x->active && h == lh && !s->held && !V.tp_aiming;
        s->laser_hit = false, s->laser_actor = ENTITY_NONE;
        if (!want) {
            show(w, s->laser, false);
            continue;
        }
        float end_t = V.laser_len;
        float uv[2], pt;
        bool panel = V.menu && xr_panel_hit(s->aim_o, s->aim_d, &pt, uv);
        if (panel) {
            end_t = pt, V.ui_hit = true, V.ui_uv[0] = uv[0], V.ui_uv[1] = uv[1];
            bool trig = x->trigger > 0.6f;
            if (trig && !s->trig_was) V.st.ui_clicks++, buzz(h, 0.2f, 0.02f);
            V.ui_down = trig;
        } else {
            float far_[3] = { s->aim_o[0] + s->aim_d[0] * V.laser_len, s->aim_o[1] + s->aim_d[1] * V.laser_len, s->aim_o[2] + s->aim_d[2] * V.laser_len };
            float f = trace(s->aim_o, far_);
            if (f < 1.0f) end_t = f * V.laser_len, s->laser_hit = true;
            float ad = 0.0f;
            Entity ia = find_interactable(w, 0, NULL, 0.0f, s->aim_o, s->aim_d, &ad, ENTITY_NONE);
            if (ia && ad <= end_t + 5.0f) end_t = fmaxf(ad, 1.0f), s->laser_actor = ia, s->laser_hit = true;
        }
        for (int k = 0; k < 3; k++) s->laser_end[k] = s->aim_o[k] + s->aim_d[k] * end_t;
        show(w, s->laser, true);
        float up[3] = { 0, 0, 1 };
        place_segment(w, s->laser, s->aim_o, s->laser_end, s->laser_hit ? 0.5f : 0.3f, up);
        s->trig_was = x->trigger > 0.6f;
    }
    // the player's Interactor along the laser; the trigger uses (the menu panel takes the trigger while shown)
    const VrHandState* ls = &V.h[lh];
    bool use = xr_hand(lh)->trigger > 0.6f && !V.menu;
    interaction_vr_pointer(V.laser && xr_hand(lh)->active, ls->aim_o, ls->aim_d, use);
    if (use && ls->laser_actor && !V.menu_was) V.st.interacts++;
    V.menu_was = V.menu;
    { // a jump of the play space past 1 m in a frame that no teleport made: in the log, with what moved it (the headset tests)
        float dj[3] = { V.origin[0] - V.last_origin[0], V.origin[1] - V.last_origin[1], V.origin[2] - V.last_origin[2] };
        float pz = 0.0f, pf[3];
        if (pawn_feet(w, pf)) pz = pf[2];
        if (V.frame > 2 && V.st.teleports == V.teleports_seen && dot(dj, dj) > 100.0f * 100.0f) {
            printf("[RealEngine] VR: the play space jumped %.0f cm (%.0f %.0f %.0f): dragging %d/%d, glide %.0f cm/s, the pawn's feet at z %.0f, rescues %u\n",
                   sqrtf(dot(dj, dj)), dj[0], dj[1], dj[2], V.h[0].dragging, V.h[1].dragging, sqrtf(V.fling[0] * V.fling[0] + V.fling[1] * V.fling[1]), pz, V.st.rescues);
            fflush(stdout);
        }
        memcpy(V.last_origin, V.origin, 12), V.teleports_seen = V.st.teleports;
    }
    V.st.tick_ms += (re_time_seconds() - t0) * 1000.0;
}

bool vr_pawn_ui_pointer(float uv[2], bool* down) {
    if (!V.playing || !V.ui_hit) return false;
    uv[0] = V.ui_uv[0], uv[1] = V.ui_uv[1];
    if (down) *down = V.ui_down;
    return true;
}

bool vr_pawn_laser(int hand, float hit[3], Entity* actor) {
    const VrHandState* s = &V.h[hand ? 1 : 0];
    if (hit) memcpy(hit, s->laser_end, 12);
    if (actor) *actor = s->laser_actor;
    return s->laser_hit;
}

// --- The self-test (--vr-test): the simulated headset over a scene of its own (a floor at z 0, a wall) ---
static float g_wall_x = 1e9f, g_wall_top = 600.0f;
static float test_trace(const float a[3], const float b[3]) {
    float best = 1.0f;
    if (a[2] > 0.0f && b[2] <= 0.0f) best = a[2] / (a[2] - b[2]); // the floor
    // the wall: x in [g_wall_x, g_wall_x + 20], z in [0, top], any y
    if (b[0] != a[0]) {
        float t = (g_wall_x - a[0]) / (b[0] - a[0]);
        float z = a[2] + (b[2] - a[2]) * t;
        if (t >= 0.0f && t < best && z >= 0.0f && z <= g_wall_top) best = t;
    }
    return best;
}
static void tick_n(World* w, int n, float dt) {
    for (int i = 0; i < n; i++) vr_pawn_tick(w, dt), vr_pawn_late_tick(w, dt);
}
static void hand_set(int h, const float pos[3], const float fwd[3], float trigger, float grip, float sx, float sy) {
    XrHand x;
    memset(&x, 0, sizeof x);
    x.active = true;
    memcpy(x.pos, pos, 12), memcpy(x.fwd, fwd, 12), x.up[2] = 1.0f;
    memcpy(x.aim_pos, pos, 12), memcpy(x.aim_fwd, fwd, 12);
    x.trigger = trigger, x.grip = grip, x.stick[0] = sx, x.stick[1] = sy;
    xr_sim_hand(h, &x);
}
static Entity test_cube(World* w, float x, float y, float z, uint16_t ia_flags, bool body) {
    uint64_t m = COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_INTERACTABLE);
    if (body) m |= COMP_BIT(COMP_RIGID_BODY);
    Entity e = ecs_create(w, m);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = z;
    t->scale[0] = t->scale[1] = t->scale[2] = 0.1f; // (10 cm)
    t->flags = MOBILITY_MOVABLE;
    CInteractable* ia = ecs_get(w, e, COMP_INTERACTABLE);
    ia->flags = (uint16_t)(IA_ENABLED | ia_flags);
    if (body) ((CRigidBody*)ecs_get(w, e, COMP_RIGID_BODY))->simulate = 1;
    return e;
}

int vr_pawn_selftest(void) {
    int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: vr: "), printf(__VA_ARGS__), printf("\n"), fails++; } } while (0)
    double t0 = re_time_seconds();
    // game.ini's keys
    XrConfig c;
    xr_config_defaults(&c);
    xr_config_key(&c, "vr", "1"), xr_config_key(&c, "vr_refresh", "120"), xr_config_key(&c, "vr_foveation", "9");
    xr_config_key(&c, "vr_render_scale", "0.2"), xr_config_key(&c, "vr_tracking", "1"), xr_config_key(&c, "vr_quality", "4");
    CHECK(c.start && c.refresh_hz == 120 && c.foveation == 3 && c.render_scale == 0.5f && c.tracking == 1 && c.quality == 4 &&
          !xr_config_key(&c, "volume", "1"), "game.ini's VR keys (start %d, %d Hz, foveation %d, scale %.2f)", c.start, c.refresh_hz, c.foveation, c.render_scale);
    xr_config_defaults(&c);
    xr_set_config(&c);
#ifdef _WIN32
    _putenv("REALENGINE_XR_SIM_ONLY=1");
#else
    setenv("REALENGINE_XR_SIM_ONLY", "1", 1);
#endif
    CHECK(xr_init(true) && xr_info()->sim && xr_active(), "the simulated headset starts");
    CHECK(xr_info()->eye_w == 1280 && xr_info()->eye_h == 1400 && xr_frame_period_ms() > 11.0 && xr_frame_period_ms() < 11.2, "simulated eyes 1280 x 1400 at 90 Hz");
    XrEye eyes[2];
    float head[3] = { 0, 0, 170 };
    xr_sim_head(head, 0.0f, 0.0f);
    CHECK(xr_begin_frame(eyes) && fabsf(eyes[1].pos[1] - eyes[0].pos[1] - 6.4f) < 0.01f && fabsf(eyes[0].pos[2] - 170.0f) < 0.01f, "the eyes 6.4 cm apart at the head");
    static World w;
    if (!ecs_init(&w)) return fails + 1;
    project()->vr.locomotion = VR_LOCO_TELEPORT, project()->vr.turn = VR_TURN_SNAP, project()->vr.snap_angle = 45.0f;
    project()->vr.vignette = 0.6f, project()->vr.teleport_range = 900.0f, project()->vr.tracking = VR_TRACK_FLOOR;
    g_test_trace = test_trace;
    vr_pawn_begin_play(&w);
    const float dt = 1.0f / 90.0f;
    const float fwd[3] = { 1, 0, 0 }, zero[3] = { 0, 0, 0 };
    float rp[3] = { 20, 20, 120 }, lp[3] = { 20, -20, 120 };
    hand_set(0, lp, fwd, 0, 0, 0, 0), hand_set(1, rp, fwd, 0, 0, 0, 0);
    // the play space: a point of it into the world
    vr_pawn_teleport(&w, zero, 0.0f, false);
    float hw[3];
    vr_pawn_to_world(head, hw);
    CHECK(fabsf(hw[0]) < 0.01f && fabsf(hw[1]) < 0.01f && fabsf(hw[2] - 170.0f) < 0.01f, "the head over the teleport's spot (%.2f %.2f %.2f)", hw[0], hw[1], hw[2]);
    // snap turns about the head: the right stick pushed right twice, let go between
    float hx[3] = { 30, 0, 170 }, before[3], after[3];
    xr_sim_head(hx, 0.0f, 0.0f);
    vr_pawn_to_world(hx, before);
    hand_set(1, rp, fwd, 0, 0, 1.0f, 0), tick_n(&w, 3, dt);
    hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 2, dt);
    hand_set(1, rp, fwd, 0, 0, 1.0f, 0), tick_n(&w, 2, dt);
    float vig = V.vignette;
    hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 1, dt);
    vr_pawn_to_world(hx, after);
    float yaw;
    float org[3];
    vr_pawn_origin(org, &yaw);
    CHECK(V.st.snap_turns == 2 && fabsf(yaw - 90.0f) < 0.01f, "two snap turns of 45 degrees (%u, yaw %.1f)", V.st.snap_turns, yaw);
    CHECK(fabsf(after[0] - before[0]) < 0.01f && fabsf(after[1] - before[1]) < 0.01f, "the head stays where it was through the turns");
    CHECK(vig > 0.05f, "the snap's comfort pulse darkens the edge (%.2f)", vig);
    // smooth turn: 0.5 s at 90 degrees a second
    vr_pawn_set_locomotion(-1, VR_TURN_SMOOTH);
    hand_set(1, rp, fwd, 0, 0, 1.0f, 0), tick_n(&w, 45, dt);
    hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 1, dt);
    vr_pawn_origin(org, &yaw);
    CHECK(fabsf(yaw - 135.0f) < 1.0f, "a smooth turn of 0.5 s at 90 deg/s (yaw %.1f)", yaw);
    vr_pawn_set_locomotion(-1, VR_TURN_SNAP);
    xr_sim_head(head, 0.0f, 0.0f);
    vr_pawn_teleport(&w, zero, 0.0f, false);
    // teleport: the left stick aims an arc 45 degrees up; a floor ahead: valid; released: the head lands over it
    const float up45[3] = { 0.7071f, 0, 0.7071f };
    hand_set(0, lp, up45, 0, 0, 0, 1.0f), tick_n(&w, 2, dt);
    float tgt[3] = { V.tp_target[0], V.tp_target[1], V.tp_target[2] };
    CHECK(V.tp_aiming && V.tp_valid && tgt[0] > 700.0f && tgt[0] < 1150.0f && fabsf(tgt[2]) < 0.5f, "the arc lands on the floor ahead (%.0f %.0f %.0f, valid %d)", tgt[0], tgt[1], tgt[2], V.tp_valid);
    hand_set(0, lp, up45, 0, 0, 0, 0), tick_n(&w, 1, dt);
    vr_pawn_to_world(head, hw);
    CHECK(V.st.teleports == 3 && fabsf(hw[0] - tgt[0]) < 0.5f && fabsf(hw[1] - tgt[1]) < 0.5f, "teleported: the head over the arc's end (%u, %.0f %.0f)", V.st.teleports, hw[0], hw[1]);
    // a wall 4 m ahead, taller than the arc: refused (met rising, no floor)
    g_wall_x = hw[0] + 400.0f;
    hand_set(0, lp, up45, 0, 0, 0, 1.0f), tick_n(&w, 2, dt);
    CHECK(V.tp_aiming && !V.tp_valid, "the arc into a wall is refused");
    hand_set(0, lp, up45, 0, 0, 0, 0), tick_n(&w, 1, dt);
    CHECK(V.st.teleports == 3, "no teleport into the wall (%u)", V.st.teleports);
    g_wall_x = 1e9f;
    // smooth walking (no pawn: the play space itself): 1 s at 200 cm/s where the head looks (+X)
    vr_pawn_set_locomotion(VR_LOCO_SMOOTH, -1);
    vr_pawn_origin(before, NULL);
    hand_set(0, lp, fwd, 0, 0, 0, 1.0f), tick_n(&w, 90, dt);
    float vig_walk = V.vignette;
    hand_set(0, lp, fwd, 0, 0, 0, 0), tick_n(&w, 1, dt);
    vr_pawn_origin(after, NULL);
    CHECK(fabsf(after[0] - before[0] - 200.0f) < 5.0f && fabsf(after[1] - before[1]) < 1.0f, "walked 200 cm forward (%.1f, %.1f)", after[0] - before[0], after[1] - before[1]);
    CHECK(vig_walk > 0.5f, "the vignette while walking (%.2f)", vig_walk);
    vr_pawn_set_locomotion(VR_LOCO_TELEPORT, -1);
    // grab: a Grabbable body under the right hand; the grip takes it, it follows; let go moving at 300 cm/s: thrown
    vr_pawn_teleport(&w, zero, 0.0f, false);
    tick_n(&w, 1, dt);
    float rw[3];
    vr_pawn_to_world(rp, rw);
    Entity ball = test_cube(&w, rw[0] + 3.0f, rw[1], rw[2], IA_GRAB, true);
    hand_set(1, rp, fwd, 0, 1.0f, 0, 0), tick_n(&w, 1, dt);
    CHECK(vr_pawn_held(1) == ball, "the grip took the ball (%u)", vr_pawn_held(1));
    CHECK(!((CRigidBody*)ecs_get(&w, ball, COMP_RIGID_BODY))->simulate, "its physics off while held");
    float p2[3] = { rp[0], rp[1], rp[2] };
    for (int i = 0; i < 12; i++) p2[0] += 300.0f * dt, hand_set(1, p2, fwd, 0, 1.0f, 0, 0), tick_n(&w, 1, dt);
    const CTransform* bt = ecs_get(&w, ball, COMP_TRANSFORM);
    float hw2[3];
    vr_pawn_to_world(p2, hw2);
    { // (the drawn hand stopped at the ball's surface when it closed: the offset is the drawn hand's, kept while it follows)
        static float off0;
        off0 = bt->location[0] - hw2[0];
        hand_set(1, p2, fwd, 0, 1.0f, 0, 0), tick_n(&w, 1, dt);
        bt = ecs_get(&w, ball, COMP_TRANSFORM);
        CHECK(fabsf(bt->location[0] - hw2[0] - off0) < 0.05f && off0 > 2.9f && off0 < 9.0f && fabsf(bt->location[1] - hw2[1]) < 0.05f,
              "it follows the hand where it was taken (%.2f)", off0);
    }
    hand_set(1, p2, fwd, 0, 0.0f, 0, 0), tick_n(&w, 1, dt);
    CHECK(!vr_pawn_held(1) && V.st.throws == 1 && fabsf(V.st.last_throw_speed - 300.0f) < 30.0f, "let go: thrown at the hand's speed (%.0f cm/s)", V.st.last_throw_speed);
    CHECK(((CRigidBody*)ecs_get(&w, ball, COMP_RIGID_BODY))->simulate, "its physics back on");
    // snapped: held as made, at the hand
    Entity gun = test_cube(&w, rw[0] + 5.0f, rw[1] + 2.0f, rw[2], IA_GRAB | IA_GRAB_SNAP, false);
    ((CTransform*)ecs_get(&w, gun, COMP_TRANSFORM))->rotation[1] = 70.0f;
    hand_set(1, rp, fwd, 0, 1.0f, 0, 0), tick_n(&w, 2, dt); // (held: no longer a box the hand stops at)
    const CTransform* gt = ecs_get(&w, gun, COMP_TRANSFORM);
    CHECK(vr_pawn_held(1) == gun && fabsf(gt->location[0] - rw[0]) < 0.05f && fabsf(gt->rotation[1]) < 0.1f, "snapped to the hand's pose (%.2f, yaw %.1f)", gt->location[0] - rw[0], gt->rotation[1]);
    // a Grab Socket 10 cm from where it is let go: snapped into it; one asking a tag the gun hasn't: refused
    Entity sock = test_cube(&w, rw[0], rw[1] + 10.0f, rw[2], IA_GRAB_SOCKET, false);
    ((CTransform*)ecs_get(&w, sock, COMP_TRANSFORM))->rotation[1] = 30.0f;
    hand_set(1, rp, fwd, 0, 0.0f, 0, 0), tick_n(&w, 1, dt);
    gt = ecs_get(&w, gun, COMP_TRANSFORM);
    CHECK(V.st.socketed == 1 && fabsf(gt->location[1] - rw[1] - 10.0f) < 0.05f && fabsf(gt->rotation[1] - 30.0f) < 0.1f, "snapped into the Grab Socket (%u)", V.st.socketed);
    snprintf(((CInteractable*)ecs_get(&w, sock, COMP_INTERACTABLE))->require, IA_TAG, "Item.Key");
    hand_set(1, rp, fwd, 0, 1.0f, 0, 0), tick_n(&w, 1, dt);
    hand_set(1, rp, fwd, 0, 0.0f, 0, 0), tick_n(&w, 1, dt);
    CHECK(V.st.socketed == 1, "a socket asking Item.Key refuses it");
    ecs_destroy(&w, sock), ecs_destroy(&w, gun), ecs_destroy(&w, ball);
    // the laser on an Interactable 3 m ahead; the player's Interactor looks along it and the trigger uses it
    Entity door = test_cube(&w, rw[0] + 300.0f, rw[1], rw[2], 0, false);
    Entity me = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_INTERACTOR));
    ecs_component_defaults(COMP_INTERACTOR, ecs_get(&w, me, COMP_INTERACTOR));
    ((CInteractor*)ecs_get(&w, me, COMP_INTERACTOR))->flags &= ~(IAR_PROMPT | IAR_SIGHT);
    ((CInteractor*)ecs_get(&w, me, COMP_INTERACTOR))->range = 400.0f;
    interaction_begin_play(&w);
    interaction_after_begin_play(&w, ENTITY_NONE);
    hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 1, dt);
    float hit[3];
    Entity act = ENTITY_NONE;
    CHECK(vr_pawn_laser(1, hit, &act) && act == door && fabsf(hit[0] - (rw[0] + 295.0f)) < 1.0f, "the laser ends on the Interactable (%u, %.1f)", act, hit[0] - rw[0]);
    interaction_tick(&w, dt, me);
    CHECK(interaction_focused(&w, me) == door, "the Interactor looks along the laser (%u)", interaction_focused(&w, me));
    uint32_t used = interaction_stats()->interactions;
    hand_set(1, rp, fwd, 1.0f, 0, 0, 0), tick_n(&w, 1, dt);
    interaction_tick(&w, dt, me);
    CHECK(interaction_stats()->interactions == used + 1, "the trigger used it (%u)", interaction_stats()->interactions - used);
    hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 1, dt), interaction_tick(&w, dt, me);
    // poke: the index tip (a controller's: 9 cm ahead of the grip) into the door's box uses it, once on entering
    used = interaction_stats()->interactions;
    float near_door[3] = { rp[0] + 291.0f, rp[1], rp[2] };
    hand_set(1, near_door, fwd, 0, 0, 0, 0), tick_n(&w, 3, dt);
    CHECK(V.st.pokes == 1 && interaction_stats()->interactions == used + 1, "the index tip touching the door used it once (%u pokes)", V.st.pokes);
    hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 1, dt);
    interaction_end_play(&w);
    ecs_destroy(&w, door), ecs_destroy(&w, me);
    // the menu panel 1 m ahead (play space): the laser's point on it as the UI's pointer, the trigger its click
    float pp[3] = { rp[0] + 100.0f, rp[1], rp[2] };
    xr_panel_place(pp, fwd, 128.0f, 80.0f, true);
    vr_pawn_set_menu(true);
    tick_n(&w, 1, dt);
    float uv[2] = { -1, -1 };
    bool down = true;
    CHECK(vr_pawn_ui_pointer(uv, &down) && fabsf(uv[0] - 0.5f) < 0.01f && fabsf(uv[1] - 0.5f) < 0.01f && !down, "the laser at the panel's middle (%.3f %.3f)", uv[0], uv[1]);
    const float right_up[3] = { 0.98f, 0.12f, 0.12f };
    hand_set(1, rp, right_up, 1.0f, 0, 0, 0), tick_n(&w, 1, dt);
    CHECK(vr_pawn_ui_pointer(uv, &down) && uv[0] > 0.55f && uv[1] < 0.45f && down && V.st.ui_clicks == 1, "right and up on it, the trigger down (%.3f %.3f, %u)", uv[0], uv[1], V.st.ui_clicks);
    vr_pawn_set_menu(false);
    xr_panel_place(pp, fwd, 0.0f, 0.0f, false);
    hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 1, dt);
    CHECK(!vr_pawn_ui_pointer(uv, &down), "the panel hidden: no pointer");
    // a tracked hand: the pinch (thumb and index tips 1 cm apart) and the fist (the three tips by the palm)
    { // tracked hands made as hands: each finger 4 bones (2.5 cm each) bent by an angle at every joint; a straight hand, a
        // relaxed open one (15 degrees a joint: what a hand at rest is) and a fist (80); the pinch: thumb tip on the index's
        XrHand th;
        const float bends[3] = { 0.0f, 15.0f, 80.0f };
        float grips[3], pinches[3];
        for (int c = 0; c < 3; c++) {
            memset(&th, 0, sizeof th);
            for (int f = 0; f < 4; f++) { // index, middle, ring, little: from x = 5, in a row across y, bending down in z
                int meta = 6 + f * 5;
                float p[3] = { 5.0f, (float)f * 2.0f, 100.0f }, ang = 0.0f;
                for (int k = 0; k <= 4; k++) {
                    memcpy(th.joint[meta + k], p, 12);
                    if (k > 0) ang += bends[c] * 0.0174533f;
                    p[0] += cosf(ang) * 2.5f, p[2] -= sinf(ang) * 2.5f;
                }
            }
            th.joint[0][0] = 3.0f, th.joint[0][1] = 3.0f, th.joint[0][2] = 100.0f;
            memcpy(th.joint[5], c == 2 ? th.joint[10] : (float[3]){ 6.0f, -6.0f, 100.0f }, 12); // (the thumb tip on the index tip: the fist pinches)
            for (int k = 0; k < 8; k++) th.active = false, xr_hand_from_joints(&th); // (smoothed: a few frames; no controller, as xr.c reads a tracked hand)
            grips[c] = th.grip, pinches[c] = th.pinch;
        }
        CHECK(grips[0] < 0.1f && grips[1] < 0.25f && grips[2] > 0.8f, "the fist read as a hand closes: straight %.2f, relaxed %.2f, a fist %.2f", grips[0], grips[1], grips[2]);
        CHECK(pinches[0] < 0.1f && pinches[2] > 0.9f && th.trigger > 0.9f, "the pinch: apart %.2f, touching %.2f", pinches[0], pinches[2]);
        CHECK(th.gesture == XR_GESTURE_FIST || th.gesture == XR_GESTURE_PINCH, "the closed hand's shape (%s)", g_xr_gesture_names[th.gesture]);
        // an open hand, the fingers together (1.6 cm apart: the middle beside the index), the thumb resting by the index's
        // base: no touch, so nothing grabbed nor held by it (things stuck to the open hand, Q25's headset test)
        memset(&th, 0, sizeof th);
        for (int f = 0; f < 4; f++)
            for (int k = 0; k <= 4; k++) th.joint[6 + f * 5 + k][0] = 5.0f + 2.5f * (float)k, th.joint[6 + f * 5 + k][1] = 1.6f * (float)f, th.joint[6 + f * 5 + k][2] = 100.0f;
        th.joint[0][0] = 3.0f, th.joint[0][1] = 3.0f, th.joint[0][2] = 100.0f;
        th.joint[2][0] = 4.0f, th.joint[2][1] = -3.0f, th.joint[2][2] = 100.0f;
        th.joint[3][0] = 6.0f, th.joint[3][1] = -2.5f, th.joint[3][2] = 100.0f;
        th.joint[4][0] = 8.0f, th.joint[4][1] = -2.0f, th.joint[4][2] = 100.0f;
        th.joint[5][0] = 8.0f, th.joint[5][1] = -1.2f, th.joint[5][2] = 100.0f; // (1.2 cm from the index's base bone)
        for (int k = 0; k < 10; k++) th.active = false, xr_hand_from_joints(&th);
        CHECK(th.touch[0] < 0.1f && th.touch[1] < 0.1f && hand_closed(&th, VR_GESTURE_FIST_OR_TOUCH) < 0.3f && hand_closed(&th, VR_GESTURE_ANY_TO_INDEX) < 0.3f,
              "an open hand, fingers together: no touch (thumb %.2f, middle %.2f, grip %.2f)", th.touch[0], th.touch[1], th.grip);
        // the thumb's tip on the index's middle bone: a pinch
        th.joint[5][0] = 11.0f, th.joint[5][1] = -0.6f;
        for (int k = 0; k < 10; k++) th.active = false, xr_hand_from_joints(&th);
        CHECK(th.touch[0] > 0.9f && hand_closed(&th, VR_GESTURE_PINCH) > 0.9f, "the thumb on the index's middle: a touch (%.2f)", th.touch[0]);
        // the middle finger's tip on the index: Any Finger to Index closes the hand, the fist doesn't
        th.touch[1] = 1.0f, th.touch[0] = 0.0f, th.grip = 0.0f, th.tracked = true;
        CHECK(hand_closed(&th, VR_GESTURE_ANY_TO_INDEX) > 0.9f && hand_closed(&th, VR_GESTURE_FIST) < 0.1f && hand_closed(&th, VR_GESTURE_FIST_OR_TOUCH) > 0.9f &&
              hand_closed(&th, VR_GESTURE_PINCH) < 0.1f, "the grab gestures: a finger on the index");
    }
    // drag locomotion: the right hand closed in the air (nothing to grab) and pulled 30 cm back: the play space 30 cm forward;
    // let go while moving: a glide, slowing to a stop
    vr_pawn_teleport(&w, zero, 0.0f, false);
    tick_n(&w, 1, dt);
    vr_pawn_origin(before, NULL);
    float dp[3] = { rp[0], rp[1], rp[2] };
    hand_set(1, dp, fwd, 0, 1.0f, 0, 0), tick_n(&w, 1, dt);
    for (int i = 0; i < 10; i++) dp[0] -= 3.0f, hand_set(1, dp, fwd, 0, 1.0f, 0, 0), tick_n(&w, 1, dt);
    vr_pawn_origin(after, NULL);
    CHECK(V.st.drags == 1 && fabsf(after[0] - before[0] - 30.0f) < 0.5f && fabsf(after[1] - before[1]) < 0.5f, "dragged 30 cm forward (%.2f, %u drags)", after[0] - before[0], V.st.drags);
    hand_set(1, dp, fwd, 0, 0.0f, 0, 0), tick_n(&w, 1, dt);
    float glide0[3], glide1[3];
    vr_pawn_origin(glide0, NULL);
    tick_n(&w, 90, dt);
    vr_pawn_origin(glide1, NULL);
    CHECK(glide1[0] - after[0] > 5.0f && glide1[0] - after[0] < 120.0f && V.fling[0] == 0.0f, "let go: a glide forward that stops (%.1f cm)", glide1[0] - after[0]);
    // the tracking's glitches while dragging (the headset test: the player thrown across the level): the hand seen 1 m away
    // for a frame, then lost and found again 2 m away still closed: the player never moves with them
    float g0[3], g1[3];
    vr_pawn_origin(g0, NULL);
    float gp[3] = { rp[0], rp[1], rp[2] };
    hand_set(1, gp, fwd, 0, 1.0f, 0, 0), tick_n(&w, 2, dt);
    gp[0] += 100.0f, hand_set(1, gp, fwd, 0, 1.0f, 0, 0), tick_n(&w, 1, dt);
    XrHand lost;
    memset(&lost, 0, sizeof lost);
    lost.grip = 1.0f; // (a lost hand's last values: xr.c zeroes them; the pawn must not trust them either)
    xr_sim_hand(1, &lost), tick_n(&w, 3, dt);
    gp[0] -= 300.0f, hand_set(1, gp, fwd, 0, 1.0f, 0, 0), tick_n(&w, 2, dt);
    hand_set(1, gp, fwd, 0, 0.0f, 0, 0), tick_n(&w, 30, dt);
    vr_pawn_origin(g1, NULL);
    CHECK(fabsf(g1[0] - g0[0]) < 1.0f && fabsf(g1[1] - g0[1]) < 1.0f, "a hand's tracking glitch never moves the player (%.1f %.1f cm)", g1[0] - g0[0], g1[1] - g0[1]);
    { // physical hands: a wall 30 cm ahead of the hand, the real hand 50 cm on: the drawn one stops at the wall; what it holds too
        vr_pawn_teleport(&w, zero, 0.0f, false);
        hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 2, dt);
        float hw0[3];
        vr_pawn_to_world(rp, hw0);
        g_wall_x = hw0[0] + 30.0f;
        float in[3] = { rp[0] + 50.0f, rp[1], rp[2] };
        uint32_t blocked = V.st.hand_blocked;
        hand_set(1, in, fwd, 0, 0, 0, 0), tick_n(&w, 2, dt);
        float hp6[6][3];
        int hn = hand_points(1, hp6);
        float far_x = -1e9f;
        for (int i = 0; i < hn; i++) far_x = fmaxf(far_x, hp6[i][0]);
        CHECK(far_x <= g_wall_x + 0.1f && far_x > g_wall_x - 2.0f && V.st.hand_blocked > blocked,
              "the physical hand stopped at the wall, its fingertips at it (%.2f, the wall %.2f)", far_x, g_wall_x);
        hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 2, dt);
        Entity box = test_cube(&w, hw0[0] + 2.0f, hw0[1], hw0[2], IA_GRAB, false);
        hand_set(1, rp, fwd, 0, 1.0f, 0, 0), tick_n(&w, 1, dt);
        for (int i = 0; i < 10; i++) {
            float st[3] = { rp[0] + 5.0f * (float)(i + 1), rp[1], rp[2] };
            hand_set(1, st, fwd, 0, 1.0f, 0, 0), tick_n(&w, 1, dt);
        }
        const CTransform* bxt = ecs_get(&w, box, COMP_TRANSFORM);
        CHECK(vr_pawn_held(1) == box && bxt->location[0] <= g_wall_x + 0.1f, "what the hand holds stopped at the wall too (%.2f)", bxt->location[0]);
        hand_set(1, rp, fwd, 0, 0.0f, 0, 0), tick_n(&w, 2, dt);
        ecs_destroy(&w, box);
        // the head into the wall: faded to black; with Push Back the player moved so it stays out
        float hin[3] = { 60.0f, 0.0f, 170.0f };
        xr_sim_head(hin, 0.0f, 0.0f);
        tick_n(&w, 20, dt);
        CHECK(vr_pawn_fade() > 0.5f && V.st.head_blocked > 0, "the head in the wall: the view faded (%.2f)", vr_pawn_fade());
        project()->vr.head_collision = VR_HEAD_PUSH;
        tick_n(&w, 2, dt);
        float hwp[3];
        vr_pawn_to_world(hin, hwp);
        CHECK(hwp[0] <= g_wall_x + 0.5f, "Push Back: the head kept out of the wall (%.2f, the wall %.2f)", hwp[0], g_wall_x);
        project()->vr.head_collision = VR_HEAD_FADE;
        xr_sim_head(head, 0.0f, 0.0f);
        g_wall_x = 1e9f;
        tick_n(&w, 30, dt);
        CHECK(vr_pawn_fade() < 0.05f, "out of the wall: the view back (%.2f)", vr_pawn_fade());
    }
    { // the whole hand against a cube (an Interactable, not grabbed): the hand pushed 15 cm into it stays outside, the
        // fingertips too
        vr_pawn_teleport(&w, zero, 0.0f, false);
        hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 2, dt);
        float hw0[3];
        vr_pawn_to_world(rp, hw0);
        Entity cube = test_cube(&w, hw0[0] + 25.0f, hw0[1], hw0[2], 0, false); // (10 cm: its near face 20 cm ahead)
        float into[3] = { rp[0] + 15.0f, rp[1], rp[2] };
        for (int i = 0; i < 4; i++) hand_set(1, into, fwd, 0, 0, 0, 0), tick_n(&w, 1, dt);
        float pts[6][3], mn[3], mx[3];
        int np = hand_points(1, pts);
        world_box(&w, cube, mn, mx);
        bool in = false;
        for (int i = 0; i < np; i++)
            if (pts[i][0] > mn[0] + 0.1f && pts[i][0] < mx[0] && pts[i][1] > mn[1] && pts[i][1] < mx[1] && pts[i][2] > mn[2] && pts[i][2] < mx[2]) in = true;
        CHECK(!in, "the open hand pushed into a cube: no fingertip inside it (the tip at %.1f, the face at %.1f)", pts[1][0], mn[0]);
        // the dev's choice: the hands collide with the level only (Set VR Hand Collision: With Objects off): the hand goes in
        vr_pawn_set_hand_collision(-1, 0, -1);
        for (int i = 0; i < 4; i++) hand_set(1, into, fwd, 0, 0, 0, 0), tick_n(&w, 1, dt);
        np = hand_points(1, pts);
        in = false;
        for (int i = 0; i < np; i++)
            if (pts[i][0] > mn[0] + 0.1f && pts[i][0] < mx[0] && pts[i][1] > mn[1] && pts[i][1] < mx[1] && pts[i][2] > mn[2] && pts[i][2] < mx[2]) in = true;
        CHECK(in && project()->vr.hand_objects == false, "With Objects off: the hand goes into the cube (the level only stops it)");
        vr_pawn_set_hand_collision(-1, 1, -1);
        ecs_destroy(&w, cube);
        hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 2, dt);
    }
    { // the hands push a body with physics they touch; distance grab: the grip on what the laser points at pulls it in
        vr_pawn_teleport(&w, zero, 0.0f, false);
        hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 2, dt);
        float hw1[3];
        vr_pawn_to_world(rp, hw1);
        Entity body = test_cube(&w, hw1[0] + 30.0f, hw1[1], hw1[2], 0, true);
        uint32_t pushes = V.st.pushes;
        float pp2[3] = { rp[0], rp[1], rp[2] };
        for (int i = 0; i < 12; i++) pp2[0] += 3.0f, hand_set(1, pp2, fwd, 0, 0, 0, 0), tick_n(&w, 1, dt); // (270 cm/s into it)
        CHECK(V.st.pushes > pushes, "the moving hand pushed the body (%u)", V.st.pushes - pushes);
        ecs_destroy(&w, body);
        hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 2, dt);
        // an open hand laid slowly on it (18 cm/s, under the quick push's 30): its penetration pushes it all the same
        body = test_cube(&w, hw1[0] + 30.0f, hw1[1], hw1[2], 0, true);
        pushes = V.st.pushes;
        const float bx0 = ((const CTransform*)ecs_get(&w, body, COMP_TRANSFORM))->location[0];
        float slow_v = 0.0f;
        pp2[0] = rp[0];
        for (int i = 0; i < 100; i++) {
            pp2[0] += 0.2f, hand_set(1, pp2, fwd, 0, 0, 0, 0), tick_n(&w, 1, dt);
            slow_v = fmaxf(slow_v, ((const CRigidBody*)ecs_get(&w, body, COMP_RIGID_BODY))->velocity[0]);
        }
        const float bx1 = ((const CTransform*)ecs_get(&w, body, COMP_TRANSFORM))->location[0];
        CHECK(V.st.pushes > pushes && (slow_v > 1.0f || bx1 - bx0 > 0.5f), "a slow open hand pushed the body by touching it (%u pushes, %.1f cm/s, moved %.1f cm)",
              V.st.pushes - pushes, slow_v, bx1 - bx0);
        ecs_destroy(&w, body);
        // a controller reaching slowly into a Grabbable (to grip it there): not pushed away
        body = test_cube(&w, hw1[0] + 30.0f, hw1[1], hw1[2], IA_GRAB, true);
        hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 2, dt);
        pushes = V.st.pushes;
        pp2[0] = rp[0];
        for (int i = 0; i < 100; i++) pp2[0] += 0.2f, hand_set(1, pp2, fwd, 0, 0, 0, 0), tick_n(&w, 1, dt);
        CHECK(V.st.pushes == pushes, "a controller reaching slowly into a Grabbable doesn't push it (%u pushes)", V.st.pushes - pushes);
        ecs_destroy(&w, body);
        hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 2, dt);
        Entity far_cube = test_cube(&w, hw1[0] + 300.0f, hw1[1], hw1[2], IA_GRAB, false);
        tick_n(&w, 2, dt); // (the laser finds it)
        hand_set(1, rp, fwd, 0, 1.0f, 0, 0), tick_n(&w, 1, dt);
        const CTransform* fct = ecs_get(&w, far_cube, COMP_TRANSFORM);
        float hw2[3];
        vr_pawn_to_world(rp, hw2);
        CHECK(vr_pawn_held(1) == far_cube && fabsf(fct->location[0] - hw2[0]) < 0.5f && V.st.distance_grabs == 1, "distance grab: the cube 3 m away pulled into the hand (%.1f)", fct->location[0] - hw2[0]);
        hand_set(1, rp, fwd, 0, 0.0f, 0, 0), tick_n(&w, 2, dt);
        ecs_destroy(&w, far_cube);
    }
    { // left-handed: the left stick turns
        project()->vr.dominant_hand = 1;
        uint32_t turns = V.st.snap_turns;
        hand_set(0, lp, fwd, 0, 0, 1.0f, 0), tick_n(&w, 2, dt);
        hand_set(0, lp, fwd, 0, 0, 0, 0), tick_n(&w, 1, dt);
        CHECK(V.st.snap_turns == turns + 1, "left-handed: the left stick snap turns (%u)", V.st.snap_turns - turns);
        project()->vr.dominant_hand = 0;
    }
    // a wall 20 cm ahead of a Character's capsule stops the drag (below, with the pawn)
    // a Character as the pawn: a teleport puts its capsule under the head, a snap turn keeps it where it stands
    Entity pawn = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER));
    CCharacter* ch = ecs_get(&w, pawn, COMP_CHARACTER);
    ch->half_height = 90.0f, ch->radius = 35.0f, ch->max_walk_speed = 600.0f;
    g_test_pawn = pawn;
    hx[0] = 40.0f, hx[1] = -25.0f;
    xr_sim_head(hx, 0.0f, 0.0f);
    float spot[3] = { 500.0f, 300.0f, 0.0f };
    tick_n(&w, 1, dt);
    vr_pawn_teleport(&w, spot, 0.0f, true);
    tick_n(&w, 1, dt);
    const CTransform* pt = ecs_get(&w, pawn, COMP_TRANSFORM);
    vr_pawn_to_world(hx, hw);
    CHECK(fabsf(pt->location[0] - 500.0f) < 0.01f && fabsf(pt->location[1] - 300.0f) < 0.01f && fabsf(pt->location[2] - 92.0f) < 0.01f &&
          fabsf(hw[0] - 500.0f) < 0.5f && fabsf(hw[1] - 300.0f) < 0.5f, "the Character's capsule and the head over the spot (%.1f %.1f %.1f; head %.1f %.1f)",
          pt->location[0], pt->location[1], pt->location[2], hw[0], hw[1]);
    float pbefore[2] = { pt->location[0], pt->location[1] };
    hand_set(1, rp, fwd, 0, 0, -1.0f, 0), tick_n(&w, 2, dt);
    hand_set(1, rp, fwd, 0, 0, 0, 0), tick_n(&w, 1, dt);
    pt = ecs_get(&w, pawn, COMP_TRANSFORM);
    ch = ecs_get(&w, pawn, COMP_CHARACTER);
    vr_pawn_origin(org, &yaw);
    CHECK(fabsf(pt->location[0] - pbefore[0]) < 0.01f && fabsf(pt->location[1] - pbefore[1]) < 0.01f && fabsf(ch->control_yaw - yaw) < 0.01f,
          "a snap turn left keeps the Character where it stands, its control yaw the play space's (%.1f)", yaw);
    // smooth walking with a Character: its movement input, where the head looks, scaled to the VR speed
    vr_pawn_set_locomotion(VR_LOCO_SMOOTH, -1);
    ch->input[0] = ch->input[1] = 0.0f;
    hand_set(0, lp, fwd, 0, 0, 0, 1.0f), vr_pawn_tick(&w, dt);
    float hf[3], hfw[3], hu[3];
    xr_head(hw, hf, hu), vr_pawn_dir_to_world(hf, hfw);
    CHECK(fabsf(ch->input[0] - hfw[0] * 200.0f / 600.0f) < 0.01f && fabsf(ch->input[1] - hfw[1] * 200.0f / 600.0f) < 0.01f,
          "the Character's movement input along the head (%.3f %.3f)", ch->input[0], ch->input[1]);
    hand_set(0, lp, fwd, 0, 0, 0, 0);
    vr_pawn_tick(&w, dt);
    { // the pawn's own body hidden in VR: its mesh and a component's (a mannequin) hidden, the hands still seen
        ecs_add(&w, pawn, COMP_STATIC_MESH);
        Entity part = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_ATTACH));
        ((CAttach*)ecs_get(&w, part, COMP_ATTACH))->owner = pawn;
        tick_n(&w, 31, dt);
        const CTransform* bt2 = ecs_get(&w, part, COMP_TRANSFORM);
        const CTransform* pt2 = ecs_get(&w, pawn, COMP_TRANSFORM);
        CHECK((bt2->flags & TRANSFORM_HIDDEN_GAME) && !ecs_get(&w, pawn, COMP_STATIC_MESH) && !(pt2->flags & TRANSFORM_HIDDEN_GAME),
              "the pawn's body hidden in VR (its component hidden, its own mesh off, the pawn itself not hidden: its overlaps go on)");
        ch = ecs_get(&w, pawn, COMP_CHARACTER);
        pt = ecs_get(&w, pawn, COMP_TRANSFORM);
    }
    // a wall 40 cm ahead of the capsule (its radius 35): dragging 30 cm toward it stops at the wall
    vr_pawn_set_locomotion(VR_LOCO_TELEPORT, -1);
    ch->input[0] = ch->input[1] = 0.0f;
    pt = ecs_get(&w, pawn, COMP_TRANSFORM);
    float hfx[3], hux[3], hpx[3];
    xr_head(hpx, hfx, hux);
    float pawn_x = pt->location[0], dir_x[3] = { 1, 0, 0 }, lw[3];
    vr_pawn_dir_to_world(dir_x, lw); // (the play space's +X in the world: the wall along the world's X)
    (void)lw;
    g_wall_x = pawn_x + 40.0f;
    float dq[3] = { rp[0], rp[1], rp[2] };
    hand_set(1, dq, fwd, 0, 1.0f, 0, 0), tick_n(&w, 1, dt);
    for (int i = 0; i < 10; i++) {
        // (the hand pulled back along the world's -X: in the play space turned by its yaw)
        float back[3] = { -3.0f, 0, 0 }, lb[3];
        rotz(-V.yaw, back, lb);
        dq[0] += lb[0], dq[1] += lb[1], hand_set(1, dq, fwd, 0, 1.0f, 0, 0), tick_n(&w, 1, dt);
    }
    hand_set(1, dq, fwd, 0, 0.0f, 0, 0), tick_n(&w, 1, dt);
    V.fling[0] = V.fling[1] = 0.0f;
    pt = ecs_get(&w, pawn, COMP_TRANSFORM);
    CHECK(pt->location[0] - pawn_x > 1.0f && pt->location[0] - pawn_x < 5.5f, "the drag stopped by a wall: %.1f of 30 cm (the capsule's edge at it)", pt->location[0] - pawn_x);
    g_wall_x = 1e9f;
    { // the safety net: the pawn on the ground half a second, then 6 m under it: back where it stood, still
        ch = ecs_get(&w, pawn, COMP_CHARACTER);
        CTransform* sp = ecs_get(&w, pawn, COMP_TRANSFORM);
        ch->on_ground = 1;
        tick_n(&w, 50, dt);
        float stood[3] = { sp->location[0], sp->location[1], sp->location[2] };
        ch->on_ground = 0, sp->location[2] -= 600.0f, ch->velocity[2] = -900.0f;
        tick_n(&w, 1, dt);
        sp = ecs_get(&w, pawn, COMP_TRANSFORM);
        CHECK(V.st.rescues == 1 && fabsf(sp->location[2] - stood[2]) < 0.5f && ch->velocity[2] == 0.0f, "the safety net: fallen 6 m, back on the ground (%u, z %.1f)", V.st.rescues, sp->location[2]);
        ch->on_ground = 1;
    }
    g_test_pawn = ENTITY_NONE;
    // 1000 frames of both ticks with the hands posed (the cost a frame)
    double c0 = re_time_seconds();
    for (int i = 0; i < 1000; i++) {
        hand_set(1, rp, fwd, (float)(i % 10) * 0.1f, (float)(i % 7) * 0.1f, 0, 0);
        tick_n(&w, 1, dt);
    }
    double per = (re_time_seconds() - c0) * 1000.0 / 1000.0;
    CHECK(per < 0.05, "the VR pawn's two ticks %.4f ms a frame (< 0.05)", per);
    VrStats st = V.st;
    vr_pawn_end_play(&w);
    g_test_trace = NULL;
    xr_shutdown();
    ecs_shutdown(&w);
    printf("[RealEngine] VR self-test: %d failures (%.0f ms; the ticks %.4f ms a frame, %u teleports, %u snap turns, %u grabs, %u throws, %u socketed, %u drags, %u pokes, %u pushes, %u distance grabs)\n", fails,
           (re_time_seconds() - t0) * 1000.0, per, st.teleports, st.snap_turns, st.grabs, st.throws, st.socketed, st.drags, st.pokes, st.pushes, st.distance_grabs);
    return fails;
#undef CHECK
}

// --- The played test (--vr-script, the packaged VR template with the simulated headset): the right hand to a Grabbable
// body, the fist, lifted and thrown forward; the player's capsule must stay put while holding it, the cube fly, fall and
// land; then the head looks down (a --shot shows whether the body is hidden). Prints a PASS / FAIL line ---
static struct { float t; Entity cube; float local[3], p0[2], r0[3], hold_pawn[2]; int phase, fails; bool said; } S;
static void script_local(const float wp[3], float out[3]) { // a world point into the play space
    float d[3] = { wp[0] - V.origin[0], wp[1] - V.origin[1], 0.0f }, l[3];
    rotz(-V.yaw, d, l);
    out[0] = l[0], out[1] = l[1], out[2] = wp[2] - V.origin[2] - V.seat;
}
bool vr_pawn_script(World* w, float dt) {
    if (!V.playing) return false;
    S.t += dt;
    const float fwd[3] = { 1, 0, 0 };
    float head[3] = { 0, 0, 170 };
    if (!S.cube) { // the first Grabbable with physics
        for (uint32_t ai = 0; ai < w->archetype_count && !S.cube; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            const uint64_t need = COMP_BIT(COMP_INTERACTABLE) | COMP_BIT(COMP_RIGID_BODY) | COMP_BIT(COMP_TRANSFORM);
            if ((a->mask & need) != need) continue;
            const CInteractable* ia = ecs_arch_column(a, COMP_INTERACTABLE);
            for (uint32_t i = 0; i < a->count; i++)
                if (ia[i].flags & IA_GRAB) { S.cube = ecs_arch_entities(a)[i]; break; }
        }
        if (!S.cube) { printf("[RealEngine] VR script: FAIL: no Grabbable with physics in the level\n"); return true; }
    }
    CTransform* ct = ecs_get(w, S.cube, COMP_TRANSFORM);
    if (S.t - dt <= 0.0f || (S.t >= 0.9f && S.t - dt < 0.9f) || (S.t >= 2.5f && S.t - dt < 2.5f)) {
        const CRigidBody* rb0 = ecs_get(w, S.cube, COMP_RIGID_BODY);
        printf("[RealEngine] VR script: t %.2f the cube at (%.0f, %.0f, %.0f), simulate %u, %u static colliders, %u bodies\n", S.t, ct->location[0], ct->location[1],
               ct->location[2], rb0 ? rb0->simulate : 0u, physics_stats()->statics, physics_stats()->bodies);
    }
    float pf[3] = { 0, 0, 0 };
    pawn_feet(w, pf);
    xr_sim_head(head, 0.0f, S.t > 3.6f ? -75.0f : 0.0f);
    XrHand h;
    memset(&h, 0, sizeof h);
    h.active = true, h.up[2] = 1.0f;
    memcpy(h.fwd, fwd, 12), memcpy(h.aim_fwd, fwd, 12);
    if (S.t < 1.0f) { // the hand at the cube (where it was at the start: not following it)
        if (S.t - dt <= 0.0f) script_local(ct->location, S.local);
        S.p0[0] = pf[0], S.p0[1] = pf[1];
    } else if (S.t < 1.3f) {
        h.grip = 1.0f;
    } else if (S.t < 1.8f) { // lifted and swung forward while held
        h.grip = 1.0f;
        S.local[0] += 250.0f * dt, S.local[2] += 80.0f * dt;
        if (S.phase == 0) S.hold_pawn[0] = pf[0], S.hold_pawn[1] = pf[1], S.phase = 1;
    } else if (S.phase == 1) { // let go moving: thrown
        if (vr_pawn_held(1) != S.cube && vr_pawn_held(0) != S.cube) {}
        float mv = sqrtf((pf[0] - S.p0[0]) * (pf[0] - S.p0[0]) + (pf[1] - S.p0[1]) * (pf[1] - S.p0[1]));
        if (mv > 3.0f) printf("[RealEngine] VR script: FAIL: the player's capsule moved %.1f cm while holding the cube\n", mv), S.fails++;
        memcpy(S.r0, ct->location, 12);
        S.phase = 2;
    }
    if (S.t >= 1.05f && S.t < 1.1f && vr_pawn_held(1) != S.cube && !S.said) printf("[RealEngine] VR script: FAIL: the fist didn't take the cube\n"), S.fails++, S.said = true;
    memcpy(h.pos, S.local, 12), memcpy(h.aim_pos, S.local, 12);
    xr_sim_hand(1, &h);
    XrHand off;
    memset(&off, 0, sizeof off);
    xr_sim_hand(0, &off);
    if (S.phase == 2 && S.t >= 3.5f) {
        float dx = ct->location[0] - S.r0[0], dy = ct->location[1] - S.r0[1], dz = ct->location[2] - S.r0[2];
        float hor = sqrtf(dx * dx + dy * dy);
        const CRigidBody* rb = ecs_get(w, S.cube, COMP_RIGID_BODY);
        if (hor < 30.0f) printf("[RealEngine] VR script: FAIL: the thrown cube went %.1f cm (no physics?)\n", hor), S.fails++;
        if (dz > -10.0f) printf("[RealEngine] VR script: FAIL: the thrown cube didn't fall (%.1f cm)\n", dz), S.fails++;
        if (!rb || !rb->simulate) printf("[RealEngine] VR script: FAIL: its physics off after the throw\n"), S.fails++;
        printf("[RealEngine] VR script: thrown %.0f cm, fell %.0f cm, the capsule moved %.1f cm while holding; %s\n", hor, -dz,
               sqrtf((S.hold_pawn[0] - S.p0[0]) * (S.hold_pawn[0] - S.p0[0]) + (S.hold_pawn[1] - S.p0[1]) * (S.hold_pawn[1] - S.p0[1])),
               S.fails ? "FAIL" : "PASS");
        S.phase = 3;
    }
    // the drag: the right hand closed in the air before the chest, pulled 40 cm back over half a second: the player 40 cm on
    if (S.phase == 3 && S.t >= 4.0f) {
        memcpy(S.r0, pf, 12);
        S.local[0] = 40.0f, S.local[1] = 15.0f, S.local[2] = 120.0f;
        S.phase = 4;
    }
    if (S.phase == 4) {
        h.grip = 1.0f;
        if (S.t >= 4.2f) S.local[0] -= 80.0f * dt; // (80 cm/s for half a second)
        memcpy(h.pos, S.local, 12), memcpy(h.aim_pos, S.local, 12);
        xr_sim_hand(1, &h);
        if (S.t >= 4.7f) S.phase = 5;
    }
    if (S.phase == 5) {
        h.grip = 0.0f;
        memcpy(h.pos, S.local, 12), memcpy(h.aim_pos, S.local, 12);
        xr_sim_hand(1, &h);
        if (S.t >= 5.2f) {
            float mv = sqrtf((pf[0] - S.r0[0]) * (pf[0] - S.r0[0]) + (pf[1] - S.r0[1]) * (pf[1] - S.r0[1]));
            printf("[RealEngine] VR script: dragged %.1f cm (40 pulled, the glide after)%s\n", mv, mv < 30.0f ? ": FAIL" : "");
            printf("[RealEngine] VR script: the body: %u meshes of the pawn hidden\n", V.nbody);
            fflush(stdout);
            return true;
        }
    }
    return false;
}
