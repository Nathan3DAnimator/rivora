// The audio assets' editor (audio_editor.h; notas-dev/audio.md): Unreal's Sound Wave, Sound Cue, Sound Attenuation,
// Sound Class, Sound Mix, Sound Concurrency and Submix editors in one window (the open asset's), the Add > Sounds menu,
// the Sound Waves' thumbnails.
#include "editor_docs.h"
#include "audio_editor.h"
#include "ui_number.h" // (2026-11-25, item 20: a typed value past the slider is kept)
#include "asset_viewer.h" // (2026-11-24: the Details' search and sections)
#include "theme.h"
#include "graph_menu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include "graph_style.h"

#include "audio.h"
#include "audio_internal.h"
#include "content.h"
#include "graph_view.h"
#include "graph_layout.h"
#include "i18n.h"
#include "metasound.h"
#include "platform.h"
#include "platform_files.h"
#include "ui_icons.h"
#include "ui_tips.h"

#define WAVE_COLS 2048

// The Sound Cue menu's recents and recommendations (graph_menu.h): a node kind by its name
static int cue_kind_named(const char* name) {
    for (int k = CUE_WAVE; k < CUE_KIND_COUNT; k++)
        if (!strcmp(g_cue_kind_names[k], name)) return k;
    return -1;
}
static bool cue_accept(const char* item, void* user) {
    const char** shown = user;
    return cue_kind_named(item) >= CUE_WAVE && (!shown[0] || strcmp(shown[0], item)) && (!shown[1] || strcmp(shown[1], item));
}

static struct {
    char vpath[512];
    MsAsset* ms;            // a MetaSound open (B8)
    SoundWave* wave;        // a Sound Wave open
    AuAsset* asset;         // or one of the small assets
    bool unsaved;
    char status[160];
    AudioHandle preview;
    // the wave's outline at the editor's resolution (min / max per column)
    float wmin[2][WAVE_COLS], wmax[2][WAVE_COLS];
    const SoundWave* wave_for;
    // the cue's graph
    GraphView view;
    int sel, drag, wire_from;   // the selected node, the dragged one, a wire being drawn from a node's output
    float add_at[2];
} E = { .sel = -1, .drag = -1, .wire_from = -1 };

static AudioHandle g_thumb_play; // a thumbnail's preview
static char g_thumb_vpath[512];

bool audio_editor_handles(const char* vpath) {
    const char* dot = strrchr(vpath, '.');
    if (!dot) return false;
    if (!_stricmp(dot, ".rsound") || !_stricmp(dot, ".rmsnd")) return true;
    for (int k = 0; k < AUA_KIND_COUNT; k++)
        if (!_stricmp(dot + 1, g_aua_ext[k])) return true;
    return false;
}

static void stop_preview(void) {
    if (E.preview) audio_stop(E.preview, 0.05f);
    E.preview = 0;
}

void audio_editor_open(const char* vpath) {
    if (content_defer_open(vpath)) return;
    stop_preview();
    E.wave = NULL, E.asset = NULL, E.ms = NULL, E.unsaved = false, E.status[0] = 0, E.sel = -1, E.drag = -1, E.wire_from = -1;
    E.view = (GraphView){ { 40.0f, 40.0f }, 1.0f, false };
    snprintf(E.vpath, sizeof E.vpath, "%s", vpath);
    const char* dot = strrchr(vpath, '.');
    audio_rescan();
    if (dot && !_stricmp(dot, ".rsound")) E.wave = audio_wave_load(vpath);
    else if (dot && !_stricmp(dot, ".rmsnd")) E.ms = ms_find_edit(audio_vpath_hash(vpath));
    else E.asset = au_asset_load(vpath);
    if (!E.wave && !E.asset && !E.ms) snprintf(E.status, sizeof E.status, "%s", TR("Could not open the asset."));
}

static void save(void) {
    bool ok = E.ms ? ms_save(E.ms) : E.wave ? audio_wave_save(E.wave) : au_asset_save(E.asset);
    snprintf(E.status, sizeof E.status, "%s", ok ? TR("Saved.") : TR("Could not save."));
    if (ok) E.unsaved = false;
}

// --- Pickers ---
bool audio_pick(const char* label, uint32_t* hash, int kind) {
    bool ch = false;
    const char* cur = *hash ? au_sound_name(*hash) : "";
    if (*hash && !cur[0]) cur = TR("(missing)");
    if (igBeginCombo(label, *hash ? cur : TR("None"), 0)) {
        if (igSelectable_Bool(TR("None"), !*hash, 0, (ImVec2){ 0, 0 })) *hash = 0, ch = true;
        if (kind < 0) { // sounds: waves, then cues
            uint32_t n = audio_wave_count();
            for (uint32_t i = 0; i < n; i++) {
                SoundWave* w = audio_wave_at(i);
                igPushID_Int((int)i);
                if (igSelectable_Bool(w->name, w->hash == *hash, 0, (ImVec2){ 0, 0 })) *hash = w->hash, ch = true;
                igPopID();
            }
        }
        uint32_t n = au_asset_count();
        for (uint32_t i = 0; i < n; i++) {
            AuAsset* a = au_asset_at(i);
            if (!(kind < 0 ? a->kind == AUA_CUE : a->kind == kind)) continue;
            igPushID_Int(100000 + (int)i);
            char nm[96];
            snprintf(nm, sizeof nm, "%s%s", a->name, a->builtin ? "  (Engine)" : "");
            if (igSelectable_Bool(nm, a->hash == *hash, 0, (ImVec2){ 0, 0 })) *hash = a->hash, ch = true;
            igPopID();
        }
        igEndCombo();
    }
    // a Sound Wave / Cue dropped from the Content Browser on the field
    if (kind < 0 && igBeginDragDropTarget()) {
        const ImGuiPayload* pl = igAcceptDragDropPayload(RE_PAYLOAD_ASSET, 0);
        if (pl && pl->Data) {
            const AssetPayload* ap = pl->Data;
            if (ap->kind == ASSET_SOUND_WAVE || ap->kind == ASSET_SOUND_CUE || ap->kind == ASSET_METASOUND) *hash = audio_vpath_hash(ap->vpath), ch = true;
        }
        igEndDragDropTarget();
    }
    return ch;
}

static bool chk(const char* label, uint8_t* v) {
    bool b = *v != 0;
    bool ch = igCheckbox(PL(TR(label)), &b);
    if (ch) *v = (uint8_t)b;
    return ch;
}
static bool num(const char* label, float* v, float speed, float lo, float hi, const char* fmt) {
    return ui_num_float(PL(TR(label)), v, speed, lo, hi, lo, 3.4e38f, fmt, 0); // (the bottom real, the top the slider's)
}
static bool combo(const char* label, uint8_t* v, const char* const* items, int n) {
    int i = *v;
    const char* tr_items[16];
    for (int k = 0; k < n && k < 16; k++) tr_items[k] = TR(items[k]);
    bool ch = igCombo_Str_arr(PL(TR(label)), &i, tr_items, n, -1);
    if (ch) *v = (uint8_t)i;
    return ch;
}

