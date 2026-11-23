// First Person Camera (Q12, notas-dev/camera-primeira-pessoa.md): the realistic first-person view on a pawn's Camera
// component (CCamera.fp: fields at the end of the camera, no component of its own). While playing, the player's camera
// is moved each tick after its boom placed it (bp_components.c), so what hangs from it (the weapon, the arms) moves too:
//   - the head's bob in step with the feet: down at each foot's contact, to the side of the stance foot; the phase locks
//     onto the animation's Footstep notifies (anim.c) when they come, else it runs from the distance walked (Stride);
//   - the look's inertia and lag (a spring on the control rotation), the lean into turns and strafes;
//   - breathing (deeper when tired, quicker when afraid: Set First Person Exertion / Fear, 0-1 by Blueprint);
//   - the landing's dip (by the fall's height), the recoil (Add Camera Recoil: a weapon, a hit);
//   - Camera Shakes (camshake.h: Start / Stop Camera Shake, Play World Camera Shake) on the player's view;
//   - autofocus: a ray at the center of the view or the focus target (an actor), the focus easing like an eye or a lens,
//     the depth of field by the Cine Camera's (post.c, the bokeh's blades); the effort's vignette (the post's own);
//   - the pawn's own body seen (its head bone hidden: anim_hide_bone).
// Presets: Realistic, Cinematic, Arcade, Horror. The math is one pure step (fpcam_step) the tests drive directly.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "components.h"
#include "ecs.h"

// CCamera.fp
#define FP_ON 1u          // the First Person Camera works (the other bits choose its parts)
#define FP_BOB 2u         // the head's bob with the steps
#define FP_LOOK_LAG 4u    // the look's inertia (Look Lag)
#define FP_LEAN 8u        // the lean into turns and strafes
#define FP_BREATH 16u     // breathing
#define FP_LAND 32u       // the landing's dip
#define FP_AUTOFOCUS 64u  // autofocus and its depth of field
#define FP_VIGNETTE 128u  // the effort's vignette
#define FP_BODY 256u      // the pawn's own body seen (its Hidden Bone and below hidden)

enum { FPP_CUSTOM = 0, FPP_REALISTIC, FPP_CINEMATIC, FPP_ARCADE, FPP_HORROR, FPP_COUNT };
extern const char* const g_fpcam_preset_names[FPP_COUNT];
void fpcam_apply_preset(CCamera* c, uint32_t preset); // its values and parts (FP_ON on; FP_BODY kept)

// --- the pure step (the tests drive it) ---
typedef struct FpSpring { float x, v; } FpSpring;
typedef struct FpState {
    Entity cam, owner;
    float time;                // the state's own clock (s)
    float phase;               // the step's phase 0..1 (0: a foot's contact)
    uint32_t steps;            // steps taken (odd / even: which foot)
    float step_rate;           // steps a second (from the notifies, else speed / stride)
    float phase_fix;           // a notify's correction still to apply
    float last_note, note_dt;  // when the last Footstep notify came (state time), the interval between the last two
    uint32_t notes;            // notifies seen
    float amp;                 // the bob's amount now (0..1.5: the speed's share, eased)
    FpSpring look[2];          // the lagged pitch, yaw (degrees, relative to where the control rotation was)
    float ctl_prev[2];         // the control pitch, yaw last tick
    bool have_prev;
    float turn_rate;           // the yaw's rate (deg/s), eased
    float roll;                // the lean, eased
    float breath;              // the breathing's phase (cycles)
    float exertion, fear;      // now (eased toward the targets)
    float exertion_target, fear_target;
    FpSpring land;             // the landing's dip (cm)
    bool was_ground;
    float vz_prev;
    FpSpring recoil[3];        // pitch, yaw (degrees), back (cm)
    float focus_diopter;       // the focus now (1 / m)
    Entity focus_target;       // an actor to keep in focus (0: the center ray)
    float body_retry;          // (the body's bone: asked again until the mesh is there)
    bool body_done;
    // the last step's output (the tests and the Debug Viewer read it)
    float out_loc[3], out_rot[3]; // cm forward, right, up; degrees pitch, yaw, roll
    float out_focus_cm, out_vignette;
} FpState;

