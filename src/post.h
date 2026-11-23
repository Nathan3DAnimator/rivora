// Post processing (Unreal's Post Process Volume): the settings a volume overrides, blended at the camera by priority,
// weight and blend radius (post.c), turned into the frame's post parameters (render.c, shaders/composite.frag, bloom.comp).
// Blueprints change a volume's settings by name (bp_vm.c: Set Post Process Setting).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

// Each setting: its name (Details, Blueprints), group, default, range; the volume's override bit is its index
typedef enum PostField {
    PP_BLOOM_INTENSITY = 0, PP_BLOOM_THRESHOLD,
    PP_EXPOSURE_COMP, PP_EXPOSURE_MIN_EV, PP_EXPOSURE_MAX_EV,
    PP_SATURATION, PP_CONTRAST, PP_GAMMA, PP_GAIN, PP_OFFSET, PP_TEMPERATURE, PP_TINT,
    PP_VIGNETTE, PP_CHROMATIC, PP_GRAIN, PP_SHARPEN,
    PP_MB_AMOUNT, PP_MB_MAX, PP_MB_TARGET_FPS,
    PP_DOF_FOCAL_DISTANCE, PP_DOF_FSTOP, PP_DOF_MAX_BLUR,
    PP_GI_INTENSITY, PP_AO_INTENSITY,
    PP_FIELD_COUNT
} PostField;

typedef struct PostFieldInfo {
    const char* name;  // Unreal's ("Bloom Intensity", "Exposure Compensation", ...)
    const char* group; // "Bloom", "Exposure", "Color Grading", "Lens", "Motion Blur", "Depth of Field", "Global Illumination"
    float def, lo, hi;
} PostFieldInfo;
extern const PostFieldInfo g_post_fields[PP_FIELD_COUNT];
// The settings a camera overrides (CCamera.post, in this order): the lens, exposure, bloom, motion blur and grading
#define CAMERA_POST_FIELDS 14
extern const uint8_t g_camera_post_fields[CAMERA_POST_FIELDS];

_Static_assert(PP_FIELD_COUNT == PP_FIELDS, "components.h PostSettings"); // (PostSettings, CPostProcess: components.h)

void post_defaults(PostSettings* s);
void post_volume_defaults(CPostProcess* v);
// The settings at a point: the defaults, then each enabled volume by priority (lower first), weighted by its blend.
// Returns the settings some volume there overrides (a bit per PostField)
uint32_t post_blend_at(World* w, const float pos[3], PostSettings* out);
struct RenderView;
// A view's post processing (render.h RenderView.post, post_overrides, camera_blur): the blend at pos into *store; the
// camera's motion blur while playing, from the player's camera (CCamera.no_camera_blur)
void post_fill_view(World* w, const float pos[3], Entity player, bool playing, PostSettings* store, struct RenderView* rv);
// A setting by its name (Blueprints, Details); -1: none
int post_field_index(const char* name);
// (Q13) the volume whose Horror group applies at the camera (the last post_blend_at; NULL: none) and its weight there
const CPostProcess* post_terror_volume(float* weight);
