// (Q12, 2026-11-18) The First Person Camera's tests (notas-dev/camera-primeira-pessoa.md):
//   --fpcam-test fp_bob_sync           the bob's lowest point at each Footstep notify (locked onto them from any phase),
//                                      and from the distance walked (Stride) without them; none in the air
//   --fpcam-test camshake_determinism  a shake's samples the same for the same seed (sine, Perlin, curves), different for
//                                      another; its envelope; the shakes playing through fpcam_tick twice the same
//   --fpcam-test fp_autofocus          the focus converges (eye 6/s, lens 2.5/s) without overshooting, in diopters
//   --fpcam-test fp_land_recoil        the landing's dip by the fall's height and its return; the recoil's peak and return;
//                                      the step's cost
//   --scene fps_camera (with --new-project ... --template 2 --play): the First Person template's player in Play: the
//                                      autofocus on a wall then far, the bob against the Mannequin's own Footstep notifies,
//                                      the lean in a strafe, shakes (Start, World, Stop), a recoil; the CPU cost
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "camshake.h"
#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "fpcam.h"
#include "platform.h"
#include "render.h"

float bp_trace_boxes(const float a[3], const float b[3], float radius);

static CCamera preset_cam(uint32_t p) {
    CCamera c;
    memset(&c, 0, sizeof c);
    c.fov_deg = 90.0f;
    fpcam_apply_preset(&c, p);
    return c;
}

// --- fp_bob_sync ---
static bool test_bob_sync(void) {
    bool ok = true;
    CCamera c = preset_cam(FPP_REALISTIC);
    // (1) Footstep notifies every 0.53 s from 0.21 s on (the phase starts anywhere), the frames' times jittered
    FpState s;
    memset(&s, 0, sizeof s);
    fpcam_state_reset(&s);
    const float period = 0.53f, first = 0.21f;
    float next = first, prevz = 0.0f, prev2 = 0.0f, worst = 0.0f, sum = 0.0f;
    int mins = 0, n = 0;
    uint32_t rng = 1234567u;
    while (s.time < 6.0f) {
        rng = rng * 1664525u + 1013904223u;
        FpInput in;
        memset(&in, 0, sizeof in);
        in.dt = 1.0f / 144.0f + (float)(rng >> 24) / 255.0f * 0.004f; // (6.9 to 10.9 ms)
        in.speed = 400.0f, in.on_ground = true, in.max_speed = 600.0f, in.gravity = 980.0f;
        float now = s.time + in.dt;
        if (now >= next) in.footstep = true, in.footstep_age = now - next, next += period; // (the tick after the notify)
        fpcam_step(&s, &c, &in);
        float z = s.out_loc[2];
        n++;
        if (n > 2 && prevz < prev2 && prevz <= z && s.time > 2.0f) { // a lowest point (the frame before): its distance to the nearest step
            float t = s.time - in.dt, k = roundf((t - first) / period), d = fabsf(t - (first + k * period));
            worst = fmaxf(worst, d), sum += d, mins++;
        }
        prev2 = prevz, prevz = z;
    }
    float mean = mins ? sum / (float)mins : 1.0f;
    printf("[RealEngine] fp_bob_sync: notifies every %.2f s: %d lowest points after 2 s, %.1f ms from the contact on average, %.1f ms at worst; amount %.2f\n",
           period, mins, mean * 1000.0f, worst * 1000.0f, s.amp);
    if (mins < 6 || mean > 0.015f || worst > 0.03f) printf("[RealEngine] FAIL: fp_bob_sync: the bob isn't on the steps\n"), ok = false;
    // (2) no notifies: from the distance walked, 300 cm/s and a 75 cm stride: a step every 0.25 s
    memset(&s, 0, sizeof s);
    fpcam_state_reset(&s);
    float last_min = -1.0f, gap_worst = 0.0f;
    int gaps = 0;
    prevz = prev2 = 0.0f, n = 0;
    while (s.time < 4.0f) {
        FpInput in;
        memset(&in, 0, sizeof in);
        in.dt = 1.0f / 120.0f, in.speed = 300.0f, in.on_ground = true, in.max_speed = 600.0f;
        fpcam_step(&s, &c, &in);
        float z = s.out_loc[2];
        n++;
        if (n > 2 && prevz < prev2 && prevz <= z && s.time > 1.0f) {
            float t = s.time - in.dt;
            if (last_min > 0.0f) gap_worst = fmaxf(gap_worst, fabsf((t - last_min) - 0.25f)), gaps++;
            last_min = t;
        }
        prev2 = prevz, prevz = z;
    }
    float amp_walk = s.amp;
    printf("[RealEngine] fp_bob_sync: by the stride: %d steps, %.1f ms off 0.25 s at worst; amount %.2f (300 of 400 cm/s)\n", gaps, gap_worst * 1000.0f, amp_walk);
    if (gaps < 8 || gap_worst > 0.012f || fabsf(amp_walk - 0.75f) > 0.05f) printf("[RealEngine] FAIL: fp_bob_sync: the stride's steps\n"), ok = false;
    // (3) in the air: the bob fades out
    for (int i = 0; i < 120; i++) {
        FpInput in;
        memset(&in, 0, sizeof in);
        in.dt = 1.0f / 120.0f, in.speed = 300.0f, in.on_ground = false, in.vz = -100.0f;
        fpcam_step(&s, &c, &in);
    }
    if (s.amp > 0.01f) printf("[RealEngine] FAIL: fp_bob_sync: still bobbing in the air (%.3f)\n", s.amp), ok = false;
    if (ok) printf("[RealEngine] fp_bob_sync: ok\n");
    return ok;
}

