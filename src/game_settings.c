// Game User Settings and the game's pause (game_settings.h, notas-dev/pausa-opcoes.md): the settings by name (the
// nodes, the widgets bound to one), GameUserSettings.ini (its lines are the same names: read back through gus_set), the
// keys remapped (input.c asks gus_remapped for every mapping it reads), what applies at once and what the host applies.
// Nothing here runs per frame but gus_remapped (one compare when nothing is remapped) and the bound widgets' reads.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "game_settings.h"
#include "upscale.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLFW/glfw3.h>

#include "audio.h"
#include "bp_internal.h"
#include "i18n.h"
#include "input.h"
#include "platform.h"
#include "platform_files.h"
#include "project.h"
#include "render.h"

static struct {
    GameUserSettings cur, applied, base;
    bool ready, base_ready;
    char path[1100];
    GusHost host;
    uint32_t serial, remap_hits;
    bool paused, quit;
} S;

static void ensure(void) {
    if (S.ready) return;
    S.ready = true;
    gus_reset();
    S.applied = S.cur;
}

void gus_defaults(GameUserSettings* s) {
    const ProjectSettings* p = project();
    memset(s, 0, sizeof *s);
    s->scalability = p->game_scalability >= 0 && p->game_scalability < RENDER_SCALABILITY_LEVELS ? p->game_scalability : 2;
    s->res_w = p->resolution_w > 0 ? p->resolution_w : 0;
    s->res_h = p->resolution_h > 0 ? p->resolution_h : 0;
    s->window_mode = p->fullscreen ? GUS_WINDOW_WINDOWED_FULLSCREEN : GUS_WINDOW_WINDOWED;
    s->vsync = p->vsync;
    s->frame_limit = 0;
    s->res_scale = 100;
    for (int k = 0; k < GUS_VOL_COUNT; k++) s->volume[k] = 1.0f;
    s->sensitivity = 1.0f;
    s->language = -1;
    s->auto_save = true;
    s->upscale_mode = p->upscale_mode >= 0 && p->upscale_mode < UPM_COUNT ? p->upscale_mode : UPM_PRESET;
    s->dyn_res = p->dyn_res;
    s->upscaler = -1; // (the project's)
}
GameUserSettings* gus_base(void) {
    if (!S.base_ready) S.base_ready = true, gus_defaults(&S.base);
    return &S.base;
}
void gus_reset(void) {
    S.cur = *gus_base();
    S.serial++;
}
GameUserSettings* gus(void) { return ensure(), &S.cur; }
const GameUserSettings* gus_applied(void) { return ensure(), &S.applied; }
uint32_t gus_serial(void) { return S.serial; }

// --- The file ---

void gus_set_path(const char* path) { snprintf(S.path, sizeof S.path, "%s", path ? path : ""); }
const char* gus_path(void) {
    const char* env = getenv("REALENGINE_SETTINGS_FILE");
    return env && env[0] ? env : S.path;
}

void gus_default_path(const char* game_name, bool packaged, char* out, size_t cap) {
    char root[1024] = "";
#ifdef _WIN32
    if (packaged) { // %LOCALAPPDATA%/<game>/Saved/Config (Unreal's place for a packaged game's settings)
        wchar_t w[600];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", w, 600);
        char la[1024];
        if (n > 0 && n < 600 && WideCharToMultiByte(CP_UTF8, 0, w, -1, la, (int)sizeof la, NULL, NULL) > 0) {
            char name[96];
            size_t k = 0;
            for (const char* c = game_name && game_name[0] ? game_name : "RealEngineGame"; *c && k + 1 < sizeof name; c++)
                name[k++] = strchr("\\/:*?\"<>|", *c) ? '_' : *c; // (a folder's name)
            name[k] = 0;
            snprintf(root, sizeof root, "%s/%s/Saved/Config", la, name);
        }
    }
#endif
    if (!root[0]) { // the editor's Play: the project's Saved/Config (Android: the app's files, the working folder)
        if (packaged) snprintf(root, sizeof root, "Saved/Config");
        else {
            char pr[900];
            project_root(pr, sizeof pr);
            snprintf(root, sizeof root, "%s/Saved/Config", pr);
        }
    }
    snprintf(out, cap, "%s/GameUserSettings.ini", root);
}

