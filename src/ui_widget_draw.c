// UMG's drawing (ui_widget.h, notas-dev/umg.md): the layout of a tree into rectangles (a Canvas's anchors, the boxes'
// Auto / Fill slots, an Overlay, a Uniform Grid, a Wrap Box, a Scroll Box's offset; each widget's Render Transform,
// opacity, visibility and clip), the widgets drawn with ImGui's draw lists (the game's view, Play in Editor, the
// Designer), their input (a Button's hover / press / click, a Slider's and a Spin Box's drag, a Check Box, an Editable
// Text's typing, a Combo Box's list, a Scroll Box's wheel, the tool tips) and the Designer tab (Palette, Hierarchy,
// the preview at a chosen resolution, Details by section, the Widget Animations' timeline). Nothing allocated per frame.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "theme.h"
#include "brand.h"
#include "content.h"
#include "i18n.h"
#include "platform.h"
#ifdef __ANDROID__
void re_vibrate_ms(int ms); // (android/glfw_android.c)
#endif
#include "platform_files.h"
#include "ui_widget.h"
#include "save_progress.h"
#include "health.h"
#include "hud.h"
#include "ui_tips.h"
#include "inventory.h"
#include "ability.h"
#include "game_settings.h"
#include <GLFW/glfw3.h>
#include "audio.h"
#include "audio_internal.h"

// A sound of the project (Sound Waves and Cues) for a widget's event or an animation's key (0: none)
static bool snd_pick(const char* label, uint32_t* hash) {
    bool ch = false;
    const char* cur = *hash ? au_sound_name(*hash) : "";
    if (igBeginCombo(label, *hash ? (cur[0] ? cur : "?") : TR("None"), 0)) {
        if (igSelectable_Bool(TR("None"), !*hash, 0, (ImVec2){ 0, 0 })) *hash = 0, ch = true;
        for (uint32_t i = 0; i < audio_wave_count(); i++) {
            SoundWave* sw = audio_wave_at(i);
            igPushID_Int((int)i);
            if (igSelectable_Bool(sw->name, sw->hash == *hash, 0, (ImVec2){ 0, 0 })) *hash = sw->hash, ch = true;
            igPopID();
        }
        for (uint32_t i = 0; i < au_asset_count(); i++) {
            AuAsset* a = au_asset_at(i);
            if (a->kind != AUA_CUE) continue;
            igPushID_Int(100000 + (int)i);
            if (igSelectable_Bool(a->name, a->hash == *hash, 0, (ImVec2){ 0, 0 })) *hash = a->hash, ch = true;
            igPopID();
        }
        igEndCombo();
    }
    if (igBeginDragDropTarget()) { // (dropped from the Content Browser)
        const ImGuiPayload* pl = igAcceptDragDropPayload(RE_PAYLOAD_ASSET, 0);
        if (pl && pl->Data && (((const AssetPayload*)pl->Data)->kind == ASSET_SOUND_WAVE || ((const AssetPayload*)pl->Data)->kind == ASSET_SOUND_CUE))
            *hash = audio_vpath_hash(((const AssetPayload*)pl->Data)->vpath), ch = true;
        igEndDragDropTarget();
    }
    return ch;
}

#define IM_COL32(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

typedef struct UiRect { float x0, y0, x1, y1; } UiRect;
typedef struct UiLayout {
    UiRect r[UI_MAX_WIDGETS];    // where each widget is (screen pixels)
    UiRect clip[UI_MAX_WIDGETS]; // what of it shows (a Scroll Box clips its content)
    float alpha[UI_MAX_WIDGETS]; // its opacity times its parents'
    float content[UI_MAX_WIDGETS]; // a Scroll Box's content height (px)
    uint8_t drawn[UI_MAX_WIDGETS], hit[UI_MAX_WIDGETS]; // shown; takes the mouse
} UiLayout;

static bool collapsed(const UiWidget* w) { return !w->visible || w->vis_mode == UI_VIS_COLLAPSED; }

static UiRect rect_isect(UiRect a, UiRect b) {
    UiRect r = { fmaxf(a.x0, b.x0), fmaxf(a.y0, b.y0), fminf(a.x1, b.x1), fminf(a.y1, b.y1) };
    if (r.x1 < r.x0) r.x1 = r.x0;
    if (r.y1 < r.y0) r.y1 = r.y0;
    return r;
}

// A child in a slot rectangle by its alignment (Fill: the whole of it) and its own size
static UiRect align_in(UiRect s, const UiWidget* w, float k) {
    UiRect r = s;
    float sw = w->size[0] * k, sh = w->size[1] * k;
    if (w->h_align != UI_ALIGN_FILL) {
        float x = w->h_align == UI_ALIGN_START ? s.x0 : w->h_align == UI_ALIGN_CENTER ? (s.x0 + s.x1 - sw) * 0.5f : s.x1 - sw;
        r.x0 = x, r.x1 = x + sw;
    }
    if (w->v_align != UI_ALIGN_FILL) {
        float y = w->v_align == UI_ALIGN_START ? s.y0 : w->v_align == UI_ALIGN_CENTER ? (s.y0 + s.y1 - sh) * 0.5f : s.y1 - sh;
        r.y0 = y, r.y1 = y + sh;
    }
    return r;
}
static UiRect pad_in(UiRect s, const float p[4], float k) { return (UiRect){ s.x0 + p[0] * k, s.y0 + p[1] * k, s.x1 - p[2] * k, s.y1 - p[3] * k }; }

static void layout(const UiTree* t, int i, UiRect area, UiRect clip, float alpha, bool hit_ok, float k, UiLayout* L) {
    const UiWidget* p = &t->w[i];
    // its Render Transform: scaled about its center, translated (its children with it)
    if (p->scale[0] != 1.0f || p->scale[1] != 1.0f || p->xlate[0] != 0.0f || p->xlate[1] != 0.0f) {
        float cx = (area.x0 + area.x1) * 0.5f, cy = (area.y0 + area.y1) * 0.5f, hw = (area.x1 - area.x0) * 0.5f * p->scale[0],
              hh = (area.y1 - area.y0) * 0.5f * p->scale[1];
        cx += p->xlate[0] * k, cy += p->xlate[1] * k;
        area = (UiRect){ cx - hw, cy - hh, cx + hw, cy + hh };
    }
    L->r[i] = area;
    L->clip[i] = clip;
    L->alpha[i] = alpha * p->opacity;
    L->content[i] = 0.0f;
    bool shown = !collapsed(p) && p->vis_mode != UI_VIS_HIDDEN;
    L->drawn[i] = shown;
    L->hit[i] = shown && hit_ok && p->enabled && p->vis_mode != UI_VIS_NOT_HIT && p->vis_mode != UI_VIS_SELF_NOT_HIT;
    bool child_hit = hit_ok && p->enabled && p->vis_mode != UI_VIS_NOT_HIT;
    // the children: collapsed ones take no room; under a hidden one they hide too
    int kids[UI_MAX_WIDGETS], nk = 0;
    for (uint32_t c = (uint32_t)i + 1; c < t->n; c++)
        if (t->w[c].parent == i) {
            if (collapsed(&t->w[c]) || !shown) {
                for (uint32_t d = c; d < t->n; d++) { // (it and its subtree: not drawn)
                    int q = (int)d;
                    while (q > (int)c) q = t->w[q].parent;
                    if (q == (int)c) L->drawn[d] = L->hit[d] = 0;
                }
                continue;
            }
            kids[nk++] = (int)c;
        }
    if (!nk) return;
    float alpha_c = L->alpha[i];
    bool boxed = p->kind == UIW_BUTTON || p->kind == UIW_BORDER || p->kind == UIW_VBOX || p->kind == UIW_HBOX || p->kind == UIW_SCROLL;
    float pad = boxed ? p->padding * k : 0.0f;
    UiRect in = { area.x0 + pad, area.y0 + pad, area.x1 - pad, area.y1 - pad };
    switch (p->kind) {
    case UIW_CANVAS:
        for (int j = 0; j < nk; j++) {
            const UiWidget* w = &t->w[kids[j]];
            float pw = area.x1 - area.x0, ph = area.y1 - area.y0;
            float ax0 = area.x0 + w->anchor[0] * pw, ay0 = area.y0 + w->anchor[1] * ph;
            float ax1 = area.x0 + w->anchor[2] * pw, ay1 = area.y0 + w->anchor[3] * ph;
            UiRect r;
            if (fabsf(w->anchor[2] - w->anchor[0]) < 1e-4f) {
                r.x0 = ax0 + w->pos[0] * k - w->align[0] * w->size[0] * k;
                r.x1 = r.x0 + w->size[0] * k;
            } else
                r.x0 = ax0 + w->pos[0] * k, r.x1 = ax1 - w->size[0] * k;
            if (fabsf(w->anchor[3] - w->anchor[1]) < 1e-4f) {
                r.y0 = ay0 + w->pos[1] * k - w->align[1] * w->size[1] * k;
                r.y1 = r.y0 + w->size[1] * k;
            } else
                r.y0 = ay0 + w->pos[1] * k, r.y1 = ay1 - w->size[1] * k;
            layout(t, kids[j], r, clip, alpha_c, child_hit, k, L);
        }
        break;
    case UIW_VBOX:
    case UIW_HBOX:
    case UIW_SCROLL: { // stacked: Auto slots their size, Fill slots a share of the rest (a Scroll Box: all Auto)
        bool vert = p->kind != UIW_HBOX;
        float avail = vert ? in.y1 - in.y0 : in.x1 - in.x0, used = 0.0f, fills = 0.0f;
        for (int j = 0; j < nk; j++) {
            const UiWidget* w = &t->w[kids[j]];
            float sp = (vert ? w->slot_pad[1] + w->slot_pad[3] : w->slot_pad[0] + w->slot_pad[2]) * k;
            if (w->size_rule == 1 && p->kind != UIW_SCROLL) fills += fmaxf(w->fill, 0.0f), used += sp;
            else used += (vert ? w->size[1] : w->size[0]) * k + sp;
            if (j) used += p->padding * k;
        }
        float rest = fmaxf(avail - used, 0.0f), cursor = vert ? in.y0 : in.x0;
        if (p->kind == UIW_SCROLL) cursor -= p->scroll * k;
        UiRect cclip = p->kind == UIW_SCROLL ? rect_isect(clip, area) : clip;
        for (int j = 0; j < nk; j++) {
            const UiWidget* w = &t->w[kids[j]];
            float len = w->size_rule == 1 && p->kind != UIW_SCROLL ? (fills > 0.0f ? rest * fmaxf(w->fill, 0.0f) / fills : 0.0f)
                                                                     : (vert ? w->size[1] : w->size[0]) * k;
            float lead = (vert ? w->slot_pad[1] : w->slot_pad[0]) * k, trail = (vert ? w->slot_pad[3] : w->slot_pad[2]) * k;
            UiRect s = vert ? (UiRect){ in.x0 + w->slot_pad[0] * k, cursor + lead, in.x1 - w->slot_pad[2] * k, cursor + lead + len }
                            : (UiRect){ cursor + lead, in.y0 + w->slot_pad[1] * k, cursor + lead + len, in.y1 - w->slot_pad[3] * k };
            cursor += lead + len + trail + p->padding * k;
            // the cross axis by its alignment; along the stack an Auto slot is its size already
            UiWidget a = *w;
            if (w->size_rule != 1 || p->kind == UIW_SCROLL) {
                if (vert) a.v_align = UI_ALIGN_FILL;
                else a.h_align = UI_ALIGN_FILL;
            }
            layout(t, kids[j], align_in(s, &a, k), cclip, alpha_c, child_hit, k, L);
        }
        if (p->kind == UIW_SCROLL) L->content[i] = cursor + p->scroll * k - in.y0;
        break;
    }
    case UIW_GRID: { // Uniform Grid: the children in order, columns x rows of equal cells
        int cols = p->columns > 0 ? p->columns : 1, rows = (nk + cols - 1) / cols;
        float cw = (in.x1 - in.x0) / (float)cols, ch = (in.y1 - in.y0) / (float)(rows > 0 ? rows : 1);
        for (int j = 0; j < nk; j++) {
            const UiWidget* w = &t->w[kids[j]];
            UiRect s = { in.x0 + (float)(j % cols) * cw, in.y0 + (float)(j / cols) * ch, 0, 0 };
            s.x1 = s.x0 + cw, s.y1 = s.y0 + ch;
            layout(t, kids[j], align_in(pad_in(s, w->slot_pad, k), w, k), clip, alpha_c, child_hit, k, L);
        }
        break;
    }
    case UIW_WRAP: { // Wrap Box: in a row while they fit, then the next row
        float x = in.x0, y = in.y0, row_h = 0.0f, gap = p->padding * k;
        for (int j = 0; j < nk; j++) {
            const UiWidget* w = &t->w[kids[j]];
            float ww = (w->size[0] + w->slot_pad[0] + w->slot_pad[2]) * k, hh = (w->size[1] + w->slot_pad[1] + w->slot_pad[3]) * k;
            if (x > in.x0 && x + ww > in.x1) x = in.x0, y += row_h + gap, row_h = 0.0f;
            UiRect s = { x, y, x + ww, y + hh };
            layout(t, kids[j], pad_in(s, w->slot_pad, k), clip, alpha_c, child_hit, k, L);
            x += ww + gap;
            row_h = fmaxf(row_h, hh);
        }
        break;
    }
    default: // a Button's, a Border's, a Size Box's, an Overlay's: each child in the whole by its alignment
        for (int j = 0; j < nk; j++) {
            const UiWidget* w = &t->w[kids[j]];
            layout(t, kids[j], align_in(pad_in(in, w->slot_pad, k), w, k), clip, alpha_c, child_hit, k, L);
        }
        break;
    }
}

static ImU32 col4(const float c[4], float alpha) {
    float a = c[3] * alpha;
    return IM_COL32((int)(fminf(fmaxf(c[0], 0), 1) * 255), (int)(fminf(fmaxf(c[1], 0), 1) * 255), (int)(fminf(fmaxf(c[2], 0), 1) * 255),
                    (int)(fminf(fmaxf(a, 0), 1) * 255));
}
static void tint(const float c[4], float f, float add, float out[4]) {
    for (int k = 0; k < 3; k++) out[k] = fminf(c[k] * f + add, 1.0f);
    out[3] = c[3];
}

// A box with the widget's corners and outline
static void box(ImDrawList* dl, ImVec2 a, ImVec2 b, const float fill[4], const UiWidget* w, float k, float alpha, const float* outline) {
    float rad = fminf(w->radius * k, fminf(b.x - a.x, b.y - a.y) * 0.5f);
    if (fill[3] > 0.0f) ImDrawList_AddRectFilled(dl, a, b, col4(fill, alpha), rad, 0);
    const float* oc = outline ? outline : w->border_col;
    if (w->border_w > 0.0f && oc[3] > 0.0f) ImDrawList_AddRect(dl, a, b, col4(oc, alpha), rad, fmaxf(w->border_w * k, 1.0f), 0);
}

// --- Fonts (B7a, notas-dev/umg.md "Fontes, Rich Text e listas"): a widget's TTF / OTF loaded once into ImGui's atlas;
// ImGui 1.92 makes each size's glyphs when first drawn, so a font is sharp at any size (what MSDF gives a 2D UI). Bold
// and italic: the family's own files when there, else the regular drawn twice (bold) or sheared (italic) ---
static int combo_count(const char* text);
typedef struct UiFontSlot { char path[96]; uint8_t bold, italic, real_b, real_i; ImFont* f; } UiFontSlot;
static UiFontSlot g_fonts[32];
static uint32_t g_nfonts;
static bool font_disk(const char* vpath, char* out, size_t cap) {
    if (!strncmp(vpath, "/Content/", 9)) snprintf(out, cap, "%s/%s", ui_content_root(), vpath + 9);
    else snprintf(out, cap, "%s", vpath);
    return re_file_stat(out, NULL, NULL, NULL);
}
// a face of its family: Name-Bold.ttf, NameBold.ttf, Name_Bold.ttf or Windows' nameb.ttf (i: italic, z: both); a
// Name-Regular.ttf's family is Name
static bool face_disk(const char* vpath, bool bold, bool italic, char* out, size_t cap) {
    const char* dot = strrchr(vpath, '.');
    if (!dot) return false;
    size_t stem = (size_t)(dot - vpath);
    if (stem > 8 && !_strnicmp(vpath + stem - 8, "-Regular", 8)) stem -= 8;
    static const char* const sfx[3][4] = { { "-Bold", "Bold", "_Bold", "b" }, { "-Italic", "Italic", "_Italic", "i" },
                                           { "-BoldItalic", "BoldItalic", "_BoldItalic", "z" } };
    int k = bold && italic ? 2 : bold ? 0 : 1;
    char v[256];
    for (int s = 0; s < 4; s++) {
        snprintf(v, sizeof v, "%.*s%s%s", (int)stem, vpath, sfx[k][s], dot);
        if (font_disk(v, out, cap)) return true;
    }
    return false;
}
static ImFont* ui_font(const char* vpath, bool bold, bool italic, bool* real_b, bool* real_i) {
    *real_b = *real_i = false;
    if (!vpath || !vpath[0]) return igGetFont();
    for (uint32_t i = 0; i < g_nfonts; i++)
        if (g_fonts[i].bold == bold && g_fonts[i].italic == italic && !strcmp(g_fonts[i].path, vpath)) {
            *real_b = g_fonts[i].real_b, *real_i = g_fonts[i].real_i;
            return g_fonts[i].f ? g_fonts[i].f : igGetFont();
        }
    if (g_nfonts == 32) return igGetFont();
    UiFontSlot* u = &g_fonts[g_nfonts++];
    memset(u, 0, sizeof *u);
    snprintf(u->path, sizeof u->path, "%s", vpath);
    u->bold = bold, u->italic = italic;
    char disk[800];
    if ((bold || italic) && face_disk(vpath, bold, italic, disk, sizeof disk)) {
        u->f = ImFontAtlas_AddFontFromFileTTF(igGetIO_Nil()->Fonts, disk, 32.0f, NULL, NULL);
        u->real_b = bold, u->real_i = italic;
    } else if (bold && italic && face_disk(vpath, true, false, disk, sizeof disk)) { // (bold italic: the bold face, sheared)
        u->f = ImFontAtlas_AddFontFromFileTTF(igGetIO_Nil()->Fonts, disk, 32.0f, NULL, NULL);
        u->real_b = true;
    } else if (bold || italic) {
        bool rb, ri;
        u->f = ui_font(vpath, false, false, &rb, &ri); // (the regular one, made bold / italic when drawn)
        if (u->f == igGetFont()) u->f = NULL;
    } else if (font_disk(vpath, disk, sizeof disk))
        u->f = ImFontAtlas_AddFontFromFileTTF(igGetIO_Nil()->Fonts, disk, 32.0f, NULL, NULL);
    ui_stats()->fonts = g_nfonts;
    *real_b = u->real_b, *real_i = u->real_i;
    return u->f ? u->f : igGetFont();
}

