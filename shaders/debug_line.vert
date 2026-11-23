#version 460
// (2026-11-19) The debug lines and points (debug_draw.h): one primitive an instance, six vertices a quad in screen space
// (a line its width in pixels; a point a square), drawn over the picture after the composite. Or (the top bit of
// pc.instances.y) the batches' trace records: 18 vertices each - the line to the hit (trace color), after it (hit
// color), a point on the hit; a miss only the first.
// pc.vertices: the primitives or records; pc.instances: the viewport's origin (px); pc.pass, pc.pad: its size (px).
#include "common.glsl"

struct DdPrim {
    vec3 a;
    uint rgba;
    vec3 b;
    float size;
};
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer DdBuf { DdPrim p[]; };
struct DdTrace {
    vec3 s;
    float t;
    vec3 e;
    uint colors;
};
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer DdTraceBuf { DdTrace r[]; };

layout(location = 0) out vec4 o_color;
layout(location = 1) noperspective out float o_across; // -1..1 across the line (its anti-aliased edge)
layout(location = 2) noperspective out float o_width;  // its width (px)
layout(location = 3) out vec2 o_zw;                     // the clip z and w (the fragment's depth against the scene's)

vec4 rgb565(uint c) { return vec4(float((c >> 11) & 31u) / 31.0, float((c >> 5) & 63u) / 63.0, float(c & 31u) / 31.0, 1.0); }

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    vec2 size = vec2(float(pc.pass), float(pc.pad));
    const vec2 q[6] = vec2[6](vec2(0.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(0.0, -1.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
    vec3 pa, pb;
    float psize;
    vec2 c;
    if ((pc.instances.y & 0xC0000000u) == 0xC0000000u) { // a trace record as hardware lines: 8 vertices
        DdTrace r = DdTraceBuf(pc.vertices).r[gl_InstanceIndex];
        int v = gl_VertexIndex;
        bool hit = r.t < 1.0;
        vec3 h = mix(r.s, r.e, r.t);
        o_across = 0.0, o_width = 8.0;
        if (v >= 2 && !hit) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); o_zw = vec2(0.0, 1.0); o_color = vec4(0.0); return; }
        o_color = rgb565(v == 2 || v == 3 ? (r.colors & 0xFFFFu) : (r.colors >> 16));
        vec4 cp;
        if (v < 4) cp = f.tsr_vp * vec4(v == 0 ? r.s : v == 1 ? (hit ? h : r.e) : v == 2 ? h : r.e, 1.0); // (the hardware clips the lines)
        else { // a cross on the hit (4 px each way)
            cp = f.tsr_vp * vec4(h, 1.0);
            vec2 o = v == 4 ? vec2(-4.0, 0.0) : v == 5 ? vec2(4.0, 0.0) : v == 6 ? vec2(0.0, -4.0) : vec2(0.0, 4.0);
            cp.xy += o * 2.0 / size * cp.w;
        }
        gl_Position = cp;
        o_zw = cp.zw;
        return;
    }
    if ((pc.instances.y & 0x80000000u) != 0u) { // a trace record
        DdTrace r = DdTraceBuf(pc.vertices).r[gl_InstanceIndex];
        int seg = gl_VertexIndex / 6;
        c = q[gl_VertexIndex % 6];
        bool hit = r.t < 1.0;
        if (seg > 0 && !hit) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); return; }
        vec3 h = mix(r.s, r.e, r.t);
        pa = seg == 0 ? r.s : h;
        pb = seg == 0 ? (hit ? h : r.e) : seg == 1 ? r.e : h;
        psize = seg == 2 ? -8.0 : 1.0;
        o_color = rgb565(seg == 1 ? (r.colors & 0xFFFFu) : (r.colors >> 16));
    } else {
        DdPrim p = DdBuf(pc.vertices).p[gl_InstanceIndex];
        c = q[gl_VertexIndex];
        pa = p.a, pb = p.b, psize = p.size;
        o_color = unpackUnorm4x8(p.rgba);
    }
    vec4 ca = f.tsr_vp * vec4(pa, 1.0), cb = f.tsr_vp * vec4(pb, 1.0);
    if (psize < 0.0) { // a point: a square around a
        if (ca.w <= 1e-3) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); return; }
        vec2 off = vec2(c.x * 2.0 - 1.0, c.y) * (-psize * 0.5 + 0.75) * 2.0 / size;
        gl_Position = vec4(ca.xy / ca.w + off, ca.z / ca.w, 1.0);
        o_zw = vec2(ca.z / ca.w, 1.0);
        o_across = c.y, o_width = -psize + 1.5;
        return;
    }
    // the part in front of the camera (reversed Z: w is the distance along the view)
    const float near_w = 1e-2;
    if (ca.w < near_w && cb.w < near_w) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); return; }
    if (ca.w < near_w) ca = mix(ca, cb, (near_w - ca.w) / (cb.w - ca.w));
    if (cb.w < near_w) cb = mix(cb, ca, (near_w - cb.w) / (ca.w - cb.w));
    vec2 sa = ca.xy / ca.w * 0.5 * size, sb = cb.xy / cb.w * 0.5 * size; // (pixels from the center)
    vec2 dir = sb - sa;
    float len = length(dir);
    dir = len > 1e-4 ? dir / len : vec2(1.0, 0.0);
    vec2 nrm = vec2(-dir.y, dir.x);
    float w = max(psize, 1.0) + 1.5; // (a pixel and a half more: the edge's fade)
    vec4 cp = c.x < 0.5 ? ca : cb;
    vec2 sp = (c.x < 0.5 ? sa : sb) + nrm * c.y * w * 0.5 + dir * (c.x < 0.5 ? -0.5 : 0.5);
    gl_Position = vec4(sp / (0.5 * size) * cp.w, cp.z, cp.w);
    o_zw = cp.zw;
    o_across = c.y, o_width = w;
}
