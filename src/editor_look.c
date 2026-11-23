// The editor's look of 0.44 (editor_look.h, notas-dev/temas-e-janelas.md "Q26"): the brand and the readouts of the menu
// bar, the target platforms and their readiness, the status bar's right side, the Platform Support panel, the window's
// title bar in the theme's color and the one-time offer of the new default look. Every readout is measured (the frame's
// stats, the pipelines and materials still compiling, the derived data cache's size on disk, the SDKs found), every
// button does what it says; the slow parts (the cache's size, the Android devices) run on a thread of their own.
#include "editor_look.h"

#include "editor.h"
#include "theme.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

#include "brand.h"
#include "debug_viewer.h"
#include "editor_dock.h"
#include "gi.h"
#include "i18n.h"
#include "material.h"
#include "platform.h"
#include "platform_files.h"
#include "project.h"
#include "render.h"
#include "ui_icons.h"
#include "ui_tips.h"
#include "vkctx.h"

enum { LOOK_VERSION = 2 }; // (1: the Órbita look of 0.43; 2: Azul Real, the mockups' layout)

static struct {
    int target;            // LOOK_PC...
    int seen_version;      // look= in editor.ini (0: an editor.ini from before 0.44)
    bool have_pref;
    bool offer, switched;  // the card: offering the new look / it was switched to (go back offered)
    bool keep_layout;      // false: the new default layout replaces the saved one this run
    char prev_theme[THEME_NAME];
    char prev_layout[900]; // the layout the new look replaced, kept as a user layout
    bool platforms_open;
    uint32_t theme_gen;    // the title bar's color follows the theme
} L = { LOOK_PC, 0, false, false, false, true, "", "", false, 0 };

int look_target(void) { return L.target; }
void look_set_target(int t) {
    if (t >= 0 && t < LOOK_TARGET_COUNT) L.target = t;
}
const char* look_target_name(int t) {
    static const char* const n[LOOK_TARGET_COUNT] = { "PC", "Android", "VR" };
    return t >= 0 && t < LOOK_TARGET_COUNT ? n[t] : "";
}
bool* look_platforms_flag(void) { return &L.platforms_open; }
static bool g_modes_open;
bool* look_modes_flag(void) { return &g_modes_open; }

// --- Small parts --------------------------------------------------------------------------------------------------
bool look_primary_button(const char* id, int icon, const char* label, const char* tip) {
    igPushStyleColor_U32(ImGuiCol_Button, TH(TK_ACCENT));
    igPushStyleColor_U32(ImGuiCol_ButtonHovered, theme_mix(TH(TK_ACCENT), TH(TK_TEXT_ON_ACCENT), 0.12f));
    igPushStyleColor_U32(ImGuiCol_ButtonActive, TH(TK_ACCENT_STRONG));
    igPushStyleColor_U32(ImGuiCol_Text, TH(TK_TEXT_ON_ACCENT));
    bool r = ui_icon_button(id, (UiIcon)icon, label, TH(TK_TEXT_ON_ACCENT), false, tip);
    igPopStyleColor(4);
    return r;
}

bool look_pill(const char* id, const char* text, uint32_t bg_tok, uint32_t text_tok, const char* tip) {
    ImVec2 ts = igCalcTextSize(text, NULL, false, -1.0f);
    float h = igGetFrameHeight(), padx = h * 0.45f;
    ImVec2 p = igGetCursorScreenPos();
    bool hit = igInvisibleButton(id, (ImVec2){ ts.x + padx * 2.0f, h }, 0);
    bool hov = igIsItemHovered(0);
    if (tip) ui_tip_item(-1, text, tip, NULL);
    ImDrawList* dl = igGetWindowDrawList();
    uint32_t bg = TH((int)bg_tok);
    if (hov) bg = theme_mix(bg, TH((int)text_tok), 0.12f);
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + ts.x + padx * 2.0f, p.y + h }, bg, h * 0.5f, 0);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + padx, p.y + (h - ts.y) * 0.5f }, TH((int)text_tok), text, NULL);
    return hit;
}

// --- Readiness: what each target has and lacks (a few file lookups every few seconds) ----------------------------
enum { CK_OK, CK_WARN, CK_MISSING };
enum { ACT_NONE, ACT_PACKAGING, ACT_PROJECT, ACT_FOLDER };
typedef struct LookCheck { uint8_t state, action; char text[240]; char path[400]; } LookCheck;
static struct {
    LookCheck c[LOOK_TARGET_COUNT][10];
    int n[LOOK_TARGET_COUNT], missing[LOOK_TARGET_COUNT], warn[LOOK_TARGET_COUNT];
    double at;
    char sdk[600], runtime[200];
    bool have_sdk;
} K;

static bool env_utf8(const char* name, char* out, size_t cap) {
    out[0] = 0;
#ifdef _WIN32
    wchar_t wn[64], wv[1024];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 64);
    DWORD n = GetEnvironmentVariableW(wn, wv, 1024);
    if (!n || n >= 1024) return false;
    return WideCharToMultiByte(CP_UTF8, 0, wv, -1, out, (int)cap, NULL, NULL) > 0 && out[0];
#else
    const char* v = getenv(name);
    if (!v || !v[0]) return false;
    snprintf(out, cap, "%s", v);
    return true;