static const char* const g_names[] = { "Scalability", "Resolution", "WindowMode", "VSync", "FrameRateLimit", "ResolutionScale",
                                       "MasterVolume", "MusicVolume", "SFXVolume", "VoiceVolume", "MouseSensitivity", "InvertY",
                                       "Language", "AutoSave", "UpscalingMode", "DynamicResolution", "Upscaler" };
enum { N_SCAL = 0, N_RES, N_WIN, N_VSYNC, N_FPS, N_RSCALE, N_VOL0, N_VOL1, N_VOL2, N_VOL3, N_SENS, N_INV, N_LANG, N_AUTOSAVE, N_UPMODE, N_DYNRES, N_UPS, N_COUNT };
// (4.4a) the Upscaler's choices: the project's, then each one that can run here (its runtime installed, its GPU)
static int ups_choices(int* kinds) {
    int n = 0;
    kinds[n++] = -1;
    for (int k = 0; k < UPS_COUNT; k++)
        if (upscale_available(k, NULL)) kinds[n++] = k;
    return n;
}
// (Q22) the Upscaler combo's choices asked once while loading (Play, the packaged game's start): the first ask loads
// each vendor's runtime and starts NGX (~1.4 s), which happened in the frame that first drew the Options (pause_options:
// a 1.4 s frame, 128 fps measured instead of 650). Later asks are the cached answers
void gus_warm(void) {
    int kinds[UPS_COUNT + 1];
    double t0 = re_time_seconds();
    int n = ups_choices(kinds);
    double ms = (re_time_seconds() - t0) * 1000.0;
    if (ms > 5.0) printf("[RealEngine] Game User Settings: the upscalers asked while loading (%d choices, %.0f ms)\n", n, ms);
}
uint32_t gus_setting_count(void) { return N_COUNT; }
const char* gus_setting_name(uint32_t i) { return i < N_COUNT ? g_names[i] : ""; }
static int name_id(const char* n) {
    for (int i = 0; n && i < N_COUNT; i++)
        if (!_stricmp(n, g_names[i])) return i;
    return -1;
}

bool gus_save(void) {
    ensure();
    const char* p = gus_path();
    if (!p[0]) return false;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", p);
    char* cut = strrchr(dir, '/');
    char* cut2 = strrchr(dir, '\\');
    if (cut2 > cut) cut = cut2;
    if (cut) *cut = 0, re_make_dirs(dir);
    FILE* f = re_fopen(p, "wb");
    if (!f) return false;
    fprintf(f, "; Real Engine: the game's options. Deleting the file brings the defaults back.\n");
    fprintf(f, "[GameUserSettings]\nVersion=1\n");
    for (int i = 0; i < N_COUNT; i++) {
        char text[96];
        float v;
        if (gus_get(g_names[i], &v, text, sizeof text)) fprintf(f, "%s=%s\n", g_names[i], text);
    }
    if (S.cur.nremap) fprintf(f, "[Controls]\n");
    for (uint32_t r = 0; r < S.cur.nremap; r++)
        fprintf(f, "Key:%s:%s=%s\n", S.cur.remap[r].action, S.cur.remap[r].from, S.cur.remap[r].to);
    fclose(f);
    return true;
}

bool gus_load(void) {
    ensure();
    FILE* f = re_fopen(gus_path(), "rb");
    if (!f) return false;
    GameUserSettings keep = S.cur;
    S.cur.nremap = 0;
    char line[400];
    int read = 0;
    while (fgets(line, sizeof line, f)) {
        char* e = line + strlen(line);
        while (e > line && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ')) *--e = 0;
        char* eq = line[0] && line[0] != ';' && line[0] != '[' ? strrchr(line, '=') : NULL;
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(line, "Version")) continue;
        if (gus_set(line, (float)atof(eq + 1), eq + 1)) read++;
    }
    fclose(f);
    if (!read) S.cur = keep;
    S.serial++;
    return read > 0;
}

// --- By name ---

static bool numeric(const char* t) {
    if (!t || !t[0]) return false;
    char* end;
    strtod(t, &end);
    return *end == 0;
}