// --- Sound Wave ---
static void wave_columns(const SoundWave* w) {
    if (E.wave_for == w) return;
    E.wave_for = w;
    uint32_t ch = w->h.channels, fr = w->h.frames;
    for (uint32_t c = 0; c < 2; c++)
        for (int k = 0; k < WAVE_COLS; k++) E.wmin[c][k] = 0.0f, E.wmax[c][k] = 0.0f;
    for (int k = 0; k < WAVE_COLS && fr; k++) {
        uint64_t a = (uint64_t)fr * k / WAVE_COLS, b = (uint64_t)fr * (k + 1) / WAVE_COLS;
        uint64_t step = (b - a) > 4096 ? (b - a) / 4096 : 1; // (a long sound: sampled within the column)
        for (uint32_t c = 0; c < ch; c++) {
            int mn = 0, mx = 0;
            for (uint64_t i = a; i < b; i += step) {
                int v = w->pcm[i * ch + c];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            E.wmin[c][k] = mn / 32768.0f, E.wmax[c][k] = mx / 32768.0f;
        }
    }
}

static void wave_view(SoundWave* w) {
    wave_columns(w);
    ImVec2 o = igGetCursorScreenPos();
    ImVec2 avail = igGetContentRegionAvail();
    float W = avail.x;
    float H = fmaxf(igGetFontSize() * 9.0f, 120.0f);
    igInvisibleButton("wave", (ImVec2){ W, H }, 0); // tips: none (a canvas, a node or a tile drawn with its own name and text)
    bool clicked = igIsItemClicked(ImGuiMouseButton_Left);
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_AddRectFilled(dl, o, (ImVec2){ o.x + W, o.y + H }, TH(TK_N3), 3.0f, 0);
    uint32_t ch = w->h.channels;
    float lane = H / (float)ch;
    for (uint32_t c = 0; c < ch; c++) {
        float mid = o.y + lane * (c + 0.5f);
        ImDrawList_AddLine(dl, (ImVec2){ o.x, mid }, (ImVec2){ o.x + W, mid }, TH(TK_N7), 1.0f);
        for (int x = 0; x < (int)W; x++) {
            int k = (int)((float)x / W * WAVE_COLS);
            float a = E.wmin[c][k], b = E.wmax[c][k];
            ImDrawList_AddLine(dl, (ImVec2){ o.x + x + 0.5f, mid - b * lane * 0.46f }, (ImVec2){ o.x + x + 0.5f, mid - a * lane * 0.46f - 1.0f },
                               TH(TK_BLUE), 1.0f);
        }
    }
    // the looped part
    if (w->h.flags & SND_LOOPING) {
        float a = (float)w->h.loop_start / w->h.frames, b = (float)(w->h.loop_end ? w->h.loop_end : w->h.frames) / w->h.frames;
        ImDrawList_AddRectFilled(dl, (ImVec2){ o.x + a * W, o.y }, (ImVec2){ o.x + b * W, o.y + H }, THA(TK_GREEN, 32), 0, 0);
    }
    // the playhead
    if (E.preview && audio_is_playing(E.preview)) {
        float t = audio_playback_time(E.preview) / (float)(w->length > 0 ? w->length : 1.0);
        ImDrawList_AddLine(dl, (ImVec2){ o.x + t * W, o.y }, (ImVec2){ o.x + t * W, o.y + H }, TH(TK_N15), 1.5f);
    }
    if (clicked) { // played from where it was clicked
        ImVec2 m = igGetIO_Nil()->MousePos;
        stop_preview();
        AudioPlay p;
        memset(&p, 0, sizeof p);
        p.sound = w->hash, p.preview = true, p.bone = -1, p.start_time = (float)((m.x - o.x) / W * w->length);
        E.preview = audio_play(&p);
    }
    igSetItemTooltip("%s", TR("Click to play from there."));
}

static void wave_ui(SoundWave* w) {
    SoundWaveHeader* h = &w->h;
    wave_view(w);
    double mb = (double)h->data_bytes / (1024.0 * 1024.0);
    igTextDisabled(TR("%.2f s  |  %u Hz  |  %s  |  %.2f MB  |  %s %.1f dB  |  RMS %.1f dB"), w->length, h->sample_rate,
                   h->channels == 2 ? TR("Stereo") : TR("Mono"), mb, TR("Peak"), 20.0 * log10(fmax(h->peak, 1e-6)),
                   20.0 * log10(fmax(h->rms, 1e-6)));
    // (2026-11-24, item 8) the Details as the Sound Wave editor's: a search, sections that fold (asset_viewer.h)
    static char find[64];
    av_details_search(find, sizeof find);
    if (av_section("Info", "duration sample rate channels size peak rms format memory frames")) {
        av_row(TR("Duration"), "%.3f s", w->length);
        av_row(TR("Sample Rate"), "%u Hz", h->sample_rate);
        av_row(TR("Channels"), "%s", h->channels == 2 ? TR("Stereo") : TR("Mono"));
        av_row(TR("Frames"), "%u", h->frames);
        av_row(TR("Format"), "%s", "PCM 16-bit");
        av_row(TR("Memory"), "%.2f MB%s", mb, (h->flags & SND_STREAM) ? TR(" (streamed)") : "");
        av_row(TR("Peak"), "%.1f dB", 20.0 * log10(fmax(h->peak, 1e-6)));
        av_row("RMS", "%.1f dB", 20.0 * log10(fmax(h->rms, 1e-6)));
    }
    bool ch = false;
    if (av_section("Sound", "volume pitch priority looping loop region streaming ui")) {
    ch |= num("Volume", &h->volume, 0.01f, 0.0f, 4.0f, "%.2f");
    ch |= num("Pitch", &h->pitch, 0.01f, 0.1f, 4.0f, "%.2f");
    ch |= num("Priority", &h->priority, 0.01f, 0.0f, 100.0f, "%.2f");
    bool loop = (h->flags & SND_LOOPING) != 0, stream = (h->flags & SND_STREAM) != 0, ui = (h->flags & SND_UI) != 0;
    if (igCheckbox(PL(TR("Looping")), &loop)) h->flags = loop ? h->flags | SND_LOOPING : h->flags & ~SND_LOOPING, ch = true;
    if (loop) {
        float a = (float)h->loop_start / h->sample_rate, b = (float)(h->loop_end ? h->loop_end : h->frames) / h->sample_rate;
        if (igDragFloatRange2(PL(TR("Loop Region")), &a, &b, 0.001f, 0.0f, (float)w->length, "%.3f s", "%.3f s", ImGuiSliderFlags_AlwaysClamp)) {
            h->loop_start = (uint32_t)(a * h->sample_rate), h->loop_end = (uint32_t)(b * h->sample_rate);
            if (h->loop_end >= h->frames) h->loop_end = 0;
            ch = true;
        }
    }
    if (igCheckbox(PL(TR("Streaming")), &stream)) h->flags = stream ? h->flags | SND_STREAM : h->flags & ~SND_STREAM, ch = true;
    if (igCheckbox(PL(TR("UI Sound")), &ui)) h->flags = ui ? h->flags | SND_UI : h->flags & ~SND_UI, ch = true;
    }
    if (av_section("Settings", "sound class attenuation concurrency")) {
    ch |= audio_pick(PL(TR("Sound Class")), &h->sound_class, AUA_CLASS);
    ch |= audio_pick(PL(TR("Attenuation")), &h->attenuation, AUA_ATTENUATION);
    ch |= audio_pick(PL(TR("Concurrency")), &h->concurrency, AUA_CONCURRENCY);
    }
    av_details_end();
    if (ch) w->dirty = true, E.unsaved = true;
}

// --- The small assets ---
static const char* const g_falloff_names[AU_FALLOFF_COUNT] = { "Linear", "Logarithmic", "Inverse", "Log Reverse", "Natural Sound" };
static const char* const g_spat_names[AU_SPAT_COUNT] = { "Project Default", "Panning", "Binaural" };
static const char* const g_rule_names[AU_RES_COUNT] = { "Prevent New", "Stop Oldest", "Stop Farthest then Prevent New",
                                                        "Stop Farthest then Oldest", "Stop Lowest Priority", "Stop Quietest",
                                                        "Stop Lowest Priority then Prevent New" };
static const char* const g_fx_names[AUFX_COUNT] = { "None", "Reverb", "EQ", "Compressor", "Delay", "Filter" };
static const char* const g_fx_params[AUFX_COUNT][AU_FX_PARAMS] = {
    { 0 },
    { "Decay Time", "High Frequency Ratio", "Density", "Pre-Delay (ms)", "Wet Level", "Dry Level", "Early Reflections", "Diffusion" },
    { "Low Shelf (Hz)", "Low Gain (dB)", "Mid (Hz)", "Mid Gain (dB)", "Mid Q", "High Shelf (Hz)", "High Gain (dB)", "Output Gain (dB)" },
    { "Threshold (dB)", "Ratio", "Attack (ms)", "Release (ms)", "Makeup Gain (dB)", "Knee (dB)", "Look-Ahead (ms)", NULL },
    { "Delay Time (ms)", "Feedback", "Wet Level", "Dry Level", "Low Pass (Hz)", "Ping-Pong", NULL, NULL },
    { "Filter Type", "Cutoff (Hz)", "Q", NULL, NULL, NULL, NULL, NULL },
};

// the attenuation's curve: the volume with the distance (to its falloff's end)
static void falloff_plot(const AuAttenuation* a) {
    float vals[96], end = a->inner_radius + a->falloff_distance;
    for (int k = 0; k < 96; k++) {
        float d = end * 1.1f * k / 95.0f;
        AuAttenuation t = *a;
        float g;
        if (d <= t.inner_radius) g = 1.0f;
        else {
            float tt = fminf((d - t.inner_radius) / fmaxf(t.falloff_distance, 1.0f), 1.0f);
            switch (t.falloff) {
            case AU_FALLOFF_LINEAR: g = 1.0f - tt; break;
            case AU_FALLOFF_LOG: g = fmaxf(0.0f, 1.0f - log10f(1.0f + 9.0f * tt)); break;
            case AU_FALLOFF_LOG_REVERSE: g = fmaxf(0.0f, log10f(10.0f - 9.0f * tt)); break;
            case AU_FALLOFF_INVERSE: g = tt >= 1.0f ? 0.0f : (1.0f - tt) / (1.0f + 9.0f * tt); break;
            default: g = tt >= 1.0f ? 0.0f : powf(10.0f, (t.db_at_max < 0.0f ? t.db_at_max : -60.0f) * tt / 20.0f); break;
            }
        }
        vals[k] = g;
    }
    char ov[64];
    snprintf(ov, sizeof ov, "0 - %.0f m", end * 1.1f / 100.0f);
    igPlotLines_FloatPtr("##falloff", vals, 96, 0, ov, 0.0f, 1.0f, (ImVec2){ 0, igGetFontSize() * 5.0f }, sizeof(float));
}

static bool attenuation_ui(AuAttenuation* a) {
    bool ch = false;
    igSeparatorText(TR("Attenuation (Volume)"));
    ch |= chk("Spatialize", &a->spatialize);
    ch |= combo("Attenuation Function", &a->falloff, g_falloff_names, AU_FALLOFF_COUNT);
    ch |= num("Inner Radius", &a->inner_radius, 5.0f, 0.0f, 1e6f, "%.0f cm");
    ch |= num("Falloff Distance", &a->falloff_distance, 10.0f, 1.0f, 1e7f, "%.0f cm");
    if (a->falloff == AU_FALLOFF_NATURAL) ch |= num("dB Attenuation At Max", &a->db_at_max, 0.5f, -120.0f, -1.0f, "%.1f dB");
    falloff_plot(a);
    igSeparatorText(TR("Attenuation (Spatialization)"));
    ch |= combo("Spatialization Method", &a->method, g_spat_names, AU_SPAT_COUNT);
    ch |= num("Non-Spatialized Radius", &a->non_spatial_radius, 1.0f, 0.0f, 1e5f, "%.0f cm");
    ch |= num("3D Stereo Spread", &a->spread, 1.0f, 0.0f, 1e5f, "%.0f cm");
    igSeparatorText(TR("Attenuation (Air Absorption)"));
    ch |= chk("Enable Air Absorption", &a->air_absorption);
    if (a->air_absorption) {
        ch |= num("Low Pass Near", &a->lpf_near_hz, 50.0f, 20.0f, 20000.0f, "%.0f Hz");
        ch |= num("Low Pass Far", &a->lpf_far_hz, 50.0f, 20.0f, 20000.0f, "%.0f Hz");
        ch |= num("Absorption Min Distance", &a->absorb_min, 10.0f, 0.0f, 1e7f, "%.0f cm");
        ch |= num("Absorption Max Distance", &a->absorb_max, 10.0f, 0.0f, 1e7f, "%.0f cm");
    }
    igSeparatorText(TR("Attenuation (Occlusion)"));
    ch |= chk("Enable Occlusion", &a->occlusion);
    if (a->occlusion) {
        ch |= num("Occlusion Low Pass", &a->occlusion_lpf_hz, 50.0f, 20.0f, 20000.0f, "%.0f Hz");
        ch |= num("Occlusion Volume", &a->occlusion_volume, 0.01f, 0.0f, 1.0f, "%.2f");
        ch |= num("Occlusion Interpolation Time", &a->occlusion_interp, 0.01f, 0.0f, 5.0f, "%.2f s");
    }
    igSeparatorText(TR("Attenuation (Reverb Send)"));
    ch |= chk("Enable Reverb Send", &a->reverb_send);
    if (a->reverb_send) {
        ch |= num("Reverb Wet Level Min", &a->reverb_wet_min, 0.01f, 0.0f, 2.0f, "%.2f");
        ch |= num("Reverb Wet Level Max", &a->reverb_wet_max, 0.01f, 0.0f, 2.0f, "%.2f");
        ch |= num("Reverb Min Send Distance", &a->reverb_dist_min, 10.0f, 0.0f, 1e7f, "%.0f cm");
        ch |= num("Reverb Max Send Distance", &a->reverb_dist_max, 10.0f, 0.0f, 1e7f, "%.0f cm");
    }
    igSeparatorText(TR("Attenuation (Other)"));
    ch |= num("Doppler Intensity", &a->doppler, 0.01f, 0.0f, 1.0f, "%.2f");
    ch |= num("Priority Attenuation", &a->priority_falloff, 0.01f, 0.0f, 1.0f, "%.2f");
    return ch;
}

static bool class_ui(AuAsset* as) {
    AuClass* c = &as->u.cls;
    bool ch = false;
    ch |= audio_pick(PL(TR("Parent Class")), &c->parent, AUA_CLASS);
    if (c->parent == as->hash) c->parent = 0; // (not its own parent)
    ch |= num("Volume", &c->volume, 0.01f, 0.0f, 4.0f, "%.2f");
    ch |= num("Pitch", &c->pitch, 0.01f, 0.1f, 4.0f, "%.2f");
    ch |= num("Low Pass Filter Frequency", &c->lpf_hz, 50.0f, 0.0f, 20000.0f, "%.0f Hz");
    ch |= audio_pick(PL(TR("Default Submix")), &c->submix, AUA_SUBMIX);
    ch |= chk("UI Sound", &c->ui);
    ch |= chk("Always Play", &c->always_play);
    igSeparatorText(TR("Passive Sound Mix Modifiers"));
    ch |= audio_pick(PL(TR("Passive Sound Mix")), &c->passive_mix, AUA_MIX);
    ch |= num("Min Volume Threshold", &c->passive_threshold, 0.001f, 0.0f, 1.0f, "%.3f");
    return ch;
}

static bool mix_ui(AuMix* m) {
    bool ch = false;
    ch |= num("Fade In Time", &m->fade_in, 0.01f, 0.0f, 30.0f, "%.2f s");
    ch |= num("Fade Out Time", &m->fade_out, 0.01f, 0.0f, 30.0f, "%.2f s");
    ch |= num("Duration", &m->duration, 0.05f, 0.0f, 3600.0f, m->duration > 0.0f ? "%.2f s" : "until popped");
    igSeparatorText(TR("Sound Class Effects"));
    for (uint32_t i = 0; i < m->nadj; i++) {
        igPushID_Int((int)i);
        ch |= audio_pick(PL(TR("Sound Class")), &m->adj[i].sound_class, AUA_CLASS);
        ch |= num("Volume Adjuster", &m->adj[i].volume, 0.01f, 0.0f, 4.0f, "%.2f");
        ch |= num("Pitch Adjuster", &m->adj[i].pitch, 0.01f, 0.1f, 4.0f, "%.2f");
        ch |= num("Low Pass Filter Frequency", &m->adj[i].lpf_hz, 50.0f, 0.0f, 20000.0f, "%.0f Hz");
        ch |= chk("Apply to Children", &m->adj[i].children);
        if (igSmallButton(TR("Remove"))) {
            memmove(&m->adj[i], &m->adj[i + 1], (m->nadj - i - 1) * sizeof m->adj[0]);
            m->nadj--, ch = true;
        }
        igSeparator();
        igPopID();
    }
    if (m->nadj < AU_MIX_ADJUST && igButton(TR("Add Sound Class Effect"), (ImVec2){ 0, 0 })) {
        memset(&m->adj[m->nadj], 0, sizeof m->adj[0]);
        m->adj[m->nadj].volume = m->adj[m->nadj].pitch = 1.0f, m->adj[m->nadj].children = 1;
        m->nadj++, ch = true;
    }
    return ch;
}

static bool conc_ui(AuConcurrency* c) {
    bool ch = false;
    int n = (int)c->max_count;
    if (ui_num_int(PL(TR("Max Count")), &n, 0.2f, 0, 256, 0, 1 << 20, n ? "%d" : TR("no limit"), 0)) c->max_count = (uint32_t)n, ch = true;
    ch |= combo("Resolution Rule", &c->rule, g_rule_names, AU_RES_COUNT);
    ch |= chk("Limit To Owner", &c->per_owner);
    ch |= num("Retrigger Time", &c->retrigger_time, 0.005f, 0.0f, 10.0f, "%.3f s");
    ch |= num("Volume Scale", &c->volume_scale, 0.01f, 0.0f, 1.0f, "%.2f");
    return ch;
}

static bool effect_ui(AuEffect* e) {
    bool ch = false;
    uint8_t k = e->kind;
    if (combo("Effect", &k, g_fx_names, AUFX_COUNT)) au_effect_defaults(e, k), ch = true;
    for (int p = 0; p < AU_FX_PARAMS && e->kind; p++) {
        const char* nm = g_fx_params[e->kind][p];
        if (!nm) continue;
        if ((e->kind == AUFX_DELAY && p == 5) || (e->kind == AUFX_FILTER && p == 0)) {
            static const char* const ft[3] = { "Low Pass", "High Pass", "Band Pass" };
            int v = (int)e->p[p];
            const char* items[3] = { TR(ft[0]), TR(ft[1]), TR(ft[2]) };
            if (e->kind == AUFX_DELAY) {
                bool b = e->p[p] > 0.5f;
                if (igCheckbox(PL(TR(nm)), &b)) e->p[p] = b ? 1.0f : 0.0f, ch = true;
            } else if (igCombo_Str_arr(PL(TR(nm)), &v, items, 3, -1)) e->p[p] = (float)v, ch = true;
            continue;
        }
        float sp = fabsf(e->p[p]) > 100.0f ? 5.0f : 0.01f;
        ch |= igDragFloat(PL(TR(nm)), &e->p[p], sp, -100000.0f, 100000.0f, "%.3f", 0);
    }
    return ch;
}

static bool submix_ui(AuAsset* as) {
    AuSubmix* s = &as->u.sub;
    bool ch = false;
    ch |= audio_pick(PL(TR("Parent Submix")), &s->parent, AUA_SUBMIX);
    if (s->parent == as->hash) s->parent = 0;
    ch |= num("Output Volume", &s->volume, 0.01f, 0.0f, 4.0f, "%.2f");
    igSeparatorText(TR("Submix Effect Chain"));
    for (uint32_t i = 0; i < s->nfx; i++) {
        igPushID_Int((int)i);
        ch |= effect_ui(&s->fx[i]);
        if (igSmallButton(TR("Remove"))) {
            memmove(&s->fx[i], &s->fx[i + 1], (s->nfx - i - 1) * sizeof s->fx[0]);
            s->nfx--, ch = true;
        }
        igSeparator();
        igPopID();
    }
    if (s->nfx < AU_BUS_FX && igButton(TR("Add Effect"), (ImVec2){ 0, 0 })) {
        au_effect_defaults(&s->fx[s->nfx++], AUFX_REVERB);
        ch = true;
    }
    return ch;
}

// --- Sound Cue's graph ---
#define NODE_W 170.0f
static bool many_inputs(int kind) { return kind == CUE_RANDOM || kind == CUE_MIXER || kind == CUE_CONCAT; }
static int shown_inputs(const AuCueNode* n) {
    if (n->kind == CUE_WAVE) return 0;
    if (!many_inputs(n->kind)) return 1;
    return n->nin < AU_CUE_IN ? n->nin + 1 : AU_CUE_IN; // (a free one to connect into)
}
static float node_h(const AuCueNode* n) {
    int k = shown_inputs(n);
    return igGetFontSize() * 1.6f + fmaxf(1.0f, (float)k) * igGetFontSize() * 1.3f + 8.0f;
}
static ImVec2 in_pin(const GraphView* v, ImVec2 o, const AuCueNode* n, int k) {
    float y = n->y + igGetFontSize() * 1.6f + (k + 0.5f) * igGetFontSize() * 1.3f;
    return gv_screen(v, o, n->x, y);
}
static ImVec2 out_pin(const GraphView* v, ImVec2 o, const AuCueNode* n) {
    return gv_screen(v, o, n->x + NODE_W, n->y + igGetFontSize() * 0.8f);
}

// (2026-11-23) Organize Nodes (Q, the menu): the cue in columns toward its Output (graph_layout.c)
static bool cue_organize(AuCue* c) {
    static GlNode nodes[AU_CUE_NODES];
    static GlEdge edges[AU_CUE_NODES * AU_CUE_IN];
    int ne = 0;
    for (uint32_t i = 0; i < c->nnodes; i++) {
        nodes[i] = (GlNode){ c->n[i].x, c->n[i].y, NODE_W, node_h(&c->n[i]) };
        for (int k = 0; k < AU_CUE_IN; k++)
            if (c->n[i].in[k] >= 0 && c->n[i].in[k] < (int)c->nnodes) edges[ne++] = (GlEdge){ c->n[i].in[k], (int)i };
    }
    if (!graph_layout(nodes, (int)c->nnodes, edges, ne, NULL, GL_FLOW_RIGHT, 70.0f, 22.0f)) return false;
    for (uint32_t i = 0; i < c->nnodes; i++) c->n[i].x = roundf(nodes[i].x), c->n[i].y = roundf(nodes[i].y);
    return true;
}

static void node_remove(AuCue* c, int j) {
    if (j <= 0 || j >= (int)c->nnodes) return; // (the Output stays)
    for (uint32_t i = 0; i < c->nnodes; i++) {
        AuCueNode* n = &c->n[i];
        for (int k = 0; k < n->nin; k++) {
            if (n->in[k] == j) n->in[k] = -1;
            else if (n->in[k] > j) n->in[k]--;
        }
        if (many_inputs(n->kind)) { // (the unplugged inputs closed up)
            int w = 0;
            for (int k = 0; k < n->nin; k++)
                if (n->in[k] >= 0) n->weight[w] = n->weight[k], n->in[w++] = n->in[k];
            for (int k = w; k < AU_CUE_IN; k++) n->in[k] = -1;
            n->nin = (uint8_t)w;
        }
    }
    memmove(&c->n[j], &c->n[j + 1], (c->nnodes - j - 1) * sizeof c->n[0]);
    c->nnodes--;
}

static int node_add(AuCue* c, int kind, float x, float y) {
    if (c->nnodes >= AU_CUE_NODES) return -1;
    AuCueNode* n = &c->n[c->nnodes];
    memset(n, 0, sizeof *n);
    n->kind = (uint8_t)kind, n->x = x, n->y = y, n->last = -1;
    for (int k = 0; k < AU_CUE_IN; k++) n->in[k] = -1, n->weight[k] = 1.0f;
    if (kind == CUE_MODULATOR) n->p[0] = 0.9f, n->p[1] = 1.1f, n->p[2] = 0.95f, n->p[3] = 1.05f;
    if (kind == CUE_DELAY) n->p[0] = 0.0f, n->p[1] = 0.2f;
    if (kind == CUE_RANDOM) n->flag = 1; // (no repeats: Unreal's Randomize Without Replacement's spirit)
    if (!many_inputs(kind) && kind != CUE_WAVE) n->nin = 1;
    return (int)c->nnodes++;
}

static bool connect(AuCue* c, int from, int to, int k) {
    if (from <= 0 || to < 0 || from == to) return false;
    for (int d = 0, cur = to; d < 64 && cur >= 0; d++) { // (no loops: the target mustn't feed the source)
        (void)cur;
        break;
    }
    AuCueNode* n = &c->n[to];
    if (k >= AU_CUE_IN) return false;
    n->in[k] = (int16_t)from;
    if (k >= n->nin) n->nin = (uint8_t)(k + 1);
    return true;
}

static bool node_details(AuCue* c, AuCueNode* n) {
    bool ch = false;
    igSeparatorText(TR(g_cue_kind_names[n->kind]));
    switch (n->kind) {
    case CUE_WAVE: {
        ch |= audio_pick(PL(TR("Sound Wave")), &n->wave, AUA_KIND_COUNT); // (waves only: no asset kind matches)
        bool loop = n->flag != 0;
        if (igCheckbox(PL(TR("Looping")), &loop)) n->flag = loop, ch = true;
        break;
    }
    case CUE_RANDOM: {
        bool nr = n->flag != 0;
        if (igCheckbox(PL(TR("No Repeats")), &nr)) n->flag = nr, ch = true;
        for (int k = 0; k < n->nin; k++) {
            char l[32];
            snprintf(l, sizeof l, "%s %d", TR("Weight"), k);
            igPushID_Int(k);
            ch |= ui_num_float(PL(l), &n->weight[k], 0.01f, 0.0f, 100.0f, 0.0f, 3.4e38f, "%.2f", 0);
            igPopID();
        }
        break;
    }
    case CUE_MIXER:
        for (int k = 0; k < n->nin; k++) {
            char l[32];
            snprintf(l, sizeof l, "%s %d", TR("Input Volume"), k);
            igPushID_Int(k);
            ch |= ui_num_float(PL(l), &n->weight[k], 0.01f, 0.0f, 4.0f, 0.0f, 3.4e38f, "%.2f", 0);
            igPopID();
        }
        break;
    case CUE_MODULATOR:
        ch |= ui_num_float_range(PL(TR("Volume Range")), &n->p[0], &n->p[1], 0.01f, 0.0f, 4.0f, 0.0f, 3.4e38f, "%.2f", "%.2f", 0);
        ch |= ui_num_float_range(PL(TR("Pitch Range")), &n->p[2], &n->p[3], 0.01f, 0.1f, 4.0f, 0.01f, 3.4e38f, "%.2f", "%.2f", 0);
        break;
    case CUE_DELAY:
        ch |= ui_num_float_range(PL(TR("Delay Range")), &n->p[0], &n->p[1], 0.005f, 0.0f, 60.0f, 0.0f, 3.4e38f, "%.3f s", "%.3f s", 0);
        break;
    default: igTextDisabled("%s", TR("No settings.")); break;
    }
    (void)c;
    return ch;
}

static bool cue_ui(AuAsset* as) {
    AuCue* c = &as->u.cue;
    bool ch = false;
    ImVec2 avail = igGetContentRegionAvail();
    static float side_w; // (the bar between the graph and the side drags it; double-click: back)
    float side_def = fminf(igGetFontSize() * 22.0f, avail.x * 0.4f);
    if (side_w <= 0.0f) side_w = side_def;
    float side = fminf(side_w, fmaxf(avail.x - 200.0f, 140.0f));
    // the canvas
    igBeginChild_Str("cue_canvas", (ImVec2){ avail.x - side - UI_SPLIT, avail.y }, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 o = igGetCursorScreenPos(), size = igGetContentRegionAvail();
    ImDrawList* dl = igGetWindowDrawList();
    igInvisibleButton("canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle); // tips: none (a canvas, a node or a tile drawn with its own name and text)
    bool canvas_hovered = igIsItemHovered(0);
    gv_input(&E.view, o, canvas_hovered);
    gv_grid(&E.view, dl, o, size, TH(TK_N5));
    ImGuiIO* io = igGetIO_Nil();
    float fs = igGetFontSize();
    // the dragged node (2026-11-23: applied before drawing; after it the nodes and their wires showed a frame behind the mouse)
    if (E.drag >= 0 && igIsMouseDown_Nil(ImGuiMouseButton_Left) && E.drag < (int)c->nnodes) {
        c->n[E.drag].x += io->MouseDelta.x / E.view.zoom, c->n[E.drag].y += io->MouseDelta.y / E.view.zoom;
        if (io->MouseDelta.x != 0 || io->MouseDelta.y != 0) ch = true;
    } else E.drag = -1;
    { // (2026-11-23) Blender's edge pan while a node or a wire is dragged
        float es[2];
        if ((E.drag >= 0 || E.wire_from >= 0) && igIsMouseDown_Nil(ImGuiMouseButton_Left) && gv_edge_scroll(o, size, es)) {
            E.view.pan[0] -= es[0] / E.view.zoom, E.view.pan[1] -= es[1] / E.view.zoom;
            if (E.drag >= 0 && E.drag < (int)c->nnodes) c->n[E.drag].x += es[0] / E.view.zoom, c->n[E.drag].y += es[1] / E.view.zoom;
        }
    }
    // the wires (from a node's output, on its right, to an input on the left)
    for (uint32_t i = 0; i < c->nnodes; i++) {
        const AuCueNode* n = &c->n[i];
        for (int k = 0; k < n->nin; k++) {
            int f = n->in[k];
            if (f <= 0 || f >= (int)c->nnodes) continue;
            ImVec2 a = out_pin(&E.view, o, &c->n[f]), b = in_pin(&E.view, o, n, k);
            graph_wire(dl, a, b, TH(TK_N14), E.view.zoom, false, -1); // (Editor Preferences > Graph Style)
        }
    }
    // the nodes
    int hovered_in_node = -1, hovered_in_pin = -1;
    bool any_node_hovered = false;
    for (uint32_t i = 0; i < c->nnodes; i++) {
        AuCueNode* n = &c->n[i];
        ImVec2 a = gv_screen(&E.view, o, n->x, n->y), b = gv_screen(&E.view, o, n->x + NODE_W, n->y + node_h(n));
        uint32_t head = n->kind == CUE_OUTPUT ? TH(TK_N9) : n->kind == CUE_WAVE ? TH(TK_GREEN_D) : TH(TK_BLUE_D);
        bool orbit = graph_orbit();
        if (orbit) // (Q5) a card: the family's band and icon (a player green, the output gray, the rest blue)
            graph_card(dl, a, b, fs * 1.4f * E.view.zoom, head, n->kind == CUE_WAVE ? ICON_SOUND_WAVE : n->kind == CUE_OUTPUT ? ICON_SOUND : ICON_SOUND_CUE,
                       (int)i == E.sel, false, false, E.view.zoom);
        else {
            ImDrawList_AddRectFilled(dl, a, b, THA(TK_N5, 240), 6.0f * E.view.zoom, 0);
            ImDrawList_AddRectFilled(dl, a, (ImVec2){ b.x, a.y + fs * 1.4f * E.view.zoom }, head, 6.0f * E.view.zoom, ImDrawFlags_RoundCornersTop);
            ImDrawList_AddRect(dl, a, b, (int)i == E.sel ? TH(TK_ACCENT) : TH(TK_N10), 6.0f * E.view.zoom, (int)i == E.sel ? 2.0f : 1.0f, 0);
        }
        char title[96];
        if (n->kind == CUE_WAVE) snprintf(title, sizeof title, "%s", n->wave ? au_sound_name(n->wave) : TR("Wave Player"));
        else snprintf(title, sizeof title, "%s", TR(g_cue_kind_names[n->kind]));
        ImDrawList_AddText_Vec2(dl, (ImVec2){ orbit ? graph_card_title_x(a, fs * 1.4f * E.view.zoom, 0, E.view.zoom) : a.x + 6 * E.view.zoom, a.y + 2 * E.view.zoom },
                                TH(TK_NODE_TITLE), title, NULL);
        // inputs
        int shown = shown_inputs(n);
        for (int k = 0; k < shown; k++) {
            ImVec2 p = in_pin(&E.view, o, n, k);
            bool linked = k < n->nin && n->in[k] > 0;
            float r = 5.0f * E.view.zoom;
            ImVec2 m = io->MousePos;
            bool over = (m.x - p.x) * (m.x - p.x) + (m.y - p.y) * (m.y - p.y) < (r + 4) * (r + 4);
            if (over) hovered_in_node = (int)i, hovered_in_pin = k;
            if (orbit) graph_pin(dl, p, GP_VALUE, over ? TH(TK_TEXT_STRONG) : TH(TK_ASSET_AUDIO), 0, linked, E.view.zoom); // (Q5)
            else if (linked) ImDrawList_AddCircleFilled(dl, p, r, over ? TH(TK_N15) : TH(TK_N14), 12);
            else ImDrawList_AddCircle(dl, p, r, over ? TH(TK_N15) : TH(TK_N12), 12, 1.5f);
            if (n->kind == CUE_WAVE) continue;
            char l[24];
            snprintf(l, sizeof l, "%s", n->kind == CUE_OUTPUT ? TR("Output") : many_inputs(n->kind) ? "" : TR("In"));
            if (many_inputs(n->kind)) snprintf(l, sizeof l, "%d", k);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + 9 * E.view.zoom, p.y - fs * 0.5f }, TH(TK_N13), l, NULL);
        }
        if (n->kind == CUE_WAVE && n->wave) { // the wave's length
            SoundWave* w = audio_wave(n->wave);
            char l[48];
            snprintf(l, sizeof l, "%.2f s%s", w ? w->length : 0.0, n->flag ? "  (loop)" : "");
            ImDrawList_AddText_Vec2(dl, (ImVec2){ a.x + 8 * E.view.zoom, a.y + fs * 1.7f * E.view.zoom }, TH(TK_N12), l, NULL);
        }
        // its output
        if (n->kind != CUE_OUTPUT) {
            ImVec2 p = out_pin(&E.view, o, n);
            ImDrawList_AddTriangleFilled(dl, (ImVec2){ p.x - 4 * E.view.zoom, p.y - 5 * E.view.zoom }, (ImVec2){ p.x + 5 * E.view.zoom, p.y },
                                         (ImVec2){ p.x - 4 * E.view.zoom, p.y + 5 * E.view.zoom }, TH(TK_N14));
        }
        // interaction: a press on the output starts a wire, on the body selects and drags
        ImVec2 m = io->MousePos;
        bool in_body = m.x >= a.x && m.x <= b.x && m.y >= a.y && m.y <= b.y;
        if (in_body) any_node_hovered = true;
        if (canvas_hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
            ImVec2 op = out_pin(&E.view, o, n);
            if (n->kind != CUE_OUTPUT && fabsf(m.x - op.x) < 9 && fabsf(m.y - op.y) < 9) E.wire_from = (int)i;
            else if (in_body && hovered_in_node != (int)i) E.sel = (int)i, E.drag = (int)i;
        }
    }
    if (canvas_hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false) && !any_node_hovered && E.wire_from < 0) E.sel = -1;
    if (E.wire_from >= 0) { // the wire under the mouse, dropped on an input
        ImVec2 a = out_pin(&E.view, o, &c->n[E.wire_from]);
        ImDrawList_AddBezierCubic(dl, a, (ImVec2){ a.x + 60, a.y }, (ImVec2){ io->MousePos.x - 60, io->MousePos.y }, io->MousePos, TH(TK_BLUE_L), 2.0f, 0);
        if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
            if (hovered_in_node >= 0) ch |= connect(c, E.wire_from, hovered_in_node, hovered_in_pin);
            E.wire_from = -1;
        }
    }
    // a right click on a linked input unplugs it; elsewhere, the menu of nodes
    if (canvas_hovered && gv_right_click()) {
        if (hovered_in_node >= 0 && hovered_in_pin < c->n[hovered_in_node].nin) {
            AuCueNode* n = &c->n[hovered_in_node];
            n->in[hovered_in_pin] = -1;
            if (many_inputs(n->kind)) { // (closed up)
                for (int k = hovered_in_pin; k < n->nin - 1; k++) n->in[k] = n->in[k + 1], n->weight[k] = n->weight[k + 1];
                n->in[--n->nin] = -1;
            }
            ch = true;
        } else {
            gv_graph(&E.view, o, io->MousePos, &E.add_at[0], &E.add_at[1]);
            igOpenPopup_Str("cue_add", 0);
        }
    }
    if (igBeginPopup("cue_add", 0)) {
        if (ui_menu_item(ICON_SORT, TR("Organize Nodes"), "Q", false, c->nnodes > 1)) ch |= cue_organize(c), igCloseCurrentPopup();
        igSeparator();
        // the last 2 used in Sound Cues and 2 recommended after the selected node (graph_menu.h), then all
        const char* ctx = E.sel > 0 && E.sel < (int)c->nnodes ? g_cue_kind_names[c->n[E.sel].kind] : "";
        const char* items[8];
        const char* shown[2] = { NULL, NULL };
        int n = gm_recent(GM_SOUND_CUE, items, 8), ns = 0, pick = -1;
        for (int i = 0; i < n && ns < GM_RECENT_SHOWN; i++) {
            int k = cue_kind_named(items[i]);
            if (k < CUE_WAVE) continue;
            if (!ns) igSeparatorText(gm_title_recent());
            igPushID_Str("cr"), igPushID_Int(ns);
            if (ui_menu_item(ICON_SOUND_CUE, TR(items[i]), NULL, false, c->nnodes < AU_CUE_NODES)) pick = k;
            igPopID(), igPopID();
            shown[ns++] = items[i];
        }
        n = gm_recommend(GM_SOUND_CUE, ctx, cue_accept, (void*)shown, items, GM_RECOMMEND_SHOWN);
        for (int i = 0; i < n; i++) {
            if (!i) igSeparatorText(gm_title_recommended());
            igPushID_Str("cm"), igPushID_Int(i);
            if (ui_menu_item(ICON_SOUND_CUE, TR(items[i]), NULL, false, c->nnodes < AU_CUE_NODES)) pick = cue_kind_named(items[i]);
            igPopID(), igPopID();
        }
        igSeparatorText(TR("Add Node"));
        for (int k = CUE_WAVE; k < CUE_KIND_COUNT; k++)
            if (ui_menu_item(ICON_SOUND_CUE, TR(g_cue_kind_names[k]), NULL, false, c->nnodes < AU_CUE_NODES)) pick = k;
        if (pick >= CUE_WAVE) {
            gm_note(GM_SOUND_CUE, ctx, g_cue_kind_names[pick]);
            E.sel = node_add(c, pick, E.add_at[0], E.add_at[1]);
            ch = true;
        }
        igEndPopup();
    }
    // a Sound Wave dropped from the Content Browser: a Wave Player
    if (igBeginDragDropTarget()) {
        const ImGuiPayload* pl = igAcceptDragDropPayload(RE_PAYLOAD_ASSET, 0);
        if (pl && pl->Data && ((const AssetPayload*)pl->Data)->kind == ASSET_SOUND_WAVE) {
            float gx, gy;
            gv_graph(&E.view, o, io->MousePos, &gx, &gy);
            int k = node_add(c, CUE_WAVE, gx, gy);
            if (k >= 0) c->n[k].wave = audio_vpath_hash(((const AssetPayload*)pl->Data)->vpath), E.sel = k, ch = true;
        }
        igEndDragDropTarget();
    }
    if (igIsWindowFocused(0) && E.sel > 0 && igIsKeyPressed_Bool(ImGuiKey_Delete, false)) {
        node_remove(c, E.sel);
        E.sel = -1, ch = true;
    }
    if (igIsWindowFocused(0) && !io->WantTextInput && !io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_Q, false)) ch |= cue_organize(c); // (2026-11-23)
    igEndChild();
    // the side: the selected node's, else the cue's settings
    igSameLine(0, 0);
    ui_splitter("##cue_split", true, &side_w, -1.0f, 140.0f, avail.x - 200.0f, avail.y, side_def);
    igSameLine(0, 0);
    igBeginChild_Str("cue_side", (ImVec2){ side, avail.y }, 0, 0);
    if (E.sel >= 0 && E.sel < (int)c->nnodes && c->n[E.sel].kind != CUE_OUTPUT) ch |= node_details(c, &c->n[E.sel]);
    else {
        igSeparatorText(TR("Sound Cue"));
        ch |= num("Volume Multiplier", &c->volume, 0.01f, 0.0f, 4.0f, "%.2f");
        ch |= num("Pitch Multiplier", &c->pitch, 0.01f, 0.1f, 4.0f, "%.2f");
        ch |= num("Priority", &c->priority, 0.01f, 0.0f, 100.0f, "%.2f");
        ch |= audio_pick(PL(TR("Sound Class")), &c->sound_class, AUA_CLASS);
        ch |= audio_pick(PL(TR("Attenuation")), &c->attenuation, AUA_ATTENUATION);
        ch |= audio_pick(PL(TR("Concurrency")), &c->concurrency, AUA_CONCURRENCY);
        igTextWrapped("%s", TR("Right click the graph to add nodes; drag from a node's output to an input; right click an input to unplug it; drop Sound Waves from the Content Browser."));
    }
    igEndChild();
    return ch;
}

