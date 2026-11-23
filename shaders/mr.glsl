// (Q25b) Mixed reality (xr_mr.c, render.c render_mr_frame, notas-dev/xr.md): the real room behind and among the virtual
// things on a passthrough headset. composite.frag gives the eye's image an alpha the compositor blends over the
// passthrough layer (premultiplied): 0 where the real world shows (the sky's place with Background, a real thing nearer
// than the virtual one, a Passthrough Window), the real surfaces darkened where the virtual things shadow them. The
// simulated headset (--vr-sim) draws a stand-in room in the real world's place, styled as the headset would.
// f.mr: 0 on, the virtual's opacity, the soft occlusion's ramp (cm), the shadows' strength; 1 the passthrough's opacity,
// style, brightness, contrast; 2 saturation, tint rgb; 3 edge color rgba; 4-7 this eye's depth camera (position + near cm,
// forward + far cm (0: infinite), right + tan left, up + tan right); 8 tan up, tan down, its layer, the depth's y flipped;
// 9 the depth's size, the projection's near (cm), -; 10 the stand-in room's lamp (world, cm)
layout(set = 0, binding = 86) uniform sampler2DArray u_mr_depth; // the real depth (D16: the headset's, or the simulated one)

#define MR_ON 1u
#define MR_BACKGROUND 2u      // nothing drawn (the sky's place): the real room
#define MR_SIM 4u             // the simulated headset: the stand-in room drawn here
#define MR_DEPTH 8u           // u_mr_depth holds this frame's real depth
#define MR_ROOM_OCCLUDES 16u  // the room's boxes (planes, volumes) hide what is behind them
#define MR_SHADOWS 32u        // the virtual things shadow the real surfaces (the sun's shadow maps)
#define MR_KIND_ROOM 0.0
#define MR_KIND_WINDOW 1.0

// A box as world -> its unit cube ([-1, 1] each way: three rows, made on the CPU so the shader divides nothing); info:
// its kind (0 the room, 1 a window), its label
struct MrBox { vec4 rx; vec4 ry; vec4 rz; vec4 info; };
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer MrBoxBuf { MrBox b[]; };

// One pass over the boxes along a ray: the nearest window's entry (tw) and the nearest room surface's (tr), its normal
// and label; -1: none (a box the eye is inside of hides nothing: a table leant into)
void mr_hits(FrameBuf f, vec3 o, vec3 d, out float tw, out float tr, out vec3 n, out float label) {
    float bw = 1e9, br = 1e9;
    n = vec3(0.0, 0.0, 1.0), label = -1.0;
    MrBoxBuf bb = MrBoxBuf(f.mr_boxes);
    for (uint i = 0u; i < f.mr_box_count; i++) {
        MrBox b = bb.b[i];
        vec3 lo = vec3(dot(b.rx.xyz, o) + b.rx.w, dot(b.ry.xyz, o) + b.ry.w, dot(b.rz.xyz, o) + b.rz.w);
        vec3 ld = vec3(dot(b.rx.xyz, d), dot(b.ry.xyz, d), dot(b.rz.xyz, d));
        vec3 inv = 1.0 / mix(ld, vec3(1e-9), lessThan(abs(ld), vec3(1e-9)));
        vec3 t0 = (-1.0 - lo) * inv, t1 = (1.0 - lo) * inv;
        vec3 mn = min(t0, t1), mx = max(t0, t1);
        float tn = max(max(mn.x, mn.y), mn.z), tf = min(min(mx.x, mx.y), mx.z);
        if (tf < tn || tn <= 0.0) continue;
        if (b.info.x > 0.5) bw = min(bw, tn);
        else if (tn < br) {
            br = tn, label = b.info.y;
            vec3 row = tn == mn.x ? b.rx.xyz : tn == mn.y ? b.ry.xyz : b.rz.xyz;
            float s = tn == mn.x ? ld.x : tn == mn.y ? ld.y : ld.z;
            n = normalize(row) * (s > 0.0 ? -1.0 : 1.0);
        }
    }
    tw = bw < 1e9 ? bw : -1.0, tr = br < 1e9 ? br : -1.0;
}

