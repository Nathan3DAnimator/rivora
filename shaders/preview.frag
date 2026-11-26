#version 460
// The asset previews (render_preview.c, notas-dev/editor.md): Unreal's asset editors' studio. The materials as the
// main pass reads them (their baked textures, mesh.frag), GGX + Lambert under a key light with its shadow map (PCF), a
// fill and a rim, a studio environment for the ambient and the reflections (split sum, Karis's analytic fit); the floor
// takes the shadow and the grid and fades into the background. pass 3: the wireframe's lines.
#include "common.glsl"
#include "preview.glsl"

layout(set = 0, binding = 0) uniform sampler2DShadow u_pv_shadow;
#define MAT_SET 1
#include "mat_common.glsl"
#include "toon.glsl" // (Q11) a Toon material in the Material Editor's preview as in the level

layout(location = 0) in vec3 i_world;
layout(location = 1) in vec3 i_normal;
layout(location = 2) flat in uint i_instance;
layout(location = 3) in vec2 i_uv;
layout(location = 0) out vec4 o_color;

const vec3 SKY = vec3(0.34, 0.40, 0.52), GROUND = vec3(0.10, 0.09, 0.08), KEY = vec3(1.75, 1.65, 1.5), RIM = vec3(0.45, 0.55, 0.75);
const vec3 SEL = vec3(1.0, 0.55, 0.12);

float d_ggx(float nh, float a) {
    float a2 = a * a, d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}
float v_smith(float nv, float nl, float a) {
    float a2 = a * a;
    float gv = nl * sqrt(nv * nv * (1.0 - a2) + a2), gl = nv * sqrt(nl * nl * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}
vec3 brdf(vec3 n, vec3 v, vec3 l, vec3 diff, vec3 f0, float a) {
    vec3 h = normalize(v + l);
    float nl = max(dot(n, l), 0.0), nv = max(dot(n, v), 1e-4), nh = max(dot(n, h), 0.0), vh = max(dot(v, h), 0.0);
    vec3 F = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
    return (diff / PI + d_ggx(nh, a) * v_smith(nv, nl, a) * F) * nl;
}
// The studio around: a sky above, the dark floor below, a softbox where the key light is (blurred by the roughness);
// (Q7) outdoors: the sky itself, toward its mean as the roughness grows
vec3 studio(PvFrame f, vec3 d, float rough) {
    if ((f.flags & PVF_SKY) != 0u) return mix(pv_sky(f, d), mix(vec3(0.16, 0.14, 0.12), vec3(0.45, 0.6, 0.85), smoothstep(-0.3, 0.4, d.z)), rough * rough);
    vec3 c = mix(GROUND, SKY * 1.6, smoothstep(-0.25, 0.35, d.z));
    float spread = mix(48.0, 2.0, rough);
    return c + KEY * 0.9 * pow(max(dot(d, f.key.xyz), 0.0), spread) * (spread + 2.0) / 50.0;
}
vec2 env_brdf(float nv, float rough) { // Karis, "Physically Based Shading on Mobile"
    vec4 r = rough * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);
    float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}
