// What each bone of a Skeletal Mesh is (rig_detect.h): the Tlender's analyzer over the bind pose
#include "rig_detect.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "platform.h"
#include "render.h"
#include "render_gpu.h"
#include "rig_systems.h"
#include "rig_armature_math.h"

const RigInfo* rig_info(const SkelMesh* m) { return m ? (const RigInfo*)m->rig : NULL; }
void rig_forget(SkelMesh* m) {
    if (m) free(m->rig), m->rig = NULL;
}

static void lower(const char* s, char* out, size_t cap) {
    size_t i = 0;
    for (; s[i] && i + 1 < cap; i++) out[i] = (char)tolower((unsigned char)s[i]);
    out[i] = 0;
}
static bool has(const char* lo, const char* w) { return strstr(lo, w) != NULL; }

// --- The armature the analyzer reads, from the bind pose ---------------------------------------------------------------
// Heads at the bones' places (meters), tails at their main child (the one with the most bones below it: the spine's
// next, not a thigh), an end bone carried on along its parent's line. turn: the mesh's space turned about Z (radians)
// so that the character faces -Y with its left at +X (Blender's: the analyzer's sides by X)
static void build_armature(const SkelMesh* m, float turn, const unsigned char* skin, Armature* a, Bone* bones) {
    memset(bones, 0, sizeof(Bone) * m->nbones);
    a->bones = bones, a->bone_count = (int)m->nbones, a->autorig_rig = false, a->charmap = NULL;
    int below[ANIM_MAX_BONES];
    for (uint32_t b = 0; b < m->nbones; b++) below[b] = 0;
    for (int b = (int)m->nbones - 1; b >= 0; b--)
        if (m->bones[b].parent >= 0) below[m->bones[b].parent] += below[b] + 1;
    float c = cosf(turn), s = sinf(turn);
    for (uint32_t b = 0; b < m->nbones; b++) {
        Bone* o = &bones[b];
        snprintf(o->name, sizeof o->name, "%s", m->bones[b].name);
        o->parent = m->bones[b].parent;
        float x = m->bind_world[b][0][3], y = m->bind_world[b][1][3], z = m->bind_world[b][2][3];
        o->head = v3((x * c - y * s) * 0.01f, (x * s + y * c) * 0.01f, z * 0.01f);
        o->deform = skin ? skin[b] != 0 : true;
        o->rig_role = RROLE_NONE, o->rig_module = -1;
    }
    for (uint32_t b = 0; b < m->nbones; b++) {
        int main = -1;
        for (uint32_t k = b + 1; k < m->nbones; k++)
            if (m->bones[k].parent == (int)b && (main < 0 || below[k] > below[main] ||
                                                 (below[k] == below[main] && v3_len(v3_sub(bones[k].head, bones[b].head)) > v3_len(v3_sub(bones[main].head, bones[b].head)))))
                main = (int)k;
        Bone* o = &bones[b];
        if (main >= 0 && v3_len(v3_sub(bones[main].head, o->head)) > 1e-4f) o->tail = bones[main].head;
        else { // an end: on along its parent's line, half its parent's length (else up)
            int p = o->parent;
            vec3 d = p >= 0 ? v3_sub(o->head, bones[p].head) : v3(0, 0, 0.1f);
            float l = v3_len(d);
            if (!(l > 1e-5f)) d = v3(0, 0, 0.05f), l = 0.05f;
            o->tail = v3_add(o->head, v3_scale(d, fminf(0.5f, 0.05f / l + 0.3f)));
        }
        o->length = v3_len(v3_sub(o->tail, o->head));
    }
}

