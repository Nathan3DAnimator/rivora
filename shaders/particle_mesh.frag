#version 460
// The particles' meshes (particle_mesh.vert): lit like the scene's meshes in the sun (its virtual shadow maps when the
// emitter receives shadows) and the sky, a GGX highlight; the emitter's glow added. Pre-exposed like the main pass.
#include "common.glsl"
#include "particles.glsl"
#include "vsm.glsl" // the sun's virtual shadow maps (bindings 0, 51)

layout(location = 0) in vec3 i_world;
layout(location = 1) in vec3 i_normal;
layout(location = 2) in vec4 i_color;
layout(location = 0) out vec4 o_color;

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    Emitter e = EmitterBuf(pc.instances).e;
    vec3 n = normalize(i_normal), v = normalize(f.camera_pos.xyz - i_world);
    if (dot(n, v) < 0.0) n = -n;
    vec3 base = i_color.rgb;
    float rough = clamp(e.render2.w, 0.05, 1.0), metal = e.mesh_color.w;
    vec3 diff = base * (1.0 - metal), f0 = mix(vec3(0.04), base, metal);
    vec3 color = f.sky_params.x > 0.0 ? diff * sh_irradiance(f, n) / PI : vec3(0.0);
    int sl = int(f.sun_radiance.w);
    if (sl >= 0) {
        GpuLight L = f.lights[sl];
        vec3 l = -L.direction;
        float nl = dot(n, l);
        if (nl > 0.0) {
            float vis = (e.flags & PF_SHADOWS) != 0u && (f.gi_flags & GI_VSM) != 0u ? vsm_shadow(f, i_world, n) : 1.0;
            vec3 h = normalize(l + v);
            float a2 = rough * rough * rough * rough, nh = max(dot(n, h), 0.0);
            float d = a2 / (PI * pow(nh * nh * (a2 - 1.0) + 1.0, 2.0));
            vec3 fr = f0 + (1.0 - f0) * pow(1.0 - max(dot(h, v), 0.0), 5.0);
            color += (diff / PI + fr * d * 0.25) * L.radiance * nl * vis;
        }
    }
    color += base * e.render_p.w;
    o_color = vec4(min(color * pre_exposure(f), vec3(60000.0)), 1.0);
}
