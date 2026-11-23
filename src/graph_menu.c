// The graph menus' recents and recommendations (graph_menu.h)
#include "graph_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "platform.h"
#include "platform_files.h"

#define GM_RECENTS 8
#define GM_PAIRS 4096
#define GM_USES 1024
#define GM_KEY 96

typedef struct GmPair { uint8_t g; uint32_t count; char ctx[GM_KEY], item[GM_KEY]; } GmPair;
typedef struct GmUse { uint8_t g; uint32_t count; char item[GM_KEY]; } GmUse;

static struct {
    bool loaded;
    char path[700];
    char recent[GM_COUNT][GM_RECENTS][GM_KEY];
    GmPair* pairs; // (the table: allocated once)
    uint32_t npairs;
    GmUse* uses;
    uint32_t nuses;
} M;

// The usual pairs (Unreal's habits), per graph: after ctx ("" with nothing around), item, its weight (1-5). An item
// or a context ending in '*' matches by its start (a class's "Cast To ...")
typedef struct GmDefault { uint8_t g; const char* ctx; const char* item; uint8_t w; } GmDefault;
static const GmDefault g_defaults[] = {
    // Blueprints: the Event Graph
    { GM_BP_EVENT, "", "Event BeginPlay", 5 }, { GM_BP_EVENT, "", "Print String", 4 }, { GM_BP_EVENT, "", "Event Tick", 3 },
    { GM_BP_EVENT, "", "Branch", 3 }, { GM_BP_EVENT, "", "Get Player Character", 2 },
    { GM_BP_EVENT, "Event BeginPlay", "Print String", 5 }, { GM_BP_EVENT, "Event BeginPlay", "Set Timer by Event*", 4 },
    { GM_BP_EVENT, "Event BeginPlay", "Delay", 4 }, { GM_BP_EVENT, "Event BeginPlay", "Create Widget", 3 },
    { GM_BP_EVENT, "Event BeginPlay", "Spawn Actor from Class", 3 }, { GM_BP_EVENT, "Event BeginPlay", "Get Player Character", 3 },
    { GM_BP_EVENT, "Event Tick", "Add Actor World Offset", 5 }, { GM_BP_EVENT, "Event Tick", "Add Actor Local Rotation", 4 },
    { GM_BP_EVENT, "Event Tick", "Get World Delta Seconds", 4 }, { GM_BP_EVENT, "Event Tick", "Branch", 3 },
    { GM_BP_EVENT, "Event Tick", "Get Actor Location", 3 },
    { GM_BP_EVENT, "Get Actor Location", "Set Actor Location", 5 }, { GM_BP_EVENT, "Get Actor Location", "Add (Vector)", 4 },
    { GM_BP_EVENT, "Get Actor Location", "Get Distance To", 3 }, { GM_BP_EVENT, "Get Actor Location", "Break Vector", 3 },
    { GM_BP_EVENT, "Get Actor Location", "Spawn Actor from Class", 2 },
    { GM_BP_EVENT, "Get Actor Forward Vector", "Multiply (Vector * Float)", 5 }, { GM_BP_EVENT, "Get Actor Forward Vector", "Add Movement Input", 4 },
    { GM_BP_EVENT, "Get Player Character", "Cast To*", 5 }, { GM_BP_EVENT, "Get Player Character", "Get Actor Location", 4 },
    { GM_BP_EVENT, "Get Player Character", "Get Distance To", 3 }, { GM_BP_EVENT, "Get Player Pawn", "Get Actor Location", 4 },
    { GM_BP_EVENT, "Cast To*", "Destroy Actor", 3 }, { GM_BP_EVENT, "Cast To*", "Print String", 3 },
    { GM_BP_EVENT, "Branch", "Print String", 4 }, { GM_BP_EVENT, "Branch", "Destroy Actor", 3 }, { GM_BP_EVENT, "Branch", "Delay", 3 },
    { GM_BP_EVENT, "Delay", "Print String", 4 }, { GM_BP_EVENT, "Delay", "Destroy Actor", 4 }, { GM_BP_EVENT, "Delay", "Spawn Actor from Class", 3 },
    { GM_BP_EVENT, "Spawn Actor from Class", "Set Actor Location", 3 }, { GM_BP_EVENT, "Spawn Actor from Class", "Add Impulse", 3 },
    { GM_BP_EVENT, "Create Widget", "Add to Viewport", 5 }, { GM_BP_EVENT, "Add to Viewport", "Set Text (Widget)", 3 },
    { GM_BP_EVENT, "Get World Delta Seconds", "Multiply (Float)", 5 }, { GM_BP_EVENT, "Multiply (Float)", "Make Vector", 3 },
    { GM_BP_EVENT, "Make Vector", "Add Actor World Offset", 4 }, { GM_BP_EVENT, "Make Vector", "Set Actor Location", 4 },
    { GM_BP_EVENT, "Make Rotator", "Add Actor Local Rotation", 5 }, { GM_BP_EVENT, "Make Rotator", "Set Actor Rotation", 4 },
    { GM_BP_EVENT, "Random Float in Range", "Make Vector", 3 }, { GM_BP_EVENT, "Random Float in Range", "Delay", 3 },
    { GM_BP_EVENT, "Get Velocity", "Vector Length", 5 }, { GM_BP_EVENT, "Vector Length", "Greater (Float)", 4 },
    { GM_BP_EVENT, "Greater (Float)", "Branch", 5 }, { GM_BP_EVENT, "Less (Float)", "Branch", 5 }, { GM_BP_EVENT, "Equal (Float)", "Branch", 5 },
    { GM_BP_EVENT, "Is Valid", "Branch", 4 }, { GM_BP_EVENT, "Get Distance To", "Less (Float)", 5 },
    { GM_BP_EVENT, "For Each Loop", "Print String", 3 }, { GM_BP_EVENT, "Get All Actors Of Class", "For Each Loop", 5 },
    { GM_BP_EVENT, "Get Input Axis", "Add Movement Input", 5 }, { GM_BP_EVENT, "Enhanced Input Action*", "Add Movement Input", 4 },
    { GM_BP_EVENT, "Enhanced Input Action*", "Jump", 3 }, { GM_BP_EVENT, "Key Event*", "Jump", 3 },
    { GM_BP_EVENT, "Play Sound at Location", "Destroy Actor", 3 }, { GM_BP_EVENT, "Event End Play", "Clear Timer by Event*", 3 },
    { GM_BP_EVENT, "Event Blueprint Update Animation", "Try Get Pawn Owner", 5 }, { GM_BP_EVENT, "Try Get Pawn Owner", "Get Velocity", 5 },
    { GM_BP_EVENT, "Try Get Pawn Owner", "Is Falling", 4 }, { GM_BP_EVENT, "Create Session", "Open Level (by Name)", 3 },
    { GM_BP_EVENT, "Find Sessions", "Get Session Count", 4 }, { GM_BP_EVENT, "Event On Sessions Found", "Get Session Count", 5 },
    { GM_BP_EVENT, "Get Session Count", "Greater (Integer)", 4 }, { GM_BP_EVENT, "Create Advanced Session", "Set Session Property", 3 },
    { GM_BP_EVENT, "Find Sessions Advanced", "Get Session Details", 4 }, { GM_BP_EVENT, "Line Trace By Channel", "Branch", 4 },
    // functions, macros, the Construction Script
    { GM_BP_FUNCTION, "", "Return Node", 4 }, { GM_BP_FUNCTION, "", "Branch", 3 }, { GM_BP_FUNCTION, "", "Print String", 3 },
    { GM_BP_FUNCTION, "Function Entry", "Branch", 4 }, { GM_BP_FUNCTION, "Function Entry", "Return Node", 3 },
    { GM_BP_FUNCTION, "Branch", "Return Node", 4 }, { GM_BP_FUNCTION, "Add (Float)", "Return Node", 4 },
    { GM_BP_FUNCTION, "Multiply (Float)", "Return Node", 4 }, { GM_BP_FUNCTION, "Get Actor Location", "Return Node", 3 },
    { GM_BP_FUNCTION, "Get Actor Location", "Get Distance To", 3 }, { GM_BP_FUNCTION, "Greater (Float)", "Branch", 5 },
    { GM_BP_MACRO, "", "Inputs", 4 }, { GM_BP_MACRO, "", "Outputs", 4 }, { GM_BP_MACRO, "Inputs", "Branch", 4 },
    { GM_BP_MACRO, "Branch", "Outputs", 4 },
    { GM_BP_CONSTRUCTION, "", "Set Actor Scale 3D", 4 }, { GM_BP_CONSTRUCTION, "", "Set Base Color", 4 },
    { GM_BP_CONSTRUCTION, "", "Random Float in Range", 3 }, { GM_BP_CONSTRUCTION, "Function Entry", "Set Base Color", 4 },
    { GM_BP_CONSTRUCTION, "Function Entry", "Set Actor Scale 3D", 4 }, { GM_BP_CONSTRUCTION, "Random Float in Range", "Make Vector", 4 },
    { GM_BP_CONSTRUCTION, "Make Vector", "Set Actor Scale 3D", 4 }, { GM_BP_CONSTRUCTION, "Make Linear Color", "Set Base Color", 5 },
    // the AnimGraph
    { GM_ANIM_GRAPH, "", "State Machine", 5 }, { GM_ANIM_GRAPH, "", "Sequence Player", 4 }, { GM_ANIM_GRAPH, "", "Slot", 3 },
    { GM_ANIM_GRAPH, "State Machine", "Slot", 5 }, { GM_ANIM_GRAPH, "State Machine", "Layered blend per bone", 3 },
    { GM_ANIM_GRAPH, "State Machine", "Inertialization", 3 }, { GM_ANIM_GRAPH, "Sequence Player", "Blend Poses by bool", 3 },
    { GM_ANIM_GRAPH, "Sequence Player", "Slot", 3 }, { GM_ANIM_GRAPH, "Blend Space 1D Player", "Slot", 3 },
    { GM_ANIM_GRAPH, "Layered blend per bone", "Slot", 4 }, { GM_ANIM_GRAPH, "Slot", "Leg IK", 3 }, { GM_ANIM_GRAPH, "Slot", "Aim Offset", 3 },
    { GM_ANIM_GRAPH, "Motion Matching", "Inertialization", 4 }, { GM_ANIM_GRAPH, "Aim Offset", "Leg IK", 3 }, { GM_ANIM_GRAPH, "Save Cached Pose", "Use Cached Pose", 4 },
    // materials
    { GM_MATERIAL, "", "Image Texture", 5 }, { GM_MATERIAL, "", "Principled BSDF", 4 }, { GM_MATERIAL, "", "Mix", 3 },
    { GM_MATERIAL, "Image Texture", "Principled BSDF", 5 }, { GM_MATERIAL, "Image Texture", "Normal Map", 4 }, { GM_MATERIAL, "Image Texture", "Mix", 3 },
    { GM_MATERIAL, "Texture Coordinate", "Mapping", 5 }, { GM_MATERIAL, "Mapping", "Image Texture", 5 }, { GM_MATERIAL, "Noise Texture", "Color Ramp", 5 },
    { GM_MATERIAL, "Color Ramp", "Principled BSDF", 4 }, { GM_MATERIAL, "Normal Map", "Principled BSDF", 5 }, { GM_MATERIAL, "Mix", "Principled BSDF", 4 },
    { GM_MATERIAL, "Principled BSDF", "Material Output", 5 }, { GM_MATERIAL, "Math", "Mix", 3 }, { GM_MATERIAL, "Fresnel", "Mix", 3 },
    // Behavior Trees
    { GM_BEHAVIOR_TREE, "", "Selector", 5 }, { GM_BEHAVIOR_TREE, "", "Sequence", 5 }, { GM_BEHAVIOR_TREE, "", "Move To", 4 },
    { GM_BEHAVIOR_TREE, "Selector", "Sequence", 4 }, { GM_BEHAVIOR_TREE, "Selector", "Move To", 3 }, { GM_BEHAVIOR_TREE, "Sequence", "Move To", 5 },
    { GM_BEHAVIOR_TREE, "Sequence", "Wait", 4 }, { GM_BEHAVIOR_TREE, "Move To", "Wait", 4 }, { GM_BEHAVIOR_TREE, "Sequence", "Rotate to face BB entry", 3 },
    // Sound Cues
    { GM_SOUND_CUE, "", "Wave Player", 5 }, { GM_SOUND_CUE, "", "Random", 4 }, { GM_SOUND_CUE, "Wave Player", "Random", 4 },
    { GM_SOUND_CUE, "Wave Player", "Modulator", 4 }, { GM_SOUND_CUE, "Wave Player", "Mixer", 3 }, { GM_SOUND_CUE, "Random", "Modulator", 4 },
    { GM_SOUND_CUE, "Modulator", "Looping", 3 }, { GM_SOUND_CUE, "Wave Player", "Looping", 3 }, { GM_SOUND_CUE, "Mixer", "Modulator", 3 },
    { GM_SOUND_CUE, "Wave Player", "Concatenator", 2 }, { GM_SOUND_CUE, "Concatenator", "Delay", 2 },
    // particles (particles.h): an emitter's modules in their usual order
    { GM_PARTICLES, "", "Spawn Rate", 5 }, { GM_PARTICLES, "", "Initial Velocity", 4 }, { GM_PARTICLES, "", "Gravity Force", 3 },
    { GM_PARTICLES, "Spawn Rate", "Initial Velocity", 5 }, { GM_PARTICLES, "Spawn Burst", "Initial Velocity", 5 },
    { GM_PARTICLES, "Initial Velocity", "Gravity Force", 5 }, { GM_PARTICLES, "Initial Velocity", "Drag", 4 },
    { GM_PARTICLES, "Gravity Force", "Collision", 4 }, { GM_PARTICLES, "Gravity Force", "Color Over Life", 3 },
    { GM_PARTICLES, "Color Over Life", "Size Over Life", 4 }, { GM_PARTICLES, "Curl Noise", "Color Over Life", 3 },
    { GM_PARTICLES, "Collision", "Size Over Life", 3 }, { GM_PARTICLES, "Size Over Life", "Mesh Renderer", 3 },
    { GM_PARTICLES, "Size Over Life", "Sprite Renderer", 4 }, { GM_PARTICLES, "Vortex Force", "Curl Noise", 3 },
};