// Which bones the skins use (a file that exports its controls weights only the deforming bones): from the loaded
// skins' vertices; false when none is loaded (every bone counts as deforming then)
static bool skin_bones(const SkelMesh* m, unsigned char* used) {
    memset(used, 0, ANIM_MAX_BONES);
    bool any = false;
    for (uint32_t k = 0; k < m->nskins; k++) {
        const GpuVertex* v = NULL;
        const uint32_t* idx = NULL;
        uint32_t first = 0, count = 0;
        int32_t off = 0;
        render_mesh_cpu(m->skins[k].mesh, &v, &idx, &first, &count, &off);
        if (!v || !idx || !count) continue;
        for (uint32_t i = 0; i < count; i++) {
            const GpuVertex* g = &v[(int64_t)idx[first + i] + off];
            uint32_t j, w;
            memcpy(&j, &g->uv2[0], 4), memcpy(&w, &g->uv2[1], 4);
            for (int q = 0; q < 4; q++)
                if (((w >> (q * 8)) & 255u) > 12u) used[(j >> (q * 8)) & 255u] = 1, any = true;
        }
    }
    return any;
}

static const char* const KIND_WORDS_CTRL[] = { "ctrl", "control", "ctl", "c_", "ik_", "_ik", "ik.", "_fk", "fk_", "fk.", "mch", "org-", "tweak", "_target",
                                               "properties", "root.", "torso", "master", "_ref", "pivot", "roll", NULL };
static bool word_at(const char* lo, const char* w) { // w at the name's start or after a separator
    const char* p = strstr(lo, w);
    while (p) {
        if (p == lo || p[-1] == '_' || p[-1] == '.' || p[-1] == '-' || p[-1] == ':' || p[-1] == ' ') return true;
        p = strstr(p + 1, w);
    }
    return false;
}

// a chain's kind by its bones' names (the analyzer's kind first)
static int chain_kind(const char* lo, int cck) {
    if (has(lo, "tail") && !has(lo, "ponytail")) return RIGC_TAIL;
    if (word_at(lo, "ear") || has(lo, "ears")) return has(lo, "earring") ? RIGC_ACCESSORY : RIGC_EAR;
    if (has(lo, "hair") || has(lo, "ponytail") || has(lo, "braid") || has(lo, "bang") || has(lo, "pigtail")) return RIGC_HAIR;
    if (has(lo, "chain") || has(lo, "rope") || has(lo, "cable") || has(lo, "leash")) return RIGC_CHAIN;
    if (has(lo, "cape") || has(lo, "cloak") || has(lo, "skirt") || has(lo, "coat") || has(lo, "scarf") || has(lo, "tie")) return RIGC_CAPE;
    if (has(lo, "earring") || has(lo, "pendant") || has(lo, "acc") || has(lo, "strap") || has(lo, "bag") || has(lo, "pouch") || has(lo, "charm") ||
        has(lo, "necklace") || has(lo, "antenna") || has(lo, "feather"))
        return RIGC_ACCESSORY;
    if (cck == CCK_TAIL) return RIGC_TAIL;
    if (cck == CCK_EAR) return RIGC_EAR;
    return RIGC_OTHER;
}

static float score_of(const CharAnalysis* r) { return (float)r->main_mapped * 10.0f + r->confidence * 5.0f + (float)r->mapped * 0.1f; }

