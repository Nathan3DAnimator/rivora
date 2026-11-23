// Post Process Volumes (post.h): the defaults and the blend at the camera
#include "post.h"
#include "fpcam.h"
#include "terror.h"

#include <math.h>
#include <string.h>

#include "components.h"
#include "lights.h" // (lights_level_weight: a Lighting Level's blend)
#include "render.h"

const PostFieldInfo g_post_fields[PP_FIELD_COUNT] = {
    [PP_BLOOM_INTENSITY] = { "Bloom Intensity", "Bloom", 0.675f, 0.0f, 8.0f },
    [PP_BLOOM_THRESHOLD] = { "Bloom Threshold", "Bloom", 1.0f, 0.0f, 8.0f },
    [PP_EXPOSURE_COMP] = { "Exposure Compensation", "Exposure", 0.0f, -15.0f, 15.0f },
    [PP_EXPOSURE_MIN_EV] = { "Min EV100", "Exposure", -4.0f, -10.0f, 20.0f },
    [PP_EXPOSURE_MAX_EV] = { "Max EV100", "Exposure", 18.0f, -10.0f, 20.0f },
    [PP_SATURATION] = { "Saturation", "Color Grading", 1.0f, 0.0f, 2.0f },
    [PP_CONTRAST] = { "Contrast", "Color Grading", 1.0f, 0.0f, 2.0f },
    [PP_GAMMA] = { "Gamma", "Color Grading", 1.0f, 0.2f, 3.0f },
    [PP_GAIN] = { "Gain", "Color Grading", 1.0f, 0.0f, 4.0f },
    [PP_OFFSET] = { "Offset", "Color Grading", 0.0f, -0.5f, 0.5f },
    [PP_TEMPERATURE] = { "Temperature", "Color Grading", 6500.0f, 1500.0f, 15000.0f },
    [PP_TINT] = { "Tint", "Color Grading", 0.0f, -1.0f, 1.0f },
    [PP_VIGNETTE] = { "Vignette Intensity", "Lens", 0.4f, 0.0f, 1.0f },
    [PP_CHROMATIC] = { "Chromatic Aberration", "Lens", 0.0f, 0.0f, 5.0f },
    [PP_GRAIN] = { "Grain Intensity", "Lens", 0.0f, 0.0f, 1.0f },
    [PP_SHARPEN] = { "Sharpen", "Lens", 0.0f, 0.0f, 2.0f },
    [PP_MB_AMOUNT] = { "Motion Blur Amount", "Motion Blur", 0.5f, 0.0f, 1.0f },
    [PP_MB_MAX] = { "Motion Blur Max", "Motion Blur", 5.0f, 0.0f, 100.0f },
    [PP_MB_TARGET_FPS] = { "Motion Blur Target FPS", "Motion Blur", 30.0f, 0.0f, 120.0f },
    [PP_DOF_FOCAL_DISTANCE] = { "Focal Distance", "Depth of Field", 0.0f, 0.0f, 1e6f },
    [PP_DOF_FSTOP] = { "Aperture (F-stop)", "Depth of Field", 4.0f, 0.7f, 32.0f },
    [PP_DOF_MAX_BLUR] = { "Max Bokeh Size", "Depth of Field", 12.0f, 0.0f, 48.0f },
    [PP_GI_INTENSITY] = { "Indirect Lighting Intensity", "Global Illumination", 1.0f, 0.0f, 4.0f },
    [PP_AO_INTENSITY] = { "Ambient Occlusion Intensity", "Global Illumination", 1.0f, 0.0f, 1.0f },
};

void post_defaults(PostSettings* s) {
    for (int i = 0; i < PP_FIELD_COUNT; i++) s->v[i] = g_post_fields[i].def;
    s->color_tint[0] = s->color_tint[1] = s->color_tint[2] = 1.0f;
}

void post_volume_defaults(CPostProcess* v) {
    memset(v, 0, sizeof *v);
    post_defaults(&v->s);
    v->blend_radius = 100.0f;
    v->blend_weight = 1.0f;
    v->flags = PPV_ENABLED | PPV_UNBOUND;
    v->lut_intensity = 1.0f;
}

// (A9.4) the Color Grading LUT at the camera: the highest priority volume there that has one, by its weight
static const char* g_lut;
static float g_lut_w;
// (Q13) the horror kit's volume at the camera: the highest priority one there with its Horror group on, by its weight
static const CPostProcess* g_terror;
static float g_terror_w;
const CPostProcess* post_terror_volume(float* weight) {
    if (weight) *weight = g_terror_w;
    return g_terror;
}