float shadow(PvFrame f, vec3 p, vec3 n) {
    if ((f.flags & PVF_SHADOW) == 0u) return 1.0;
    vec4 c = f.light_matrix * vec4(p + n * f.floor_params.w * 1.5, 1.0);
    vec2 uv = c.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || c.z > 1.0) return 1.0;
    vec2 t = 1.0 / vec2(textureSize(u_pv_shadow, 0));
    float s = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) s += texture(u_pv_shadow, vec3(uv + vec2(x, y) * t * 1.25, c.z - 0.001));
    return s / 9.0;
}
// (Q7) the AO view mode: points of the hemisphere over the surface (cosine-weighted, turned per pixel) projected into
// the depth pass; behind what the pass saw there, within the radius (30% of the subject's): occluded
float pv_ao(PvFrame f, vec3 p, vec3 n) {
    if (f.ao_depth == uvec2(0u)) return 1.0;
    PvDepth db = PvDepth(f.ao_depth);
    float r = max(f.target.w * 0.3, 2.0), pd = dot(p - f.eye.xyz, f.cam_fwd.xyz);
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec3 t = normalize(abs(n.z) < 0.9 ? cross(n, vec3(0.0, 0.0, 1.0)) : cross(n, vec3(1.0, 0.0, 0.0))), b = cross(n, t);
    float occ = 0.0, cnt = 0.0;
    for (int i = 0; i < 12; i++) {
        float fi = (float(i) + 0.5) / 12.0, ang = float(i) * 2.399963 + noise * 6.2831853;
        vec3 dir = (t * cos(ang) + b * sin(ang)) * sqrt(fi) + n * sqrt(1.0 - fi);
        vec3 q = p + n * (r * 0.015) + dir * (r * mix(0.08, 1.0, fract(fi * 7.31 + noise)));
        vec4 c = f.view_proj * vec4(q, 1.0);
        if (c.w <= 1e-3) continue;
        vec2 uv = c.xy / c.w * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) continue;
        uvec2 px = min(uvec2(uv * vec2(f.ao_w, f.ao_h)), uvec2(f.ao_w - 1u, f.ao_h - 1u));
        uint bits = db.d[px.y * f.ao_w + px.x];
        cnt += 1.0;
        if (bits == 0xFFFFFFFFu) continue;
        float scene = uintBitsToFloat(bits), qd = dot(q - f.eye.xyz, f.cam_fwd.xyz);
        if (scene < qd - r * 0.01) occ += smoothstep(0.0, 1.0, r / max(abs(pd - scene), 1e-3));
    }
    return cnt > 0.0 ? pow(clamp(1.0 - occ / cnt, 0.0, 1.0), 2.0) : 1.0;
}

