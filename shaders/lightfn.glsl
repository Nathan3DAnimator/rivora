// (2026-11-17, Q4) Unreal's Light Function, shared: mesh.frag (the surfaces), weather.comp (the volumetric fog), rt.glsl's
// light_irradiance (the GI: the surface cache, the probes). Needs mat_common.glsl (the material set bound: MAT_SET).
#ifndef LIGHTFN_GLSL
#define LIGHTFN_GLSL
// (2026-11-17, Q4) Unreal's Light Function: the light times its material's baked Emission (else Base Color), read where
// the point is in the light's own map: a spot's cone (its slide, the far plane of the cone is the texture), a point
// light's directions (octahedral), a directional light's world tiled every L.half_h cm across its direction. A material
// with a Time node plays its loop (its frames blended, as on a surface). lights.c puts its slot + 1 in L.half_w
vec3 light_fn(FrameBuf f, GpuLight L, vec3 p) {
    GpuMaterial gm = GpuMaterialBuf(f.materials).m[uint(L.half_w + 0.5) - 1u];
    vec3 w = p - L.position, d = L.direction, r = L.right, u = cross(d, r);
    uint k = light_kind(L);
    vec2 uv;
    if (k == LIGHT_DIRECTIONAL) uv = vec2(dot(p, r), dot(p, u)) / max(L.half_h, 1.0);
    else if (k == LIGHT_SPOT) {
        float z = max(dot(w, d), 1e-3), ta = sqrt(max(1.0 - L.cos_outer * L.cos_outer, 0.0)) / max(L.cos_outer, 1e-3);
        uv = vec2(dot(w, r), dot(w, u)) / (z * ta) * 0.5 + 0.5;
    } else {
        vec3 o = vec3(dot(w, r), dot(w, u), dot(w, d));
        o /= max(abs(o.x) + abs(o.y) + abs(o.z), 1e-6);
        uv = (o.z >= 0.0 ? o.xy : (1.0 - abs(o.yx)) * vec2(o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0)) * 0.5 + 0.5;
    }
    bool em = (gm.flags & MATF_EMISSIVE) != 0u;
    if (gm.anim.x > 1u) {
        float ft = fract(f.camera_pos.w / max(uintBitsToFloat(gm.anim.y), 0.05)) * float(gm.anim.x);
        uint f0 = uint(ft) % gm.anim.x, f1 = (f0 + 1u) % gm.anim.x;
        uvec4 t0 = MatFrameBuf(f.mat_params).t[gm.anim.z + f0], t1 = MatFrameBuf(f.mat_params).t[gm.anim.z + f1];
        return mix(textureLod(u_mat_tex[nonuniformEXT(em ? t0.z : t0.x)], uv, 0.0).rgb, textureLod(u_mat_tex[nonuniformEXT(em ? t1.z : t1.x)], uv, 0.0).rgb, fract(ft));
    }
    return textureLod(u_mat_tex[nonuniformEXT(em ? gm.tex_emis : gm.tex_base)], uv, 0.0).rgb;
}
// a light's Light Function at p (lights.c: its material slot + 1 in half_w, never on a rect light): 1 without one
vec3 light_fn_of(FrameBuf f, GpuLight L, vec3 p) { return light_kind(L) != LIGHT_RECT && L.half_w >= 1.0 ? light_fn(f, L, p) : vec3(1.0); }
#endif