// a text drawn at p: its outline (8 copies around), its shadow, itself (twice for a made bold), sheared for a made italic
static void text_at(ImDrawList* dl, ImFont* font, float size, ImVec2 p, ImU32 c, const char* s, const char* e, float wrap, float k,
                    bool bold, bool italic, float outline, ImU32 oc, bool shadow, ImU32 sc) {
    int v0 = dl->VtxBuffer.Size;
    if (outline > 0.0f) {
        float o = fmaxf(outline * k, 0.75f);
        static const float d[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 0.7f, 0.7f }, { -0.7f, 0.7f }, { 0.7f, -0.7f }, { -0.7f, -0.7f } };
        int nq = o < 2.0f ? 4 : 8; // (a thin outline: the 4 sides cover it; the diagonals from 2 px)
        for (int q = 0; q < nq; q++) ImDrawList_AddText_FontPtr(dl, font, size, (ImVec2){ p.x + d[q][0] * o, p.y + d[q][1] * o }, oc, s, e, wrap, NULL);
    }
    if (shadow) ImDrawList_AddText_FontPtr(dl, font, size, (ImVec2){ p.x + 1.5f * k, p.y + 1.5f * k }, sc, s, e, wrap, NULL);
    ImDrawList_AddText_FontPtr(dl, font, size, p, c, s, e, wrap, NULL);
    if (bold) ImDrawList_AddText_FontPtr(dl, font, size, (ImVec2){ p.x + fmaxf(size * 0.035f, 0.6f), p.y }, c, s, e, wrap, NULL);
    if (italic) // (the letters leaned 12 degrees around each line's baseline)
        for (int v = v0; v < dl->VtxBuffer.Size; v++) {
            ImDrawVert* q = &dl->VtxBuffer.Data[v];
            float line = floorf((q->pos.y - p.y) / size);
            q->pos.x += (p.y + (line + 0.8f) * size - q->pos.y) * 0.21f;
        }
}

// A text in a rectangle by its justification (x: 0 left .. 1 right, y: 0 top .. 1 bottom), its font, bold, italic,
// outline, shadow, wrapped or not
static void text_in(ImDrawList* dl, ImFont* font, const UiWidget* w, const char* s, UiRect r, float k, float alpha, const float col[4], float jx, float jy) {
    bool rb, ri;
    if (w->font[0] || w->bold || w->italic) font = ui_font(w->font, w->bold, w->italic, &rb, &ri);
    else rb = ri = false;
    float size = fmaxf(w->font_size * k, 6.0f), wrap = w->wrap ? r.x1 - r.x0 : 0.0f;
    ImVec2 ts = ImFont_CalcTextSizeA(font, size, 1e9f, wrap, s, NULL, NULL);
    ImVec2 p = { r.x0 + (r.x1 - r.x0 - ts.x) * jx, r.y0 + (r.y1 - r.y0 - ts.y) * jy };
    text_at(dl, font, size, p, col4(col, alpha), s, NULL, wrap, k, w->bold && !rb, w->italic && !ri, w->outline, col4(w->outline_col, alpha),
            w->shadow, col4((float[4]){ 0, 0, 0, 0.55f }, alpha * col[3]));
}

// --- Rich Text (B7b, Unreal's Rich Text Block): runs of the text between its marks, laid out word by word, wrapped ---
typedef struct RichRun { const char* s; int n; uint8_t bold, italic, outline; float size; float col[4]; } RichRun;
static int hexd(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int rich_runs(const char* s, const UiWidget* w, RichRun* out, int cap) {
    struct { uint8_t b, i, o; float size; float col[4]; } st[16];
    int depth = 0, n = 0;
    st[0].b = w->bold, st[0].i = w->italic, st[0].o = w->outline > 0.0f, st[0].size = w->font_size;
    memcpy(st[0].col, w->color, 16);
    const char* run = s;
    for (const char* p = s;; p++) {
        bool tag = *p == '<' && (p[1] == '/' || p[1] == 'b' || p[1] == 'i' || p[1] == 'c' || p[1] == 's' || p[1] == 'o' || p[1] == 'B' || p[1] == 'I');
        const char* close = tag ? strchr(p, '>') : NULL;
        if (*p && !(tag && close)) continue;
        if (p > run && n < cap) { // the text so far, in the current style
            RichRun* r = &out[n++];
            r->s = run, r->n = (int)(p - run), r->bold = st[depth].b, r->italic = st[depth].i, r->outline = st[depth].o, r->size = st[depth].size;
            memcpy(r->col, st[depth].col, 16);
        }
        if (!*p) break;
        // the mark: </...> pops, <b> <i> <outline> <size=N> <color=#RRGGBB[AA]> push
        if (p[1] == '/') {
            if (depth > 0) depth--;
        } else if (depth < 15) {
            st[depth + 1] = st[depth];
            depth++;
            char t = (char)(p[1] | 32);
            if (t == 'b') st[depth].b = 1;
            else if (t == 'i') st[depth].i = 1;
            else if (t == 'o') st[depth].o = 1;
            else if (t == 's') {
                const char* eq = strchr(p, '=');
                if (eq && eq < close) st[depth].size = fmaxf((float)atof(eq + 1), 4.0f);
            } else if (t == 'c') {
                const char* h = strchr(p, '#');
                if (h && h < close) {
                    float c[4] = { 1, 1, 1, st[depth].col[3] };
                    int nd = 0;
                    while (h + 1 + nd < close && hexd(h[1 + nd]) >= 0) nd++;
                    for (int q = 0; q < 4 && 2 * q + 1 < nd; q++) c[q] = (float)(hexd(h[1 + 2 * q]) * 16 + hexd(h[2 + 2 * q])) / 255.0f;
                    for (int q = 0; q < 3; q++) c[q] = c[q] <= 0.04045f ? c[q] / 12.92f : powf((c[q] + 0.055f) / 1.055f, 2.4f); // (sRGB in the mark)
                    memcpy(st[depth].col, c, 16);
                }
            }
        }
        p = close;
        run = p + 1;
    }
    return n;
}
static void draw_rich(ImDrawList* dl, const UiWidget* w, const char* s, UiRect r, float k, float alpha) {
    static RichRun runs[128];
    int n = rich_runs(s, w, runs, 128);
    ui_stats()->rich_runs = (uint32_t)n;
    float x = r.x0, y = r.y0, line_h = 0.0f, maxw = r.x1 - r.x0;
    for (int i = 0; i < n; i++) {
        RichRun* u = &runs[i];
        bool rb, ri;
        ImFont* f = ui_font(w->font, u->bold, u->italic, &rb, &ri);
        float size = fmaxf(u->size * k, 6.0f);
        const char* p = u->s, *end = u->s + u->n;
        while (p < end) { // word by word (a word with the spaces after it), to the next line when it doesn't fit
            const char* q = p;
            while (q < end && *q != ' ' && *q != '\n') q++;
            while (q < end && *q == ' ') q++;
            bool nl = q < end && *q == '\n';
            ImVec2 ws = ImFont_CalcTextSizeA(f, size, 1e9f, 0.0f, p, q, NULL);
            if (w->wrap && x > r.x0 && x + ws.x > r.x0 + maxw) x = r.x0, y += line_h, line_h = 0.0f;
            text_at(dl, f, size, (ImVec2){ x, y }, col4(u->col, alpha), p, q, 0.0f, k, u->bold && !rb, u->italic && !ri, u->outline ? fmaxf(w->outline, 1.5f) : 0.0f,
                    col4(w->outline_col[3] > 0.0f ? w->outline_col : (float[4]){ 0, 0, 0, 1 }, alpha), w->shadow, col4((float[4]){ 0, 0, 0, 0.55f }, alpha * u->col[3]));
            x += ws.x, line_h = fmaxf(line_h, size * 1.15f);
            p = q;
            if (nl) x = r.x0, y += line_h, line_h = 0.0f, p++;
        }
    }
}

// --- List View and Tile View (B7b): only the rows (tiles) on the screen are drawn and walked to ---
static float list_row_h(const UiWidget* w, float k) { return fmaxf(w->font_size * 1.6f, 12.0f) * k; }
static int list_count(const char* s) { return s[0] ? combo_count(s) : 0; }
static const char* list_skip(const char* s, int n) { // (the n-th item's start)
    for (int i = 0; i < n && s; i++) {
        s = strchr(s, '|');
        if (s) s++;
    }
    return s ? s : "";
}
static void list_geom(const UiWidget* w, UiRect r, float k, int* cols, float* cw, float* ch) {
    *cols = w->kind == UIW_TILE_VIEW ? (w->columns > 0 ? w->columns : 4) : 1;
    *cw = (r.x1 - r.x0 - 2.0f * w->padding * k) / (float)*cols;
    *ch = w->kind == UIW_TILE_VIEW ? fmaxf(*cw * 0.8f, list_row_h(w, k)) : list_row_h(w, k);
}
static int list_hit(const UiWidget* w, const char* items, UiRect r, float k, float mx, float my) {
    int cols;
    float cw, ch;
    list_geom(w, r, k, &cols, &cw, &ch);
    float x = mx - r.x0 - w->padding * k, y = my - r.y0 - w->padding * k + w->scroll * k;
    if (x < 0.0f || y < 0.0f || mx > r.x1 || my > r.y1) return -1;
    int c = (int)(x / cw), row = (int)(y / ch), idx = row * cols + (c < cols ? c : cols - 1);
    return idx < list_count(items) ? idx : -1;
}
static float list_content(const UiWidget* w, const char* items, UiRect r, float k) {
    int cols;
    float cw, ch;
    list_geom(w, r, k, &cols, &cw, &ch);
    int rows = (list_count(items) + cols - 1) / cols;
    return (float)rows * ch + 2.0f * w->padding * k;
}
static void draw_list(ImDrawList* dl, ImFont* font, const UiWidget* w, const char* items, UiRect r, float k, float al, int hot_item) {
    box(dl, (ImVec2){ r.x0, r.y0 }, (ImVec2){ r.x1, r.y1 }, w->bg, w, k, al, NULL);
    int cols, n = list_count(items);
    float cw, ch;
    list_geom(w, r, k, &cols, &cw, &ch);
    float top = w->scroll * k;
    int first_row = (int)floorf(top / ch), last_row = (int)ceilf((top + (r.y1 - r.y0)) / ch);
    int first = first_row * cols, last = (last_row + 1) * cols;
    if (last > n) last = n;
    const char* p = list_skip(items, first);
    uint32_t drawn = 0;
    static const float sel_default[4] = { 0.15f, 0.55f, 1.0f, 0.55f };
    for (int i = first; i < last && p; i++) {
        const char* e = strchr(p, '|');
        int row = i / cols, col = i % cols;
        float x0 = r.x0 + w->padding * k + (float)col * cw, y0 = r.y0 + w->padding * k + (float)row * ch - top;
        UiRect cell = { x0 + (cols > 1 ? 2.0f * k : 0.0f), y0 + 1.0f * k, x0 + cw - (cols > 1 ? 2.0f * k : 0.0f), y0 + ch - 1.0f * k };
        bool sel = (int)w->value == i, hv = i == hot_item;
        if (cols > 1 || sel || hv) {
            float c[4];
            if (sel) memcpy(c, w->press[3] > 0.0f ? w->press : sel_default, 16);
            else if (hv) memcpy(c, w->hover, 16), c[3] *= 0.6f;
            else c[0] = c[1] = c[2] = 1.0f, c[3] = 0.06f;
            ImDrawList_AddRectFilled(dl, (ImVec2){ cell.x0, cell.y0 }, (ImVec2){ cell.x1, cell.y1 }, col4(c, al), w->radius * k, 0);
        }
        char txt[256];
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len >= sizeof txt) len = sizeof txt - 1;
        memcpy(txt, p, len), txt[len] = 0;
        UiRect tr = { cell.x0 + 8.0f * k, cell.y0, cell.x1 - 4.0f * k, cell.y1 };
        text_in(dl, font, w, txt, tr, k, al, w->color, cols > 1 ? 0.5f : 0.0f, 0.5f);
        drawn++;
        p = e ? e + 1 : NULL;
    }
    UiStats* s = ui_stats();
    s->list_items = (uint32_t)n, s->list_rows = drawn;
    float content = list_content(w, items, r, k), view = r.y1 - r.y0;
    if (content > view + 1.0f) { // the scroll bar
        float bh = fmaxf(view * view / content, 12.0f * k), by = r.y0 + (view - bh) * fminf(top / fmaxf(content - view, 1.0f), 1.0f);
        ImDrawList_AddRectFilled(dl, (ImVec2){ r.x1 - 5.0f * k, by }, (ImVec2){ r.x1 - 2.0f * k, by + bh }, col4((float[4]){ 1, 1, 1, 0.3f }, al), 2.0f * k, 0);
    }
}

static void combo_label(const char* text, int k, char* out, size_t cap) {
    const char* s = text;
    for (int i = 0; i < k && s; i++) {
        s = strchr(s, '|');
        if (s) s++;
    }
    if (!s) s = "";
    const char* e = strchr(s, '|');
    size_t n = e ? (size_t)(e - s) : strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n), out[n] = 0;
}
static int combo_count(const char* text) {
    if (!text[0]) return 0;
    int n = 1;
    for (const char* s = text; *s; s++) n += *s == '|';
    return n;
}

typedef struct UiState { int hot, held, focus, open_combo; bool designer; double now; int nav, capture; Entity owner; } UiState;
struct UiLive;
static void draw_touch(ImDrawList* dl, const struct UiLive* l, const UiTree* t, const UiLayout* L, int i, float k, float al, bool designer);

