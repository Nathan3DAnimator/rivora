// Trace window (B14b, as Unreal Insights' Timing view): record, open a recording (<project>/Saved/Profiling), the frames
// (one bar per frame, its color by the frame rate; click one: the timeline shows it), the timeline (a track per thread
// and the GPU's, the scopes nested; the wheel zooms around the mouse, a drag pans) and the timers (count, total, mean
// and max of every scope in the timeline's range).
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "debug_viewer.h"
#include "theme.h"
#include "i18n.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "trace.h"

uint32_t trace_gpu_passes(const char** names, double* ms, uint32_t cap) {
    const RenderStats* rs = render_stats();
    if (!rs || cap < 8) return 0;
    uint32_t n = 0;
    names[n] = "GPU Scene (cull, skin)", ms[n++] = rs->gpu_scene_ms;
    names[n] = "GPU Shadows", ms[n++] = rs->gpu_shadow_ms;
    names[n] = "GPU Global Illumination", ms[n++] = rs->gpu_gi_ms;
    names[n] = "GPU Main Pass", ms[n++] = rs->gpu_main_ms;
    names[n] = "GPU Selection Mask", ms[n++] = rs->gpu_mask_ms;
    names[n] = "GPU Exposure", ms[n++] = rs->gpu_exposure_ms;
    names[n] = "GPU Post Process", ms[n++] = rs->gpu_post_ms;
    names[n] = "GPU Composite", ms[n++] = rs->gpu_composite_ms;
    return n;
}

static struct {
    TraceFile f;
    bool loaded;
    char path[700];
    double v0, v1;          // the timeline's range (seconds)
    uint32_t first[32], count[32]; // each track's events in f.ev (sorted by track, then start)
    double maxdur[32];
    float p99;              // the frames' 99th percentile (ms): the graph's scale (a start's long frame doesn't flatten it)
    float scroll_y;         // the timeline's tracks scrolled (Shift + wheel, or the wheel over the names)
    // the timers of the range (recomputed when it changes)
    double tv0, tv1;
    uint32_t* tm_n; double* tm_total; double* tm_max; uint8_t* tm_gpu; uint32_t* tm_order; uint32_t tm_rows;
    char files[24][260]; int nfiles; int file_sel;
    bool pending_load;      // a recording just stopped: opened once its file is written
    char status[300];
} V;

static void view_reset(void) {
    V.v0 = 0.0, V.v1 = V.f.t_end > 0.0 ? V.f.t_end : 1.0;
    V.tv0 = V.tv1 = -1.0;
}

static int float_cmp(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b;
    return x < y ? -1 : x > y;
}

static bool open_file(const char* path) {
    if (V.loaded) trace_free(&V.f), free(V.tm_n), free(V.tm_total), free(V.tm_max), free(V.tm_gpu), free(V.tm_order);
    V.loaded = false;
    V.tm_n = NULL, V.tm_total = NULL, V.tm_max = NULL, V.tm_gpu = NULL, V.tm_order = NULL;
    if (!trace_load(path, &V.f)) {
        snprintf(V.status, sizeof V.status, TR("The trace could not be read: %s"), path);
        return false;
    }
    snprintf(V.path, sizeof V.path, "%s", path);
    memset(V.first, 0, sizeof V.first), memset(V.count, 0, sizeof V.count), memset(V.maxdur, 0, sizeof V.maxdur);
    for (uint32_t i = 0; i < V.f.n; i++) {
        const TraceEv* e = &V.f.ev[i];
        if (!V.count[e->track]) V.first[e->track] = i;
        V.count[e->track]++;
        if (e->dur > V.maxdur[e->track]) V.maxdur[e->track] = e->dur;
    }
    uint32_t nn = V.f.nnames ? V.f.nnames : 1;
    V.tm_n = calloc(nn, sizeof *V.tm_n), V.tm_total = calloc(nn, sizeof *V.tm_total), V.tm_max = calloc(nn, sizeof *V.tm_max);
    V.tm_gpu = calloc(nn, 1), V.tm_order = calloc(nn, sizeof *V.tm_order);
    V.loaded = V.tm_n && V.tm_total && V.tm_max && V.tm_gpu && V.tm_order;
    V.p99 = 5.0f, V.scroll_y = 0.0f;
    if (V.f.nframes) {
        float* s = malloc(V.f.nframes * sizeof *s);
        if (s) {
            memcpy(s, V.f.frame_ms, V.f.nframes * sizeof *s);
            qsort(s, V.f.nframes, sizeof *s, float_cmp);
            V.p99 = s[(uint32_t)((V.f.nframes - 1) * 0.99)];
            free(s);
        }
    }
    view_reset();
    V.status[0] = 0;
    return V.loaded;
}

