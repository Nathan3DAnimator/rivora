#version 460
// The rain's drops and splashes (water.c): no state, every one from a hash, 6 vertices each.
//   the drops (f.water[3].w of them): fixed in the world in a box 50 m wide and 20 m tall around the camera (snow: 26 x 14 m; the box
//   wraps: walking doesn't drag them), falling at their speed with the wind, as streaks as long as the shutter sees
//   them move; one under the rain's surface (a roof, a tree) is hidden
//   the splashes (the rest): a few hundred cycling places on the rain's surface near the camera, a crown a cycle; on
//   something moving (a walker, a swinging arm: water.glsl rain_mover) a drop flung off it along its motion instead
//   the drips (one drop id in four near the camera): a bead swelling at a wet edge, falling, a crown where it lands
#include "common.glsl"
#include "lights.glsl"
#include "water.glsl"

layout(location = 0) out vec2 o_uv;
layout(location = 1) out float o_alpha;
layout(location = 2) flat out uint o_kind; // 0 a drop, 1 a splash
layout(location = 3) out float o_age; // (a drip: 0 swelling, 1 falling)
layout(location = 4) out vec3 o_light; // (2026-11-15) the light the drop sends to the eye (drop_light), not pre-exposed
layout(location = 5) flat out vec3 o_len; // (2026-11-15) a stream off an edge: its length, width (cm), how much water
layout(location = 6) flat out vec4 o_rep; // (2026-11-17, Q4) last frame's screen minus this one's at the drop (uv), the scene copy's read
layout(location = 7) out vec3 o_env;      // (2026-11-17, Q4) the world it refracts, as drop_light took it (the copy replaces it)
vec3 g_at = vec3(0.0); // where the drop is (its light: main)
vec3 g_env = vec3(0.0);
// (2026-11-15, "com a resolução adaptativa as quedas d'água ficam flickando") the pixel's size by the render's height: the
// minimum widths were for 1440 lines; with a lower dynamic resolution a drop got under a pixel and shimmered
float g_pxs = 1.0;

float hg_ph(float g, float c) { float g2 = g * g; return (1.0 - g2) / (12.566371 * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5)); }
// (2026-11-15, "realista, tanto em tempestades quanto nas gotas") the light a falling drop sends to the eye (Garg and
// Nayar 2006): a drop refracts most of the world around it into a wide view (the sky above, the ground below, upside
// down: about the mean of both) and scatters a light strongly forward (Mie: a lobe toward where the light goes, a weak
// one back, the rainbow's side). Lit from behind, by the sun low ahead or a street lamp beyond it, it shines; from the
// camera's side it barely shows, and against the bright sky a drop is as bright as the sky: rain shows against the dark.
// The lamps from the light grid near the camera; x the Rain's Drop Light. The Stylized style keeps the bright lines.
vec3 drop_light(FrameBuf f, vec3 p, uint kind) {
    vec3 to_cam = normalize(f.camera_pos.xyz - p);
    vec3 sky = f.sky_params.x > 0.0 ? sh_irradiance(f, vec3(0.0, 0.0, 1.0)) / PI : vec3(0.05);
    vec3 ground = f.sky_params.x > 0.0 ? sh_irradiance(f, vec3(0.0, 0.0, -1.0)) / PI : vec3(0.02);
    int sl = int(f.sun_radiance.w);
    float clouds = (f.weather_flags & 4u) != 0u ? mix(1.0, 0.12, clamp(f.cloud0.x, 0.0, 1.0)) : 1.0;
    float flash = f.water[85].x, kl = f.water[89].x;
    bool sty = rain_stylized(f);
    if (kind == 2u || sty) { // a flake (white ice scattering all around), or the stylized drops' bright lines
        vec3 c = sky * (kind == 2u ? 0.9 : 2.6);
        if (sl >= 0) {
            GpuLight L = f.lights[sl];
            c += L.radiance * clouds * (kind == 2u ? 0.9 : 0.04 + 0.5 * pow(max(dot(normalize(f.camera_fwd.xyz), -L.direction), 0.0), 8.0)) / PI;
        }
        return c * (1.0 + 4.0 * flash);
    }
    // (2026-11-15, "fica muito branco e feio") under a roof a drop refracts the room, not the sky: a third of it, and no sun
    ivec2 pc_ = rain_cell(f, rain_u(f, p));
    float open = rain_holds(f, pc_) && p.z < rain_top(f, pc_) - 20.0 ? 0.0 : 1.0;
    vec3 c = (sky * 0.6 + ground * 0.4) * mix(0.3, 1.0, open);
    g_env = c;
    if (sl >= 0 && open > 0.0) {
        GpuLight L = f.lights[sl];
        float ct = dot(L.direction, to_cam);
        c += L.radiance * (clouds * kl * (0.85 * hg_ph(0.82, ct) + 0.15 * hg_ph(-0.35, ct)));
    }
    if (kl > 0.0 && distance(p, f.camera_pos.xyz) < 3500.0) { // the lamps near it
        LightLists ll = light_lists(f, p);
        uint taken = 0u;
        for (uint lv = ll.lp; lv < LG_LEVELS && taken < 12u; lv++) {
            uvec2 rg = light_range(ll, lv);
            for (uint k = 0u; k < rg.y && taken < 12u; k++) {
                uint it = light_item(f, rg, k);
                if (!light_take(it, lv, ll.lp)) continue;
                GpuLight L = local_light(f, it);
                vec3 lv3 = L.position - p;
                float d = length(lv3);
                vec3 l = lv3 / max(d, 1e-3);
                float w = clamp(1.0 - pow(d * d * L.inv_range2, 2.0), 0.0, 1.0);
                float dm = max(d * 0.01, max(L.source_radius * 0.01, 0.01));
                float atten = w * w / (dm * dm);
                if (light_kind(L) == LIGHT_SPOT) {
                    float cs = clamp((dot(-l, L.direction) - L.cos_outer) / max(L.cos_inner - L.cos_outer, 1e-4), 0.0, 1.0);
                    atten *= cs * cs;
                } else if (light_kind(L) == LIGHT_RECT) atten *= max(dot(-l, L.direction), 0.0);
                if (atten <= 0.0) continue;
                taken++;
                float ct = dot(-l, to_cam); // (the light goes from the lamp through the drop toward the eye)
                c += L.radiance * (atten * kl * (0.85 * hg_ph(0.82, ct) + 0.15 * hg_ph(-0.35, ct)));
            }
        }
    }
    return c + sky * (6.0 * flash); // (the lightning lights every drop at once)
}