#endif
}
static bool is_file(const char* p) {
    uint64_t sz;
    int64_t mt;
    bool dir;
    return re_file_stat(p, &sz, &mt, &dir) && !dir;
}
static bool is_dir(const char* p) {
    uint64_t sz;
    int64_t mt;
    bool dir;
    return re_file_stat(p, &sz, &mt, &dir) && dir;
}
typedef struct DirCount { int dirs; const char* prefix; char first[260]; } DirCount;
static void count_dir(void* user, const ReDirEntry* e) {
    DirCount* d = user;
    if (!e->is_dir || (d->prefix && _strnicmp(e->name, d->prefix, strlen(d->prefix)) != 0)) return;
    if (!d->dirs) snprintf(d->first, sizeof d->first, "%s", e->name);
    d->dirs++;
}
static int subdirs(const char* dir, const char* prefix, char* first, size_t cap) {
    DirCount d = { 0, prefix, "" };
    re_dir_walk(dir, count_dir, &d);
    if (first) snprintf(first, cap, "%s", d.first);
    return d.dirs;
}
// The engine's android folder (the packager's: beside the executable, or a build's)
static bool engine_android_dir(char* out, size_t cap) {
    char exe[700], p[900];
    if (!re_exe_dir(exe, sizeof exe)) return false;
    static const char* const rel[] = { "android", "..\\android", "..\\..\\android" };
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof p, "%s\\%s\\package_apk.ps1", exe, rel[i]);
        if (is_file(p)) {
            snprintf(out, cap, "%s\\%s", exe, rel[i]);
            return true;
        }
    }
    return false;
}
// The OpenXR runtime the system has (HKLM\SOFTWARE\Khronos\OpenXR\1 ActiveRuntime): its name, from its manifest
static bool openxr_runtime(char* name, size_t cap) {
    name[0] = 0;
#ifdef _WIN32
    char json[1024] = "";
    DWORD n = sizeof json;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime", RRF_RT_REG_SZ, NULL, json, &n) != ERROR_SUCCESS || !json[0])
        return false;
    FILE* f = fopen(json, "rb");
    char text[4096] = "";
    if (f) {
        size_t r = fread(text, 1, sizeof text - 1, f);
        text[r] = 0;
        fclose(f);
    }
    const char* k = strstr(text, "\"name\"");
    if (k && (k = strchr(k + 6, ':')) != NULL && (k = strchr(k, '"')) != NULL) {
        size_t i = 0;
        for (k++; *k && *k != '"' && i + 1 < cap; k++) name[i++] = *k;
        name[i] = 0;
    }
    if (!name[0]) { // (no name in it: the manifest's file)
        const char* b = strrchr(json, '\\');
        snprintf(name, cap, "%s", b ? b + 1 : json);
    }
    return true;
#else
    (void)cap;
    return false;
#endif
}

static LookCheck* add_check(int t, int state, int action, const char* path, const char* fmt, ...) {
    if (K.n[t] >= 10) return NULL;
    LookCheck* c = &K.c[t][K.n[t]++];
    c->state = (uint8_t)state, c->action = (uint8_t)action;
    snprintf(c->path, sizeof c->path, "%s", path ? path : "");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->text, sizeof c->text, fmt, ap);
    va_end(ap);
    if (state == CK_MISSING) K.missing[t]++;
    if (state == CK_WARN) K.warn[t]++;
    return c;
}

static void refresh_checks(Editor* ed) {
    memset(K.n, 0, sizeof K.n), memset(K.missing, 0, sizeof K.missing), memset(K.warn, 0, sizeof K.warn);
    const ProjectSettings* ps = project();
    bool pkg = ps->package_dir[0] != 0;
    // PC (Windows): the GPU this editor runs on, ray tracing, where the game is packaged
    add_check(LOOK_PC, CK_OK, ACT_NONE, NULL, TR("GPU: %s (Vulkan 1.3)"), ed->vk ? ed->vk->gpu_props.deviceName : "?");
    if (gi_available()) add_check(LOOK_PC, CK_OK, ACT_NONE, NULL, "%s", TR("Hardware ray tracing: on (RealGI, ray traced shadows and reflections)"));
    else add_check(LOOK_PC, CK_WARN, ACT_NONE, NULL, "%s", TR("No hardware ray tracing: the GI and the shadows use their software paths"));
    if (pkg) add_check(LOOK_PC, CK_OK, ACT_FOLDER, ps->package_dir, TR("Package folder: %s"), ps->package_dir);
    else add_check(LOOK_PC, CK_WARN, ACT_PACKAGING, NULL, "%s", TR("No package folder yet: Package Game asks for one"));
    // Android: the packager, the SDK (build-tools, a platform), a JDK, the engine's native part (prebuilt or the NDK)
    char adir[700], p[1000];
    bool script = engine_android_dir(adir, sizeof adir);
    if (script) add_check(LOOK_ANDROID, CK_OK, ACT_NONE, NULL, "%s", TR("The Android packager is installed"));
    else add_check(LOOK_ANDROID, CK_MISSING, ACT_NONE, NULL, "%s", TR("The Android packager (android/package_apk.ps1) is missing from the engine's folder"));
    char lad[600];
    K.have_sdk = env_utf8("ANDROID_HOME", K.sdk, sizeof K.sdk) && is_dir(K.sdk);
    if (!K.have_sdk && env_utf8("LOCALAPPDATA", lad, sizeof lad)) {
        snprintf(K.sdk, sizeof K.sdk, "%s\\Android\\Sdk", lad);
        K.have_sdk = is_dir(K.sdk);
    }
    if (!K.have_sdk) {
        add_check(LOOK_ANDROID, CK_MISSING, ACT_NONE, NULL, "%s", TR("Android SDK not found (ANDROID_HOME or %LOCALAPPDATA%\\Android\\Sdk): install it with Android Studio or the cmdline-tools"));
    } else {
        char bt[260] = "", pl[260] = "";
        snprintf(p, sizeof p, "%s\\build-tools", K.sdk);
        int nbt = subdirs(p, NULL, bt, sizeof bt);
        snprintf(p, sizeof p, "%s\\platforms", K.sdk);
        int npl = subdirs(p, "android-", pl, sizeof pl);
        if (nbt && npl) add_check(LOOK_ANDROID, CK_OK, ACT_FOLDER, K.sdk, TR("Android SDK: build-tools %s, %s"), bt, pl);
        else add_check(LOOK_ANDROID, CK_MISSING, ACT_FOLDER, K.sdk, "%s", TR("The Android SDK has no build-tools or platform: sdkmanager \"build-tools;35.0.0\" \"platforms;android-35\""));
    }
    char jdk[600] = "";
    bool have_jdk = env_utf8("JAVA_HOME", jdk, sizeof jdk) && is_dir(jdk);
    static const char* const jroots[] = { "C:\\Program Files\\Microsoft", "C:\\Program Files\\Java", "C:\\Program Files\\Eclipse Adoptium" };
    for (int i = 0; i < 3 && !have_jdk; i++) {
        char first[260];
        if (subdirs(jroots[i], "jdk", first, sizeof first)) snprintf(jdk, sizeof jdk, "%s\\%s", jroots[i], first), have_jdk = true;
    }
    if (have_jdk) add_check(LOOK_ANDROID, CK_OK, ACT_NONE, NULL, TR("JDK: %s"), jdk);
    else add_check(LOOK_ANDROID, CK_MISSING, ACT_NONE, NULL, "%s", TR("No JDK found (JAVA_HOME): install a JDK 17 or newer"));
    snprintf(p, sizeof p, "%s\\prebuilt\\librealengine_game.so", adir);
    char ndkdir[700];
    snprintf(ndkdir, sizeof ndkdir, "%s\\ndk", K.sdk);
    if (script && is_file(p)) add_check(LOOK_ANDROID, CK_OK, ACT_NONE, NULL, "%s", TR("The engine's Android part is ready (prebuilt)"));
    else if (K.have_sdk && subdirs(ndkdir, NULL, NULL, 0)) add_check(LOOK_ANDROID, CK_WARN, ACT_NONE, NULL, "%s", TR("The engine's Android part is built on the first package (the NDK was found: a few minutes)"));
    else add_check(LOOK_ANDROID, CK_MISSING, ACT_NONE, NULL, "%s", TR("The engine's Android part is missing: no prebuilt librealengine_game.so and no NDK to build it"));
    if (pkg) add_check(LOOK_ANDROID, CK_OK, ACT_FOLDER, ps->package_dir, TR("Package folder: %s"), ps->package_dir);
    else add_check(LOOK_ANDROID, CK_MISSING, ACT_PACKAGING, NULL, "%s", TR("Choose the package folder in Packaging Settings"));
    if (ps->android_keystore[0] && is_file(ps->android_keystore)) add_check(LOOK_ANDROID, CK_OK, ACT_NONE, NULL, "%s", TR("Google Play upload key: set"));
    else add_check(LOOK_ANDROID, CK_WARN, ACT_PROJECT, NULL, "%s", TR("Google Play needs an upload key (Project Settings > Android > Create Upload Key); the APK doesn't"));
    // VR: the OpenXR runtime (PC VR: Quest Link / Air Link, SteamVR), the project's VR platforms, Meta Quest = Android
    if (openxr_runtime(K.runtime, sizeof K.runtime)) add_check(LOOK_VR, CK_OK, ACT_NONE, NULL, TR("OpenXR runtime: %s"), K.runtime);
    else add_check(LOOK_VR, CK_MISSING, ACT_NONE, NULL, "%s", TR("No OpenXR runtime: install the Meta Quest app (Link) or SteamVR for VR Preview and PC VR"));
    if (ps->vr.pc || ps->vr.quest)
        add_check(LOOK_VR, CK_OK, ACT_PROJECT, NULL, TR("Project targets: %s%s%s"), ps->vr.pc ? "PC VR" : "", ps->vr.pc && ps->vr.quest ? ", " : "", ps->vr.quest ? "Meta Quest" : "");
    else add_check(LOOK_VR, CK_WARN, ACT_PROJECT, NULL, "%s", TR("The project has no VR platform on (Project Settings > VR)"));
    if (K.missing[LOOK_ANDROID]) add_check(LOOK_VR, CK_WARN, ACT_NONE, NULL, "%s", TR("Meta Quest (standalone) packages through Android: see Android"));
    else add_check(LOOK_VR, CK_OK, ACT_NONE, NULL, "%s", TR("Meta Quest (standalone): Android is ready"));
    K.at = re_time_seconds();
}