static void list_file(void* user, const char* name) {
    (void)user;
    size_t n = strlen(name);
    if (n < 6 || strcmp(name + n - 5, ".json") || V.nfiles >= 24) return;
    snprintf(V.files[V.nfiles++], sizeof V.files[0], "%s", name);
}
static int name_desc(const void* a, const void* b) { return -strcmp(a, b); } // (trace_YYYYMMDD_HHMMSS: newest first)

static uint32_t lower_bound(uint32_t track, double t) { // the first event of the track starting at or after t
    uint32_t lo = V.first[track], hi = V.first[track] + V.count[track];
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (V.f.ev[m].ts < t) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static uint32_t color_of(uint32_t name) {
    static const uint32_t pal[12] = { TK_BLUE, TK_AMBER, TK_GREEN, TK_RED, TK_MAGENTA, TK_AMBER,
                                      TK_RED_L, TK_CYAN, TK_AMBER, TK_TEAL, TK_PINK, TK_ORANGE };
    uint32_t h = 2166136261u;
    for (const char* s = V.f.names[name]; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return TH(pal[h % 12]);
}

static int timer_cmp(const void* a, const void* b) {
    double x = V.tm_total[*(const uint32_t*)a], y = V.tm_total[*(const uint32_t*)b];
    return x > y ? -1 : x < y ? 1 : 0;
}

static void timers_update(void) {
    if (V.tv0 == V.v0 && V.tv1 == V.v1) return;
    V.tv0 = V.v0, V.tv1 = V.v1;
    uint32_t nn = V.f.nnames;
    memset(V.tm_n, 0, nn * sizeof *V.tm_n), memset(V.tm_total, 0, nn * sizeof *V.tm_total);
    memset(V.tm_max, 0, nn * sizeof *V.tm_max), memset(V.tm_gpu, 0, nn);
    for (uint32_t t = 0; t < V.f.ntracks && t < 32; t++) {
        for (uint32_t i = lower_bound(t, V.v0), end = V.first[t] + V.count[t]; i < end && V.f.ev[i].ts <= V.v1; i++) {
            const TraceEv* e = &V.f.ev[i];
            if (e->counter) continue;
            V.tm_n[e->name]++, V.tm_total[e->name] += e->dur;
            if (e->dur > V.tm_max[e->name]) V.tm_max[e->name] = e->dur;
            if (t == 0) V.tm_gpu[e->name] = 1;
        }
    }
    V.tm_rows = 0;
    for (uint32_t i = 0; i < nn; i++)
        if (V.tm_n[i]) V.tm_order[V.tm_rows++] = i;
    qsort(V.tm_order, V.tm_rows, sizeof *V.tm_order, timer_cmp);
}

static void frames_graph(float h) {
    ImVec2 p = igGetCursorScreenPos();
    ImVec2 avail = igGetContentRegionAvail();
    float w = avail.x;
    igInvisibleButton("frames", (ImVec2){ w, h }, ImGuiButtonFlags_MouseButtonLeft); // tips: none (a canvas, a node or a tile drawn with its own name and text)
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + h }, TH(TK_N4), 0.0f, 0);
    const TraceFile* f = &V.f;
    if (!f->nframes || f->t_end <= 0.0) return;
    float top = fminf(fmaxf(V.p99 * 1.6f, 6.0f), 100.0f); // (longer frames: full height)
    for (int px = 0; px < (int)w; px++) { // (each column: the slowest frame starting in it)
        double ta = f->t_end * px / w, tb = f->t_end * (px + 1) / w;
        uint32_t lo = 0, hi = f->nframes;
        while (lo < hi) {
            uint32_t m = (lo + hi) / 2;
            if (f->frames[m] < ta) lo = m + 1;
            else hi = m;
        }
        float worst = 0.0f;
        for (uint32_t i = lo; i < f->nframes && f->frames[i] < tb; i++) worst = fmaxf(worst, f->frame_ms[i]);
        if (worst <= 0.0f) continue;
        float c[3];
        dv_fps_color(1000.0 / worst, c);
        uint32_t col = 0xFF000000u | ((uint32_t)(c[2] * 255) << 16) | ((uint32_t)(c[1] * 255) << 8) | (uint32_t)(c[0] * 255);
        float bh = fminf(worst / top, 1.0f) * (h - 2.0f);
        ImDrawList_AddRectFilled(dl, (ImVec2){ p.x + px, p.y + h - bh }, (ImVec2){ p.x + px + 1, p.y + h }, col, 0.0f, 0);
    }
    const float lines[2] = { 5.0f, 16.667f }; // 200 and 60 fps
    for (int k = 0; k < 2; k++) {
        if (lines[k] > top) continue;
        float y = p.y + h - lines[k] / top * (h - 2.0f);
        ImDrawList_AddLine(dl, (ImVec2){ p.x, y }, (ImVec2){ p.x + w, y }, THA(TK_N15, 96), 1.0f);
        char t[24];
        snprintf(t, sizeof t, "%s", k ? "60 fps" : "200 fps");
        ImDrawList_AddText_Vec2(dl, (ImVec2){ k ? p.x + 3 : p.x + w - 60.0f, y - igGetFontSize() }, THA(TK_N15, 160), t, NULL);
    }
    float x0 = p.x + (float)(V.v0 / f->t_end) * w, x1 = p.x + (float)(V.v1 / f->t_end) * w;
    ImDrawList_AddRectFilled(dl, (ImVec2){ x0, p.y }, (ImVec2){ fmaxf(x1, x0 + 1.0f), p.y + h }, THA(TK_N15, 48), 0.0f, 0);
    if (igIsItemHovered(0)) {
        double t = (igGetIO_Nil()->MousePos.x - p.x) / w * f->t_end;
        uint32_t lo = 0, hi = f->nframes;
        while (lo < hi) {
            uint32_t m = (lo + hi) / 2;
            if (f->frames[m] < t) lo = m + 1;
            else hi = m;
        }
        uint32_t i = lo > 0 ? lo - 1 : 0;
        igSetTooltip(TR("Frame %u: %.2f ms (%.0f fps)\nClick: show it in the timeline"), i, f->frame_ms[i], f->frame_ms[i] > 0 ? 1000.0 / f->frame_ms[i] : 0.0);
        if (igIsItemActive() && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
            double len = f->frame_ms[i] * 0.001;
            V.v0 = f->frames[i] - len * 0.05, V.v1 = f->frames[i] + len * 1.05;
        }
    }
}