bool rig_detect(SkelMesh* m) {
    if (!m) return false;
    double t0 = re_time_seconds();
    RigInfo* ri = m->rig ? (RigInfo*)m->rig : calloc(1, sizeof *ri);
    if (!ri) return false;
    m->rig = ri;
    memset(ri, 0, sizeof *ri);
    ri->mesh_hash = m->hash;
    for (int r = 0; r < CR_COUNT; r++) ri->role[r] = ri->control[r] = -1;
    for (int l = 0; l < 4; l++) ri->ik_handle[l] = ri->pole[l] = -1;
    ri->forward[0] = 1.0f; // (Unreal's mannequin: +X)
    if (!m->nbones) return false;
    // (a load, not a frame: its own memory, any thread)
    struct Work { Bone bones[ANIM_MAX_BONES]; unsigned char skin[ANIM_MAX_BONES]; CharMap map, best_map; }* wk = malloc(sizeof *wk);
    if (!wk) return false;
    Bone* bones = wk->bones;
    unsigned char* skin = wk->skin;
    bool have_skin = skin_bones(m, skin);
    ri->had_skin = have_skin;
    Armature a;
    CharMap* pmap = &wk->map;
    CharMap* pbest = &wk->best_map;
#define map (*pmap)
#define best_map (*pbest)
    CharAnalysis rep, best_rep;
    memset(&best_rep, 0, sizeof best_rep);
    float best_turn = 0.0f, best_score = -1.0f;
    // the sides go by X: the analysis in the four turns of the mesh's space, the one where the feet point forward
    // (-Y) wins; a rig the names place fits every turn the same
    for (int k = 0; k < 4; k++) {
        float turn = (float)k * 1.5707963f;
        build_armature(m, turn, have_skin ? skin : NULL, &a, bones);
        memset(&map, 0, sizeof map);
        if (!charmap_analyze_ex(&a, NULL, have_skin ? skin : NULL, &map, &rep)) continue;
        float sc = score_of(&rep);
        int fl = charmap_bone(&a, &map, CR_FOOT_L), tl = charmap_bone(&a, &map, CR_TOE_L);
        int fr = charmap_bone(&a, &map, CR_FOOT_R), tr = charmap_bone(&a, &map, CR_TOE_R);
        float fy = 0.0f;
        if (fl >= 0 && tl >= 0) fy += bones[tl].head.y - bones[fl].head.y;
        if (fr >= 0 && tr >= 0) fy += bones[tr].head.y - bones[fr].head.y;
        int hl = charmap_bone(&a, &map, CR_UPPER_ARM_L), hr = charmap_bone(&a, &map, CR_UPPER_ARM_R);
        if (fy == 0.0f && hl >= 0 && hr >= 0) fy = -(bones[hl].head.x - bones[hr].head.x) * 0.01f; // (no toes: the left arm at +X)
        if (fy < -0.005f) sc += 1.0f; // (facing -Y: the toes ahead of the feet by more than 5 mm)
        if (getenv("REALENGINE_RIG_DEBUG")) printf("[RealEngine] rig_detect: turn %d: score %.2f (main %d, conf %.2f, mapped %d), toes %.3f\n", k, sc, rep.main_mapped, rep.confidence, rep.mapped, fy);
        if (sc > best_score) best_score = sc, best_turn = turn, best_map = map, best_rep = rep;
    }
    if (best_score < 0.0f) return free(wk), false;
    build_armature(m, best_turn, have_skin ? skin : NULL, &a, bones);
    ri->rig_class = best_rep.rig_class;
    snprintf(ri->convention, sizeof ri->convention, "%s", best_rep.convention);
    ri->confidence = best_rep.confidence, ri->main_mapped = best_rep.main_mapped, ri->main_total = best_rep.main_total;
    ri->deform_bones = best_rep.deform_bones, ri->control_bones = best_rep.control_bones;
    ri->system = rigsys_detect(&a);
    for (int r = 1; r < CR_COUNT; r++) {
        ri->role[r] = (int16_t)charmap_bone(&a, &best_map, r);
        int c = best_map.roles[r].control[0] ? armature_find_bone(&a, best_map.roles[r].control) : -1;
        ri->control[r] = (int16_t)(c != ri->role[r] ? c : -1);
    }
    for (int l = 0; l < 4; l++) ri->ik_handle[l] = (int16_t)rigsys_ik_control(&a, ri->system, l), ri->pole[l] = (int16_t)rigsys_pole_control(&a, ri->system, l);
    // the forward in the mesh's space: the turned space's -Y turned back (R(-t) (0, -1) = (-sin t, -cos t))
    ri->forward[0] = -sinf(best_turn), ri->forward[1] = -cosf(best_turn), ri->forward[2] = 0.0f;
    // what each bone is for
    for (uint32_t b = 0; b < m->nbones; b++) {
        char lo[ANIM_NAME];
        lower(m->bones[b].name, lo, sizeof lo);
        int kind = RIGB_DEFORM;
        if (have_skin && !skin[b]) kind = RIGB_HELPER;
        if (has(lo, "twist") || word_at(lo, "roll")) kind = RIGB_TWIST;
        for (int w = 0; KIND_WORDS_CTRL[w]; w++)
            if (word_at(lo, KIND_WORDS_CTRL[w]) || (KIND_WORDS_CTRL[w][0] == '_' && has(lo, KIND_WORDS_CTRL[w]))) kind = (has(lo, "mch") || has(lo, "org-")) ? RIGB_MECH : RIGB_CONTROL;
        if (has(lo, "pole") || has(lo, "elbow_target") || has(lo, "knee_target")) kind = RIGB_POLE;
        if (has(lo, "def-")) kind = RIGB_DEFORM;
        ri->kind[b] = (uint8_t)kind;
    }
    for (int l = 0; l < 4; l++) {
        if (ri->ik_handle[l] >= 0) ri->kind[ri->ik_handle[l]] = RIGB_IK_HANDLE;
        if (ri->pole[l] >= 0) ri->kind[ri->pole[l]] = RIGB_POLE;
    }
    for (int r = 1; r < CR_COUNT; r++)
        if (ri->role[r] >= 0) {
            ri->kind[ri->role[r]] = charrole_is_twist(r) ? RIGB_TWIST : charrole_is_finger(r) ? RIGB_FINGER : RIGB_DEFORM;
            if (ri->control[r] >= 0) ri->kind[ri->control[r]] = RIGB_CONTROL;
        }
    // the chains: the analyzer's generic ones (tails, ears, extra limbs), then hanging chains the names give (hair,
    // chains, capes, earrings) that no role took
    bool taken[ANIM_MAX_BONES] = { 0 };
    for (int r = 1; r < CR_COUNT; r++)
        if (ri->role[r] >= 0) taken[ri->role[r]] = true;
    for (int c = 0; c < best_map.chain_count && ri->nchains < RIG_MAX_CHAINS; c++) {
        const CharChain* ch = &best_map.chains[c];
        int f = armature_find_bone(&a, ch->first), l = armature_find_bone(&a, ch->last);
        if (f < 0 || l < 0 || ch->kind == CCK_LEG || ch->kind == CCK_ARM || ch->kind == CCK_WING) continue;
        char lo[96];
        lower(ch->name, lo, sizeof lo);
        char bl[ANIM_NAME];
        lower(m->bones[f].name, bl, sizeof bl);
        int kind = chain_kind(bl, ch->kind);
        if (kind == RIGC_OTHER) kind = chain_kind(lo, ch->kind);
        RigChainInfo* o = &ri->chains[ri->nchains++];
        o->first = (int16_t)f, o->last = (int16_t)l, o->kind = (uint8_t)kind, o->side = ch->side;
        snprintf(o->name, sizeof o->name, "%s", ch->name);
        for (int b = l; b >= 0; b = m->bones[b].parent) {
            taken[b] = true, ri->kind[b] = RIGB_CHAIN;
            if (b == f) break;
        }
    }
    for (uint32_t b = 0; b < m->nbones && ri->nchains < RIG_MAX_CHAINS; b++) {
        if (taken[b] || ri->kind[b] == RIGB_CONTROL || ri->kind[b] == RIGB_MECH || ri->kind[b] == RIGB_POLE || ri->kind[b] == RIGB_IK_HANDLE) continue;
        int p = m->bones[b].parent;
        char lo[ANIM_NAME], pl[ANIM_NAME] = "";
        lower(m->bones[b].name, lo, sizeof lo);
        if (p >= 0) lower(m->bones[p].name, pl, sizeof pl);
        int kind = chain_kind(lo, CCK_GENERIC);
        if (kind == RIGC_OTHER || (p >= 0 && chain_kind(pl, CCK_GENERIC) == kind && !taken[p])) continue; // (its chain's first bone only)
        if (p >= 0 && taken[p] && ri->kind[p] == RIGB_CHAIN) { // (the tip the analyzer left off a chain: still that chain's)
            taken[b] = true, ri->kind[b] = RIGB_CHAIN;
            continue;
        }
        { // a chain the analyzer began one bone below (its first bone's parent is this one): grown up to it
            bool grown = false;
            for (uint32_t c = 0; c < ri->nchains && !grown; c++)
                if (m->bones[ri->chains[c].first].parent == (int)b) ri->chains[c].first = (int16_t)b, taken[b] = true, ri->kind[b] = RIGB_CHAIN, grown = true;
            if (grown) continue;
        }
        // down its main line to the end
        int last = (int)b;
        for (bool more = true; more;) {
            more = false;
            for (uint32_t k = (uint32_t)last + 1; k < m->nbones; k++)
                if (m->bones[k].parent == last) {
                    last = (int)k, more = true;
                    break;
                }
        }
        RigChainInfo* o = &ri->chains[ri->nchains++];
        o->first = (int16_t)b, o->last = (int16_t)last, o->kind = (uint8_t)kind;
        float x = m->bind_world[b][0][3] * cosf(best_turn) - m->bind_world[b][1][3] * sinf(best_turn);
        o->side = fabsf(x) < 1.0f ? 0 : x > 0.0f ? 1 : 2;
        snprintf(o->name, sizeof o->name, "%s", m->bones[b].name);
        for (int k = last; k >= 0; k = m->bones[k].parent) {
            taken[k] = true, ri->kind[k] = RIGB_CHAIN;
            if (k == (int)b) break;
        }
    }
    ri->ms = (re_time_seconds() - t0) * 1000.0;
#undef map
#undef best_map
    free(wk);
    return ri->main_mapped > 0 || ri->nchains > 0;
}