int look_target_missing(int t) { return t >= 0 && t < LOOK_TARGET_COUNT ? K.missing[t] : 0; }
static uint32_t ready_tok(int t) { return K.missing[t] ? TK_ERROR : K.warn[t] ? TK_WARN : TK_OK; }

// --- The derived data cache's size (a thread walks %LOCALAPPDATA%\RealEngine: pipelines, meshes, impostors...) -----
static struct {
    ReThread* th;
    volatile int32_t done;
    uint64_t bytes, result;
    bool have;
    double next;
    char dir[600];
} D;
static void dd_walk(const char* dir, int depth);
static void dd_one(void* user, const ReDirEntry* e) {
    const char* dir = user;
    if (e->is_dir) {
        char sub[1200];
        snprintf(sub, sizeof sub, "%s\\%s", dir, e->name);
        dd_walk(sub, 1);
    } else D.bytes += e->size;
}
static void dd_walk(const char* dir, int depth) {
    if (depth > 12) return;
    re_dir_walk(dir, dd_one, (void*)dir);
}
static int dd_thread(void* arg) {
    (void)arg;
    D.bytes = 0;
    dd_walk(D.dir, 0);
    D.result = D.bytes;
    re_atomic_store(&D.done, 1);
    return 0;
}
static void dd_poll(double now) {
    if (D.th && re_atomic_load(&D.done)) {
        re_thread_join(D.th);
        D.th = NULL, D.have = true, D.next = now + 60.0;
    }
    if (!D.th && now >= D.next && re_cache_dir(D.dir, sizeof D.dir)) {
        re_atomic_store(&D.done, 0);
        D.th = re_thread_start(dd_thread, NULL);
        if (!D.th) D.next = now + 60.0;
    }
}

// --- The Android devices connected (adb devices -l, on a thread when asked) ----------------------------------------
static struct {
    ReThread* th;
    volatile int32_t done;
    bool ran, adb_missing;
    int n;
    char dev[8][160];
    char adb[700], log[700];
} A;
static int adb_thread(void* arg) {
    (void)arg;
    char cmd[1600];
    snprintf(cmd, sizeof cmd, "\"%s\" devices -l", A.adb);
    int rc = re_run_command(cmd, A.log);
    A.n = 0;
    FILE* f = rc == 0 ? re_fopen(A.log, "r") : NULL;
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "List of", 7) || line[0] == '*' || line[0] == '\r' || line[0] == '\n' || A.n >= 8) continue;
        char serial[96] = "", state[32] = "", model[96] = "";
        if (sscanf(line, "%95s %31s", serial, state) < 2) continue;
        const char* m = strstr(line, "model:");
        if (m) sscanf(m + 6, "%95s", model);
        for (char* c = model; *c; c++)
            if (*c == '_') *c = ' ';
        snprintf(A.dev[A.n++], sizeof A.dev[0], "%s  (%s, %s)", model[0] ? model : serial, serial, state);
    }
    if (f) fclose(f);
    re_atomic_store(&A.done, 1);
    return 0;
}
static void adb_refresh(void) {
    if (A.th) return;
    char tmp[600];
    A.adb_missing = true;
    if (!K.have_sdk) return;
    snprintf(A.adb, sizeof A.adb, "%s\\platform-tools\\adb.exe", K.sdk);
    if (!is_file(A.adb)) return;
    A.adb_missing = false;
    if (!env_utf8("TEMP", tmp, sizeof tmp)) snprintf(tmp, sizeof tmp, ".");
    snprintf(A.log, sizeof A.log, "%s\\realengine_adb_devices.txt", tmp);
    re_atomic_store(&A.done, 0);
    A.th = re_thread_start(adb_thread, NULL);
}
static void adb_poll(void) {
    if (A.th && re_atomic_load(&A.done)) re_thread_join(A.th), A.th = NULL, A.ran = true;
}

