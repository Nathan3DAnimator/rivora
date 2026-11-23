// Enhanced Input (input.h): the assets (JSON) and the frame's work — each active context's mappings read their keys,
// their modifiers shape the value, the actions add them up (a higher priority context consumes its keys when the action
// says so), the action's own modifiers, then its triggers decide the events.
#include "input.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLFW/glfw3.h>

#include "bp_internal.h"
#include "game_settings.h"
#include "io_json.h"
#include "platform.h"
#include "platform_files.h"

const char* const g_in_value_names[IAV_COUNT] = { "Digital (bool)", "Axis1D (float)", "Axis2D (Vector2D)", "Axis3D (Vector)" };
const char* const g_in_mod_names[IMOD_COUNT] = { "Negate", "Swizzle Input Axis Values", "Dead Zone", "Scalar" };
const char* const g_in_trig_names[ITRG_COUNT] = { "Down", "Pressed", "Released", "Hold", "Hold And Release", "Tap", "Pulse" };

#define MAX_ACTIONS 256

// An action while playing: its value, this frame's events, how long it has been actuated / triggered
typedef struct InSt {
    float value[3];
    uint32_t events;
    float elapsed, triggered_s, pulse_t;
    bool actuated, was_actuated, triggered_any, was_triggered;
} InSt;
#define MAX_CONTEXTS 64
#define MAX_ACTIVE 16

static struct {
    char root[512];
    InputAction* actions[MAX_ACTIONS];
    uint32_t nactions;
    InputContext* contexts[MAX_CONTEXTS];
    uint32_t ncontexts;
    bool scanned;
    // play
    struct { const InputContext* c; int priority; } active[MAX_ACTIVE];
    uint32_t nactive;
    uint8_t keys[512], prev_keys[512];
    float mouse[2], wheel;
    InSt st[MAX_ACTIONS];
} I;

// --- The keys a mapping names ---
static const char* const g_keys[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y",
    "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "Space", "Enter", "Escape", "Tab", "BackSpace", "Left Shift",
    "Left Ctrl", "Left Alt", "Up", "Down", "Left", "Right", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11",
    "F12", "Left Mouse Button", "Right Mouse Button", "Middle Mouse Button", "Mouse X", "Mouse Y", "Mouse XY 2D-Axis",
    "Mouse Wheel Axis", "Gamepad Left Thumbstick 2D-Axis", "Gamepad Right Thumbstick 2D-Axis", "Gamepad Left Thumbstick X-Axis",
    "Gamepad Left Thumbstick Y-Axis", "Gamepad Right Thumbstick X-Axis", "Gamepad Right Thumbstick Y-Axis",
    "Gamepad Left Trigger Axis", "Gamepad Right Trigger Axis", "Gamepad Face Button Bottom", "Gamepad Face Button Right",
    "Gamepad Face Button Left", "Gamepad Face Button Top", "Gamepad Left Shoulder", "Gamepad Right Shoulder",
    "Gamepad Special Left", "Gamepad Special Right", "Gamepad D-pad Up", "Gamepad D-pad Down", "Gamepad D-pad Left",
    "Gamepad D-pad Right", "Gamepad Left Thumbstick Button", "Gamepad Right Thumbstick Button",
};
uint32_t input_key_count(void) { return (uint32_t)(sizeof g_keys / sizeof g_keys[0]); }
const char* input_key_name(uint32_t i) { return i < input_key_count() ? g_keys[i] : ""; }

