// The VR base hand in the editor (vr_hand.h): Project Settings > VR > Use the Base Hand (exported, imported into the
// project, set as the hands) and the test vr_base_hand (--vr-hand-test): the FBX read back by the engine's own importer,
// its skeleton, skin and poses where the base hand has them, the solver on it (tracked joints, the controller's curls)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "content.h"
#include "editor.h"
#include "fbx_read.h"
#include "platform.h"
#include "platform_files.h"
#include "project.h"
#include "vr_hand.h"

typedef struct FindSk { const char* want; char found[160]; } FindSk;
static void find_sk(void* user, const char* name) {
    FindSk* f = user;
    size_t n = strlen(name);
    if (n > 5 && !strcmp(name + n - 5, ".rskm") && strstr(name, f->want) && !f->found[0]) snprintf(f->found, sizeof f->found, "%s", name);
}

// The base hand exported to <project>/Saved/BaseHand, imported into /Content/VR/Hands; vpaths: the two Skeletal Meshes
bool vr_hand_use_base(Editor* ed, char vpaths[2][128], char* err, size_t cap) {
    char root[600], dir[700], src[900];
    project_root(root, sizeof root);
    snprintf(dir, sizeof dir, "%s/Saved/BaseHand", root);
    if (!vrh_export_folder(dir, err, cap)) return false;
    for (int s = 0; s < 2; s++) {
        snprintf(src, sizeof src, "%s/%s", dir, vrh_file_name(s));
        if (!editor_import_tlend(ed, src, "/Content/VR/Hands", false)) return snprintf(err, cap, "the import of %s failed", src), false;
    }
    char disk[700];
    snprintf(disk, sizeof disk, "%s/VR/Hands", content_root());
    for (int s = 0; s < 2; s++) {
        FindSk f = { s ? "BaseHand_R" : "BaseHand_L", { 0 } };
        re_list_dir(disk, find_sk, &f);
        if (!f.found[0]) return snprintf(err, cap, "no Skeletal Mesh BaseHand%s in %s", s ? "_R" : "_L", disk), false;
        snprintf(vpaths[s], 128, "/Content/VR/Hands/%s", f.found);
    }
    return true;
}

// --- the test ---
static int g_fail;
#define CHECK(c, ...) ((c) ? (void)0 : (printf("[RealEngine] FAIL: vr base hand: " __VA_ARGS__), printf("\n"), (void)g_fail++))