// --- The main menu bar ----------------------------------------------------------------------------------------------
static float bar_pad(void) { return floorf(igGetFontSize() * 0.55f); }
float look_menu_bar_height(void) { return igGetFontSize() + 2.0f * bar_pad(); }
void look_menu_bar_push(void) {
    igPushStyleVar_Vec2(ImGuiStyleVar_FramePadding, (ImVec2){ 8.0f, bar_pad() });
    igPushStyleColor_U32(ImGuiCol_MenuBarBg, TH(TK_BG_APP));
}
void look_menu_bar_pop(void) {
    igPopStyleColor(1);
    igPopStyleVar(1);
}

void look_menu_brand(Editor* ed) {
    (void)ed;
    ImDrawList* dl = igGetWindowDrawList();
    float h = look_menu_bar_height(), fs = igGetFontSize();
    ImVec2 p = igGetCursorScreenPos();
    p.y = igGetWindowPos().y;
    float ls = h * 0.86f;
    void* mark = brand_mark();
    float x = p.x + 2.0f;
    if (mark) { // the logo: its white mark in the accent, a lighter copy over its top half (the mockup's gradient)
        ImVec2 a = { x, p.y + (h - ls) * 0.5f }, b = { x + ls, p.y + (h + ls) * 0.5f };
        ImTextureRef_c tex = { NULL, (ImTextureID)(uintptr_t)mark };
        ImDrawList_AddImage(dl, tex, a, b, (ImVec2){ 0, 0 }, (ImVec2){ 1, 1 }, TH(TK_ACCENT));
        ImDrawList_AddImage(dl, tex, a, (ImVec2){ b.x, (a.y + b.y) * 0.5f }, (ImVec2){ 0, 0 }, (ImVec2){ 1, 0.5f },
                            THA(TK_TEXT_STRONG, 70));
    }
    x += ls + fs * 0.4f;
    const char* name = "Real Engine";
    float tfs = fs * 1.18f;
    ImFont* tf = g_font_title ? g_font_title : igGetFont();
    ImVec2 ns = ImFont_CalcTextSizeA(tf, tfs, 1e9f, -1.0f, name, NULL, NULL);
    ImDrawList_AddText_FontPtr(dl, tf, tfs, (ImVec2){ x, p.y + (h - ns.y) * 0.5f }, TH(TK_TEXT_STRONG), name, NULL, 0.0f, NULL);
    x += ns.x + fs * 0.35f;
    char ver[32];
    snprintf(ver, sizeof ver, "%s", RE_VERSION_STRING);
    ImVec2 vs = ImFont_CalcTextSizeA(tf, tfs, 1e9f, -1.0f, ver, NULL, NULL);
    ImDrawList_AddText_FontPtr(dl, tf, tfs, (ImVec2){ x, p.y + (h - vs.y) * 0.5f }, TH(TK_ACCENT), ver, NULL, 0.0f, NULL);
    x += vs.x + fs * 1.4f;
    igSetCursorScreenPos((ImVec2){ p.x, igGetCursorScreenPos().y });
    igDummy((ImVec2){ x - p.x, 1.0f }); // tips: none (the brand: a picture and the version, no control)
    igSameLine(0, 0);
}

static void fps_color(double fps, uint32_t* col) {
    *col = fps >= 120.0 ? TH(TK_OK) : fps >= 55.0 ? TH(TK_WARN) : TH(TK_ERROR);
}

