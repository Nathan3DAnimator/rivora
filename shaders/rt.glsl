// Ray queries against the scene's TLAS (render.c builds it every frame from every instance): shadow rays, the closest
// hit with its normal and material, the direct light at a point, and the GI probes' irradiance. Included after
// common.glsl by the shaders that trace (path tracer, probes, lightmap bake, reflections in mesh.frag).
#ifndef RE_SWRT
#extension GL_EXT_ray_query : require
#endif
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#include "lights.glsl"
#include "weather.glsl"

#define RT_MASK_VISIBLE 0x01u
#define RT_MASK_SHADOW 0x02u
#define RT_MASK_STATIC 0x04u // Static meshes only (the lightmap bake: movable actors stay out of baked light)
// what isn't moving right now (render.c: every static instance, a Movable one standing still): the surface cache's and
// the probes' rays (RT_STILL_ONLY) see only these: an actor walking past would sit in their history a few frames late
#define RT_MASK_STILL 0x08u
#define RT_MASK_STILL_SHADOW 0x10u
#define RT_MASK_GLASS 0x20u // (2026-11-17, Q4) the Translucent instances: only mesh.frag glass_tint's rays see them
// the mask a query really uses: under RT_STILL_ONLY the visible and shadow masks become their still ones
uint rt_mask(uint mask) {
#ifdef RT_STILL_ONLY
    if ((mask & RT_MASK_SHADOW) != 0u) return RT_MASK_STILL_SHADOW;
    if ((mask & RT_MASK_VISIBLE) != 0u) return RT_MASK_STILL;
#endif
    return mask;
}

// Two TLAS (render.c, notas-dev/escala.md): the static scene (rebuilt only when edited) and the dynamic set (the
// Movable actors, rebuilt every frame, f.tlas_dyn 0 when empty). A query traces the static one, and the dynamic
// one when the ray crosses its bounds (hits_dyn); the nearest hit wins.
#ifdef RE_SWRT
#include "sw_rt.glsl" // (A1: no ray query on this GPU; f.tlas and f.tlas_dyn hold the software sets' headers)
#else
accelerationStructureEXT scene_tlas(FrameBuf f) { return accelerationStructureEXT(packUint2x32(f.tlas)); }
accelerationStructureEXT dyn_tlas(FrameBuf f) { return accelerationStructureEXT(packUint2x32(f.tlas_dyn)); }
#endif
bool has_dyn(FrameBuf f) { return (f.tlas_dyn.x | f.tlas_dyn.y) != 0u; }
// Whether [o, o + d * tmax] crosses the dynamic set's bounds (a slab test, 1 cm of margin): most rays don't, and skip its
// query (each one costs a traversal from the root)
bool hits_dyn(FrameBuf f, vec3 o, vec3 d, float tmax) {
    if (!has_dyn(f)) return false;
    vec3 inv = 1.0 / vec3(d.x != 0.0 ? d.x : 1e-9, d.y != 0.0 ? d.y : 1e-9, d.z != 0.0 ? d.z : 1e-9);
    vec3 t0 = (f.dyn_box[0].xyz - 1.0 - o) * inv, t1 = (f.dyn_box[1].xyz + 1.0 - o) * inv;
    vec3 lo = min(t0, t1), hi = max(t0, t1);
    return max(max(lo.x, lo.y), max(lo.z, 0.0)) <= min(min(hi.x, hi.y), min(hi.z, tmax));
}

// Whether a ray is valid for a query: the spec wants a finite origin and direction and tmin (0.5) <= tmax; anything else is
// undefined (a shadow ray from a point under 2.5 cm from its light)
bool ray_ok(vec3 o, vec3 d, float tmax) {
    return tmax > 0.5 && !any(isnan(o)) && !any(isinf(o)) && !any(isnan(d)) && !any(isinf(d)) && dot(d, d) > 0.0;
}