// --- MetaSound Source (B8): its inputs (sliders that change the preview while it plays), its nodes as rows (each input a
// value or another node's output), the program's order or what stops it compiling ---
static bool ms_input_ui(MsAsset* a, MsNode* nd, int k) {
    bool ch = false;
    igPushID_Int(k);
    const char* const* names = g_ms_in_names[nd->kind];
    igTextUnformatted(TR(names[k]), NULL);
    igSameLine(igGetFontSize() * 7.0f, -1);
    char cur[64];
    if (nd->in[k].node >= 0) snprintf(cur, sizeof cur, "#%d %s", nd->in[k].node, TR(g_ms_kind_names[a->n[nd->in[k].node].kind]));
    else snprintf(cur, sizeof cur, "%s", TR("Value"));
    igSetNextItemWidth(igGetFontSize() * 9.0f);
    if (igBeginCombo("##src", cur, 0)) {
        if (igSelectable_Bool(TR("Value"), nd->in[k].node < 0, 0, (ImVec2){ 0, 0 })) nd->in[k].node = -1, ch = true;
        for (uint32_t j = 1; j < a->nnodes; j++) {
            if (&a->n[j] == nd) continue;
            char lab[80];
            snprintf(lab, sizeof lab, "#%u %s%s%s", j, TR(g_ms_kind_names[a->n[j].kind]), a->n[j].name[0] ? " " : "", a->n[j].name);
            if (igSelectable_Bool(lab, nd->in[k].node == (int)j, 0, (ImVec2){ 0, 0 })) nd->in[k].node = (int8_t)j, ch = true;
        }
        igEndCombo();
    }
    if (nd->in[k].node < 0) {
        igSameLine(0, 4);
        igSetNextItemWidth(igGetFontSize() * 6.0f);
        ch |= igDragFloat("##v", &nd->in[k].value, fmaxf(fabsf(nd->in[k].value) * 0.01f, 0.001f), -1e6f, 1e6f, "%.3f", 0);
    }
    igPopID();
    return ch;
}
static bool metasound_ui(MsAsset* a) {
    bool ch = false, structure = false;
    // the inputs: the game's parameters (Set Float Parameter); while the preview plays, moved here they change it
    igSeparatorText(TR("Inputs"));
    for (uint32_t p = 0; p < a->nparams; p++) {
        MsNode* nd = &a->n[a->param_node[p]];
        igPushID_Int((int)p + 1000);
        igSetNextItemWidth(igGetFontSize() * 14.0f);
        float v = nd->in[0].value;
        if (igDragFloat(nd->name, &v, fmaxf(fabsf(v) * 0.01f, 0.01f), -1e6f, 1e6f, "%.2f", 0)) {
            nd->in[0].value = v, ch = true;
            if (E.preview) audio_set_param(E.preview, nd->name, v);
        }
        igPopID();
    }
    if (!a->nparams) igTextDisabled("%s", TR("None yet: add an Input node and name it (\"RPM\", \"Intensity\")."));
    igSeparatorText(TR("Nodes"));
    int del = -1;
    for (uint32_t i = 0; i < a->nnodes; i++) {
        MsNode* nd = &a->n[i];
        igPushID_Int((int)i);
        char head[96];
        snprintf(head, sizeof head, "#%u %s%s%s###n", i, TR(g_ms_kind_names[nd->kind]), nd->name[0] ? ": " : "", nd->name);
        if (igCollapsingHeader_TreeNodeFlags(head, ImGuiTreeNodeFlags_DefaultOpen)) {
            if (nd->kind == MSN_INPUT) {
                igSetNextItemWidth(igGetFontSize() * 10.0f);
                ch |= igInputText(TR("Name"), nd->name, sizeof nd->name, 0, NULL, NULL);
            }
            const char* const* modes = g_ms_mode_names[nd->kind];
            if (modes) {
                int nm = 0, md = nd->mode;
                while (modes[nm]) nm++;
                const char* tm[8];
                for (int q = 0; q < nm && q < 8; q++) tm[q] = TR(modes[q]);
                igSetNextItemWidth(igGetFontSize() * 10.0f);
                if (igCombo_Str_arr(TR("Mode"), &md, tm, nm, -1)) nd->mode = (uint8_t)md, ch = true;
            }
            for (int k = 0; k < nd->nin; k++) {
                int8_t was = nd->in[k].node;
                ch |= ms_input_ui(a, nd, k);
                structure |= was != nd->in[k].node;
            }
            if (i > 0 && igSmallButton(TR("Delete Node"))) del = (int)i;
        }
        igPopID();
    }
    if (del > 0) { // (the links after it move down; links to it become their values)
        for (uint32_t i = 0; i < a->nnodes; i++)
            for (int k = 0; k < MS_MAX_IN; k++) {
                if (a->n[i].in[k].node == del) a->n[i].in[k].node = -1;
                else if (a->n[i].in[k].node > del) a->n[i].in[k].node--;
            }
        memmove(&a->n[del], &a->n[del + 1], (a->nnodes - (uint32_t)del - 1) * sizeof a->n[0]);
        a->nnodes--, ch = structure = true;
    }
    if (a->nnodes < MS_MAX_NODES && igBeginCombo("##addms", TR("+ Add Node"), 0)) {
        for (int k = MSN_INPUT; k < MSN_KIND_COUNT; k++)
            if (igSelectable_Bool(TR(g_ms_kind_names[k]), false, 0, (ImVec2){ 0, 0 })) {
                ms_node_defaults(&a->n[a->nnodes++], k);
                ch = structure = true;
            }
        igEndCombo();
    }
    if (ch) {
        if (structure && E.preview) stop_preview(); // (the program changes: the preview starts again from Play)
        if (!ms_compile(a)) snprintf(E.status, sizeof E.status, "%s: %s", TR("Not compiled"), a->error);
        else E.status[0] = 0;
    }
    igSeparator();
    igTextDisabled(TR("%u nodes, %u run (the rest isn't connected to the Output)"), a->nnodes, a->norder);
    return ch;
}