// sRGB-encoded, the wireframe's or the lit color with the selection's glow
void main() {
    PvFrame f = PvFrame(pc.frame);
    Instance inst = InstanceBuf(pc.instances).i[i_instance];
    uint fl = inst.ids.y;
    if (f.mode == PV_OVERDRAW) { // (Q7) every layer adds its share: dark red, orange, yellow, white past ~30
        o_color = vec4(0.10, 0.035, 0.012, 1.0);
        return;
    }
    if (pc.pass == 3u) {
        vec3 w = (fl & PVI_SELECTED) != 0u ? SEL : f.mode == PV_WIREFRAME ? vec3(0.45, 0.72, 1.0) : vec3(0.06, 0.07, 0.09);
        o_color = vec4(w, 1.0);
        return;
    }
    vec3 n = normalize(i_normal);
    vec3 v = normalize(f.eye.xyz - i_world);
    if (dot(n, v) < 0.0) n = -n; // (open meshes seen from behind)
    if ((fl & PVI_FLOOR) != 0u) {
        // the floor: its shadow, the grid (1 and 5 cells, anti-aliased), faded out toward the background
        float sh = shadow(f, i_world, vec3(0.0, 0.0, 1.0));
        vec3 c = vec3(0.16) * (0.35 + 0.65 * mix(0.35, 1.0, sh));
        if (f.mode == PV_AO) c = vec3(0.55) * pv_ao(f, i_world, vec3(0.0, 0.0, 1.0));
        else if ((f.flags & PVF_SKY) != 0u) c *= vec3(1.25, 1.2, 1.1);
        if ((f.flags & PVF_GRID) != 0u) {
            vec2 g = i_world.xy / f.floor_params.z, fw = max(fwidth(g), vec2(1e-4));
            vec2 d1 = abs(fract(g - 0.5) - 0.5) / fw, d5 = abs(fract(g / 5.0 - 0.5) - 0.5) * 5.0 / fw;
            float l1 = 1.0 - clamp(min(d1.x, d1.y) - 0.5, 0.0, 1.0), l5 = 1.0 - clamp(min(d5.x, d5.y) - 0.5, 0.0, 1.0);
            l1 *= clamp(3.0 - max(fw.x, fw.y) * 6.0, 0.0, 1.0); // (the small cells fade before they alias)
            c = mix(c, vec3(0.42), max(l1 * 0.25, l5 * 0.55));
        }
        float r = length(i_world.xy - f.target.xy), fade = 1.0 - smoothstep(f.floor_params.y * 0.35, f.floor_params.y, r);
        vec3 bg = pv_background(f, gl_FragCoord.y * f.viewport.w);
        if ((f.flags & PVF_SKY) != 0u) { // (the horizon behind it: the sky along the view ray)
            vec3 d = normalize(i_world - f.eye.xyz);
            bg = pv_display(pv_sky(f, vec3(d.xy, max(d.z, 0.0))));
        }
        o_color = vec4(mix(bg, f.mode == PV_AO ? c : pv_display(c), fade), 1.0);
        return;
    }
    vec3 base = inst.base_metal.rgb, emissive = (fl & INST_MATERIAL) != 0u ? vec3(0.0) : inst.emissive_rough.rgb;
    float metal = inst.base_metal.w, rough = clamp(inst.emissive_rough.w, 0.045, 1.0);
    if ((fl & INST_MATERIAL) != 0u) { // (mesh.frag's: the baked textures, the tangent frame from the derivatives)
        vec3 dp1 = dFdx(i_world), dp2 = dFdy(i_world);
        vec2 duv1 = dFdx(i_uv), duv2 = dFdy(i_uv);
        GpuMaterial gm = GpuMaterialBuf(f.materials).m[floatBitsToUint(inst.emissive_rough.x)];
        vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
        vec3 T = dp2perp * duv1.x + dp1perp * duv2.x, B = dp2perp * duv1.y + dp1perp * duv2.y;
        float im = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
        mat3 tbn = mat3(T * im, B * im, n);
        vec4 tb = texture(u_mat_tex[nonuniformEXT(gm.tex_base)], i_uv), tn = texture(u_mat_tex[nonuniformEXT(gm.tex_nrm)], i_uv);
        if ((gm.flags & MATF_MASKED) != 0u && tb.a < float(gm.flags >> 24u) / 255.0) discard;
        base = srgb_to_linear(tb.rgb);
        vec3 nts = vec3(tn.xy * 2.0 - 1.0, 0.0);
        nts.z = sqrt(max(1.0 - dot(nts.xy, nts.xy), 0.0));
        rough = clamp(tn.z, 0.045, 1.0), metal = tn.w;
        emissive = (gm.flags & MATF_EMISSIVE) != 0u ? texture(u_mat_tex[nonuniformEXT(gm.tex_emis)], i_uv).rgb : vec3(0.0);
        if ((gm.flags & MATF_AFFINE) != 0u) mat_affine(f.materials, floatBitsToUint(inst.emissive_rough.x), base, metal, rough, emissive), rough = max(rough, 0.045); // (2026-11-24: the affine fold)
        if ((gm.flags & MATF_WORLD_GRID) != 0u) { // the engine's grid material (mesh.frag)
            vec3 an = abs(n);
            vec2 wp = an.z >= an.x && an.z >= an.y ? i_world.xy : an.x >= an.y ? i_world.yz : i_world.xz;
            vec2 fw = max(fwidth(wp), vec2(1e-3)), d1 = abs(fract(wp / 100.0 - 0.5) - 0.5) * 100.0 / fw;
            float l1 = 1.0 - clamp(min(d1.x, d1.y) - 0.5, 0.0, 1.0), sq = mod(floor(wp.x / 100.0) + floor(wp.y / 100.0), 2.0);
            base *= (sq > 0.5 ? 0.9 : 1.0) * mix(1.0, 0.45, l1);
        }
        n = normalize(tbn * nts);
        if (dot(n, v) < 0.0) n = normalize(n - v * dot(n, v) * 1.01);
    }
    uint mode = f.mode;
    vec3 c;
    if (mode == PV_LOD) { // (Q7) Unreal's LOD Coloration: white, red, green, blue on a lit gray
        uint lod = (fl >> PVI_LOD_SHIFT) & 15u;
        vec3 tint = lod == 0u ? vec3(1.0) : lod == 1u ? vec3(1.0, 0.22, 0.2) : lod == 2u ? vec3(0.25, 1.0, 0.3) : vec3(0.25, 0.45, 1.0);
        base = vec3(0.55) * tint, metal = 0.0, rough = 0.6, emissive = vec3(0.0);
    }
    if (mode == PV_UNLIT) c = pv_encode(clamp(base + emissive, 0.0, 1.0));
    else if (mode == PV_ALBEDO) c = pv_encode(clamp(base, 0.0, 1.0)); // (Q7) the base color alone
    else if (mode == PV_AO) c = vec3(pv_ao(f, i_world, n));
    else if (mode == PV_UV) { // (Q7) a checker of 8 x 8 per UV tile, tinted by U (red) and V (green)
        vec2 g = fract(i_uv * 8.0);
        float ch = (g.x < 0.5) != (g.y < 0.5) ? 1.0 : 0.55;
        vec2 u = fract(i_uv);
        c = vec3(0.25 + 0.75 * u.x, 0.25 + 0.75 * u.y, 0.35) * ch * (0.65 + 0.35 * max(dot(n, normalize(f.key.xyz)), 0.0));
    }
    else if (mode == PV_NORMALS) c = n * 0.5 + 0.5;
    else if (mode == PV_ROUGHNESS) c = vec3(rough);
    else if (mode == PV_METALLIC) c = vec3(metal);
    else {
        if (mode == PV_LIGHTING_ONLY) base = vec3(0.5), metal = 0.0, emissive = vec3(0.0);
        vec3 diff = base * (1.0 - metal), f0 = mix(vec3(0.04), base, metal);
        float a = rough * rough, nv = max(dot(n, v), 1e-4);
        vec3 irr = (f.flags & PVF_SKY) != 0u ? mix(vec3(0.17, 0.15, 0.12), vec3(0.38, 0.52, 0.78), n.z * 0.5 + 0.5) * 1.5
                                             : mix(GROUND, SKY, n.z * 0.5 + 0.5) * 1.3;
        uint ms = floatBitsToUint(inst.emissive_rough.x);
        vec3 lit;
        if ((fl & INST_MATERIAL) != 0u && (GpuMaterialBuf(f.materials).m[ms].flags & MATF_TOON) != 0u) {
            // (Q11) the Toon shading model: the key, fill and rim lights in its bands, its ambient share, its hatching
            GpuToonBuf tb = GpuToonBuf(f.materials);
            diff = base;
            float l0, l1, l2;
            vec3 k = KEY * f.key.w, fl3 = SKY * f.fill.w, r3 = RIM * f.rim.w;
            lit = toon_shade(tb, ms, n, v, f.key.xyz, diff, shadow(f, i_world, n), l0) * k;
            lit += toon_shade(tb, ms, n, v, f.fill.xyz, diff, 1.0, l1) * fl3 + toon_shade(tb, ms, n, v, f.rim.xyz, diff, 1.0, l2) * r3;
            vec3 Y = vec3(0.2126, 0.7152, 0.0722);
            float wk = dot(k, Y), wf = dot(fl3, Y), wr = dot(r3, Y);
            lit += diff * irr * tb.t[ms].t6.y + emissive;
            lit *= toon_hatch(tb, ms, gl_FragCoord.xy, 1.0, (l0 * wk + l1 * wf + l2 * wr) / max(wk + wf + wr, 1e-4));
        } else {
            lit = brdf(n, v, f.key.xyz, diff, f0, a) * KEY * f.key.w * shadow(f, i_world, n);
            lit += brdf(n, v, f.fill.xyz, diff, f0, a) * SKY * f.fill.w + brdf(n, v, f.rim.xyz, diff, f0, a) * RIM * f.rim.w;
            vec2 ab = env_brdf(nv, rough);
            lit += diff * irr + studio(f, reflect(-v, n), rough) * (f0 * ab.x + ab.y) + emissive;
        }
        c = pv_display(lit);
    }
    if ((fl & PVI_SELECTED) != 0u) c = mix(c, SEL, 0.18 + 0.5 * pow(1.0 - max(dot(n, v), 0.0), 2.0)); // (Unreal's selection)
    o_color = vec4(c, 1.0);
}