// (2026-11-15) an edge holding water near the camera, from a seed: up to 3 cells, each with its 4 sides, for one 30 cm or
// more above the next (a roof's eave, a ledge, a step) with flat ground below it (with wind the slanted rain sees a
// wall's face as a ramp of cells stepping down: there the next cell keeps going down)
// (2026-11-15, "as goteiras ficam sumindo e aparecendo ao andar com a câmera") a place around the camera that stays put in
// the world: the 18 m window cut in 6 x 6 tiles of 3 m, each slot a tile (the one of its residue nearest the camera: as
// it walks, a tile left behind wraps to the side ahead, as the drops' box) and a spot in it hashed by the tile itself.
// Before, the spots were the camera's place plus a random offset: walking moved every drip to another edge
vec2 drip_spot(FrameBuf f, vec3 cam, uint slot, float k) {
    const float T = 300.0, M = 6.0;
    vec2 b = floor(rain_u(f, cam) / T) - M * 0.5;
    vec2 r = vec2(float(slot % 6u), float((slot / 6u) % 6u));
    vec2 tile = b + mod(r - b, M);
    return (tile + w_hash22(tile * 1.731 + vec2(k, float(slot / 36u) * 3.17))) * T;
}
// (2026-11-15, "os rios de goteiras tão sendo criados em locais sem sentido") an edge the water really leaves: 30 cm or more
// above the next cell (flat below it: with wind the slanted rain sees a wall's face as a ramp of cells stepping down), and
// the shallow water's flow out over it as much as min_q cells of rain (a roof's eave drains dozens; a wall's thin top, a
// step's nose, a box's rim only their own rain). q: that flow, in cells of rain
bool find_edge(FrameBuf f, vec3 cam, uint slot, float seed, float rot, float min_q, out ivec2 ce, out ivec2 sd, out float top, out float low, out float q) {
    const ivec2 side4[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
    ce = ivec2(0), sd = ivec2(0), top = 0.0, low = 0.0, q = 0.0;
    for (int k = 0; k < 3; k++) {
        ivec2 c = rain_cell(f, drip_spot(f, cam, slot, seed + float(k) * 1.37 + 5.3));
        if (!rain_holds(f, c)) continue;
        float tc = rain_surface(f, c).x;
        for (int j = 0; j < 4; j++) {
            ivec2 sj = side4[(j + int(rot * 4.0)) & 3];
            ivec2 nb = c + sj;
            if (!rain_holds(f, nb)) continue;
            float ln = rain_surface(f, nb).x;
            if (tc - ln <= 30.0 || tc - ln >= 3000.0) continue;
            ivec2 n2 = nb + sj;
            float ln2 = rain_holds(f, n2) ? rain_surface(f, n2).x : ln;
            if (abs(ln - ln2) > 0.35 * (tc - ln)) continue;
            float qj = rain_outflow(f, c, sj);
            if (qj < min_q) continue;
            ce = c, sd = sj, top = tc, low = ln, q = qj;
            return true;
        }
    }
    return false;
}

void place();
void main() {
    place();
    FrameBuf f = FrameBuf(pc.frame);
    o_light = o_alpha > 0.0 ? drop_light(f, g_at, o_kind) : vec3(0.0);
    o_env = g_env;
    o_rep = vec4(0.0);
    if (o_alpha > 0.0 && f.water[85].y > 0.5) { // (Q4: where the scene behind it was last frame, for the copy)
        vec4 c0 = f.view_proj * vec4(g_at, 1.0), c1 = f.cloud_prev_vp * vec4(g_at, 1.0);
        if (c0.w > 1e-3 && c1.w > 1e-3) o_rep.xy = (c1.xy / c1.w - c0.xy / c0.w) * 0.5;
    }
    o_alpha *= f.water[89].y; // (the Rain's Drop Opacity)
}

void place() {
    FrameBuf f = FrameBuf(pc.frame);
    g_pxs = max(1.0, 1440.0 * f.viewport.w);
    uint vi = uint(gl_VertexIndex), id = vi / 6u, k = vi % 6u;
    const vec2 corner[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 cn = corner[k];
    uint ndrops = uint(f.water[3].w);
    float t = f.water[0].y;
    vec3 cam = f.camera_pos.xyz;
    vec2 wind = f.water[84].xy; // cm/s (2026-11-14: with the Rain's Gusts: water.c; the water on the ground keeps the mean wind)
    vec2 drift = f.water[84].zw; // (the way the gusting wind carried the drops so far: their place moves smoothly)
    // (2026-11-14) the far box: half the drops on the ground where the camera looks, from high up (water.c)
    float far_amt = f.water[83].z;
    vec4 farb = f.water[81];
    gl_Position = vec4(0.0, 0.0, -2.0, 1.0); // (hidden unless placed)
    o_uv = cn, o_alpha = 0.0, o_kind = 0u, o_age = 0.0, o_len = vec3(0.0);
    if (id < ndrops) {
        vec4 h = vec4(w_hash22(vec2(float(id), 1.7)), w_hash22(vec2(float(id), 9.3)));
        bool snow = f.water[39].x > 0.5;
        // (2026-11-16, "pesquise como realmente as chuvas funcionam") a drop's size from the Marshall-Palmer spectrum of this
        // rain rate (N(D) = N0 e^(-L D), L = 4.1 R^-0.21 per mm: many small drops, a few big; the 0.5..6 mm ones seen),
        // scaled so its median is the Rain's Drop Size; its speed the terminal one of that size (Atlas et al. 1973 on Gunn
        // and Kinzer: 9.65 - 10.3 e^(-0.6 D) m/s): small drops fall slow and lean far in the wind, big ones fast and steep
        float size, speed; // mm, cm/s
        if (snow) size = f.water[39].y * (0.5 + h.w), speed = (60.0 + 40.0 * h.w) * max(f.water[56].z, 0.25); // (a flake floats)
        else {
            float L = 4.1 * pow(max(f.water[0].x, 0.1), -0.21), e = exp(-L * 5.5);
            float dm = 0.5 - log(1.0 - 0.5 * (1.0 - e)) / L; // (the seen spectrum's median)
            size = (0.5 - log(1.0 - h.w * (1.0 - e)) / L) * f.water[3].y / dm;
            speed = max(9.65 - 10.3 * exp(-0.6 * size), 0.8) * 100.0 * max(f.water[56].z, 0.25); // (x the Fall Speed)
        }
        float S = snow ? 2600.0 : 5000.0, H = snow ? 1400.0 : 2000.0; // (snow: a smaller box, the flakes dense near the eye)
        bool far = !snow && far_amt > 0.0 && (id & 1u) == 1u;
        vec2 bc = cam.xy;
        if (far) S = farb.w, bc = farb.xy;
        float fall = t * speed + h.z * H;
        vec3 p;
        p.xy = h.xy * S + drift + wind * (h.z * H / speed); // (carried by the wind as it falls: the way so far, then its own)
        p.xy = bc - S * 0.5 + mod(p.xy - (bc - S * 0.5), vec2(S));
        p.z = far ? farb.z + H - mod(fall, H) : cam.z + H * 0.5 - mod(fall, H);
        if (snow) { // a flake flutters side to side as it falls, and is drawn round, facing the camera
            float ph = h.z * 40.0 + t * (1.3 + h.x);
            p.xy += vec2(sin(ph), cos(ph * 0.8)) * (8.0 + 10.0 * h.y);
            ivec2 cs = rain_cell(f, rain_u(f, p)); // (the slanted grid: water.glsl rain_u)
            if (rain_holds(f, cs) && p.z < rain_top(f, cs)) return;
            vec3 to_c = normalize(cam - p);
            vec3 sr = normalize(cross(vec3(0.0, 0.0, 1.0), to_c) + vec3(1e-5)), su = cross(to_c, sr);
            float dist = length(cam - p);
            float rad = max(size * 0.08, dist * 0.0018); // (at least ~2 px across)
            g_at = p;
            gl_Position = f.view_proj * vec4(p + (sr * cn.x + su * cn.y) * rad, 1.0);
            o_alpha = f.water[3].x * clamp(size * 0.12 / rad, 0.6, 1.0) * (1.0 - smoothstep(900.0, 1300.0, dist)) * smoothstep(20.0, 60.0, dist);
            o_kind = 2u;
            return;
        }
        // (2026-11-15, "as goteiras não estavam nem um pouco realistas, eram apenas uns pontos brancos caindo; goteiras
        // realistas tipo as de beiras de casa numa tempestade que ficam caindo como se fosse um mini rio") in a heavy rain
        // an edge holding water lets it go as a sheet along the eave, breaking into streams (rain.frag kind 4): one drop id
        // in 64 is a stream, held at its edge for 20 s (fading in and out), as wide and as whole as the water there and
        // the rain's rate make it, falling in an arc off the edge and thinning as it speeds up; three more splash at its
        // foot, crown after crown. A lighter rain keeps the drips below (beads swelling and falling)
        float storm = clamp((f.water[0].x - 12.0) / 48.0, 0.0, 1.0) * f.water[2].x;
        uint sub = id & 63u;
        if ((sub == 0u || sub == 2u || sub == 4u || sub == 6u) && storm > 0.0 && !far) {
            bool foot = sub != 0u;
            float sid = float(id - sub);
            vec4 hs = vec4(w_hash22(vec2(sid, 1.7)), w_hash22(vec2(sid, 9.3)));
            float ep = t / 20.0 + hs.y, epoch = floor(ep), live = smoothstep(0.0, 0.06, fract(ep)) * (1.0 - smoothstep(0.94, 1.0, fract(ep)));
            ivec2 ce, sd;
            float top, low;
            float qe;
            if (live > 0.0 && find_edge(f, cam, uint(sid) / 64u, epoch * 17.3, hs.z, 4.0, ce, sd, top, low, qe)) {
                float s = storm * clamp(qe / 20.0, 0.0, 1.5); // (how much water comes off: 20 cells of roof, a full sheet)
                vec2 along = vec2(-sd.y, sd.x), out_ = vec2(sd);
                float W = (6.0 + 22.0 * s) * (0.6 + 0.8 * hs.w); // cm along the edge
                vec2 e = (vec2(ce) + 0.5 + out_ * 0.5) * f.water[1].x; // (the cell's outer side, in the slanted grid)
                vec3 a = vec3(e, top - 1.0);
                a.xy -= rain_slant(f) * a.z;
                float drop = top - low, T = sqrt(2.0 * drop / 981.0), v0 = 12.0 + 22.0 * s; // (s: the fall; cm/s off the edge)
                vec3 b = vec3(a.xy + out_ * v0 * T, low);
                float dist = length(cam - 0.5 * (a + b));
                float fade = live * (1.0 - smoothstep(1300.0, 1800.0, dist)) * smoothstep(15.0, 50.0, dist);
                if (fade <= 0.0) return;
                if (foot) { // the splash at its foot: a crown somewhere along it, again and again
                    float cyc = t * 5.0 + hs.x * 7.0 + float(sub) * 0.37, ag = fract(cyc);
                    vec2 r2 = w_hash22(vec2(sid + float(sub) * 0.13, floor(cyc)));
                    vec3 c = b + vec3(along * (r2.x - 0.5) * W * 0.9, 0.0);
                    vec3 to_c = normalize(cam - c);
                    vec3 side = normalize(vec3(-to_c.y, to_c.x, 0.0) + vec3(1e-4, 0.0, 0.0));
                    float sz = (5.0 + 10.0 * s) * (0.4 + 0.6 * ag) * (0.7 + 0.6 * r2.y) * max(1.0, dist * g_pxs * 0.0007 * 2.0);
                    g_at = c;
                    gl_Position = f.view_proj * vec4(c + side * cn.x * sz + vec3(0.0, 0.0, (cn.y * 0.5 + 0.5) * sz * 0.9), 1.0);
                    o_alpha = fade * (1.0 - ag) * 1.3, o_kind = 1u, o_age = ag;
                    return;
                }
                // the sheet: a ribbon in the vertical plane along the edge, from the edge down to where it lands (its
                // middle bowed out: the arc off the edge); the column's two ends (cn.y: 1 the top)
                float y01 = cn.y * -0.5 + 0.5; // (0 at the top, 1 at the foot)
                vec3 pnt = mix(a, b, y01);
                pnt.xy += out_ * v0 * T * (sqrt(y01) - y01) * 0.8; // (it leaves the edge fast and falls ever steeper)
                float wid = W * mix(1.0, 0.75, y01) * 0.5;          // (narrower as it speeds up)
                float px = dist * g_pxs * 0.0009;
                wid = max(wid, px * 2.0);
                g_at = 0.5 * (a + b);
                // (2026-11-15, "sumindo ao mudar o ângulo da câmera") seen along the edge the sheet was a line and vanished:
                // it turns to face the camera as it gets edge-on (then a column of falling water, as it looks)
                vec3 sa = vec3(along, 0.0), to_c = normalize(cam - pnt);
                vec3 sc = normalize(cross(vec3(0.0, 0.0, 1.0), to_c) + vec3(1e-5));
                vec3 sw = normalize(mix(sa, sc * (dot(sa, sc) < 0.0 ? -1.0 : 1.0), smoothstep(0.45, 0.9, abs(dot(sa, to_c)))));
                gl_Position = f.view_proj * vec4(pnt + sw * cn.x * wid, 1.0);
                o_alpha = fade, o_kind = 4u, o_age = hs.x;
                o_len = vec3(drop, W, s);
                return;
            }
        } // (no stream for this id: a drip or an ordinary drop, below)
        // (2026-11-15) the drops seen falling off a wet actor under a roof were taken away ("o escoamento na parte de baixo
        // do modelo fica muito forte e feio, pode tirar ele"): its spots on the floor stay (water.c, the wet marks)
        // (2026-11-10, "a água escorre e forma goteiras"; "não consegui visualizar as goteiras") one drop in four near the
        // camera may be a drip: up to 3 random cells (the drop's own each cycle), each with its 4 sides, for an edge
        // holding water (its surface 30 cm or more above the next one: a roof, a ledge, a step, a branch). There a bead
        // swells under the edge, lets go, falls stretching, and a crown splashes where it lands; again and again
        if ((id & 3u) == 0u && f.water[2].x > 0.0 && !far) {
            float period = 1.1 + 1.1 * h.x;
            float cyc = floor(t / period + h.y), ph = fract(t / period + h.y);
            ivec2 ce, sd;
            float top, low;
            float qb;
            bool found = find_edge(f, cam, id / 4u, cyc * 3.0, h.z, 1.2, ce, sd, top, low, qb); // (an eave the water leaves)
            if (found) {
                float drop = top - low, T = sqrt(2.0 * drop / 981.0); // (s: the fall)
                float swell = max(period - T - 0.18, 0.2); // (the bead grows the rest of the cycle; the crown 0.18 s)
                float tc = ph * period;
                vec2 e = (vec2(ce) + 0.5 + vec2(sd) * 0.5 + vec2(sd.y, sd.x) * (w_hash12(vec2(float(id), cyc)) - 0.5) * 0.8) * f.water[1].x;
                vec3 dp = vec3(e, top - 1.5);
                dp.xy -= rain_slant(f) * dp.z; // (from the slanted grid to the world)
                float dist = length(cam - dp);
                float fade = (1.0 - smoothstep(1100.0, 1500.0, dist)) * smoothstep(15.0, 50.0, dist);
                vec3 to_c = normalize(cam - dp);
                vec3 sr = normalize(cross(vec3(0.0, 0.0, 1.0), to_c) + vec3(1e-5)), su = cross(to_c, sr);
                float px = dist * g_pxs * 0.0007; // (~a pixel at this distance: a drip is drawn at least ~3 px across, or nobody sees it)
                if (tc < swell) { // the bead hanging under the edge, swelling
                    float g = tc / swell, rad = max(0.2 + 0.2 * g, px * (1.6 + 1.4 * g));
                    vec3 c = dp - vec3(0.0, 0.0, rad * (0.6 + 0.6 * g)); // (it sags as it fills)
                    g_at = c;
                    gl_Position = f.view_proj * vec4(c + sr * cn.x * rad + su * cn.y * rad * (1.0 + 0.5 * g), 1.0);
                    o_alpha = fade, o_kind = 3u, o_age = 0.0;
                    return;
                }
                float tf = tc - swell;
                if (tf < T) { // falling, stretching with its speed
                    float vz = 981.0 * tf;
                    vec3 c = dp - vec3(0.0, 0.0, 0.5 * 981.0 * tf * tf + 0.6);
                    float rad = max(0.35, px * 2.6), ln = max(rad * 2.4, vz * 0.035);
                    g_at = c;
                    gl_Position = f.view_proj * vec4(c + sr * cn.x * rad + vec3(0.0, 0.0, cn.y * 0.5 * ln), 1.0);
                    o_alpha = fade, o_kind = 3u, o_age = 1.0;
                    return;
                }
                float ag = (tf - T) / 0.18; // its crown where it lands
                if (ag < 1.0) {
                    vec3 c = vec3(dp.xy, low);
                    vec3 side = normalize(vec3(-to_c.y, to_c.x, 0.0) + vec3(1e-4, 0.0, 0.0));
                    float sz = (2.0 + 5.0 * ag) * max(1.0, px * 2.0);
                    g_at = c;
                    gl_Position = f.view_proj * vec4(c + side * cn.x * sz + vec3(0.0, 0.0, (cn.y * 0.5 + 0.5) * sz * 0.8), 1.0);
                    o_alpha = fade * (1.0 - ag) * 1.4, o_kind = 1u, o_age = ag;
                }
                return;
            }
        }
        // under the rain's first surface: hidden
        ivec2 c = rain_cell(f, rain_u(f, p));
        if (rain_holds(f, c) && p.z < rain_top(f, c)) return;
        vec3 vel = vec3(wind, -speed);
        // (2026-11-16) the streak an eye sees: ~1/35 s of the fall (10-25 cm by the drop's speed; 1/22 s drew every drop as a
        // long line; the physical 1/60 s read as dust with the drops a game can draw); x Streak Length
        float len = speed * 0.0286 * f.water[82].w;
        vec3 d = normalize(vel);
        vec3 to_cam = normalize(cam - p);
        vec3 side = normalize(cross(d, to_cam) + vec3(1e-5));
        float dist = length(cam - p);
        float width = max(size * 0.05 * f.water[83].x, dist * g_pxs * 0.0009); // cm (at least ~1 px wide far away: thinner drops fade instead; x Drop Width)
        if (far) len = max(len, dist * 0.012); // (seen from high up: still a streak, a dozen pixels)
        vec3 wp = p + d * (cn.y * 0.5 * len) + side * (cn.x * width);
        g_at = wp;
        gl_Position = f.view_proj * vec4(wp, 1.0);
        float fade = far ? far_amt * (1.0 - smoothstep(0.38, 0.5, max(abs(p.x - bc.x), abs(p.y - bc.y)) / S)) * smoothstep(1500.0, 3000.0, dist)
                         : (1.0 - smoothstep(1800.0, 2500.0, dist)) * smoothstep(40.0, 120.0, dist);
        // (2026-11-16) a drop crossing the shutter covers each point of its streak for D / length of the time: a moving drop
        // is nearly clear (1-2 %), real rain shows by its numbers and the light in it. That share, boosted for the drops a
        // game draws (a big drop more than a small one), thinned when it is drawn wider than it is
        float cover = clamp(size * 0.1 / max(len, 1.0) * 30.0, 0.08, 0.85);
        o_alpha = f.water[3].x * cover * clamp(size * 0.1 * f.water[83].x / width, far ? 0.35 : 0.55, 1.0) * fade;
        o_kind = 0u;
        return;
    }
    // a splash: a place and a time from its cycle
    uint s = id - ndrops;
    float period = 0.35;
    float ph = w_hash12(vec2(float(s), 3.1));
    float cyc = floor(t / period + ph);
    float age = fract(t / period + ph);
    vec2 r = w_hash22(vec2(float(s), cyc));
    // (2026-11-14) from high up the splashes go where the camera looks (the far box), half of them
    bool far = far_amt > 0.0 && (s & 1u) == 1u;
    float R = far ? max(1800.0, farb.w * 0.5) : 1800.0;
    vec2 xy = (far ? rain_u(f, farb.xyz) : rain_u(f, cam)) - R + r * 2.0 * R; // (a place in the slanted grid)
    ivec2 c = rain_cell(f, xy);
    vec4 sm, mv = vec4(0.0);
    bool held = rain_holds(f, c);
    if (held) sm = rain_surface(f, c), mv = rain_mover(f, c);
    else if (far) sm = vec4(farb.z); // (past the map: the open ground, flat at the ground's height)
    else return;
    bool on_mover = held && mv.w > sm.x + 1.0;
    float z = on_mover ? mv.w : sm.x;
    if (abs(z - (far ? farb.z : cam.z)) > (far ? 3000.0 : 2000.0)) return;
    vec3 p = vec3(xy - rain_slant(f) * z, z); // (back to the world)
    float dist = length(cam - p);
    if (on_mover && dot(mv.xyz, mv.xyz) > 900.0) { // (2026-11-10) off something moving: a drop flung along its motion (and up a little), falling as it flies
        vec2 rk = w_hash22(vec2(float(s), cyc + 7.7));
        float tf = age * period;
        vec3 v0 = mv.xyz * (0.75 + 0.5 * rk.x) + vec3(0.0, 0.0, 40.0 + 90.0 * rk.y);
        vec3 c = p + v0 * tf + vec3(0.0, 0.0, -0.5 * 981.0 * tf * tf);
        vec3 vel = v0 + vec3(0.0, 0.0, -981.0 * tf);
        float sp = length(vel);
        vec3 d = vel / max(sp, 1e-3);
        vec3 side = normalize(cross(d, normalize(cam - c)) + vec3(1e-5));
        float width = max(0.12, dist * g_pxs * 0.0008), len = max(width * 2.0, sp * 0.03);
        g_at = c;
        gl_Position = f.view_proj * vec4(c + d * (cn.y * 0.5 * len) + side * (cn.x * width), 1.0);
        o_alpha = f.water[3].z * (1.0 - age) * (1.0 - smoothstep(1000.0, 1800.0, dist)) * smoothstep(20.0, 60.0, dist) * 1.3;
        o_kind = 0u;
        return;
    }
    float size = (2.0 + 6.0 * age) * (0.6 + f.water[3].y * 0.2) * f.water[83].y; // cm: the crown grows (x Splash Size)
    if (far) size = max(size, dist * 0.0025 * (0.5 + age)); // (a couple of pixels at least, seen from high up)
    vec3 to_cam = cam - p;
    vec3 side = normalize(vec3(-to_cam.y, to_cam.x, 0.0) + vec3(1e-4, 0.0, 0.0));
    vec3 wp = p + side * cn.x * size + vec3(0.0, 0.0, (cn.y * 0.5 + 0.5) * size * 0.8);
    g_at = wp;
    gl_Position = f.view_proj * vec4(wp, 1.0);
    o_alpha = f.water[3].z * (1.0 - age) * (far ? far_amt * (1.0 - smoothstep(R * 0.8, R, length(p.xy - farb.xy))) * (1.0 - smoothstep(farb.w * 2.0, farb.w * 3.0, dist))
                                                : 1.0 - smoothstep(1000.0, 1800.0, dist));
    o_kind = 1u;
    o_age = age;
}