// --- camshake_determinism ---
static bool test_shake(void) {
    bool ok = true;
    const char* names[] = { "Explosion", "Earthquake", "Handheld", "Hit", "Gunshot", "Footstep", "Heartbeat", "Dread" };
    int checked = 0;
    for (int i = 0; i < 8; i++) {
        const CamShake* cs = camshake_find(names[i]);
        if (!cs) {
            printf("[RealEngine] FAIL: camshake_determinism: no built-in %s\n", names[i]), ok = false;
            continue;
        }
        float a[CS_CHANNELS], b[CS_CHANNELS], d[CS_CHANNELS];
        double diff_other = 0.0, peak = 0.0;
        for (int k = 0; k < 480; k++) {
            float t = (float)k / 240.0f;
            camshake_sample(cs, t, 77u, a);
            camshake_sample(cs, t, 77u, b);
            camshake_sample(cs, t, 78u, d);
            if (memcmp(a, b, sizeof a)) printf("[RealEngine] FAIL: camshake_determinism: %s differs at %.3f s with the same seed\n", names[i], t), ok = false, k = 480;
            for (int c = 0; c < CS_CHANNELS; c++) diff_other += fabs(a[c] - d[c]), peak = fmax(peak, fabs(a[c]));
        }
        bool noise = false;
        for (int c = 0; c < CS_CHANNELS; c++) noise |= cs->ch[c].amplitude != 0.0f && (cs->ch[c].wave == CSW_PERLIN || cs->ch[c].random_offset);
        if (noise && diff_other < 1e-3) printf("[RealEngine] FAIL: camshake_determinism: %s is the same with another seed\n", names[i]), ok = false;
        if (peak <= 0.0) printf("[RealEngine] FAIL: camshake_determinism: %s doesn't move\n", names[i]), ok = false;
        if (cs->blend_in > 0.0f && camshake_envelope(cs, 0.0f, -1.0f) > 1e-4f) printf("[RealEngine] FAIL: camshake_determinism: %s starts at full\n", names[i]), ok = false;
        if (cs->duration > 0.0f && (!camshake_finished(cs, cs->duration + 0.01f, -1.0f) || camshake_envelope(cs, cs->duration, -1.0f) > 1e-4f))
            printf("[RealEngine] FAIL: camshake_determinism: %s doesn't end\n", names[i]), ok = false;
        if (!camshake_finished(cs, 5.0f + cs->blend_out, 5.0f)) printf("[RealEngine] FAIL: camshake_determinism: %s doesn't stop\n", names[i]), ok = false;
        checked++;
    }
    // the shakes playing (fpcam_tick, the handles' seeds) the same twice: a play started over moves the view the same way
    double run[2] = { 0, 0 };
    float trace[2][240];
    for (int r = 0; r < 2; r++) {
        fpcam_begin_play();
        uint32_t h1 = fpcam_start_shake("Explosion", 1.0f, 0), h2 = fpcam_start_shake("Handheld", 0.5f, 0);
        for (int k = 0; k < 240; k++) {
            if (k == 120) fpcam_stop_shake(h2, false);
            fpcam_tick(NULL, 1.0f / 120.0f);
            float pos[3] = { 0, 0, 0 }, yaw = 0, pitch = 0, roll = 0, fa = 0;
            fpcam_shake_view(pos, &yaw, &pitch, &roll, &fa);
            trace[r][k] = pos[0] + pos[1] * 3.0f + pos[2] * 7.0f + pitch * 11.0f + yaw * 13.0f + roll * 17.0f + fa * 19.0f;
            run[r] += fabs(trace[r][k]);
        }
        if (!h1 || !h2) printf("[RealEngine] FAIL: camshake_determinism: no handle\n"), ok = false;
        if (fpcam_shakes_active() != 0) printf("[RealEngine] FAIL: camshake_determinism: %u shakes still playing after their end\n", fpcam_shakes_active()), ok = false;
    }
    if (memcmp(trace[0], trace[1], sizeof trace[0]) || run[0] <= 0.0) printf("[RealEngine] FAIL: camshake_determinism: two plays differ\n"), ok = false;
    // the cost: the 8 built-ins sampled
    const CamShake* all[8];
    for (int i = 0; i < 8; i++) all[i] = camshake_find(names[i]);
    double t0 = re_time_seconds();
    float acc = 0.0f;
    for (int k = 0; k < 20000; k++) {
        float o[CS_CHANNELS];
        if (!all[k & 7]) continue;
        camshake_sample(all[k & 7], (float)k * 0.001f, 5u, o);
        acc += o[0];
    }
    double us = (re_time_seconds() - t0) * 1e6 / 20000.0;
    fpcam_begin_play();
    printf("[RealEngine] camshake_determinism: %d built-in shakes the same with their seed, other seeds differ; two plays the same (%.1f); %.3f us a sample (%g)\n",
           checked, run[0], us, (double)acc * 0.0);
    if (ok) printf("[RealEngine] camshake_determinism: ok\n");
    return ok;
}

