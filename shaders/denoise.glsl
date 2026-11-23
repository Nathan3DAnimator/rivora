// The unified denoiser's packing (A9.1d): mesh.frag's RE_DEN variants write it, denoise.comp reads it. Apart from
// common.glsl so that the other shaders' code stays exactly what it was
// The unified denoiser's (A9.1d: mesh.frag o_den, denoise.comp). A color in 32 bits as R11G11B10F (the half floats'
// bits rounded: 6, 6 and 5 bits of mantissa; non-negative, under 60000): its noise is far larger than the steps
uint f11_bits(float v, uint shift) {
    uint h = packHalf2x16(vec2(clamp(v, 0.0, 60000.0), 0.0)) & 0x7FFFu;
    return min((h + (1u << (shift - 1u))) >> shift, (1u << (15u - shift)) - 1u);
}
// A signal not traced this frame (GI_DEN_CHECKER: the pixel traced the other one): denoise.comp's pre-blur takes it from
// the neighbors that did. No color packs to it (each channel clamps under 60000)
#define DEN_MISSING 0xFFFFFFFFu
// mesh.frag's o_den.y before den_trace.comp: the rays this pixel wants (the diffuse, the rough specular)
#define DEN_RAY_D 0x40000000u
#define DEN_RAY_S 0x80000000u
uint pack_r11g11b10(vec3 c) { return f11_bits(c.r, 4u) | (f11_bits(c.g, 4u) << 11u) | (f11_bits(c.b, 5u) << 22u); }
vec3 unpack_r11g11b10(uint u) {
    return vec3(unpackHalf2x16((u & 0x7FFu) << 4u).x, unpackHalf2x16(((u >> 11u) & 0x7FFu) << 4u).x, unpackHalf2x16((u >> 22u) << 5u).x);
}
// A surface's key: its normal (octahedral, 10 + 10 bits), its roughness (6 bits), a hash of its actor (5 bits) and a
// set bit (0: no surface of ours)
uint den_key(vec3 n, float rough, uint actor) {
    vec3 on = n / (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 oe = on.z >= 0.0 ? on.xy : (1.0 - abs(on.yx)) * vec2(on.x >= 0.0 ? 1.0 : -1.0, on.y >= 0.0 ? 1.0 : -1.0);
    uvec2 q = uvec2(clamp(oe * 0.5 + 0.5, 0.0, 1.0) * 1023.0 + 0.5);
    return q.x | (q.y << 10u) | (uint(clamp(rough, 0.0, 1.0) * 63.0 + 0.5) << 20u) | (((actor * 2654435761u) >> 27u) << 26u) | 0x80000000u;
}
vec3 den_key_normal(uint k) {
    vec2 e = vec2(float(k & 1023u), float((k >> 10u) & 1023u)) / 1023.0 * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}
float den_key_rough(uint k) { return float((k >> 20u) & 63u) / 63.0; }
uint den_key_actor(uint k) { return (k >> 26u) & 31u; }