// The widgets of a tree into their rectangles
static void draw_tree(ImDrawList* dl, const UiTree* t, const UiLayout* L, float k, const UiState* st) {
    ImFont* font = igGetFont();
    static const float accent[4] = { 0.15f, 0.55f, 1.0f, 1.0f };
    for (uint32_t i = 0; i < t->n; i++) {
        const UiWidget* w = &t->w[i];
        if (!L->drawn[i]) continue;
        UiRect cr = L->clip[i];
        if (cr.x1 <= cr.x0 || cr.y1 <= cr.y0) continue;
        ImDrawList_PushClipRect(dl, (ImVec2){ cr.x0, cr.y0 }, (ImVec2){ cr.x1, cr.y1 }, true);
        float al = L->alpha[i] * (w->enabled ? 1.0f : 0.45f);
        ImVec2 a = { L->r[i].x0, L->r[i].y0 }, b = { L->r[i].x1, L->r[i].y1 };
        bool hot = (int)i == st->hot || (int)i == st->nav, held = (int)i == st->held;
        // (the game's widgets in the game's language: a text the engine translates shows translated; bound ones as they are)
        const char* wtext = st->designer || w->bind_text[0] || w->bind_setting[0] ? w->text : gus_tr(w->text);
        switch (w->kind) {
        case UIW_BUTTON: {
            float bg[4];
            if (held) {
                if (w->press[3] > 0.0f) memcpy(bg, w->press, 16);
                else tint(w->bg, 0.75f, 0.0f, bg);
            } else if (hot) {
                if (w->hover[3] > 0.0f) memcpy(bg, w->hover, 16);
                else tint(w->bg, 1.35f, 0.05f, bg);
            } else memcpy(bg, w->bg, 16);
            float oc[4] = { w->border_col[0], w->border_col[1], w->border_col[2], w->border_col[3] * (hot ? 2.8f : 1.0f) };
            box(dl, a, b, bg, w, k, al, oc);
            break;
        }
        case UIW_BORDER:
        case UIW_SIZEBOX:
        case UIW_OVERLAY:
        case UIW_GRID:
        case UIW_WRAP:
        case UIW_VBOX:
        case UIW_HBOX:
        case UIW_CANVAS: box(dl, a, b, w->bg, w, k, al, NULL); break;
        case UIW_SCROLL: {
            box(dl, a, b, w->bg, w, k, al, NULL);
            float view = b.y - a.y, content = L->content[i];
            if (content > view + 1.0f) { // its scroll bar
                float th = fmaxf(view * view / content, 18.0f * k), at = a.y + (view - th) * (w->scroll * k) / (content - view);
                ImDrawList_AddRectFilled(dl, (ImVec2){ b.x - 6.0f * k, at }, (ImVec2){ b.x - 2.0f * k, at + th }, col4((float[4]){ 1, 1, 1, 0.35f }, al), 2.0f * k, 0);
            }
            break;
        }
        case UIW_PROGRESS: {
            box(dl, a, b, w->bg, w, k, al, NULL);
            float p = fminf(fmaxf(w->percent, 0.0f), 1.0f);
            if (p > 0.0f) ImDrawList_AddRectFilled(dl, a, (ImVec2){ a.x + (b.x - a.x) * p, b.y }, col4(w->color, al), fminf(w->radius * k, (b.y - a.y) * 0.5f), 0);
            break;
        }
        case UIW_IMAGE: {
            float aspect = 1.0f;
            void* tex = NULL;
            char tkey[SAVE_SLOT_NAME + 8];
            uint32_t tw = 0, th = 0, tgen = 0;
            const uint8_t* tpx = !strncmp(w->image, "save:", 5) ? (snprintf(tkey, sizeof tkey, "%s", w->image), progress_thumb(w->image + 5, &tw, &th, &tgen))
                               : save_menu_thumb(w->bind_setting, tkey, sizeof tkey, &tw, &th, &tgen); // (a save's picture: save_progress.h)
            if (tpx) tex = brand_ui_image_rgb(tkey, tpx, tw, th, tgen);
            else if (w->image[0] && strncmp(w->image, "save:", 5)) tex = brand_ui_image(ui_content_root(), w->image, &aspect);
            ImTextureRef_c tr = { NULL, (ImTextureID)(uintptr_t)tex };
            if (tex && fabsf(w->angle) > 0.01f) { // turned about its center
                float cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.5f, hw = (b.x - a.x) * 0.5f, hh = (b.y - a.y) * 0.5f;
                float cs = cosf(w->angle * 0.0174533f), sn = sinf(w->angle * 0.0174533f);
                ImVec2 q[4];
                const float cx4[4] = { -1, 1, 1, -1 }, cy4[4] = { -1, -1, 1, 1 };
                for (int c = 0; c < 4; c++) q[c] = (ImVec2){ cx + cx4[c] * hw * cs - cy4[c] * hh * sn, cy + cx4[c] * hw * sn + cy4[c] * hh * cs };
                ImDrawList_AddImageQuad(dl, tr, q[0], q[1], q[2], q[3], (ImVec2){ 0, 0 }, (ImVec2){ 1, 0 }, (ImVec2){ 1, 1 }, (ImVec2){ 0, 1 }, col4(w->color, al));
            } else if (tex)
                ImDrawList_AddImageRounded(dl, tr, a, b, (ImVec2){ 0, 0 }, (ImVec2){ 1, 1 }, col4(w->color, al), w->radius * k, 0);
            else
                ImDrawList_AddRectFilled(dl, a, b, col4(w->color, al), w->radius * k, 0);
            break;
        }
        case UIW_RICH_TEXT: draw_rich(dl, w, wtext, L->r[i], k, al); break;
        case UIW_LIST_VIEW: case UIW_TILE_VIEW: {
            const char* items = st->designer ? w->text : ui_list_items(st->owner, (int)i, w);
            int hv = -1;
            if (hot && !st->designer) {
                ImVec2 m = igGetIO_Nil()->MousePos;
                hv = list_hit(w, items, L->r[i], k, m.x, m.y);
            }
            draw_list(dl, font, w, items, L->r[i], k, al, hv);
            break;
        }
        case UIW_TEXT: {
            bool in_button = w->parent > 0 && t->w[w->parent].kind == UIW_BUTTON && w->h_align == UI_ALIGN_FILL; // (a Button's label: centered)
            text_in(dl, font, w, wtext, L->r[i], k, al, w->color, in_button ? 0.5f : w->align[0], in_button ? 0.5f : w->align[1]);
            break;
        }
        case UIW_SLIDER: { // the track, its filled part, the handle
            float range = w->vmax > w->vmin ? w->vmax - w->vmin : 1.0f, u = fminf(fmaxf((w->value - w->vmin) / range, 0.0f), 1.0f);
            float cy = (a.y + b.y) * 0.5f, th = fmaxf((b.y - a.y) * 0.18f, 2.0f), hr = (b.y - a.y) * 0.32f, x = a.x + hr + (b.x - a.x - 2 * hr) * u;
            ImDrawList_AddRectFilled(dl, (ImVec2){ a.x + hr, cy - th }, (ImVec2){ b.x - hr, cy + th }, col4(w->bg, al), th, 0);
            ImDrawList_AddRectFilled(dl, (ImVec2){ a.x + hr, cy - th }, (ImVec2){ x, cy + th }, col4(w->color, al), th, 0);
            float hc[4] = { 0.92f, 0.92f, 0.95f, 1.0f };
            if (hot || held) tint(hc, 1.0f, 0.08f, hc);
            ImDrawList_AddCircleFilled(dl, (ImVec2){ x, cy }, hr * (held ? 1.12f : 1.0f), col4(hc, al), 20);
            break;
        }
        case UIW_CHECKBOX: {
            float s = fminf(b.x - a.x, b.y - a.y);
            ImVec2 a2 = a, b2 = { a.x + s, a.y + s };
            float bg[4];
            memcpy(bg, w->checked ? w->color : w->bg, 16);
            if (hot) tint(bg, 1.2f, 0.04f, bg);
            box(dl, a2, b2, bg, w, k, al, NULL);
            if (w->checked) { // the check mark
                ImVec2 m[3] = { { a.x + s * 0.24f, a.y + s * 0.52f }, { a.x + s * 0.43f, a.y + s * 0.70f }, { a.x + s * 0.77f, a.y + s * 0.30f } };
                ImDrawList_AddPolyline(dl, m, 3, col4((float[4]){ 1, 1, 1, 1 }, al), fmaxf(s * 0.11f, 1.5f), 0);
            }
            break;
        }
        case UIW_EDIT:
        case UIW_COMBO:
        case UIW_SPIN: {
            bool focus = (int)i == st->focus || (w->kind == UIW_COMBO && (int)i == st->open_combo);
            float bg[4];
            tint(w->bg, hot && w->enabled ? 1.25f : 1.0f, hot ? 0.03f : 0.0f, bg);
            box(dl, a, b, bg, w, k, al, focus ? accent : NULL);
            UiRect tr = { a.x + 10.0f * k, a.y, b.x - 10.0f * k, b.y };
            if (w->kind == UIW_EDIT) {
                bool hint = !w->text[0];
                const float gray[4] = { 0.55f, 0.55f, 0.6f, 1.0f };
                text_in(dl, font, w, hint ? (st->designer ? w->hint : gus_tr(w->hint)) : w->text, tr, k, al, hint ? gray : w->color, 0.0f, 0.5f);
                if (focus && fmod(st->now, 1.0) < 0.55) { // the caret after the text
                    float size = fmaxf(w->font_size * k, 6.0f);
                    ImVec2 ts = ImFont_CalcTextSizeA(font, size, 1e9f, 0.0f, w->text, NULL, NULL);
                    float x = tr.x0 + (hint ? 0.0f : ts.x) + 1.0f;
                    ImDrawList_AddLine(dl, (ImVec2){ x, (a.y + b.y) * 0.5f - size * 0.5f }, (ImVec2){ x, (a.y + b.y) * 0.5f + size * 0.5f }, col4(w->color, al), fmaxf(1.5f * k, 1.0f));
                }
            } else if (w->kind == UIW_COMBO) {
                char opt[128];
                combo_label(w->text, (int)w->value, opt, sizeof opt);
                text_in(dl, font, w, opt, (UiRect){ tr.x0, tr.y0, tr.x1 - 18.0f * k, tr.y1 }, k, al, w->color, 0.0f, 0.5f);
                float cx = b.x - 16.0f * k, cy = (a.y + b.y) * 0.5f, s = 5.0f * k; // the arrow
                ImDrawList_AddTriangleFilled(dl, (ImVec2){ cx - s, cy - s * 0.5f }, (ImVec2){ cx + s, cy - s * 0.5f }, (ImVec2){ cx, cy + s * 0.6f }, col4(w->color, al * 0.8f));
            } else {
                char v[32];
                snprintf(v, sizeof v, w->vstep >= 1.0f ? "%.0f" : "%.2f", w->value);
                text_in(dl, font, w, v, tr, k, al, w->color, 0.5f, 0.5f);
                float cy = (a.y + b.y) * 0.5f, s = 4.0f * k; // the drag arrows, left and right
                ImDrawList_AddTriangleFilled(dl, (ImVec2){ a.x + 6 * k, cy }, (ImVec2){ a.x + 6 * k + s, cy - s }, (ImVec2){ a.x + 6 * k + s, cy + s }, col4(w->color, al * 0.5f));
                ImDrawList_AddTriangleFilled(dl, (ImVec2){ b.x - 6 * k, cy }, (ImVec2){ b.x - 6 * k - s, cy - s }, (ImVec2){ b.x - 6 * k - s, cy + s }, col4(w->color, al * 0.5f));
            }
            break;
        }
        case UIW_THROBBER: { // dots pulsing one after another
            int n = 3;
            float r = fminf((b.y - a.y) * 0.35f, (b.x - a.x) / (float)(n * 3)), gap = (b.x - a.x) / (float)n;
            for (int d = 0; d < n; d++) {
                float ph = (float)fmod(st->now * 2.0 - d * 0.22, 1.0), s = 0.55f + 0.45f * (0.5f + 0.5f * sinf(ph * 6.2831853f));
                ImDrawList_AddCircleFilled(dl, (ImVec2){ a.x + gap * (d + 0.5f), (a.y + b.y) * 0.5f }, r * s, col4(w->color, al * (0.4f + 0.6f * s)), 12);
            }
            break;
        }
        case UIW_CIRCLE_THROBBER: { // dots on a circle, the bright one turning
            int n = 8;
            float cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.5f, R = fminf(b.x - a.x, b.y - a.y) * 0.38f, head = (float)fmod(st->now * 1.2, 1.0) * n;
            for (int d = 0; d < n; d++) {
                float ang = (float)d / n * 6.2831853f, f = fmodf(head - d + n, (float)n) / n;
                ImDrawList_AddCircleFilled(dl, (ImVec2){ cx + cosf(ang) * R, cy + sinf(ang) * R }, R * 0.16f, col4(w->color, al * (1.0f - f * 0.85f)), 10);
            }
            break;
        }
        case UIW_INVENTORY: inv_widget_draw(dl, w, a.x, a.y, b.x, b.y, k, al, st->designer); break; // (inventory_widget.c)
        case UIW_ABILITY_SLOT: case UIW_EFFECT_LIST: abil_widget_draw(dl, w, a.x, a.y, b.x, b.y, k, al, st->designer); break; // (ability_widget.c)
        case UIW_KEY_SELECTOR: { // the key, or "Press a key..." while it waits
            bool waiting = (int)i == st->capture;
            float bg[4];
            tint(w->bg, hot && w->enabled ? 1.3f : 1.0f, hot ? 0.04f : 0.0f, bg);
            box(dl, a, b, bg, w, k, al, waiting || hot ? accent : NULL);
            const float gray[4] = { 0.55f, 0.55f, 0.6f, 1.0f };
            const char* s = waiting ? (st->designer ? "Press a key..." : gus_tr("Press a key...")) : w->text[0] ? (st->designer ? w->text : gus_tr(w->text))
                                                                                                          : (st->designer ? w->hint : gus_tr(w->hint));
            text_in(dl, font, w, s, (UiRect){ a.x + 8.0f * k, a.y, b.x - 8.0f * k, b.y }, k, al * (waiting && fmod(st->now, 1.0) > 0.5 ? 0.55f : 1.0f),
                    w->text[0] || waiting ? w->color : gray, 0.5f, 0.5f);
            break;
        }
        case UIW_JOYSTICK: case UIW_TOUCH_BUTTON: case UIW_TOUCH_AREA: // (the game draws them over the rest, at the fingers)
            if (st->designer) draw_touch(dl, NULL, t, L, (int)i, k, al, true);
            break;
        default:
            if (hud_is_kind(w->kind)) hud_widget_draw(dl, w, st->owner, (int)i, a.x, a.y, b.x, b.y, k, al, st->designer, st->now); // (hud_widget.c)
            break;
        }
        if (st->designer && (ui_is_container(w->kind) || w->kind == UIW_SPACER) && w->kind != UIW_BUTTON)
            ImDrawList_AddRect(dl, a, b, THA(TK_N15, 64), 0.0f, 1.0f, 0);
        if ((int)i == st->nav) { // the keyboard's / gamepad's widget: an outline around it
            float g2 = 3.0f * k;
            ImDrawList_AddRect(dl, (ImVec2){ a.x - g2, a.y - g2 }, (ImVec2){ b.x + g2, b.y + g2 }, col4(accent, al), w->radius * k + g2, fmaxf(2.0f * k, 1.5f), 0);
        }
        ImDrawList_PopClipRect(dl);
    }
    // an open Combo Box's list, over everything
    if (st->open_combo >= 0 && st->open_combo < (int)t->n) {
        const UiWidget* w = &t->w[st->open_combo];
        UiRect r = L->r[st->open_combo];
        int n = combo_count(w->text);
        float rh = (r.y1 - r.y0);
        ImVec2 a = { r.x0, r.y1 + 2.0f * k }, b = { r.x1, r.y1 + 2.0f * k + rh * n };
        float bg[4];
        tint(w->bg, 1.1f, 0.02f, bg);
        bg[3] = 0.98f;
        ImDrawList_AddRectFilled(dl, a, b, col4(bg, 1.0f), w->radius * k, 0);
        for (int o = 0; o < n; o++) {
            char opt[128];
            combo_label(w->text, o, opt, sizeof opt);
            UiRect orr = { a.x, a.y + rh * o, b.x, a.y + rh * (o + 1) };
            if (o == (int)w->value) ImDrawList_AddRectFilled(dl, (ImVec2){ orr.x0 + 2, orr.y0 + 1 }, (ImVec2){ orr.x1 - 2, orr.y1 - 1 }, col4(accent, 0.35f), w->radius * k, 0);
            text_in(dl, font, w, opt, (UiRect){ orr.x0 + 10.0f * k, orr.y0, orr.x1, orr.y1 }, k, 1.0f, w->color, 0.0f, 0.5f);
        }
    }
}

// The topmost widget under a point that takes the mouse (interactive: the ones with input, a Scroll Box for the wheel)
static bool interactive(uint8_t kind) {
    return kind == UIW_BUTTON || kind == UIW_SLIDER || kind == UIW_CHECKBOX || kind == UIW_EDIT || kind == UIW_COMBO || kind == UIW_SPIN ||
           kind == UIW_INVENTORY || kind == UIW_KEY_SELECTOR || kind == UIW_ABILITY_SLOT || // (its slots take the mouse: inventory_widget.c, ability_widget.c)
           kind == UIW_LIST_VIEW || kind == UIW_TILE_VIEW; // (B7: an item clicked)
}
static int pick(const UiTree* t, const UiLayout* L, float x, float y, int mode) { // mode 0: any (the designer), 1: interactive, 2: Scroll Box
    for (int i = (int)t->n - 1; i >= 0; i--) {
        if (!L->drawn[i] || (mode == 0 && i == 0)) continue;
        if (mode == 1 && (!L->hit[i] || !interactive(t->w[i].kind))) continue;
        if (mode == 2 && (!L->hit[i] && !(L->drawn[i] && t->w[i].kind == UIW_SCROLL))) continue;
        if (mode == 2 && t->w[i].kind != UIW_SCROLL) continue;
        UiRect c = rect_isect(L->r[i], L->clip[i]);
        if (x >= c.x0 && x <= c.x1 && y >= c.y0 && y <= c.y1) return i;
    }
    return -1;
}

// The events of a frame, sent after the drawing (an event may change the widgets)
typedef struct UiEv { UiLive* l; int16_t w; const char* name; } UiEv;
static UiEv g_ev[32];
static int g_nev;
static void post(UiLive* l, int w, const char* name) {
    if (g_nev < 32) g_ev[g_nev++] = (UiEv){ l, (int16_t)w, name };
}