// --- fp_autofocus ---
static float focus_time(uint32_t preset, float from_cm, float to_cm, bool* monotonic) {
    CCamera c = preset_cam(preset);
    FpState s;
    memset(&s, 0, sizeof s);
    fpcam_state_reset(&s);
    FpInput in;
    memset(&in, 0, sizeof in);
    in.dt = 1.0f / 120.0f, in.on_ground = true, in.focus_cm = from_cm;
    for (int i = 0; i < 240; i++) fpcam_step(&s, &c, &in);
    in.focus_cm = to_cm;
    float t = -1.0f, prev = s.out_focus_cm;
    *monotonic = true;
    for (int i = 0; i < 600; i++) {
        fpcam_step(&s, &c, &in);
        if ((to_cm > from_cm && s.out_focus_cm < prev - 0.01f) || (to_cm < from_cm && s.out_focus_cm > prev + 0.01f) ||
            (to_cm > from_cm ? s.out_focus_cm > to_cm * 1.001f : s.out_focus_cm < to_cm * 0.999f))
            *monotonic = false;
        prev = s.out_focus_cm;
        if (t < 0.0f && fabsf(s.out_focus_cm - to_cm) < to_cm * 0.05f) t = (float)(i + 1) / 120.0f;
    }
    return t;
}
static bool test_autofocus(void) {
    bool ok = true, m1, m2, m3, m4;
    float eye_far = focus_time(FPP_REALISTIC, 300.0f, 1200.0f, &m1), eye_near = focus_time(FPP_REALISTIC, 1200.0f, 40.0f, &m2);
    float lens_far = focus_time(FPP_CINEMATIC, 300.0f, 1200.0f, &m3), lens_near = focus_time(FPP_CINEMATIC, 1200.0f, 40.0f, &m4);
    printf("[RealEngine] fp_autofocus: 3 m -> 12 m within 5%%: eye %.2f s, lens %.2f s; 12 m -> 40 cm: eye %.2f s, lens %.2f s; overshoot: %s\n",
           eye_far, lens_far, eye_near, lens_near, m1 && m2 && m3 && m4 ? "none" : "YES");
    if (eye_far < 0.0f || eye_far > 1.0f || eye_near < 0.0f || eye_near > 1.0f) printf("[RealEngine] FAIL: fp_autofocus: the eye's focus doesn't converge in 1 s\n"), ok = false;
    if (lens_far < 0.0f || lens_far > 2.5f || lens_far <= eye_far) printf("[RealEngine] FAIL: fp_autofocus: the lens's focus (slower than the eye's) doesn't converge\n"), ok = false;
    if (!(m1 && m2 && m3 && m4)) printf("[RealEngine] FAIL: fp_autofocus: the focus overshoots\n"), ok = false;
    // the range: nothing in view -> the far limit; nearer than the near limit -> the near limit
    CCamera c = preset_cam(FPP_REALISTIC);
    FpState s;
    memset(&s, 0, sizeof s);
    fpcam_state_reset(&s);
    FpInput in;
    memset(&in, 0, sizeof in);
    in.dt = 1.0f / 60.0f, in.on_ground = true, in.focus_cm = 0.0f;
    for (int i = 0; i < 300; i++) fpcam_step(&s, &c, &in);
    float far_cm = s.out_focus_cm;
    in.focus_cm = 2.0f;
    for (int i = 0; i < 300; i++) fpcam_step(&s, &c, &in);
    printf("[RealEngine] fp_autofocus: nothing seen: %.0f cm (max %.0f); 2 cm away: %.1f cm (min %.0f)\n", far_cm, c.focus_max_cm, s.out_focus_cm, c.focus_min_cm);
    if (fabsf(far_cm - c.focus_max_cm) > c.focus_max_cm * 0.01f || fabsf(s.out_focus_cm - c.focus_min_cm) > 0.5f)
        printf("[RealEngine] FAIL: fp_autofocus: the focus range\n"), ok = false;
    if (ok) printf("[RealEngine] fp_autofocus: ok\n");
    return ok;
}