static const int g_fps[] = { 0, 30, 60, 90, 120, 144, 165, 240 };
#define NFPS ((int)(sizeof g_fps / sizeof g_fps[0]))

// The monitor's sizes (the Resolution combo), the largest first; without a monitor (the tests) the common ones
static int resolutions(int (*out)[2], int cap) {
    int n = 0;
    GLFWmonitor* m = glfwGetPrimaryMonitor();
    int count = 0;
    const GLFWvidmode* modes = m ? glfwGetVideoModes(m, &count) : NULL;
    for (int i = count - 1; i >= 0 && n < cap; i--) {
        if (modes[i].width < 1024 || modes[i].height < 600) continue;
        bool dup = false;
        for (int k = 0; k < n && !dup; k++) dup = out[k][0] == modes[i].width && out[k][1] == modes[i].height;
        if (!dup) out[n][0] = modes[i].width, out[n][1] = modes[i].height, n++;
    }
    if (!n) {
        static const int common[][2] = { { 3840, 2160 }, { 2560, 1440 }, { 1920, 1080 }, { 1600, 900 }, { 1280, 720 } };
        for (int i = 0; i < 5 && n < cap; i++) out[n][0] = common[i][0], out[n][1] = common[i][1], n++;
    }
    return n;
}
static void current_res(int* w, int* h) {
    *w = S.cur.res_w, *h = S.cur.res_h;
    if (*w > 0 && *h > 0) return;
    GLFWmonitor* m = glfwGetPrimaryMonitor();
    const GLFWvidmode* md = m ? glfwGetVideoMode(m) : NULL;
    *w = md ? md->width : 1920, *h = md ? md->height : 1080;
}

// Key:<Action>[:<Key>]: the action and the key of its mapping (the default keyboard one when not named)
static bool key_setting(const char* name, char* action, char* from, bool* gamepad) {
    if (_strnicmp(name, "Key:", 4)) return false;
    const char* a = name + 4;
    const char* c = strchr(a, ':');
    size_t n = c ? (size_t)(c - a) : strlen(a);
    if (!n || n >= GUS_NAME) return false;
    memcpy(action, a, n), action[n] = 0;
    *gamepad = false;
    if (c && c[1]) {
        if (!_stricmp(c + 1, "Gamepad")) *gamepad = true, from[0] = 0;
        else snprintf(from, GUS_NAME, "%s", c + 1);
    } else from[0] = 0;
    if (!from[0]) snprintf(from, GUS_NAME, "%s", gus_action_default_key(action, *gamepad));
    return from[0] != 0;
}

bool gus_get(const char* name, float* value, char* text, size_t cap) {
    ensure();
    const GameUserSettings* s = &S.cur;
    float v = 0.0f;
    char t[96] = "";
    switch (name_id(name)) {
    case N_SCAL: v = (float)s->scalability; break;
    case N_RES: {
        int w, h;
        current_res(&w, &h);
        v = (float)w, snprintf(t, sizeof t, "%dx%d", w, h);
        break;
    }
    case N_WIN: v = (float)s->window_mode; break;
    case N_VSYNC: v = s->vsync ? 1.0f : 0.0f; break;
    case N_FPS: v = (float)s->frame_limit; break;
    case N_RSCALE: v = (float)s->res_scale; break;
    case N_VOL0: case N_VOL1: case N_VOL2: case N_VOL3: v = s->volume[name_id(name) - N_VOL0]; break;
    case N_SENS: v = s->sensitivity; break;
    case N_INV: v = s->invert_y ? 1.0f : 0.0f; break;
    case N_AUTOSAVE: v = s->auto_save ? 1.0f : 0.0f; break;
    case N_UPMODE: v = (float)s->upscale_mode; break;
    case N_DYNRES: v = s->dyn_res ? 1.0f : 0.0f; break;
    case N_UPS: v = (float)s->upscaler; break;
    case N_LANG: v = (float)s->language, snprintf(t, sizeof t, "%s", s->language >= 0 ? i18n_language_code(s->language) : "auto"); break;
    default: {
        char action[GUS_NAME], from[GUS_NAME];
        bool gp;
        if (!name || !key_setting(name, action, from, &gp)) return false;
        snprintf(t, sizeof t, "%s", gus_remapped(action, from));
        break;
    }
    }
    if (!t[0]) snprintf(t, sizeof t, "%g", v);
    if (value) *value = v;
    if (text && cap) snprintf(text, cap, "%s", t);
    return true;
}

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