// The real depth (cm along the depth camera's axis) at a texel of this eye's layer; 0 unknown (D16 1.0: nothing measured)
float mr_depth_texel(FrameBuf f, ivec2 p) {
    ivec2 sz = ivec2(f.mr[9].xy);
    float d = texelFetch(u_mr_depth, ivec3(clamp(p, ivec2(0), sz - 1), int(f.mr[8].z)), 0).r;
    if (d <= 0.0 || d >= 1.0) return 0.0;
    float n = f.mr[4].w, fa = f.mr[5].w;
    if (fa <= 0.0) return n / (1.0 - d); // (an infinite far: OpenGL's and Vulkan's depth agree)
    float ndc = d * 2.0 - 1.0; // (Meta's: OpenGL's [-1, 1] stored as [0, 1])
    return 2.0 * n * fa / (fa + n - ndc * (fa - n));
}

// A world point in this eye's depth camera: its texel coordinates (px) and its own depth there (cm); false: behind it
bool mr_depth_project(FrameBuf f, vec3 p, out vec2 px, out float z) {
    vec3 v = p - f.mr[4].xyz;
    z = dot(v, f.mr[5].xyz);
    px = vec2(0.0);
    if (z <= 1.0) return false;
    float x = dot(v, f.mr[6].xyz) / z, y = dot(v, f.mr[7].xyz) / z;
    float tl = f.mr[6].w, tr = f.mr[7].w, tu = f.mr[8].x, td = f.mr[8].y;
    vec2 uv = vec2((x - tl) / (tr - tl), (tu - y) / (tu - td)); // (row 0 the top, Vulkan's; f.mr[8].w flips)
    if (f.mr[8].w > 0.5) uv.y = 1.0 - uv.y;
    px = uv * f.mr[9].xy - 0.5;
    return true;
}

// How much of a virtual point the real world hides (0..1): the real depth's 4 texels around where it projects, each
// nearer than it by more than the ramp hiding it whole (a soft edge: Meta's own occlusion's)
float mr_depth_occlusion(FrameBuf f, vec3 p) {
    vec2 px;
    float z;
    if (!mr_depth_project(f, p, px, z)) return 0.0;
    if (px.x < -0.5 || px.y < -0.5 || px.x > f.mr[9].x - 0.5 || px.y > f.mr[9].y - 0.5) return 0.0;
    ivec2 i = ivec2(floor(px));
    vec2 t = px - vec2(i);
    float ramp = max(f.mr[0].z, 0.5), bias = 1.0 + z * 0.01;
    float o = 0.0, wsum = 0.0;
    for (int k = 0; k < 4; k++) {
        ivec2 o2 = ivec2(k & 1, k >> 1);
        float w = (o2.x == 1 ? t.x : 1.0 - t.x) * (o2.y == 1 ? t.y : 1.0 - t.y);
        float r = mr_depth_texel(f, i + o2);
        if (r <= 0.0) continue; // (unknown: no say)
        o += w * clamp((z - bias - r) / ramp, 0.0, 1.0);
        wsum += w;
    }
    return wsum > 0.05 ? o / wsum : 0.0;
}

