// (Q21) The distant view (farview.c, notas-dev/visao-distante.md): the terrain's far shadow past the sun's virtual maps
// and the impostors' octahedral atlases. The frame's far_* fields (render_gpu.h GpuFrame) say what is on.
#ifndef FARVIEW_GLSL
#define FARVIEW_GLSL

#define FAR_SHADOW 1u    // far_u.x: the heightfield's horizon map is there (farshadow.comp)
#define FAR_IMP 2u       // ... the impostors are drawn (cull.comp swaps the far instances for their card)
#define FAR_FOG_SKY 4u   // ... the analytic fog's ambient light is the sky's horizon (weather.glsl fog_far_light)

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer FarFloatBuf { float v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer ImpTabBuf { uvec4 e[]; };

// --- The far shadow: per texel of the landscapes' heightfield (far_buf: the heights in cm, then per texel the steepest
// rise toward the sun as tan(angle) and its distance in m, two halves) ---

// The sun's share against one horizon word k (0: the rise nearer than the split, 1: past it), bilinear over its 4 texels
// (the rise and its distance interpolated, then the sun's disk once), raised by how far p stands over the ground there
float far_word_vis(FrameBuf f, uint a, uint res, vec2 fr, uint k, float dz) {
    UintBuf hz = UintBuf(addr_add(f.far_buf, res * res * 4u));
    vec2 h00 = unpackHalf2x16(hz.v[a * 2u + k]), h10 = unpackHalf2x16(hz.v[(a + 1u) * 2u + k]);
    vec2 h01 = unpackHalf2x16(hz.v[(a + res) * 2u + k]), h11 = unpackHalf2x16(hz.v[(a + res + 1u) * 2u + k]);
    vec2 hd = mix(mix(h00, h10, fr.x), mix(h01, h11, fr.x), fr.y);
    float d_cm = max(hd.y, 0.01) * 100.0;
    float t = hd.x - dz / d_cm;           // the horizon seen from p, not from the ground under it
    float w = 0.0047 + 0.5 * f.far_f.z / (d_cm + f.far_f.z); // the sun's radius, widened by a texel seen from there
    return clamp((f.far_g.y - atan(t)) / (2.0 * w) + 0.5, 0.0, 1.0);
}

// The terrain's shadow on the sun at p (1: lit; outside the heightfield, or off: 1). far_only: only what rises past the
// split (inside the sun's virtual maps, which draw the nearer terrain themselves)
float far_terrain_shadow(FrameBuf f, vec3 p, bool far_only) {
    if ((f.far_u.x & FAR_SHADOW) == 0u) return 1.0;
    uint res = f.far_u.w;
    vec2 g = (p.xy - f.far_f.xy) / f.far_f.z - 0.5;
    if (any(lessThan(g, vec2(0.0))) || any(greaterThanEqual(g, vec2(float(res - 1u))))) return 1.0;
    uvec2 i0 = uvec2(g);
    vec2 fr = g - vec2(i0);
    uint a = i0.y * res + i0.x;
    FarFloatBuf hb = FarFloatBuf(f.far_buf);
    float h = mix(mix(hb.v[a], hb.v[a + 1u], fr.x), mix(hb.v[a + res], hb.v[a + res + 1u], fr.x), fr.y);
    float dz = max(p.z - h, 0.0);
    float v = far_word_vis(f, a, res, fr, 1u, dz);
    if (!far_only) v = min(v, far_word_vis(f, a, res, fr, 0u, dz));
    return mix(1.0, v, f.far_g.x);
}

#endif
