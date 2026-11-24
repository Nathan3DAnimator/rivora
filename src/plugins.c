// Plugins (B14c): finding, loading and calling them (plugins.h). The registry is fixed arrays (PLUGINS_MAX plugins,
// PLUGIN_FN_MAX functions) filled at startup; a node's call is a lookup by index (its name resolved when the Blueprint
// compiles: plugin_fn_find), then the function under an exception guard on Windows.
#include "plugins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ecs.h"
#include "io_json.h"
#include "platform.h"
#include "platform_files.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef struct { void (*fn)(float, bool, void*); void* user; uint8_t plugin; } Tick;
typedef struct { char label[80]; void (*fn)(void*); void* user; uint8_t plugin; } Menu;

static struct {
    PluginInfo p[PLUGINS_MAX];
    uint32_t n;
    PluginFn f[PLUGIN_FN_MAX];
    uint32_t nf;
    Tick ticks[64];
    uint32_t nticks;
    Menu menus[64];
    uint32_t nmenus;
    char project[700];
    bool editor;
    World* world;
    void (*status)(const char*);
    RePluginApi api;
    int loading; // the plugin whose init runs (its add_* calls are its own)
    bool inited;
} P;

uint32_t plugins_load_folder(const char* root, bool project);

// --- the DLLs ---
static void* dll_open(const char* path) {
#ifdef _WIN32
    wchar_t w[1100];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1100)) return NULL;
    for (wchar_t* c = w; *c; c++) // (LOAD_WITH_ALTERED_SEARCH_PATH wants a full path with backslashes)
        if (*c == L'/') *c = L'\\';
    return (void*)LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH); // (its own dependencies next to it)
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}
static void* dll_sym(void* dll, const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)dll, name);
#else
    return dlsym(dll, name);
#endif
}
static void dll_close(void* dll) {
#ifdef _WIN32
    FreeLibrary((HMODULE)dll);
#else
    dlclose(dll);
#endif
}

// --- the API given to the plugins ---
static int plugin_by_name(const char* name) {
    if (P.loading >= 0 && name && !strcmp(P.p[P.loading].name, name)) return P.loading;
    for (uint32_t i = 0; i < P.n; i++)
        if (name && !strcmp(P.p[i].name, name)) return (int)i;
    return P.loading;
}

static void api_log(const char* plugin, const char* text) {
    printf("[RealEngine] Plugin %s: %s\n", plugin ? plugin : "?", text ? text : "");
    fflush(stdout);
}

static const char* const g_types[] = { "float", "int", "bool", "vector", "rotator", "color" };

// "i:float:A,o:vector:B": the inputs and outputs counted; false: a token that isn't one of these types
static bool parse_pins(const char* spec, uint8_t* nin, uint8_t* nout) {
    *nin = *nout = 0;
    char buf[300];
    snprintf(buf, sizeof buf, "%s", spec ? spec : "");
    for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        if ((tok[0] != 'i' && tok[0] != 'o') || tok[1] != ':') return false;
        char* t = tok + 2;
        char* colon = strchr(t, ':');
        if (!colon || !colon[1]) return false;
        *colon = 0;
        bool known = false;
        for (size_t k = 0; k < sizeof g_types / sizeof g_types[0]; k++) known |= !strcmp(t, g_types[k]);
        if (!known) return false;
        if (tok[0] == 'i') (*nin)++;
        else (*nout)++;
    }
    return *nin <= 8 && *nout <= 4;
}

static bool api_add_function(const char* plugin, const char* name, const char* category, const char* tooltip, const char* pins,
                             bool pure, RePluginFn fn, void* user) {
    int pi = plugin_by_name(plugin);
    if (pi < 0 || !name || !name[0] || strlen(name) > 47 || !fn || P.nf >= PLUGIN_FN_MAX) return false;
    if (plugin_fn_find(name) >= 0) {
        printf("[RealEngine] Plugin %s: the node \"%s\" is taken (another plugin's)\n", P.p[pi].name, name);
        return false;
    }
    PluginFn* f = &P.f[P.nf];
    memset(f, 0, sizeof *f);
    if (!parse_pins(pins, &f->nin, &f->nout)) {
        printf("[RealEngine] Plugin %s: the node \"%s\" has a bad pin list \"%s\" (up to 8 inputs and 4 outputs of float, int, bool, vector, rotator, color)\n",
               P.p[pi].name, name, pins ? pins : "");
        return false;
    }
    snprintf(f->name, sizeof f->name, "%s", name);
    snprintf(f->category, sizeof f->category, "Plugins|%s", category && category[0] ? category : P.p[pi].friendly);
    snprintf(f->tooltip, sizeof f->tooltip, "%s", tooltip ? tooltip : "");
    snprintf(f->pins, sizeof f->pins, "%s", pins ? pins : "");
    f->pure = pure, f->fn = fn, f->user = user, f->plugin = (uint8_t)pi;
    P.nf++;
    P.p[pi].nfuncs++;
    return true;
}