static void timeline(float h) {
    ImVec2 p = igGetCursorScreenPos();
    float w = igGetContentRegionAvail().x;
    const float label_w = 120.0f, row = igGetFontSize() + 5.0f, ruler = igGetFontSize() + 6.0f;
    igInvisibleButton("timeline", (ImVec2){ w, h }, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight); // tips: none (a canvas, a node or a tile drawn with its own name and text)
    bool hovered = igIsItemHovered(0), active = igIsItemActive();
    ImGuiIO* io = igGetIO_Nil();
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + h }, TH(TK_N3), 0.0f, 0);
    float tl = p.x + label_w, tw = w - label_w;
    if (tw < 50.0f) return;
    double span = V.v1 - V.v0;
    if (hovered && io->MouseWheel != 0.0f && (io->KeyShift || io->MousePos.x < tl)) { // Shift + wheel (or over the names): the tracks scroll
        V.scroll_y -= io->MouseWheel * row * 3.0f;
    } else if (hovered && io->MouseWheel != 0.0f) { // the wheel: zoom around the mouse
        double at = V.v0 + (io->MousePos.x - tl) / tw * span;
        double k = pow(0.8, io->MouseWheel);
        span = fmax(fmin(span * k, V.f.t_end * 1.2 + 1e-3), 2e-6);
        V.v0 = at - (at - V.v0) * (span / (V.v1 - V.v0)), V.v1 = V.v0 + span;
    }
    if (active && (io->MouseDelta.x != 0.0f)) { // a drag: pan
        double d = -io->MouseDelta.x / tw * span;
        V.v0 += d, V.v1 += d;
    }
    ImDrawList_PushClipRect(dl, p, (ImVec2){ p.x + w, p.y + h }, true);
    // the ruler: a step of 1, 2 or 5 x 10^k so that labels are ~100 px apart
    double step = pow(10.0, floor(log10(span / tw * 100.0)));
    if (span / step > tw / 100.0 * 5) step *= 5;
    else if (span / step > tw / 100.0 * 2) step *= 2;
    for (double t = ceil(V.v0 / step) * step; t <= V.v1; t += step) {
        float x = tl + (float)((t - V.v0) / span) * tw;
        ImDrawList_AddLine(dl, (ImVec2){ x, p.y }, (ImVec2){ x, p.y + h }, THA(TK_N15, 32), 1.0f);
        char s[32];
        if (step >= 1.0) snprintf(s, sizeof s, "%.0f s", t);
        else if (step >= 1e-3) snprintf(s, sizeof s, "%.*f ms", step >= 1e-2 ? 0 : 1, t * 1000.0);
        else snprintf(s, sizeof s, "%.0f us", t * 1e6);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ x + 3, p.y + 2 }, TH(TK_N12), s, NULL);
    }
    const TraceEv* hover = NULL;
    // the tracks: the GPU's first, the game thread, the workers (as Insights); scrolled under the ruler
    uint32_t order[32], no = 0;
    if (V.f.ntracks) order[no++] = 0;
    for (uint32_t t = 1; t < V.f.ntracks && t < 32; t++) order[no++] = t;
    float content = 0.0f;
    for (uint32_t oi = 0; oi < no; oi++)
        if (V.count[order[oi]]) content += fminf((float)(V.f.track_depth[order[oi]] ? V.f.track_depth[order[oi]] : 1), 12.0f) * row + 4.0f;
    V.scroll_y = fmaxf(fminf(V.scroll_y, content - (h - ruler)), 0.0f);
    float y = p.y + ruler - V.scroll_y;
    ImDrawList_PushClipRect(dl, (ImVec2){ p.x, p.y + ruler }, (ImVec2){ p.x + w, p.y + h }, true);
    for (uint32_t oi = 0; oi < no; oi++) {
        uint32_t t = order[oi];
        if (!V.count[t]) continue;
        uint32_t depth = V.f.track_depth[t] ? V.f.track_depth[t] : 1;
        if (depth > 12) depth = 12;
        float th = depth * row + 4.0f;
        ImDrawList_AddLine(dl, (ImVec2){ p.x, y }, (ImVec2){ p.x + w, y }, TH(TK_N7), 1.0f);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + 6, y + 3 }, TH(TK_N14), V.f.tracks[t][0] ? V.f.tracks[t] : "?", NULL);
        float last_x[12];
        for (int d = 0; d < 12; d++) last_x[d] = -1e9f;
        for (uint32_t i = lower_bound(t, V.v0 - V.maxdur[t]), end = V.first[t] + V.count[t]; i < end && V.f.ev[i].ts <= V.v1; i++) {
            const TraceEv* e = &V.f.ev[i];
            if (e->counter || e->depth >= depth || e->ts + e->dur < V.v0) continue;
            float x0 = tl + (float)((e->ts - V.v0) / span) * tw, x1 = tl + (float)((e->ts + e->dur - V.v0) / span) * tw;
            if (x1 - x0 < 1.0f) { // (thinner than a pixel: one mark per pixel and row)
                if (x0 - last_x[e->depth] < 1.0f) continue;
                x1 = x0 + 1.0f;
            }
            last_x[e->depth] = x1;
            float ey = y + 2.0f + e->depth * row;
            uint32_t col = color_of(e->name);
            ImDrawList_AddRectFilled(dl, (ImVec2){ fmaxf(x0, tl), ey }, (ImVec2){ x1, ey + row - 1.0f }, col, 0.0f, 0);
            if (x1 - x0 > 40.0f) {
                char s[96];
                snprintf(s, sizeof s, "%s %.2f ms", V.f.names[e->name], e->dur * 1000.0);
                ImDrawList_PushClipRect(dl, (ImVec2){ fmaxf(x0, tl), ey }, (ImVec2){ x1 - 2.0f, ey + row }, true);
                ImDrawList_AddText_Vec2(dl, (ImVec2){ fmaxf(x0, tl) + 3, ey + 1 }, TH(TK_N2), s, NULL);
                ImDrawList_PopClipRect(dl);
            }
            if (hovered && io->MousePos.y >= p.y + ruler && io->MousePos.x >= x0 && io->MousePos.x < x1 && io->MousePos.y >= ey && io->MousePos.y < ey + row) hover = e;
        }
        y += th;
    }
    ImDrawList_PopClipRect(dl);
    ImDrawList_PopClipRect(dl);
    ImDrawList_AddLine(dl, (ImVec2){ tl, p.y }, (ImVec2){ tl, p.y + h }, TH(TK_N9), 1.0f);
    if (hover)
        igSetTooltip("%s\n%.3f ms\n%s %.3f s (%s)", V.f.names[hover->name], hover->dur * 1000.0, TR("at"), hover->ts,
                     V.f.tracks[hover->track]);
}