void look_menu_readouts(Editor* ed) {
    double now = re_time_seconds();
    if (now - K.at > 5.0) refresh_checks(ed);
    const FrameStats* s = &ed->stats;
    char buf[96];
    snprintf(buf, sizeof buf, "FPS: %.1f  |  %s: %.1f ms", s->fps, TR("Frame"), s->frame_ms);
    float fs = igGetFontSize(), isz = fs * 0.95f;
    // the targets' width: each its icon and name
    float tw = 0.0f;
    for (int t = 0; t < LOOK_TARGET_COUNT; t++) tw += isz + fs * 0.35f + igCalcTextSize(look_target_name(t), NULL, false, -1).x + fs * 1.2f;
    float rw = isz + fs * 0.4f + igCalcTextSize(buf, NULL, false, -1).x + fs * 1.4f + tw;
    float rx = igGetWindowPos().x + igGetWindowWidth() - rw - fs * 0.6f;
    if (rx < igGetCursorScreenPos().x + fs) return; // (a narrow window: the menus first)
    ImDrawList* dl = igGetWindowDrawList();
    float h = look_menu_bar_height(), y0 = igGetWindowPos().y;
    // the frame: the signal bars in the fps' color, the numbers; hovered: the CPU and the GPU
    igSetCursorScreenPos((ImVec2){ rx, igGetCursorScreenPos().y });
    ImVec2 a = { rx, y0 };
    float w0 = isz + fs * 0.4f + igCalcTextSize(buf, NULL, false, -1).x;
    igInvisibleButton("##look_fps", (ImVec2){ w0, h - 2.0f }, 0); // tips: none (a readout; its tooltip says what it measures)
    if (igIsItemHovered(0)) {
        char tip[320];
        const char* pm = ed->vk->present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "Mailbox" : ed->vk->present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "Immediate" : "VSync";
        snprintf(tip, sizeof tip, TR("The editor's frame, averaged over half a second: %.0f fps, %.3f ms a frame (CPU %.3f ms, GPU %.3f ms, %s). Click: the Debug Viewer (F10)."),
                 s->fps, s->frame_ms, s->cpu_ms, s->gpu_ms, pm);
        ui_tip_item(ICON_SIGNAL, TR("Frame Rate"), tip, "F10");
    }
    if (igIsItemClicked(ImGuiMouseButton_Left)) dv_set_open(!dv_open());
    uint32_t fc;
    fps_color(s->fps, &fc);
    ui_icon(dl, ICON_SIGNAL, a.x, y0 + (h - isz) * 0.5f, isz, fc);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ a.x + isz + fs * 0.4f, y0 + (h - fs) * 0.5f }, TH(TK_TEXT_DIM), buf, NULL);
    float x = a.x + w0 + fs * 0.7f;
    ImDrawList_AddLine(dl, (ImVec2){ x, y0 + h * 0.28f }, (ImVec2){ x, y0 + h * 0.72f }, TH(TK_BORDER_STRONG), 1.0f);
    x += fs * 0.7f;
    // the targets: the one chosen in the accent, each with its readiness dot; a click chooses it
    static const UiIcon ic[LOOK_TARGET_COUNT] = { ICON_MONITOR, ICON_ANDROID, ICON_VR };
    for (int t = 0; t < LOOK_TARGET_COUNT; t++) {
        const char* nm = look_target_name(t);
        float w = isz + fs * 0.35f + igCalcTextSize(nm, NULL, false, -1).x;
        igSetCursorScreenPos((ImVec2){ x - fs * 0.3f, y0 + 1.0f });
        char id[24];
        snprintf(id, sizeof id, "##look_target%d", t);
        bool hit = igInvisibleButton(id, (ImVec2){ w + fs * 0.6f, h - 2.0f }, 0); // tips: none (its tooltip: the target's readiness)
        bool hov = igIsItemHovered(0), sel = L.target == t;
        if (hov) {
            char tip[600];
            int n = snprintf(tip, sizeof tip, "%s ", TR("The target platform: Play and the Platforms menu act for it. Click to choose it."));
            for (int i = 0; i < K.n[t] && n > 0 && n < (int)sizeof tip; i++)
                n += snprintf(tip + n, sizeof tip - (size_t)n, "\n%s %s", K.c[t][i].state == CK_OK ? "+" : K.c[t][i].state == CK_WARN ? "!" : "x", K.c[t][i].text);
            ui_tip_item(ic[t], nm, tip, NULL);
            ImDrawList_AddRectFilled(dl, igGetItemRectMin(), igGetItemRectMax(), TH(TK_BG_HOVER), TMV(TM_RADIUS_FRAME), 0);
        }
        if (hit) L.target = t, L.platforms_open = true;
        uint32_t col = sel ? TH(TK_TEXT_STRONG) : TH(TK_TEXT_DIM);
        ui_icon(dl, ic[t], x, y0 + (h - isz) * 0.5f, isz, sel ? TH(TK_ACCENT) : col);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ x + isz + fs * 0.35f, y0 + (h - fs) * 0.5f }, col, nm, NULL);
        ImDrawList_AddCircleFilled(dl, (ImVec2){ x + isz - 1.0f, y0 + (h - isz) * 0.5f + 1.5f }, fs * 0.17f, TH((int)ready_tok(t)), 10);
        if (sel) ImDrawList_AddRectFilled(dl, (ImVec2){ x - fs * 0.1f, y0 + h - 3.0f }, (ImVec2){ x + w + fs * 0.1f, y0 + h - 1.0f }, TH(TK_ACCENT), 1.0f, 0);
        x += w + fs * 1.2f;
    }
}

// --- The status bar ---------------------------------------------------------------------------------------------------
void look_status_left(Editor* ed) {
    igSameLine(0, 4);
    if (ui_icon_button("sb_stats", ICON_STATS, TR("Statistics"), TH(TK_N14), ed->show_stats, TR("The viewport's statistics: frame, CPU and GPU, the scene's counts")))
        ed->show_stats = !ed->show_stats;
    igSameLine(0, 4);
    if (ui_icon_button("sb_profiler", ICON_TIMER, TR("Profiler"), TH(TK_N14), dv_open(), TR("The Debug Viewer: the cost of each system and object, the Trace (F10)")))
        dv_set_open(!dv_open());
    igSameLine(0, 4);
    if (ui_icon_button("sb_devices", ICON_PHONE, TR("Device Manager"), TH(TK_N14), L.platforms_open, TR("Platform Support: what each target has and lacks, and the devices connected (Android by adb, the VR runtime)"))) {
        L.platforms_open = true;
        igSetWindowFocus_Str(dock_panel_id(DP_PLATFORMS));
        adb_refresh();
    }
}