static bool api_add_tick(const char* plugin, void (*fn)(float, bool, void*), void* user) {
    int pi = plugin_by_name(plugin);
    if (pi < 0 || !fn || P.nticks >= 64) return false;
    P.ticks[P.nticks++] = (Tick){ fn, user, (uint8_t)pi };
    P.p[pi].nticks++;
    return true;
}

static bool api_add_menu_item(const char* plugin, const char* label, void (*fn)(void*), void* user) {
    int pi = plugin_by_name(plugin);
    if (pi < 0 || !fn || !label || P.nmenus >= 64) return false;
    Menu* m = &P.menus[P.nmenus++];
    snprintf(m->label, sizeof m->label, "%s", label);
    m->fn = fn, m->user = user, m->plugin = (uint8_t)pi;
    P.p[pi].nmenus++;
    return true;
}

static void api_status(const char* text) {
    if (P.status) P.status(text ? text : "");
    else api_log("status", text);
}
static double api_time(void) { return re_time_seconds(); }
static uint32_t api_actor_count(void) { return P.world ? P.world->alive : 0; }
static bool api_in_editor(void) { return P.editor; }

// --- finding them ---
static char* read_text(const char* path, size_t* len) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = n > 0 && n < (1 << 20) ? malloc((size_t)n + 1) : NULL;
    if (t && fread(t, 1, (size_t)n, f) != (size_t)n) free(t), t = NULL;
    fclose(f);
    if (t) t[n] = 0, *len = (size_t)n;
    return t;
}

static bool ini_enabled(const char* name, bool* out) { // <project>/Config/Plugins.ini: "Name=1"
    char path[800], exe[700];
    if (P.project[0]) snprintf(path, sizeof path, "%s/Config/Plugins.ini", P.project);
    else if (!P.editor && re_exe_dir(exe, sizeof exe)) snprintf(path, sizeof path, "%s/Config/Plugins.ini", exe); // (the packaged game's)
    else return false;
    size_t len;
    char* t = read_text(path, &len);
    if (!t) return false;
    bool found = false;
    size_t nl = strlen(name);
    for (char* line = t; line && *line;) {
        char* e = strchr(line, '\n');
        if (!strncmp(line, name, nl) && line[nl] == '=') *out = line[nl + 1] == '1', found = true;
        line = e ? e + 1 : NULL;
    }
    free(t);
    return found;
}

typedef struct { const char* root; bool project; } ScanCtx;

static void scan_one(void* user, const ReDirEntry* de) {
    const ScanCtx* c = user;
    const char* entry = de->name;
    if (!de->is_dir || P.n >= PLUGINS_MAX || entry[0] == '.') return;
    char path[900];
    snprintf(path, sizeof path, "%s/%s/plugin.json", c->root, entry);
    size_t len;
    char* text = read_text(path, &len);
    if (!text) return;
    for (uint32_t i = 0; i < P.n; i++) // (the project's copy wins over the engine's of the same name)
        if (!strcmp(P.p[i].name, entry)) {
            free(text);
            return;
        }
    PluginInfo* p = &P.p[P.n];
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", entry);
    snprintf(p->dir, sizeof p->dir, "%s/%s", c->root, entry);
    p->project = c->project;
    JsonDoc doc;
    char err[160];
    bool def = false;
    if (json_parse(text, len, &doc, err, sizeof err)) {
        const JsonValue* r = &doc.root;
        snprintf(p->friendly, sizeof p->friendly, "%s", json_get_str(r, "FriendlyName", entry));
        snprintf(p->version, sizeof p->version, "%s", json_get_str(r, "VersionName", "1.0"));
        snprintf(p->author, sizeof p->author, "%s", json_get_str(r, "CreatedBy", ""));
        snprintf(p->category, sizeof p->category, "%s", json_get_str(r, "Category", "Other"));
        snprintf(p->description, sizeof p->description, "%s", json_get_str(r, "Description", ""));
        snprintf(p->module, sizeof p->module, "%s", json_get_str(r, "Module", entry));
        def = json_get_bool(r, "EnabledByDefault", true);
        json_free(&doc);
    } else {
        snprintf(p->friendly, sizeof p->friendly, "%s", entry);
        snprintf(p->error, sizeof p->error, "plugin.json: %s", err);
    }
    free(text);
    bool en = def;
    ini_enabled(p->name, &en);
    const char* force = getenv("REALENGINE_PLUGINS_ENABLE"); // (the tests: "ExamplePlugin,Other")
    if (force) {
        size_t nl = strlen(p->name);
        for (const char* s = strstr(force, p->name); s; s = strstr(s + 1, p->name))
            if ((s == force || s[-1] == ',') && (s[nl] == 0 || s[nl] == ',')) en = true;
    }
    p->enabled = p->enabled_at_start = en;
    P.n++;
}

