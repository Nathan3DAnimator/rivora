#version 460
// The rain's drops and splashes (rain.vert): a thin streak brighter in the middle, lit by the sky and the sun behind
// it (drops shine against the light); a splash's crown (a ring of droplets thrown up). Blended over (premultiplied).
#include "common.glsl"
#include "water.glsl" // (w_vnoise, w_hash12)

layout(location = 0) in vec2 i_uv;
layout(location = 1) in float i_alpha;
layout(location = 2) flat in uint i_kind;
layout(location = 3) in float i_age;
layout(location = 4) in vec3 i_light; // (2026-11-15) the light it sends to the eye (rain.vert drop_light)
layout(location = 5) flat in vec3 i_len; // (2026-11-15) a stream: its length, width (cm), how much water
layout(location = 6) flat in vec4 i_rep;
layout(location = 7) in vec3 i_env;
layout(location = 0) out vec4 o_color;
// (2026-11-17, Q4: "gotas e filetes brancos") last frame's scene (the SSR's / the glass's copy, render.c: made every
// frame while it rains); water[85].y > 0.5: it is there at this size
layout(set = 0, binding = 82) uniform sampler2D u_scene_copy;

// (2026-11-17, Q4) clear water as the drops standing on the walls and the windows are shaded (water.glsl rain_glass: the
// water film's Fresnel and highlight over what is behind): what is behind it bent through a small lens (q: the place on
// the drop's cross-section, -1..1 on each axis it has; px: a drop's radius on screen in pixels along them, the lens
// inverting a few radii of the scene), the sky and the room by Fresnel at its rim (F0 0.02), the light it scatters
// forward (drop_light, minus the world's mean it put in) as a small highlight off its middle
vec3 drop_water(FrameBuf f, vec2 q, vec2 px_x, vec2 px_y, float glint) {
    float r2 = min(dot(q, q), 1.0), nv = sqrt(1.0 - r2);
    float F = 0.02 + 0.98 * pow(1.0 - nv, 5.0);
    vec2 off = -(px_x * q.x + px_y * q.y) * 2.2; // (pixels: the lens' inverted view, ~2 radii out at the rim)
    vec2 uv = clamp((gl_FragCoord.xy + off) * f.viewport.zw + i_rep.xy, vec2(0.0), vec2(1.0));
    vec3 behind = textureLod(u_scene_copy, uv, 0.0).rgb / max(pre_exposure(f), 1e-12);
    vec3 scatter = max(i_light - i_env, vec3(0.0)) * f.water[82].rgb;
    return behind * (1.0 - F) * vec3(0.94, 0.97, 0.98) + i_env * (F * 1.6) + scatter * (0.25 + glint);
}

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    float a;
    if (i_kind == 0u) {
        // a drop moving through the shutter: the same all along (a constant speed), soft across; the Stylized style a
        // crisp line (2026-11-15)
        a = (f.water[86].w > 1.5 ? 1.0 - i_uv.x * i_uv.x : exp(-i_uv.x * i_uv.x * 3.0)) * (1.0 - smoothstep(0.6, 1.0, abs(i_uv.y)));
        if (f.water[85].y > 0.5 && f.water[86].w <= 1.5) { // (Q4) clear water: the scene behind it, bent
            vec2 gx = vec2(dFdx(i_uv.x), dFdy(i_uv.x));
            float g = max(length(gx), 1e-4);
            vec2 dir = gx / g / g; // (pixels per unit of i_uv.x across: the streak's half width on screen)
            float x = clamp(i_uv.x, -1.0, 1.0);
            a = (1.0 - smoothstep(0.8, 1.0, abs(x))) * (1.0 - smoothstep(0.6, 1.0, abs(i_uv.y))) * i_alpha;
            if (a < 0.003) discard;
            float gl = exp(-(x - 0.3) * (x - 0.3) * 30.0);
            vec3 c = drop_water(f, vec2(x, 0.0), dir, vec2(0.0), 1.4 * gl);
            o_color = vec4(min(c * pre_exposure(f), vec3(60000.0)) * a, a);
            return;
        }
    } else if (i_kind == 4u) { // (2026-11-15) a sheet of water off an edge, breaking into streams as it falls
        float y = i_uv.y * -0.5 + 0.5, L = i_len.x, W = i_len.y, s = i_len.z, t = f.water[0].y;
        float z = y * L;                                   // cm fallen
        // (columns of 2.2 cm, or ~2 pixels when that is less: thinner lines flickered as the dynamic resolution changed)
        float ucm = (i_uv.x * 0.5 + 0.5) * W;
        float u = ucm / max(2.2, fwidth(ucm) * 2.5);
        float col = floor(u), fu = fract(u) - 0.5;
        float hc = w_hash12(vec2(col, i_age * 97.0));
        // the sheet holds near the edge and breaks into streams lower down; less water, more and sooner
        float whole = clamp(s * 0.5 - y * 0.9, 0.0, 1.0) * 0.6;
        float stream = hc < 0.2 + 0.45 * s ? 1.0 - smoothstep(0.1 + 0.15 * hc, 0.3, abs(fu + 0.12 * sin(z * 0.05 + hc * 9.0))) : 0.0;
        float body = max(stream, whole * (0.55 + 0.45 * w_vnoise(vec2(u * 0.7, z * 0.02))));
        // pulses running down with the fall's speed (v = sqrt(2 g z): their phase ~ the time to get there)
        float ph = sqrt(2.0 * max(z, 0.0) / 981.0) * 6.0 - t * 6.0 + hc * 13.0;
        float n = w_vnoise(vec2(col * 3.1 + 0.5, ph * 2.0)) * 0.6 + w_vnoise(vec2(u * 2.3, ph * 5.0 + z * 0.03)) * 0.4;
        float brk = smoothstep(mix(0.15, 0.55, y * (1.1 - 0.6 * s)), mix(0.25, 0.65, y), n); // (lower: drops and gaps)
        float ends = smoothstep(0.0, 0.03, y) * (1.0 - smoothstep(0.93, 1.0, y)) * (1.0 - smoothstep(0.85, 1.0, abs(i_uv.x)));
        a = body * mix(1.0, brk, 0.35 + 0.65 * y) * ends;
        a *= i_alpha;
        if (a < 0.003) discard;
        // water: the world refracted through it and the lights it catches, a brighter rim where it bends them
        float rim = 1.0 - abs(fu) * 2.0;
        if (f.water[85].y > 0.5 && f.water[86].w <= 1.5) { // (Q4) clear water: each stream a little cylinder bending what is behind
            vec2 gu = vec2(dFdx(u), dFdy(u));
            float g = max(length(gu), 1e-4);
            vec2 dir = gu / g / g * 0.5; // (a column's half width in pixels)
            float x = clamp(fu * 2.0, -1.0, 1.0);
            vec3 c = drop_water(f, vec2(x, 0.0), dir, vec2(0.0), 0.8 * exp(-(x - 0.3) * (x - 0.3) * 20.0) + 0.3 * n * n);
            a = min(a * 1.6, 1.0);
            o_color = vec4(min(c * pre_exposure(f), vec3(60000.0)) * a, a);
            return;
        }
        vec3 light = i_light * (0.35 + 1.1 * (1.0 - rim) + 0.5 * n * n) * f.water[82].rgb;
        a *= 0.45; // (clear water: mostly what is behind it, bent)
        o_color = vec4(min(light * pre_exposure(f), vec3(60000.0)) * a, a);
        return;
    } else if (i_kind == 2u) { // a snowflake: a soft round blob
        a = 1.0 - smoothstep(0.5, 1.0, length(i_uv));
    } else if (i_kind == 3u) { // (2026-11-10) a drip: a clear bead (falling: a teardrop), a rim darker where it bends the light, a glint
        vec2 q = i_uv;
        if (i_age > 0.5) q.y = q.y > 0.0 ? q.y * 0.8 : q.y * 1.15; // (round below, drawn out above while it falls)
        float r = length(q);
        if (r > 1.0) discard;
        float rim = smoothstep(0.72, 0.98, r);
        float glint = exp(-dot(q - vec2(-0.35, 0.4), q - vec2(-0.35, 0.4)) * 28.0);
        if (f.water[85].y > 0.5 && f.water[86].w <= 1.5) { // (Q4) a bead: a little ball lens, the scene behind it upside down
            a = (1.0 - smoothstep(0.92, 1.0, r)) * i_alpha;
            if (a < 0.003) discard;
            vec2 gx = vec2(dFdx(q.x), dFdy(q.x)), gy = vec2(dFdx(q.y), dFdy(q.y));
            float det = gx.x * gy.y - gx.y * gy.x; // (the pixels per unit of q on each axis: the inverse of q's Jacobian)
            vec2 px_x = abs(det) > 1e-8 ? vec2(gy.y, -gy.x) / det : vec2(0.0), px_y = abs(det) > 1e-8 ? vec2(-gx.y, gx.x) / det : vec2(0.0);
            vec3 c = drop_water(f, q, px_x, px_y, 3.0 * glint);
            o_color = vec4(min(c * pre_exposure(f), vec3(60000.0)) * a, a);
            return;
        }
        a = 0.55 + 0.45 * rim;
        a *= i_alpha;
        if (a < 0.003) discard;
        vec3 sky = f.sky_params.x > 0.0 ? sh_irradiance(f, vec3(0.0, 0.0, 1.0)) / PI : vec3(0.05);
        vec3 ground = f.sky_params.x > 0.0 ? sh_irradiance(f, vec3(0.0, 0.0, -1.0)) / PI : vec3(0.02);
        // (2026-11-15) the world it refracts and the lights it catches (rain.vert drop_light: a third under a roof), darker
        // at the rim, a glint (it was the sky x 6: white beads indoors)
        vec3 c = i_light * (mix(1.0, 0.45, rim) + 2.5 * glint);
        o_color = vec4(min(c * pre_exposure(f), vec3(60000.0)) * a, a);
        return;
    } else { // the crown: droplets around a rim, rising and falling
        float x = i_uv.x, y = i_uv.y * 0.5 + 0.5;
        float rim = exp(-pow((abs(x) - (0.3 + 0.6 * i_age)) * 6.0, 2.0));
        float up = exp(-pow((y - 0.4 * (1.0 - i_age) * (1.0 - abs(x))) * 5.0, 2.0));
        float dots = step(0.55, fract(sin(floor(x * 9.0) * 12.9898) * 43758.5453));
        a = (rim * up + 0.6 * exp(-y * 12.0) * rim) * mix(0.6, 1.0, dots);
    }
    a *= i_alpha;
    if (a < 0.003) discard;
    // (2026-11-15) the light rain.vert found for it: the world it refracts, the sun and the lamps it scatters forward
    // (Stylized: the bright lines of before); a flake: white ice scattering all around
    vec3 light = i_light;
    if (i_kind != 2u) light *= f.water[82].rgb; // (2026-11-14: the Rain's Drop Color)
    vec3 rgb = min(light * pre_exposure(f), vec3(60000.0));
    o_color = vec4(rgb * a, a);
}