static bool test_path_set;

static void path_of(void) {
    if (M.path[0]) return;
    const char* env = getenv("REALENGINE_GRAPH_MENU_FILE"); // (the tests: a file of their own, not the user's)
    if (env && env[0]) {
        snprintf(M.path, sizeof M.path, "%s", env);
        return;
    }
    char dir[600];
    if (re_config_dir(dir, sizeof dir)) snprintf(M.path, sizeof M.path, "%s/graph_menu.txt", dir);
}

static void load(void) {
    if (M.loaded) return;
    M.loaded = true;
    if (!M.pairs) M.pairs = calloc(GM_PAIRS, sizeof(GmPair)); // (the module's table: once)
    if (!M.uses) M.uses = calloc(GM_USES, sizeof(GmUse));
    path_of();
    FILE* f = M.path[0] ? re_fopen(M.path, "rb") : NULL;
    if (!f) return;
    char line[400];
    int nrec[GM_COUNT] = { 0 };
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        int g = 0;
        unsigned c = 0;
        int n = 0;
        if (line[0] == 'R' && sscanf(line, "R %d %n", &g, &n) == 1 && g >= 0 && g < GM_COUNT && nrec[g] < GM_RECENTS)
            snprintf(M.recent[g][nrec[g]++], GM_KEY, "%s", line + n);
        else if (line[0] == 'P' && sscanf(line, "P %d %u %n", &g, &c, &n) == 2 && g >= 0 && g < GM_COUNT && M.pairs && M.npairs < GM_PAIRS) {
            char* tab = strchr(line + n, '\t');
            if (!tab) continue;
            *tab = 0;
            GmPair* p = &M.pairs[M.npairs++];
            p->g = (uint8_t)g, p->count = c;
            snprintf(p->ctx, GM_KEY, "%s", line + n);
            snprintf(p->item, GM_KEY, "%s", tab + 1);
        } else if (line[0] == 'U' && sscanf(line, "U %d %u %n", &g, &c, &n) == 2 && g >= 0 && g < GM_COUNT && M.uses && M.nuses < GM_USES) {
            GmUse* u = &M.uses[M.nuses++];
            u->g = (uint8_t)g, u->count = c;
            snprintf(u->item, GM_KEY, "%s", line + n);
        }
    }
    fclose(f);
}

