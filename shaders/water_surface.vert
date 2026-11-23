#version 460
// The water's surface (water.c): no vertex buffer, the grid from gl_VertexIndex.
//   pc.pass < 4: a lake / pool (its rectangle in f.water[29 + 2 pass]): a 128 x 128 grid, its two wave trains (the
//                same as the boats feel: vehicle_sim.inc water_at)
//   pc.pass 100: the ocean: rings around the camera (200 rings of 256 segments, from 30 cm apart to ~20 km out),
//                displaced by the FFT's three cascades, each fading where its waves are smaller than a vertex
// Both get the ripples of what moves in them.
#include "common.glsl"
#include "water.glsl"

layout(location = 0) out vec3 o_world;
layout(location = 1) out vec2 o_base;    // the point before the displacement (the maps' place)
layout(location = 2) out float o_spacing; // the grid's spacing there (cm): the cascades' fade
layout(location = 3) out vec3 o_flow;     // (B12c) a river's flow there (cm/s, xy; z 1 on a river)

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    uint vi = uint(gl_VertexIndex), q = vi / 6u, k = vi % 6u;
    const uvec2 corner[6] = uvec2[6](uvec2(0, 0), uvec2(1, 0), uvec2(1, 1), uvec2(0, 0), uvec2(1, 1), uvec2(0, 1));
    uvec2 cc = corner[k];
    vec2 xy;
    float level, spacing;
    vec3 disp = vec3(0.0);
    o_flow = vec3(0.0);
    if (pc.pass >= 200u) { // (B12c) a river: a ribbon RIVER_ACROSS quads wide along its samples
        RiverBuf R = RiverBuf(pc.instances); // (pc.vertices: the meshes', the rays read them)
        uint base = (pc.pass - 200u) * RIVER_STRIDE;
        uint seg = q / RIVER_ACROSS, col = q % RIVER_ACROSS;
        uint at = min(seg + cc.y, uint(R.v[base].x) - 1u);
        vec4 s0 = R.v[base + 4u + 2u * at], s1 = R.v[base + 5u + 2u * at];
        float u = float(col + cc.x) / float(RIVER_ACROSS) - 0.5;
        vec2 right = vec2(-s1.y, s1.x);
        xy = s0.xy + right * (u * s0.w);
        level = s0.z;
        spacing = s0.w / float(RIVER_ACROSS);
        float edge = 1.0 - abs(u) * 2.0;
        o_flow = vec3(s1.xy * s1.z * (0.35 + 0.65 * sqrt(max(edge, 0.0))), 1.0);
    } else if (pc.pass >= 100u) {
        const uint S = 256u;
        float ring = float(q / S + cc.y), seg = float(q % S + cc.x);
        float r = 30.0 * (pow(1.04, ring) - 1.0) / 0.04;
        float a = seg / float(S) * 6.2831853;
        vec2 center = floor(f.camera_pos.xy / 50.0) * 50.0; // (snapped: the near rings don't swim)
        xy = center + r * vec2(cos(a), sin(a));
        level = f.water[7].x;
        spacing = max(30.0 * pow(1.04, ring), r * 6.2831853 / float(S));
        for (uint c = 0u; c < 3u; c++) {
            float L = W_CASCADE_SIDE(f, c) * 100.0;
            float fade = 1.0 - smoothstep(L / float(W_OCEAN_N) * 2.0, L / float(W_OCEAN_N) * 8.0, spacing); // (waves under ~4 vertices: off)
            disp += ocean_tap(f, c, xy, 0u).xyz * 100.0 * fade;
        }
        disp.xy *= smoothstep(0.0, 1.0, f.water[7].y); // (choppiness 0: no sideways pull)
    } else {
        uint b = min(pc.pass, 3u);
        vec4 a0 = f.water[29u + 2u * b], a1 = f.water[30u + 2u * b];
        vec2 uv = vec2(float(q % 128u + cc.x), float(q / 128u + cc.y)) / 128.0;
        xy = a0.xy + (uv * 2.0 - 1.0) * vec2(a0.w, a1.x);
        level = a0.z;
        spacing = 2.0 * max(a0.w, a1.x) / 128.0;
        float t = f.water[0].y, kk = a1.z, w = a1.w;
        disp.z = a1.y * (0.6 * sin(kk * xy.x - w * t) + 0.4 * sin(kk * 0.7 * (xy.x * 0.6 + xy.y * 0.8) - w * 0.8 * t));
    }
    disp.z += ripple_at(f, xy);
    vec3 world = vec3(xy + disp.xy, level + disp.z);
    o_world = world;
    o_base = xy;
    o_spacing = spacing;
    gl_Position = f.view_proj * vec4(world, 1.0);
}