int post_field_index(const char* name) {
    for (int i = 0; i < PP_FIELD_COUNT; i++)
        if (!_stricmp(g_post_fields[i].name, name)) return i;
    return -1;
}

// How much a volume weighs at p: 1 inside its box, fading to 0 over the blend radius outside (unbound: everywhere)
static float volume_weight(const CPostProcess* v, const CTransform* t, const float p[3]) {
    if (!(v->flags & PPV_ENABLED)) return 0.0f;
    float w = fminf(fmaxf(v->blend_weight, 0.0f), 1.0f);
    if (v->flags & PPV_UNBOUND) return w;
    float m[3][3], local[3], d[3] = { p[0] - t->location[0], p[1] - t->location[1], p[2] - t->location[2] };
    rotator_axes(t->rotation, m);
    float out = 0.0f;
    for (int k = 0; k < 3; k++) {
        local[k] = d[0] * m[k][0] + d[1] * m[k][1] + d[2] * m[k][2];
        float half = fabsf(t->scale[k]) * 100.0f, e = fabsf(local[k]) - half;
        if (e > 0.0f) out += e * e;
    }
    out = sqrtf(out);
    if (out <= 0.0f) return w;
    return v->blend_radius > 0.0f ? w * fmaxf(1.0f - out / v->blend_radius, 0.0f) : 0.0f;
}

uint32_t post_blend_at(World* w, const float pos[3], PostSettings* out) {
    post_defaults(out);
    enum { MAXV = 64 };
    const CPostProcess* vs[MAXV];
    const CTransform* ts[MAXV];
    float lws[MAXV]; // (2026-11-17, Q3) its Lighting Level's weight: a level's Post Process Volume fades with it; off, none
    uint32_t n = 0;
    for (uint32_t ai = 0; w && ai < w->archetype_count && n < MAXV; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_POST_PROCESS)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CPostProcess* p = ecs_arch_column(a, COMP_POST_PROCESS);
        const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count && n < MAXV; i++) {
            if (t[i].flags & TRANSFORM_LEVEL_HIDDEN) continue; // (its level's eye off)
            float lw = lights_level_weight(w, ents[i]);
            if (lw <= 0.0f) continue;
            uint32_t k = n++;
            while (k > 0 && vs[k - 1]->priority > p[i].priority) vs[k] = vs[k - 1], ts[k] = ts[k - 1], lws[k] = lws[k - 1], k--; // by priority
            vs[k] = &p[i], ts[k] = &t[i], lws[k] = lw;
        }
    }
    uint32_t mask = 0;
    g_lut = NULL, g_lut_w = 0.0f;
    g_terror = NULL, g_terror_w = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float wt = volume_weight(vs[i], ts[i], pos) * fminf(lws[i], 1.0f);
        if (wt <= 0.0f) continue;
        if (vs[i]->terror_flags) g_terror = vs[i], g_terror_w = wt; // (Q13: by priority, the last wins)
        if (vs[i]->lut[0]) g_lut = vs[i]->lut, g_lut_w = fminf(fmaxf(vs[i]->lut_intensity, 0.0f), 1.0f) * wt; // (by priority: the last wins)
        mask |= vs[i]->overrides;
        for (int f = 0; f < PP_FIELD_COUNT; f++)
            if (vs[i]->overrides >> f & 1u) out->v[f] += (vs[i]->s.v[f] - out->v[f]) * wt;
        if (vs[i]->overrides >> PP_GAIN & 1u)
            for (int k = 0; k < 3; k++) out->color_tint[k] += (vs[i]->s.color_tint[k] - out->color_tint[k]) * wt;
    }
    return mask;
}

const uint8_t g_camera_post_fields[CAMERA_POST_FIELDS] = { PP_EXPOSURE_COMP, PP_BLOOM_INTENSITY, PP_MB_AMOUNT, PP_MB_MAX, PP_DOF_FOCAL_DISTANCE,
                                                          PP_DOF_FSTOP, PP_DOF_MAX_BLUR, PP_VIGNETTE, PP_SATURATION, PP_CONTRAST,
                                                          PP_TEMPERATURE, PP_GRAIN, PP_CHROMATIC, PP_SHARPEN };

