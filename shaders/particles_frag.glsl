// The particles' sprites' shading (particle.frag, particle_point.frag): a soft or hard disc, a square or a material's
// texture; lit (a sphere's normal in the sun and the sky: smoke, dust) or glowing (emissive: fire, sparks, magic);
// blended over or added (premultiplied alpha). Pre-exposed like the main pass. Returns alpha -1 to discard.
#ifndef PARTICLES_FRAG_GLSL
#define PARTICLES_FRAG_GLSL
#define MAT_SET 1
#include "mat_common.glsl"
// A sprite's pixel (particle.frag, particle_point.frag): uv in -1..1 over its quad, its color, its quad's axes
vec4 shade_sprite(vec2 i_uv, vec4 i_color, vec3 i_right, vec3 i_up, float frame) {
    FrameBuf f = FrameBuf(pc.frame);
    Emitter e = EmitterBuf(pc.instances).e;
    float r2 = dot(i_uv, i_uv);
    uint shape = uint(e.render_p.x);
    float alpha = i_color.a;
    vec3 base = i_color.rgb;
    if (shape == 0u) alpha *= pow(clamp(1.0 - r2, 0.0, 1.0), max(e.render_p.y, 0.05)); // soft disc
    else if (shape == 1u) { if (r2 > 1.0) return vec4(-1.0); } // hard disc
    else if (shape == 3u && e.render2.z > 0.5) { // a texture (a material's base color image)
        vec2 uv01 = i_uv * vec2(0.5, -0.5) + 0.5;
        uint ti = uint(e.render2.z) - 1u;
        vec4 t;
        if ((e.flags & PF_SUBUV) != 0u) { // a flipbook's cell (and the next one blended in)
            t = texture(u_mat_tex[nonuniformEXT(ti)], p_subuv_uv(e, uv01, frame));
            if ((e.flags & PF_SUBUV_BLEND) != 0u && fract(frame) > 0.01)
                t = mix(t, texture(u_mat_tex[nonuniformEXT(ti)], p_subuv_uv(e, uv01, frame + 1.0)), fract(frame));
        } else
            t = texture(u_mat_tex[nonuniformEXT(ti)], uv01);
        base *= t.rgb, alpha *= t.a;
    }
    if (shape != 1u && shape != 2u && r2 > 1.0 && shape != 3u) return vec4(-1.0);
    if (alpha < 0.002) return vec4(-1.0);
    vec3 rgb;
    if ((e.flags & PF_LIT) != 0u) { // a sphere's normal: lit from the side the sun is on, the sky all around
        vec3 n = normalize(i_right * i_uv.x + i_up * i_uv.y - f.camera_fwd.xyz * sqrt(max(1.0 - min(r2, 1.0), 0.0)));
        int sl = int(f.sun_radiance.w);
        vec3 light = f.sky_params.x > 0.0 ? sh_irradiance(f, n) / PI : vec3(0.0);
        if (sl >= 0) {
            GpuLight L = f.lights[sl];
            float wrap = max((dot(n, -L.direction) + 0.35) / 1.35, 0.0); // (light through the puff: a wrapped lambert)
            light += L.radiance * wrap / PI;
        }
        rgb = base * light + base * e.render_p.w;
    } else
        rgb = base * e.render_p.w; // glowing (nits)
    rgb = min(rgb * pre_exposure(f), vec3(60000.0));
    float a = alpha * e.render2.y;
    return (e.flags & PF_ADDITIVE) != 0u ? vec4(rgb * a, 0.0) : vec4(rgb * a, a);
}

#endif
