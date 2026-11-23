// (Q21) The impostors' atlases read (farview.c bakes them; mesh.frag, masked.frag): needs mat_common.glsl (u_mat_tex) and
// common.glsl. A mesh seen from 8 x 8 directions of the upper hemisphere (hemi-octahedral): one atlas holds its base color
// (sRGB, as the materials' baked base) and alpha, the other its normal (mesh space, octahedral), its depth along the view
// and its roughness; both premultiplied by the coverage (cleared to 0: the mips and the bilinear reads average only what
// is there, divided back here: no dark fringe around the leaves)
#ifndef IMPOSTOR_GLSL
#define IMPOSTOR_GLSL
#include "farview.glsl"

vec2 imp_hemi_enc(vec3 d) {
    d.z = max(d.z, 0.0);
    d /= abs(d.x) + abs(d.y) + d.z + 1e-8;
    return vec2(d.x + d.y, d.x - d.y);
}
vec3 imp_hemi_dec(vec2 e) {
    vec2 t = vec2(e.x + e.y, e.x - e.y) * 0.5;
    return normalize(vec3(t, max(1.0 - abs(t.x) - abs(t.y), 0.0)));
}
// a frame's direction (toward its camera) and its screen axes (impostor_bake.vert uses the same)
void imp_frame(uvec2 k, uint n, out vec3 d, out vec3 r, out vec3 u) {
    d = imp_hemi_dec(vec2(k) / float(n - 1u) * 2.0 - 1.0);
    vec3 c = cross(vec3(0.0, 0.0, 1.0), d);
    r = dot(c, c) < 1e-8 ? vec3(1.0, 0.0, 0.0) : normalize(c);
    u = cross(d, r);
}

// What the impostor shows along a view ray: q the ray's point on the card and v its direction, both in the mesh's unit
// space (its bounding sphere's center at 0, radius 1). The three frames around the view (a triangle of the grid, weighted
// by where the view falls in it), each read where the ray meets its plane, then one step of parallax by the depth there.
// Out: base color (sRGB), coverage, normal (mesh space), roughness, the point shown (unit space)
struct ImpSample { vec3 base; float alpha; vec3 n; float rough; vec3 p; };

