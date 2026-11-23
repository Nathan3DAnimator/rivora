// First Person Camera (fpcam.h; notas-dev/camera-primeira-pessoa.md): the pure step (bob locked to the steps, look lag,
// lean, breathing, landing, recoil, autofocus, vignette), the states of the player's cameras, the Camera Shakes playing
// and the Blueprint's calls. No allocation: a few fixed states and shakes; the cost is measured each tick (fpcam_stats).
#include "fpcam.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "anim.h"
#include "blueprint.h"
#include "camshake.h"
#include "debug_viewer.h"
#include "platform.h"
#include "post.h"
#include "project.h"
#include "render.h"

#define FP_DEG2RAD 0.017453293f
#define FP_TAU 6.28318531f
float bp_trace_boxes(const float a[3], const float b[3], float radius); // (bp_vm.c: the level's boxes and landscapes while playing)

const char* const g_fpcam_preset_names[FPP_COUNT] = { "Custom", "Realistic", "Cinematic", "Arcade", "Horror" };

void fpcam_apply_preset(CCamera* c, uint32_t p) {
    uint32_t body = c->fp & FP_BODY;
    char bone[sizeof c->hide_bone];
    memcpy(bone, c->hide_bone, sizeof bone);
    const uint32_t all = FP_ON | FP_BOB | FP_LOOK_LAG | FP_LEAN | FP_BREATH | FP_LAND | FP_AUTOFOCUS | FP_VIGNETTE;
    // bob up, side, roll, speed, stride; look lag Hz, damping; lean turn, strafe; breath deg, rate; land cm/m, max;
    // recoil Hz; focus speed, min, max, f-stop; vignette
    switch (p) {
    case FPP_CINEMATIC: // a camera operator's: wider sway, slow heavy look, a lens's slow focus and a shallow depth
        c->fp = all, c->bob_up_cm = 1.0f, c->bob_side_cm = 1.6f, c->bob_roll_deg = 0.9f, c->bob_speed = 400.0f, c->stride_cm = 80.0f;
        c->look_lag_hz = 5.0f, c->look_damping = 0.7f, c->lean_turn_deg = 3.0f, c->lean_strafe_deg = 2.5f;
        c->breath_deg = 0.4f, c->breath_rate = 12.0f, c->land_cm_per_m = 4.0f, c->land_max_cm = 18.0f, c->recoil_hz = 2.5f;
        c->focus_speed = 2.5f, c->focus_min_cm = 20.0f, c->focus_max_cm = 8000.0f, c->focus_fstop = 1.4f, c->vignette_effort = 0.6f;
        break;
    case FPP_ARCADE: // quick and readable: a light bob, no lag, no blur
        c->fp = FP_ON | FP_BOB | FP_LEAN | FP_LAND | FP_VIGNETTE;
        c->bob_up_cm = 0.6f, c->bob_side_cm = 0.4f, c->bob_roll_deg = 0.0f, c->bob_speed = 600.0f, c->stride_cm = 90.0f;
        c->look_lag_hz = 0.0f, c->look_damping = 1.0f, c->lean_turn_deg = 0.0f, c->lean_strafe_deg = 1.0f;
        c->breath_deg = 0.0f, c->breath_rate = 14.0f, c->land_cm_per_m = 1.5f, c->land_max_cm = 6.0f, c->recoil_hz = 5.0f;
        c->focus_speed = 8.0f, c->focus_min_cm = 15.0f, c->focus_max_cm = 10000.0f, c->focus_fstop = 4.0f, c->vignette_effort = 0.2f;
        break;
    case FPP_HORROR: // a slow, heavy walk, the head's sway and breath felt, the look a beat behind, a strong vignette
        c->fp = all, c->bob_up_cm = 1.8f, c->bob_side_cm = 1.6f, c->bob_roll_deg = 1.0f, c->bob_speed = 300.0f, c->stride_cm = 65.0f;
        c->look_lag_hz = 8.0f, c->look_damping = 0.75f, c->lean_turn_deg = 2.0f, c->lean_strafe_deg = 2.0f;
        c->breath_deg = 0.45f, c->breath_rate = 16.0f, c->land_cm_per_m = 4.0f, c->land_max_cm = 16.0f, c->recoil_hz = 3.0f;
        c->focus_speed = 4.0f, c->focus_min_cm = 15.0f, c->focus_max_cm = 5000.0f, c->focus_fstop = 2.0f, c->vignette_effort = 0.8f;
        break;
    default: // FPP_REALISTIC: a person's head (measured gait: ~1.5 cm up and down, ~1.2 side to side at a walk), an eye's focus
        p = FPP_REALISTIC;
        c->fp = all, c->bob_up_cm = 1.6f, c->bob_side_cm = 1.2f, c->bob_roll_deg = 0.6f, c->bob_speed = 400.0f, c->stride_cm = 75.0f;
        c->look_lag_hz = 14.0f, c->look_damping = 0.85f, c->lean_turn_deg = 1.2f, c->lean_strafe_deg = 1.5f;
        c->breath_deg = 0.25f, c->breath_rate = 14.0f, c->land_cm_per_m = 2.5f, c->land_max_cm = 12.0f, c->recoil_hz = 3.0f;
        c->focus_speed = 6.0f, c->focus_min_cm = 15.0f, c->focus_max_cm = 5000.0f, c->focus_fstop = 2.8f, c->vignette_effort = 0.45f;
        break;
    }
    c->fp |= body;
    c->fp_preset = p;
    memcpy(c->hide_bone, bone, sizeof bone);
    if (!c->hide_bone[0]) snprintf(c->hide_bone, sizeof c->hide_bone, "head");
}