// A key's value now (x y z): a button 0 / 1, an axis its value
static void key_value(const char* key, const GLFWgamepadstate* gp, bool have_gp, float v[3]) {
    v[0] = v[1] = v[2] = 0.0f;
    if (!_stricmp(key, "Mouse X")) v[0] = I.mouse[0];
    else if (!_stricmp(key, "Mouse Y")) v[0] = I.mouse[1];
    else if (!_stricmp(key, "Mouse XY 2D-Axis")) v[0] = I.mouse[0], v[1] = I.mouse[1];
    else if (!_stricmp(key, "Mouse Wheel Axis")) v[0] = I.wheel;
    else if (!_stricmp(key, "Middle Mouse Button")) v[0] = I.keys[502] ? 1.0f : 0.0f;
    else if (!_strnicmp(key, "Gamepad", 7)) {
        if (!have_gp) return;
        static const struct { const char* n; int b; } buttons[] = {
            { "Gamepad Face Button Bottom", GLFW_GAMEPAD_BUTTON_A }, { "Gamepad Face Button Right", GLFW_GAMEPAD_BUTTON_B },
            { "Gamepad Face Button Left", GLFW_GAMEPAD_BUTTON_X }, { "Gamepad Face Button Top", GLFW_GAMEPAD_BUTTON_Y },
            { "Gamepad Left Shoulder", GLFW_GAMEPAD_BUTTON_LEFT_BUMPER }, { "Gamepad Right Shoulder", GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER },
            { "Gamepad Special Left", GLFW_GAMEPAD_BUTTON_BACK }, { "Gamepad Special Right", GLFW_GAMEPAD_BUTTON_START },
            { "Gamepad D-pad Up", GLFW_GAMEPAD_BUTTON_DPAD_UP }, { "Gamepad D-pad Down", GLFW_GAMEPAD_BUTTON_DPAD_DOWN },
            { "Gamepad D-pad Left", GLFW_GAMEPAD_BUTTON_DPAD_LEFT }, { "Gamepad D-pad Right", GLFW_GAMEPAD_BUTTON_DPAD_RIGHT },
            { "Gamepad Left Thumbstick Button", GLFW_GAMEPAD_BUTTON_LEFT_THUMB },
            { "Gamepad Right Thumbstick Button", GLFW_GAMEPAD_BUTTON_RIGHT_THUMB },
        };
        for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
            if (!_stricmp(key, buttons[i].n)) {
                v[0] = gp->buttons[buttons[i].b] ? 1.0f : 0.0f;
                return;
            }
        const float* ax = gp->axes; // (GLFW's y is down: up is positive here, Unreal's)
        if (!_stricmp(key, "Gamepad Left Thumbstick 2D-Axis")) v[0] = ax[GLFW_GAMEPAD_AXIS_LEFT_X], v[1] = -ax[GLFW_GAMEPAD_AXIS_LEFT_Y];
        else if (!_stricmp(key, "Gamepad Right Thumbstick 2D-Axis")) v[0] = ax[GLFW_GAMEPAD_AXIS_RIGHT_X], v[1] = -ax[GLFW_GAMEPAD_AXIS_RIGHT_Y];
        else if (!_stricmp(key, "Gamepad Left Thumbstick X-Axis")) v[0] = ax[GLFW_GAMEPAD_AXIS_LEFT_X];
        else if (!_stricmp(key, "Gamepad Left Thumbstick Y-Axis")) v[0] = -ax[GLFW_GAMEPAD_AXIS_LEFT_Y];
        else if (!_stricmp(key, "Gamepad Right Thumbstick X-Axis")) v[0] = ax[GLFW_GAMEPAD_AXIS_RIGHT_X];
        else if (!_stricmp(key, "Gamepad Right Thumbstick Y-Axis")) v[0] = -ax[GLFW_GAMEPAD_AXIS_RIGHT_Y];
        else if (!_stricmp(key, "Gamepad Left Trigger Axis")) v[0] = (ax[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0f) * 0.5f;
        else if (!_stricmp(key, "Gamepad Right Trigger Axis")) v[0] = (ax[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f;
    } else {
        int k = bp_key_code(key);
        if (k >= 0 && k < 512) v[0] = I.keys[k] ? 1.0f : 0.0f;
    }
}

static void apply_mods(const InMod* mods, uint32_t n, float v[3]) {
    for (uint32_t i = 0; i < n; i++) {
        const InMod* m = &mods[i];
        switch (m->kind) {
        case IMOD_NEGATE:
            for (int k = 0; k < 3; k++)
                if (m->flags >> k & 1) v[k] = -v[k];
            break;
        case IMOD_SWIZZLE: {
            static const int order[5][3] = { { 1, 0, 2 }, { 2, 1, 0 }, { 0, 2, 1 }, { 1, 2, 0 }, { 2, 0, 1 } };
            const int* o = order[m->flags % 5];
            float t[3] = { v[o[0]], v[o[1]], v[o[2]] };
            memcpy(v, t, 12);
            break;
        }
        case IMOD_DEADZONE: { // radial on the vector: under lower nothing, rescaled to reach 1 at upper
            float lo = m->v[0], hi = m->v[1] > lo ? m->v[1] : 1.0f;
            float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            float k = len <= lo ? 0.0f : fminf((len - lo) / (hi - lo), 1.0f) / len;
            for (int j = 0; j < 3; j++) v[j] *= k;
            break;
        }
        case IMOD_SCALAR:
            for (int k = 0; k < 3; k++) v[k] *= m->v[k];
            break;
        default: break;
        }
    }
}

// --- Assets: JSON ---

static void write_mods(JsonWriter* w, const InMod* m, uint32_t n) {
    jw_key(w, "modifiers");
    jw_arr_begin(w);
    for (uint32_t i = 0; i < n; i++) {
        jw_obj_begin(w);
        jw_kint(w, "kind", m[i].kind);
        jw_kint(w, "flags", m[i].flags);
        jw_kfloats(w, "v", m[i].v, 3);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}
static uint8_t read_mods(const JsonValue* arr, InMod* m) {
    uint8_t n = 0;
    for (size_t i = 0; i < json_len(arr) && n < IN_MAX_MODS; i++) {
        const JsonValue* o = json_at(arr, i);
        InMod* x = &m[n];
        memset(x, 0, sizeof *x);
        x->kind = (uint8_t)json_get_int(o, "kind", 0);
        x->flags = (uint8_t)json_get_int(o, "flags", 0);
        json_get_floats(o, "v", x->v, 3);
        if (x->kind < IMOD_COUNT) n++;
    }
    return n;
}
static void write_trigs(JsonWriter* w, const InTrig* t, uint32_t n) {
    jw_key(w, "triggers");
    jw_arr_begin(w);
    for (uint32_t i = 0; i < n; i++) {
        jw_obj_begin(w);
        jw_kint(w, "kind", t[i].kind);
        jw_kbool(w, "once", t[i].once);
        jw_kfloat(w, "time", t[i].time);
        jw_kfloat(w, "threshold", t[i].threshold);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}
static uint8_t read_trigs(const JsonValue* arr, InTrig* t) {
    uint8_t n = 0;
    for (size_t i = 0; i < json_len(arr) && n < IN_MAX_TRIGS; i++) {
        const JsonValue* o = json_at(arr, i);
        InTrig* x = &t[n];
        x->kind = (uint8_t)json_get_int(o, "kind", 0);
        x->once = json_get_bool(o, "once", true);
        x->time = (float)json_get_num(o, "time", 0.5);
        x->threshold = (float)json_get_num(o, "threshold", 0.5);
        if (x->kind < ITRG_COUNT) n++;
    }
    return n;
}

static bool write_file(const char* path, JsonWriter* w) {
    FILE* f = re_fopen(path, "wb");
    bool ok = f && !w->fail && fwrite(w->buf, 1, w->len, f) == w->len;
    if (f) fclose(f);
    return ok;
}
static char* read_file(const char* path, size_t* len) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = n > 0 ? malloc((size_t)n + 1) : NULL; // a load, not a frame
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) free(b), b = NULL;
    fclose(f);
    if (b) b[n] = 0, *len = (size_t)n;
    return b;
}

bool input_action_save(const InputAction* a) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1);
    jw_kint(&w, "value_type", a->value_type);
    jw_kbool(&w, "consume", a->consume);
    write_trigs(&w, a->trig, a->ntrig);
    write_mods(&w, a->mods, a->nmods);
    jw_obj_end(&w);
    bool ok = write_file(a->path, &w);
    jw_free(&w);
    return ok;
}
bool input_context_save(const InputContext* c) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1);
    jw_key(&w, "mappings");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < c->nmaps; i++) {
        const InputMapping* m = &c->maps[i];
        jw_obj_begin(&w);
        jw_kstr(&w, "action", m->action);
        jw_kstr(&w, "key", m->key);
        write_trigs(&w, m->trig, m->ntrig);
        write_mods(&w, m->mods, m->nmods);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    bool ok = write_file(c->path, &w);
    jw_free(&w);
    return ok;
}

