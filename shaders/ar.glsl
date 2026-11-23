// AR (ar_arcore.c, render.c): the camera's picture and the real world's depth, as the screen sees them (sky.frag,
// ar_occlude.frag). f.ar: 0, 1 the screen's corners in the picture; 2 on, the shadows' strength, the picture's size; 3 the
// depth on, its size, the projection's near (cm); 4, 5 the screen's corners in the depth
layout(set = 0, binding = 63) uniform sampler2D u_ar_y;     // the picture's Y
layout(set = 0, binding = 64) uniform sampler2D u_ar_uv;    // its U, V at half size
layout(set = 0, binding = 65) uniform sampler2D u_ar_depth; // the real depth: R16 unorm, mm / 65535 (0: unknown)

vec2 ar_corner_uv(vec4 a, vec4 b, vec2 ndc) {
    vec2 s = ndc * 0.5 + 0.5;
    return mix(mix(a.xy, a.zw, s.x), mix(b.xy, b.zw, s.x), s.y);
}

// The camera's picture at a screen point, so that the composite after (the exposure, the ACES fit at 0.6x) gives back the
// camera's colors
vec4 ar_camera_color(FrameBuf f, vec2 ndc) {
    vec2 uv = ar_corner_uv(f.ar[0], f.ar[1], ndc);
    float Y = texture(u_ar_y, uv).r;
    vec2 C = texture(u_ar_uv, uv).rg - 0.5;
    vec3 cs = clamp(vec3(Y + 1.402 * C.y, Y - 0.344136 * C.x - 0.714136 * C.y, Y + 1.772 * C.x), 0.0, 1.0); // (BT.601, full range)
    vec3 lin = mix(cs / 12.92, pow((cs + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), cs));
    vec3 y = min(lin, vec3(0.97));
    vec3 a = 2.43 * y - 2.51, b = 0.59 * y - 0.03, c = 0.14 * y;
    vec3 x = (-b - sqrt(max(b * b - 4.0 * a * c, vec3(0.0)))) / (2.0 * a);
    ExposureBuf e = ExposureBuf(f.exposure);
    vec3 v = max(x, vec3(0.0)) / 0.6 / max(e.exposure / max(e.pre_exposure, 1e-12), 1e-12);
    return (f.gi_flags & GI_AA_WEIGHTED) != 0u ? aa_encode(v) : vec4(v, 1.0);
}

// The real depth at a screen point (cm along the view axis; 0: unknown): bilinear over the 4 nearest texels, the unknown
// ones left out (an unknown 0 blended in would read as very near)
float ar_real_depth(FrameBuf f, vec2 ndc) {
    vec2 sz = f.ar[3].yz;
    vec2 p = ar_corner_uv(f.ar[4], f.ar[5], ndc) * sz - 0.5;
    ivec2 i = ivec2(floor(p)), hi = ivec2(sz) - 1;
    vec2 t = p - vec2(i);
    float d00 = texelFetch(u_ar_depth, clamp(i, ivec2(0), hi), 0).r;
    float d10 = texelFetch(u_ar_depth, clamp(i + ivec2(1, 0), ivec2(0), hi), 0).r;
    float d01 = texelFetch(u_ar_depth, clamp(i + ivec2(0, 1), ivec2(0), hi), 0).r;
    float d11 = texelFetch(u_ar_depth, clamp(i + ivec2(1, 1), ivec2(0), hi), 0).r;
    vec4 d = vec4(d00, d10, d01, d11);
    vec4 w = vec4((1.0 - t.x) * (1.0 - t.y), t.x * (1.0 - t.y), (1.0 - t.x) * t.y, t.x * t.y) * step(vec4(1e-7), d);
    float ws = w.x + w.y + w.z + w.w;
    return ws > 0.05 ? dot(d, w) / ws * 6553.5 : 0.0; // (unorm x 65535 mm / 10)
}