// --- the pure step ---

static void spring(FpSpring* s, float target, float w, float z, float dt) { // (semi-implicit, steps of 1/240 s at most)
    int n = (int)ceilf(dt * 240.0f);
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    float h = dt / (float)n;
    for (int i = 0; i < n; i++) {
        s->v += (w * w * (target - s->x) - 2.0f * z * w * s->v) * h;
        s->x += s->v * h;
    }
}
// A critically damped spring toward 0, exactly (the landing, the recoil: their peak is the amount asked, 1/w after)
static void spring_crit(FpSpring* s, float w, float dt) {
    float e = expf(-w * dt), k = s->v + w * s->x;
    s->v = (s->v - w * k * dt) * e;
    s->x = (s->x + k * dt) * e;
}
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static float ease(float dt, float rate) { return 1.0f - expf(-dt * rate); }

void fpcam_state_reset(FpState* s) {
    Entity cam = s->cam, owner = s->owner;
    memset(s, 0, sizeof *s);
    s->cam = cam, s->owner = owner, s->was_ground = true, s->last_note = -100.0f;
}

void fpcam_step(FpState* s, const CCamera* c, const FpInput* in) {
    float dt = in->dt;
    if (dt <= 0.0f) return;
    if (dt > 0.1f) dt = 0.1f;
    s->time += dt;
    uint32_t f = c->fp;
    float loc[3] = { 0, 0, 0 }, rot[3] = { 0, 0, 0 };
    float ke = ease(dt, 2.5f);
    s->exertion += (s->exertion_target - s->exertion) * ke;
    s->fear += (s->fear_target - s->fear) * ke;

    // the steps: the phase from the Footstep notifies when they come (the animation's own contacts), else from the
    // distance walked (Stride); the bob's amount from the speed
    float stride = c->stride_cm > 1.0f ? c->stride_cm : 75.0f;
    float rate_dist = in->speed / stride;
    bool was_locked = s->notes >= 2 && s->note_dt > 0.0f && s->time - dt - s->last_note < s->note_dt * 2.5f;
    if (in->footstep) {
        float t_ev = s->time - in->footstep_age;
        float d = t_ev - s->last_note;
        if (s->notes && d > 0.12f && d < 1.5f) s->note_dt = s->note_dt > 0.0f && was_locked ? s->note_dt + (d - s->note_dt) * 0.5f : d;
        s->last_note = t_ev, s->notes++;
    }
    bool locked = s->notes >= 2 && s->note_dt > 0.0f && s->time - s->last_note < s->note_dt * 2.5f;
    float rate = locked ? 1.0f / s->note_dt : rate_dist;
    if (locked && !was_locked) s->step_rate = rate; // (the notifies' rhythm taken at once: the walk is only starting)
    else s->step_rate += (rate - s->step_rate) * ease(dt, 8.0f);
    s->phase += s->step_rate * dt;
    if (in->footstep) { // the contact was phase 0, footstep_age ago: the error to it now, eased away below
        float err = in->footstep_age * (s->step_rate > 0.05f ? s->step_rate : 1.8f) - s->phase;
        err -= roundf(err);
        if (was_locked) s->phase_fix = err;
        else s->phase += err, s->phase_fix = 0.0f; // (not in step yet - a start: the bob still small, set at once)
    }
    float fx = s->phase_fix * ease(dt, 14.0f);
    s->phase_fix -= fx;
    s->phase += fx;
    while (s->phase >= 1.0f) s->phase -= 1.0f, s->steps++;
    while (s->phase < 0.0f) s->phase += 1.0f, s->steps--;
    if (f & FP_BOB) {
        float bs = c->bob_speed > 1.0f ? c->bob_speed : 400.0f;
        float want = in->on_ground && in->speed > 10.0f ? fminf(in->speed / bs, 1.5f) : 0.0f;
        s->amp += (want - s->amp) * ease(dt, 8.0f);
        float a = s->amp, c2 = cosf(FP_TAU * s->phase);
        float side = sinf(FP_TAU * ((float)(s->steps & 1u) + s->phase) * 0.5f); // (over two steps: the stance foot's side)
        loc[2] += -c->bob_up_cm * a * c2; // down at the contact, up at mid-stance
        loc[1] += c->bob_side_cm * a * side;
        rot[2] += c->bob_roll_deg * a * side;
        rot[0] += -0.08f * c->bob_up_cm * a * c2; // (the head nods a little at the contact)
    } else
        s->amp = 0.0f;

    // the look: a spring between the control rotation and the view (its inertia; overshoot under damping 1)
    float dp = 0.0f, dy = 0.0f;
    if (s->have_prev) dp = in->ctl_pitch - s->ctl_prev[0], dy = remainderf(in->ctl_yaw - s->ctl_prev[1], 360.0f);
    s->ctl_prev[0] = in->ctl_pitch, s->ctl_prev[1] = in->ctl_yaw, s->have_prev = true;
    s->turn_rate += (dy / dt - s->turn_rate) * ease(dt, 10.0f);
    if ((f & FP_LOOK_LAG) && c->look_lag_hz > 0.0f && fabsf(dy) < 60.0f && fabsf(dp) < 60.0f) {
        float w = FP_TAU * c->look_lag_hz, z = c->look_damping > 0.05f ? c->look_damping : 0.8f;
        s->look[0].x -= dp, s->look[1].x -= dy;
        for (int i = 0; i < 2; i++) {
            spring(&s->look[i], 0.0f, w, z, dt);
            s->look[i].x = clampf(s->look[i].x, -8.0f, 8.0f);
        }
        rot[0] += s->look[0].x, rot[1] += s->look[1].x;
    } else
        memset(s->look, 0, sizeof s->look);

    if (f & FP_LEAN) { // into a turn (180 deg/s: Lean Turn) and a strafe (at the walk speed: Lean Strafe)
        float ms = in->max_speed > 1.0f ? in->max_speed : 600.0f;
        float want = c->lean_turn_deg * clampf(s->turn_rate / 180.0f, -1.5f, 1.5f) + c->lean_strafe_deg * clampf(in->vel_right / ms, -1.0f, 1.0f);
        s->roll += (want - s->roll) * ease(dt, 6.0f);
        rot[2] += s->roll;
    } else
        s->roll = 0.0f;

    if (f & FP_BREATH) { // tired: deeper and quicker; afraid: quicker, shallow, a small tremor
        float r = (c->breath_rate > 0.0f ? c->breath_rate : 14.0f) / 60.0f * (1.0f + 1.2f * s->exertion + 1.6f * s->fear);
        s->breath += r * dt;
        s->breath -= floorf(s->breath);
        float amp = c->breath_deg * (1.0f + 3.0f * s->exertion + 0.8f * s->fear), b = sinf(FP_TAU * s->breath);
        rot[0] += amp * b, loc[2] += amp * 0.8f * b;
        rot[1] += 0.05f * s->fear * sinf(FP_TAU * 7.3f * s->time) * (c->breath_deg > 0.0f ? 1.0f : 0.0f);
    }

    // the landing: a dip by the fall's height (the knees taking it), eased back up
    float wl = FP_TAU * 3.0f;
    if ((f & FP_LAND) && in->on_ground && !s->was_ground && s->vz_prev < -150.0f) {
        float g = in->gravity > 1.0f ? in->gravity : 980.0f;
        float h = s->vz_prev * s->vz_prev / (2.0f * g); // cm fallen
        float dip = fminf(c->land_cm_per_m * h / 100.0f, c->land_max_cm > 0.0f ? c->land_max_cm : 12.0f);
        s->land.v -= dip * wl * 2.7182818f; // (critically damped: its lowest is -dip, 1/w after)
    }
    if (!in->on_ground) s->vz_prev = in->vz;
    s->was_ground = in->on_ground;
    spring_crit(&s->land, wl, dt);
    if (f & FP_LAND) loc[2] += s->land.x, rot[0] += s->land.x * 0.15f;

    // the recoil: kicks given by Add Camera Recoil, back to rest (critically damped)
    float wr = FP_TAU * (c->recoil_hz > 0.1f ? c->recoil_hz : 3.0f);
    for (int i = 0; i < 3; i++) spring_crit(&s->recoil[i], wr, dt);
    rot[0] += s->recoil[0].x, rot[1] += s->recoil[1].x, loc[0] -= s->recoil[2].x;

    // autofocus: eased in diopters (1/m: as an eye accommodates, near changes count more than far)
    if (f & FP_AUTOFOCUS) {
        float mn = fmaxf(c->focus_min_cm, 5.0f), mx = c->focus_max_cm > mn ? c->focus_max_cm : 10000.0f;
        float d = in->focus_cm > 0.0f ? clampf(in->focus_cm, mn, mx) : mx;
        float want = 100.0f / d;
        if (s->focus_diopter <= 0.0f) s->focus_diopter = want;
        s->focus_diopter += (want - s->focus_diopter) * ease(dt, c->focus_speed > 0.0f ? c->focus_speed : 6.0f);
        s->out_focus_cm = 100.0f / s->focus_diopter;
    } else
        s->out_focus_cm = 0.0f, s->focus_diopter = 0.0f;
    s->out_vignette = (f & FP_VIGNETTE) ? c->vignette_effort * fminf(s->exertion + 0.5f * s->fear, 1.0f) : 0.0f;
    memcpy(s->out_loc, loc, sizeof loc);
    memcpy(s->out_rot, rot, sizeof rot);
}