// The player's camera: a component of the possessed pawn, else the level's auto-activated camera (NULL: none)
static const CCamera* player_camera(World* w, Entity player) {
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            if (!(a->mask & COMP_BIT(COMP_CAMERA))) continue;
            bool attached = (a->mask & COMP_BIT(COMP_ATTACH)) != 0;
            if (attached != (pass == 0)) continue;
            const CCamera* c = ecs_arch_column(a, COMP_CAMERA);
            const CAttach* at = attached ? ecs_arch_column(a, COMP_ATTACH) : NULL;
            for (uint32_t i = 0; i < a->count; i++)
                if (attached ? (player && at[i].owner == player) : c[i].auto_activate != 0) return &c[i];
        }
    return NULL;
}

// Whether the camera's own motion blurs the view: the player's camera (a component of the possessed pawn, else the
// level's auto-activated camera) unless its Motion Blur says no; without one it does
static bool camera_blur(World* w, Entity player) {
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            if (!(a->mask & COMP_BIT(COMP_CAMERA))) continue;
            bool attached = (a->mask & COMP_BIT(COMP_ATTACH)) != 0;
            if (attached != (pass == 0)) continue;
            const CCamera* c = ecs_arch_column(a, COMP_CAMERA);
            const CAttach* at = attached ? ecs_arch_column(a, COMP_ATTACH) : NULL;
            for (uint32_t i = 0; i < a->count; i++)
                if (attached ? (player && at[i].owner == player) : c[i].auto_activate != 0) return !c[i].no_camera_blur;
        }
    return true;
}

void post_fill_view(World* w, const float pos[3], Entity player, bool playing, PostSettings* store, RenderView* rv) {
    rv->post_overrides = post_blend_at(w, pos, store);
    const CCamera* cam = playing ? player_camera(w, player) : NULL;
    if (cam && cam->post_overrides) { // the player's camera's own settings, over the volumes' (Unreal's camera post process)
        float wt = cam->post_weight > 0.0f ? fminf(cam->post_weight, 1.0f) : 1.0f;
        for (int k = 0; k < CAMERA_POST_FIELDS; k++)
            if (cam->post_overrides >> k & 1u) {
                int f = g_camera_post_fields[k];
                store->v[f] += (cam->post[k] - store->v[f]) * wt;
                rv->post_overrides |= 1u << f;
            }
    }
    rv->post = store;
    rv->grade_lut = g_lut, rv->grade_lut_intensity = g_lut_w; // (the volumes' Color Grading LUT)
    // (A9.4) the Cine Camera: its lens, exposure and shutter over the volumes' (Unreal's Cine Camera Actor)
    rv->cam_manual_exposure = false, rv->dof_focal_mm = rv->dof_sensor_mm = 0.0f, rv->bokeh_blades = 0, rv->bokeh_rot_deg = 0.0f;
    if (cam && cam->cine) {
        float N = cam->aperture > 0.0f ? cam->aperture : 2.8f, t = cam->shutter_s > 0.0f ? cam->shutter_s : 1.0f / 60.0f;
        float iso = cam->iso > 0.0f ? cam->iso : 100.0f;
        if (cam->cine & CINE_LENS) {
            store->v[PP_DOF_FSTOP] = N;
            store->v[PP_DOF_FOCAL_DISTANCE] = cam->focus_cm;
            rv->post_overrides |= 1u << PP_DOF_FSTOP | 1u << PP_DOF_FOCAL_DISTANCE;
            rv->dof_focal_mm = cam->focal_mm > 0.0f ? cam->focal_mm : 35.0f;
            rv->dof_sensor_mm = cam->sensor_mm > 0.0f ? cam->sensor_mm : 36.0f;
            rv->bokeh_blades = cam->blades >= 3 && cam->blades <= 16 ? cam->blades : 0;
            rv->bokeh_rot_deg = cam->blade_rot;
        }
        if (cam->cine & CINE_EXPOSURE) rv->cam_manual_exposure = true, rv->cam_ev100 = log2f(N * N / t) - log2f(iso / 100.0f);
        if (cam->cine & CINE_SHUTTER_BLUR) {
            store->v[PP_MB_AMOUNT] = fminf(fmaxf(t * store->v[PP_MB_TARGET_FPS], 0.0f), 1.0f);
            rv->post_overrides |= 1u << PP_MB_AMOUNT;
        }
    }
    // the editor's flying camera doesn't blur (Unreal's editor views: the moving actors only); playing, the player's
    rv->camera_blur = playing && camera_blur(w, player);
    fpcam_post(cam, store, rv); // (Q12) a First Person Camera's autofocus (the lens' focus and depth of field) and its effort's vignette
    terror_view(w, rv, playing); // (Q13) the horror kit: the tension's lens, the scare rigs, the filters, the horror exposure
}