static void utf8_put(char* s, size_t cap, unsigned c) {
    size_t n = strlen(s);
    char b[4];
    int m = c < 0x80 ? (b[0] = (char)c, 1) : c < 0x800 ? (b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 63)), 2)
                                                       : (b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 63)), b[2] = (char)(0x80 | (c & 63)), 3);
    if (n + (size_t)m + 1 > cap) return;
    memcpy(s + n, b, (size_t)m), s[n + (size_t)m] = 0;
}
static void utf8_pop(char* s) {
    size_t n = strlen(s);
    while (n > 0 && ((unsigned char)s[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0) n--;
    s[n] = 0;
}

static float snap_value(const UiWidget* w, float v) {
    if (w->vstep > 0.0f) v = w->vmin + roundf((v - w->vmin) / w->vstep) * w->vstep;
    if (w->vmax > w->vmin) v = fminf(fmaxf(v, w->vmin), w->vmax);
    return v;
}


// --- Touch controls (notas-dev/android.md): fingers taken by the joystick / button / look area they land on, held
// until they lift; several at once (a touch screen's points: platform_touch_points), else the mouse as one finger ---
typedef struct TouchHold { int id; UiLive* l; int16_t w; float x0, y0, x, y, px, py; bool used; } TouchHold;
static TouchHold g_hold[10];
static float g_touch_move[2], g_touch_look[2];

static int key_of_name(const char* s) { // ("Space", "E", "Shift", "Left Mouse Button"... -> a GLFW key code; -1: none)
    if (!s || !s[0]) return -1;
    if (!s[1] && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) return s[0];
    if (!s[1] && s[0] >= 'a' && s[0] <= 'z') return s[0] - 32;
    static const struct { const char* n; int k; } t[] = { { "Space", 32 }, { "Enter", 257 }, { "Escape", 256 }, { "Tab", 258 }, { "Backspace", 259 },
                                                        { "Shift", 340 }, { "Left Shift", 340 }, { "Ctrl", 341 }, { "Left Ctrl", 341 }, { "Alt", 342 },
                                                        { "Up", 265 }, { "Down", 264 }, { "Left", 263 }, { "Right", 262 },
                                                        { "Left Mouse Button", 500 }, { "Right Mouse Button", 501 } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!_stricmp(s, t[i].n)) return t[i].k;
    return -1;
}

// This frame's fingers: id, x, y (screen pixels)
static int g_test_n = -1, g_test_id[10]; // (ui_touch_selftest's fingers)
static float g_test_xy[10][2];
static int touch_points(int* ids, float (*xy)[2], int cap, const float mouse[2], bool down) {
    if (g_test_n >= 0) {
        int n = g_test_n < cap ? g_test_n : cap;
        for (int i = 0; i < n; i++) ids[i] = g_test_id[i], xy[i][0] = g_test_xy[i][0], xy[i][1] = g_test_xy[i][1];
        return n;
    }
    int n = platform_touch_points(ids, xy, cap);
    if (n > 0 || !mouse || !down) return n;
    ids[0] = 0, xy[0][0] = mouse[0], xy[0][1] = mouse[1]; // (the PC: the mouse is one finger)
    return 1;
}

static void touch_widgets(World* w, UiLive* l, const UiTree* t, const UiLayout* L, float k, const float mouse[2], bool down) {
    int ids[10];
    float xy[10][2];
    int n = touch_points(ids, xy, 10, mouse, down);
    // new fingers: the touch widget under each (topmost first)
    for (int p = 0; p < n; p++) {
        bool held = false;
        for (int h = 0; h < 10; h++) held |= g_hold[h].used && g_hold[h].id == ids[p];
        if (held) continue;
        for (int i = (int)t->n - 1; i > 0; i--) {
            const UiWidget* tw = &t->w[i];
            if ((tw->kind != UIW_JOYSTICK && tw->kind != UIW_TOUCH_BUTTON && tw->kind != UIW_TOUCH_AREA) || !L->drawn[i] || !tw->enabled) continue;
            UiRect r = L->r[i];
            float x = xy[p][0], y = xy[p][1];
            bool in = x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
            if (tw->kind == UIW_TOUCH_BUTTON) { // (a round button: its circle)
                float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f, rr = fminf(r.x1 - r.x0, r.y1 - r.y0) * 0.5f;
                in = (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rr * rr * 1.2f;
            }
            if (!in) continue;
            for (int h = 0; h < 10; h++)
                if (!g_hold[h].used) {
                    g_hold[h] = (TouchHold){ ids[p], l, (int16_t)i, x, y, x, y, x, y, true };
                    if (tw->kind == UIW_TOUCH_BUTTON) {
                        int key = key_of_name(tw->text);
                        if (key >= 0) bp_key_event(w, key, true);
                        post(l, i, "OnPressed");
#ifdef __ANDROID__
                        re_vibrate_ms(12);
#endif
                    }
                    break;
                }
            break;
        }
    }
    // held fingers: moved, or lifted (a button lets its key go)
    for (int h = 0; h < 10; h++) {
        TouchHold* th = &g_hold[h];
        if (!th->used || th->l != l) continue;
        int p = -1;
        for (int q = 0; q < n && p < 0; q++)
            if (ids[q] == th->id) p = q;
        const UiWidget* tw = th->w < (int)t->n ? &t->w[th->w] : NULL;
        if (p < 0 || !tw) {
            if (tw && tw->kind == UIW_TOUCH_BUTTON) {
                int key = key_of_name(tw->text);
                if (key >= 0) bp_key_event(w, key, false);
                post(l, th->w, "OnReleased");
            }
            th->used = false;
            continue;
        }
        th->px = th->x, th->py = th->y, th->x = xy[p][0], th->y = xy[p][1];
        if (tw->kind == UIW_JOYSTICK) {
            UiRect r = L->r[th->w];
            float rad = fminf(r.x1 - r.x0, r.y1 - r.y0) * 0.5f;
            float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f;
            float dx = (th->x - cx) / fmaxf(rad, 1.0f), dy = (th->y - cy) / fmaxf(rad, 1.0f);
            float len = sqrtf(dx * dx + dy * dy);
            if (len > 1.0f) dx /= len, dy /= len;
            if (len < 0.08f) dx = dy = 0.0f; // (a dead zone)
            if (!_stricmp(tw->text, "Look")) g_touch_look[0] += dx * 6.0f, g_touch_look[1] -= dy * 6.0f;
            else g_touch_move[0] += -dy, g_touch_move[1] += dx; // (up: forward; right: right)
        } else if (tw->kind == UIW_TOUCH_AREA) {
            float deg = tw->value > 0.0f ? tw->value : 0.25f;
            g_touch_look[0] += (th->x - th->px) * deg / fmaxf(k, 0.1f) * 0.6f;
            g_touch_look[1] -= (th->y - th->py) * deg / fmaxf(k, 0.1f) * 0.6f;
        }
    }
}

// A joystick, a touch button, a look area (the designer outlines it)
static void draw_touch(ImDrawList* dl, const struct UiLive* l, const UiTree* t, const UiLayout* L, int i, float k, float al, bool designer) {
    const UiWidget* w = &t->w[i];
    UiRect r = L->r[i];
    float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f, rad = fminf(r.x1 - r.x0, r.y1 - r.y0) * 0.5f;
    const TouchHold* held = NULL;
    for (int h = 0; h < 10; h++)
        if (g_hold[h].used && g_hold[h].l == l && g_hold[h].w == i) held = &g_hold[h];
    if (w->kind == UIW_TOUCH_AREA) {
        if (designer) ImDrawList_AddRect(dl, (ImVec2){ r.x0, r.y0 }, (ImVec2){ r.x1, r.y1 }, col4((float[4]){ 1, 1, 1, 0.35f }, al), 0, 1.5f, 0);
        return;
    }
    ImDrawList_AddCircleFilled(dl, (ImVec2){ cx, cy }, rad, col4(held && w->kind == UIW_TOUCH_BUTTON && w->press[3] > 0.0f ? w->press : w->bg, al), 48);
    if (w->border_w > 0.0f) ImDrawList_AddCircle(dl, (ImVec2){ cx, cy }, rad, col4(w->border_col, al), 48, w->border_w * k * 2.0f);
    if (w->kind == UIW_JOYSTICK) { // the knob, at the finger (within the base)
        float kx = cx, ky = cy;
        if (held) {
            float dx = held->x - cx, dy = held->y - cy, len = sqrtf(dx * dx + dy * dy), lim = rad * 0.6f;
            if (len > lim) dx *= lim / len, dy *= lim / len;
            kx += dx, ky += dy;
        }
        ImDrawList_AddCircleFilled(dl, (ImVec2){ kx, ky }, rad * 0.42f, col4(w->color, al * (held ? 0.95f : 0.7f)), 40);
    } else if (w->hint[0]) { // the label
        float size = w->font_size * k;
        ImVec2 ts = ImFont_CalcTextSizeA(igGetFont(), size, 1e9f, 0.0f, w->hint, NULL, NULL);
        ImDrawList_AddText_FontPtr(dl, igGetFont(), size, (ImVec2){ cx - ts.x * 0.5f, cy - ts.y * 0.5f }, col4(w->color, al), w->hint, NULL, 0.0f, NULL);
    }
}

// The touch controls on a 1920x1080 screen with fingers made up: the joystick's directions, two fingers at once (the
// stick and a button: its key held, let go when the finger lifts), a drag in the look area; their cost (--touch-test)
int ui_touch_selftest(void) {
    int fails = 0;
#define TCHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: touch: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
    UiLive* l = calloc(1, sizeof *l);
    UiLayout* L = calloc(1, sizeof *L);
    if (!l || !L) return free(l), free(L), 1;
    ui_tree_default(&l->t);
    UiTree* t = &l->t;
    int area = ui_tree_add(t, UIW_TOUCH_AREA, 0), joy = ui_tree_add(t, UIW_JOYSTICK, 0), btn = ui_tree_add(t, UIW_TOUCH_BUTTON, 0);
    TCHECK(area > 0 && joy > 0 && btn > 0, "the widgets could not be added");
    if (fails) return free(l), free(L), fails;
    UiRect scr = { 0, 0, 1920, 1080 };
    layout(t, 0, scr, scr, 1.0f, true, 1.0f, L);
    UiRect rj = L->r[joy], rb = L->r[btn], ra = L->r[area];
    float jx = (rj.x0 + rj.x1) * 0.5f, jy = (rj.y0 + rj.y1) * 0.5f, jr = (rj.x1 - rj.x0) * 0.5f;
    TCHECK(rj.x0 < 400 && rj.y1 > 900, "the joystick is not at the bottom left (%.0f %.0f)", rj.x0, rj.y1);
    TCHECK(rb.x1 > 1700 && rb.y1 > 900, "the button is not at the bottom right (%.0f %.0f)", rb.x1, rb.y1);
    TCHECK(ra.x0 >= 900 && ra.x1 >= 1900, "the look area is not the right half (%.0f..%.0f)", ra.x0, ra.x1);
    memset(g_hold, 0, sizeof g_hold);
    float m[2], lk[2];
    bp_key_event(NULL, 32, false);
    // 1: a finger at the stick's right edge: right
    g_test_n = 1, g_test_id[0] = 1, g_test_xy[0][0] = jx + jr, g_test_xy[0][1] = jy;
    touch_widgets(NULL, l, t, L, 1.0f, NULL, false);
    ui_touch_axes(m, lk);
    TCHECK(m[1] > 0.9f && fabsf(m[0]) < 0.1f, "the stick's right gave %.2f %.2f", m[0], m[1]);
    // 2: the same finger up (forward), another on the button: Space held
    g_test_n = 2, g_test_xy[0][0] = jx, g_test_xy[0][1] = jy - jr * 2.0f; // (past the edge: kept to 1)
    g_test_id[1] = 2, g_test_xy[1][0] = (rb.x0 + rb.x1) * 0.5f, g_test_xy[1][1] = (rb.y0 + rb.y1) * 0.5f;
    touch_widgets(NULL, l, t, L, 1.0f, NULL, false);
    ui_touch_axes(m, lk);
    TCHECK(m[0] > 0.9f && fabsf(m[1]) < 0.1f, "the stick's up gave %.2f %.2f", m[0], m[1]);
    TCHECK(bp_key_held(32), "the button did not hold Space");
    // 3: the stick's finger lifts (no movement), a third finger drags in the look area
    g_test_n = 2, g_test_id[0] = 2, g_test_xy[0][0] = g_test_xy[1][0], g_test_xy[0][1] = g_test_xy[1][1];
    g_test_id[1] = 3, g_test_xy[1][0] = 1300, g_test_xy[1][1] = 400;
    touch_widgets(NULL, l, t, L, 1.0f, NULL, false);
    ui_touch_axes(m, lk);
    TCHECK(fabsf(m[0]) + fabsf(m[1]) < 0.01f, "the lifted stick still moves (%.2f %.2f)", m[0], m[1]);
    g_test_xy[1][0] = 1400, g_test_xy[1][1] = 360; // (right and up: the camera turns right and up)
    touch_widgets(NULL, l, t, L, 1.0f, NULL, false);
    ui_touch_axes(m, lk);
    TCHECK(lk[0] > 1.0f && lk[1] > 0.1f, "the look drag gave %.2f %.2f", lk[0], lk[1]);
    TCHECK(bp_key_held(32), "Space was let go with the finger still down");
    // 4: every finger lifts: Space let go
    g_test_n = 0;
    touch_widgets(NULL, l, t, L, 1.0f, NULL, false);
    ui_touch_axes(m, lk);
    TCHECK(!bp_key_held(32), "Space still held after the finger lifted");
    // the cost: ten fingers on the widgets, a frame
    g_test_n = 10;
    for (int i = 0; i < 10; i++) g_test_id[i] = 10 + i, g_test_xy[i][0] = (i & 1) ? 1200.0f + i * 40.0f : jx, g_test_xy[i][1] = 300.0f + i * 50.0f;
    double t0 = re_time_seconds();
    for (int f = 0; f < 1000; f++) {
        g_test_xy[0][0] = jx + (float)(f % 50);
        touch_widgets(NULL, l, t, L, 1.0f, NULL, false);
        ui_touch_axes(m, lk);
    }
    double us = (re_time_seconds() - t0) * 1e6 / 1000.0;
    g_test_n = 0;
    touch_widgets(NULL, l, t, L, 1.0f, NULL, false);
    g_test_n = -1;
    bp_key_event(NULL, 32, false);
    printf("[RealEngine] touch: 10 fingers %.2f us/frame\n", us);
    TCHECK(us < 50.0, "10 fingers cost %.1f us a frame", us);
    free(l), free(L);
    if (!fails) printf("[RealEngine] touch self-test: ok\n");
#undef TCHECK
    return fails;
}

bool ui_touch_held(int id) { // (bp_touch_update: a finger a touch widget took)
    for (int h = 0; h < 10; h++)
        if (g_hold[h].used && g_hold[h].id == id) return true;
    return false;
}

void ui_touch_axes(float move[2], float look[2]) { // (the frame's, taken: the next frame starts at 0)
    move[0] = fminf(fmaxf(g_touch_move[0], -1.0f), 1.0f), move[1] = fminf(fmaxf(g_touch_move[1], -1.0f), 1.0f);
    look[0] = g_touch_look[0], look[1] = g_touch_look[1];
    g_touch_move[0] = g_touch_move[1] = g_touch_look[0] = g_touch_look[1] = 0.0f;
}

// --- The keyboard and the gamepad (notas-dev/pausa-opcoes.md): while the cursor is out (a menu), the front widget that has
// controls takes the arrows / the D-pad / the left stick (moving to the nearest control that way; on a Slider, a Spin Box
// or a Combo Box left / right change it), Enter / Space / A (a click, a toggle, the next choice, a Key Selector's wait).
// A Key Selector waiting takes the next key, mouse button or gamepad button (Esc and Start cancel) ---
static GLFWgamepadstate g_gp, g_gp_prev;
static bool g_have_gp, g_nav_shown;
static int g_test_dir = -1;
static char g_test_key[48];
static uint32_t g_frame, g_capture_frame;
void ui_test_nav(int dir) { g_test_dir = dir; }
void ui_test_key(const char* key) { snprintf(g_test_key, sizeof g_test_key, "%s", key ? key : ""); }

static bool gp_pressed(int b) { return g_have_gp && g_gp.buttons[b] && !g_gp_prev.buttons[b]; }

typedef struct NavIn { int dir; bool accept; } NavIn; // dir -1 none, 0 up, 1 down, 2 left, 3 right
static NavIn nav_input(double now) {
    NavIn in = { -1, false };
    static const ImGuiKey arrows[4] = { ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow };
    for (int d = 0; d < 4; d++)
        if (igIsKeyPressed_Bool(arrows[d], true)) in.dir = d;
    in.accept = igIsKeyPressed_Bool(ImGuiKey_Enter, false) || igIsKeyPressed_Bool(ImGuiKey_KeypadEnter, false) || igIsKeyPressed_Bool(ImGuiKey_Space, false);
    if (g_have_gp) { // the D-pad or the left stick: once, then repeating while held
        static int held = -1;
        static double again;
        int d = g_gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] ? 0 : g_gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] ? 1 : g_gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] ? 2
              : g_gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] ? 3 : -1;
        if (d < 0) {
            float x = g_gp.axes[GLFW_GAMEPAD_AXIS_LEFT_X], y = g_gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
            if (fabsf(y) > 0.5f && fabsf(y) >= fabsf(x)) d = y < 0.0f ? 0 : 1;
            else if (fabsf(x) > 0.5f) d = x < 0.0f ? 2 : 3;
        }
        if (d != held) {
            held = d;
            if (d >= 0) in.dir = d, again = now + 0.4;
        } else if (d >= 0 && now >= again) in.dir = d, again = now + 0.12;
        in.accept |= gp_pressed(GLFW_GAMEPAD_BUTTON_A);
    }
    if (g_test_dir >= 0) {
        if (g_test_dir < 4) in.dir = g_test_dir;
        else if (g_test_dir == 4) in.accept = true;
        g_test_dir = -1;
    }
    return in;
}

// The key pressed this frame for a waiting Key Selector, by its Enhanced Input name (NULL: none); cancel: Esc / Start
static const char* captured_key(bool* cancel) {
    static char out[48];
    *cancel = igIsKeyPressed_Bool(ImGuiKey_Escape, false) || gp_pressed(GLFW_GAMEPAD_BUTTON_START);
    if (g_test_key[0]) return snprintf(out, sizeof out, "%s", g_test_key), g_test_key[0] = 0, out;
    for (int c = 0; c < 26; c++)
        if (igIsKeyPressed_Bool((ImGuiKey)(ImGuiKey_A + c), false)) return out[0] = (char)('A' + c), out[1] = 0, out;
    for (int c = 0; c < 10; c++)
        if (igIsKeyPressed_Bool((ImGuiKey)(ImGuiKey_0 + c), false)) return out[0] = (char)('0' + c), out[1] = 0, out;
    for (int c = 0; c < 12; c++)
        if (igIsKeyPressed_Bool((ImGuiKey)(ImGuiKey_F1 + c), false)) return snprintf(out, sizeof out, "F%d", c + 1), out;
    static const struct { ImGuiKey k; const char* n; } keys[] = {
        { ImGuiKey_Space, "Space" }, { ImGuiKey_Enter, "Enter" }, { ImGuiKey_Tab, "Tab" }, { ImGuiKey_Backspace, "BackSpace" },
        { ImGuiKey_LeftShift, "Left Shift" }, { ImGuiKey_LeftCtrl, "Left Ctrl" }, { ImGuiKey_LeftAlt, "Left Alt" }, { ImGuiKey_UpArrow, "Up" },
        { ImGuiKey_DownArrow, "Down" }, { ImGuiKey_LeftArrow, "Left" }, { ImGuiKey_RightArrow, "Right" } };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (igIsKeyPressed_Bool(keys[i].k, false)) return keys[i].n;
    if (igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) return "Left Mouse Button";
    if (igIsMouseClicked_Bool(ImGuiMouseButton_Right, false)) return "Right Mouse Button";
    if (igIsMouseClicked_Bool(ImGuiMouseButton_Middle, false)) return "Middle Mouse Button";
    static const char* const gpn[GLFW_GAMEPAD_BUTTON_LAST + 1] = {
        "Gamepad Face Button Bottom", "Gamepad Face Button Right", "Gamepad Face Button Left", "Gamepad Face Button Top", "Gamepad Left Shoulder",
        "Gamepad Right Shoulder", "Gamepad Special Left", NULL, NULL, "Gamepad Left Thumbstick Button", "Gamepad Right Thumbstick Button",
        "Gamepad D-pad Up", "Gamepad D-pad Right", "Gamepad D-pad Down", "Gamepad D-pad Left" };
    for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; b++)
        if (gpn[b] && gp_pressed(b)) return gpn[b];
    return NULL;
}

