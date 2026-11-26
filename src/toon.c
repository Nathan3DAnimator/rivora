// (Q11) The Toon shading model's and the outline's values (toon.h, notas-dev/toon.md)
#include "toon.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "io_json.h"

static void set3(float d[3], float a, float b, float c) { d[0] = a, d[1] = b, d[2] = c; }

void toon_defaults(MatToon* t) {
    memset(t, 0, sizeof *t);
    t->bands = 2, t->softness = 0.08f, t->shadow_threshold = 0.5f;
    set3(t->shadow_color, 0.32f, 0.30f, 0.42f); // (a cool shadow: the cel look's)
    set3(t->recv_color, 0.32f, 0.30f, 0.42f);
    t->nstops = 2;
    t->stop_pos[0] = 0.0f, t->stop_pos[1] = 1.0f, t->stop_pos[2] = 1.0f, t->stop_pos[3] = 1.0f;
    set3(t->stop_col[0], 0.0f, 0.0f, 0.0f);
    for (int k = 1; k < TOON_STOPS; k++) set3(t->stop_col[k], 1.0f, 1.0f, 1.0f);
    set3(t->spec_color, 1.0f, 1.0f, 1.0f), t->spec_strength = 0.6f, t->spec_size = 0.2f, t->spec_hardness = 0.85f;
    set3(t->rim_color, 1.0f, 1.0f, 1.0f), t->rim_strength = 0.0f, t->rim_width = 0.3f, t->rim_softness = 0.1f;
    set3(t->hatch_color, 0.08f, 0.07f, 0.1f), t->hatch_strength = 0.0f, t->hatch_scale = 7.0f, t->hatch_angle = 45.0f, t->hatch_threshold = 0.45f;
    t->ambient = 0.6f; // (the bands read over a flatter indirect light)
    t->outline = false;
    set3(t->outline_color, 0.02f, 0.02f, 0.025f), t->outline_width = 3.0f;
    t->adaptive = true, t->near_dist = 1.0f, t->far_dist = 40.0f, t->near_scale = 0.6f, t->far_scale = 1.6f, t->curve_exp = 1.0f;
    t->depth_sens = 0.04f, t->crease_deg = 0.0f;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void read3(const JsonValue* o, const char* key, float d[3]) {
    float v[3] = { d[0], d[1], d[2] };
    if (json_get_floats(o, key, v, 3) == 3) memcpy(d, v, sizeof v);
}

void toon_read(MatToon* t, const JsonValue* root) {
    toon_defaults(t);
    const JsonValue* o = json_get(root, "toon");
    if (o) {
        t->bands = json_get_int(o, "bands", t->bands);
        t->softness = (float)json_get_num(o, "softness", t->softness);
        t->shadow_threshold = (float)json_get_num(o, "shadow_threshold", t->shadow_threshold);
        read3(o, "shadow_color", t->shadow_color);
        read3(o, "receive_color", t->recv_color);
        const JsonValue* st = json_get(o, "ramp");
        if (st && json_len(st)) {
            t->nstops = 0;
            for (size_t i = 0; i < json_len(st) && t->nstops < TOON_STOPS; i++) {
                float v[4] = { 0, 1, 1, 1 };
                json_get_floats(json_at(st, i), "v", v, 4);
                t->stop_pos[t->nstops] = v[0];
                memcpy(t->stop_col[t->nstops], v + 1, 12);
                t->nstops++;
            }
            if (!t->nstops) t->nstops = 1;
        }
        snprintf(t->ramp_image, sizeof t->ramp_image, "%s", json_get_str(o, "ramp_image", ""));
        read3(o, "specular_color", t->spec_color);
        t->spec_strength = (float)json_get_num(o, "specular", t->spec_strength);
        t->spec_size = (float)json_get_num(o, "specular_size", t->spec_size);
        t->spec_hardness = (float)json_get_num(o, "specular_hardness", t->spec_hardness);
        read3(o, "rim_color", t->rim_color);
        t->rim_strength = (float)json_get_num(o, "rim", t->rim_strength);
        t->rim_width = (float)json_get_num(o, "rim_width", t->rim_width);
        t->rim_softness = (float)json_get_num(o, "rim_softness", t->rim_softness);
        read3(o, "hatch_color", t->hatch_color);
        t->hatch_strength = (float)json_get_num(o, "hatching", t->hatch_strength);
        t->hatch_scale = (float)json_get_num(o, "hatch_scale", t->hatch_scale);
        t->hatch_angle = (float)json_get_num(o, "hatch_angle", t->hatch_angle);
        t->hatch_threshold = (float)json_get_num(o, "hatch_threshold", t->hatch_threshold);
        t->ambient = (float)json_get_num(o, "ambient", t->ambient);
    }
    const JsonValue* ol = json_get(root, "outline");
    if (ol) {
        t->outline = json_get_bool(ol, "on", true);
        read3(ol, "color", t->outline_color);
        t->outline_width = (float)json_get_num(ol, "width", t->outline_width);
        t->adaptive = json_get_bool(ol, "adaptive", t->adaptive);
        t->near_dist = (float)json_get_num(ol, "near", t->near_dist);
        t->far_dist = (float)json_get_num(ol, "far", t->far_dist);
        t->near_scale = (float)json_get_num(ol, "near_scale", t->near_scale);
        t->far_scale = (float)json_get_num(ol, "far_scale", t->far_scale);
        t->curve_exp = (float)json_get_num(ol, "curve", t->curve_exp);
        t->depth_sens = (float)json_get_num(ol, "depth_sensitivity", t->depth_sens);
        t->crease_deg = (float)json_get_num(ol, "crease", t->crease_deg);
    }
}

void toon_write(JsonWriter* w, const MatToon* t, bool shading_toon) {
    if (shading_toon) {
        jw_key(w, "toon");
        jw_obj_begin(w);
        jw_kint(w, "bands", t->bands);
        jw_kfloat(w, "softness", t->softness);
        jw_kfloat(w, "shadow_threshold", t->shadow_threshold);
        jw_kfloats(w, "shadow_color", t->shadow_color, 3);
        jw_kfloats(w, "receive_color", t->recv_color, 3);
        jw_key(w, "ramp");
        jw_arr_begin(w);
        for (int i = 0; i < t->nstops && i < TOON_STOPS; i++) {
            float v[4] = { t->stop_pos[i], t->stop_col[i][0], t->stop_col[i][1], t->stop_col[i][2] };
            jw_obj_begin(w);
            jw_kfloats(w, "v", v, 4);
            jw_obj_end(w);
        }
        jw_arr_end(w);
        if (t->ramp_image[0]) jw_kstr(w, "ramp_image", t->ramp_image);
        jw_kfloats(w, "specular_color", t->spec_color, 3);
        jw_kfloat(w, "specular", t->spec_strength);
        jw_kfloat(w, "specular_size", t->spec_size);
        jw_kfloat(w, "specular_hardness", t->spec_hardness);
        jw_kfloats(w, "rim_color", t->rim_color, 3);
        jw_kfloat(w, "rim", t->rim_strength);
        jw_kfloat(w, "rim_width", t->rim_width);
        jw_kfloat(w, "rim_softness", t->rim_softness);
        jw_kfloats(w, "hatch_color", t->hatch_color, 3);
        jw_kfloat(w, "hatching", t->hatch_strength);
        jw_kfloat(w, "hatch_scale", t->hatch_scale);
        jw_kfloat(w, "hatch_angle", t->hatch_angle);
        jw_kfloat(w, "hatch_threshold", t->hatch_threshold);
        jw_kfloat(w, "ambient", t->ambient);
        jw_obj_end(w);
    }
    if (t->outline) {
        jw_key(w, "outline");
        jw_obj_begin(w);
        jw_kbool(w, "on", true);
        jw_kfloats(w, "color", t->outline_color, 3);
        jw_kfloat(w, "width", t->outline_width);
        jw_kbool(w, "adaptive", t->adaptive);
        jw_kfloat(w, "near", t->near_dist);
        jw_kfloat(w, "far", t->far_dist);
        jw_kfloat(w, "near_scale", t->near_scale);
        jw_kfloat(w, "far_scale", t->far_scale);
        jw_kfloat(w, "curve", t->curve_exp);
        jw_kfloat(w, "depth_sensitivity", t->depth_sens);
        jw_kfloat(w, "crease", t->crease_deg);
        jw_obj_end(w);
    }
}

void toon_pack(const MatToon* t, uint32_t ramp_tex, float o[12][4]) {
    memset(o, 0, sizeof(float) * 48);
    for (int k = 0; k < 3; k++) {
        o[0][k] = clampf(t->shadow_color[k], 0.0f, 1.0f);
        o[1][k] = clampf(t->recv_color[k], 0.0f, 1.0f);
        o[2][k] = fmaxf(t->spec_color[k], 0.0f) * fmaxf(t->spec_strength, 0.0f);
        o[3][k] = fmaxf(t->rim_color[k], 0.0f) * fmaxf(t->rim_strength, 0.0f);
        o[4][k] = clampf(t->hatch_color[k], 0.0f, 1.0f);
    }
    // (2026-11-25, item 20: a number typed past the slider reaches the GPU) the bands: toon.glsl only counts with them;
    // 255 a safe top (the Details' 0..8 is the slider's). Hatch Spacing only kept off 0 (a division), the ambient off
    // the negatives
    o[0][3] = (float)(t->bands < 0 ? 0 : t->bands > TOON_BANDS_MAX ? TOON_BANDS_MAX : t->bands);
    o[1][3] = clampf(t->softness, 0.0f, 1.0f);
    o[2][3] = clampf(t->spec_size, 0.0f, 1.0f);
    o[3][3] = clampf(t->rim_width, 0.0f, 1.0f);
    o[4][3] = clampf(t->hatch_strength, 0.0f, 1.0f);
    o[5][0] = fmaxf(t->hatch_scale, 0.5f);
    o[5][1] = t->hatch_angle * 0.017453292f;
    o[5][2] = clampf(t->hatch_threshold, 0.0f, 1.0f);
    o[5][3] = clampf(t->spec_hardness, 0.0f, 1.0f);
    o[6][0] = clampf(t->rim_softness, 0.0f, 1.0f);
    o[6][1] = fmaxf(t->ambient, 0.0f);
    o[6][2] = (float)ramp_tex; // (a float value, as e6's slot: small integers' bits are denormals)
    int n = t->nstops < 1 ? 1 : t->nstops > TOON_STOPS ? TOON_STOPS : t->nstops;
    o[6][3] = (float)n;
    float last = 0.0f;
    for (int i = 0; i < TOON_STOPS; i++) {
        int s = i < n ? i : n - 1;
        float p = clampf(t->stop_pos[s], 0.0f, 1.0f);
        if (p < last) p = last; // (kept in order)
        last = p;
        o[7][i] = p;
        for (int k = 0; k < 3; k++) o[8 + i][k] = fmaxf(t->stop_col[s][k], 0.0f);
    }
    o[8][3] = clampf(t->shadow_threshold, 0.0f, 1.0f);
}

// --- fields by name (the Blueprint nodes) ---
static bool name_is(const char* a, const char* b) { // (spaces, underscores and case ignored)
    for (;;) {
        while (*a == ' ' || *a == '_') a++;
        while (*b == ' ' || *b == '_') b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++, b++;
    }
}

typedef struct ToonField { const char* name; const char* alt; size_t off; } ToonField;
#define TF(n, a, f) { n, a, offsetof(MatToon, f) }
static const ToonField g_scalars[] = {
    TF("Softness", "Suavidade", softness), TF("Shadow Threshold", "Limite da Sombra", shadow_threshold),
    TF("Specular", "Especular", spec_strength), TF("Specular Size", "Tamanho do Especular", spec_size),
    TF("Specular Hardness", "Dureza do Especular", spec_hardness), TF("Rim", "Luz de Borda", rim_strength),
    TF("Rim Width", "Largura da Borda", rim_width), TF("Rim Softness", "Suavidade da Borda", rim_softness),
    TF("Hatching", "Hachura", hatch_strength), TF("Hatch Scale", "Escala da Hachura", hatch_scale),
    TF("Hatch Angle", "Ângulo da Hachura", hatch_angle), TF("Hatch Threshold", "Limite da Hachura", hatch_threshold),
    TF("Ambient", "Ambiente", ambient), TF("Outline Width", "Largura do Contorno", outline_width),
    TF("Near Distance", "Distância Perto", near_dist), TF("Far Distance", "Distância Longe", far_dist),
    TF("Near Scale", "Escala Perto", near_scale), TF("Far Scale", "Escala Longe", far_scale),
    TF("Curve", "Curva", curve_exp), TF("Depth Sensitivity", "Sensibilidade da Profundidade", depth_sens),
    TF("Crease Angle", "Ângulo da Dobra", crease_deg),
};
static const ToonField g_colors[] = {
    TF("Shadow Color", "Cor da Sombra", shadow_color), TF("Receive Shadow Color", "Cor da Sombra Recebida", recv_color),
    TF("Specular Color", "Cor do Especular", spec_color), TF("Rim Color", "Cor da Borda", rim_color),
    TF("Hatch Color", "Cor da Hachura", hatch_color), TF("Outline Color", "Cor do Contorno", outline_color),
};

bool toon_set_scalar(MatToon* t, const char* name, float v) {
    if (!name) return false;
    if (name_is(name, "Bands") || name_is(name, "Faixas")) return t->bands = (int)lroundf(clampf(v, 0.0f, 8.0f)), true;
    if (name_is(name, "Outline") || name_is(name, "Contorno")) return t->outline = v != 0.0f, true;
    if (name_is(name, "Adaptive") || name_is(name, "Adaptativo")) return t->adaptive = v != 0.0f, true;
    for (size_t i = 0; i < sizeof g_scalars / sizeof g_scalars[0]; i++)
        if (name_is(name, g_scalars[i].name) || name_is(name, g_scalars[i].alt)) {
            *(float*)((char*)t + g_scalars[i].off) = v;
            return true;
        }
    return false;
}

bool toon_get_scalar(const MatToon* t, const char* name, float* v) {
    if (!name) return false;
    if (name_is(name, "Bands") || name_is(name, "Faixas")) return *v = (float)t->bands, true;
    if (name_is(name, "Outline") || name_is(name, "Contorno")) return *v = t->outline ? 1.0f : 0.0f, true;
    for (size_t i = 0; i < sizeof g_scalars / sizeof g_scalars[0]; i++)
        if (name_is(name, g_scalars[i].name) || name_is(name, g_scalars[i].alt)) return *v = *(const float*)((const char*)t + g_scalars[i].off), true;
    return false;
}

bool toon_set_color(MatToon* t, const char* name, const float rgb[3]) {
    if (!name) return false;
    for (size_t i = 0; i < sizeof g_colors / sizeof g_colors[0]; i++)
        if (name_is(name, g_colors[i].name) || name_is(name, g_colors[i].alt)) {
            memcpy((char*)t + g_colors[i].off, rgb, 12);
            return true;
        }
    return false;
}

float toon_outline_radius(const MatToon* t, float dist_cm, float render_h) {
    float w = fmaxf(t->outline_width, 0.0f) * (render_h / 1440.0f);
    if (t->adaptive) {
        float nd = fmaxf(t->near_dist, 0.0f) * 100.0f, fd = fmaxf(t->far_dist * 100.0f, nd + 1.0f);
        float x = clampf((dist_cm - nd) / (fd - nd), 0.0f, 1.0f);
        x = powf(x, clampf(t->curve_exp, 0.1f, 8.0f));
        w *= t->near_scale + (t->far_scale - t->near_scale) * x;
    }
    return 0.5f * w;
}