// --- fp_land_recoil ---
static bool test_land_recoil(void) {
    bool ok = true;
    CCamera c = preset_cam(FPP_REALISTIC);
    c.fp &= ~(FP_BREATH | FP_BOB); // (the dip and the kick alone)
    FpState s;
    memset(&s, 0, sizeof s);
    fpcam_state_reset(&s);
    FpInput in;
    memset(&in, 0, sizeof in);
    in.dt = 1.0f / 120.0f, in.gravity = 980.0f;
    float lowest[2] = { 0, 0 }, back[2] = { 0, 0 }, t_low = 0.0f;
    const float heights[2] = { 200.0f, 1000.0f }; // a 2 m fall (dip 5 cm), a 10 m one (25 cm: held at the 12 cm limit)
    for (int f = 0; f < 2; f++) {
        float vz = -sqrtf(2.0f * 980.0f * heights[f]);
        in.on_ground = false, in.vz = vz;
        fpcam_step(&s, &c, &in);
        in.on_ground = true, in.vz = 0.0f;
        for (int i = 0; i < 240; i++) {
            fpcam_step(&s, &c, &in);
            if (s.out_loc[2] < lowest[f]) lowest[f] = s.out_loc[2], t_low = (float)(i + 1) / 120.0f;
        }
        back[f] = s.out_loc[2];
    }
    printf("[RealEngine] fp_land_recoil: a 2 m fall dips %.2f cm (at %.0f ms), back to %.3f; a 10 m one %.2f cm (most %.0f)\n",
           -lowest[0], t_low * 1000.0f, back[0], -lowest[1], c.land_max_cm);
    if (fabsf(-lowest[0] - 2.5f * 2.0f) > 0.4f || fabsf(back[0]) > 0.05f || fabsf(-lowest[1] - c.land_max_cm) > 0.6f)
        printf("[RealEngine] FAIL: fp_land_recoil: the landing's dip\n"), ok = false;
    // the recoil: 2 degrees up, 1 cm back: the peak, then back to rest
    memset(&s, 0, sizeof s);
    fpcam_state_reset(&s);
    s.recoil[0].v += 2.0f * 6.28318531f * c.recoil_hz * 2.7182818f, s.recoil[2].v += 1.0f * 6.28318531f * c.recoil_hz * 2.7182818f; // (fpcam_add_recoil's kick)
    float peak = 0.0f, peak_t = 0.0f;
    for (int i = 0; i < 120; i++) {
        fpcam_step(&s, &c, &in);
        if (s.out_rot[0] > peak) peak = s.out_rot[0], peak_t = (float)(i + 1) / 120.0f;
    }
    printf("[RealEngine] fp_land_recoil: a 2 deg recoil peaks at %.2f deg after %.0f ms, %.3f deg after 1 s\n", peak, peak_t * 1000.0f, s.out_rot[0]);
    if (fabsf(peak - 2.0f) > 0.1f || fabsf(s.out_rot[0]) > 0.05f) printf("[RealEngine] FAIL: fp_land_recoil: the recoil\n"), ok = false;
    // the step's cost (every part on: Realistic)
    CCamera all = preset_cam(FPP_REALISTIC);
    memset(&s, 0, sizeof s);
    fpcam_state_reset(&s);
    double t0 = re_time_seconds();
    for (int i = 0; i < 100000; i++) {
        FpInput fi;
        memset(&fi, 0, sizeof fi);
        fi.dt = 1.0f / 200.0f, fi.speed = 400.0f, fi.on_ground = true, fi.ctl_yaw = (float)i * 0.3f, fi.focus_cm = 500.0f + (float)(i & 255);
        fi.footstep = (i % 106) == 0;
        fpcam_step(&s, &all, &fi);
    }
    double us = (re_time_seconds() - t0) * 1e6 / 100000.0;
    printf("[RealEngine] fp_land_recoil: the step costs %.3f us (every part on)\n", us);
    if (us > 20.0) printf("[RealEngine] FAIL: fp_land_recoil: the step is too slow\n"), ok = false;
    if (ok) printf("[RealEngine] fp_land_recoil: ok\n");
    return ok;
}