static void load_one(uint32_t i) {
    PluginInfo* p = &P.p[i];
    if (p->error[0] || !p->enabled) return;
    double t0 = re_time_seconds();
    char path[900];
#ifdef _WIN32
    snprintf(path, sizeof path, "%s/%s.dll", p->dir, p->module);
#else
    snprintf(path, sizeof path, "%s/lib%s.so", p->dir, p->module);
#endif
    p->dll = dll_open(path);
    if (!p->dll) {
        snprintf(p->error, sizeof p->error, "its library %s.dll could not be loaded (missing, or built for another platform)", p->module);
        return;
    }
    RePluginInitFn init = (RePluginInitFn)dll_sym(p->dll, "re_plugin_init");
    p->shutdown = (RePluginShutdownFn)dll_sym(p->dll, "re_plugin_shutdown");
    if (!init) {
        snprintf(p->error, sizeof p->error, "%s", "its library has no re_plugin_init");
        dll_close(p->dll), p->dll = NULL;
        return;
    }
    P.api.plugin = p->name;
    P.loading = (int)i;
    int rc = -1;
#ifdef _MSC_VER
    __try {
        rc = init(&P.api);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rc = -2;
    }
#else
    rc = init(&P.api);
#endif
    P.loading = -1;
    p->load_ms = (re_time_seconds() - t0) * 1000.0;
    if (rc != 0) {
        snprintf(p->error, sizeof p->error, rc == -2 ? "its init crashed" : "its init returned %d", rc);
        // (what it registered stays out: its functions, ticks and menus dropped)
        uint32_t k = 0;
        for (uint32_t j = 0; j < P.nf; j++)
            if (P.f[j].plugin != i) P.f[k++] = P.f[j];
        P.nf = k, p->nfuncs = 0;
        k = 0;
        for (uint32_t j = 0; j < P.nticks; j++)
            if (P.ticks[j].plugin != i) P.ticks[k++] = P.ticks[j];
        P.nticks = k, k = 0;
        for (uint32_t j = 0; j < P.nmenus; j++)
            if (P.menus[j].plugin != i) P.menus[k++] = P.menus[j];
        P.nmenus = k;
        dll_close(p->dll), p->dll = NULL, p->shutdown = NULL;
        return;
    }
    p->loaded = true;
    printf("[RealEngine] Plugin %s %s loaded (%s): %u nodes, %u ticks, %u menu items, %.1f ms\n", p->name, p->version,
           p->project ? "project" : "engine", p->nfuncs, p->nticks, p->nmenus, p->load_ms);
}

// The built-in plugins (no DLL): listed with the others, their state read from the project's Plugins.ini (on by default)
static bool g_gameplay = true;
bool plugins_gameplay_enabled(void) { return g_gameplay; }

static void builtin_refresh(void) {
    int i = plugin_by_name(PLUGIN_GAMEPLAY);
    if (i < 0 && P.n < PLUGINS_MAX) {
        PluginInfo* p = &P.p[P.n];
        memset(p, 0, sizeof *p);
        snprintf(p->name, sizeof p->name, "%s", PLUGIN_GAMEPLAY);
        snprintf(p->friendly, sizeof p->friendly, "Gameplay Framework");
        snprintf(p->version, sizeof p->version, "%s", RE_VERSION_STRING);
        snprintf(p->author, sizeof p->author, "Rivora Engine");
        snprintf(p->category, sizeof p->category, "Gameplay");
        snprintf(p->description, sizeof p->description,
                 "The ready-made game modules: Health and damage, Inventory and items, Interaction (doors, chests, levers), "
                 "Pause Menu and Options, Save Progress and checkpoints, Dialogue, Quests, Game Rules, Abilities and effects, "
                 "the modular HUD. Off: their nodes, components, actors and assets leave the menus.");
        p->builtin = p->loaded = true;
        i = (int)P.n++;
    }
    if (i < 0) return;
    bool en = true;
    ini_enabled(PLUGIN_GAMEPLAY, &en);
    P.p[i].enabled = P.p[i].enabled_at_start = en;
    if (g_gameplay != en || !en) printf("[RealEngine] Plugin %s (built-in): %s\n", PLUGIN_GAMEPLAY, en ? "on" : "off");
    g_gameplay = en;
}