void look_status_right(Editor* ed, double now) {
    float fs = igGetFontSize(), h = igGetFrameHeight();
    int shaders = render_pipelines_pending_count() + mat_compiling_count();
    char sc[64], dd[64], ver[48];
    snprintf(sc, sizeof sc, "%s: %d", TR("Shader Compile"), shaders);
    if (D.have) {
        double gb = (double)D.result / (1024.0 * 1024.0 * 1024.0);
        if (gb >= 1.0) snprintf(dd, sizeof dd, "%s: %.1f GB", TR("Derived Data"), gb);
        else snprintf(dd, sizeof dd, "%s: %.0f MB", TR("Derived Data"), (double)D.result / (1024.0 * 1024.0));
    } else snprintf(dd, sizeof dd, "%s: ...", TR("Derived Data"));
    snprintf(ver, sizeof ver, "Real Engine %s", RE_VERSION_STRING);
    const char* state = ed->playing ? TR("Playing") : shaders ? TR("Compiling") : TR("Ready");
    float isz = fs * 0.95f, gap = fs * 1.4f;
    float w_state = isz + fs * 0.4f + igCalcTextSize(state, NULL, false, -1).x;
    float w_sc = isz + fs * 0.4f + igCalcTextSize(sc, NULL, false, -1).x;
    float w_dd = isz + fs * 0.4f + igCalcTextSize(dd, NULL, false, -1).x;
    float w_ver = igCalcTextSize(ver, NULL, false, -1).x + h * 0.9f;
    float total = w_state + w_sc + w_dd + w_ver + gap * 3.0f;
    igSameLine(0, 0);
    float x = igGetCursorPosX() + igGetContentRegionAvail().x - total;
    if (x > igGetCursorPosX()) igSetCursorPosX(x);
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 p = igGetCursorScreenPos();
    float cy = p.y + (h - isz) * 0.5f, ty = p.y + (h - fs) * 0.5f;
    // Ready (or what runs)
    if (shaders && !ed->playing) ui_spinner(dl, p.x, cy, isz, TH(TK_ACCENT), now);
    else ui_icon(dl, ed->playing ? ICON_PLAY : ICON_CHECK, p.x, cy, isz, ed->playing ? TH(TK_ACCENT2) : TH(TK_OK));
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + isz + fs * 0.4f, ty }, TH(TK_TEXT), state, NULL);
    igInvisibleButton("##look_state", (ImVec2){ w_state, h }, 0); // tips: none (a readout with its own tooltip)
    if (igIsItemHovered(0)) ui_tip_item(ICON_CHECK, state, TR("The editor's state: ready, playing, or compiling shaders and materials in the background (the editor never waits for them)."), NULL);
    // Shader Compile: the pipelines made on their threads and the materials compiling (each a GPU bake)
    igSameLine(0, gap);
    p = igGetCursorScreenPos();
    ui_icon(dl, ICON_COMPILE, p.x, cy, isz, shaders ? TH(TK_WARN) : TH(TK_TEXT_DIM));
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + isz + fs * 0.4f, ty }, shaders ? TH(TK_TEXT) : TH(TK_TEXT_DIM), sc, NULL);
    igInvisibleButton("##look_shaders", (ImVec2){ w_sc, h }, 0); // tips: none (a readout with its own tooltip)
    if (igIsItemHovered(0)) {
        char tip[300];
        snprintf(tip, sizeof tip, TR("Pipelines still compiling on their threads: %d; materials still compiling: %d. The scene draws meanwhile with what is ready."),
                 render_pipelines_pending_count(), mat_compiling_count());
        ui_tip_item(ICON_COMPILE, TR("Shader Compile"), tip, NULL);
    }
    // Derived Data: the cache's size on disk (the pipeline cache, the processed meshes, the impostors...); a click opens it
    igSameLine(0, gap);
    p = igGetCursorScreenPos();
    ui_icon(dl, ICON_SAVE, p.x, cy, isz, TH(TK_TEXT_DIM));
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + isz + fs * 0.4f, ty }, TH(TK_TEXT_DIM), dd, NULL);
    if (igInvisibleButton("##look_ddc", (ImVec2){ w_dd, h }, 0) && D.dir[0]) re_show_in_explorer(D.dir); // tips: none (its own tooltip)
    if (igIsItemHovered(0)) {
        char tip[900];
        snprintf(tip, sizeof tip, TR("The derived data cache on this PC (%s): the compiled pipelines, the processed meshes and textures, measured every minute. Click: open the folder."), D.dir);
        ui_tip_item(ICON_SAVE, TR("Derived Data"), tip, NULL);
    }
    // the version: a pill in the accent; About
    igSameLine(0, gap);
    if (look_pill("##look_ver", ver, TK_ACCENT, TK_TEXT_ON_ACCENT, TR("The engine's version. Click: About Real Engine (and Help > Check for Updates)."))) ed->show_about = true;
}

// --- The Platform Support panel ---------------------------------------------------------------------------------------
static bool target_tile(int t, float w, float hgt) {
    static const UiIcon ic[LOOK_TARGET_COUNT] = { ICON_MONITOR, ICON_ANDROID, ICON_VR };
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 p = igGetCursorScreenPos();
    igPushID_Int(t);
    bool hit = igInvisibleButton("##tile", (ImVec2){ w, hgt }, 0); // tips: none (a tile with its own name and tooltip)
    bool hov = igIsItemHovered(0), sel = L.target == t;
    if (hov) ui_tip_item(ic[t], look_target_name(t), TR("Choose this target: its checks below; Play and the Platforms menu act for it."), NULL);
    igPopID();
    float r = TMV(TM_RADIUS_PANEL), fs = igGetFontSize();
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + hgt }, sel ? TH(TK_ACCENT_DIM) : hov ? TH(TK_BG_HOVER) : TH(TK_BG_RAISED), r, 0);
    if (sel) ImDrawList_AddRect(dl, p, (ImVec2){ p.x + w, p.y + hgt }, TH(TK_ACCENT), r, 1.5f, 0);
    float is = fminf(w * 0.34f, hgt * 0.4f);
    ui_icon(dl, ic[t], p.x + (w - is) * 0.5f, p.y + hgt * 0.1f, is, sel ? TH(TK_TEXT_STRONG) : TH(TK_TEXT));
    const char* nm = look_target_name(t);
    ImVec2 ns = igCalcTextSize(nm, NULL, false, -1);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + (w - ns.x) * 0.5f, p.y + hgt * 0.1f + is + fs * 0.25f }, TH(TK_TEXT_STRONG), nm, NULL);
    const char* st = K.missing[t] ? TR("Something missing") : K.warn[t] ? TR("Ready, with notes") : TR("Ready");
    ImVec2 ss = igCalcTextSize(st, NULL, false, -1);
    float sy = p.y + hgt * 0.1f + is + fs * 1.4f, dot = fs * 0.22f;
    ImDrawList_AddCircleFilled(dl, (ImVec2){ p.x + (w - ss.x) * 0.5f - dot * 2.2f, sy + fs * 0.5f }, dot, TH((int)ready_tok(t)), 10);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + (w - ss.x) * 0.5f, sy }, TH(TK_TEXT_DIM), st, NULL);
    return hit;
}