bool gus_set(const char* name, float value, const char* text) {
    ensure();
    GameUserSettings* s = &S.cur;
    int id = name_id(name);
    bool has_text = text && text[0];
    if (id >= 0 && has_text && numeric(text)) value = (float)atof(text), has_text = false;
    switch (id) {
    case N_SCAL: case N_WIN: case N_FPS: case N_UPMODE: // a choice by its name ("High", "Windowed", "60", "Quality")
        if (has_text) {
            char opts[256];
            gus_options(name, opts, sizeof opts);
            int k = 0;
            for (char* o = strtok(opts, "|"); o; o = strtok(NULL, "|"), k++)
                if (!_stricmp(o, text)) return gus_set_option(name, k);
            return false;
        }
        if (id == N_SCAL) s->scalability = (int)clampf(value, 0.0f, (float)(RENDER_SCALABILITY_LEVELS - 1));
        else if (id == N_WIN) s->window_mode = (int)clampf(value, 0.0f, (float)(GUS_WINDOW_COUNT - 1));
        else if (id == N_UPMODE) s->upscale_mode = (int)clampf(value, 0.0f, (float)(UPM_COUNT - 1));
        else s->frame_limit = value <= 0.0f ? 0 : (int)clampf(value, 15.0f, 1000.0f);
        break;
    case N_RES: {
        int w = 0, h = 0;
        if (has_text && sscanf(text, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200) s->res_w = w, s->res_h = h;
        else if (!has_text && value <= 0.0f) s->res_w = s->res_h = 0; // (0: the monitor's)
        else return false;
        break;
    }
    case N_VSYNC: s->vsync = value != 0.0f; break;
    case N_RSCALE: s->res_scale = (int)clampf(value, 50.0f, 100.0f); break;
    case N_VOL0: case N_VOL1: case N_VOL2: case N_VOL3: s->volume[id - N_VOL0] = clampf(value, 0.0f, 1.0f); break;
    case N_SENS: s->sensitivity = clampf(value, 0.1f, 4.0f); break;
    case N_INV: s->invert_y = value != 0.0f; break;
    case N_AUTOSAVE: s->auto_save = value != 0.0f; break;
    case N_DYNRES: s->dyn_res = value != 0.0f; break;
    case N_UPS:
        if (has_text) { // (a choice by its name: "NVIDIA DLSS", "Project Default")
            char opts[256];
            gus_options(name, opts, sizeof opts);
            int k = 0;
            for (char* o = strtok(opts, "|"); o; o = strtok(NULL, "|"), k++)
                if (!_stricmp(o, text)) return gus_set_option(name, k);
            return false;
        }
        s->upscaler = value < 0.0f || value >= (float)UPS_COUNT ? -1 : (int)value;
        break;
    case N_LANG:
        if (has_text) s->language = !_stricmp(text, "auto") || !_stricmp(text, "system") ? -1 : i18n_language_from_code(text);
        else s->language = value < 0.0f || value >= LANG_COUNT ? -1 : (int)value;
        break;
    default: {
        char action[GUS_NAME], from[GUS_NAME];
        bool gp;
        if (!name || !key_setting(name, action, from, &gp)) return false;
        if (!gus_remap(action, from, has_text ? text : "")) return false;
        break;
    }
    }
    S.serial++;
    return true;
}

bool gus_options(const char* name, char* out, size_t cap) {
    int lang = gus_language();
    out[0] = 0;
    size_t o = 0;
#define ADD(s) do { int n_ = snprintf(out + o, cap - o, "%s%s", o ? "|" : "", (s)); if (n_ > 0 && o + (size_t)n_ < cap) o += (size_t)n_; } while (0)
    switch (name_id(name)) {
    case N_SCAL: {
        int levels = project()->render_mode == PROJECT_RENDER_QUALITY ? RENDER_SCALABILITY_LEVELS : RENDER_SCALABILITY_LEVELS - 1;
        for (int i = 0; i < levels; i++) ADD(tr_lang(render_scalability_name(i), lang));
        return true;
    }
    case N_WIN:
        ADD(tr_lang("Fullscreen", lang));
        ADD(tr_lang("Windowed Fullscreen", lang));
        ADD(tr_lang("Windowed", lang));
        return true;
    case N_FPS:
        for (int i = 0; i < NFPS; i++) {
            char b[16];
            snprintf(b, sizeof b, "%d", g_fps[i]);
            ADD(i ? b : tr_lang("Unlimited", lang));
        }
        return true;
    case N_RES: {
        int r[24][2], n = resolutions(r, 24);
        for (int i = 0; i < n; i++) {
            char b[24];
            snprintf(b, sizeof b, "%dx%d", r[i][0], r[i][1]);
            ADD(b);
        }
        return true;
    }
    case N_UPMODE:
        for (int i = 0; i < UPM_COUNT; i++) ADD(tr_lang(upscale_mode_name(i), lang));
        return true;
    case N_UPS: {
        int kinds[UPS_COUNT + 1], n = ups_choices(kinds);
        for (int i = 0; i < n; i++) ADD(kinds[i] < 0 ? tr_lang("Project Default", lang) : upscale_kind_name(kinds[i]));
        return true;
    }
    case N_LANG:
        ADD(tr_lang("System", lang));
        for (int i = 0; i < LANG_COUNT; i++) ADD(i18n_language_name(i));
        return true;
    default: return false;
    }
#undef ADD
}

int gus_option_index(const char* name) {
    ensure();
    const GameUserSettings* s = &S.cur;
    switch (name_id(name)) {
    case N_SCAL: return s->scalability;
    case N_WIN: return s->window_mode;
    case N_UPMODE: return s->upscale_mode;
    case N_UPS: {
        int kinds[UPS_COUNT + 1], n = ups_choices(kinds);
        for (int i = 0; i < n; i++)
            if (kinds[i] == s->upscaler) return i;
        return 0;
    }
    case N_FPS: {
        int best = 0;
        for (int i = 0; i < NFPS; i++)
            if (abs(g_fps[i] - s->frame_limit) < abs(g_fps[best] - s->frame_limit)) best = i;
        return best;
    }
    case N_RES: {
        int r[24][2], n = resolutions(r, 24), w, h;
        current_res(&w, &h);
        for (int i = 0; i < n; i++)
            if (r[i][0] == w && r[i][1] == h) return i;
        return 0;
    }
    case N_LANG: return s->language + 1;
    default: return -1;
    }
}

bool gus_set_option(const char* name, int k) {
    if (k < 0) return false;
    switch (name_id(name)) {
    case N_SCAL: case N_WIN: case N_UPMODE: return gus_set(name, (float)k, NULL);
    case N_UPS: {
        int kinds[UPS_COUNT + 1], n = ups_choices(kinds);
        return k < n && gus_set(name, (float)kinds[k], NULL);
    }
    case N_FPS: return k < NFPS && gus_set(name, (float)g_fps[k], NULL);
    case N_RES: {
        int r[24][2], n = resolutions(r, 24);
        if (k >= n) return false;
        char b[24];
        snprintf(b, sizeof b, "%dx%d", r[k][0], r[k][1]);
        return gus_set(name, 0.0f, b);
    }
    case N_LANG: return gus_set(name, (float)(k - 1), NULL);
    default: return false;
    }
}

bool gus_value_text(const char* name, char* out, size_t cap) {
    float v;
    char t[96];
    if (!gus_get(name, &v, t, sizeof t)) return false;
    int id = name_id(name), lang = gus_language();
    if (id >= N_VOL0 && id <= N_VOL3) snprintf(out, cap, "%.0f%%", v * 100.0f);
    else if (id == N_RSCALE) snprintf(out, cap, "%.0f%%", v);
    else if (id == N_SENS) snprintf(out, cap, "%.2fx", v);
    else if (id == N_VSYNC || id == N_INV || id == N_AUTOSAVE || id == N_DYNRES) snprintf(out, cap, "%s", tr_lang(v != 0.0f ? "On" : "Off", lang));
    else if (id == N_SCAL || id == N_WIN || id == N_FPS || id == N_LANG || id == N_UPMODE || id == N_UPS) {
        char opts[512];
        gus_options(name, opts, sizeof opts);
        int k = gus_option_index(name), i = 0;
        out[0] = 0;
        for (char* o = strtok(opts, "|"); o; o = strtok(NULL, "|"), i++)
            if (i == k) snprintf(out, cap, "%s", o);
    } else if (id < 0) snprintf(out, cap, "%s", tr_lang(t, lang)); // a key ("Space": "Espaço" where translated)
    else snprintf(out, cap, "%s", t);
    return true;
}

bool gus_slider_range(const char* name, float* vmin, float* vmax, float* step) {
    int id = name_id(name);
    if (id >= N_VOL0 && id <= N_VOL3) return *vmin = 0.0f, *vmax = 1.0f, *step = 0.05f, true;
    if (id == N_SENS) return *vmin = 0.1f, *vmax = 4.0f, *step = 0.05f, true;
    if (id == N_RSCALE) return *vmin = 50.0f, *vmax = 100.0f, *step = 5.0f, true;
    return false;
}

// --- The controls ---

const char* gus_remapped(const char* action, const char* key) {
    const GameUserSettings* s = &S.cur;
    if (!s->nremap) return key; // (the common case: one compare)
    for (uint32_t i = 0; i < s->nremap; i++)
        if (!_stricmp(s->remap[i].from, key) && !_stricmp(s->remap[i].action, action)) return S.remap_hits++, s->remap[i].to;
    return key;
}
uint32_t gus_remap_hits(void) { return S.remap_hits; }

int gus_remapped_code(const char* action, int code) {
    if (!S.cur.nremap) return code; // (nothing remapped: the key itself)
    static struct { int code, out; uint32_t serial; const char* action; } cache[8];
    for (int i = 0; i < 8; i++)
        if (cache[i].serial == S.serial + 1u && cache[i].code == code && cache[i].action == action) return cache[i].out;
    int out = code;
    for (uint32_t i = 0; i < input_key_count(); i++) {
        const char* n = input_key_name(i);
        if (bp_key_code(n) != code) continue;
        const char* r = gus_remapped(action, n);
        int c = r != n ? bp_key_code(r) : code;
        out = c >= 0 && c < 512 ? c : code;
        break;
    }
    static int next;
    cache[next].code = code, cache[next].out = out, cache[next].serial = S.serial + 1u, cache[next].action = action;
    next = (next + 1) & 7;
    return out;
}

static bool digital_key(const char* k) { return !strstr(k, "Axis") && _stricmp(k, "Mouse X") && _stricmp(k, "Mouse Y"); }

const char* gus_action_default_key(const char* action, bool gamepad) {
    for (uint32_t c = 0; c < input_context_count(); c++) {
        const InputContext* ic = input_context_at(c);
        for (uint32_t m = 0; ic && m < ic->nmaps; m++) {
            const InputMapping* mp = &ic->maps[m];
            if (_stricmp(mp->action, action) || !digital_key(mp->key)) continue;
            if ((_strnicmp(mp->key, "Gamepad", 7) == 0) == gamepad) return mp->key;
        }
    }
    return "";
}
const char* gus_action_key(const char* action, bool gamepad) {
    const char* k = gus_action_default_key(action, gamepad);
    return k[0] ? gus_remapped(action, k) : k;
}

bool gus_remap(const char* action, const char* from, const char* to) {
    ensure();
    GameUserSettings* s = &S.cur;
    if (!action || !action[0]) return false;
    if (!from || !from[0]) from = gus_action_default_key(action, false);
    if (!from[0]) return false;
    char f[GUS_NAME], t[GUS_NAME];
    snprintf(f, sizeof f, "%s", from), snprintf(t, sizeof t, "%s", to ? to : "");
    int at = -1;
    for (uint32_t i = 0; i < s->nremap && at < 0; i++)
        if (!_stricmp(s->remap[i].action, action) && !_stricmp(s->remap[i].from, f)) at = (int)i;
    if (!t[0] || !_stricmp(t, f)) { // back to its own key
        if (at >= 0) s->remap[at] = s->remap[--s->nremap];
    } else {
        if (at < 0) {
            if (s->nremap >= GUS_MAX_REMAPS) return false;
            at = (int)s->nremap++;
        }
        snprintf(s->remap[at].action, GUS_NAME, "%s", action);
        snprintf(s->remap[at].from, GUS_NAME, "%s", f);
        snprintf(s->remap[at].to, GUS_NAME, "%s", t);
    }
    S.applied.nremap = s->nremap; // (the keys apply at once: input.c reads the current ones)
    memcpy(S.applied.remap, s->remap, sizeof s->remap);
    S.serial++;
    return true;
}

// --- The language ---

int gus_language(void) {
    ensure();
    int l = S.applied.language;
    if (l >= 0 && l < LANG_COUNT) return l;
    return S.host.global_language ? i18n_system_language() : i18n_language(); // (the editor's Play: the editor's)
}
const char* gus_tr(const char* text) {
    int l = gus_language();
    return l == LANG_EN ? text : tr_lang(text, l);
}

// --- Applying ---

void gus_set_host(const GusHost* h) { S.host = h ? *h : (GusHost){ 0 }; }

void gus_apply_live(void) {
    ensure();
    const GameUserSettings* s = &S.cur;
    audio_set_master_volume(s->volume[GUS_VOL_MASTER]);
    audio_set_user_class_volume(AUDIO_CLASS_MUSIC, s->volume[GUS_VOL_MUSIC]);
    audio_set_user_class_volume(AUDIO_CLASS_SFX, s->volume[GUS_VOL_SFX]);
    audio_set_user_class_volume(AUDIO_CLASS_VOICE, s->volume[GUS_VOL_VOICE]);
    // what doesn't change the window: applied now; the window's (resolution, mode, VSync) waits for Apply Settings
    GameUserSettings a = S.applied;
    int res_w = a.res_w, res_h = a.res_h, mode = a.window_mode;
    bool vsync = a.vsync;
    a = *s;
    a.res_w = res_w, a.res_h = res_h, a.window_mode = mode, a.vsync = vsync;
    bool video = a.scalability != S.applied.scalability || a.res_scale != S.applied.res_scale || a.frame_limit != S.applied.frame_limit ||
                 a.upscale_mode != S.applied.upscale_mode || a.dyn_res != S.applied.dyn_res || a.upscaler != S.applied.upscaler;
    bool lang = a.language != S.applied.language;
    S.applied = a;
    if (video && S.host.apply_video) S.host.apply_video(&S.applied, false);
    if (lang && S.host.global_language) i18n_set_language(gus_language());
    S.serial++;
}

void gus_apply(bool save) {
    ensure();
    bool display = S.cur.res_w != S.applied.res_w || S.cur.res_h != S.applied.res_h || S.cur.window_mode != S.applied.window_mode ||
                   S.cur.vsync != S.applied.vsync;
    bool lang = S.cur.language != S.applied.language;
    audio_set_master_volume(S.cur.volume[GUS_VOL_MASTER]);
    audio_set_user_class_volume(AUDIO_CLASS_MUSIC, S.cur.volume[GUS_VOL_MUSIC]);
    audio_set_user_class_volume(AUDIO_CLASS_SFX, S.cur.volume[GUS_VOL_SFX]);
    audio_set_user_class_volume(AUDIO_CLASS_VOICE, S.cur.volume[GUS_VOL_VOICE]);
    S.applied = S.cur;
    if (S.host.apply_video) S.host.apply_video(&S.applied, display);
    if (S.host.global_language && (lang || display)) i18n_set_language(gus_language());
    S.serial++;
    if (save) gus_save();
}

// --- The pause, Quit Game ---

bool game_paused(void) { return S.paused; }
void game_set_paused(bool paused) { S.paused = paused; }
void game_request_quit(void) { S.quit = true; }
bool game_quit_requested(void) { return S.quit; }
void game_quit_clear(void) { S.quit = false; }