#if defined(RT_ALPHA) && defined(MAT_COMMON_GLSL) && !defined(RE_SWRT)
// (RT_ALPHA: mesh.frag's own variants, render.c picks them once a Masked mesh is drawn: the second ray query took
// registers from every traced pixel even when it never ran)
// A Masked material's triangle the ray reached (a candidate: render.c / tlas_fill.comp mark the instances of the meshes
// a Masked or Two Sided material draws not opaque): whether its baked alpha there is at the clip value or over. Hair
// cards cast their quads' whole shadow without it: blocks of light and dark on the hair that jumped as it moved
bool rt_alpha_blocks(FrameBuf f, rayQueryEXT rq) {
    Instance inst = InstanceBuf(f.instances).i[uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false))];
    if ((inst.ids.y & INST_MATERIAL) == 0u) return true;
    GpuMaterial gm = GpuMaterialBuf(f.materials).m[floatBitsToUint(inst.emissive_rough.x)];
    if ((gm.flags & MATF_MASKED) == 0u) return true; // (Two Sided only)
    IndexBuf ib = IndexBuf(f.indices);
    VertexBuf vb = VertexBuf(pc.vertices);
    // (a cluster BLAS, A8: each triangle's geometry index is its place in the mesh + 1; a classic one has 0)
    uint cg = uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false));
    uint base = inst.ids.z + (cg != 0u ? cg - 1u : uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false))) * 3u;
    int vo = int(inst.ids.w);
    vec2 bc = rayQueryGetIntersectionBarycentricsEXT(rq, false);
    vec2 uv = (vb.v[ib.i[base] + vo].uv * (1.0 - bc.x - bc.y) + vb.v[ib.i[base + 1u] + vo].uv * bc.x + vb.v[ib.i[base + 2u] + vo].uv * bc.y) *
              inst.emissive_rough.y; // (the tiling, as mesh.vert)
    // at most 512 texels across (a ray has no footprint: mip 0 of a 4096 texture missed the cache candidate after candidate;
    // the mips keep the cut's coverage, material.c alpha_coverage_mips)
    uint tex = (gm.flags & MATF_UDIM) != 0u ? udim_tile(f.mat_params, gm, uv).x : gm.tex_base; // (Q18: the tile under the uv)
    float lod = max(log2(float(textureSize(u_mat_tex[nonuniformEXT(tex)], 0).x)) - 9.0, 0.0);
    return textureLod(u_mat_tex[nonuniformEXT(tex)], uv, lod).a >= float(gm.flags >> 24u) / 255.0;
}
// visible_in with a Masked mesh drawn (and the materials bound: mesh.frag): the ray isn't opaque, the opaque instances
// commit by themselves, a Masked mesh's triangles come as candidates and its alpha decides
bool visible_in_alpha(FrameBuf f, bool dyn, vec3 o, vec3 d, float tmax, uint mask) {
    rayQueryEXT rq;
    uint flags = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT;
    if (dyn) rayQueryInitializeEXT(rq, dyn_tlas(f), flags, rt_mask(mask), o, 0.5, d, tmax);
    else rayQueryInitializeEXT(rq, scene_tlas(f), flags, rt_mask(mask), o, 0.5, d, tmax);
    while (rayQueryProceedEXT(rq))
        if (rt_alpha_blocks(f, rq)) rayQueryConfirmIntersectionEXT(rq);
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT;
}
#endif

bool visible_in(FrameBuf f, bool dyn, vec3 o, vec3 d, float tmax, uint mask) {
    if (!ray_ok(o, d, tmax)) return true; // (too short to be blocked)
#ifdef RE_SWRT
    {
        float t_;
        uint ii_, prim_;
        vec2 bc_;
        return !sw_trace(f, dyn ? f.tlas_dyn : f.tlas, o, d, tmax, rt_mask(mask), true, t_, ii_, prim_, bc_);
    }
#endif
#if defined(RT_ALPHA) && defined(MAT_COMMON_GLSL) && !defined(RE_SWRT)
    if ((f.gi_flags & GI_RT_MASKED) != 0u) return visible_in_alpha(f, dyn, o, d, tmax, mask);
#endif
#ifndef RE_SWRT
    rayQueryEXT rq;
    uint flags = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT | gl_RayFlagsSkipClosestHitShaderEXT;
    if (dyn) rayQueryInitializeEXT(rq, dyn_tlas(f), flags, rt_mask(mask), o, 0.5, d, tmax);
    else rayQueryInitializeEXT(rq, scene_tlas(f), flags, rt_mask(mask), o, 0.5, d, tmax);
    while (rayQueryProceedEXT(rq)) {}
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT;
#endif
}