void plugins_init(const char* project_dir, bool editor) {
    // Once for the engine's plugins (and the project's when given); called again with a project (opened later): that
    // project's plugins added (the engine's stay as they loaded: a change waits for a restart, as Unreal's)
    char dir[900];
    if (!P.inited) {
        P.inited = true;
        P.editor = editor;
        P.loading = -1;
        P.api = (RePluginApi){ RE_PLUGIN_API_VERSION, sizeof(RePluginApi), NULL, api_log, api_add_function, api_add_tick, api_add_menu_item,
                               api_status, api_time, api_actor_count, api_in_editor };
        if (project_dir && project_dir[0]) {
            snprintf(P.project, sizeof P.project, "%s", project_dir);
            snprintf(dir, sizeof dir, "%s/Plugins", P.project);
            plugins_load_folder(dir, true);
        }
        char exe[800];
        if (re_exe_dir(exe, sizeof exe)) {
            snprintf(dir, sizeof dir, "%s/Plugins", exe);
            plugins_load_folder(dir, false);
        }
        builtin_refresh();
        return;
    }
    if (project_dir && project_dir[0] && strcmp(P.project, project_dir)) {
        snprintf(P.project, sizeof P.project, "%s", project_dir);
        snprintf(dir, sizeof dir, "%s/Plugins", P.project);
        plugins_load_folder(dir, true);
    }
    builtin_refresh(); // (another project: its own choice)
}

void plugins_shutdown(void) {
    for (uint32_t i = 0; i < P.n; i++) {
        PluginInfo* p = &P.p[i];
        if (p->loaded && p->shutdown && !p->faulted) p->shutdown();
        // (the DLLs stay mapped until the process ends: a pointer of theirs somewhere can't outlive them)
    }
    P.nticks = 0, P.nmenus = 0;
}

void plugins_set_world(World* w) { P.world = w; }
void plugins_set_status(void (*fn)(const char* text)) { P.status = fn; }

static void fault(uint8_t pi, const char* what) {
    PluginInfo* p = &P.p[pi];
    if (p->faulted) return;
    p->faulted = true;
    snprintf(p->error, sizeof p->error, "crashed in %s: disabled until the next start", what);
    printf("[RealEngine] Plugin %s %s\n", p->name, p->error);
    if (P.status) {
        char s[300];
        snprintf(s, sizeof s, "Plugin %s %s", p->name, p->error);
        P.status(s);
    }
}

void plugins_tick(float dt, bool playing) {
    for (uint32_t i = 0; i < P.nticks; i++) {
        Tick* t = &P.ticks[i];
        if (P.p[t->plugin].faulted) continue;
#ifdef _MSC_VER
        __try {
            t->fn(dt, playing, t->user);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            fault(t->plugin, "its tick");
        }
#else
        t->fn(dt, playing, t->user);
#endif
    }
}

uint32_t plugin_count(void) { return P.n; }
const PluginInfo* plugin_at(uint32_t i) { return i < P.n ? &P.p[i] : NULL; }

void plugin_set_enabled(uint32_t i, bool enabled) {
    if (i >= P.n || !P.project[0]) return;
    P.p[i].enabled = enabled;
    if (P.p[i].builtin) { // (part of the engine: at once)
        P.p[i].enabled_at_start = enabled;
        if (!strcmp(P.p[i].name, PLUGIN_GAMEPLAY)) g_gameplay = enabled;
    }
    char dir[800], path[900];
    snprintf(dir, sizeof dir, "%s/Config", P.project);
    re_make_dirs(dir);
    snprintf(path, sizeof path, "%s/Plugins.ini", dir);
    FILE* f = re_fopen(path, "wb");
    if (!f) return;
    fputs("; Edit > Plugins: the plugins of this project, enabled (1) or not (0). A change counts at the next start.\n", f);
    for (uint32_t k = 0; k < P.n; k++) fprintf(f, "%s=%d\n", P.p[k].name, P.p[k].enabled ? 1 : 0);
    fclose(f);
}