// the bones' places (mesh space) at a local pose
static void fk(const SkelMesh* m, const AnimXf* pose, const float rot[3][3], const float pos[3], float scale, float out[][3]) {
    float id[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    static float rows[ANIM_MAX_BONES][3][4];
    anim_world(m, pose, (const float(*)[4])id, rows);
    for (uint32_t b = 0; b < m->nbones; b++) {
        float p[3] = { rows[b][0][3], rows[b][1][3], rows[b][2][3] };
        for (int a = 0; a < 3; a++) out[b][a] = rot ? pos[a] + (rot[0][a] * p[0] + rot[1][a] * p[1] + rot[2][a] * p[2]) * scale : p[a];
    }
}
static float dist(const float a[3], const float b[3]) {
    float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

bool vr_hand_test_run(Editor* ed) {
    g_fail = 0;
    char vp[2][128], err[300];
    double t0 = re_time_seconds();
    if (!vr_hand_use_base(ed, vp, err, sizeof err)) {
        printf("[RealEngine] FAIL: vr base hand: %s\n", err);
        return false;
    }
    double t_export = re_time_seconds() - t0;
    // the files parse (the engine's reader) and say what a DCC needs
    char root[600], path[900];
    project_root(root, sizeof root);
    for (int s = 0; s < 2; s++) {
        snprintf(path, sizeof path, "%s/Saved/BaseHand/%s", root, vrh_file_name(s));
        FbxDoc d;
        char e2[200];
        bool ok = fbx_doc_load(&d, path, e2, sizeof e2);
        CHECK(ok, "%s does not parse: %s", path, e2);
        if (!ok) continue;
        CHECK(d.version == 7400 && !d.ascii, "%s: version %d", path, d.version);
        const FbxNode* objs = fbx_find(&d.root, "Objects");
        int stacks = 0, limbs = 0;
        for (const FbxNode* n = objs ? objs->child : NULL; n; n = n->next) {
            if (!strcmp(n->name, "AnimationStack")) stacks++;
            if (!strcmp(n->name, "Model") && !strcmp(fbx_prop_str(fbx_prop(n, 2)), "LimbNode")) limbs++;
        }
        CHECK(stacks == VRH_POSES && limbs == VRH_JOINTS, "%s: %d stacks, %d bones", path, stacks, limbs);
        fbx_doc_free(&d);
    }
    // the Skeletal Meshes: 26 bones, a skin, the poses as animations, the rig read
    for (int s = 0; s < 2; s++) {
        SkelMesh* m = anim_mesh_load(vp[s]);
        CHECK(m != NULL, "%s not loaded", vp[s]);
        if (!m) continue;
        CHECK(m->nbones == VRH_JOINTS && m->nskins >= 1, "%s: %u bones, %u skins", vp[s], m->nbones, m->nskins);
        static VrHandRig r;
        bool ok = vrh_rig_build(&r, m, s);
        CHECK(ok && r.tracked_ok, "%s: the rig (fingers %d, tracked %d)", vp[s], (int)r.ok, (int)r.tracked_ok);
        CHECK(r.open && r.fist, "%s: its Open / Fist animations not found", vp[s]);
        if (!ok) continue;
        // the bind: the base hand's joints in the engine's space (the axes and the mirror right)
        float base[VRH_JOINTS][3], got[ANIM_MAX_BONES][3];
        vrh_base_joints(s, VRH_POSE_OPEN, base);
        AnimXf pose[ANIM_MAX_BONES];
        for (uint32_t b = 0; b < m->nbones; b++) pose[b] = m->bones[b].bind;
        fk(m, pose, NULL, NULL, 1.0f, got);
        float worst = 0.0f;
        for (int j = 0; j < VRH_JOINTS; j++) worst = fmaxf(worst, dist(got[r.bone_of[j]], base[j]));
        CHECK(worst < 0.02f, "%s: the bind %.3f cm off the base hand's joints", vp[s], worst);
        // the animations: the Fist / Point / Pinch clips end where the base hand's poses put the tips
        const char* clips[3] = { "Fist", "Point", "Pinch" };
        const int poses[3] = { VRH_POSE_FIST, VRH_POSE_POINT, VRH_POSE_PINCH };
        for (int c = 0; c < 3; c++) {
            const AnimSeq* q = vrh_find_clip(m, clips[c]);
            CHECK(q != NULL, "%s: no %s animation", vp[s], clips[c]);
            if (!q) continue;
            anim_sample(q, m, vrh_clip_end(q), pose);
            fk(m, pose, NULL, NULL, 1.0f, got);
            vrh_base_joints(s, poses[c], base);
            worst = 0.0f;
            for (int j = 0; j < VRH_JOINTS; j++) worst = fmaxf(worst, dist(got[r.bone_of[j]], base[j]));
            CHECK(worst < 0.15f, "%s: %s's end %.3f cm off the base pose", vp[s], clips[c], worst);
            if (worst >= 0.15f)
                for (int j = 0; j < VRH_JOINTS; j++)
                    if (dist(got[r.bone_of[j]], base[j]) > 0.5f)
                        printf("[RealEngine]   %s: (%.2f %.2f %.2f) want (%.2f %.2f %.2f)\n", g_vrh_joint_names[j], got[r.bone_of[j]][0], got[r.bone_of[j]][1],
                               got[r.bone_of[j]][2], base[j][0], base[j][1], base[j][2]);
            printf("[RealEngine] VR base hand %s %s: the clip within %.3f cm of the pose\n", s ? "R" : "L", clips[c], worst);
        }
        { // the pinch: the thumb's tip meets the index's (pads touching: their radii apart)
            vrh_base_joints(s, VRH_POSE_PINCH, base);
            float dp = dist(base[5], base[10]);
            CHECK(dp < 2.0f && dp > 0.8f, "%s: the pinch's thumb tip %.2f cm from the index's", vp[s], dp);
            printf("[RealEngine] VR base hand %s pinch: thumb tip %.2f cm from the index tip\n", s ? "R" : "L", dp);
        }
        { // the in-between frames: Fist's middle finger joint halfway (no Euler spin between the keys: 100 degrees, not 180-80-180)
            const AnimSeq* q = vrh_find_clip(m, "Fist");
            int b = r.bone_of[VRH_INDEX_META + 2];
            if (q && b >= 0) {
                anim_sample(q, m, vrh_clip_end(q) * 0.5f, pose);
                const float* a = pose[b].q;
                const float* c = m->bones[b].bind.q;
                float d = fabsf(a[0] * c[0] + a[1] * c[1] + a[2] * c[2] + a[3] * c[3]);
                float deg = 2.0f * acosf(fminf(d, 1.0f)) * 57.29578f;
                CHECK(deg > 35.0f && deg < 65.0f, "%s: Fist's middle frame bends the index's middle joint %.1f degrees (50 wanted)", vp[s], deg);
                printf("[RealEngine] VR base hand %s Fist halfway: the index's middle joint at %.1f degrees (100 at the end)\n", s ? "R" : "L", deg);
            }
        }
        // the tracked solver: the base poses' joints moved and turned somewhere in the world -> the hand follows
        for (int c = 0; c < 3; c++) {
            vrh_base_joints(s, poses[c], base);
            const float ax[3] = { 0.3f, -0.5f, 0.8f };
            float axn = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]), an = 0.9f, cs = cosf(an), sn = sinf(an);
            float u[3] = { ax[0] / axn, ax[1] / axn, ax[2] / axn }, J[VRH_JOINTS][3];
            for (int j = 0; j < VRH_JOINTS; j++) { // (Rodrigues, then a move)
                const float* v = base[j];
                float d = u[0] * v[0] + u[1] * v[1] + u[2] * v[2], cr[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
                for (int a = 0; a < 3; a++) J[j][a] = v[a] * cs + cr[a] * sn + u[a] * d * (1.0f - cs) + (a == 0 ? 120.0f : a == 1 ? -40.0f : 150.0f);
            }
            float rot[3][3], pos[3];
            bool ok2 = vrh_pose_tracked(&r, m, (const float(*)[3])J, 1.0f, rot, pos, pose);
            CHECK(ok2, "%s: tracked solve refused", vp[s]);
            fk(m, pose, rot, pos, 1.0f, got);
            worst = 0.0f;
            for (int j = 0; j < VRH_JOINTS; j++) worst = fmaxf(worst, dist(got[r.bone_of[j]], J[j]));
            CHECK(worst < 0.5f, "%s: tracked %s: a joint %.3f cm off", vp[s], clips[c], worst); // (the frame by the knuckles: the ring's and little's cup tilt it)
            printf("[RealEngine] VR base hand %s tracked %s: every joint within %.3f cm\n", s ? "R" : "L", clips[c], worst);
        }
        // the controller: a full curl = the Fist clip; the procedural curl (a rig without the clips) near it
        const float full[5] = { 1, 1, 1, 1, 1 };
        vrh_pose_curl(&r, m, full, pose);
        fk(m, pose, NULL, NULL, 1.0f, got);
        vrh_base_joints(s, VRH_POSE_FIST, base);
        float tipc = 0.0f, tipp = 0.0f;
        for (int f = 1; f < 5; f++) tipc = fmaxf(tipc, dist(got[r.bone_of[VRH_INDEX_META + (f - 1) * 5 + 4]], base[VRH_INDEX_META + (f - 1) * 5 + 4]));
        r.open = r.fist = NULL;
        vrh_pose_curl(&r, m, full, pose);
        fk(m, pose, NULL, NULL, 1.0f, got);
        for (int f = 1; f < 5; f++) tipp = fmaxf(tipp, dist(got[r.bone_of[VRH_INDEX_META + (f - 1) * 5 + 4]], base[VRH_INDEX_META + (f - 1) * 5 + 4]));
        CHECK(tipc < 0.15f && tipp < 1.5f, "%s: a fist by the clips %.2f cm, by the bones' axes %.2f cm off", vp[s], tipc, tipp);
        printf("[RealEngine] VR base hand %s controller fist: by the clips %.3f cm, by the bones' axes %.3f cm\n", s ? "R" : "L", tipc, tipp);
        // its cost: a tracked solve and a curl, per hand per frame
        double c0 = re_time_seconds();
        float rot[3][3], pos[3];
        for (int k = 0; k < 1000; k++) vrh_pose_tracked(&r, m, (const float(*)[3])base, 1.0f, rot, pos, pose), vrh_pose_curl(&r, m, full, pose);
        printf("[RealEngine] VR base hand %s: tracked solve + curl %.4f ms per hand\n", s ? "R" : "L", (re_time_seconds() - c0));
    }
    // the project's VR hands now (the packaged game's test vr_hand_played moves them: Use the Base Hand's own setting)
    if (!g_fail) {
        for (int s = 0; s < 2; s++) snprintf(project()->vr.hand_mesh[s], sizeof project()->vr.hand_mesh[s], "%s", vp[s]);
        CHECK(project_save(NULL), "the project not saved");
    }
    printf("[RealEngine] VR base hand: exported and imported in %.0f ms, %s\n", t_export * 1000.0, g_fail ? "FAIL" : "OK");
    return g_fail == 0;
}