// --- Unreal's names to roles ----------------------------------------------------------------------------------------------
static const struct { const char* name; int role; } UE[] = {
    { "root", CR_ROOT }, { "pelvis", CR_HIPS }, { "spine_01", CR_SPINE1 }, { "spine_02", CR_SPINE2 }, { "spine_03", CR_SPINE3 },
    { "spine_04", CR_SPINE4 }, { "spine_05", CR_CHEST }, { "neck_01", CR_NECK1 }, { "neck_02", CR_NECK2 }, { "head", CR_HEAD },
    { "jaw", CR_JAW }, { "facial_jaw", CR_JAW },
};
static const struct { const char* name; int base; } UE_SIDE[] = {
    { "clavicle", CR_CLAVICLE_L }, { "upperarm", CR_UPPER_ARM_L }, { "lowerarm", CR_FOREARM_L }, { "hand", CR_HAND_L },
    { "thumb_01", CR_THUMB1_L }, { "thumb_02", CR_THUMB2_L }, { "thumb_03", CR_THUMB3_L },
    { "index_01", CR_INDEX1_L }, { "index_02", CR_INDEX2_L }, { "index_03", CR_INDEX3_L },
    { "middle_01", CR_MIDDLE1_L }, { "middle_02", CR_MIDDLE2_L }, { "middle_03", CR_MIDDLE3_L },
    { "ring_01", CR_RING1_L }, { "ring_02", CR_RING2_L }, { "ring_03", CR_RING3_L },
    { "pinky_01", CR_PINKY1_L }, { "pinky_02", CR_PINKY2_L }, { "pinky_03", CR_PINKY3_L },
    { "thigh", CR_THIGH_L }, { "calf", CR_SHIN_L }, { "foot", CR_FOOT_L }, { "ball", CR_TOE_L },
    { "upperarm_twist_01", CR_UPPER_ARM_TWIST_L }, { "lowerarm_twist_01", CR_FOREARM_TWIST_L },
    { "thigh_twist_01", CR_THIGH_TWIST_L }, { "calf_twist_01", CR_SHIN_TWIST_L },
};
int rig_role_of_ue(const char* n) {
    if (!n || !n[0]) return CR_NONE;
    char lo[ANIM_NAME];
    lower(n, lo, sizeof lo);
    for (size_t i = 0; i < sizeof UE / sizeof UE[0]; i++)
        if (!strcmp(lo, UE[i].name)) return UE[i].role;
    size_t L = strlen(lo);
    if (L < 3 || lo[L - 2] != '_' || (lo[L - 1] != 'l' && lo[L - 1] != 'r')) return CR_NONE;
    char base[ANIM_NAME];
    snprintf(base, sizeof base, "%.*s", (int)(L - 2), lo);
    for (size_t i = 0; i < sizeof UE_SIDE / sizeof UE_SIDE[0]; i++)
        if (!strcmp(base, UE_SIDE[i].name)) return UE_SIDE[i].base + (lo[L - 1] == 'r' ? CR_SIDE_COUNT : 0);
    return CR_NONE;
}
int16_t rig_role_bone(const SkelMesh* m, int role) {
    const RigInfo* ri = rig_info(m);
    return ri && role > CR_NONE && role < CR_COUNT ? ri->role[role] : -1;
}
int16_t rig_bone_ue(const SkelMesh* m, const char* name) {
    if (!m || !name || !name[0]) return -1;
    for (uint32_t b = 0; b < m->nbones; b++)
        if (!strcmp(m->bones[b].name, name)) return (int16_t)b;
    const RigInfo* ri = rig_info(m);
    int role = rig_role_of_ue(name);
    if (!ri || role == CR_NONE) return -1;
    if (ri->role[role] >= 0) return ri->role[role];
    // a spine the rig has fewer bones of: the nearest (spine_05 / spine_04 -> the chest, else the top spine bone)
    static const int spine[] = { CR_SPINE1, CR_SPINE2, CR_SPINE3, CR_SPINE4, CR_CHEST };
    for (int i = 0; i < 5; i++)
        if (spine[i] == role) {
            for (int d = 1; d < 5; d++) {
                if (i + d < 5 && role != CR_SPINE1 && ri->role[spine[i + d]] >= 0 && i >= 3) return ri->role[spine[i + d]];
                if (i - d >= 0 && ri->role[spine[i - d]] >= 0) return ri->role[spine[i - d]];
                if (i + d < 5 && ri->role[spine[i + d]] >= 0) return ri->role[spine[i + d]];
            }
        }
    if (role == CR_NECK2 && ri->role[CR_NECK1] >= 0) return ri->role[CR_NECK1];
    return -1;
}

const char* rig_class_label(int c) {
    static const char* const L[CRC_COUNT] = { "Unknown", "Humanoid", "Near Human", "Quadruped", "Non Human", "Mechanical" };
    return c >= 0 && c < CRC_COUNT ? L[c] : L[0];
}
const char* rig_system_label(int s) {
    const char* n = rigsys_name(s);
    return n && n[0] ? n : "Unknown";
}
const char* rig_kind_label(int k) {
    static const char* const L[RIGB_KIND_COUNT] = { "Deform", "Control", "IK Handle", "Pole", "Mechanism", "Twist", "Finger", "Chain", "Helper" };
    return k >= 0 && k < RIGB_KIND_COUNT ? L[k] : L[0];
}
const char* rig_chain_kind_label(int k) {
    static const char* const L[RIGC_KIND_COUNT] = { "Tail", "Ear", "Hair", "Chain", "Accessory", "Cape / Skirt", "Other" };
    return k >= 0 && k < RIGC_KIND_COUNT ? L[k] : L[RIGC_OTHER];
}

#include "ikrig.h" // (DYN_* presets)
#include "rig_detect_test.inc"