bool fpcam_selftest_named(const char* name) {
    if (!strcmp(name, "fp_bob_sync")) return test_bob_sync();
    if (!strcmp(name, "camshake_determinism")) return test_shake();
    if (!strcmp(name, "fp_autofocus")) return test_autofocus();
    if (!strcmp(name, "fp_land_recoil")) return test_land_recoil();
    printf("[RealEngine] FAIL: no First Person Camera test %s\n", name);
    return false;
}

// --- the scene fps_camera ---
static struct {
    bool setup, done_turn, shook, stopped, recoiled;
    double t0;
    Entity player, wall;
    float wall_cm, focus_on_wall, focus_far_target, focus_far_time;
    float max_shake, max_lean;
    uint32_t notes_seen, notes_at_walk;
    float note_t[256];
    int nnote;
    float zs[3], ts[3];
    int nz;
    float min_t[512];
    int nmin;
    double ms_sum;
    int ms_n;
    float worst_ms;
    int frames;
} Q;

static float player_time(Editor* ed) { return (float)(ed->play_time - Q.t0); }

static void step(Editor* ed, double dt) {
    World* w = &ed->world;
    if (!Q.setup || !ecs_alive(w, Q.player)) return;
    CCharacter* ch = ecs_get(w, Q.player, COMP_CHARACTER);
    if (!ch) return;
    float t = player_time(ed);
    ch->input[0] = ch->input[1] = 0.0f;
    if (t >= 0.8f && !Q.done_turn) ch->control_yaw += 180.0f, Q.done_turn = true; // (face away from the wall: far)
    float fwd = t >= 1.6f && t < 5.0f ? 1.0f : 0.0f, right = t >= 5.0f && t < 6.0f ? 1.0f : 0.0f; // run forward, then strafe right
    float cy = cosf(ch->control_yaw * 0.017453293f), sy = sinf(ch->control_yaw * 0.017453293f); // (the input is the world's)
    ch->input[0] = cy * fwd - sy * right, ch->input[1] = sy * fwd + cy * right;
    if (t >= 6.0f && !Q.shook) {
        fpcam_start_shake("Explosion", 1.0f, 0);
        const CTransform* pt = ecs_get(w, Q.player, COMP_TRANSFORM);
        float epi[3] = { pt->location[0] + 600.0f, pt->location[1], pt->location[2] };
        if (!fpcam_play_world_shake(w, "Earthquake", epi, 300.0f, 2000.0f, 1.0f)) printf("[RealEngine] FAIL: fps_camera: the world shake didn't reach the player\n");
        Q.shook = true;
    }
    if (t >= 6.5f && !Q.recoiled) fpcam_add_recoil(w, Q.player, 1.5f, 0.3f, 1.0f), Q.recoiled = true;
    if (t >= 7.0f && !Q.stopped) fpcam_stop_all_shakes(false), Q.stopped = true;
    if (t >= 7.3f) ch->control_pitch = fmaxf(ch->control_pitch - 120.0f * (float)dt, -55.0f); // (looks down at its own body: the end's picture)
}