// --- the world ---

#define FP_MAX 8
#define SH_MAX 16
#define NOTE_MAX 16
typedef struct Shake {
    uint32_t handle, seed;
    CamShake s;
    float t, stop_t, scale;
} Shake;
static struct {
    FpState st[FP_MAX];
    uint32_t n;
    Shake sh[SH_MAX];
    uint32_t nsh, next_handle;
    float shake[CS_CHANNELS];  // the shakes' sum this tick
    float clock;
    Entity note_actor[NOTE_MAX];
    float note_time[NOTE_MAX];
    uint32_t nnotes;
    float view_focus_cm, view_vignette; // the player's camera's (fpcam_post)
    double tick_ms, comp_ms;
    FpStats stats;
} F;

void fpcam_begin_play(void) {
    memset(&F, 0, sizeof F);
}

static FpState* state_of(Entity owner, bool make) {
    for (uint32_t i = 0; i < F.n; i++)
        if (F.st[i].owner == owner) return &F.st[i];
    if (!make || !owner) return NULL;
    FpState* s = F.n < FP_MAX ? &F.st[F.n++] : &F.st[FP_MAX - 1]; // (the last one reused: more first-person pawns than that)
    memset(s, 0, sizeof *s);
    s->owner = owner;
    fpcam_state_reset(s);
    return s;
}
const FpState* fpcam_state_of(Entity owner) { return state_of(owner, false); }