// true when nothing blocks [o, o + d * tmax]
bool trace_visible_mask(FrameBuf f, vec3 o, vec3 d, float tmax, uint mask) {
    return visible_in(f, false, o, d, tmax, mask) && (!hits_dyn(f, o, d, tmax) || visible_in(f, true, o, d, tmax, mask));
}
bool trace_visible(FrameBuf f, vec3 o, vec3 d, float tmax) { return trace_visible_mask(f, o, d, tmax, RT_MASK_SHADOW); }

struct Hit {
    bool hit;
    float t;
    vec3 pos, normal;  // the normal faces the ray
    vec3 base, emissive;
    vec3 glow;        // the emission the diffuse GI adds (0 for an actor whose glow lights as a point light: common.glsl inst_glow)
    float metal, rough;
    vec2 uv2;
    vec2 uv;          // the mesh's UV (without the material's tiling)
    float uv_density; // its UV units per cm there (the triangle's: a ray cone's texture footprint)
    uint inst;
    bool front;
};

// The nearest triangle in one TLAS before tmax: its instance (the slot), primitive, barycentrics; false: none
bool closest_in(FrameBuf f, bool dyn, vec3 o, vec3 d, float tmax, uint mask, out float t, out uint ii, out uint prim,
                out vec2 bc, out bool front) {
    if (!ray_ok(o, d, tmax)) return false;
#ifdef RE_SWRT
    front = true;
    return sw_trace(f, dyn ? f.tlas_dyn : f.tlas, o, d, tmax, rt_mask(mask), false, t, ii, prim, bc);
#else
    rayQueryEXT rq;
    if (dyn) rayQueryInitializeEXT(rq, dyn_tlas(f), gl_RayFlagsOpaqueEXT, rt_mask(mask), o, 0.5, d, tmax);
    else rayQueryInitializeEXT(rq, scene_tlas(f), gl_RayFlagsOpaqueEXT, rt_mask(mask), o, 0.5, d, tmax);
    while (rayQueryProceedEXT(rq)) {}
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) return false;
    t = rayQueryGetIntersectionTEXT(rq, true);
    ii = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
    uint cg = uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true)); // (a cluster BLAS, A8: the triangle's place + 1)
    prim = cg != 0u ? cg - 1u : uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
    bc = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    front = rayQueryGetIntersectionFrontFaceEXT(rq, true);
#endif
    return true;
}

Hit trace_closest_mask(FrameBuf f, vec3 o, vec3 d, float tmax, uint mask) {
    Hit h;
    h.hit = false;
    float t;
    uint ii, prim;
    vec2 bc;
    bool front;
    bool hit = closest_in(f, false, o, d, tmax, mask, t, ii, prim, bc, front);
    if (hits_dyn(f, o, d, hit ? t : tmax)) { // the dynamic set, only before the static hit
        float t2;
        uint ii2, prim2;
        vec2 bc2;
        bool front2;
        if (closest_in(f, true, o, d, hit ? t : tmax, mask, t2, ii2, prim2, bc2, front2)) {
            hit = true;
            t = t2; ii = ii2; prim = prim2; bc = bc2; front = front2;
        }
    }
    if (!hit) return h;
    h.hit = true;
    h.t = t;
    h.pos = o + d * h.t;
    h.front = front;
    Instance inst = InstanceBuf(f.instances).i[ii];
    IndexBuf ib = IndexBuf(f.indices);
    VertexBuf vb = VertexBuf(pc.vertices);
    uint base = inst.ids.z + prim * 3u;
    int vo = int(inst.ids.w);
    Vertex v0 = vb.v[ib.i[base] + vo], v1 = vb.v[ib.i[base + 1u] + vo], v2 = vb.v[ib.i[base + 2u] + vo];
    float w0 = 1.0 - bc.x - bc.y;
    vec3 n = oct_decode(v0.normal_oct) * w0 + oct_decode(v1.normal_oct) * bc.x + oct_decode(v2.normal_oct) * bc.y;
    h.uv2 = v0.uv2 * w0 + v1.uv2 * bc.x + v2.uv2 * bc.y;
    h.uv = v0.uv * w0 + v1.uv * bc.x + v2.uv * bc.y;
    vec3 a = vec3(inst.row0.x, inst.row1.x, inst.row2.x);
    vec3 b = vec3(inst.row0.y, inst.row1.y, inst.row2.y);
    vec3 c = vec3(inst.row0.z, inst.row1.z, inst.row2.z);
    n = normalize(cross(b, c) * n.x + cross(c, a) * n.y + cross(a, b) * n.z);
    { // (the UV area over the world area of the triangle; the compiler drops it where uv_density isn't read)
        vec3 e1 = v1.pos - v0.pos, e2 = v2.pos - v0.pos;
        vec3 w1 = a * e1.x + b * e1.y + c * e1.z, w2 = a * e2.x + b * e2.y + c * e2.z;
        vec2 t1 = v1.uv - v0.uv, t2 = v2.uv - v0.uv;
        h.uv_density = sqrt(abs(t1.x * t2.y - t1.y * t2.x) / max(length(cross(w1, w2)), 1e-6));
    }
    h.normal = dot(n, d) > 0.0 ? -n : n;
    h.front = dot(n, d) <= 0.0; // by the vertex normals (outward on closed meshes), not the winding: a probe inside an object sees backs
    h.base = inst.base_metal.rgb;
    h.metal = inst.base_metal.w;
    h.emissive = inst_emission(inst.ids.y, inst.emissive_rough); // (2026-11-23: a material's mean emission, before 0: its glow lit nothing)
    h.glow = inst_glow(inst.ids.y, inst.emissive_rough);
    h.rough = inst.emissive_rough.w;
    h.inst = ii;
    return h;
}
Hit trace_closest(FrameBuf f, vec3 o, vec3 d, float tmax) { return trace_closest_mask(f, o, d, tmax, RT_MASK_VISIBLE); }