void editor_q12_scene(Editor* ed, const char* name) {
    (void)name;
    memset(&Q, 0, sizeof Q);
    ed->test_step = step;
}

void editor_q12_tick(Editor* ed, double dt) {
    (void)dt;
    World* w = &ed->world;
    if (!ed->playing) return;
    if (!Q.setup) {
        Entity p = bp_player(w);
        const CTransform* pt = p ? ecs_get(w, p, COMP_TRANSFORM) : NULL;
        const CCharacter* ch = p ? ecs_get(w, p, COMP_CHARACTER) : NULL;
        if (!pt || !ch || !ch->on_ground) return;
        // a wall 3.5 m in front of the eyes (the autofocus' first target), spawned into the play (its box joins the traces)
        float yaw = ch->control_yaw * 0.017453293f;
        float at[3] = { pt->location[0] + cosf(yaw) * 380.0f, pt->location[1] + sinf(yaw) * 380.0f, pt->location[2] + 60.0f };
        Q.wall = editor_spawn_shape(ed, SHAPE_CUBE, at, 0.0);
        CTransform* wt = ecs_get(w, Q.wall, COMP_TRANSFORM);
        if (wt) wt->scale[0] = 0.2f, wt->scale[1] = 4.0f, wt->scale[2] = 3.0f, wt->rotation[1] = ch->control_yaw, wt->flags = (wt->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_STATIC;
        bp_box_spawned(Q.wall);
        Q.player = p, Q.t0 = ed->play_time, Q.setup = true;
        ed->test_step = step; // (again: opening the project cleared it)
        printf("[RealEngine] fps_camera: the player at (%.0f, %.0f, %.0f), a wall in front\n", pt->location[0], pt->location[1], pt->location[2]);
        return;
    }
    const FpState* s = fpcam_state_of(Q.player);
    if (!s) return;
    float t = player_time(ed);
    Q.frames++;
    const FpStats* st = fpcam_stats();
    if (t > 0.5f) Q.ms_sum += st->ms, Q.ms_n++, Q.worst_ms = fmaxf(Q.worst_ms, (float)st->ms);
    // the autofocus: on the wall (where the view's center ray meets it), then far once turned
    if (t > 0.7f && t < 0.8f) {
        float pos[3], yaw, pitch;
        if (bp_player_camera(w, pos, &yaw, &pitch)) {
            float f[3] = { cosf(pitch * 0.017453293f) * cosf(yaw * 0.017453293f), cosf(pitch * 0.017453293f) * sinf(yaw * 0.017453293f), sinf(pitch * 0.017453293f) };
            float end[3] = { pos[0] + f[0] * 5000.0f, pos[1] + f[1] * 5000.0f, pos[2] + f[2] * 5000.0f };
            Q.wall_cm = bp_trace_boxes(pos, end, 0.0f) * 5000.0f;
        }
        Q.focus_on_wall = s->out_focus_cm;
    }
    if (Q.done_turn && t < 1.6f) {
        float pos[3], yaw, pitch;
        if (bp_player_camera(w, pos, &yaw, &pitch)) {
            float f[3] = { cosf(pitch * 0.017453293f) * cosf(yaw * 0.017453293f), cosf(pitch * 0.017453293f) * sinf(yaw * 0.017453293f), sinf(pitch * 0.017453293f) };
            const CCamera* cam = NULL;
            float roll;
            bp_component_camera_ex(w, Q.player, pos, &yaw, &pitch, &roll, &cam);
            float far_cm = cam && cam->focus_max_cm > 1.0f ? cam->focus_max_cm : 5000.0f;
            float end[3] = { pos[0] + f[0] * far_cm, pos[1] + f[1] * far_cm, pos[2] + f[2] * far_cm };
            float want = bp_trace_boxes(pos, end, 0.0f) * far_cm;
            // (converged: within 0.02 diopters, a depth of field's tolerance: far away 5% of the distance is far finer than that)
            if (Q.focus_far_time <= 0.0f && s->out_focus_cm > 0.0f && fabsf(100.0f / s->out_focus_cm - 100.0f / want) < 0.02f)
                Q.focus_far_time = t - 0.8f, Q.focus_far_target = want;
        }
    }
    // the bob against the Mannequin's Footstep notifies (the state's clock), while walking
    if (s->notes != Q.notes_seen) {
        Q.notes_seen = s->notes;
        if (t > 1.6f && t < 5.0f && Q.nnote < 256) Q.note_t[Q.nnote++] = s->last_note;
    }
    if (t > 2.4f && t < 5.0f) {
        Q.zs[0] = Q.zs[1], Q.zs[1] = Q.zs[2], Q.zs[2] = s->out_loc[2];
        Q.ts[0] = Q.ts[1], Q.ts[1] = Q.ts[2], Q.ts[2] = s->time;
        if (++Q.nz >= 3 && Q.zs[1] < Q.zs[0] && Q.zs[1] <= Q.zs[2] && s->amp > 0.5f && Q.nmin < 512) Q.min_t[Q.nmin++] = Q.ts[1];
    } else
        Q.nz = 0;
    if (t > 5.3f && t < 6.0f) Q.max_lean = fmaxf(Q.max_lean, s->out_rot[2]);
    if (t > 6.0f && t < 7.0f) {
        float pos[3] = { 0, 0, 0 }, yaw = 0, pitch = 0, roll = 0, fa = 0;
        fpcam_shake_view(pos, &yaw, &pitch, &roll, &fa);
        Q.max_shake = fmaxf(Q.max_shake, fabsf(pos[0]) + fabsf(pos[1]) + fabsf(pos[2]) + fabsf(pitch) + fabsf(yaw) + fabsf(roll));
    }
}

bool editor_q12_check(Editor* ed) {
    bool ok = true;
    float t = Q.setup ? player_time(ed) : 0.0f;
    const FpState* s = Q.setup ? fpcam_state_of(Q.player) : NULL;
    if (!Q.setup || !s) {
        printf("[RealEngine] FAIL: fps_camera: no first-person player (setup %d)\n", Q.setup);
        return false;
    }
    if (t < 8.0f) printf("[RealEngine] FAIL: fps_camera: the run ended at %.1f s of its 8\n", t), ok = false;
    // the bob's lowest points against the notifies
    float worst = 0.0f, sum = 0.0f;
    int matched = 0;
    for (int i = 0; i < Q.nmin; i++) {
        float best = 1e9f;
        for (int k = 0; k < Q.nnote; k++) best = fminf(best, fabsf(Q.min_t[i] - Q.note_t[k]));
        if (best > 0.5f) continue;
        if (getenv("REALENGINE_FPCAM_DEBUG")) printf("[RealEngine] fps_camera: lowest point at %.3f s, %.1f ms from a notify\n", Q.min_t[i], best * 1000.0f);
        worst = fmaxf(worst, best), sum += best, matched++;
    }
    for (int k = 0; getenv("REALENGINE_FPCAM_DEBUG") && k < Q.nnote; k++) printf("[RealEngine] fps_camera: notify at %.3f s\n", Q.note_t[k]);
    float mean = matched ? sum / (float)matched : 1.0f;
    const CTransform* pt = ecs_get(&ed->world, Q.player, COMP_TRANSFORM);
    if (pt) printf("[RealEngine] fps_camera: the player ended at (%.0f, %.0f, %.0f), %u steps counted\n", pt->location[0], pt->location[1], pt->location[2], s->steps);
    double avg = Q.ms_n ? Q.ms_sum / Q.ms_n : 0.0;
    printf("[RealEngine] fps_camera: %d frames; autofocus %.0f cm on the wall at %.0f cm, far (%.0f cm) within 0.02 diopters in %.2f s; %d Footstep notifies "
           "walking, %d lowest points %.1f ms from them on average (%.1f at worst); lean %.2f deg in the strafe; shakes up to %.2f, %u playing at the end; "
           "body %s; CPU %.4f ms a tick on average (%.4f at worst)\n",
           Q.frames, Q.focus_on_wall, Q.wall_cm, Q.focus_far_target, Q.focus_far_time, Q.nnote, matched, mean * 1000.0f, worst * 1000.0f,
           Q.max_lean, Q.max_shake, fpcam_shakes_active(), s->body_done ? "seen (head hidden)" : "NOT hidden", avg, Q.worst_ms);
    if (Q.wall_cm <= 0.0f || fabsf(Q.focus_on_wall - Q.wall_cm) > Q.wall_cm * 0.05f) printf("[RealEngine] FAIL: fps_camera: the autofocus isn't on the wall\n"), ok = false;
    if (Q.focus_far_time <= 0.0f || Q.focus_far_time > 1.0f) printf("[RealEngine] FAIL: fps_camera: the autofocus didn't converge far\n"), ok = false;
    if (Q.nnote < 4 || matched < 4 || mean > 0.02f || worst > 0.045f) printf("[RealEngine] FAIL: fps_camera: the bob isn't on the Mannequin's steps\n"), ok = false;
    if (Q.max_lean < 0.3f) printf("[RealEngine] FAIL: fps_camera: no lean in the strafe\n"), ok = false;
    if (Q.max_shake < 0.5f || fpcam_shakes_active() != 0) printf("[RealEngine] FAIL: fps_camera: the shakes\n"), ok = false;
    if (!s->body_done) printf("[RealEngine] FAIL: fps_camera: the body's head isn't hidden\n"), ok = false;
    if (avg > 0.05) printf("[RealEngine] FAIL: fps_camera: the First Person Camera costs %.4f ms (the target: under 0.05)\n", avg), ok = false;
    return ok;
}