static void save(void) {
    path_of();
    if (!M.path[0]) return;
    FILE* f = re_fopen(M.path, "wb");
    if (!f) return;
    fprintf(f, "# Real Engine: the graph menus' recents (R), learned pairs (P count context<TAB>item) and uses (U)\n");
    for (int g = 0; g < GM_COUNT; g++)
        for (int i = 0; i < GM_RECENTS; i++)
            if (M.recent[g][i][0]) fprintf(f, "R %d %s\n", g, M.recent[g][i]);
    for (uint32_t i = 0; i < M.npairs; i++) fprintf(f, "P %d %u %s\t%s\n", M.pairs[i].g, M.pairs[i].count, M.pairs[i].ctx, M.pairs[i].item);
    for (uint32_t i = 0; i < M.nuses; i++) fprintf(f, "U %d %u %s\n", M.uses[i].g, M.uses[i].count, M.uses[i].item);
    fclose(f);
}

void gm_note(GraphMenuKind g, const char* context, const char* item) {
    if ((int)g < 0 || g >= GM_COUNT || !item || !item[0]) return;
    load();
    if (!context) context = "";
    // the recents: to the front (once)
    char (*r)[GM_KEY] = M.recent[g];
    int at = GM_RECENTS - 1;
    for (int i = 0; i < GM_RECENTS; i++)
        if (!strcmp(r[i], item)) at = i;
    memmove(r[1], r[0], (size_t)at * GM_KEY);
    snprintf(r[0], GM_KEY, "%s", item);
    // the pair and the use
    GmPair* p = NULL;
    for (uint32_t i = 0; M.pairs && i < M.npairs && !p; i++)
        if (M.pairs[i].g == g && !strcmp(M.pairs[i].ctx, context) && !strcmp(M.pairs[i].item, item)) p = &M.pairs[i];
    if (!p && M.pairs) {
        if (M.npairs >= GM_PAIRS) { // full: the least counted one goes
            uint32_t lo = 0;
            for (uint32_t i = 1; i < M.npairs; i++)
                if (M.pairs[i].count < M.pairs[lo].count) lo = i;
            p = &M.pairs[lo];
        } else
            p = &M.pairs[M.npairs++];
        p->g = (uint8_t)g, p->count = 0;
        snprintf(p->ctx, GM_KEY, "%s", context);
        snprintf(p->item, GM_KEY, "%s", item);
    }
    if (p) p->count++;
    GmUse* u = NULL;
    for (uint32_t i = 0; M.uses && i < M.nuses && !u; i++)
        if (M.uses[i].g == g && !strcmp(M.uses[i].item, item)) u = &M.uses[i];
    if (!u && M.uses && M.nuses < GM_USES) u = &M.uses[M.nuses++], u->g = (uint8_t)g, u->count = 0, snprintf(u->item, GM_KEY, "%s", item);
    if (u) u->count++;
    save();
}

