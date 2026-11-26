#version 460
// Every mesh pass (main, shadow, selection mask): vertices and instances pulled from buffers by device address.
#include "common.glsl"
#define LSM_NO_SAMPLER
#include "lsm.glsl" // the local lights' shadow maps' faces (pass 1 with pc.pad's bit 31)
#extension GL_EXT_nonuniform_qualifier : require
#define MAT_SET 1
#include "mat_common.glsl"
#include "tess.glsl" // (Q17) Nanite Tessellation: the displaced base vertices (Q22: the micro vertices in tess.vert)
#include "farview.glsl" // (Q21) FAR_IMP: the impostors' card

// the prepass and the main pass compute the same depth (the main pass tests EQUAL-or-nearer against it)
invariant gl_Position;

layout(location = 0) out vec3 o_world;
layout(location = 1) out vec3 o_normal;
layout(location = 2) flat out uint o_instance;
layout(location = 3) out vec4 o_uvs; // xy: UV2 (the lightmap, the cache), zw: the materials' UV (times the tiling)
// how far its rays start off the surface (cm): the LOD drawn against LOD 0. (Q17) Negative: a displaced surface (its
// magnitude in it): mesh.frag lifts its shadow maps' lookup that far too (no varying of its own: +0.08 ms on the rocks)
layout(location = 4) flat out float o_ray_bias;
float o_disp_bias; // (Q17: folded into o_ray_bias's sign at the end)
#ifdef RE_EXT
// (mesh_x.vert: the RE_EXT pipelines' only) the vertex color, linear (the material's Color Attribute: M5); white without
layout(location = 5) out vec4 o_vcol;
#endif

// (Q17) a displaced material's base vertex (tess.glsl): its canonical vertex's height along its normal; else as it is
vec3 q17_pos(FrameBuf f, Instance inst, MeshInfo mi, Vertex v) {
    if ((f.tess_u.x & TESS_ON) == 0u || (inst.ids.y & INST_SKINNED) != 0u || !tess_material(f, inst)) return v.pos;
    TessCtx c = tess_ctx(f, inst, mi, uint(mi.vertex_offset.x));
    return tess_vertex(f, c, uint(gl_VertexIndex - mi.vertex_offset.x), v.pos);
}
// how far its surface can sink under the base one (cm in the world): the rays' and the shadow maps' lift
float q17_bias(FrameBuf f, Instance inst, MeshInfo mi) {
    if ((f.tess_u.x & TESS_ON) == 0u || !tess_material(f, inst)) return 0.0;
    TessCtx c = tess_ctx(f, inst, mi, 0u);
    return c.on ? abs(c.mag) * max(c.center, 0.0) * c.s + 0.5 : 0.0;
}