static bool focusable(const UiTree* t, const UiLayout* L, int i) { return i > 0 && i < (int)t->n && L->hit[i] && interactive(t->w[i].kind); }
static int first_focusable(const UiTree* t, const UiLayout* L) {
    for (int i = 1; i < (int)t->n; i++)
        if (focusable(t, L, i)) return i;
    return -1;
}
// The nearest control from cur that way (its center ahead, the sideways distance counting twice)
static int nav_move(const UiTree* t, const UiLayout* L, int cur, int dir) {
    if (!focusable(t, L, cur)) return first_focusable(t, L);
    float cx = (L->r[cur].x0 + L->r[cur].x1) * 0.5f, cy = (L->r[cur].y0 + L->r[cur].y1) * 0.5f, best_s = 1e30f;
    int best = -1;
    for (int i = 1; i < (int)t->n; i++) {
        if (i == cur || !focusable(t, L, i)) continue;
        float dx = (L->r[i].x0 + L->r[i].x1) * 0.5f - cx, dy = (L->r[i].y0 + L->r[i].y1) * 0.5f - cy;
        float along = dir == 0 ? -dy : dir == 1 ? dy : dir == 2 ? -dx : dx, side = dir < 2 ? fabsf(dx) : fabsf(dy);
        if (along <= 1.0f) continue;
        float s = along + side * 2.0f;
        if (s < best_s) best_s = s, best = i;
    }
    return best >= 0 ? best : cur;
}
// A Scroll Box above it scrolled so that it shows
static bool scroll_into_view(UiTree* t, const UiLayout* L, int i, float k) {
    for (int p = t->w[i].parent; p > 0; p = t->w[p].parent) {
        if (t->w[p].kind != UIW_SCROLL) continue;
        UiWidget* sw = &t->w[p];
        float pad = sw->padding * k, s0 = sw->scroll;
        if (L->r[i].y1 > L->r[p].y1 - pad) sw->scroll += (L->r[i].y1 - L->r[p].y1 + pad) / k;
        else if (L->r[i].y0 < L->r[p].y0 + pad) sw->scroll -= (L->r[p].y0 + pad - L->r[i].y0) / k;
        if (sw->scroll < 0.0f) sw->scroll = 0.0f;
        return sw->scroll != s0;
    }
    return false;
}
// The front tree under the mouse takes it (a hit-testable widget there: a menu's dimmed backdrop keeps the widgets
// behind from the clicks, as Unreal's do)
static bool blocks_mouse(const UiTree* t, const UiLayout* L, const float m[2]) {
    for (int i = (int)t->n - 1; i > 0; i--) {
        uint8_t kd = t->w[i].kind;
        if (!L->hit[i] || kd == UIW_JOYSTICK || kd == UIW_TOUCH_BUTTON || kd == UIW_TOUCH_AREA || kd == UIW_SPACER || ui_is_container(kd) && kd != UIW_BORDER && kd != UIW_BUTTON)
            continue;
        UiRect c = rect_isect(L->r[i], L->clip[i]);
        if (m[0] >= c.x0 && m[0] <= c.x1 && m[1] >= c.y0 && m[1] <= c.y1) return true;
    }
    return false;
}

// A live widget's rectangle and scale: the screen's, or the HUD Layout's (its safe area, its scale: hud.h)
static UiRect live_rect(const UiLive* l, float x0, float y0, float x1, float y1, float k, float* kl) {
    float r[4] = { x0, y0, x1, y1 };
    *kl = k;
    if (hud_layout_rect(l->e, r, kl)) return (UiRect){ r[0], r[1], r[2], r[3] };
    return (UiRect){ x0, y0, x1, y1 };
}

void ui_draw_viewport(World* w, ImDrawList* dl, float x0, float y0, float x1, float y1, const float mouse[2], bool down, bool clicked) {
    double t0 = re_time_seconds();
    hud_frame_begin(); // (the HUD's pieces drawn last frame into its stats)
    static double last;
    float dt = last > 0.0 ? (float)(t0 - last) : 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    last = t0;
    UiLive* list[32];
    uint32_t n = ui_live_list(w, list, 32, dt);
    if (progress_hide_ui()) return; // (this frame is a save's picture: the game without its widgets)
    float k = (y1 - y0) / UI_REF_HEIGHT;
    static UiLayout L;
    ImGuiIO* io = igGetIO_Nil();
    g_nev = 0;
    // the Inventory Grids' drag and drop, right and double clicks (against the last frame's grids: inventory_widget.c)
    inv_widget_frame(w, mouse, down, clicked, mouse && igIsMouseClicked_Bool(ImGuiMouseButton_Right, false),
                     mouse && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left));
    abil_widget_frame(w, mouse, clicked); // (the Ability Slots the last frame drew: a click activates)
    bool consumed = false; // (the topmost tree under the mouse takes the click: back to front, so the last that wants it)
    static double hover_since;
    static int tip_w = -1;
    static UiLive* tip_l;
    g_frame++;
    g_gp_prev = g_gp;
    g_have_gp = glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &g_gp);
    if (!g_have_gp) memset(&g_gp, 0, sizeof g_gp);
    // which tree takes the mouse (the front one with a widget under it) and the keyboard / gamepad (the front one with a
    // control, while the cursor is out: a menu; the HUD's buttons don't take the game's keys)
    int owner = -1, navtree = -1;
    bool nav_on = ui_mouse_shown();
    for (int i = (int)n - 1; i >= 0 && (owner < 0 || (nav_on && navtree < 0)); i--) {
        float kl;
        UiRect lr = live_rect(list[i], x0, y0, x1, y1, k, &kl);
        layout(&list[i]->t, 0, lr, (UiRect){ x0, y0, x1, y1 }, 1.0f, true, kl, &L);
        if (owner < 0 && mouse && blocks_mouse(&list[i]->t, &L, mouse)) owner = i;
        if (nav_on && navtree < 0 && first_focusable(&list[i]->t, &L) >= 0) navtree = i;
    }
    NavIn nin = navtree >= 0 ? nav_input(t0) : (NavIn){ -1, false };
    if (nin.dir >= 0 || nin.accept) g_nav_shown = true;
    if (io->MouseDelta.x != 0.0f || io->MouseDelta.y != 0.0f || clicked) g_nav_shown = false;
    const float* all_mouse = mouse;
    bool all_clicked = clicked;
    for (uint32_t i = 0; i < n; i++) {
        UiLive* l = list[i];
        UiTree* t = &l->t;
        mouse = owner < 0 || owner == (int)i ? all_mouse : NULL; // (a tree behind the one under the mouse: no hover, no click)
        clicked = mouse ? all_clicked : false;
        float k_screen = k;
        UiRect lr = live_rect(l, x0, y0, x1, y1, k_screen, &k);
        layout(t, 0, lr, (UiRect){ x0, y0, x1, y1 }, 1.0f, true, k, &L);
        // a Key Selector waiting: the next key is its (the click that started it doesn't count)
        if (l->capture >= 0 && l->capture < (int)t->n && g_frame > g_capture_frame) {
            bool cancel = false;
            const char* key = captured_key(&cancel);
            if (cancel) l->capture = -1, l->bind_serial = UINT32_MAX;
            else if (key) {
                snprintf(t->w[l->capture].text, sizeof t->w[l->capture].text, "%s", key);
                post(l, l->capture, "OnKeySelected");
                l->capture = -1;
            }
            clicked = false, mouse = NULL, nin = (NavIn){ -1, false };
        }
        // the keyboard / gamepad on the front menu
        if ((int)i == navtree) {
            if (!focusable(t, &L, l->nav)) l->nav = (int16_t)first_focusable(t, &L);
            UiWidget* nw = l->nav >= 0 ? &t->w[l->nav] : NULL;
            bool side = nin.dir == 2 || nin.dir == 3;
            if (nw && side && (nw->kind == UIW_SLIDER || nw->kind == UIW_SPIN)) { // left / right: a step
                float st = nw->vstep > 0.0f ? nw->vstep : (nw->vmax > nw->vmin ? (nw->vmax - nw->vmin) / 20.0f : 0.05f);
                float v = snap_value(nw, nw->value + (nin.dir == 3 ? st : -st));
                if (v != nw->value) nw->value = v, post(l, l->nav, "OnValueChanged");
            } else if (nw && side && nw->kind == UIW_COMBO) { // the choice before / after
                int no = combo_count(nw->text), c = (int)nw->value + (nin.dir == 3 ? 1 : -1);
                if (no > 0) nw->value = (float)((c + no) % no), post(l, l->nav, "OnSelectionChanged");
            } else if (nin.dir >= 0) {
                int to = nav_move(t, &L, l->nav, nin.dir);
                if (to != l->nav) {
                    if (l->nav >= 0 && t->w[l->nav].kind == UIW_BUTTON) post(l, l->nav, "OnUnhovered");
                    l->nav = (int16_t)to;
                    if (to >= 0 && t->w[to].kind == UIW_BUTTON) post(l, to, "OnHovered");
                    if (to >= 0 && scroll_into_view(t, &L, to, k)) layout(t, 0, lr, (UiRect){ x0, y0, x1, y1 }, 1.0f, true, k, &L);
                }
            }
            nw = l->nav >= 0 ? &t->w[l->nav] : NULL;
            if (nw && nin.accept) {
                switch (nw->kind) {
                case UIW_BUTTON: post(l, l->nav, "OnPressed"), post(l, l->nav, "OnReleased"), post(l, l->nav, "OnClicked"); break;
                case UIW_CHECKBOX: nw->checked = !nw->checked, post(l, l->nav, "OnCheckStateChanged"); break;
                case UIW_COMBO: {
                    int no = combo_count(nw->text);
                    if (no > 0) nw->value = (float)(((int)nw->value + 1) % no), post(l, l->nav, "OnSelectionChanged");
                    break;
                }
                case UIW_KEY_SELECTOR: l->capture = l->nav, g_capture_frame = g_frame; break;
                case UIW_EDIT: l->focus = l->nav; break;
                default: break;
                }
            }
        }
        int hot = mouse ? pick(t, &L, mouse[0], mouse[1], 1) : -1;
        // a Combo Box open: its list takes the mouse
        if (l->open_combo >= 0 && l->open_combo < (int)t->n && mouse) {
            UiWidget* cw = &t->w[l->open_combo];
            UiRect r = L.r[l->open_combo];
            float rh = r.y1 - r.y0, ly = r.y1 + 2.0f * k;
            int no = combo_count(cw->text);
            if (clicked) {
                int o = mouse[0] >= r.x0 && mouse[0] <= r.x1 && mouse[1] >= ly ? (int)((mouse[1] - ly) / rh) : -1;
                if (o >= 0 && o < no) {
                    if ((int)cw->value != o) cw->value = (float)o, post(l, l->open_combo, "OnSelectionChanged");
                }
                l->open_combo = -1, clicked = false, consumed = true;
            }
            hot = -1;
        }
        // hover: a Button's events
        if (hot != l->hot) {
            if (l->hot >= 0 && l->hot < (int)t->n && t->w[l->hot].kind == UIW_BUTTON) post(l, l->hot, "OnUnhovered");
            if (hot >= 0 && t->w[hot].kind == UIW_BUTTON) post(l, hot, "OnHovered");
            l->hot = (int16_t)hot;
            hover_since = t0;
        }
        if (hot >= 0 && t->w[hot].tooltip[0]) tip_l = l, tip_w = hot;
        if (hot >= 0 && (int)i == navtree && !g_nav_shown) l->nav = (int16_t)hot; // (the keys go on from where the mouse is)
        if (clicked && !consumed) {
            if (hot >= 0) {
                l->pressed = (uint8_t)(hot + 1);
                UiWidget* hw = &t->w[hot];
                l->focus = hw->kind == UIW_EDIT ? (int16_t)hot : -1;
                if (hw->kind == UIW_BUTTON) post(l, hot, "OnPressed");
                if (hw->kind == UIW_COMBO) l->open_combo = (int16_t)hot;
                consumed = true;
            } else l->focus = -1;
        }
        // a held Slider follows the mouse; a held Spin Box changes by the drag
        if (down && l->pressed) {
            UiWidget* hw = &t->w[l->pressed - 1];
            UiRect r = L.r[l->pressed - 1];
            if (hw->kind == UIW_SLIDER && mouse) {
                float hr = (r.y1 - r.y0) * 0.32f, u = (mouse[0] - r.x0 - hr) / fmaxf(r.x1 - r.x0 - 2 * hr, 1.0f);
                u = fminf(fmaxf(u, 0.0f), 1.0f);
                float v = snap_value(hw, hw->vmin + u * (hw->vmax > hw->vmin ? hw->vmax - hw->vmin : 1.0f));
                if (v != hw->value) hw->value = v, post(l, l->pressed - 1, "OnValueChanged");
            } else if (hw->kind == UIW_SPIN && io->MouseDelta.x != 0.0f) {
                float step = hw->vstep > 0.0f ? hw->vstep : (hw->vmax > hw->vmin ? (hw->vmax - hw->vmin) / 200.0f : 0.01f);
                static float acc;
                acc += io->MouseDelta.x * 0.25f;
                float v = hw->value;
                while (acc >= 1.0f) v += step, acc -= 1.0f;
                while (acc <= -1.0f) v -= step, acc += 1.0f;
                v = snap_value(hw, v);
                if (v != hw->value) hw->value = v, post(l, l->pressed - 1, "OnValueChanged");
            }
        }
        if (!down && l->pressed) { // released: a click when still over the same widget
            int pw = l->pressed - 1;
            UiWidget* hw = &t->w[pw];
            if (hw->kind == UIW_BUTTON) {
                post(l, pw, "OnReleased");
                if (hot == pw) post(l, pw, "OnClicked");
            } else if (hw->kind == UIW_CHECKBOX && hot == pw)
                hw->checked = !hw->checked, post(l, pw, "OnCheckStateChanged");
            else if (hw->kind == UIW_KEY_SELECTOR && hot == pw)
                l->capture = (int16_t)pw, g_capture_frame = g_frame; // (waits for the next key)
            else if ((hw->kind == UIW_LIST_VIEW || hw->kind == UIW_TILE_VIEW) && hot == pw && mouse) { // (B7: the item under the mouse)
                int it = list_hit(hw, ui_list_items(l->e, pw, hw), L.r[pw], k, mouse[0], mouse[1]);
                if (it >= 0) {
                    if ((int)hw->value != it) hw->value = (float)it, post(l, pw, "OnSelectionChanged");
                    post(l, pw, "OnItemClicked");
                }
            }
            l->pressed = 0;
        }
        // typing into the focused Editable Text
        if (l->focus >= 0 && l->focus < (int)t->n) {
            UiWidget* ew = &t->w[l->focus];
            bool changed = false;
            for (int c = 0; c < io->InputQueueCharacters.Size; c++) {
                unsigned ch = io->InputQueueCharacters.Data[c];
                if (ch >= 32 && ch != 127) utf8_put(ew->text, sizeof ew->text, ch), changed = true;
            }
            if (igIsKeyPressed_Bool(ImGuiKey_Backspace, true) && ew->text[0]) utf8_pop(ew->text), changed = true;
            if (changed) post(l, l->focus, "OnTextChanged");
            if (igIsKeyPressed_Bool(ImGuiKey_Enter, false) || igIsKeyPressed_Bool(ImGuiKey_KeypadEnter, false)) post(l, l->focus, "OnTextCommitted"), l->focus = -1;
            else if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) l->focus = -1;
        }
        // the wheel scrolls the List / Tile View under the mouse (B7), else the Scroll Box
        if (mouse && io->MouseWheel != 0.0f && hot >= 0 && (t->w[hot].kind == UIW_LIST_VIEW || t->w[hot].kind == UIW_TILE_VIEW)) {
            UiWidget* lw = &t->w[hot];
            float maxs = fmaxf((list_content(lw, ui_list_items(l->e, hot, lw), L.r[hot], k) - (L.r[hot].y1 - L.r[hot].y0)) / k, 0.0f);
            lw->scroll = fminf(fmaxf(lw->scroll - io->MouseWheel * 60.0f, 0.0f), maxs);
            io->MouseWheel = 0.0f;
        }
        if (mouse && io->MouseWheel != 0.0f) {
            int sb = pick(t, &L, mouse[0], mouse[1], 2);
            if (sb >= 0) {
                UiWidget* sw = &t->w[sb];
                float maxs = fmaxf((L.content[sb] - (L.r[sb].y1 - L.r[sb].y0) + 2.0f * sw->padding * k) / k, 0.0f);
                sw->scroll = fminf(fmaxf(sw->scroll - io->MouseWheel * 60.0f, 0.0f), maxs);
                layout(t, 0, lr, (UiRect){ x0, y0, x1, y1 }, 1.0f, true, k, &L);
            }
        }
        touch_widgets(w, l, t, &L, k, all_mouse, down);
        UiState st = { hot, l->pressed ? l->pressed - 1 : -1, l->focus, l->open_combo, false, t0, g_nav_shown && (int)i == navtree ? l->nav : -1, l->capture, l->e };
        draw_tree(dl, t, &L, k, &st);
        for (uint32_t ti = 0; ti < t->n; ti++)
            if (L.drawn[ti] && (t->w[ti].kind == UIW_JOYSTICK || t->w[ti].kind == UIW_TOUCH_BUTTON || t->w[ti].kind == UIW_TOUCH_AREA))
                draw_touch(dl, l, t, &L, (int)ti, k, L.alpha[ti], false);
        k = k_screen;
    }
    inv_widget_overlay(dl); // (the dragged item, an item's tip)
    // a tool tip after a moment under the mouse
    mouse = all_mouse;
    if (tip_l && tip_w >= 0 && tip_w < (int)tip_l->t.n && tip_l->hot == tip_w && t0 - hover_since > 0.6 && mouse) {
        const char* s = gus_tr(tip_l->t.w[tip_w].tooltip);
        ImFont* font = igGetFont();
        float size = 18.0f * k;
        ImVec2 ts = ImFont_CalcTextSizeA(font, size, 1e9f, 400.0f * k, s, NULL, NULL);
        ImVec2 a = { mouse[0] + 14.0f * k, mouse[1] + 18.0f * k }, b = { a.x + ts.x + 16.0f * k, a.y + ts.y + 10.0f * k };
        ImDrawList_AddRectFilled(dl, a, b, THA(TK_N5, 240), 4.0f * k, 0);
        ImDrawList_AddRect(dl, a, b, THA(TK_N15, 64), 4.0f * k, 1.0f, 0);
        ImDrawList_AddText_FontPtr(dl, font, size, (ImVec2){ a.x + 8.0f * k, a.y + 5.0f * k }, TH(TK_N14), s, NULL, 400.0f * k, NULL);
    }
    if (!tip_l || tip_l->hot != tip_w) tip_l = NULL, tip_w = -1;
    for (int i = 0; i < g_nev; i++) ui_event(w, g_ev[i].l, g_ev[i].w, g_ev[i].name); // (after the drawing)
    ui_stats()->draw_ms = (re_time_seconds() - t0) * 1000.0;
}