int gm_recent(GraphMenuKind g, const char** out, int max) {
    if ((int)g < 0 || g >= GM_COUNT) return 0;
    load();
    int n = 0;
    for (int i = 0; i < GM_RECENTS && n < max; i++)
        if (M.recent[g][i][0]) out[n++] = M.recent[g][i];
    return n;
}

static bool key_match(const char* pat, const char* s) {
    size_t l = strlen(pat);
    if (l && pat[l - 1] == '*') return !strncmp(pat, s, l - 1);
    return !strcmp(pat, s);
}

typedef struct Cand { const char* item; float score; } Cand;

static void cand_add(Cand* c, int* n, int cap, const char* item, float score) {
    for (int i = 0; i < *n; i++)
        if (!strcmp(c[i].item, item)) {
            c[i].score += score;
            return;
        }
    if (*n < cap) c[*n].item = item, c[*n].score = score, (*n)++;
}

int gm_recommend(GraphMenuKind g, const char* context, GmAccept accept, void* user, const char** out, int max) {
    if ((int)g < 0 || g >= GM_COUNT || max <= 0) return 0;
    load();
    if (!context) context = "";
    Cand c[256];
    int n = 0;
    // what the user did after this context (learned: they outweigh the table once seen twice), then the table's
    for (uint32_t i = 0; M.pairs && i < M.npairs; i++)
        if (M.pairs[i].g == g && context[0] && !strcmp(M.pairs[i].ctx, context)) cand_add(c, &n, 256, M.pairs[i].item, 8.0f * (float)M.pairs[i].count);
    for (size_t i = 0; i < sizeof g_defaults / sizeof g_defaults[0]; i++)
        if (g_defaults[i].g == g && g_defaults[i].ctx[0] && key_match(g_defaults[i].ctx, context))
            cand_add(c, &n, 256, g_defaults[i].item, 3.0f * g_defaults[i].w);
    // nothing for this context: the most used of this graph, the table's starters
    for (uint32_t i = 0; M.uses && i < M.nuses; i++)
        if (M.uses[i].g == g) cand_add(c, &n, 256, M.uses[i].item, 0.5f * (float)M.uses[i].count);
    for (size_t i = 0; i < sizeof g_defaults / sizeof g_defaults[0]; i++)
        if (g_defaults[i].g == g && !g_defaults[i].ctx[0]) cand_add(c, &n, 256, g_defaults[i].item, 0.25f * g_defaults[i].w);
    // best first (a few dozen: a sort by insertion)
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && c[j].score > c[j - 1].score; j--) {
            Cand t = c[j];
            c[j] = c[j - 1], c[j - 1] = t;
        }
    int k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (!accept || accept(c[i].item, user)) out[k++] = c[i].item;
    return k;
}