void audio_editor_draw(struct Editor* ed, double now, float dt) {
    (void)ed, (void)now, (void)dt;
    if (g_thumb_play && !audio_is_playing(g_thumb_play)) g_thumb_play = 0, g_thumb_vpath[0] = 0;
    if (E.ms) { // (B8: a MetaSound Source)
        char title[200];
        snprintf(title, sizeof title, "%s%s - %s###audio_editor", E.ms->name, E.unsaved ? "*" : "", TR("Sound Graph"));
        igSetNextWindowSize((ImVec2){ igGetFontSize() * 34.0f, igGetFontSize() * 40.0f }, ImGuiCond_FirstUseEver);
        bool open = true;
        if (igBegin(title, &open, 0)) {
            bool focused = igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            if (ui_icon_button("au_save", ICON_SAVE, TR("Save"), TH(TK_N14), false, TR("Saves the asset (Ctrl+S).")) ||
                (focused && igGetIO_Nil()->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false)))
                save();
            igSameLine(0, 6);
            bool playing = E.preview && audio_is_playing(E.preview);
            if (ui_icon_button("au_play", playing ? ICON_CROSS : ICON_SOUND, playing ? TR("Stop") : TR("Play"), TH(TK_N14), playing,
                               TR("Plays the sound in the editor (Space).")) ||
                (focused && igIsKeyPressed_Bool(ImGuiKey_Space, false) && !igGetIO_Nil()->WantTextInput)) {
                if (playing) stop_preview();
                else {
                    AudioPlay p;
                    memset(&p, 0, sizeof p);
                    p.sound = E.ms->hash, p.preview = true, p.bone = -1;
                    E.preview = audio_play(&p);
                    for (uint32_t q = 0; E.preview && q < E.ms->nparams; q++) // (the inputs as set here)
                        audio_set_param(E.preview, E.ms->n[E.ms->param_node[q]].name, E.ms->n[E.ms->param_node[q]].in[0].value);
                }
            }
            if (E.status[0]) igSameLine(0, 10), igTextDisabled("%s", E.status);
            igSeparator();
            if (metasound_ui(E.ms)) E.unsaved = true;
        }
        igEnd();
        if (!open) stop_preview(), E.ms = NULL;
        return;
    }
    if (!E.wave && !E.asset) return;
    const char* name = E.wave ? E.wave->name : E.asset->name;
    const char* kind = E.wave ? TR("Sound Wave") : TR(g_aua_names[E.asset->kind]);
    char title[200];
    snprintf(title, sizeof title, "%s%s - %s###audio_editor", name, E.unsaved ? "*" : "", kind);
    bool cue = E.asset && E.asset->kind == AUA_CUE;
    igSetNextWindowSize((ImVec2){ igGetFontSize() * (cue ? 62.0f : 36.0f), igGetFontSize() * (cue ? 34.0f : 36.0f) }, ImGuiCond_FirstUseEver);
    bool open = true;
    if (igBegin(title, &open, 0)) {
        bool focused = igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (ui_icon_button("au_save", ICON_SAVE, TR("Save"), TH(TK_N14), false, TR("Saves the asset (Ctrl+S).")) ||
            (focused && igGetIO_Nil()->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false)))
            save();
        bool playable = E.wave || cue;
        if (playable) {
            igSameLine(0, 6);
            bool playing = E.preview && audio_is_playing(E.preview);
            if (ui_icon_button("au_play", playing ? ICON_CROSS : ICON_SOUND, playing ? TR("Stop") : TR("Play"), TH(TK_N14), playing,
                               TR("Plays the sound in the editor (Space).")) ||
                (focused && igIsKeyPressed_Bool(ImGuiKey_Space, false) && !igGetIO_Nil()->WantTextInput)) {
                if (playing) stop_preview();
                else {
                    AudioPlay p;
                    memset(&p, 0, sizeof p);
                    p.sound = E.wave ? E.wave->hash : E.asset->hash, p.preview = true, p.bone = -1;
                    E.preview = audio_play(&p);
                }
            }
        }
        if (E.status[0]) igSameLine(0, 10), igTextDisabled("%s", E.status);
        igSeparator();
        bool ch = false;
        if (E.wave) wave_ui(E.wave);
        else {
            switch (E.asset->kind) {
            case AUA_ATTENUATION: ch = attenuation_ui(&E.asset->u.att); break;
            case AUA_CLASS: ch = class_ui(E.asset); break;
            case AUA_MIX: ch = mix_ui(&E.asset->u.mix); break;
            case AUA_CONCURRENCY: ch = conc_ui(&E.asset->u.conc); break;
            case AUA_SUBMIX: ch = submix_ui(E.asset); break;
            case AUA_CUE: ch = cue_ui(E.asset); break;
            default: break;
            }
            if (E.asset->builtin) igTextDisabled("%s", TR("An engine asset: its changes last until the editor closes."));
        }
        if (ch) E.unsaved = true, E.asset->dirty = true;
    }
    igEnd();
    if (!open) {
        if (E.unsaved && !(E.asset && E.asset->builtin)) save();
        stop_preview();
        E.wave = NULL, E.asset = NULL;
    }
}