static uint32_t vhash(const char* vpath) { // the hash the assets are found by (bp_vpath_hash's)
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) h = (h ^ (uint8_t)tolower((unsigned char)*c)) * 16777619u;
    return h ? h : 1u;
}
static void disk_of(const char* vpath, char* out, size_t cap) {
    snprintf(out, cap, "%s%s", I.root, !strncmp(vpath, "/Content", 8) ? vpath + 8 : vpath);
}
static void name_of(const char* path, char* out) {
    const char* s = strrchr(path, '/');
    const char* s2 = strrchr(path, '\\');
    if (s2 > s) s = s2;
    snprintf(out, IN_NAME, "%s", s ? s + 1 : path);
    char* dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

InputAction* input_action_load(const char* vpath) {
    uint32_t h = vhash(vpath);
    for (uint32_t i = 0; i < I.nactions; i++)
        if (I.actions[i]->hash == h) return I.actions[i];
    if (I.nactions >= MAX_ACTIONS) return NULL;
    char path[700];
    disk_of(vpath, path, sizeof path);
    size_t len;
    char* text = read_file(path, &len);
    if (!text) return NULL;
    JsonDoc doc;
    char err[128];
    InputAction* a = NULL;
    if (json_parse(text, len, &doc, err, sizeof err)) {
        a = calloc(1, sizeof *a); // an asset, not a frame
        if (a) {
            snprintf(a->path, sizeof a->path, "%s", path);
            name_of(path, a->name);
            a->hash = h;
            a->value_type = (uint8_t)json_get_int(&doc.root, "value_type", IAV_BOOL);
            if (a->value_type >= IAV_COUNT) a->value_type = IAV_BOOL;
            a->consume = json_get_bool(&doc.root, "consume", false);
            a->ntrig = read_trigs(json_get(&doc.root, "triggers"), a->trig);
            a->nmods = read_mods(json_get(&doc.root, "modifiers"), a->mods);
            I.actions[I.nactions++] = a;
        }
        json_free(&doc);
    }
    free(text);
    return a;
}

InputContext* input_context_load(const char* vpath) {
    uint32_t h = vhash(vpath);
    for (uint32_t i = 0; i < I.ncontexts; i++)
        if (I.contexts[i]->hash == h) return I.contexts[i];
    if (I.ncontexts >= MAX_CONTEXTS) return NULL;
    char path[700];
    disk_of(vpath, path, sizeof path);
    size_t len;
    char* text = read_file(path, &len);
    if (!text) return NULL;
    JsonDoc doc;
    char err[128];
    InputContext* c = NULL;
    if (json_parse(text, len, &doc, err, sizeof err)) {
        c = calloc(1, sizeof *c); // an asset, not a frame
        if (c) {
            snprintf(c->path, sizeof c->path, "%s", path);
            name_of(path, c->name);
            c->hash = h;
            const JsonValue* ms = json_get(&doc.root, "mappings");
            for (size_t i = 0; i < json_len(ms) && c->nmaps < IN_MAX_MAPS; i++) {
                const JsonValue* o = json_at(ms, i);
                InputMapping* m = &c->maps[c->nmaps++];
                snprintf(m->action, IN_NAME, "%s", json_get_str(o, "action", ""));
                snprintf(m->key, IN_NAME, "%s", json_get_str(o, "key", ""));
                m->ntrig = read_trigs(json_get(o, "triggers"), m->trig);
                m->nmods = read_mods(json_get(o, "modifiers"), m->mods);
            }
            I.contexts[I.ncontexts++] = c;
        }
        json_free(&doc);
    }
    free(text);
    return c;
}

// Every input asset of the Content folder (once): what the names look up
typedef struct ScanCtx { char disk[700], vpath[700]; int depth; } ScanCtx;
static void scan_one(void* user, const ReDirEntry* e) {
    ScanCtx* s = user;
    if (e->name[0] == '.') return;
    if (e->is_dir) {
        if (s->depth >= 12) return;
        ScanCtx sub;
        snprintf(sub.disk, sizeof sub.disk, "%s/%s", s->disk, e->name);
        snprintf(sub.vpath, sizeof sub.vpath, "%s/%s", s->vpath, e->name);
        sub.depth = s->depth + 1;
        re_dir_walk(sub.disk, scan_one, &sub);
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (!dot) return;
    char vp[800];
    snprintf(vp, sizeof vp, "%s/%s", s->vpath, e->name);
    if (!_stricmp(dot, ".rinputaction")) input_action_load(vp);
    else if (!_stricmp(dot, ".rimc")) input_context_load(vp);
}
static void scan(void) {
    if (I.scanned) return;
    I.scanned = true;
    ScanCtx s;
    snprintf(s.disk, sizeof s.disk, "%s", I.root);
    snprintf(s.vpath, sizeof s.vpath, "/Content");
    s.depth = 0;
    re_dir_walk(s.disk, scan_one, &s);
}

InputAction* input_action_named(const char* name) {
    scan();
    for (uint32_t i = 0; i < I.nactions; i++)
        if (!_stricmp(I.actions[i]->name, name)) return I.actions[i];
    return NULL;
}
InputContext* input_context_named(const char* name) {
    scan();
    for (uint32_t i = 0; i < I.ncontexts; i++)
        if (!_stricmp(I.contexts[i]->name, name)) return I.contexts[i];
    return NULL;
}
uint32_t input_action_count(void) { return scan(), I.nactions; }
InputAction* input_action_at(uint32_t i) { return i < I.nactions ? I.actions[i] : NULL; }
uint32_t input_context_count(void) { return scan(), I.ncontexts; }
InputContext* input_context_at(uint32_t i) { return i < I.ncontexts ? I.contexts[i] : NULL; }

static bool unique_path(const char* folder_vpath, const char* name, const char* ext, char* vp, size_t cap) {
    for (int n = 0; n < 1000; n++) {
        if (n) snprintf(vp, cap, "%s/%s%d.%s", folder_vpath, name, n + 1, ext);
        else snprintf(vp, cap, "%s/%s.%s", folder_vpath, name, ext);
        char disk[800];
        disk_of(vp, disk, sizeof disk);
        if (!re_file_stat(disk, NULL, NULL, NULL)) return true;
    }
    return false;
}

InputAction* input_action_create(const char* folder_vpath, const char* name) {
    char vp[700];
    if (!unique_path(folder_vpath, name, "rinputaction", vp, sizeof vp) || I.nactions >= MAX_ACTIONS) return NULL;
    InputAction* a = calloc(1, sizeof *a); // an asset, not a frame
    if (!a) return NULL;
    disk_of(vp, a->path, sizeof a->path);
    name_of(a->path, a->name);
    a->hash = vhash(vp);
    a->value_type = IAV_BOOL;
    if (!input_action_save(a)) {
        free(a);
        return NULL;
    }
    I.actions[I.nactions++] = a;
    return a;
}
InputContext* input_context_create(const char* folder_vpath, const char* name) {
    char vp[700];
    if (!unique_path(folder_vpath, name, "rimc", vp, sizeof vp) || I.ncontexts >= MAX_CONTEXTS) return NULL;
    InputContext* c = calloc(1, sizeof *c); // an asset, not a frame
    if (!c) return NULL;
    disk_of(vp, c->path, sizeof c->path);
    name_of(c->path, c->name);
    c->hash = vhash(vp);
    if (!input_context_save(c)) {
        free(c);
        return NULL;
    }
    I.contexts[I.ncontexts++] = c;
    return c;
}

// Unreal's template input: IA_Move (WASD and the left stick as a 2D vector: x right, y forward), IA_Look (the mouse
// and the right stick), IA_Jump (Space, the bottom face button), all in IMC_Default
static void make_defaults(void) {
    char dir[700];
    snprintf(dir, sizeof dir, "%s/Input", I.root);
    char p[800];
    snprintf(p, sizeof p, "%s/IMC_Default.rimc", dir);
    if (re_file_stat(p, NULL, NULL, NULL) || !re_make_dirs(dir)) return;
    InputAction* mv = input_action_create("/Content/Input", "IA_Move");
    InputAction* lk = input_action_create("/Content/Input", "IA_Look");
    InputAction* jp = input_action_create("/Content/Input", "IA_Jump");
    InputContext* c = input_context_create("/Content/Input", "IMC_Default");
    if (!mv || !lk || !jp || !c) return;
    mv->value_type = lk->value_type = IAV_AXIS2D;
    input_action_save(mv), input_action_save(lk);
    static const struct { const char* action; const char* key; int nm; InMod m[2]; } maps[] = {
        { "IA_Move", "W", 1, { { IMOD_SWIZZLE, 0, { 0 } } } },
        { "IA_Move", "S", 2, { { IMOD_SWIZZLE, 0, { 0 } }, { IMOD_NEGATE, 7, { 0 } } } },
        { "IA_Move", "A", 1, { { IMOD_NEGATE, 7, { 0 } } } },
        { "IA_Move", "D", 0, { { 0 } } },
        { "IA_Move", "Gamepad Left Thumbstick 2D-Axis", 1, { { IMOD_DEADZONE, 0, { 0.2f, 1.0f, 0 } } } },
        { "IA_Look", "Mouse XY 2D-Axis", 0, { { 0 } } },
        { "IA_Look", "Gamepad Right Thumbstick 2D-Axis", 2, { { IMOD_DEADZONE, 0, { 0.2f, 1.0f, 0 } }, { IMOD_SCALAR, 0, { 4.0f, 4.0f, 1.0f } } } },
        { "IA_Jump", "Space", 0, { { 0 } } },
        { "IA_Jump", "Gamepad Face Button Bottom", 0, { { 0 } } },
    };
    for (size_t i = 0; i < sizeof maps / sizeof maps[0]; i++) {
        InputMapping* m = &c->maps[c->nmaps++];
        memset(m, 0, sizeof *m);
        snprintf(m->action, IN_NAME, "%s", maps[i].action);
        snprintf(m->key, IN_NAME, "%s", maps[i].key);
        m->nmods = (uint8_t)maps[i].nm;
        memcpy(m->mods, maps[i].m, sizeof maps[i].m);
    }
    input_context_save(c);
}

void input_init(const char* content_root, bool make_defaults_now) {
    for (uint32_t i = 0; i < I.nactions; i++) free(I.actions[i]);
    for (uint32_t i = 0; i < I.ncontexts; i++) free(I.contexts[i]);
    memset(&I, 0, sizeof I);
    snprintf(I.root, sizeof I.root, "%s", content_root ? content_root : "Content");
    if (make_defaults_now) make_defaults();
}

// --- Play ---

void input_begin_play(void) {
    I.nactive = 0;
    memset(I.st, 0, sizeof I.st);
    memset(I.keys, 0, sizeof I.keys);
    memset(I.prev_keys, 0, sizeof I.prev_keys);
}

void input_add_context(const char* name, int priority) {
    const InputContext* c = input_context_named(name);
    if (!c) {
        printf("[RealEngine] Add Mapping Context: no Input Mapping Context \"%s\"\n", name);
        return;
    }
    for (uint32_t i = 0; i < I.nactive; i++)
        if (I.active[i].c == c) {
            I.active[i].priority = priority;
            return;
        }
    if (I.nactive < MAX_ACTIVE) I.active[I.nactive].c = c, I.active[I.nactive++].priority = priority;
}

void input_remove_context(const char* name) {
    for (uint32_t i = 0; i < I.nactive; i++)
        if (!_stricmp(I.active[i].c->name, name)) {
            I.active[i] = I.active[--I.nactive];
            return;
        }
}

void input_set_devices(const uint8_t* keys, uint32_t nkeys, const float mouse_delta[2], float wheel) {
    memcpy(I.prev_keys, I.keys, sizeof I.keys);
    memset(I.keys, 0, sizeof I.keys);
    if (keys) memcpy(I.keys, keys, nkeys < 512 ? nkeys : 512);
    I.mouse[0] = mouse_delta ? mouse_delta[0] : 0.0f;
    I.mouse[1] = mouse_delta ? mouse_delta[1] : 0.0f;
    I.wheel = wheel;
}

static int action_index(const char* name) {
    for (uint32_t i = 0; i < I.nactions; i++)
        if (!_stricmp(I.actions[i]->name, name)) return (int)i;
    return -1;
}

// A trigger's verdict this frame (Unreal's: the action triggers when every explicit trigger does)
static bool trigger_ok(const InTrig* t, int ai, bool on, bool was_on, float elapsed, float dt) {
    switch (t->kind) {
    case ITRG_PRESSED: return on && !was_on;
    case ITRG_RELEASED: return !on && was_on;
    case ITRG_HOLD: return on && elapsed >= t->time && (!t->once || elapsed - dt < t->time);
    case ITRG_HOLD_RELEASE: return !on && was_on && elapsed >= t->time;
    case ITRG_TAP: return !on && was_on && elapsed < t->time;
    case ITRG_PULSE: {
        if (!on) return false;
        if (!was_on) return true;
        I.st[ai].pulse_t += dt;
        if (I.st[ai].pulse_t >= t->time) {
            I.st[ai].pulse_t -= t->time;
            return true;
        }
        return false;
    }
    default: return on; // Down
    }
}

void input_update(float dt) {
    GLFWgamepadstate gp;
    bool have_gp = glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &gp);
    float acc[MAX_ACTIONS][3];
    bool seen[MAX_ACTIONS];
    int8_t trig_map[MAX_ACTIONS]; // a mapping's own triggers (the last mapping that has some): its index + 1
    const InputMapping* trig_src[MAX_ACTIONS];
    memset(acc, 0, sizeof acc);
    memset(seen, 0, sizeof seen);
    memset(trig_map, 0, sizeof trig_map);
    // the contexts by priority (higher first): their mappings add into their actions; a consuming action's keys are
    // taken from the lower contexts
    uint32_t order[MAX_ACTIVE];
    for (uint32_t i = 0; i < I.nactive; i++) order[i] = i;
    for (uint32_t i = 1; i < I.nactive; i++)
        for (uint32_t k = i; k > 0 && I.active[order[k]].priority > I.active[order[k - 1]].priority; k--) {
            uint32_t t = order[k];
            order[k] = order[k - 1], order[k - 1] = t;
        }
    char consumed[32][IN_NAME];
    uint32_t nconsumed = 0;
    for (uint32_t oi = 0; oi < I.nactive; oi++) {
        const InputContext* c = I.active[order[oi]].c;
        for (uint32_t m = 0; m < c->nmaps; m++) {
            const InputMapping* mp = &c->maps[m];
            const char* key = gus_remapped(mp->action, mp->key); // (the player's key: the options' Controls)
            bool taken = false;
            for (uint32_t k = 0; k < nconsumed && !taken; k++) taken = !_stricmp(consumed[k], key);
            if (taken) continue;
            int ai = action_index(mp->action);
            if (ai < 0) continue;
            float v[3];
            key_value(key, &gp, have_gp, v);
            if (have_gp && !_strnicmp(key, "Gamepad Right Thumbstick", 24)) { // the look's sensitivity and Invert Y (the options)
                const GameUserSettings* us = gus_applied();
                float s = us->sensitivity > 0.0f ? us->sensitivity : 1.0f;
                v[0] *= s, v[1] *= s;
                if (us->invert_y) {
                    if (strstr(key, "2D")) v[1] = -v[1];
                    else if (strstr(key, "Y-Axis")) v[0] = -v[0];
                }
            }
            apply_mods(mp->mods, mp->nmods, v);
            for (int k = 0; k < 3; k++) acc[ai][k] += v[k];
            seen[ai] = true;
            if (mp->ntrig && (fabsf(v[0]) + fabsf(v[1]) + fabsf(v[2]) > 0.0f || !trig_map[ai])) trig_map[ai] = 1, trig_src[ai] = mp;
            if (I.actions[ai]->consume && nconsumed < 32) snprintf(consumed[nconsumed++], IN_NAME, "%s", key);
        }
    }
    for (uint32_t ai = 0; ai < I.nactions; ai++) {
        const InputAction* a = I.actions[ai];
        float v[3] = { acc[ai][0], acc[ai][1], acc[ai][2] };
        apply_mods(a->mods, a->nmods, v);
        if (a->value_type == IAV_BOOL) v[0] = fabsf(v[0]) + fabsf(v[1]) + fabsf(v[2]) > 0.0f ? 1.0f : 0.0f, v[1] = v[2] = 0.0f;
        else if (a->value_type == IAV_AXIS1D) v[1] = v[2] = 0.0f;
        else if (a->value_type == IAV_AXIS2D) v[2] = 0.0f;
        // the triggers: the mapping's when it has some, else the action's, else Down
        const InTrig* trigs = trig_map[ai] ? trig_src[ai]->trig : a->trig;
        uint32_t ntrig = trig_map[ai] ? trig_src[ai]->ntrig : a->ntrig;
        float mag = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        bool on = seen[ai] && (a->value_type == IAV_BOOL ? v[0] > 0.0f : mag > 1e-4f); // actuated
        InSt* s = &I.st[ai];
        s->was_actuated = s->actuated;
        s->actuated = on;
        if (on) s->elapsed += dt;
        bool trig = ntrig == 0 ? on : true;
        for (uint32_t t = 0; t < ntrig; t++) trig = trig && trigger_ok(&trigs[t], (int)ai, on, s->was_actuated, s->elapsed, dt);
        uint32_t ev = 0;
        if (on && !s->was_actuated) ev |= 1u << IEV_STARTED, s->triggered_any = false, s->pulse_t = 0.0f;
        if (trig) ev |= 1u << IEV_TRIGGERED, s->triggered_s += dt, s->triggered_any = true;
        else if (on) ev |= 1u << IEV_ONGOING;
        bool ended = !on && s->was_actuated;
        if (ended || (!trig && s->was_triggered && !on)) ev |= s->triggered_any ? 1u << IEV_COMPLETED : 1u << IEV_CANCELED;
        if (ended && trig) ev |= 1u << IEV_COMPLETED; // (Released / Tap: triggered and completed together)
        s->was_triggered = trig;
        if (!on && !trig) s->elapsed = 0.0f, s->triggered_s = 0.0f;
        memcpy(s->value, v, 12);
        s->events = ev;
    }
}

bool input_action_state(const char* name, float value[3], uint32_t* events, float* elapsed, float* triggered_s) {
    int ai = action_index(name);
    if (ai < 0) {
        if (value) value[0] = value[1] = value[2] = 0.0f;
        if (events) *events = 0;
        return false;
    }
    if (value) memcpy(value, I.st[ai].value, 12);
    if (events) *events = I.st[ai].events;
    if (elapsed) *elapsed = I.st[ai].elapsed;
    if (triggered_s) *triggered_s = I.st[ai].triggered_s;
    return true;
}

// --- The last device used (interaction.c: the prompt's key, the keyboard's or the gamepad's) and a key by its name ---
static int g_last_pad = 0; // 1: the gamepad was the last used
bool input_gamepad_used(void) {
    GLFWgamepadstate gp;
    if (glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &gp)) {
        bool pad = false;
        for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST && !pad; b++) pad = gp.buttons[b] != 0;
        for (int a = 0; a < 4 && !pad; a++) pad = fabsf(gp.axes[a]) > 0.5f; // (the sticks)
        if (pad) g_last_pad = 1;
    } else g_last_pad = 0;
    bool kb = I.mouse[0] != 0.0f || I.mouse[1] != 0.0f;
    for (int k = 0; k < 512 && !kb; k++) kb = I.keys[k] != 0;
    if (kb) g_last_pad = 0;
    return g_last_pad != 0;
}
bool input_key_held(const char* key) {
    if (!key || !key[0]) return false;
    GLFWgamepadstate gp;
    bool have_gp = !_strnicmp(key, "Gamepad", 7) && glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &gp);
    if (!_strnicmp(key, "Gamepad", 7) && !have_gp) return false;
    float v[3];
    key_value(key, &gp, have_gp, v);
    return fabsf(v[0]) > 0.5f;
}
