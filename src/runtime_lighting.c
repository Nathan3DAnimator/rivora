// (Q16) Build Lighting in the game: the request a Blueprint makes, started and watched by the host's frame
// (runtime_lighting.h, notas-dev/em-jogo.md). The bake is the editor's (gi_bake.c), in the game's modes.
#include "runtime_lighting.h"

#include <stdio.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "gi.h"
#include "gi_bake.h"
#include "platform.h"
#include "platform_files.h"
#include "save_progress.h"
#include "vkctx.h"

static struct {
    VkCtx* vk;
    bool asked, running, owns, built, changed; // changed: the lightmaps shown are not the level's own any more
    int quality;
    RtlMode mode;
    float budget_ms;
    double seconds;
    char status[160];
} L;

bool rtl_request(int quality, RtlMode mode, float budget_ms) {
    if (L.asked || L.running || gi_baking()) {
        snprintf(L.status, sizeof L.status, "a bake is already under way");
        return false;
    }
    L.asked = true, L.built = false;
    L.quality = quality, L.mode = mode, L.budget_ms = budget_ms;
    L.status[0] = 0;
    return true;
}

void rtl_cancel(void) {
    if (L.running && L.owns && gi_baking()) gi_bake_cancel();
    L.asked = false; // (a running one ends in the next rtl_frame: its event with Success false)
}

bool rtl_busy(void) { return L.asked || L.running; }
bool rtl_blocking(void) { return (L.asked || L.running) && L.mode == RTL_LOADING_SCREEN; }
float rtl_progress(void) { return L.built ? 1.0f : L.running ? gi_bake_progress() : 0.0f; }
bool rtl_built(void) { return L.built; }
double rtl_last_seconds(void) { return L.seconds; }
const char* rtl_status(void) { return L.status; }
bool rtl_owns_bake(void) { return L.owns; }

static void fire(World* w, bool ok) {
    BpValue a[2] = { { { ok ? 1.0f : 0.0f } }, { { (float)L.seconds } } };
    bp_fire_event(w, BPK_EVENT_LIGHTING_BUILT, a, 2); // (Event On Lighting Built: Success, Seconds)
}

void rtl_frame(VkCtx* vk, World* w) {
    L.vk = vk;
    if (L.asked) {
        L.asked = false;
        char why[128] = "";
        GiBakeMode m = L.mode == RTL_LOADING_SCREEN ? GI_BAKE_BLOCKING : GI_BAKE_PROGRESSIVE;
        if (gi_bake_start_ex(vk, w, L.quality ? BAKE_PRODUCTION : BAKE_PREVIEW, m, L.budget_ms, why, sizeof why)) {
            L.running = L.owns = L.changed = true;
        } else {
            snprintf(L.status, sizeof L.status, "%s", why);
            printf("[RealEngine] Build Lighting in the game: not started (%s)\n", why);
            L.seconds = 0.0;
            fire(w, false);
            return;
        }
    }
    if (!L.running) return;
    gi_bake_frame(vk); // (a bake while playing: its thread's work taken over before the render)
    if (gi_baking()) return;
    GiBakeStats st;
    gi_bake_stats(&st);
    L.seconds = st.seconds;
    L.running = false;
    L.built = gi_bake_take_finished(); // (false: cancelled)
    printf("[RealEngine] Build Lighting in the game (%s): %s in %.2f s, %u frames; GPU %.2f ms a frame (%.2f at most, budget %.1f); "
           "start %.1f ms\n",
           L.mode == RTL_LOADING_SCREEN ? "loading screen" : "progressive", L.built ? "built" : "cancelled", st.seconds, st.frames,
           st.gpu_avg_ms, st.gpu_max_ms, st.budget_ms, st.start_ms);
    fire(w, L.built);
}

static void slot_path(const char* name, char* out, size_t cap) {
    char s[64];
    size_t k = 0;
    for (const char* c = name && name[0] ? name : "Lighting"; *c && k + 1 < sizeof s; c++) // (a file's name: no folders)
        s[k++] = (*c == '/' || *c == '\\' || *c == ':' || *c == '.') ? '_' : *c;
    s[k] = 0;
    snprintf(out, cap, "%s/%s.rlight", progress_dir(), s);
}

bool rtl_save(const char* name) {
    VkCtx* vk = L.vk;
    if (!vk || gi_baking() || !gi_lightmaps_present()) return false;
    char p[1100];
    slot_path(name, p, sizeof p);
    re_make_dirs(progress_dir());
    return gi_lightmaps_save(vk, p);
}

bool rtl_load(const char* name) {
    VkCtx* vk = L.vk;
    if (!vk || gi_baking()) return false;
    char p[1100];
    slot_path(name, p, sizeof p);
    if (!gi_lightmaps_load(vk, p)) return false;
    gi_reset_probes();
    L.changed = L.built = true;
    return true;
}

void rtl_end_play(VkCtx* vk, const char* rlight) {
    if (L.owns) { // (its end never read as the level's: bake_tick)
        if (gi_baking()) gi_bake_cancel();
        gi_bake_take_finished();
    }
    bool changed = L.changed;
    memset(&L, 0, sizeof L);
    L.vk = vk;
    if (!changed) return;
    if (!rlight || !rlight[0] || !gi_lightmaps_load(vk, rlight)) gi_lightmaps_clear(vk);
    gi_reset_probes();
}