void fpcam_footstep(World* w, Entity actor) {
    (void)w;
    if (!actor || F.nnotes >= NOTE_MAX || !state_of(actor, false)) return; // (only a first-person pawn's steps)
    F.note_actor[F.nnotes] = actor, F.note_time[F.nnotes] = F.clock, F.nnotes++;
}

void fpcam_tick(World* w, float dt) {
    (void)w;
    double t0 = re_time_seconds();
    F.stats.ms = F.tick_ms + F.comp_ms; // (the last tick's: its shakes and its cameras)
    F.stats.ms_avg += (F.stats.ms - F.stats.ms_avg) * 0.05;
    F.comp_ms = 0.0;
    F.stats.cameras = 0;
    F.clock += dt;
    memset(F.shake, 0, sizeof F.shake);
    uint32_t live = 0;
    for (uint32_t i = 0; i < F.nsh; i++) {
        Shake* k = &F.sh[i];
        k->t += dt;
        if (camshake_finished(&k->s, k->t, k->stop_t)) continue;
        float out[CS_CHANNELS], e = camshake_envelope(&k->s, k->t, k->stop_t) * k->scale * (k->s.scale > 0.0f ? k->s.scale : 1.0f);
        camshake_sample(&k->s, k->t, k->seed, out);
        for (int c = 0; c < CS_CHANNELS; c++) F.shake[c] += out[c] * e;
        F.sh[live++] = *k;
    }
    F.nsh = live;
    F.stats.shakes = live;
    F.tick_ms = (re_time_seconds() - t0) * 1000.0;
}