// --- The Content Browser ---
bool audio_editor_add_menu(const char* folder) {
    bool ok = !strncmp(folder, "/Content", 8);
    if (!ui_begin_menu(ICON_SOUND, TR("Sounds"), ok)) return false;
    static const struct { int kind; const char* label; const char* base; } items[] = {
        { AUA_CUE, "Sound Cue", "SCue_New" },           { AUA_ATTENUATION, "Sound Attenuation", "ATT_New" },
        { AUA_CLASS, "Sound Class", "SCL_New" },        { AUA_MIX, "Sound Mix", "MIX_New" },
        { AUA_CONCURRENCY, "Sound Concurrency", "SCON_New" }, { AUA_SUBMIX, "Submix", "SUB_New" },
    };
    bool made = false;
    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        if (!ui_menu_item(i == 0 ? ICON_SOUND : ICON_SLIDERS, TR(items[i].label), NULL, false, true)) continue;
        char vp[700], disk[1200];
        for (int k = 0; k < 1000; k++) { // (a free name)
            snprintf(vp, sizeof vp, k ? "%s/%s%d.%s" : "%s/%s.%s", folder, items[i].base, k + 1, g_aua_ext[items[i].kind]);
            if (k == 0) snprintf(vp, sizeof vp, "%s/%s.%s", folder, items[i].base, g_aua_ext[items[i].kind]);
            snprintf(disk, sizeof disk, "%s%s", audio_content_root(), vp + 8);
            if (!re_file_stat(disk, NULL, NULL, NULL)) break;
        }
        if (au_asset_create(items[i].kind, vp)) {
            audio_editor_open(vp); // (named first by the Content Browser, then opened)
            made = true;
        }
    }
    igSeparator();
    if (ui_menu_item(ICON_SOUND, TR("Sound Graph"), NULL, false, true)) { // (B8: an oscillator into the Output, to start from)
        char vp[700], disk[1200];
        for (int k = 0; k < 1000; k++) {
            if (k == 0) snprintf(vp, sizeof vp, "%s/MS_New.rmsnd", folder);
            else snprintf(vp, sizeof vp, "%s/MS_New%d.rmsnd", folder, k + 1);
            snprintf(disk, sizeof disk, "%s%s", audio_content_root(), vp + 8);
            if (!re_file_stat(disk, NULL, NULL, NULL)) break;
        }
        if (ms_create(vp)) audio_editor_open(vp), made = true;
    }
    igEndMenu();
    return made;
}