const char* gm_title_recent(void) { return TR("Recent"); }
const char* gm_title_recommended(void) { return TR("Recommended"); }

// --- The self-test ---
static bool not_print(const char* item, void* user) {
    (void)user;
    return strcmp(item, "Print String") != 0;
}

int gm_selftest(void) {
    int fails = 0;
    char dir[600];
    if (!re_cache_dir(dir, sizeof dir)) return 1;
    snprintf(M.path, sizeof M.path, "%s/graph_menu_test.txt", dir);
    test_path_set = true;
    remove(M.path);
    M.loaded = false, M.npairs = M.nuses = 0;
    memset(M.recent, 0, sizeof M.recent);
    const char* o[4];
    // the table alone: after BeginPlay, Print String first
    int n = gm_recommend(GM_BP_EVENT, "Event BeginPlay", NULL, NULL, o, 2);
    if (n != 2 || strcmp(o[0], "Print String")) printf("[RealEngine] FAIL: graph menu: BeginPlay's first recommendation %s\n", n ? o[0] : "(none)"), fails++;
    // the filter: what the graph lacks or doesn't connect is skipped
    n = gm_recommend(GM_BP_EVENT, "Event BeginPlay", not_print, NULL, o, 2);
    if (n != 2 || !strcmp(o[0], "Print String")) printf("[RealEngine] FAIL: graph menu: the filter kept Print String\n"), fails++;
    // a "Cast To *" context matches its pattern
    n = gm_recommend(GM_BP_EVENT, "Cast To BP_Enemy", NULL, NULL, o, 1);
    if (n != 1 || (strcmp(o[0], "Destroy Actor") && strcmp(o[0], "Print String"))) printf("[RealEngine] FAIL: graph menu: Cast To's pattern\n"), fails++;
    // learned: placing Jump after BeginPlay three times puts it first
    for (int i = 0; i < 3; i++) gm_note(GM_BP_EVENT, "Event BeginPlay", "Jump");
    n = gm_recommend(GM_BP_EVENT, "Event BeginPlay", NULL, NULL, o, 2);
    if (n < 1 || strcmp(o[0], "Jump")) printf("[RealEngine] FAIL: graph menu: the learned pair isn't first (%s)\n", n ? o[0] : "(none)"), fails++;
    // the recents: newest first, once each, per graph
    gm_note(GM_BP_EVENT, "", "Branch");
    gm_note(GM_MATERIAL, "", "Image Texture");
    n = gm_recent(GM_BP_EVENT, o, 2);
    if (n != 2 || strcmp(o[0], "Branch") || strcmp(o[1], "Jump")) printf("[RealEngine] FAIL: graph menu: the recents %s, %s\n", n > 0 ? o[0] : "-", n > 1 ? o[1] : "-"), fails++;
    // saved and read back
    M.loaded = false, M.npairs = M.nuses = 0;
    memset(M.recent, 0, sizeof M.recent);
    n = gm_recent(GM_BP_EVENT, o, 2);
    int nm = gm_recent(GM_MATERIAL, o + 2, 1);
    if (n != 2 || strcmp(o[0], "Branch") || nm != 1 || strcmp(o[2], "Image Texture")) printf("[RealEngine] FAIL: graph menu: not read back\n"), fails++;
    n = gm_recommend(GM_BP_EVENT, "Event BeginPlay", NULL, NULL, o, 1);
    if (n != 1 || strcmp(o[0], "Jump")) printf("[RealEngine] FAIL: graph menu: the learned pair wasn't kept\n"), fails++;
    // every graph has its starters
    for (int g = 0; g < GM_COUNT; g++)
        if (gm_recommend((GraphMenuKind)g, "", NULL, NULL, o, 2) < 2) printf("[RealEngine] FAIL: graph menu: graph %d has no starters\n", g), fails++;
    remove(M.path);
    M.path[0] = 0, M.loaded = false, M.npairs = M.nuses = 0, test_path_set = false;
    memset(M.recent, 0, sizeof M.recent);
    printf("[RealEngine] Graph menu self-test: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