// The real surface along an eye's ray, from the real depth: its point and normal (from the neighbors' points)
bool mr_depth_surface(FrameBuf f, vec3 ro, vec3 rd, out vec3 p, out vec3 n) {
    vec2 px;
    float z;
    p = vec3(0.0), n = vec3(0.0, 0.0, 1.0);
    if (!mr_depth_project(f, ro + rd * 300.0, px, z)) return false; // (the eye and the depth camera a few cm apart)
    ivec2 i = ivec2(floor(px + 0.5));
    vec2 sz = f.mr[9].xy;
    if (i.x < 1 || i.y < 1 || i.x >= int(sz.x) - 1 || i.y >= int(sz.y) - 1) return false;
    float tl = f.mr[6].w, tr = f.mr[7].w, tu = f.mr[8].x, td = f.mr[8].y;
    vec3 q[3];
    ivec2 at[3] = ivec2[3](i, i + ivec2(1, 0), i + ivec2(0, 1));
    for (int k = 0; k < 3; k++) {
        float r = mr_depth_texel(f, at[k]);
        if (r <= 0.0) return false;
        vec2 uv = (vec2(at[k]) + 0.5) / sz;
        if (f.mr[8].w > 0.5) uv.y = 1.0 - uv.y;
        float x = mix(tl, tr, uv.x), y = mix(tu, td, uv.y);
        q[k] = f.mr[4].xyz + (f.mr[5].xyz + f.mr[6].xyz * x + f.mr[7].xyz * y) * r;
    }
    p = q[0];
    vec3 c = cross(q[1] - q[0], q[2] - q[0]);
    n = length(c) > 1e-4 ? normalize(c) : vec3(0.0, 0.0, 1.0);
    if (dot(n, rd) > 0.0) n = -n;
    return true;
}

// A white noise from the pixel and the frame (the stand-in's grain; composite.frag's noise01 isn't in every includer)
float mr_noise(uvec2 p, uint frame) {
    uint v = p.x * 1973u + p.y * 9277u + frame * 26699u;
    v = (v ^ 61u) ^ (v >> 16u), v *= 9u, v ^= v >> 4u, v *= 0x27d4eb2du, v ^= v >> 15u;
    return float(v) * (1.0 / 4294967296.0);
}

// The stand-in room's look (--vr-sim): a color by label, lit by a ceiling lamp, a little of the headset's grain
vec3 mr_sim_room(FrameBuf f, vec3 p, vec3 n, float label, bool hit, vec2 fc) {
    if (!hit) return vec3(0.05);
    vec3 base = label < 0.5 ? vec3(0.42, 0.30, 0.20)  // floor: wood
              : label < 1.5 ? vec3(0.85, 0.85, 0.82)  // ceiling
              : label < 2.5 ? vec3(0.72, 0.70, 0.64)  // wall
              : label < 3.5 ? vec3(0.30, 0.20, 0.13)  // table
              : label < 4.5 ? vec3(0.30, 0.36, 0.45)  // couch
              : label < 6.5 ? vec3(0.55, 0.58, 0.62)  // bed, storage
              : label < 7.5 ? vec3(0.05, 0.05, 0.06)  // screen
              : label < 9.5 ? vec3(0.65, 0.75, 0.90)  // window, door
                            : vec3(0.50);
    vec3 lamp = vec3(f.mr[10].xy, f.mr[10].z); // (the room's ceiling center, a little under it)
    vec3 l = lamp - p;
    float dl = length(l);
    float lit = 0.35 + 0.65 * max(dot(n, l / max(dl, 1.0)), 0.0) * clamp(250.0 / max(dl, 1.0), 0.3, 1.0);
    float grain = (mr_noise(uvec2(fc), f.frame_index) - 0.5) * 0.06;
    return clamp(base * lit + grain, 0.0, 1.0);
}

// The passthrough's style on a real color (the simulated headset; the real one does it in its compositor): brightness
// -100..100, contrast and saturation 1 neutral, 1 black & white, 2 one color (tint by luminance), the opacity over black
vec3 mr_style(FrameBuf f, vec3 c) {
    int style = int(f.mr[1].y + 0.5);
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    if (style == 1) c = vec3(l);
    else if (style == 2) c = f.mr[2].yzw * l * 1.6;
    else if (style == 3) {
        c = (c - 0.5) * f.mr[1].w + 0.5 + f.mr[1].z * 0.01;
        c = max(mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, f.mr[2].x), vec3(0.0));
    }
    return clamp(c, 0.0, 1.0) * f.mr[1].x;
}