typedef struct FpInput {
    float dt;
    float speed;               // cm/s on the ground plane
    float vel_right;           // cm/s along the view's right (the strafe)
    float max_speed;           // the pawn's walk speed (cm/s; the strafe's full lean)
    bool on_ground;
    float vz;                  // cm/s (up +)
    float ctl_pitch, ctl_yaw;  // the control rotation (degrees)
    bool footstep;             // a Footstep notify since the last step
    float footstep_age;        // how long ago it came (s)
    float focus_cm;            // what the autofocus sees (cm; <= 0: nothing: the far limit)
    float gravity;             // cm/s2 (980)
} FpInput;

void fpcam_state_reset(FpState* s);
void fpcam_step(FpState* s, const CCamera* c, const FpInput* in);

// --- the world (bp_vm.c, bp_components.c) ---
void fpcam_begin_play(void);   // the states, the shakes and the targets forgotten (Play starts, stops)
void fpcam_tick(World* w, float dt); // once a tick before the components follow: the shakes' time
// A pawn's camera placed by its parents this tick (bp_components_update, playing): the player's moved by its First Person
// Camera and the shakes (o: its place, ax: its axes, scaled as given)
void fpcam_component(World* w, Entity cam_e, Entity owner, const CCamera* cam, float o[3], float ax[3][3], float dt);
// The shakes on a view that has no camera component (bp_player_camera's other views): its place and turn moved
void fpcam_shake_view(float pos[3], float* yaw, float* pitch, float* roll, float* fov_add);
float fpcam_shake_fov(void); // the shakes' FOV now (degrees, added)
// A Footstep notify on the actor's mesh (anim.c fire_notify): the bob locks onto it
void fpcam_footstep(World* w, Entity actor);
// The post process of the player's camera (post.c post_fill_view, at its end): autofocus' distance and depth of field,
// the effort's vignette (only what the camera's First Person Camera asks for)
struct PostSettings;
struct RenderView;
void fpcam_post(const CCamera* cam, struct PostSettings* store, struct RenderView* rv);

// The Blueprint nodes' (Target: a pawn, or the player when none)
void fpcam_set_exertion(World* w, Entity pawn, float v);  // 0 rested .. 1 out of breath
void fpcam_set_fear(World* w, Entity pawn, float v);      // 0 calm .. 1 terrified
float fpcam_get_exertion(World* w, Entity pawn);
float fpcam_get_fear(World* w, Entity pawn);
void fpcam_add_recoil(World* w, Entity pawn, float pitch_deg, float yaw_deg, float kick_cm);
void fpcam_set_focus_target(World* w, Entity pawn, Entity target); // 0: back to the center ray
float fpcam_get_focus_distance(World* w, Entity pawn);   // cm (0: no autofocus)
bool fpcam_set_preset(World* w, Entity pawn, const char* name);  // "Realistic", "Cinematic", "Arcade", "Horror"
bool fpcam_set_enabled(World* w, Entity pawn, uint32_t bits, bool on); // FP_* parts on or off
// Camera Shakes: by an asset's name or path, or a built-in preset ("Explosion"); a handle (0: none)
uint32_t fpcam_start_shake(const char* shake, float scale, uint32_t seed);
void fpcam_stop_shake(uint32_t handle, bool immediately);
void fpcam_stop_all_shakes(bool immediately);
// Unreal's Play World Camera Shake: full within inner_radius of the epicenter, fading to nothing at outer_radius
// (falloff: the curve's exponent) from the player's view
uint32_t fpcam_play_world_shake(World* w, const char* shake, const float epicenter[3], float inner_radius, float outer_radius, float falloff);
uint32_t fpcam_shakes_active(void);

typedef struct FpStats {
    double ms;                 // the last tick's cost (the components' and the shakes')
    double ms_avg;             // eased
    uint32_t cameras, shakes;
} FpStats;
const FpStats* fpcam_stats(void);
const FpState* fpcam_state_of(Entity owner); // (the tests, the Debug Viewer)
void fpcam_register_debug(void);             // the Debug Viewer's list (debug_viewer.c)