ImpSample imp_sample(uvec4 e, vec3 q, vec3 v, float lod, bool full) {
    uint n = max(e.z & 255u, 2u);
    uint ta = e.y & 0xFFFFu, tn = e.y >> 16;
    vec2 gp = clamp(imp_hemi_enc(-v) * 0.5 + 0.5, 0.0, 1.0) * float(n - 1u);
    uvec2 cell = uvec2(min(floor(gp), vec2(float(n - 2u))));
    vec2 fr = gp - vec2(cell);
    uvec2 k0, k1, k2;
    vec3 w;
    if (fr.x + fr.y < 1.0) k0 = cell, k1 = cell + uvec2(1u, 0u), k2 = cell + uvec2(0u, 1u), w = vec3(1.0 - fr.x - fr.y, fr.x, fr.y);
    else k0 = cell + uvec2(1u), k1 = cell + uvec2(0u, 1u), k2 = cell + uvec2(1u, 0u), w = vec3(fr.x + fr.y - 1.0, 1.0 - fr.x, 1.0 - fr.y);
    ImpSample s;
    s.base = vec3(0.0), s.alpha = 0.0, s.n = vec3(0.0), s.rough = 0.0, s.p = vec3(0.0);
    float fs = 1.0 / float(n);
    float inset = exp2(lod) * 0.5 / float(textureSize(u_mat_tex[nonuniformEXT(ta)], 0).x) * float(n); // (half a texel of that mip, in frame units)
    float wsum = 0.0;
    for (int i = 0; i < 3; i++) {
        uvec2 k = i == 0 ? k0 : i == 1 ? k1 : k2;
        float wi = i == 0 ? w.x : i == 1 ? w.y : w.z;
        if (wi <= 0.0) continue;
        vec3 dk, r, u;
        imp_frame(k, n, dk, r, u);
        float dv = min(dot(v, dk), -0.05);
        vec2 o = vec2(k) * fs;
        vec3 p0 = q + v * (-dot(q, dk) / dv);
        vec2 uv0 = clamp(vec2(0.5 + 0.5 * dot(p0, r), 0.5 - 0.5 * dot(p0, u)), vec2(inset), vec2(1.0 - inset));
        vec4 a0 = textureLod(u_mat_tex[nonuniformEXT(ta)], o + uv0 * fs, lod);
        if (!full) { // (the prepass's coverage: the planes' alone, 3 reads; the parallax moves the inside, barely the edge)
            s.alpha += wi * a0.a;
            continue;
        }
        vec4 b0 = textureLod(u_mat_tex[nonuniformEXT(tn)], o + uv0 * fs, lod);
        float h = a0.a > 0.02 ? b0.z / a0.a * 2.0 - 1.0 : 0.0;
        vec3 p = q + v * ((h - dot(q, dk)) / dv);
        vec2 uv1 = clamp(vec2(0.5 + 0.5 * dot(p, r), 0.5 - 0.5 * dot(p, u)), vec2(inset), vec2(1.0 - inset));
        vec4 a = textureLod(u_mat_tex[nonuniformEXT(ta)], o + uv1 * fs, lod);
        float wa = wi * a.a;
        s.alpha += wa;
        if (a.a > 0.02) {
            vec4 b = textureLod(u_mat_tex[nonuniformEXT(tn)], o + uv1 * fs, lod) / a.a;
            s.base += a.rgb * wi;
            s.n += oct_decode_f(b.xy * 2.0 - 1.0) * wa;
            s.rough += b.w * wa;
            s.p += (q + v * ((b.z * 2.0 - 1.0 - dot(q, dk)) / dv)) * wa;
            wsum += wa;
        }
    }
    if (full) {
        if (wsum > 1e-5) {
            s.base /= wsum, s.rough /= wsum, s.p /= wsum;
            s.n = normalize(s.n + vec3(0.0, 0.0, 1e-5));
        } else
            s.base = vec3(0.0), s.n = vec3(0.0, 0.0, 1.0), s.rough = 1.0, s.p = q;
    }
    return s;
}

// A card's fragment (mesh.vert: pw on the card, corner -1..1 across it; the caller's flow is uniform per triangle, the
// derivatives defined): the impostor of the instance's mesh there, and the world point it shows
ImpSample imp_card(FrameBuf f, Instance inst, vec3 pw, vec2 corner, bool full, out vec3 wp) {
    uint shape = inst.ids.y >> INST_SHAPE_SHIFT;
    uvec4 te = ImpTabBuf(f.imp_tab).e[shape];
    MeshInfo cm = MeshInfoBuf(f.mesh_info).m[shape];
    mat3 M = transpose(mat3(inst.row0.xyz, inst.row1.xyz, inst.row2.xyz)); // (object -> world: the rows' 3 x 3)
    mat3 Mi = inverse(M);
    vec3 org = vec3(inst.row0.w, inst.row1.w, inst.row2.w);
    vec3 q = (Mi * (pw - org) - cm.sphere.xyz) / cm.sphere.w;
    vec3 rd = view_ortho(f) ? f.camera_fwd.xyz : pw - f.camera_pos.xyz;
    vec3 vl = normalize(Mi * rd);
    uint n = max(te.z & 255u, 2u);
    float fp = float(textureSize(u_mat_tex[nonuniformEXT(te.y & 0xFFFFu)], 0).x) / float(n); // (a frame's texels a side)
    vec3 dx = dFdx(q), dy = dFdy(q); // (a frame spans the unit sphere's 2 across its fp texels)
    float lod = max(log2(max(max(length(dx), length(dy)) * 0.5 * fp, 1e-4)) + tsr_mip_bias(f.tsr_size), 0.0);
    ImpSample s = imp_sample(te, q, vl, lod, full);
    wp = org + M * (cm.sphere.xyz + s.p * cm.sphere.w);
    return s;
}

#endif