// What the center of the view (or the focus target) is at: the level's boxes and landscapes, the Characters' capsules
static float focus_distance(World* w, FpState* s, Entity owner, const float o[3], const float fwd[3], float far_cm) {
    if (s->focus_target && ecs_alive(w, s->focus_target)) {
        const CTransform* t = ecs_get(w, s->focus_target, COMP_TRANSFORM);
        if (t) {
            float d[3] = { t->location[0] - o[0], t->location[1] - o[1], t->location[2] - o[2] };
            return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        }
    }
    float end[3] = { o[0] + fwd[0] * far_cm, o[1] + fwd[1] * far_cm, o[2] + fwd[2] * far_cm };
    float best = bp_trace_boxes(o, end, 0.0f) * far_cm;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) { // (the others: a capsule as its axis' nearest sphere)
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need || !a->count) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CCharacter* ch = ecs_arch_column(a, COMP_CHARACTER);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            if (ents[i] == owner) continue;
            float d[3] = { tr[i].location[0] - o[0], tr[i].location[1] - o[1], tr[i].location[2] - o[2] };
            float along = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
            if (along <= 0.0f || along >= best) continue;
            float px = o[0] + fwd[0] * along, py = o[1] + fwd[1] * along, pz = o[2] + fwd[2] * along;
            float dz = clampf(pz - tr[i].location[2], -ch[i].half_height + ch[i].radius, ch[i].half_height - ch[i].radius);
            float qx = px - tr[i].location[0], qy = py - tr[i].location[1], qz = pz - (tr[i].location[2] + dz);
            float r2 = ch[i].radius * ch[i].radius, q2 = qx * qx + qy * qy + qz * qz;
            if (q2 < r2) best = fminf(best, along - sqrtf(r2 - q2));
        }
    }
    return best;
}

static void apply_offsets(float o[3], float ax[3][3], const float loc[3], const float rot[3]) {
    float len[3], u[3][3], r[3];
    for (int a = 0; a < 3; a++) {
        len[a] = sqrtf(ax[a][0] * ax[a][0] + ax[a][1] * ax[a][1] + ax[a][2] * ax[a][2]);
        for (int k = 0; k < 3; k++) u[a][k] = len[a] > 1e-8f ? ax[a][k] / len[a] : (a == k ? 1.0f : 0.0f);
    }
    for (int k = 0; k < 3; k++) o[k] += u[0][k] * loc[0] + u[1][k] * loc[1] + u[2][k] * loc[2];
    axes_rotator(u, r);
    r[0] = clampf(r[0] + rot[0], -89.5f, 89.5f), r[1] += rot[1], r[2] += rot[2];
    rotator_axes(r, u);
    for (int a = 0; a < 3; a++)
        for (int k = 0; k < 3; k++) ax[a][k] = u[a][k] * len[a];
}

