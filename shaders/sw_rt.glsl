// Ray tracing by software (A1, src/sw_rt.c): the same queries as rt.glsl's ray queries (RE_SWRT), walking the BVHs in
// buffers. FrameBuf.tlas / .tlas_dyn: a set's header (its BVH over the instances; the meshes' BVHs and triangles). An
// instance's leaf: its slot (InstanceBuf) and its mask; the ray goes into the mesh's space by the inverse of its rows and
// walks the mesh's BVH (GpuMeshInfo.blas: its root). No alpha test (a Masked mesh blocks as opaque) and no backface
// culling, as the hardware's FACING_CULL_DISABLE.

struct SwNode { vec4 amn, amx, bmn, bmx; }; // (w: the child's index; its count: >0 a leaf's range, 0 a node, ~0 none)
struct SwTri { vec4 p0, p1, p2; };           // (p0.w: the triangle's index in its mesh, uint bits)
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer SwNodeBuf { SwNode n[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer SwTriBuf { SwTri t[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer SwInstBuf { uint i[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer SwScene {
    uvec2 nodes, inst, mesh_nodes, mesh_tris;
    uint count;
};

#define SW_EMPTY 0xFFFFFFFFu
#define SW_STACK 40

// the slab test of a child's box: its near distance (> tmax: missed)
float sw_box(vec3 o, vec3 inv, vec3 mn, vec3 mx, float tmax) {
    vec3 t0 = (mn - o) * inv, t1 = (mx - o) * inv;
    vec3 lo = min(t0, t1), hi = max(t0, t1);
    float tn = max(max(lo.x, lo.y), max(lo.z, 0.0)), tf = min(min(hi.x, hi.y), min(hi.z, tmax));
    return tn <= tf ? tn : 1e30;
}

// Möller-Trumbore: t and the barycentrics (u, v of p1, p2)
bool sw_tri(vec3 o, vec3 d, SwTri tr, float tmin, float tmax, out float t, out vec2 bc) {
    vec3 e1 = tr.p1.xyz - tr.p0.xyz, e2 = tr.p2.xyz - tr.p0.xyz;
    vec3 pv = cross(d, e2);
    float det = dot(e1, pv);
    t = 0.0, bc = vec2(0.0);
    if (abs(det) < 1e-12) return false;
    float id = 1.0 / det;
    vec3 tv = o - tr.p0.xyz;
    float u = dot(tv, pv) * id;
    if (u < 0.0 || u > 1.0) return false;
    vec3 qv = cross(tv, e1);
    float v = dot(d, qv) * id;
    if (v < 0.0 || u + v > 1.0) return false;
    t = dot(e2, qv) * id;
    bc = vec2(u, v);
    return t >= tmin && t < tmax;
}

// The nearest triangle of one mesh along the ray in its space (t of the world ray: d not normalized), or any (any_hit)
bool sw_mesh(SwScene sc, uint root, vec3 o, vec3 d, inout float tmax, bool any_hit, out uint prim, out vec2 bc) {
    SwNodeBuf nb = SwNodeBuf(packUint2x32(sc.mesh_nodes));
    SwTriBuf tb = SwTriBuf(packUint2x32(sc.mesh_tris));
    vec3 inv = 1.0 / vec3(d.x != 0.0 ? d.x : 1e-12, d.y != 0.0 ? d.y : 1e-12, d.z != 0.0 ? d.z : 1e-12);
    uint stack[SW_STACK];
    int sp = 0;
    stack[sp++] = root;
    bool found = false;
    prim = 0u, bc = vec2(0.0);
    while (sp > 0) {
        SwNode n = nb.n[stack[--sp]];
        uint ac = floatBitsToUint(n.amx.w), bcn = floatBitsToUint(n.bmx.w);
        float ta = ac != SW_EMPTY ? sw_box(o, inv, n.amn.xyz, n.amx.xyz, tmax) : 1e30;
        float tbb = bcn != SW_EMPTY ? sw_box(o, inv, n.bmn.xyz, n.bmx.xyz, tmax) : 1e30;
        // each child that the ray crosses: a leaf's triangles now, a node onto the stack (the nearer one popped first)
        for (int c = 0; c < 2; c++) {
            bool first = (ta > tbb) == (c == 0); // (the farther one first: the nearer is popped next)
            float tc = first ? ta : tbb;
            uint cnt = first ? ac : bcn, idx = floatBitsToUint(first ? n.amn.w : n.bmn.w);
            if (tc > tmax) continue;
            if (cnt == 0u) {
                if (sp < SW_STACK) stack[sp++] = idx;
                continue;
            }
            for (uint k = 0u; k < cnt; k++) {
                SwTri tr = tb.t[idx + k];
                float t;
                vec2 b;
                if (sw_tri(o, d, tr, 0.5, tmax, t, b)) {
                    tmax = t, prim = floatBitsToUint(tr.p0.w), bc = b, found = true;
                    if (any_hit) return true;
                }
            }
        }
    }
    return found;
}

// The whole set: its instances' BVH, each instance's mesh. t: the nearest hit's; ii its slot
bool sw_trace(FrameBuf f, uvec2 scene, vec3 o, vec3 d, float tmax, uint mask, bool any_hit, out float t, out uint ii, out uint prim, out vec2 bc) {
    t = tmax, ii = 0u, prim = 0u, bc = vec2(0.0);
    if ((scene.x | scene.y) == 0u) return false;
    SwScene sc = SwScene(packUint2x32(scene));
    SwNodeBuf nb = SwNodeBuf(packUint2x32(sc.nodes));
    SwInstBuf ib = SwInstBuf(packUint2x32(sc.inst));
    vec3 inv = 1.0 / vec3(d.x != 0.0 ? d.x : 1e-12, d.y != 0.0 ? d.y : 1e-12, d.z != 0.0 ? d.z : 1e-12);
    uint stack[SW_STACK];
    int sp = 0;
    stack[sp++] = 0u;
    bool found = false;
    while (sp > 0) {
        SwNode n = nb.n[stack[--sp]];
        uint ac = floatBitsToUint(n.amx.w), bcn = floatBitsToUint(n.bmx.w);
        float ta = ac != SW_EMPTY ? sw_box(o, inv, n.amn.xyz, n.amx.xyz, t) : 1e30;
        float tbb = bcn != SW_EMPTY ? sw_box(o, inv, n.bmn.xyz, n.bmx.xyz, t) : 1e30;
        for (int c = 0; c < 2; c++) {
            bool first = (ta > tbb) == (c == 0); // (the farther one first: the nearer is popped next)
            float tc = first ? ta : tbb;
            uint cnt = first ? ac : bcn, idx = floatBitsToUint(first ? n.amn.w : n.bmn.w);
            if (tc > t) continue;
            if (cnt == 0u) {
                if (sp < SW_STACK) stack[sp++] = idx;
                continue;
            }
            for (uint k = 0u; k < cnt; k++) {
                uint e = ib.i[idx + k], slot = e & 0xFFFFFFu;
                if (((e >> 24u) & mask) == 0u) continue;
                Instance inst = InstanceBuf(f.instances).i[slot];
                uvec2 root = MeshInfoBuf(f.mesh_info).m[inst.ids.y >> INST_SHAPE_SHIFT].blas;
                if (root.y == 0u) continue;
                // the ray into the mesh's space: the rows' inverse (rotation and scale by the adjugate, then the move)
                mat3 m = mat3(inst.row0.x, inst.row1.x, inst.row2.x, inst.row0.y, inst.row1.y, inst.row2.y, inst.row0.z, inst.row1.z, inst.row2.z);
                vec3 tr = vec3(inst.row0.w, inst.row1.w, inst.row2.w);
                mat3 mi = inverse(m);
                vec3 lo = mi * (o - tr), ld = mi * d;
                float tt = t;
                uint pr;
                vec2 b;
                if (sw_mesh(sc, root.x, lo, ld, tt, any_hit, pr, b) && tt < t) {
                    t = tt, ii = slot, prim = pr, bc = b, found = true;
                    if (any_hit) return true;
                }
            }
        }
    }
    return found;
}