void trace_window_load(const char* path) { // (--trace-open: the tests) opened at the next frame (a recording writing: once done)
    snprintf(V.path, sizeof V.path, "%s", path);
    V.pending_load = true;
}
bool trace_window_loaded(uint32_t* events, uint32_t* frames) {
    if (events) *events = V.loaded ? V.f.n : 0;
    if (frames) *frames = V.loaded ? V.f.nframes : 0;
    return V.loaded;
}

void trace_window(bool* open, const char* save_dir) {
    if (V.pending_load && !trace_writing()) V.pending_load = false, open_file(V.path), V.nfiles = 0;
    if (!*open) return;
    igSetNextWindowSize((ImVec2){ 1100, 640 }, ImGuiCond_FirstUseEver);
    if (!igBegin(TR("Trace"), open, 0)) {
        igEnd();
        return;
    }
    if (!V.nfiles && save_dir && save_dir[0]) {
        re_list_dir(save_dir, list_file, NULL);
        qsort(V.files, V.nfiles, sizeof V.files[0], name_desc);
        if (!V.nfiles) V.nfiles = -1; // (none: listed again after the next recording)
    }
    if (trace_active()) {
        if (igButton(TR("Stop Recording"), (ImVec2){ 0, 0 })) {
            snprintf(V.path, sizeof V.path, "%s", trace_stop());
            V.pending_load = V.path[0] != 0;
        }
        igSameLine(0, -1);
        igText(TR("Recording: %.1f s, %u events"), trace_seconds(), trace_events());
        if (trace_events() >= TRACE_MAX_EVENTS) igSameLine(0, -1), igTextColored(THV(TK_WARN), "%s", TR("(full: the rest is dropped)"));
    } else {
        if (igButton(TR("Start Recording"), (ImVec2){ 0, 0 }) && !trace_writing() && save_dir) {
            char path[800], stamp[32];
            { time_t now = time(NULL); struct tm* lt = localtime(&now); if (!lt || !strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", lt)) snprintf(stamp, sizeof stamp, "%u", (unsigned)now); }
            snprintf(path, sizeof path, "%s/trace_%s.json", save_dir, stamp);
            re_make_dirs(save_dir);
            if (!trace_start(path)) snprintf(V.status, sizeof V.status, "%s", TR("The recording could not start."));
        }
        if (trace_writing()) igSameLine(0, -1), igTextDisabled("%s", TR("Writing the file..."));
        if (V.nfiles > 0) {
            igSameLine(0, 20);
            igSetNextItemWidth(260);
            if (V.file_sel >= V.nfiles) V.file_sel = 0;
            if (igBeginCombo("##traces", V.files[V.file_sel], 0)) {
                for (int i = 0; i < V.nfiles; i++)
                    if (igSelectable_Bool(V.files[i], i == V.file_sel, 0, (ImVec2){ 0, 0 })) V.file_sel = i;
                igEndCombo();
            }
            igSameLine(0, -1);
            if (igButton(TR("Open"), (ImVec2){ 0, 0 })) {
                char path[800];
                snprintf(path, sizeof path, "%s/%s", save_dir, V.files[V.file_sel]);
                open_file(path);
            }
        }
        igSameLine(0, -1);
        if (igButton(TR("Refresh"), (ImVec2){ 0, 0 })) V.nfiles = 0;
    }
    if (V.status[0]) igTextColored(THV(TK_WARN), "%s", V.status);
    if (!V.loaded) {
        igTextWrapped("%s", TR("Start Recording, play or move around, then Stop: every frame's CPU scopes (the game thread and the workers) "
                               "and GPU passes are saved in <project>/Saved/Profiling as a Chrome trace (.json), shown here and openable "
                               "in ui.perfetto.dev or chrome://tracing. The packaged game and the command line record with --trace FILE."));
        igEnd();
        return;
    }
    {
        double fsum = 0.0;
        float worst = 0.0f;
        for (uint32_t i = 0; i < V.f.nframes; i++) fsum += V.f.frame_ms[i], worst = fmaxf(worst, V.f.frame_ms[i]);
        const char* base = strrchr(V.path, '/');
        igText(TR("%s: %u events, %u frames in %.1f s, mean %.2f ms (%.0f fps), worst %.2f ms"), base ? base + 1 : V.path,
               V.f.n, V.f.nframes, V.f.t_end, V.f.nframes ? fsum / V.f.nframes : 0.0,
               fsum > 0.0 ? V.f.nframes * 1000.0 / fsum : 0.0, worst);
        igSameLine(0, 20);
        if (igButton(TR("Show All"), (ImVec2){ 0, 0 })) view_reset();
    }
    frames_graph(64.0f);
    float avail = igGetContentRegionAvail().y;
    float tl_h = fmaxf(avail - 190.0f, 120.0f);
    timeline(tl_h);
    timers_update();
    if (igBeginTable("timers", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                     (ImVec2){ 0, 0 }, 0)) {
        igTableSetupScrollFreeze(0, 1);
        igTableSetupColumn(TR("Timer"), ImGuiTableColumnFlags_WidthStretch, 0, 0);
        igTableSetupColumn(TR("Count"), ImGuiTableColumnFlags_WidthFixed, 70, 0);
        igTableSetupColumn(TR("Total (ms)"), ImGuiTableColumnFlags_WidthFixed, 90, 0);
        igTableSetupColumn(TR("Mean (ms)"), ImGuiTableColumnFlags_WidthFixed, 90, 0);
        igTableSetupColumn(TR("Max (ms)"), ImGuiTableColumnFlags_WidthFixed, 90, 0);
        igTableHeadersRow();
        for (uint32_t r = 0; r < V.tm_rows; r++) {
            uint32_t i = V.tm_order[r];
            igTableNextRow(0, 0);
            igTableNextColumn();
            if (V.tm_gpu[i]) igTextColored(THV(TK_INFO), "%s", V.f.names[i]);
            else igText("%s", V.f.names[i]);
            igTableNextColumn(), igText("%u", V.tm_n[i]);
            igTableNextColumn(), igText("%.3f", V.tm_total[i] * 1000.0);
            igTableNextColumn(), igText("%.3f", V.tm_total[i] * 1000.0 / V.tm_n[i]);
            igTableNextColumn(), igText("%.3f", V.tm_max[i] * 1000.0);
        }
        igEndTable();
    }
    igEnd();
}