uint32_t plugin_fn_count(void) { return P.nf; }
const PluginFn* plugin_fn_at(uint32_t i) { return i < P.nf ? &P.f[i] : NULL; }
int plugin_fn_find(const char* name) {
    if (!name) return -1;
    for (uint32_t i = 0; i < P.nf; i++)
        if (!strcmp(P.f[i].name, name)) return (int)i;
    return -1;
}

bool plugin_fn_call(int fi, const float (*in)[4], uint32_t nin, float (*out)[4], uint32_t nout) {
    for (uint32_t k = 0; k < nout; k++) out[k][0] = out[k][1] = out[k][2] = out[k][3] = 0.0f;
    if (fi < 0 || (uint32_t)fi >= P.nf) return false;
    PluginFn* f = &P.f[fi];
    if (P.p[f->plugin].faulted) return false;
    RePluginValue a[8] = { 0 }, r[4] = { 0 };
    for (uint32_t k = 0; k < nin && k < 8; k++) memcpy(a[k].v, in[k], sizeof a[k].v);
#ifdef _MSC_VER
    __try {
        f->fn(a, r, f->user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fault(f->plugin, f->name);
        return false;
    }
#else
    f->fn(a, r, f->user);
#endif
    for (uint32_t k = 0; k < nout && k < 4; k++) memcpy(out[k], r[k].v, sizeof r[k].v);
    return true;
}

uint32_t plugin_menu_count(void) { return P.nmenus; }
const char* plugin_menu_label(uint32_t i) { return i < P.nmenus ? P.menus[i].label : ""; }
void plugin_menu_run(uint32_t i) {
    if (i >= P.nmenus || P.p[P.menus[i].plugin].faulted) return;
    Menu* m = &P.menus[i];
#ifdef _MSC_VER
    __try {
        m->fn(m->user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fault(m->plugin, m->label);
    }
#else
    m->fn(m->user);
#endif
}

int plugins_selftest_report(void) {
    int fail = 0;
    const PluginInfo* ex = NULL;
    for (uint32_t i = 0; i < P.n; i++)
        if (!strcmp(P.p[i].name, "ExamplePlugin")) ex = &P.p[i];
    printf("[RealEngine] Plugins: %u found, %u nodes; ExamplePlugin %s%s%s\n", P.n, P.nf, ex && ex->loaded ? "loaded" : "NOT LOADED",
           ex && ex->error[0] ? ": " : "", ex ? ex->error : "");
    if (!ex || !ex->loaded) fail = 1;
    int ss = plugin_fn_find("Smooth Step");
    float in[3][4] = { { 0 }, { 10 }, { 5 } }, out[1][4];
    bool ok = plugin_fn_call(ss, (const float(*)[4])in, 3, out, 1);
    printf("[RealEngine] Plugin call Smooth Step(0, 10, 5) = %.3f (%s)\n", out[0][0], ok && out[0][0] == 0.5f ? "ok" : "WRONG");
    if (!ok || out[0][0] != 0.5f) fail = 1;
    // a crash inside a plugin's function: the plugin disabled, the engine goes on
    int cr = plugin_fn_find("Crash Test");
    bool crashed = cr >= 0 && !plugin_fn_call(cr, NULL, 0, out, 1) && ex && ex->faulted;
    printf("[RealEngine] Plugin crash guard: %s\n", crashed ? "the plugin was disabled, the engine went on" : "FAIL: not caught");
    if (!crashed) fail = 1;
    return fail;
}

void plugin_fn_spec(int fi, bool exec, char* out, size_t cap) {
    out[0] = 0;
    const PluginFn* f = plugin_fn_at(fi < 0 ? 0xFFFFFFFFu : (uint32_t)fi);
    if (!f || cap < 2) return;
    size_t o = 0;
    if (exec) o += (size_t)snprintf(out + o, cap - o, "i:exec:");
    for (int pass = 0; pass < 2; pass++) { // (the inputs, then Exec out, then the outputs: the engine's own nodes' order)
        if (pass == 1 && exec && o < cap) o += (size_t)snprintf(out + o, cap - o, "%so:exec:", o ? "," : "");
        char buf[300];
        snprintf(buf, sizeof buf, "%s", f->pins);
        for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            while (*tok == ' ') tok++;
            if ((tok[0] == 'i') != (pass == 0) || o >= cap) continue;
            o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? "," : "", tok);
        }
    }
}

// (the tests, a plugin built meanwhile) every plugin under root not yet known, loaded
uint32_t plugins_load_folder(const char* root, bool project) {
    uint32_t first = P.n;
    re_dir_walk(root, scan_one, &(ScanCtx){ root, project });
    for (uint32_t i = first; i < P.n; i++) load_one(i);
    return P.n - first;
}