bool audio_editor_thumb(struct ImDrawList* dl, float x0, float y0, float x1, float y1, const char* vpath, bool hovered) {
    SoundWave* w = audio_wave_load(vpath);
    if (!w) return false;
    float W = x1 - x0, H = y1 - y0, mid = y0 + H * 0.5f;
    ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, TH(TK_N3), 3.0f, 0);
    for (int k = 0; k < (int)W; k++) {
        int p = (int)((float)k / W * SND_WAVE_POINTS);
        float a = w->h.wave[p < SND_WAVE_POINTS ? p : SND_WAVE_POINTS - 1] / 255.0f * H * 0.45f;
        ImDrawList_AddLine(dl, (ImVec2){ x0 + k + 0.5f, mid - a }, (ImVec2){ x0 + k + 0.5f, mid + a + 1.0f }, TH(TK_BLUE), 1.0f);
    }
    bool playing = g_thumb_play && !strcmp(g_thumb_vpath, vpath) && audio_is_playing(g_thumb_play);
    if (hovered || playing) { // Unreal's: a play button over the thumbnail; clicked: the sound plays (again: stops)
        float r = fminf(W, H) * 0.18f, cx = x0 + W * 0.5f, cy = y0 + H * 0.5f;
        ImVec2 m = igGetIO_Nil()->MousePos;
        bool over = (m.x - cx) * (m.x - cx) + (m.y - cy) * (m.y - cy) < r * r;
        ImDrawList_AddCircleFilled(dl, (ImVec2){ cx, cy }, r, over ? THA(TK_N7, 224) : THA(TK_N5, 160), 24);
        if (playing) ImDrawList_AddRectFilled(dl, (ImVec2){ cx - r * 0.35f, cy - r * 0.35f }, (ImVec2){ cx + r * 0.35f, cy + r * 0.35f }, TH(TK_N15), 0, 0);
        else ImDrawList_AddTriangleFilled(dl, (ImVec2){ cx - r * 0.3f, cy - r * 0.45f }, (ImVec2){ cx + r * 0.5f, cy }, (ImVec2){ cx - r * 0.3f, cy + r * 0.45f }, TH(TK_N15));
        if (over && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
            if (playing) audio_stop(g_thumb_play, 0.05f), g_thumb_play = 0;
            else {
                if (g_thumb_play) audio_stop(g_thumb_play, 0.05f);
                AudioPlay p;
                memset(&p, 0, sizeof p);
                p.sound = w->hash, p.preview = true, p.bone = -1;
                g_thumb_play = audio_play(&p);
                snprintf(g_thumb_vpath, sizeof g_thumb_vpath, "%s", vpath);
            }
        }
    }
    return true;
}