// --- The Designer ---

static struct {
    const BpAsset* a;
    int sel;
    bool dragging, sizing;
    int res;            // the preview's resolution (g_res)
    char find[48];      // the Palette's search
    int anim;           // the Widget Animation edited (-1: none)
    float anim_t;       // the timeline's time
    bool anim_play;     // the preview plays it
    int key_tr, key_k;  // the key selected
    bool key_drag;
    int new_prop;       // the "+ Track" property
} D = { NULL, -1, false, false, 0, "", -1, 0.0f, false, -1, -1, false, 0 };

static const struct { const char* name; float w, h; } g_res[] = {
    { "1920 x 1080 (16:9)", 1920, 1080 }, { "1280 x 720 (16:9)", 1280, 720 }, { "2560 x 1440 (16:9)", 2560, 1440 },
    { "2560 x 1080 (21:9)", 2560, 1080 }, { "1080 x 1920 (9:16)", 1080, 1920 }, { "1024 x 768 (4:3)", 1024, 768 },
};

static void anchor_preset(UiWidget* w, int p) {
    static const float pre[9][4] = { { 0, 0, 0, 0 }, { 0.5f, 0, 0.5f, 0 }, { 1, 0, 1, 0 }, { 0, 0.5f, 0, 0.5f }, { 0.5f, 0.5f, 0.5f, 0.5f },
                                     { 1, 0.5f, 1, 0.5f }, { 0, 1, 0, 1 }, { 0.5f, 1, 0.5f, 1 }, { 1, 1, 1, 1 } };
    if (p < 9) { // a point: the pivot there too, the offset 0 (the widget sits on its anchor)
        memcpy(w->anchor, pre[p], 16);
        w->align[0] = pre[p][0], w->align[1] = pre[p][1];
        w->pos[0] = (pre[p][0] < 0.25f ? 40.0f : pre[p][0] > 0.75f ? -40.0f : 0.0f);
        w->pos[1] = (pre[p][1] < 0.25f ? 40.0f : pre[p][1] > 0.75f ? -40.0f : 0.0f);
    } else { // fill: margins
        w->anchor[0] = w->anchor[1] = 0.0f, w->anchor[2] = w->anchor[3] = 1.0f;
        w->pos[0] = w->pos[1] = w->size[0] = w->size[1] = 0.0f;
    }
}

static bool var_combo(const char* label, char* var, const BpAsset* a) {
    bool ch = false;
    if (igBeginCombo(label, var[0] ? var : TR("(none)"), 0)) {
        if (igSelectable_Bool(TR("(none)"), !var[0], 0, (ImVec2){ 0, 0 })) var[0] = 0, ch = true;
        for (uint32_t i = 0; i < a->nvars; i++)
            if (igSelectable_Bool(a->vars[i].name, !strcmp(var, a->vars[i].name), 0, (ImVec2){ 0, 0 })) snprintf(var, UI_NAME, "%s", a->vars[i].name), ch = true;
        igEndCombo();
    }
    return ch;
}

// "+ <Event>" for a widget's event that the Event Graph doesn't have yet (the Custom Event "<Event>_<name>")
static bool event_button(BpAsset* a, const UiWidget* w, const char* event) {
    char ev[UI_NAME + 32];
    snprintf(ev, sizeof ev, "%s_%s", event, w->name);
    bool have = false;
    for (uint32_t g = 0; g < a->ngraphs && !have; g++)
        for (uint32_t n = 0; n < a->graphs[g].nnodes && !have; n++) have = !strcmp(a->graphs[g].nodes[n].text, ev);
    igPushID_Str(event);
    bool ch = false;
    if (have) igTextDisabled("%s  \xe2\x9c\x93", event);
    else if (igButton(event, (ImVec2){ -1, 0 })) bp_add_custom_event(a, ev), ch = true;
    igPopID();
    return ch;
}

static bool align_combo(const char* label, uint8_t* v, bool horizontal) {
    static const char* const hn[UI_ALIGN_COUNT] = { "Fill", "Left", "Center", "Right" }, * const vn[UI_ALIGN_COUNT] = { "Fill", "Top", "Center", "Bottom" };
    const char* names[UI_ALIGN_COUNT];
    for (int i = 0; i < UI_ALIGN_COUNT; i++) names[i] = TR(horizontal ? hn[i] : vn[i]);
    int x = *v;
    bool ch = igCombo_Str_arr(label, &x, names, UI_ALIGN_COUNT, -1);
    *v = (uint8_t)x;
    return ch;
}

static bool section(const char* title) { return igCollapsingHeader_TreeNodeFlags(title, ImGuiTreeNodeFlags_DefaultOpen); }

// (B7) a text's font (a TTF / OTF dropped from the Content Browser, or typed), italic, outline
static bool font_details(UiWidget* w) {
    bool ch = igInputTextWithHint(PL(TR("Font")), TR("the engine's (drop a .ttf / .otf)"), w->font, sizeof w->font, 0, NULL, NULL);
    if (igBeginDragDropTarget()) {
        const ImGuiPayload* pl = igAcceptDragDropPayload(RE_PAYLOAD_ASSET, 0);
        if (pl && pl->Data) snprintf(w->font, sizeof w->font, "%s", ((const AssetPayload*)pl->Data)->vpath), ch = true;
        igEndDragDropTarget();
    }
    igSetItemTooltip("%s", TR("Bold and italic come from its family's files (Name-Bold.ttf, Name-Italic.ttf) when they are next to it."));
    bool it = w->italic;
    if (igCheckbox(TR("Italic"), &it)) w->italic = it, ch = true;
    ch |= igDragFloat(PL(TR("Outline")), &w->outline, 0.1f, 0.0f, 12.0f, "%.1f px", 0);
    if (w->outline > 0.0f) ch |= igColorEdit4(PL(TR("Outline Color")), w->outline_col, 0);
    return ch;
}

static bool widget_details(BpAsset* a, UiTree* t, int i) {
    UiWidget* w = &t->w[i];
    bool ch = false;
    igTextColored(THV(TK_INFO), "%s", TR(g_ui_kind_names[w->kind]));
    char name[UI_NAME];
    snprintf(name, sizeof name, "%s", w->name);
    if (igInputText(PL(TR("Name")), name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL) && name[0] && ui_tree_find(t, name) < 0)
        snprintf(w->name, UI_NAME, "%s", name), ch = true;
    // --- Slot (its place in its parent) ---
    if (i > 0 && section(TR("Slot"))) {
        if (igBeginCombo(PL(TR("Parent")), t->w[w->parent].name, 0)) {
            for (uint32_t p = 0; p < (uint32_t)i; p++)
                if (ui_is_container(t->w[p].kind) && igSelectable_Bool(t->w[p].name, (int)p == w->parent, 0, (ImVec2){ 0, 0 })) w->parent = (int16_t)p, ch = true;
            igEndCombo();
        }
        uint8_t pk = t->w[w->parent].kind;
        if (pk == UIW_CANVAS) {
            static const char* const pres[10] = { "Top Left", "Top Center", "Top Right", "Center Left", "Center", "Center Right",
                                                  "Bottom Left", "Bottom Center", "Bottom Right", "Fill" };
            if (igBeginCombo(PL(TR("Anchors")), TR("(choose)"), 0)) {
                for (int p = 0; p < 10; p++)
                    if (igSelectable_Bool(TR(pres[p]), false, 0, (ImVec2){ 0, 0 })) anchor_preset(w, p), ch = true;
                igEndCombo();
            }
            ch |= igDragFloat4(PL(TR("Anchor Min/Max")), w->anchor, 0.005f, 0.0f, 1.0f, "%.2f", 0);
            bool stretch_x = fabsf(w->anchor[2] - w->anchor[0]) > 1e-4f;
            ch |= igDragFloat2(PL(TR("Position")), w->pos, 1.0f, -4000.0f, 4000.0f, "%.0f", 0);
            ch |= igDragFloat2(stretch_x ? TR("Margins (right, bottom)") : TR("Size"), w->size, 1.0f, -4000.0f, 4000.0f, "%.0f", 0);
            ch |= igDragFloat2(PL(TR("Alignment")), w->align, 0.01f, 0.0f, 1.0f, "%.2f", 0);
        } else {
            if (pk == UIW_VBOX || pk == UIW_HBOX) {
                int rule = w->size_rule;
                const char* rules[2] = { TR("Auto"), TR("Fill") };
                if (igCombo_Str_arr(PL(TR("Size Rule")), &rule, rules, 2, -1)) w->size_rule = (uint8_t)rule, ch = true;
                if (w->size_rule == 1) ch |= igDragFloat(PL(TR("Fill Weight")), &w->fill, 0.05f, 0.0f, 100.0f, "%.2f", 0);
            }
            ch |= igDragFloat2(PL(TR("Size")), w->size, 1.0f, 1.0f, 4000.0f, "%.0f", 0);
            ch |= igDragFloat4(PL(TR("Padding (L T R B)")), w->slot_pad, 0.5f, -1000.0f, 1000.0f, "%.0f", 0);
            ch |= align_combo(PL(TR("Horizontal Alignment")), &w->h_align, true);
            ch |= align_combo(PL(TR("Vertical Alignment")), &w->v_align, false);
        }
        // its order among its siblings (the order boxes stack in)
        if (igButton(TR("Move Up"), (ImVec2){ 0, 0 })) {
            int old = i;
            if (ui_tree_move(t, i, -1)) {
                for (int q = 0; q < (int)t->n; q++) if (!strcmp(t->w[q].name, name)) D.sel = q;
                ch = true;
                (void)old;
                return ch;
            }
        }
        igSameLine(0, -1);
        if (igButton(TR("Move Down"), (ImVec2){ 0, 0 })) {
            if (ui_tree_move(t, i, +1)) {
                for (int q = 0; q < (int)t->n; q++) if (!strcmp(t->w[q].name, name)) D.sel = q;
                return true;
            }
        }
    }
    // --- Appearance / Content ---
    if (section(TR("Appearance"))) {
        switch (w->kind) {
        case UIW_TEXT:
            ch |= igInputTextMultiline(PL(TR("Text")), w->text, sizeof w->text, (ImVec2){ 0, igGetFontSize() * 3.0f }, 0, NULL, NULL);
            ch |= var_combo(TR("Bind Text"), w->bind_text, a);
            ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 6.0f, 200.0f, "%.0f", 0);
            ch |= igColorEdit4(PL(TR("Color and Opacity")), w->color, 0);
            ch |= igDragFloat2(PL(TR("Justification")), w->align, 0.01f, 0.0f, 1.0f, "%.2f", 0);
            {
                bool b = w->bold, s = w->shadow, wr = w->wrap;
                if (igCheckbox(TR("Bold"), &b)) w->bold = b, ch = true;
                igSameLine(0, -1);
                if (igCheckbox(TR("Shadow"), &s)) w->shadow = s, ch = true;
                igSameLine(0, -1);
                if (igCheckbox(TR("Auto Wrap Text"), &wr)) w->wrap = wr, ch = true;
            }
            ch |= font_details(w);
            break;
        case UIW_RICH_TEXT:
            ch |= igInputTextMultiline(PL(TR("Text")), w->text, sizeof w->text, (ImVec2){ 0, igGetFontSize() * 4.0f }, 0, NULL, NULL);
            igTextDisabled("%s", TR("Marks: <b>bold</b> <i>italic</i> <color=#FFB020>color</color> <size=32>size</size> <outline>outline</outline>"));
            ch |= var_combo(TR("Bind Text"), w->bind_text, a);
            ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 6.0f, 200.0f, "%.0f", 0);
            ch |= igColorEdit4(PL(TR("Color and Opacity")), w->color, 0);
            {
                bool s = w->shadow, wr = w->wrap;
                if (igCheckbox(TR("Shadow"), &s)) w->shadow = s, ch = true;
                igSameLine(0, -1);
                if (igCheckbox(TR("Auto Wrap Text"), &wr)) w->wrap = wr, ch = true;
            }
            ch |= font_details(w);
            break;
        case UIW_LIST_VIEW: case UIW_TILE_VIEW: {
            ch |= igInputTextMultiline(PL(TR("Items")), w->text, sizeof w->text, (ImVec2){ 0, igGetFontSize() * 3.0f }, 0, NULL, NULL);
            igTextDisabled("%s", TR("'|' between the items; Set Text (Widget) gives any number of them while playing (Join String Array)."));
            ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 6.0f, 200.0f, "%.0f", 0);
            ch |= igColorEdit4(PL(TR("Color and Opacity")), w->color, 0);
            ch |= igColorEdit4(PL(TR("Background")), w->bg, 0);
            ch |= igColorEdit4(PL(TR("Hovered")), w->hover, 0);
            ch |= igColorEdit4(PL(TR("Selected")), w->press, 0);
            if (w->kind == UIW_TILE_VIEW) {
                int c = w->columns > 0 ? w->columns : 4;
                if (igSliderInt(PL(TR("Columns")), &c, 1, 12, "%d", 0)) w->columns = (uint8_t)c, ch = true;
            }
            ch |= igDragFloat(PL(TR("Padding")), &w->padding, 0.25f, 0.0f, 64.0f, "%.0f", 0);
            ch |= font_details(w);
            ch |= event_button(a, w, "OnItemClicked");
            ch |= event_button(a, w, "OnSelectionChanged");
            break;
        }
        case UIW_IMAGE: {
            ch |= igInputText(PL(TR("Texture")), w->image, sizeof w->image, 0, NULL, NULL);
            if (igBeginDragDropTarget()) { // (dropped from the Content Browser)
                const ImGuiPayload* pl = igAcceptDragDropPayload(RE_PAYLOAD_ASSET, 0);
                if (pl && pl->Data) snprintf(w->image, sizeof w->image, "%s", ((const AssetPayload*)pl->Data)->vpath), ch = true;
                igEndDragDropTarget();
            }
            ch |= igColorEdit4(PL(TR("Tint")), w->color, 0);
            ch |= igDragFloat(PL(TR("Corner Radius")), &w->radius, 0.25f, 0.0f, 500.0f, "%.0f", 0);
            break;
        }
        case UIW_BUTTON:
        case UIW_CHECKBOX:
            ch |= igColorEdit4(PL(TR("Normal")), w->bg, 0);
            ch |= igColorEdit4(PL(TR(w->kind == UIW_CHECKBOX ? "Checked" : "Hovered")), w->kind == UIW_CHECKBOX ? w->color : w->hover, 0);
            if (w->kind == UIW_BUTTON) {
                ch |= igColorEdit4(PL(TR("Pressed")), w->press, 0);
                igTextDisabled("%s", TR("Hovered / Pressed with no opacity: lighter / darker Normal."));
                ch |= igDragFloat(PL(TR("Padding")), &w->padding, 0.5f, 0.0f, 200.0f, "%.0f", 0);
            } else {
                bool c = w->checked;
                if (igCheckbox(TR("Is Checked"), &c)) w->checked = c, ch = true;
            }
            break;
        case UIW_PROGRESS:
            ch |= igSliderFloat(PL(TR("Percent")), &w->percent, 0.0f, 1.0f, "%.2f", 0);
            ch |= var_combo(TR("Bind Percent"), w->bind_percent, a);
            ch |= igColorEdit4(PL(TR("Fill Color")), w->color, 0);
            ch |= igColorEdit4(PL(TR("Background Color")), w->bg, 0);
            break;
        case UIW_SLIDER:
        case UIW_SPIN:
            ch |= igDragFloat(PL(TR("Value")), &w->value, 0.01f, w->vmin, w->vmax > w->vmin ? w->vmax : w->vmin + 1.0f, "%.2f", 0);
            ch |= igDragFloat2(PL(TR("Min / Max")), &w->vmin, 0.1f, -1e6f, 1e6f, "%.2f", 0);
            ch |= igDragFloat(PL(TR("Step Size")), &w->vstep, 0.01f, 0.0f, 1e6f, "%.2f", 0);
            ch |= igColorEdit4(PL(TR(w->kind == UIW_SLIDER ? "Bar Fill Color" : "Text Color")), w->color, 0);
            ch |= igColorEdit4(PL(TR("Background Color")), w->bg, 0);
            if (w->kind == UIW_SPIN) ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 6.0f, 200.0f, "%.0f", 0);
            break;
        case UIW_EDIT:
            ch |= igInputText(PL(TR("Text")), w->text, sizeof w->text, 0, NULL, NULL);
            ch |= igInputText(PL(TR("Hint Text")), w->hint, sizeof w->hint, 0, NULL, NULL);
            ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 6.0f, 200.0f, "%.0f", 0);
            ch |= igColorEdit4(PL(TR("Text Color")), w->color, 0);
            ch |= igColorEdit4(PL(TR("Background Color")), w->bg, 0);
            break;
        case UIW_JOYSTICK: {
            int m = !_stricmp(w->text, "Look");
            const char* ms[2] = { TR("Move (walks)"), TR("Look (turns the camera)") };
            if (igCombo_Str_arr(PL(TR("Drives")), &m, ms, 2, -1)) snprintf(w->text, sizeof w->text, "%s", m ? "Look" : "Move"), ch = true;
            ch |= igColorEdit4(PL(TR("Base Color")), w->bg, 0);
            ch |= igColorEdit4(PL(TR("Knob Color")), w->color, 0);
            break;
        }
        case UIW_TOUCH_BUTTON:
            ch |= igInputText(PL(TR("Key (held while touched)")), w->text, sizeof w->text, 0, NULL, NULL);
            ch |= igInputText(PL(TR("Label")), w->hint, sizeof w->hint, 0, NULL, NULL);
            ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 4.0f, 200.0f, "%.0f", 0);
            ch |= igColorEdit4(PL(TR("Background Color")), w->bg, 0);
            ch |= igColorEdit4(PL(TR("Pressed Color")), w->press, 0);
            ch |= igColorEdit4(PL(TR("Text Color")), w->color, 0);
            break;
        case UIW_TOUCH_AREA:
            ch |= igDragFloat(PL(TR("Degrees per Pixel")), &w->value, 0.005f, 0.01f, 2.0f, "%.3f", 0);
            igTextDisabled("%s", TR("Invisible in the game: a drag inside it turns the camera."));
            break;
        case UIW_COMBO: {
            ch |= igInputText(PL(TR("Options (a | b | c)")), w->text, sizeof w->text, 0, NULL, NULL);
            int sel = (int)w->value;
            if (igInputInt(PL(TR("Selected Index")), &sel, 1, 1, 0)) w->value = (float)(sel < 0 ? 0 : sel), ch = true;
            ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 6.0f, 200.0f, "%.0f", 0);
            ch |= igColorEdit4(PL(TR("Text Color")), w->color, 0);
            ch |= igColorEdit4(PL(TR("Background Color")), w->bg, 0);
            break;
        }
        case UIW_GRID: {
            int c = w->columns;
            if (igInputInt(PL(TR("Columns")), &c, 1, 1, 0)) w->columns = (uint8_t)(c < 1 ? 1 : c > 32 ? 32 : c), ch = true;
            ch |= igColorEdit4(PL(TR("Background Color")), w->bg, 0);
            break;
        }
        case UIW_ABILITY_SLOT: case UIW_EFFECT_LIST: { // (ability_widget.c)
            ch |= igInputText(PL(TR("Actor (empty: the player)")), w->text, sizeof w->text, 0, NULL, NULL);
            if (w->kind == UIW_ABILITY_SLOT) {
                int sl = (int)w->value + 1;
                if (igInputInt(PL(TR("Ability Slot Number")), &sl, 1, 1, 0)) w->value = (float)(sl < 1 ? 0 : sl > 8 ? 7 : sl - 1), ch = true;
            } else
                ch |= igDragFloat(PL(TR("Spacing")), &w->padding, 0.5f, 0.0f, 100.0f, "%.0f", 0);
            ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 6.0f, 100.0f, "%.0f", 0);
            ch |= igColorEdit4(PL(TR("Background Color")), w->bg, 0);
            ch |= igColorEdit4(PL(TR("Text Color")), w->color, 0);
            if (w->kind == UIW_ABILITY_SLOT) ch |= igColorEdit4(PL(TR("Border Color")), w->border_col, 0);
            igTextDisabled("%s", TR(w->kind == UIW_ABILITY_SLOT ? "The actor's ability in that slot (Give Ability order): icon, cooldown, charges, key. A click activates it."
                                                               : "The actor's active Gameplay Effects: buffs green, debuffs red, stacks and the time left."));
            break;
        }
        case UIW_INVENTORY: { // (inventory_widget.c)
            ch |= igInputText(PL(TR("Actor (empty: the player)")), w->text, sizeof w->text, 0, NULL, NULL);
            int m = w->orient == 1;
            const char* ms[2] = { TR("Slots"), TR("Equipment") };
            if (igCombo_Str_arr(PL(TR("Shows")), &m, ms, 2, -1)) w->orient = (uint8_t)m, ch = true;
            int c = w->columns;
            if (igInputInt(PL(TR("Columns")), &c, 1, 1, 0)) w->columns = (uint8_t)(c < 1 ? 1 : c > 16 ? 16 : c), ch = true;
            bool sw = !w->checked;
            if (w->orient != 1 && igCheckbox(TR("Show Weight"), &sw)) w->checked = !sw, ch = true;
            ch |= igDragFloat(PL(TR("Spacing")), &w->padding, 0.5f, 0.0f, 100.0f, "%.0f", 0);
            ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 6.0f, 100.0f, "%.0f", 0);
            ch |= igColorEdit4(PL(TR("Slot Color")), w->bg, 0);
            ch |= igColorEdit4(PL(TR("Hovered Color")), w->hover, 0);
            ch |= igColorEdit4(PL(TR("Text Color")), w->color, 0);
            igTextDisabled("%s", TR("Drag: move or swap; onto the equipment: equip; out of the grids: drop. Right click: use. Double click: equip."));
            break;
        }
        case UIW_THROBBER:
        case UIW_CIRCLE_THROBBER: ch |= igColorEdit4(PL(TR("Color and Opacity")), w->color, 0); break;
        case UIW_SPACER: igTextDisabled("%s", TR("Empty room: its Size in a box.")); break;
        default: // the panels: a background, the spacing / padding
            if (hud_is_kind(w->kind)) { // (a HUD piece: hud_widget.c)
                ch |= hud_widget_details(w);
                break;
            }
            ch |= igColorEdit4(PL(TR(w->kind == UIW_BORDER ? "Brush Color" : "Background Color")), w->bg, 0);
            if (w->kind == UIW_BORDER || w->kind == UIW_VBOX || w->kind == UIW_HBOX || w->kind == UIW_SCROLL || w->kind == UIW_WRAP)
                ch |= igDragFloat(w->kind == UIW_BORDER ? TR("Padding") : TR("Spacing"), &w->padding, 0.5f, 0.0f, 200.0f, "%.0f", 0);
            break;
        }
        if (w->kind != UIW_TEXT && w->kind != UIW_IMAGE && w->kind != UIW_SPACER && w->kind != UIW_THROBBER && w->kind != UIW_CIRCLE_THROBBER &&
            w->kind != UIW_TOUCH_AREA && w->kind != UIW_JOYSTICK && w->kind != UIW_TOUCH_BUTTON && !hud_is_kind(w->kind)) {
            ch |= igDragFloat(PL(TR("Corner Radius")), &w->radius, 0.25f, 0.0f, 500.0f, "%.0f", 0);
            ch |= igDragFloat(PL(TR("Outline Width")), &w->border_w, 0.1f, 0.0f, 50.0f, "%.1f", 0);
            if (w->border_w > 0.0f) ch |= igColorEdit4(PL(TR("Outline Color")), w->border_col, 0);
        }
    }
    // --- a Game User Setting it shows and changes (the options' menus: game_settings.h) ---
    if ((w->kind == UIW_SLIDER || w->kind == UIW_SPIN || w->kind == UIW_CHECKBOX || w->kind == UIW_COMBO || w->kind == UIW_TEXT ||
         w->kind == UIW_KEY_SELECTOR || w->kind == UIW_PROGRESS) && section(TR("Game User Setting"))) {
        if (igBeginCombo(PL(TR("Setting")), w->bind_setting[0] ? w->bind_setting : TR("None"), 0)) {
            if (igSelectable_Bool(TR("None"), !w->bind_setting[0], 0, (ImVec2){ 0, 0 })) w->bind_setting[0] = 0, ch = true;
            for (uint32_t s = 0; s < gus_setting_count(); s++)
                if (igSelectable_Bool(gus_setting_name(s), !strcmp(w->bind_setting, gus_setting_name(s)), 0, (ImVec2){ 0, 0 }))
                    snprintf(w->bind_setting, UI_NAME, "%s", gus_setting_name(s)), ch = true;
            igEndCombo();
        }
        ch |= igInputTextWithHint(PL(TR("Or by Name")), "Key:IA_Jump", w->bind_setting, sizeof w->bind_setting, 0, NULL, NULL);
        igTextDisabled("%s", TR("Shows the player's option and changes it (applied at once; the window's with Apply Settings). Key:<Action> for a key, Key:<Action>:Gamepad for its gamepad button."));
    }
    if (w->kind == UIW_KEY_SELECTOR && section(TR("Key"))) {
        ch |= igInputText(PL(TR("Selected Key")), w->text, sizeof w->text, 0, NULL, NULL);
        ch |= igInputText(PL(TR("No Key Text")), w->hint, sizeof w->hint, 0, NULL, NULL);
        ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 6.0f, 200.0f, "%.0f", 0);
        ch |= igColorEdit4(PL(TR("Text Color")), w->color, 0);
        ch |= igColorEdit4(PL(TR("Background")), w->bg, 0);
    }
    // --- Behavior ---
    if (section(TR("Behavior"))) {
        static const char* const vis[UI_VIS_COUNT] = { "Visible", "Collapsed", "Hidden", "Not Hit-Testable (Self & All Children)", "Not Hit-Testable (Self Only)" };
        const char* vt[UI_VIS_COUNT];
        for (int v = 0; v < UI_VIS_COUNT; v++) vt[v] = TR(vis[v]);
        int vm = w->vis_mode;
        if (igCombo_Str_arr(PL(TR("Visibility")), &vm, vt, UI_VIS_COUNT, -1)) w->vis_mode = (uint8_t)vm, ch = true;
        bool en = w->enabled;
        if (igCheckbox(TR("Is Enabled"), &en)) w->enabled = en, ch = true;
        ch |= igInputText(PL(TR("Tool Tip Text")), w->tooltip, sizeof w->tooltip, 0, NULL, NULL);
        ch |= igSliderFloat(PL(TR("Render Opacity")), &w->opacity, 0.0f, 1.0f, "%.2f", 0);
    }
    if (section(TR("Render Transform"))) {
        ch |= igDragFloat2(PL(TR("Translation")), w->xlate, 1.0f, -4000.0f, 4000.0f, "%.0f", 0);
        ch |= igDragFloat2(PL(TR("Scale")), w->scale, 0.01f, -10.0f, 10.0f, "%.2f", 0);
        ch |= igDragFloat(PL(TR("Angle")), &w->angle, 0.5f, -360.0f, 360.0f, "%.1f\xc2\xb0", 0);
    }
    // --- Events (Unreal's green "+" buttons): the widget's Custom Events in the Event Graph ---
    static const char* const ev_button[] = { "OnClicked", "OnPressed", "OnReleased", "OnHovered", "OnUnhovered" };
    const char* const* evs = NULL;
    int nev = 0;
    static const char* const ev_value[] = { "OnValueChanged" }, * const ev_check[] = { "OnCheckStateChanged" },
                             * const ev_text[] = { "OnTextChanged", "OnTextCommitted" }, * const ev_combo[] = { "OnSelectionChanged" },
                             * const ev_key[] = { "OnKeySelected" };
    switch (w->kind) {
    case UIW_BUTTON: evs = ev_button, nev = 5; break;
    case UIW_TOUCH_BUTTON: evs = ev_button + 1, nev = 2; break;
    case UIW_SLIDER: case UIW_SPIN: evs = ev_value, nev = 1; break;
    case UIW_CHECKBOX: evs = ev_check, nev = 1; break;
    case UIW_EDIT: evs = ev_text, nev = 2; break;
    case UIW_COMBO: evs = ev_combo, nev = 1; break;
    case UIW_KEY_SELECTOR: evs = ev_key, nev = 1; break;
    default: break;
    }
    if (nev && section(TR("Events")))
        for (int e = 0; e < nev; e++) ch |= event_button(a, w, evs[e]);
    // --- Sounds (Unreal's Hovered / Pressed Slate Sounds): played as UI sounds on the widget's events ---
    if (nev && w->kind != UIW_EDIT && section(TR("Sounds"))) {
        ch |= snd_pick(PL(TR("Hovered Sound")), &w->snd[UI_SND_HOVERED]);
        ch |= snd_pick(PL(TR("Pressed Sound")), &w->snd[UI_SND_PRESSED]);
        if (w->kind == UIW_BUTTON) ch |= snd_pick(PL(TR("Clicked Sound")), &w->snd[UI_SND_CLICKED]);
        else ch |= snd_pick(PL(TR("Changed Sound")), &w->snd[UI_SND_CHANGED]);
    }
    igSpacing();
    if (i > 0 && igButton(TR("Delete Widget"), (ImVec2){ 0, 0 })) {
        ui_tree_remove(t, i);
        D.sel = -1;
        ch = true;
    }
    return ch;
}

