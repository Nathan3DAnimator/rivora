// The local lights' shadow maps read (lsm.c): a light's map found by its slot in the frame's table, the point projected
// into its face (a cube's, or a spot's one), a 2x2 (Low) or 3x3 grid of bilinear compares inside the face's border (a
// smooth edge, no noise). Bias: the point lifted along its normal by a texel and a half there (the pipeline adds a
// slope-scaled bias when drawing). Included after common.glsl.
#ifndef LSM_GLSL
#define LSM_GLSL

#define LSM_MAX 64
#define LSM_JOBS 96
#define LSM_CULL 32
#define LSM_HASH 256u
struct LsmLight {
    vec4 pos_near; // the light as drawn, the near plane (cm)
    vec4 params;   // far (cm), the face's side (texels), 1 / the atlas's side, k = far / (far - near)
    uint tile[6];  // per face: x | y << 16 (texels)
    uint faces;    // 1: a spot's one face, 6: the cube's (+X -X +Y -Y +Z -Z)
    float s;       // the projection's scale
    mat4 m;        // a spot's face: world -> clip
};
struct LsmJob {
    mat4 m;          // world -> the face's clip (mesh.vert pass 1)
    vec4 planes[4];  // its sides (inside: dot >= 0)
    vec4 sphere;     // the light, its range
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer LsmBuf {
    uvec2 hash[LSM_HASH]; // slot + 1 (0: empty), the light's index
    LsmLight l[LSM_MAX];
    LsmJob j[LSM_JOBS];
    vec4 cull[LSM_CULL]; // the lights drawn this frame: center, range (negative: only their dynamic casters)
};

#ifndef LSM_NO_SAMPLER
layout(set = 0, binding = 66) uniform sampler2DShadow u_lsm; // compare (less or equal: lit)

// The light of this slot's map (-1: none, unshadowed)
int lsm_find(FrameBuf f, uint slot) {
    LsmBuf b = LsmBuf(f.lsm_tab);
    uint h = (slot * 2654435761u) >> 24u; // (lsm.c the same)
    int e = -1;
    for (uint k = 0u; k < 8u; k++) {
        uvec2 kv = b.hash[(h + k) & (LSM_HASH - 1u)];
        if (kv.x == slot + 1u) e = int(kv.y);
        if (kv.x == 0u || e >= 0) break;
    }
    return e;
}

// 1 lit, 0 shadowed: the point p (n: its normal toward the light) by the map of the light at slot. One return at the
// end, nothing returned early (inside the lights' loop the early returns made NVIDIA's compiler keep 48 bytes in local
// memory: test shader_spill)
float lsm_shadow(FrameBuf f, uint slot, vec3 p, vec3 n) {
    int e = lsm_find(f, slot);
    float vis = 1.0;
    if (e >= 0) {
        LsmBuf b = LsmBuf(f.lsm_tab);
        vec4 pn = b.l[e].pos_near;
        float s = b.l[e].s, res = b.l[e].params.y;
        bool spot = b.l[e].faces == 1u;
        // the lift: a texel and a half of this distance (a face's texel spans 2 w / (res s) cm)
        vec3 d = p - pn.xyz;
        vec3 a = abs(d);
        p += n * (3.0 * max(max(a.x, max(a.y, a.z)), pn.w) / (res * s)); // (a spot: about its axis's distance)
        vec3 c;
        uint face = 0u;
        if (spot) {
            vec4 h = b.l[e].m * vec4(p, 1.0);
            c = vec3(h.xy, h.w);
        } else { // the cube's face by the largest axis (lsm.c FACE: W, U, V)
            d = p - pn.xyz;
            a = abs(d);
            if (a.x >= a.y && a.x >= a.z) c = d.x >= 0.0 ? vec3(d.y, d.z, a.x) : vec3(-d.y, d.z, a.x), face = d.x >= 0.0 ? 0u : 1u;
            else if (a.y >= a.z) c = d.y >= 0.0 ? vec3(-d.x, d.z, a.y) : vec3(d.x, d.z, a.y), face = d.y >= 0.0 ? 2u : 3u;
            else c = d.z >= 0.0 ? vec3(d.x, d.y, a.z) : vec3(d.x, -d.y, a.z), face = d.z >= 0.0 ? 4u : 5u;
            c.xy *= s;
        }
        // (behind the near plane, or outside a spot's cone: no light there anyway)
        if (c.z > pn.w && all(lessThanEqual(abs(c.xy), vec2(c.z)))) {
            vec4 pr = b.l[e].params;
            float z = pr.w * (1.0 - pn.w / c.z);
            uint t = b.l[e].tile[face];
            vec2 org = vec2(float(t & 0xFFFFu), float(t >> 16u));
            vec2 tp = org + (c.xy / c.z * 0.5 + 0.5) * res; // texels in the atlas
            // the filter's taps inside the face (its border holds what lies past its edge)
            vec2 lo = org + 1.0, hi = org + res - 1.0;
            float sum = 0.0;
            if (f.lsm_flags <= 4u) { // Low: a 2x2 grid of bilinear compares (a tent over 3x3 texels)
                for (int k = 0; k < 4; k++) sum += texture(u_lsm, vec3(clamp(tp + vec2(k & 1, k >> 1) - 0.5, lo, hi) * pr.z, z));
                sum *= 0.25;
            } else {
                for (int k = 0; k < 9; k++) // 3x3 (a tent over 4x4 texels)
                    sum += texture(u_lsm, vec3(clamp(tp + vec2(k % 3 - 1, k / 3 - 1), lo, hi) * pr.z, z));
                sum *= 1.0 / 9.0;
            }
            vis = sum;
        }
    }
    return vis;
}
#endif

#endif