// (2026-11-23) an emissive actor's light (lights.c emissive_light: a point light, half_h the minus of its sphere's
// radius): the shadow rays end that far before its center (they'd hit its own mesh). Any other light: 0
float light_skip(GpuLight L) { return light_kind(L) == LIGHT_POINT && L.half_h < 0.0 ? -L.half_h : 0.0; }

// The same falloff as mesh.frag
float rt_falloff(float d, GpuLight L) {
    float w = clamp(1.0 - pow(d * d * L.inv_range2, 2.0), 0.0, 1.0);
    float dm = max(d * 0.01, max(L.source_radius * 0.01, 0.01));
    return w * w / (dm * dm);
}

// One light's irradiance at p (normal n), Lambert (cos included), with a shadow ray from o if it casts shadows and the
// budget (shadow-casting lights traced at this point) isn't spent; past it, the light is unshadowed
vec3 light_irradiance(FrameBuf f, GpuLight L, vec3 p, vec3 n, vec3 o, uint mask, inout uint budget) {
    vec3 l;
    float atten, dist;
    if (light_kind(L) == LIGHT_DIRECTIONAL) {
        l = -L.direction;
        atten = 1.0;
        dist = 1e7;
        atten = cloud_shadow(f, p); // the clouds' shadow (weather.glsl)
    } else {
        vec3 lp = L.position;
        if (light_kind(L) == LIGHT_RECT) {
            vec3 up = cross(L.direction, L.right);
            vec3 rel = p - L.position;
            lp += L.right * clamp(dot(rel, L.right), -L.half_w, L.half_w) + up * clamp(dot(rel, up), -L.half_h, L.half_h);
        }
        vec3 lv = lp - p;
        dist = length(lv);
        l = lv / max(dist, 1e-3);
        atten = rt_falloff(dist, L);
        if (light_kind(L) == LIGHT_SPOT) {
            float s = clamp((dot(-l, L.direction) - L.cos_outer) / max(L.cos_inner - L.cos_outer, 1e-4), 0.0, 1.0);
            atten *= s * s;
        } else if (light_kind(L) == LIGHT_RECT) {
            atten *= max(dot(-l, L.direction), 0.0);
        }
        dist -= 2.0 + light_skip(L);
    }
    float nl = dot(n, l);
    if (nl <= 0.0 || atten <= 0.0) return vec3(0.0);
#ifdef LIGHTFN_GLSL
    L.radiance *= light_fn_of(f, L, p); // (2026-11-17, Q4: the Light Function in the GI too)
    if (dot(L.radiance, vec3(1.0)) <= 0.0) return vec3(0.0);
#endif
    if (light_casts(L) && budget > 0u) {
        budget--;
        if (!trace_visible_mask(f, o, l, dist, mask)) return vec3(0.0);
    }
    // the light's Indirect Lighting Intensity (Unreal): everything here lights the GI, but the path tracer's reference
#ifdef RT_PHYSICAL_LIGHTS
    return L.radiance * (atten * nl);
#else
    return L.radiance * (atten * nl * light_indirect_scale(L));
#endif
}