// The Palette, Unreal's categories; a click adds to the selected panel (or the selection's parent)
static const struct { const char* cat; uint8_t kind; } g_palette[] = {
    { "Common", UIW_BORDER }, { "Common", UIW_BUTTON }, { "Common", UIW_CHECKBOX }, { "Common", UIW_IMAGE }, { "Common", UIW_PROGRESS },
    { "Common", UIW_SLIDER }, { "Common", UIW_TEXT }, { "Common", UIW_RICH_TEXT }, { "Common", UIW_SPACER }, { "Common", UIW_THROBBER },
    { "Common", UIW_CIRCLE_THROBBER }, { "Lists", UIW_LIST_VIEW }, { "Lists", UIW_TILE_VIEW },
    { "Input", UIW_COMBO }, { "Input", UIW_EDIT }, { "Input", UIW_SPIN }, { "Input", UIW_KEY_SELECTOR },
    { "Panel", UIW_CANVAS }, { "Panel", UIW_HBOX }, { "Panel", UIW_VBOX }, { "Panel", UIW_OVERLAY }, { "Panel", UIW_SCROLL },
    { "Panel", UIW_SIZEBOX }, { "Panel", UIW_GRID }, { "Panel", UIW_WRAP },
    { "Inventory", UIW_INVENTORY },
    { "Abilities", UIW_ABILITY_SLOT }, { "Abilities", UIW_EFFECT_LIST },
    { "HUD", UIW_ATTR_BAR }, { "HUD", UIW_COMPASS }, { "HUD", UIW_MESSAGES }, { "HUD", UIW_DAMAGE_NUMBERS }, { "HUD", UIW_CROSSHAIR },
    { "HUD", UIW_SCREEN_MARKERS }, { "HUD", UIW_DAMAGE_DIR }, { "HUD", UIW_MINIMAP },
};

static bool palette(UiTree* t) {
    bool ch = false;
    igSetNextItemWidth(-1);
    igInputTextWithHint("##ui_find", TR("Search Palette"), D.find, sizeof D.find, 0, NULL, NULL);
    const char* cat = NULL;
    bool open = true;
    for (size_t p = 0; p < sizeof g_palette / sizeof g_palette[0]; p++) {
        const char* nm = TR(g_ui_kind_names[g_palette[p].kind]);
        if (D.find[0]) { // (searching: the categories flat)
            char a[96], b[48];
            snprintf(a, sizeof a, "%s %s", g_ui_kind_names[g_palette[p].kind], nm);
            snprintf(b, sizeof b, "%s", D.find);
            for (char* c = a; *c; c++) *c = (char)tolower((unsigned char)*c);
            for (char* c = b; *c; c++) *c = (char)tolower((unsigned char)*c);
            if (!strstr(a, b)) continue;
        } else if (!cat || strcmp(cat, g_palette[p].cat)) {
            cat = g_palette[p].cat;
            open = igCollapsingHeader_TreeNodeFlags(TR(cat), ImGuiTreeNodeFlags_DefaultOpen);
        }
        if (!D.find[0] && !open) continue;
        if (igSelectable_Bool(nm, false, 0, (ImVec2){ 0, 0 })) {
            uint8_t k = g_palette[p].kind;
            int parent = D.sel >= 0 && ui_is_container(t->w[D.sel].kind) ? D.sel : D.sel >= 0 ? t->w[D.sel].parent : 0;
            int nw = ui_tree_add(t, k, parent < 0 ? 0 : parent);
            if (nw >= 0) {
                D.sel = nw, ch = true;
                if (k == UIW_BUTTON) { // (a Button with its label)
                    int tx = ui_tree_add(t, UIW_TEXT, nw);
                    if (tx >= 0) snprintf(t->w[tx].text, sizeof t->w[tx].text, "Button");
                }
            }
        }
    }
    return ch;
}

