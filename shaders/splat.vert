#version 460
// A Gaussian's quad (A9.5d, splat.glsl): the sorted Gaussian for gl_VertexIndex / 6, its covariance in the world (the
// actor's turn and scale), seen from the camera (the view's rows, the perspective's Jacobian: EWA), widened by 0.3 px^2
// (the pixel's own blur, as 3DGS), its 3 sigmas' square on the screen around its center; the conic (the inverse 2D
// covariance) and the offset in pixels for splat.frag, its color lit by the scene or as light
#include "common.glsl"
#include "splat.glsl"

layout(location = 0) out vec2 o_d;           // the offset from its center (px)
layout(location = 1) flat out vec3 o_conic;  // the inverse 2D covariance (a, b, c)
layout(location = 2) flat out vec4 o_color;  // color (pre-exposed light), opacity

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    SplatFrame S = SplatFrameBuf(pc.instances).f;
    uint q = uint(gl_VertexIndex) / 6u, k = uint(gl_VertexIndex) % 6u;
    uint v = SplatUint(S.sorted).v[q];
    SplatActor A = SplatActorBuf(S.actors).a[v >> SPLAT_ACTOR_SHIFT];
    SplatGpu sp = SplatBuf(S.data).s[A.first + (v & ((1u << SPLAT_ACTOR_SHIFT) - 1u))];
    vec3 w = splat_world(A, sp.pos_op.xyz);
    // the covariance in the world: L S L^T (L: the actor's linear part)
    mat3 Sm = mat3(sp.cov0.x, sp.cov0.y, sp.cov0.z, sp.cov0.y, sp.cov0.w, sp.cov1.x, sp.cov0.z, sp.cov1.x, sp.cov1.y);
    mat3 L = transpose(mat3(A.rows[0].xyz, A.rows[1].xyz, A.rows[2].xyz));
    mat3 Sw = L * Sm * transpose(L);
    // the view's axes (screen x right, y down, z ahead) and the perspective's Jacobian at the center
    vec3 rel = w - f.camera_pos.xyz;
    vec3 vr = f.camera_right.xyz, vd = -f.camera_up.xyz, vf = f.camera_fwd.xyz;
    vec3 t = vec3(dot(rel, vr), dot(rel, vd), max(dot(rel, vf), 1.0));
    vec2 size = 1.0 / f.viewport.zw;
    float fx = size.x * 0.5 / f.camera_fwd.w, fy = fx;
    mat3 W = transpose(mat3(vr, vd, vf)); // (rows: the view's axes)
    mat3 J = mat3(fx / t.z, 0.0, 0.0, 0.0, fy / t.z, 0.0, -fx * t.x / (t.z * t.z), -fy * t.y / (t.z * t.z), 0.0);
    mat3 T = J * W;
    mat3 C = T * Sw * transpose(T);
    float a = C[0][0] + 0.3, b = C[0][1], c = C[1][1] + 0.3;
    float det = a * c - b * b;
    if (det <= 1e-12) { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); return; }
    o_conic = vec3(c, -b, a) / det;
    float mid = 0.5 * (a + c), l1 = mid + sqrt(max(mid * mid - det, 0.1));
    float rad = ceil(3.0 * sqrt(l1));
    const vec2 corner[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 d = corner[k] * rad;
    vec4 cc = f.view_proj * vec4(w, 1.0);
    gl_Position = vec4(cc.xy + d / size * 2.0 * cc.w, cc.z, cc.w);
    o_d = d;
    // its color: an albedo under the scene's light (the sky's and the sun's), or a light of its own
    uint rgba = floatBitsToUint(sp.cov1.z);
    vec3 col = vec3(float(rgba & 255u), float((rgba >> 8u) & 255u), float((rgba >> 16u) & 255u)) / 255.0;
    col = col * col; // (sRGB-ish -> linear)
    vec3 light;
    if (A.mode == 1u) light = vec3(A.look.x);
    else {
        light = f.sky_params.x > 0.0 ? sh_irradiance(f, vec3(0.0, 0.0, 1.0)) / PI : vec3(0.0);
        int si = int(f.sun_radiance.w);
        if (si >= 0) light += f.lights[si].radiance * max(-f.lights[si].direction.z, 0.0) / PI;
    }
    o_color = vec4(min(col * light * pre_exposure(f), vec3(60000.0)), sp.pos_op.w * A.look.y);
}