void look_platforms_panel(Editor* ed, double now) {
    (void)now;
    if (igIsWindowAppearing() || re_time_seconds() - K.at > 5.0) refresh_checks(ed);
    adb_poll();
    float fs = igGetFontSize(), avail = igGetContentRegionAvail().x, gap = fs * 0.4f;
    float tw = (avail - gap * 2.0f) / 3.0f, th = fs * 5.6f;
    for (int t = 0; t < LOOK_TARGET_COUNT; t++) {
        if (t) igSameLine(0, gap);
        if (target_tile(t, tw, th)) L.target = t;
    }
    igDummy((ImVec2){ 0, fs * 0.2f });
    int t = L.target;
    ImDrawList* dl = igGetWindowDrawList();
    for (int i = 0; i < K.n[t]; i++) { // the checks: a mark, the text (wrapped), a button when something can be done here
        LookCheck* c = &K.c[t][i];
        igPushID_Int(i);
        ImVec2 p = igGetCursorScreenPos();
        float is = fs * 0.9f;
        ui_icon(dl, c->state == CK_OK ? ICON_CHECK : c->state == CK_WARN ? ICON_WARNING : ICON_CROSS, p.x, p.y + 1.0f, is,
                c->state == CK_OK ? TH(TK_OK) : c->state == CK_WARN ? TH(TK_WARN) : TH(TK_ERROR));
        igSetCursorScreenPos((ImVec2){ p.x + is + fs * 0.4f, p.y });
        float bw = c->action == ACT_NONE ? 0.0f : igCalcTextSize(TR(c->action == ACT_FOLDER ? "Open" : "Settings"), NULL, false, -1).x + fs * 1.2f;
        igPushTextWrapPos(igGetCursorPosX() + fmaxf(avail - is - fs * 0.6f - bw, fs * 6.0f));
        igPushStyleColor_U32(ImGuiCol_Text, c->state == CK_OK ? TH(TK_TEXT_DIM) : TH(TK_TEXT));
        igTextWrapped("%s", c->text);
        igPopStyleColor(1);
        igPopTextWrapPos();
        if (c->action != ACT_NONE) {
            igSameLine(0, fs * 0.4f);
            if (c->action == ACT_FOLDER) {
                if (igSmallButton(TR("Open"))) re_show_in_explorer(c->path);
                ui_item_tip(TR("Opens the folder in Explorer."));
            } else if (igSmallButton(TR("Settings"))) {
                if (c->action == ACT_PACKAGING) ed->show_package = true;
                else ed->show_project_settings = true;
            }
        }
        igPopID();
    }
    igDummy((ImVec2){ 0, fs * 0.2f });
    // the main action: package for the target (each one's existing packager, in the background)
    const ProjectSettings* ps = project();
    char lab[96];
    snprintf(lab, sizeof lab, TR("Package for %s"), t == LOOK_VR ? "Meta Quest" : look_target_name(t));
    bool can = !ed->playing && (t == LOOK_PC || (ps->package_dir[0] && !K.missing[LOOK_ANDROID]));
    igBeginDisabled(!can);
    if (look_primary_button("ps_package", ICON_PACKAGE, lab, TR("Packages the project for this target (in the background: the card at the bottom right)."))) {
        if (t == LOOK_PC) ed->show_package = true;
        else if (t == LOOK_ANDROID) editor_package_android(ed, NULL, true, true, re_time_seconds());
        else editor_package_quest(ed, NULL, true, true, re_time_seconds());
    }
    igEndDisabled();
    if (t == LOOK_VR) {
        igSameLine(0, fs * 0.4f);
        if (ui_icon_button("ps_vr_preview", ICON_VR, TR("VR Preview"), TH(TK_N14), false, TR("Plays the level on the headset (OpenXR) in a window of its own.")))
            editor_vr_preview(ed, re_time_seconds());
    }
    // the devices connected
    igSpacing();
    igSeparatorText(TR("Devices"));
    if (t == LOOK_VR) {
        igTextDisabled("%s %s", TR("OpenXR runtime:"), K.runtime[0] ? K.runtime : TR("none"));
    } else if (t == LOOK_ANDROID) {
        if (A.th) igTextDisabled("%s", TR("Looking for devices (adb)..."));
        else if (A.adb_missing && A.ran == false && !K.have_sdk) igTextDisabled("%s", TR("adb needs the Android SDK (platform-tools)."));
        else if (!A.ran) igTextDisabled("%s", TR("Refresh lists the phones and headsets connected by USB (adb)."));
        else if (!A.n) igTextDisabled("%s", TR("No Android device connected (USB debugging on?)."));
        for (int i = 0; i < A.n; i++) igBulletText("%s", A.dev[i]);
        if (ui_icon_button("ps_adb", ICON_REFRESH, TR("Refresh"), TH(TK_N14), false, TR("Asks adb for the devices connected (USB debugging on).")))
            adb_refresh(), A.ran = A.adb_missing ? true : A.ran;
        if (A.adb_missing && A.ran) igTextDisabled("%s", TR("adb not found in the Android SDK (platform-tools)."));
    } else {
        igTextDisabled("%s %s", TR("This PC:"), ed->vk ? ed->vk->gpu_props.deviceName : "?");
    }
}

// --- The window's title bar in the theme's color (Windows 11: DWM's caption, text and border colors) ---------------
static void title_bar_colors(Editor* ed) {
#ifdef _WIN32
    if (L.theme_gen == theme_generation() || !ed->window) return;
    L.theme_gen = theme_generation();
    HWND hw = glfwGetWin32Window(ed->window);
    static HRESULT(WINAPI * set_attr)(HWND, DWORD, LPCVOID, DWORD);
    static bool tried;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryA("dwmapi.dll");
        if (m) set_attr = (HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD))(void*)GetProcAddress(m, "DwmSetWindowAttribute");
    }
    if (!hw || !set_attr) return;
    const Theme* t = theme_at(theme_current());
    BOOL dark = !(t && t->light);
    set_attr(hw, 20, &dark, sizeof dark); // DWMWA_USE_IMMERSIVE_DARK_MODE
    DWORD cap = TH(TK_BG_APP) & 0x00FFFFFFu, txt = TH(TK_TEXT) & 0x00FFFFFFu, brd = TH(TK_BORDER) & 0x00FFFFFFu; // (ImU32 RGB = COLORREF)
    set_attr(hw, 35, &cap, sizeof cap); // DWMWA_CAPTION_COLOR (Windows 11; older: ignored)
    set_attr(hw, 36, &txt, sizeof txt); // DWMWA_TEXT_COLOR
    set_attr(hw, 34, &brd, sizeof brd); // DWMWA_BORDER_COLOR
#else
    (void)ed;
#endif
}

// --- The new look offered once -------------------------------------------------------------------------------------
bool look_read_pref(const char* key, const char* v) {
    if (!strcmp(key, "look")) L.seen_version = atoi(v), L.have_pref = true;
    else if (!strcmp(key, "target")) look_set_target(atoi(v));
    else return false;
    return true;
}
void look_write_prefs(FILE* f) { fprintf(f, "look=%d\ntarget=%d\n", LOOK_VERSION, L.target); }