void fpcam_component(World* w, Entity cam_e, Entity owner, const CCamera* cam, float o[3], float ax[3][3], float dt) {
    if (!owner || owner != bp_player(w)) return; // (the player's view only: the others' cameras aren't seen)
    bool fp = (cam->fp & FP_ON) != 0;
    if (!fp && !F.nsh) return;
    double t0 = re_time_seconds();
    float loc[3] = { 0, 0, 0 }, rot[3] = { 0, 0, 0 };
    if (fp) {
        FpState* s = state_of(owner, true);
        s->cam = cam_e;
        FpInput in;
        memset(&in, 0, sizeof in);
        in.dt = dt;
        const CCharacter* ch = ecs_get(w, owner, COMP_CHARACTER);
        float r[3], fwd[3];
        float l1 = sqrtf(ax[1][0] * ax[1][0] + ax[1][1] * ax[1][1]), l0 = sqrtf(ax[0][0] * ax[0][0] + ax[0][1] * ax[0][1] + ax[0][2] * ax[0][2]);
        r[0] = l1 > 1e-6f ? ax[1][0] / l1 : 0.0f, r[1] = l1 > 1e-6f ? ax[1][1] / l1 : 1.0f, r[2] = 0.0f;
        for (int k = 0; k < 3; k++) fwd[k] = l0 > 1e-6f ? ax[0][k] / l0 : (k == 0 ? 1.0f : 0.0f);
        if (ch) {
            in.speed = sqrtf(ch->velocity[0] * ch->velocity[0] + ch->velocity[1] * ch->velocity[1]);
            in.vel_right = ch->velocity[0] * r[0] + ch->velocity[1] * r[1];
            in.vz = ch->velocity[2], in.on_ground = ch->on_ground != 0, in.max_speed = ch->max_walk_speed;
            in.ctl_pitch = ch->control_pitch, in.ctl_yaw = ch->control_yaw;
        } else { // (a pawn without Character Movement: its turn from the camera's axes, on the ground)
            float rr[3];
            axes_rotator(ax, rr);
            in.on_ground = true, in.ctl_pitch = rr[0], in.ctl_yaw = rr[1];
        }
        in.gravity = project()->gravity_z;
        for (uint32_t i = 0; i < F.nnotes; i++) // (the latest of the owner's Footstep notifies since the last tick)
            if (F.note_actor[i] == owner) in.footstep = true, in.footstep_age = fmaxf(F.clock - F.note_time[i], 0.0f);
        if (cam->fp & FP_AUTOFOCUS) {
            float far_cm = cam->focus_max_cm > 1.0f ? cam->focus_max_cm : 10000.0f;
            in.focus_cm = focus_distance(w, s, owner, o, fwd, far_cm);
        }
        fpcam_step(s, cam, &in);
        memcpy(loc, s->out_loc, sizeof loc);
        memcpy(rot, s->out_rot, sizeof rot);
        F.view_focus_cm = s->out_focus_cm, F.view_vignette = s->out_vignette;
        if ((cam->fp & FP_BODY) && !s->body_done) { // the pawn's own body: its head hidden (asked until its mesh is made)
            s->body_retry -= dt;
            if (s->body_retry <= 0.0f) s->body_done = anim_hide_bone(w, owner, cam->hide_bone[0] ? cam->hide_bone : "head", true), s->body_retry = 0.25f;
        }
        F.stats.cameras++;
    }
    for (int k = 0; k < 3; k++) loc[k] += F.shake[CS_LOC_X + k], rot[k] += F.shake[CS_PITCH + k];
    apply_offsets(o, ax, loc, rot);
    F.nnotes = 0; // (the notifies taken)
    F.comp_ms += (re_time_seconds() - t0) * 1000.0;
}

void fpcam_shake_view(float pos[3], float* yaw, float* pitch, float* roll, float* fov_add) {
    if (fov_add) *fov_add = F.nsh ? F.shake[CS_FOV] : 0.0f;
    if (!F.nsh) return;
    float r[3] = { *pitch, *yaw, roll ? *roll : 0.0f }, u[3][3];
    rotator_axes(r, u);
    for (int k = 0; k < 3; k++) pos[k] += u[0][k] * F.shake[CS_LOC_X] + u[1][k] * F.shake[CS_LOC_Y] + u[2][k] * F.shake[CS_LOC_Z];
    *pitch = clampf(*pitch + F.shake[CS_PITCH], -89.5f, 89.5f), *yaw += F.shake[CS_YAW];
    if (roll) *roll += F.shake[CS_ROLL];
}

float fpcam_shake_fov(void) { return F.nsh ? F.shake[CS_FOV] : 0.0f; }

