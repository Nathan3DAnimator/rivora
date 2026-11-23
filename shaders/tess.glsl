// (Q17) Nanite Tessellation (notas-dev/tessellation.md): a cluster LOD DAG's triangle displaced by its material's height
// (the Displacement baked into the base color's alpha, M7) and diced into N x N micro triangles by its size on screen.
// Shared by tess.comp (the dicing factors, the seam check, Q22: the corners), tess.vert (a micro vertex) and mesh.vert (the
// other passes: a base vertex displaced, nothing diced).
// Crack free: a point on an edge is computed from that edge alone (its two corners in the canonical order, the lower
// canonical vertex first; the factor from their positions), so the two triangles along it agree to the bit. A corner's
// displacement comes from the mesh's canonical vertex at that position (render.c tess_canon_table): the copies a UV seam
// or a hard edge makes all move the same way. An edge whose two corners are such copies (a seam) is displaced linearly
// between its corners (its UVs differ on each side), the others by the height along it.
// Needs common.glsl and mat_common.glsl (the material set bound).
#ifndef TESS_GLSL
#define TESS_GLSL

#define TESS_MAX 32u         // the most micro edges along a base edge (the patterns 1..TESS_MAX: tess.c)
#define TESS_ON 1u           // f.tess_u.x: displacement on (the materials with Enable Tessellation)
#define TESS_SUBDIV 2u       // ... and diced (else the base vertices displaced alone: Low)
#define TESS_SEAMS 4u        // ... the seam check (tess.comp pass 1, the tess_seams test)
#define TESS_DUP 0x80000000u // the canonical table: this vertex shares its position with another
// R.tess's layout (render_gpu.h TESS_*: the same numbers)
#define TESS_MAX_TRIS 262144u // (Q22: was 524288; each one carries its corners now, TESS_CRN_AT)
#define TESS_CMDS_AT 320u    // 2 sets x TESS_MAX draws (20 B)
#define TESS_OWNER_AT 2048u  // per triangle its cluster entry (cull.comp pass 7)
#define TESS_IDX_AT (TESS_OWNER_AT + TESS_MAX_TRIS * 4u)  // per triangle its 3 indices (pass 7)
#define TESS_TMP_AT (TESS_IDX_AT + TESS_MAX_TRIS * 12u)   // per triangle its factors and place in its pattern's list (pass 1)
#define TESS_REC_AT (TESS_TMP_AT + TESS_MAX_TRIS * 8u)    // the drawn records, by pattern (pass 3): triangle, slot, factors, vertex offset
#define TESS_REC_PAD 8192u   // (tess.h) the records' room past TESS_MAX_TRIS
#define TESS_CRN_AT (TESS_REC_AT + (TESS_MAX_TRIS + TESS_REC_PAD) * 16u) // (Q22) per triangle its corners (pass 1): 8 x 16 B
#define TESS_NONE 0xFFFFFFFFu // a record that pads a pattern's list to its batch: its points are dropped
// base triangles an instance of pattern n draws (tess.c TESS_BATCH): full waves of the vertex shader for small patterns
uint tess_batch(uint n) { return max(128u / (n * n), 1u); }
layout(buffer_reference, scalar, buffer_reference_align = 16) buffer TessRecBuf { uvec4 r[]; };
layout(buffer_reference, scalar, buffer_reference_align = 8) buffer TessTmpBuf { uvec2 t[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) buffer TessCrnBuf { uvec4 q[]; };

// whether an instance's material is displaced (cull.comp, tess.comp: the clusters it is diced in)
bool tess_material(FrameBuf f, Instance inst) {
    if ((f.tess_u.x & TESS_ON) == 0u || (inst.ids.y & INST_MATERIAL) == 0u) return false;
    return (GpuMaterialBuf(f.materials).m[floatBitsToUint(inst.emissive_rough.x)].flags & MATF_TESS) != 0u;
}

// An instance's displacement: its material's baked base (the height in its alpha), magnitude and center (GpuMaterial
// anim.z: two halfs), the instance's UV tiling; its mesh's canonical table (its vertex count, the UV's units per cm) and rows
struct TessCtx {
    uint tex;
    float mag, center, tiling, texels_cm; // (texels_cm: the height's texels per cm of the mesh)
    uint vofs, n, first;                  // the mesh's vertex offset; the table's vertices (0: none) and first word
    vec4 r0, r1, r2;
    float s;                              // the instance's largest scale
    bool on;
};

TessCtx tess_ctx(FrameBuf f, Instance inst, MeshInfo mi, uint vofs) {
    TessCtx c;
    c.on = false, c.tex = 0u, c.mag = 0.0, c.center = 0.5, c.tiling = 1.0, c.texels_cm = 0.0;
    c.vofs = vofs, c.n = 0u, c.first = mi.tess_first;
    c.r0 = inst.row0, c.r1 = inst.row1, c.r2 = inst.row2;
    vec3 a = vec3(inst.row0.x, inst.row1.x, inst.row2.x), b = vec3(inst.row0.y, inst.row1.y, inst.row2.y), d = vec3(inst.row0.z, inst.row1.z, inst.row2.z);
    c.s = max(sqrt(max(dot(a, a), max(dot(b, b), dot(d, d)))), 1e-6);
    if (!tess_material(f, inst)) return c;
    uint ms = floatBitsToUint(inst.emissive_rough.x);
    GpuMaterial gm = GpuMaterialBuf(f.materials).m[ms];
    vec2 mc = unpackHalf2x16(gm.anim.z); // (material_gpu.inc: the magnitude and center, MATF_TESS)
    c.tex = gm.tex_base, c.mag = mc.x, c.center = mc.y, c.tiling = inst.emissive_rough.y;
    float uv_cm = 0.0;
    if (c.first != 0xFFFFFFFFu && any(notEqual(f.mesh_words, uvec2(0u)))) {
        c.n = UintBuf(f.mesh_words).v[c.first];
        uv_cm = uintBitsToFloat(UintBuf(f.mesh_words).v[c.first + 1u]);
    }
    c.texels_cm = uv_cm * c.tiling * float(textureSize(u_mat_tex[nonuniformEXT(c.tex)], 0).x);
    c.on = c.mag != 0.0 && c.tex != 0u;
    return c;
}

// a vertex's canonical one (local index; TESS_DUP: it has copies) and its position (a copy a hair off moves onto it)
uint tess_canon(FrameBuf f, TessCtx c, uint l) { return l < c.n ? UintBuf(f.mesh_words).v[c.first + 2u + l] : l; }
vec3 tess_cpos(FrameBuf f, TessCtx c, uint l) { return VertexBuf(pc.vertices).v[c.vofs + (tess_canon(f, c, l) & ~TESS_DUP)].pos; }

// (precise: no fused or reordered arithmetic, so every call site, inlined apart, rounds the same: a point on an edge
// computed by two triangles, or twice in one, gets the same bits)
vec3 tess_world(TessCtx c, vec3 p) {
    precise vec3 w = vec3(c.r0.x * p.x + c.r0.y * p.y + c.r0.z * p.z + c.r0.w, c.r1.x * p.x + c.r1.y * p.y + c.r1.z * p.z + c.r1.w,
                          c.r2.x * p.x + c.r2.y * p.y + c.r2.z * p.z + c.r2.w);
    return w;
}

// the displacement along the normal at a point (mesh space cm): the height read at the mip whose texel is about a
// micro edge there (a distant one doesn't shimmer); the same inputs give the same bits
float tess_disp(FrameBuf f, TessCtx c, vec2 uv, vec3 p) {
    if (!c.on) return 0.0;
    precise float cmpp = view_ortho(f) ? 1.0 / f.cull_params.x : max(length(tess_world(c, p) - f.camera_pos.xyz), 1.0) / f.cull_params.x;
    precise float lod = log2(max(max(f.tess.x, 1.0) * cmpp / c.s * c.texels_cm, 1.0));
    precise vec2 tuv = uv * c.tiling;
    float h = textureLod(u_mat_tex[nonuniformEXT(c.tex)], tuv, lod).a;
    precise float d = (h - c.center) * c.mag;
    return d;
}

// a corner's displacement vector: its canonical vertex's normal and height (every triangle at that point agrees)
vec3 tess_corner_disp(FrameBuf f, TessCtx c, uint l) {
    Vertex v = VertexBuf(pc.vertices).v[c.vofs + (tess_canon(f, c, l) & ~TESS_DUP)];
    precise vec3 d = normalize(oct_decode(v.normal_oct)) * tess_disp(f, c, v.uv, v.pos);
    return d;
}

// A base vertex displaced (mesh space): the passes that don't dice (shadows, the fallback without room)
vec3 tess_vertex(FrameBuf f, TessCtx c, uint l, vec3 p) { return c.on ? tess_cpos(f, c, l) + tess_corner_disp(f, c, l) : p; }

// The point k / e along the edge between the local vertices la and lb, displaced (mesh space). Computed from the edge
// alone, in its canonical order: both triangles along it get the same bits. One call site per kind of point (a corner,
// a seam's point, a smooth edge's point): the same code for the same point
vec3 tess_edge(FrameBuf f, TessCtx c, uint la, uint lb, uint k, uint e) {
    uint ca = tess_canon(f, c, la), cb = tess_canon(f, c, lb);
    if ((cb & ~TESS_DUP) < (ca & ~TESS_DUP)) {
        uint t = la; la = lb, lb = t;
        t = ca; ca = cb, cb = t;
        k = e - k;
    }
    if (k == 0u || k == e || (ca & ~TESS_DUP) == (cb & ~TESS_DUP)) { // a corner (or an edge of no length)
        uint lc = k == e && (ca & ~TESS_DUP) != (cb & ~TESS_DUP) ? lb : la;
        precise vec3 q = tess_cpos(f, c, lc) + tess_corner_disp(f, c, lc);
        return q;
    }
    float wb = float(k) / float(e), wa = float(e - k) / float(e);
    Vertex va = VertexBuf(pc.vertices).v[c.vofs + la], vb = VertexBuf(pc.vertices).v[c.vofs + lb];
    precise vec3 p = tess_cpos(f, c, la) * wa + tess_cpos(f, c, lb) * wb; // (the canonical ends: the same bits on both sides)
    if ((ca & cb & TESS_DUP) != 0u) { // a seam (both corners are copies): its corners' displacement, linearly
        precise vec3 q = p + (tess_corner_disp(f, c, la) * wa + tess_corner_disp(f, c, lb) * wb);
        return q;
    }
    precise vec3 n = normalize(oct_decode(va.normal_oct) * wa + oct_decode(vb.normal_oct) * wb);
    precise vec2 uv = va.uv * wa + vb.uv * wb;
    precise vec3 q = p + n * tess_disp(f, c, uv, p);
    return q;
}

// An edge's dicing factor: its length on screen over the target (px per micro edge), from its two ends alone (the same
// in both triangles along it)
uint tess_factor(FrameBuf f, TessCtx c, vec3 pa, vec3 pb) {
    vec3 wa = tess_world(c, pa), wb = tess_world(c, pb);
    precise float len = length(wa - wb);
    precise float px = view_ortho(f) ? len * f.cull_params.x : len / max(length((wa + wb) * 0.5 - f.camera_pos.xyz), 1.0) * f.cull_params.x;
    return uint(clamp(ceil(px / max(f.tess.x, 0.25)), 1.0, max(f.tess.y, 1.0)));
}
// the factors packed: e01 | e12 << 6 | e02 << 12 | N << 18 (N the largest: the pattern)
uint tess_pack(uint e01, uint e12, uint e02) { return e01 | (e12 << 6) | (e02 << 12) | (max(e01, max(e12, e02)) << 18); }

// (Q22) A base triangle's corners, gathered once by tess.comp pass 1 (TESS_CRN_AT, 128 B): tess.vert reads them per
// micro vertex instead of the record -> instance -> mesh -> material -> indices -> vertices -> canonical table chain
// (8 dependent reads, then a texture read per corner point). Per corner: its canonical position, its canonical
// displacement (halfs: the same bits wherever that vertex is a corner), its own normal and UVs; per triangle: the
// displacement's texture, magnitude, center, tiling, its mip's scale, the rays' bias, and per edge 3 bits (swap: its
// second end has the lower canonical vertex; one point: both ends at one canonical vertex; seam: both are copies)
struct TessCorner { vec3 p; vec3 d; uint n; vec2 uv; uint uv2; };
struct TessTri { TessCorner k0, k1, k2; uint tex, flags; float mag, center, tiling, lodk, bias; };
#define TESS_E_SWAP 1u
#define TESS_E_POINT 2u
#define TESS_E_SEAM 4u
void tess_tri_pack(TessTri t, out uvec4 q[8]) {
    q[0] = uvec4(floatBitsToUint(t.k0.p), t.k0.n), q[1] = uvec4(floatBitsToUint(t.k1.p), t.k1.n), q[2] = uvec4(floatBitsToUint(t.k2.p), t.k2.n);
    q[3] = uvec4(floatBitsToUint(t.k0.uv), t.k0.uv2, packHalf2x16(t.k0.d.xy));
    q[4] = uvec4(floatBitsToUint(t.k1.uv), t.k1.uv2, packHalf2x16(t.k1.d.xy));
    q[5] = uvec4(floatBitsToUint(t.k2.uv), t.k2.uv2, packHalf2x16(t.k2.d.xy));
    q[6] = uvec4(packHalf2x16(vec2(t.k0.d.z, t.k1.d.z)), packHalf2x16(vec2(t.k2.d.z, 0.0)), t.tex | (t.flags << 23), packHalf2x16(vec2(t.mag, t.center)));
    q[7] = uvec4(floatBitsToUint(t.tiling), floatBitsToUint(t.lodk), floatBitsToUint(t.bias), 0u);
}
TessTri tess_tri_unpack(uvec4 q0, uvec4 q1, uvec4 q2, uvec4 q3, uvec4 q4, uvec4 q5, uvec4 q6, uvec4 q7) {
    TessTri t;
    vec2 z01 = unpackHalf2x16(q6.x), z2 = unpackHalf2x16(q6.y), mc = unpackHalf2x16(q6.w);
    t.k0.p = uintBitsToFloat(q0.xyz), t.k0.n = q0.w, t.k0.uv = uintBitsToFloat(q3.xy), t.k0.uv2 = q3.z, t.k0.d = vec3(unpackHalf2x16(q3.w), z01.x);
    t.k1.p = uintBitsToFloat(q1.xyz), t.k1.n = q1.w, t.k1.uv = uintBitsToFloat(q4.xy), t.k1.uv2 = q4.z, t.k1.d = vec3(unpackHalf2x16(q4.w), z01.y);
    t.k2.p = uintBitsToFloat(q2.xyz), t.k2.n = q2.w, t.k2.uv = uintBitsToFloat(q5.xy), t.k2.uv2 = q5.z, t.k2.d = vec3(unpackHalf2x16(q5.w), z2.x);
    t.tex = q6.z & 0x7FFFFFu, t.flags = q6.z >> 23, t.mag = mc.x, t.center = mc.y;
    t.tiling = uintBitsToFloat(q7.x), t.lodk = uintBitsToFloat(q7.y), t.bias = uintBitsToFloat(q7.z);
    return t;
}
TessTri tess_tri_load(FrameBuf f, uint tri) {
    TessCrnBuf b = TessCrnBuf(addr_add(f.tess_buf, TESS_CRN_AT));
    uint o = tri * 8u;
    return tess_tri_unpack(b.q[o], b.q[o + 1u], b.q[o + 2u], b.q[o + 3u], b.q[o + 4u], b.q[o + 5u], b.q[o + 6u], b.q[o + 7u]);
}

// the displacement at a point (mesh space cm) from the triangle's own copy of its material (x: its instance's rows)
float tess_tri_disp(FrameBuf f, TessTri t, TessCtx x, vec2 uv, vec3 p) {
    if (t.tex == 0u) return 0.0;
    precise float cmpp = view_ortho(f) ? 1.0 / f.cull_params.x : max(length(tess_world(x, p) - f.camera_pos.xyz), 1.0) / f.cull_params.x;
    precise float lod = log2(max(max(f.tess.x, 1.0) * cmpp * t.lodk, 1.0));
    precise vec2 tuv = uv * t.tiling;
    float h = textureLod(u_mat_tex[nonuniformEXT(t.tex)], tuv, lod).a;
    precise float d = (h - t.center) * t.mag;
    return d;
}

// tess_edge from the corners (fl: that edge's 3 bits): the point k / e from a to b, displaced (mesh space). The same
// arithmetic in the canonical order, so both triangles along the edge get the same bits
vec3 tess_tri_edge(FrameBuf f, TessTri t, TessCtx x, TessCorner a, TessCorner b, uint fl, uint k, uint e) {
    if ((fl & TESS_E_SWAP) != 0u) {
        TessCorner s = a; a = b, b = s;
        k = e - k;
    }
    if (k == 0u || k == e || (fl & TESS_E_POINT) != 0u) { // a corner (or an edge of no length)
        TessCorner c = k == e && (fl & TESS_E_POINT) == 0u ? b : a;
        precise vec3 q = c.p + c.d;
        return q;
    }
    float wb = float(k) / float(e), wa = float(e - k) / float(e);
    precise vec3 p = a.p * wa + b.p * wb;
    if ((fl & TESS_E_SEAM) != 0u) { // a seam: its corners' displacement, linearly
        precise vec3 q = p + (a.d * wa + b.d * wb);
        return q;
    }
    precise vec3 n = normalize(oct_decode(a.n) * wa + oct_decode(b.n) * wb);
    precise vec2 uv = a.uv * wa + b.uv * wb;
    precise vec3 q = p + n * tess_tri_disp(f, t, x, uv, p);
    return q;
}
#endif