// The light reaching p (normal n) from every light (the directional ones, then the local ones of p's grid cell),
// shadowed by rays for up to budget shadow-casting lights: the irradiance, Lambert (cos included)
vec3 direct_irradiance_budget(FrameBuf f, vec3 p, vec3 n, uint mask, uint budget) {
    vec3 e = vec3(0.0);
    vec3 o = p + n * 1.0;
    for (uint i = 0u; i < f.light_count; i++) e += light_irradiance(f, f.lights[i], p, n, o, mask, budget);
    LightLists ll = light_lists(f, p);
    for (uint L = ll.lp; L < LG_LEVELS; L++) {
        uvec2 rg = light_range(ll, L);
        for (uint k = 0u; k < rg.y; k++) {
            uint it = light_item(f, rg, k);
            if (light_take(it, L, ll.lp)) e += light_irradiance(f, local_light(f, it), p, n, o, mask, budget);
        }
    }
    return e;
}
vec3 direct_irradiance_mask(FrameBuf f, vec3 p, vec3 n, uint mask) { return direct_irradiance_budget(f, p, n, mask, f.shadow_budget); }
vec3 direct_irradiance(FrameBuf f, vec3 p, vec3 n) { return direct_irradiance_mask(f, p, n, RT_MASK_SHADOW); }


// Interleaved gradient noise (Jimenez 2014): turns the few shadow rays of an area light per pixel
float ign(vec2 px) { return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))); }

// How much of a light's source o sees (l, dist: toward the point the light is computed from). A point light or the sun:
// one exact ray (hard shadows, no noise). A sphere (Source Radius) or a rectangle: 'rays' rays over its disk / its
// area on a Vogel spiral turned by rot, for the penumbra that widens away from the blocker.
bool light_is_area(GpuLight L) { // (a sphere of Source Radius or a rectangle: soft shadows, several rays)
    uint kind = light_kind(L);
    return kind == LIGHT_RECT || (kind != LIGHT_DIRECTIONAL && L.source_radius >= 0.5);
}
float light_visibility(FrameBuf f, vec3 o, GpuLight L, vec3 l, float dist, uint rays, float rot, uint mask) {
    uint kind = light_kind(L);
    bool area = light_is_area(L);
    float skip = light_skip(L); // (an emissive actor's light: the rays stop at its sphere, its own mesh inside)
    if (!area || rays <= 1u) return trace_visible_mask(f, o, l, dist - skip, mask) ? 1.0 : 0.0;
    vec3 u, v;
    float ru, rv;
    if (kind == LIGHT_RECT) {
        u = L.right;
        v = cross(L.direction, L.right);
        ru = L.half_w;
        rv = L.half_h;
    } else { // the sphere's disk facing o
        vec3 w = normalize(L.position - o);
        u = normalize(abs(w.z) < 0.999 ? cross(vec3(0, 0, 1), w) : vec3(1, 0, 0));
        v = cross(w, u);
        ru = rv = L.source_radius;
    }
    // each pixel's spiral turned (rot) and its rings shifted (jr, from rot by the golden ratio): with only the turn,
    // the few rays' levels lined up into a visible grid in a wide soft shadow; shifted too, it reads as a fine grain.
    // (more rays only in the penumbra cost 11 ms on a character seen close: measured, dropped). Only on Epic (8 rays):
    // the shifted rings scatter the rays (less coherent: -5% fps in the house walk at 4 rays); below, the 0.14 rings
    float vis = 0.0, jr = rays >= 8u ? fract(rot * 0.2575181) : 0.5;
    for (uint i = 0u; i < rays; i++) {
        float r = sqrt((float(i) + jr) / float(rays)), a = float(i) * 2.3999632 + rot;
        vec3 tv = L.position + u * (cos(a) * r * ru) + v * (sin(a) * r * rv) - o;
        float td = length(tv);
        if (trace_visible_mask(f, o, tv / td, td - 1.0 - skip, mask)) vis += 1.0;
    }
    return vis / float(rays);
}