void fpcam_post(const CCamera* cam, PostSettings* store, RenderView* rv) {
    if (!cam || !(cam->fp & FP_ON) || !store || !rv) return;
    if ((cam->fp & FP_AUTOFOCUS) && F.view_focus_cm > 0.0f) {
        store->v[PP_DOF_FOCAL_DISTANCE] = F.view_focus_cm;
        rv->post_overrides |= 1u << PP_DOF_FOCAL_DISTANCE;
        if (!(cam->cine & CINE_LENS)) { // (a Cine Camera's lens stays; else a lens of the view's angle on a full frame)
            float fov = cam->fov_deg > 1.0f ? cam->fov_deg : 90.0f;
            store->v[PP_DOF_FSTOP] = cam->focus_fstop > 0.0f ? cam->focus_fstop : 2.8f;
            rv->post_overrides |= 1u << PP_DOF_FSTOP;
            rv->dof_sensor_mm = 36.0f, rv->dof_focal_mm = 18.0f / tanf(fov * 0.5f * FP_DEG2RAD);
            rv->bokeh_blades = cam->blades >= 3 && cam->blades <= 16 ? cam->blades : 0;
            rv->bokeh_rot_deg = cam->blade_rot;
        }
    }
    if ((cam->fp & FP_VIGNETTE) && F.view_vignette > 0.0f) {
        store->v[PP_VIGNETTE] = fminf(store->v[PP_VIGNETTE] + F.view_vignette, 1.0f);
        rv->post_overrides |= 1u << PP_VIGNETTE;
    }
}

// --- the Blueprint's calls ---

static Entity pawn_or_player(World* w, Entity pawn) { return pawn && ecs_alive(w, pawn) ? pawn : bp_player(w); }

void fpcam_set_exertion(World* w, Entity pawn, float v) {
    FpState* s = state_of(pawn_or_player(w, pawn), true);
    if (s) s->exertion_target = clampf(v, 0.0f, 1.0f);
}
void fpcam_set_fear(World* w, Entity pawn, float v) {
    FpState* s = state_of(pawn_or_player(w, pawn), true);
    if (s) s->fear_target = clampf(v, 0.0f, 1.0f);
}
float fpcam_get_exertion(World* w, Entity pawn) {
    const FpState* s = state_of(pawn_or_player(w, pawn), false);
    return s ? s->exertion : 0.0f;
}
float fpcam_get_fear(World* w, Entity pawn) {
    const FpState* s = state_of(pawn_or_player(w, pawn), false);
    return s ? s->fear : 0.0f;
}
void fpcam_add_recoil(World* w, Entity pawn, float pitch_deg, float yaw_deg, float kick_cm) {
    FpState* s = state_of(pawn_or_player(w, pawn), true);
    if (!s) return;
    const CCamera* c = s->cam ? ecs_get(w, s->cam, COMP_CAMERA) : NULL;
    float wr = FP_TAU * (c && c->recoil_hz > 0.1f ? c->recoil_hz : 3.0f) * 2.7182818f; // (its peak: the amount, 1/w later)
    s->recoil[0].v += pitch_deg * wr, s->recoil[1].v += yaw_deg * wr, s->recoil[2].v += kick_cm * wr;
}
void fpcam_set_focus_target(World* w, Entity pawn, Entity target) {
    FpState* s = state_of(pawn_or_player(w, pawn), true);
    if (s) s->focus_target = target;
}
float fpcam_get_focus_distance(World* w, Entity pawn) {
    const FpState* s = state_of(pawn_or_player(w, pawn), false);
    return s ? s->out_focus_cm : 0.0f;
}

// The pawn's First Person cameras (its Camera components)
static uint32_t cameras_of(World* w, Entity pawn, CCamera** out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t ai = 0; ai < w->archetype_count && n < cap; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_CAMERA) | COMP_BIT(COMP_ATTACH);
        if ((a->mask & need) != need) continue;
        CCamera* c = ecs_arch_column(a, COMP_CAMERA);
        const CAttach* at = ecs_arch_column(a, COMP_ATTACH);
        for (uint32_t i = 0; i < a->count && n < cap; i++)
            if (at[i].owner == pawn) out[n++] = &c[i];
    }
    return n;
}
bool fpcam_set_preset(World* w, Entity pawn, const char* name) {
    pawn = pawn_or_player(w, pawn);
    uint32_t p = FPP_COUNT;
    for (uint32_t i = 1; name && i < FPP_COUNT; i++)
        if (!strcmp(name, g_fpcam_preset_names[i])) p = i;
    if (name && (!strcmp(name, "Terror") || !strcmp(name, "Terror (Horror)"))) p = FPP_HORROR;
    if (p == FPP_COUNT || !pawn) return false;
    CCamera* cs[4];
    uint32_t n = cameras_of(w, pawn, cs, 4);
    for (uint32_t i = 0; i < n; i++) fpcam_apply_preset(cs[i], p);
    return n > 0;
}
bool fpcam_set_enabled(World* w, Entity pawn, uint32_t bits, bool on) {
    pawn = pawn_or_player(w, pawn);
    CCamera* cs[4];
    uint32_t n = pawn ? cameras_of(w, pawn, cs, 4) : 0;
    for (uint32_t i = 0; i < n; i++) cs[i]->fp = on ? cs[i]->fp | bits : cs[i]->fp & ~bits;
    if ((bits & FP_BODY) && !on && pawn) {
        anim_hide_bone(w, pawn, NULL, false);
        FpState* s = state_of(pawn, false);
        if (s) s->body_done = false;
    }
    return n > 0;
}