// --- The components' Details ---
#include "components.h"

bool audio_component_details(CAudio* a) {
    bool ch = false;
    ch |= audio_pick(PL(TR("Sound")), &a->sound, -1);
    ch |= num("Volume Multiplier", &a->volume, 0.01f, 0.0f, 4.0f, "%.2f");
    ch |= num("Pitch Multiplier", &a->pitch, 0.01f, 0.1f, 4.0f, "%.2f");
    bool b = a->auto_activate != 0;
    if (igCheckbox(PL(TR("Auto Activate")), &b)) a->auto_activate = b, ch = true;
    b = a->spatialize != 0;
    if (igCheckbox(PL(TR("Allow Spatialization")), &b)) a->spatialize = b, ch = true;
    b = a->ui != 0;
    if (igCheckbox(PL(TR("UI Sound")), &b)) a->ui = b, ch = true;
    ch |= num("Fade In", &a->fade_in, 0.01f, 0.0f, 30.0f, "%.2f s");
    ch |= num("Start Time", &a->start_time, 0.01f, 0.0f, 3600.0f, "%.2f s");
    ch |= audio_pick(PL(TR("Attenuation Settings")), &a->attenuation, AUA_ATTENUATION);
    ch |= audio_pick(PL(TR("Sound Class")), &a->sound_class, AUA_CLASS);
    ch |= audio_pick(PL(TR("Concurrency")), &a->concurrency, AUA_CONCURRENCY);
    if (a->sound && igButton(TR("Preview"), (ImVec2){ 0, 0 })) { // (heard in the editor, 2D)
        stop_preview();
        E.preview = audio_play_2d(a->sound, a->volume, a->pitch);
    }
    return ch;
}