bool g_look_test; // (--look-test: the migration done by an automated run, in a folder of its own: main.c)
void look_after_prefs(Editor* ed, bool had_file, bool readonly) {
    (void)ed;
    if ((readonly && !g_look_test) || !had_file || L.seen_version >= LOOK_VERSION) return; // (a new user starts with it; a test changes nothing)
    snprintf(L.prev_theme, sizeof L.prev_theme, "%s", theme_current() == THEME_DEFAULT ? THEME_PREVIOUS_DEFAULT_NAME : theme_current_name());
    if (!strcmp(L.prev_theme, THEME_PREVIOUS_DEFAULT_NAME)) { // on the old default (or an editor.ini from before the themes,
        // with no theme= line): the new one, with its layout (going back offered)
        theme_select(THEME_DEFAULT);
        L.switched = true, L.keep_layout = false;
    } else {
        L.offer = true; // (a theme of their own: offered, not imposed)
    }
}
bool look_keep_saved_layout(void) { return L.keep_layout; }
void look_previous_layout(const char* text) {
    char dir[600];
    if (!text || !re_config_dir(dir, sizeof dir)) return;
    char ldir[700];
    snprintf(ldir, sizeof ldir, "%s\\Layouts", dir);
    re_make_dirs(ldir);
    snprintf(L.prev_layout, sizeof L.prev_layout, "%s\\%s.rlayout", ldir, TR("Before Azul Real"));
    FILE* f = re_fopen(L.prev_layout, "wb");
    if (!f) {
        L.prev_layout[0] = 0;
        return;
    }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

// Back to the look before (the card's button): the theme and the layout as they were, kept in the preferences
static void look_go_back(Editor* ed) {
    theme_select_name(L.prev_theme);
    if (L.prev_layout[0]) dock_layout_load_file(ed, L.prev_layout);
    L.switched = false;
    editor_save_prefs(ed);
}

static void offer_card(Editor* ed) {
    if (!L.offer && !L.switched) return;
    ImGuiViewport* vp = igGetMainViewport();
    float fs = igGetFontSize();
    igSetNextWindowPos((ImVec2){ vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + look_menu_bar_height() + fs * 4.0f }, ImGuiCond_Always, (ImVec2){ 0.5f, 0.0f });
    igSetNextWindowViewport(vp->ID);
    igPushStyleColor_U32(ImGuiCol_WindowBg, TH(TK_BG_RAISED));
    igPushStyleColor_U32(ImGuiCol_Border, TH(TK_ACCENT));
    igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 1.0f);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){ fs, fs * 0.8f });
    bool shown = igBegin("##look_offer", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking |
                                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    igPopStyleVar(2);
    igPopStyleColor(2);
    if (shown) {
        if (g_font_bold) igPushFont(g_font_bold, 0.0f);
        igTextUnformatted(L.switched ? TR("Real Engine has a new look: Azul Real") : TR("There is a new default look: Azul Real"), NULL);
        if (g_font_bold) igPopFont();
        igPushTextWrapPos(fs * 30.0f);
        igTextDisabled("%s", L.switched ? TR("The theme and the panels' layout of the new default (Platform Support, the Content Browser and the Output Log docked). Your previous layout is kept in Window > Layout.")
                                        : TR("A navy theme with one blue accent and the panels laid out like the new default. Your theme and layout stay unless you try it; Edit > Editor Preferences > Appearance changes it any time."));
        igPopTextWrapPos();
        igSpacing();
        if (L.switched) {
            if (look_primary_button("look_keep", ICON_CHECK, TR("Keep It"), TR("Keeps the new look (Editor Preferences > Appearance changes it any time)."))) L.switched = false;
            igSameLine(0, fs * 0.5f);
            if (ui_icon_button("look_back", ICON_UNDO, TR("Go Back to My Previous Look"), TH(TK_N14), false, TR("Your previous theme and panel layout, as they were.")))
                look_go_back(ed);
        } else {
            if (look_primary_button("look_try", ICON_THEME, TR("Try It"), TR("Switches to Azul Real and the new default layout; you can go back right after."))) {
                snprintf(L.prev_theme, sizeof L.prev_theme, "%s", theme_current_name());
                theme_select(THEME_DEFAULT);
                dock_layout_save(ed, TR("Before Azul Real"), false);
                char dir[600];
                if (re_config_dir(dir, sizeof dir)) snprintf(L.prev_layout, sizeof L.prev_layout, "%s\\Layouts\\%s.rlayout", dir, TR("Before Azul Real"));
                dock_layout_default(ed);
                L.offer = false, L.switched = true;
                editor_save_prefs(ed);
            }
            igSameLine(0, fs * 0.5f);
            if (ui_icon_button("look_no", ICON_CROSS, TR("No, Thanks"), TH(TK_N14), false, TR("Keeps your theme and layout; the new look stays in Editor Preferences > Appearance.")))
                L.offer = false;
        }
    }
    igEnd();
}

void look_frame(Editor* ed, double now) {
    static bool saved;
    if (!saved && (L.switched || L.offer)) saved = true, editor_save_prefs(ed); // (look= written: offered once, whatever is clicked)
    title_bar_colors(ed);
    dd_poll(now);
    adb_poll();
    offer_card(ed);
}

// (--look-test, the test look_migration) an editor.ini of 0.43 on the old default and its last layout (main.c writes them
// in a folder of its own): the new theme and the whole default layout, the old layout kept as a user layout, the card;
// then its "go back": the old theme again
bool look_test_check(Editor* ed) {
    int bad = 0;
#define LT_CHECK(c, what) do { if (!(c)) printf("[RealEngine] FAIL: look test: %s\n", what), bad++; } while (0)
    LT_CHECK(theme_current() == THEME_DEFAULT, "the new default theme after the old editor.ini");
    LT_CHECK(L.switched && !L.offer, "the card offering to go back");
    LT_CHECK(L.prev_layout[0] && is_file(L.prev_layout), "the previous layout kept as a user layout");
    LT_CHECK(ed->content_docked && ed->show_place && *look_platforms_flag() && *look_modes_flag() && ed->show_log, "the whole default layout");
    LT_CHECK(!strcmp(L.prev_theme, THEME_PREVIOUS_DEFAULT_NAME), "the previous theme remembered");
    look_go_back(ed);
    LT_CHECK(!strcmp(theme_current_name(), THEME_PREVIOUS_DEFAULT_NAME) && !L.switched, "going back: the old theme");
#undef LT_CHECK
    printf("[RealEngine] Look migration test %s (%s; previous layout %s)\n", bad ? "FAILED" : "OK", theme_current_name(), L.prev_layout);
    return bad == 0;
}