// --- The Widget Animations' timeline (Unreal's UMG Animations / Timeline, simpler) ---
static bool timeline(UiTree* t, float width, float height) {
    bool ch = false;
    float fs = igGetFontSize();
    // the left: the animations
    igBeginChild_Str("ui_anims", (ImVec2){ fs * 11.0f, height }, ImGuiChildFlags_Borders, 0);
    igTextDisabled("%s", TR("Animations"));
    if (t->nanims < UI_MAX_ANIMS && igButton(TR("+ Animation"), (ImVec2){ -1, 0 })) {
        UiWAnim* an = &t->anims[t->nanims];
        memset(an, 0, sizeof *an);
        for (int k = 1; k < 100; k++) {
            snprintf(an->name, UI_NAME, "NewAnimation_%d", k);
            if (ui_anim_find(t, an->name) < 0) break;
        }
        an->length = 1.0f;
        D.anim = (int)t->nanims++, D.anim_t = 0.0f, ch = true;
    }
    for (uint32_t i = 0; i < t->nanims; i++) {
        igPushID_Int((int)i);
        if (igSelectable_Bool(t->anims[i].name, D.anim == (int)i, 0, (ImVec2){ 0, 0 })) D.anim = D.anim == (int)i ? -1 : (int)i, D.anim_t = 0.0f, D.key_tr = -1;
        igPopID();
    }
    if (D.anim >= 0 && D.anim < (int)t->nanims) {
        UiWAnim* an = &t->anims[D.anim];
        igSeparator();
        char nm[UI_NAME];
        snprintf(nm, sizeof nm, "%s", an->name);
        igSetNextItemWidth(-1);
        if (igInputText("##anim_name", nm, sizeof nm, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL) && nm[0] && ui_anim_find(t, nm) < 0)
            snprintf(an->name, UI_NAME, "%s", nm), ch = true;
        igSetNextItemWidth(-1);
        ch |= igDragFloat("##anim_len", &an->length, 0.01f, 0.05f, 60.0f, TR("%.2f s long"), 0);
        if (igButton(TR("Delete Animation"), (ImVec2){ -1, 0 })) {
            memmove(&t->anims[D.anim], &t->anims[D.anim + 1], sizeof(UiWAnim) * (t->nanims - (uint32_t)D.anim - 1));
            t->nanims--, D.anim = -1, ch = true;
        }
    }
    igEndChild();
    igSameLine(0, 4);
    igBeginChild_Str("ui_timeline", (ImVec2){ width - fs * 11.0f - 4.0f, height }, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollWithMouse);
    if (D.anim < 0 || D.anim >= (int)t->nanims) {
        igTextDisabled("%s", TR("Choose or make an animation: its tracks key a widget's opacity, translation, scale, angle or color over time; Play Animation plays it in the game."));
        igEndChild();
        return ch;
    }
    UiWAnim* an = &t->anims[D.anim];
    // the toolbar: play, the time, + Track (the selected widget's property), Key
    if (igButton(D.anim_play ? TR("Stop") : TR("Play"), (ImVec2){ 0, 0 })) D.anim_play = !D.anim_play;
    igSameLine(0, -1);
    igSetNextItemWidth(fs * 6.0f);
    igDragFloat("##anim_t", &D.anim_t, 0.01f, 0.0f, an->length, "%.2f s", 0);
    igSameLine(0, fs);
    bool can_track = D.sel >= 0 && D.sel < (int)t->n && an->ntracks < UI_MAX_TRACKS;
    igBeginDisabled(!can_track);
    const char* pn[UIP_COUNT];
    for (int p = 0; p < UIP_COUNT; p++) pn[p] = TR(g_ui_prop_names[p]);
    igSetNextItemWidth(fs * 9.0f);
    igCombo_Str_arr("##new_prop", &D.new_prop, pn, UIP_COUNT, -1);
    igSameLine(0, -1);
    if (igButton(TR("+ Track"), (ImVec2){ 0, 0 }) && can_track) {
        UiWTrack* trk = &an->tracks[an->ntracks++];
        memset(trk, 0, sizeof *trk);
        snprintf(trk->widget, UI_NAME, "%s", t->w[D.sel].name);
        trk->prop = (uint8_t)D.new_prop;
        ui_track_key(trk, D.anim_t, ui_prop_get(&t->w[D.sel], D.new_prop));
        ch = true;
    }
    igEndDisabled();
    igSameLine(0, -1);
    bool can_key = D.sel >= 0 && D.sel < (int)t->n;
    igBeginDisabled(!can_key);
    igSameLine(0, -1); // a sound key at this time (Unreal's Audio track)
    static uint32_t s_new_snd;
    igSetNextItemWidth(fs * 9.0f);
    snd_pick("##anim_snd", &s_new_snd);
    igSameLine(0, -1);
    if (igButton(TR("+ Sound"), (ImVec2){ 0, 0 }) && s_new_snd && an->nsnd < 8) {
        an->snd_t[an->nsnd] = D.anim_t, an->snd[an->nsnd++] = s_new_snd;
        ch = true;
    }
    igSetItemTooltip("%s", TR("Adds the chosen sound at this time: it plays when the animation passes it. Right click a sound's mark to remove it."));
    igSameLine(0, -1);
    if (igButton(TR("Key"), (ImVec2){ 0, 0 }) && can_key) // the selected widget's tracks: their values now at this time
        for (int k = 0; k < an->ntracks; k++)
            if (!strcmp(an->tracks[k].widget, t->w[D.sel].name)) ui_track_key(&an->tracks[k], D.anim_t, ui_prop_get(&t->w[D.sel], an->tracks[k].prop)), ch = true;
    igEndDisabled();
    igSetItemTooltip("%s", TR("Keys the selected widget's tracks with its values (Details) at this time."));
    // the ruler and the tracks
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 o = igGetCursorScreenPos(), av = igGetContentRegionAvail();
    float label_w = fs * 11.0f, x0 = o.x + label_w, x1 = o.x + av.x - fs * 0.5f, rh = fs * 1.5f;
    float span = x1 - x0, len = an->length > 0.05f ? an->length : 0.05f;
    ImDrawList_AddRectFilled(dl, (ImVec2){ x0, o.y }, (ImVec2){ x1, o.y + rh }, TH(TK_N6), 0, 0);
    for (int s = 0; s <= 10; s++) {
        float x = x0 + span * s / 10.0f;
        ImDrawList_AddLine(dl, (ImVec2){ x, o.y + rh * 0.55f }, (ImVec2){ x, o.y + rh }, TH(TK_N11), 1.0f);
        char lab[16];
        snprintf(lab, sizeof lab, "%.2f", len * s / 10.0f);
        if (s % 2 == 0) ImDrawList_AddText_Vec2(dl, (ImVec2){ x + 2, o.y }, TH(TK_N12), lab, NULL);
    }
    igSetCursorScreenPos((ImVec2){ x0, o.y });
    igInvisibleButton("##ruler", (ImVec2){ span > 1 ? span : 1, rh }, 0);
    if (igIsItemActive()) D.anim_t = fminf(fmaxf((igGetIO_Nil()->MousePos.x - x0) / span * len, 0.0f), len), D.anim_play = false;
    int del_track = -1;
    for (int k = 0; k < an->ntracks; k++) {
        UiWTrack* trk = &an->tracks[k];
        float y = o.y + rh * (k + 1) + 2.0f;
        if (y + rh > o.y + av.y) break;
        ImDrawList_AddRectFilled(dl, (ImVec2){ o.x, y }, (ImVec2){ x1, y + rh - 2 }, k % 2 ? TH(TK_N5) : TH(TK_N5), 0, 0);
        char lab[UI_NAME + 48];
        snprintf(lab, sizeof lab, "%s \xc2\xb7 %s", trk->widget, TR(g_ui_prop_names[trk->prop]));
        ImDrawList_PushClipRect(dl, (ImVec2){ o.x, y }, (ImVec2){ x0 - 4, y + rh }, true);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 4, y + (rh - fs) * 0.5f - 1 }, TH(TK_N14), lab, NULL);
        ImDrawList_PopClipRect(dl);
        igSetCursorScreenPos((ImVec2){ o.x, y });
        igPushID_Int(k);
        igInvisibleButton("##trlab", (ImVec2){ label_w - 4, rh - 2 }, 0);
        if (igBeginPopupContextItem("tr_menu", ImGuiPopupFlags_MouseButtonRight)) {
            if (igMenuItem_Bool(TR("Delete Track"), NULL, false, true)) del_track = k;
            igEndPopup();
        }
        for (int q = 0; q < trk->nkeys; q++) { // its keys, diamonds; a click selects, a drag moves in time
            float x = x0 + trk->t[q] / len * span, cy = y + rh * 0.5f - 1, s = fs * 0.36f;
            bool sel = D.key_tr == k && D.key_k == q;
            ImVec2 dq[4] = { { x, cy - s }, { x + s, cy }, { x, cy + s }, { x - s, cy } };
            ImDrawList_AddConvexPolyFilled(dl, dq, 4, sel ? TH(TK_AMBER) : TH(TK_N14));
            igSetCursorScreenPos((ImVec2){ x - s, cy - s });
            igPushID_Int(q);
            igInvisibleButton("##key", (ImVec2){ s * 2, s * 2 }, 0);
            if (igIsItemActivated()) D.key_tr = k, D.key_k = q, D.anim_t = trk->t[q];
            if (igIsItemActive() && igGetIO_Nil()->MouseDelta.x != 0.0f) {
                float nt = fminf(fmaxf(trk->t[q] + igGetIO_Nil()->MouseDelta.x / span * len, 0.0f), len);
                float v = trk->v[q];
                memmove(&trk->t[q], &trk->t[q + 1], sizeof(float) * (size_t)(trk->nkeys - q - 1));
                memmove(&trk->v[q], &trk->v[q + 1], sizeof(float) * (size_t)(trk->nkeys - q - 1));
                trk->nkeys--;
                ui_track_key(trk, nt, v);
                for (int z = 0; z < trk->nkeys; z++) if (trk->t[z] == nt) D.key_k = z;
                D.anim_t = nt, ch = true;
            }
            igSetItemTooltip("%.2f s: %.3f", trk->t[q], trk->v[q]);
            igPopID();
        }
        igPopID();
    }
    if (del_track >= 0) {
        memmove(&an->tracks[del_track], &an->tracks[del_track + 1], sizeof(UiWTrack) * (size_t)(an->ntracks - del_track - 1));
        an->ntracks--, D.key_tr = -1, ch = true;
    }
    // the time line over the tracks
    float tx = x0 + D.anim_t / len * span;
    ImDrawList_AddLine(dl, (ImVec2){ tx, o.y }, (ImVec2){ tx, o.y + av.y }, TH(TK_ORANGE), 2.0f);
    // Del: the selected key
    if (D.key_tr >= 0 && D.key_tr < an->ntracks && igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && igIsKeyPressed_Bool(ImGuiKey_Delete, false)) {
        UiWTrack* trk = &an->tracks[D.key_tr];
        if (D.key_k >= 0 && D.key_k < trk->nkeys) {
            memmove(&trk->t[D.key_k], &trk->t[D.key_k + 1], sizeof(float) * (size_t)(trk->nkeys - D.key_k - 1));
            memmove(&trk->v[D.key_k], &trk->v[D.key_k + 1], sizeof(float) * (size_t)(trk->nkeys - D.key_k - 1));
            trk->nkeys--, D.key_tr = -1, ch = true;
        }
    }
    if (D.anim_play) D.anim_t = fmodf(D.anim_t + igGetIO_Nil()->DeltaTime, len);
    igEndChild();
    return ch;
}

bool ui_designer_tab(BpAsset* a, float width, float height) {
    if (!a->ui) {
        a->ui = calloc(1, sizeof(UiTree)); // (the first open of a Widget Blueprint: its root)
        if (!a->ui) return false;
        ui_tree_default(a->ui);
    }
    UiTree* t = a->ui;
    if (D.a != a) D.a = a, D.sel = -1, D.anim = -1, D.anim_play = false;
    if (D.sel >= (int)t->n) D.sel = -1;
    bool ch = false;
    float fs = igGetFontSize(), side = fminf(fs * 17.0f, width * 0.24f);
    // the left: the Palette and the Hierarchy
    igBeginChild_Str("ui_left", (ImVec2){ side, height }, ImGuiChildFlags_Borders, 0);
    igSeparatorText(TR("Palette"));
    igBeginChild_Str("ui_pal", (ImVec2){ 0, height * 0.48f }, 0, 0);
    ch |= palette(t);
    igEndChild();
    igSeparatorText(TR("Hierarchy"));
    igBeginChild_Str("ui_hier", (ImVec2){ 0, 0 }, 0, 0);
    for (uint32_t i = 0; i < t->n; i++) {
        int depth = 0;
        for (int p = t->w[i].parent; p >= 0 && depth < 16; p = t->w[p].parent) depth++;
        char line[UI_NAME + 64];
        snprintf(line, sizeof line, "%*s%s###h%u", depth * 2, "", t->w[i].name, i);
        igPushStyleColor_U32(ImGuiCol_Text, collapsed(&t->w[i]) || t->w[i].vis_mode == UI_VIS_HIDDEN ? TH(TK_N11) : TH(TK_N14));
        if (igSelectable_Bool(line, (int)i == D.sel, 0, (ImVec2){ 0, 0 })) D.sel = (int)i;
        igPopStyleColor(1);
        igSameLine(0, igGetFontSize() * 0.6f);
        igTextDisabled("%s", TR(g_ui_kind_names[t->w[i].kind]));
    }
    igEndChild();
    igEndChild();
    igSameLine(0, 4);
    // the middle: the resolution, the preview (a click selects, a drag moves, the corner resizes), the animations
    float mid = width - side * 2.0f - 12.0f;
    igBeginChild_Str("ui_mid", (ImVec2){ mid, height }, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const char* rn[sizeof g_res / sizeof g_res[0]];
    for (size_t r = 0; r < sizeof g_res / sizeof g_res[0]; r++) rn[r] = g_res[r].name;
    igSetNextItemWidth(fs * 12.0f);
    igCombo_Str_arr("##ui_res", &D.res, rn, (int)(sizeof g_res / sizeof g_res[0]), -1);
    igSetItemTooltip("%s", TR("The preview's screen: how the anchors hold at other sizes and shapes"));
    float anim_h = height * 0.30f;
    igBeginChild_Str("ui_canvas", (ImVec2){ mid, height - anim_h - igGetFrameHeightWithSpacing() - 4.0f }, ImGuiChildFlags_Borders,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 o = igGetCursorScreenPos(), av = igGetContentRegionAvail();
    float rw = g_res[D.res].w, rh = g_res[D.res].h, pw = av.x, ph = av.x * rh / rw;
    if (ph > av.y) ph = av.y, pw = ph * rw / rh;
    UiRect area = { o.x + (av.x - pw) * 0.5f, o.y + (av.y - ph) * 0.5f, 0, 0 };
    area.x1 = area.x0 + pw, area.y1 = area.y0 + ph;
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_AddRectFilled(dl, (ImVec2){ area.x0, area.y0 }, (ImVec2){ area.x1, area.y1 }, TH(TK_BLUE_D), 0, 0); // (a sky-ish backdrop)
    ImDrawList_AddRect(dl, (ImVec2){ area.x0, area.y0 }, (ImVec2){ area.x1, area.y1 }, TH(TK_N11), 0, 1.0f, 0);
    float k = ph / UI_REF_HEIGHT * (1080.0f / rh) * (rh / 1080.0f); // (1080p designer pixels over the preview's height)
    static UiLayout L;
    // an animation edited: the preview shows it at the timeline's time (a copy: the tree keeps what is authored)
    static UiTree shown_t;
    const UiTree* dt = t;
    if (D.anim >= 0 && D.anim < (int)t->nanims) {
        shown_t = *t;
        ui_anim_apply(&shown_t, &t->anims[D.anim], D.anim_t);
        dt = &shown_t;
    }
    layout(dt, 0, area, area, 1.0f, true, k, &L);
    ImDrawList_PushClipRect(dl, (ImVec2){ area.x0, area.y0 }, (ImVec2){ area.x1, area.y1 }, true);
    UiState st = { -1, -1, -1, -1, true, re_time_seconds(), -1, -1 };
    draw_tree(dl, dt, &L, k, &st);
    if (D.sel >= 0) {
        UiRect s = L.r[D.sel];
        ImDrawList_AddRect(dl, (ImVec2){ s.x0, s.y0 }, (ImVec2){ s.x1, s.y1 }, TH(TK_ACCENT), 0, 2.0f, 0);
        ImDrawList_AddRectFilled(dl, (ImVec2){ s.x1 - 5, s.y1 - 5 }, (ImVec2){ s.x1 + 5, s.y1 + 5 }, TH(TK_ORANGE), 0, 0);
    }
    ImDrawList_PopClipRect(dl);
    igSetCursorScreenPos((ImVec2){ area.x0, area.y0 });
    igInvisibleButton("ui_area", (ImVec2){ pw > 1 ? pw : 1, ph > 1 ? ph : 1 }, ImGuiButtonFlags_MouseButtonLeft);
    ImGuiIO* io = igGetIO_Nil();
    if (igIsItemActivated()) {
        UiRect s = D.sel >= 0 ? L.r[D.sel] : (UiRect){ 0, 0, 0, 0 };
        D.sizing = D.sel > 0 && fabsf(io->MousePos.x - s.x1) < 8 && fabsf(io->MousePos.y - s.y1) < 8;
        if (!D.sizing) D.sel = pick(dt, &L, io->MousePos.x, io->MousePos.y, 0);
        D.dragging = D.sel > 0;
    }
    if (igIsItemActive() && D.sel > 0 && (io->MouseDelta.x != 0.0f || io->MouseDelta.y != 0.0f)) {
        UiWidget* w = &t->w[D.sel];
        float dx = io->MouseDelta.x / k, dy = io->MouseDelta.y / k;
        bool canvas = t->w[w->parent].kind == UIW_CANVAS;
        if (D.sizing) w->size[0] = fmaxf(w->size[0] + dx, 4.0f), w->size[1] = fmaxf(w->size[1] + dy, 4.0f), ch = true;
        else if (D.dragging && canvas) w->pos[0] += dx, w->pos[1] += dy, ch = true;
    }
    if (!igIsItemActive()) D.dragging = D.sizing = false;
    if (D.sel > 0 && igIsWindowFocused(0) && igIsKeyPressed_Bool(ImGuiKey_Delete, false)) ui_tree_remove(t, D.sel), D.sel = -1, ch = true;
    igEndChild();
    ch |= timeline(t, mid, anim_h);
    igEndChild();
    igSameLine(0, 4);
    // the right: the selected widget's Details
    igBeginChild_Str("ui_right", (ImVec2){ side, height }, ImGuiChildFlags_Borders, 0);
    igPushItemWidth(-fs * 7.0f);
    if (D.sel >= 0) ch |= widget_details(a, t, D.sel);
    else igTextDisabled("%s", TR("Select a widget (the preview or the Hierarchy); add from the Palette."));
    igPopItemWidth();
    igEndChild();
    return ch;
}

bool ui_widget_combo(const char* label, char* name, size_t cap, const BpAsset* a, uint64_t kinds) {
    bool ch = false;
    const UiTree* t = a ? a->ui : NULL;
    if (igBeginCombo(label, name[0] ? name : "-", 0)) {
        for (uint32_t i = 0; t && i < t->n; i++)
            if ((!kinds || (kinds & (1ull << t->w[i].kind))) && igSelectable_Bool(t->w[i].name, !strcmp(name, t->w[i].name), 0, (ImVec2){ 0, 0 }))
                snprintf(name, cap, "%s", t->w[i].name), ch = true;
        igEndCombo();
    }
    if (!t) igTextDisabled("%s", TR("(the widget names come from this Widget Blueprint's Designer)"));
    return ch;
}

bool ui_anim_combo(const char* label, char* name, size_t cap, const BpAsset* a) {
    bool ch = false;
    const UiTree* t = a ? a->ui : NULL;
    if (igBeginCombo(label, name[0] ? name : "-", 0)) {
        for (uint32_t i = 0; t && i < t->nanims; i++)
            if (igSelectable_Bool(t->anims[i].name, !strcmp(name, t->anims[i].name), 0, (ImVec2){ 0, 0 })) snprintf(name, cap, "%s", t->anims[i].name), ch = true;
        igEndCombo();
    }
    if (!t || !t->nanims) igTextDisabled("%s", TR("(the animations come from this Widget Blueprint's Designer: + Animation)"));
    return ch;
}

// --- The Health's floating damage numbers (health.h; Health > Damage Numbers): one drawing with the HUD's Damage Numbers
// widget (hud_widget.c); here only when no such widget drew them this frame. The view: the camera's place, its axes and
// horizontal field of view, the rectangle it is drawn in ---
void ui_draw_damage_numbers(ImDrawList* dl, const float pos[3], const float fwd[3], const float right[3], const float up[3], float fov_deg,
                            float x0, float y0, float x1, float y1) {
    if (!dl || x1 <= x0 || y1 <= y0) return;
    hud_set_view(pos, fwd, right, up, fov_deg);
    hud_draw_numbers_default(dl, x0, y0, x1, y1);
}