void main() {
    FrameBuf f = FrameBuf(pc.frame);
#ifdef RE_EXT
    o_vcol = vec4(1.0);
#endif
    o_disp_bias = 0.0;
    // the slot: from the cull's visible lists (0 early, 1 shadow, 3 late: indirect draws), or the instance itself (the selection
    // mask draws the selected slot directly)
    uint slot = pc.pass == 0u ? UintBuf(f.visible_main).v[gl_InstanceIndex]
              : pc.pass == 1u ? UintBuf(f.visible_shadow).v[gl_InstanceIndex]
              : pc.pass == 3u ? UintBuf(f.visible_late).v[gl_InstanceIndex]
              : pc.pass == 6u ? UintBuf(addr_add(f.cl_work, 32u + 20u * f.cl_entries)).v[gl_InstanceIndex * 4u] // (the clusters: a draw per instance)
              : (pc.pass & 0x80000000u) != 0u ? pc.pad + uint(gl_InstanceIndex) * (pc.pass & 0x1FFu) // (a selected crowd's part in the mask: its agents nparts apart)
              : uint(gl_InstanceIndex);
    Instance inst = InstanceBuf(pc.instances).i[slot];
    if (pc.pass == 1u && (pc.pad & 0x80000000u) != 0u) {
        // A local light's face (lsm.c): job pc.pad & 0xFFFF of the frame's table, its casters (mode 1: the static ones
        // alone, into the static layer; 2: the dynamic ones). An instance whose bounding sphere misses the face's frustum
        // (its four sides and the light's range) drops every vertex at once, before its vertex is read
        LsmJob jb = LsmBuf(f.lsm_tab).j[pc.pad & 0xFFFFu];
        uint mode = (pc.pad >> 16) & 3u;
        MeshInfo mi = MeshInfoBuf(f.mesh_info).m[inst.ids.y >> INST_SHAPE_SHIFT];
        vec4 sc = vec4(mi.sphere.xyz, 1.0);
        vec3 cw = vec3(dot(inst.row0, sc), dot(inst.row1, sc), dot(inst.row2, sc));
        float rs = mi.sphere.w * sqrt(max(dot(inst.row0.xyz, inst.row0.xyz), max(dot(inst.row1.xyz, inst.row1.xyz), dot(inst.row2.xyz, inst.row2.xyz))));
        bool dyn = (inst.ids.y & INST_DYNAMIC) != 0u;
        bool out_ = (mode == 1u && dyn) || (mode == 2u && !dyn) || distance(cw, jb.sphere.xyz) > jb.sphere.w + rs;
        for (int k = 0; k < 4; k++) out_ = out_ || dot(jb.planes[k].xyz, cw) + jb.planes[k].w < -rs;
        o_normal = vec3(0.0, 0.0, 1.0);
        o_instance = slot;
        o_ray_bias = 0.0;
        if (out_) {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // (outside the clip volume: the triangle is dropped)
            o_world = vec3(0.0);
            o_uvs = vec4(0.0);
            return;
        }
        uint vi = (inst.ids.y & INST_SKINNED) != 0u ? inst.ids.w + uint(gl_VertexIndex - mi.vertex_offset.x) : uint(gl_VertexIndex);
        Vertex v = VertexBuf(pc.vertices).v[vi];
        vec4 p = vec4(q17_pos(f, inst, mi, v), 1.0);
        vec3 world = vec3(dot(inst.row0, p), dot(inst.row1, p), dot(inst.row2, p));
        gl_Position = jb.m * vec4(world, 1.0);
        o_world = world;
        o_uvs = vec4(v.uv2, v.uv * ((inst.ids.y & INST_MATERIAL) != 0u ? inst.emissive_rough.y : 1.0));
        return;
    }
    if (pc.pass == 1u) {
        // The sun's virtual shadow maps (vsm.c): drawn into level pc.pad, inside this frame's rectangle of it. An instance
        // whose bounding sphere misses the rectangle drops every vertex at once (before its vertex is read): a level's
        // dirty rectangle around a walking character costs its casters, not the whole shadow box's
        uint lv = pc.pad & 0xFFu, mode = pc.pad >> 8; // mode 1: the static casters alone (the static layer), 2: the dynamic ones
        mat4 lm = f.vsm_mat[lv];
        vec4 jr = f.vsm_job[lv];
        MeshInfo mi = MeshInfoBuf(f.mesh_info).m[inst.ids.y >> INST_SHAPE_SHIFT];
        vec4 sc = vec4(mi.sphere.xyz, 1.0);
        vec3 cw = vec3(dot(inst.row0, sc), dot(inst.row1, sc), dot(inst.row2, sc));
        float rs = mi.sphere.w * sqrt(max(dot(inst.row0.xyz, inst.row0.xyz), max(dot(inst.row1.xyz, inst.row1.xyz), dot(inst.row2.xyz, inst.row2.xyz))));
        vec2 lc = (lm * vec4(cw, 1.0)).xy;
        vec2 lo = jr.xy, hi = jr.xy + 2.0 / jr.zw;
        o_normal = vec3(0.0, 0.0, 1.0);
        o_instance = slot;
        o_ray_bias = 0.0;
        bool dyn = (inst.ids.y & INST_DYNAMIC) != 0u;
        if (any(lessThan(lc + rs, lo)) || any(greaterThan(lc - rs, hi)) || (mode == 1u && dyn) || (mode == 2u && !dyn)) {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // (outside the clip volume: the triangle is dropped)
            o_world = vec3(0.0);
            o_uvs = vec4(0.0);
            return;
        }
        // (a skin, INST_SKINNED: its own bent vertices, the same index into them)
        uint vi = (inst.ids.y & INST_SKINNED) != 0u ? inst.ids.w + uint(gl_VertexIndex - mi.vertex_offset.x) : uint(gl_VertexIndex);
        Vertex v = VertexBuf(pc.vertices).v[vi];
        vec4 p = vec4(q17_pos(f, inst, mi, v), 1.0);
        // (a plant at rest: the cache holds the same drawing whenever a rectangle of it is drawn again)
        vec3 world = vec3(dot(inst.row0, p), dot(inst.row1, p), dot(inst.row2, p));
        vec3 l = (lm * vec4(world, 1.0)).xyz;
        gl_Position = vec4((l.xy - jr.xy) * jr.zw - 1.0, l.z, 1.0);
        o_world = world;
        o_uvs = vec4(v.uv2, v.uv * ((inst.ids.y & INST_MATERIAL) != 0u ? inst.emissive_rough.y : 1.0));
        return;
    }
    if ((pc.pass == 0u || pc.pass == 3u) && (f.far_u.x & FAR_IMP) != 0u && uint(gl_VertexIndex) - f.far_u.z < 4u) {
        // (Q21) an impostor's card (cull.comp drew the instance by it): a square facing the camera at the instance's
        // bounding sphere, as wide as the sphere; mesh.frag reads the atlas (o_instance's top bit: a card; o_uvs.zw the
        // corner, -1..1). Its UV2 the middle (the surface cache's texel of the whole object)
        MeshInfo cm = MeshInfoBuf(f.mesh_info).m[inst.ids.y >> INST_SHAPE_SHIFT];
        vec4 sc = vec4(cm.sphere.xyz, 1.0);
        vec3 cw = vec3(dot(inst.row0, sc), dot(inst.row1, sc), dot(inst.row2, sc));
        vec3 ca = vec3(inst.row0.x, inst.row1.x, inst.row2.x), cb = vec3(inst.row0.y, inst.row1.y, inst.row2.y), cc = vec3(inst.row0.z, inst.row1.z, inst.row2.z);
        float rs = cm.sphere.w * sqrt(max(dot(ca, ca), max(dot(cb, cb), dot(cc, cc))));
        vec3 V = view_ortho(f) ? f.camera_fwd.xyz : normalize(cw - f.camera_pos.xyz);
        vec3 R = cross(V, vec3(0.0, 0.0, 1.0));
        R = dot(R, R) < 1e-6 ? f.camera_right.xyz : normalize(R);
        vec3 U = cross(R, V);
        uint k = uint(gl_VertexIndex) - f.far_u.z; // (build_plane's corners: -x-y, +x-y, +x+y, -x+y)
        vec2 corner = vec2(k == 1u || k == 2u ? 1.0 : -1.0, k >= 2u ? 1.0 : -1.0);
        // as wide and tall as the mesh's box seen this way (the table's half sides): a tree's card isn't its sphere's
        // square (60% of that is air that the prepass pays to cut)
        uint hb = ImpTabBuf(f.imp_tab).e[inst.ids.y >> INST_SHAPE_SHIFT].w;
        vec3 he = vec3(float(hb & 1023u), float((hb >> 10) & 1023u), float((hb >> 20) & 1023u)) * (cm.sphere.w / 1023.0);
        vec2 ext = hb == 0u ? vec2(rs) : vec2(abs(dot(R, ca)) * he.x + abs(dot(R, cb)) * he.y + abs(dot(R, cc)) * he.z,
                                              abs(dot(U, ca)) * he.x + abs(dot(U, cb)) * he.y + abs(dot(U, cc)) * he.z);
        ext = min(ext, vec2(rs));
        vec3 world = cw + R * (corner.x * ext.x) + U * (corner.y * ext.y);
        o_normal = -V;
        o_world = world;
        o_instance = slot | 0x80000000u;
        o_uvs = vec4(0.5, 0.5, corner);
        o_ray_bias = rs * 0.5; // (its rays start past the card's depth error)
        gl_Position = f.view_proj * vec4(world, 1.0);
        return;
    }
    uint vi = uint(gl_VertexIndex);
    if ((inst.ids.y & INST_SKINNED) != 0u) // a skin: its own bent vertices (render.c "skin"), the same index into them
        vi = inst.ids.w + uint(gl_VertexIndex - MeshInfoBuf(f.mesh_info).m[inst.ids.y >> INST_SHAPE_SHIFT].vertex_offset.x);
    Vertex v = VertexBuf(pc.vertices).v[vi];
    MeshInfo qm = MeshInfoBuf(f.mesh_info).m[inst.ids.y >> INST_SHAPE_SHIFT];
    vec4 p = vec4(q17_pos(f, inst, qm, v), 1.0);
    o_disp_bias = q17_bias(f, inst, qm);
    vec3 world = vec3(dot(inst.row0, p), dot(inst.row1, p), dot(inst.row2, p));
    if ((inst.ids.y & INST_WIND) != 0u) { // a plant: bent by the wind, more toward its top, gusts by place and time
        vec3 base = vec3(inst.row0.w, inst.row1.w, inst.row2.w);
        float hgt = max(world.z - base.z, 0.0), t = f.camera_pos.w;
        float phase = dot(base.xy, vec2(0.0021, 0.0017));
        float gust = 0.6 + 0.4 * sin(t * 0.7 + phase * 0.3);
        float bend = hgt * hgt * 0.00012 * gust;
        world.xy += vec2(sin(t * 1.9 + phase), cos(t * 1.4 + phase * 1.3) * 0.5) * bend + vec2(bend * 0.6, 0.0);
    }
    // The normal through the cofactor matrix (right under non-uniform scale, no inverse needed)
    vec3 a = vec3(inst.row0.x, inst.row1.x, inst.row2.x);
    vec3 b = vec3(inst.row0.y, inst.row1.y, inst.row2.y);
    vec3 c = vec3(inst.row0.z, inst.row1.z, inst.row2.z);
    vec3 n = oct_decode(v.normal_oct);
    o_normal = cross(b, c) * n.x + cross(c, a) * n.y + cross(a, b) * n.z;
    o_world = world;
    o_instance = slot;
    // one vec4 for both UVs (a vertex's outputs are what a draw of a million instances pays for)
    o_uvs = vec4(v.uv2, v.uv * ((inst.ids.y & INST_MATERIAL) != 0u ? inst.emissive_rough.y : 1.0));
#ifdef RE_EXT
    { // its mesh's vertex colors (RMESH_COLORS: sRGB bytes in the meshes' words), by its place in the mesh
        MeshInfo vm = MeshInfoBuf(f.mesh_info).m[inst.ids.y >> INST_SHAPE_SHIFT];
        if (vm.vcol_first != 0xFFFFFFFFu && any(notEqual(f.mesh_words, uvec2(0u)))) {
            vec4 c = unpackUnorm4x8(UintBuf(f.mesh_words).v[vm.vcol_first + uint(gl_VertexIndex - vm.vertex_offset.x)]);
            o_vcol = vec4(mix(c.rgb / 12.92, pow((c.rgb + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c.rgb)), c.a);
        }
    }
#endif
    // the main passes draw the LOD the cull picked (vis: its draw, shape * 4 + LOD): a coarse one sits inside the BLAS
    o_ray_bias = 0.0;

    if (pc.pass == 0u || pc.pass == 3u || pc.pass == 6u) {
        uint lod = (UintBuf(f.vis).v[slot] & 0xFFFu) % 4u;

        // a skin's BLAS holds a coarser LOD (render.c skin_rt_lod: lod_count bits 16-17, MESH_INFO_RT_LOD_SHIFT): its
        // rays start past the farther of the two
        uint rl = (inst.ids.y & INST_SKINNED) != 0u ? (MeshInfoBuf(f.mesh_info).m[inst.ids.y >> INST_SHAPE_SHIFT].lod_count >> 16u) & 3u : 0u;
        if (max(lod, rl) > 0u) {
            float s = sqrt(max(dot(a, a), max(dot(b, b), dot(c, c))));
            o_ray_bias = MeshInfoBuf(f.mesh_info).m[inst.ids.y >> INST_SHAPE_SHIFT].lod_error[max(lod, rl) - 1u] * s * 1.25;
        }
        // a cluster LOD DAG's cut (an opaque mesh's clusters, cull.comp): each cluster's error fits the limit on screen, so
        // it sits within that many pixels of LOD 0 (the BLAS) at its distance
        MeshInfo mi = MeshInfoBuf(f.mesh_info).m[inst.ids.y >> INST_SHAPE_SHIFT];
        if (pc.pass == 6u && mi.dag_count != 0u && (mi.lod_count & 0x100u) == 0u) {
            float px_cm = view_ortho(f) ? 1.0 / f.cull_params.x : length(world - f.camera_pos.xyz) / f.cull_params.x;
            o_ray_bias = max(o_ray_bias, max(uintBitsToFloat(f.pad_cull1), 0.25) * px_cm * 1.25);
        }
        o_ray_bias += o_disp_bias; // (Q17) a displaced surface sinks under its BLAS that far
    }
    if (o_disp_bias > 0.0) o_ray_bias = -max(o_ray_bias, o_disp_bias); // (Q17: the sign says displaced)
    gl_Position = (pc.pass == 2u || (pc.pass & 0x80000000u) != 0u ? f.tsr_vp : f.view_proj) * vec4(world, 1.0); // (the selection's mask: the output's size, no jitter)
}