static uint32_t mix_seed(uint32_t x) {
    x ^= x >> 16, x *= 0x7feb352du, x ^= x >> 15, x *= 0x846ca68bu, x ^= x >> 16;
    return x;
}
uint32_t fpcam_start_shake(const char* name, float scale, uint32_t seed) {
    const CamShake* cs = camshake_find(name);
    if (!cs || scale <= 0.0f) return 0;
    if (cs->single_instance)
        for (uint32_t i = 0; i < F.nsh; i++)
            if (!memcmp(&F.sh[i].s, cs, sizeof *cs)) { // (the one playing starts over)
                F.sh[i].t = 0.0f, F.sh[i].stop_t = -1.0f, F.sh[i].scale = scale;
                return F.sh[i].handle;
            }
    Shake* k = F.nsh < SH_MAX ? &F.sh[F.nsh++] : &F.sh[0]; // (full: the oldest goes)
    memset(k, 0, sizeof *k);
    k->handle = ++F.next_handle;
    if (!k->handle) k->handle = ++F.next_handle;
    k->s = *cs, k->t = 0.0f, k->stop_t = -1.0f, k->scale = scale;
    k->seed = seed ? seed : mix_seed(k->handle * 2654435761u);
    return k->handle;
}
void fpcam_stop_shake(uint32_t handle, bool immediately) {
    for (uint32_t i = 0; i < F.nsh; i++)
        if (F.sh[i].handle == handle) {
            if (immediately) F.sh[i] = F.sh[--F.nsh];
            else if (F.sh[i].stop_t < 0.0f) F.sh[i].stop_t = F.sh[i].t;
            return;
        }
}
void fpcam_stop_all_shakes(bool immediately) {
    if (immediately) F.nsh = 0, memset(F.shake, 0, sizeof F.shake);
    else
        for (uint32_t i = 0; i < F.nsh; i++)
            if (F.sh[i].stop_t < 0.0f) F.sh[i].stop_t = F.sh[i].t;
}
uint32_t fpcam_play_world_shake(World* w, const char* name, const float epi[3], float inner, float outer, float falloff) {
    float pos[3], yaw = 0.0f, pitch = 0.0f;
    if (!bp_player_camera(w, pos, &yaw, &pitch)) return 0;
    float d[3] = { pos[0] - epi[0], pos[1] - epi[1], pos[2] - epi[2] };
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), scale = 1.0f;
    if (dist > inner) {
        if (outer <= inner || dist >= outer) return 0;
        scale = powf(1.0f - (dist - inner) / (outer - inner), falloff > 0.0f ? falloff : 1.0f);
    }
    return fpcam_start_shake(name, scale, 0);
}
uint32_t fpcam_shakes_active(void) { return F.nsh; }
const FpStats* fpcam_stats(void) { return &F.stats; }

// --- the Debug Viewer ---
static void collect(DvList* l, const DvContext* c) {
    l->ms = F.stats.ms;
    snprintf(l->note, sizeof l->note, "CPU per tick (the player's camera and the shakes): %.4f ms now, %.4f ms on average", F.stats.ms, F.stats.ms_avg);
    char info[48];
    snprintf(info, sizeof info, "%u playing", F.nsh);
    dv_add(l, "Camera Shakes", info, (float)F.tick_ms, ENTITY_NONE);
    World* w = c ? c->world : NULL;
    for (uint32_t i = 0; w && i < F.n; i++) {
        const FpState* s = &F.st[i];
        if (!ecs_alive(w, s->owner)) continue;
        const CName* nm = ecs_get(w, s->owner, COMP_NAME);
        snprintf(info, sizeof info, "bob %.2f, focus %.0f cm, steps %u", s->amp, s->out_focus_cm, s->steps);
        dv_add(l, nm ? nm->text : "Pawn", info, (float)(F.stats.ms - F.tick_ms), s->owner);
    }
}
void fpcam_register_debug(void) { dv_register("First Person Camera", collect); }