bool audio_volume_details(CAudioVolume* v) {
    bool ch = false;
    bool b = v->enabled != 0;
    if (igCheckbox(PL(TR("Enabled")), &b)) v->enabled = b, ch = true;
    ch |= num("Priority", &v->priority, 0.1f, -1000.0f, 1000.0f, "%.1f");
    igSeparatorText(TR("Reverb"));
    b = v->reverb_on != 0;
    if (igCheckbox(PL(TR("Apply Reverb")), &b)) v->reverb_on = b, ch = true;
    if (v->reverb_on) {
        AuEffect e;
        au_effect_defaults(&e, AUFX_REVERB);
        memcpy(e.p, v->reverb, sizeof e.p);
        static const char* const presets[] = { "Small Room", "Hall", "Cave", "Outdoors" };
        static const float pv[4][8] = { { 0.6f, 0.5f, 0.2f, 5.0f, 0.3f, 1.0f, 0.8f, 0.5f },
                                        { 2.8f, 0.6f, 0.8f, 30.0f, 0.45f, 1.0f, 0.6f, 0.7f },
                                        { 5.5f, 0.4f, 1.0f, 60.0f, 0.6f, 1.0f, 0.9f, 0.6f },
                                        { 0.9f, 0.3f, 0.9f, 40.0f, 0.12f, 1.0f, 0.2f, 0.2f } };
        for (int k = 0; k < 4; k++) {
            if (k) igSameLine(0, 4);
            if (igSmallButton(TR(presets[k]))) memcpy(e.p, pv[k], sizeof e.p), ch = true;
        }
        for (int p = 0; p < 8; p++) ch |= igDragFloat(PL(TR(g_fx_params[AUFX_REVERB][p])), &e.p[p], p == 3 ? 1.0f : 0.01f, 0.0f, p == 3 ? 199.0f : 20.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        memcpy(v->reverb, e.p, sizeof v->reverb);
        ch |= num("Fade Time", &v->fade_time, 0.01f, 0.0f, 10.0f, "%.2f s");
    }
    igSeparatorText(TR("Ambient Zone"));
    ch |= num("Exterior Volume", &v->exterior_volume, 0.01f, 0.0f, 1.0f, "%.2f");
    ch |= num("Exterior LPF", &v->exterior_lpf, 50.0f, 20.0f, 20000.0f, "%.0f Hz");
    ch |= num("Interior Volume", &v->interior_volume, 0.01f, 0.0f, 1.0f, "%.2f");
    ch |= num("Interior LPF", &v->interior_lpf, 50.0f, 20.0f, 20000.0f, "%.0f Hz");
    return ch;
}

// (2026-11-25, bug-fixer) Its documents for editor_docs.h: a project switch and the editor's close end them before
// the systems free what they point at
int audio_editor_docs(int op, struct Editor* ed, char* names, size_t cap) {
    (void)ed;
    if (!E.ms && !E.wave && !E.asset) return 0;
    if (op == DOCS_OPEN) return 1;
    if (op == DOCS_CLOSE) return stop_preview(), E.ms = NULL, E.wave = NULL, E.asset = NULL, E.unsaved = false, 1;
    if (!E.unsaved || (!E.ms && !E.wave && E.asset->builtin)) return 0; // (an engine's asset: not saved, as its x)
    if (op == DOCS_UNSAVED) return editor_docs_add_name(names, cap, E.ms ? E.ms->name : E.wave ? E.wave->name : E.asset->name), 1;
    save();
    return E.unsaved;
}
