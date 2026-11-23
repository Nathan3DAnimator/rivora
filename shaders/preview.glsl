// The asset previews (render_preview.c): a view's data. Layout: render_preview.c PvFrame (scalar).
#ifndef PREVIEW_GLSL
#define PREVIEW_GLSL

#define PV_LIT 0
#define PV_UNLIT 1
#define PV_WIREFRAME 2
#define PV_LIT_WIREFRAME 3
#define PV_LIGHTING_ONLY 4
#define PV_NORMALS 5
#define PV_ROUGHNESS 6
#define PV_METALLIC 7
#define PV_ALBEDO 8   // (Q7)
#define PV_AO 9
#define PV_UV 10
#define PV_LOD 11
#define PV_OVERDRAW 12

// Instance.ids.y in a preview (its own bits; INST_MATERIAL is the scene's)
#define PVI_SELECTED 1u
#define PVI_FLOOR 2u
#define PVI_LOD_SHIFT 8u // (Q7) bits 8-11: the LOD drawn

#define PVF_GRID 1u
#define PVF_SHADOW 2u
#define PVF_SKY 4u // (Q7) the outdoor sky behind and in the reflections

layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer PvFrame {
    mat4 view_proj;
    mat4 light_matrix;   // world -> the key light's shadow map: xy in [-1, 1], z depth in [0, 1]
    vec4 eye;            // xyz cm, w: time (s): the animated materials
    vec4 key;            // xyz toward the key light, w: its intensity
    vec4 fill;
    vec4 rim;
    vec4 floor_params;   // x: the floor's height, y: where it has faded out (cm from the target), z: the grid's cell, w: a shadow texel (cm)
    vec4 target;         // xyz: what the camera turns around, w: the subject's radius
    vec4 bg_top;         // the background's gradient (display values)
    vec4 bg_bottom;
    vec4 viewport;       // w, h (px), 1 / w, 1 / h
    uvec2 materials;     // the GpuMaterial slots (material.c)
    uint mode;           // PV_
    uint flags;          // PVF_
    vec4 cam_fwd;        // (Q7) the view's axes; w: tan of the half fov (up: vertical, right: horizontal)
    vec4 cam_right;
    vec4 cam_up;
    uvec2 ao_depth;      // (Q7) the AO's depth pass: a uint per pixel (the view depth's float bits, the nearest)
    uint ao_w;
    uint ao_h;
};
layout(buffer_reference, scalar, buffer_reference_align = 4) buffer PvDepth { uint d[]; };

vec3 pv_aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
vec3 pv_encode(vec3 lin) { return mix(lin * 12.92, 1.055 * pow(lin, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), lin)); }
// The picture's look (bp_preview.c's: ACES at 0.8), sRGB-encoded (the UI samples the image as it is)
vec3 pv_display(vec3 hdr) { return pv_encode(pv_aces(hdr * 0.8)); }
vec3 pv_background(PvFrame f, float y01) { return mix(f.bg_top.rgb, f.bg_bottom.rgb, clamp(y01, 0.0, 1.0)); }
// (Q7) the outdoor sky (linear): a blue zenith, a pale horizon, a warm haze and the sun's disk where the key light is
vec3 pv_sky(PvFrame f, vec3 d) {
    float h = clamp(d.z, -1.0, 1.0), up = max(h, 0.0);
    vec3 zenith = vec3(0.10, 0.24, 0.62), horizon = vec3(0.62, 0.74, 0.90), ground = vec3(0.16, 0.14, 0.12);
    vec3 c = h >= 0.0 ? mix(horizon, zenith, pow(up, 0.55)) : mix(horizon * 0.7, ground, clamp(-h * 4.0, 0.0, 1.0));
    float sd = max(dot(d, normalize(f.key.xyz)), 0.0);
    c += vec3(1.0, 0.78, 0.5) * (pow(sd, 6.0) * 0.25 + pow(sd, 900.0) * 30.0);
    return c * 1.4;
}
#endif
