// The asset picker (asset_picker.h; Q6, notas-dev/content-browser.md)
#include "asset_picker.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "asset_viewer.h"
#include "content.h"
#include "editor.h"
#include "i18n.h"
#include "mem.h"
#include "platform.h"
#include "platform_files.h"
#include "theme.h"
#include "ui_icons.h"

#define PICK_MAX 8192   // the candidates at most (a project with more: the first ones by the walk, a note says so)
#define PICK_FOLDERS 1024
#define PICK_REF 256
#define PICK_LABEL 64

typedef struct Entry {
    char ref[PICK_REF];   // "/Content/..." or the caller's
    char label[PICK_LABEL];
    uint8_t kind;         // AssetKind
    int8_t icon;          // UiIcon; -1: the kind's
    uint8_t group;        // 0 first (Common Classes)
    uint16_t folder;      // index in folders (0: none / the caller's)
    uint32_t user;
    const char* tip;      // a description (static text; NULL: none)
} Entry;

static struct {
    bool open, want_open, first_frame;
    char title[96], hint[160], current[PICK_REF], ctx[512];
    uint64_t kinds;
    bool allow_none;
    uint32_t owner;
    AssetPickFn on_pick;
    ReVmBuf mem;          // entries, the visible list and the folders, reserved once
    Entry* e;
    int32_t* vis;
    char (*folders)[PICK_REF];
    int n, nvis, nfolders;
    bool dirty_filter, dirty_sort, filter_set;
    char filter[128];
    uint64_t kind_on;     // the type filter (the kinds present, each on or off)
    uint64_t kinds_present;
    int folder;           // 0 all, else folders[folder]
    int sel;              // an index in e (-1: none)
    bool scroll_to_sel;
    int view, size;       // the corner button's (editor.ini)
    // a field's result, kept until its owner reads it
    uint32_t result_owner;
    char result_ref[PICK_REF];
    uint32_t result_user;
    int result_frame;
    // the measures and the tests' layout
    double scan_ms, draw_ms;
    int per_row;
    float row_h;
    int choose; // asset_picker_choose: the visible index to pick on the next draw (-1: none)
    int dbl;    // the entry double clicked this frame (-1: none)
} P = { .view = PICKER_VIEW_BOTH, .size = 1, .sel = -1, .choose = -1, .dbl = -1 };

static bool reserve(void) {
    if (P.e) return true;
    size_t bytes = sizeof(Entry) * PICK_MAX + sizeof(int32_t) * PICK_MAX + (size_t)PICK_REF * PICK_FOLDERS;
    if (!re_vm_reserve(&P.mem, bytes) || !re_vm_ensure(&P.mem, bytes)) return false;
    P.e = (Entry*)P.mem.base;
    P.vis = (int32_t*)(P.e + PICK_MAX);
    P.folders = (char(*)[PICK_REF])(P.vis + PICK_MAX);
    return true;
}

// --- The candidates ---

static int folder_index(const char* ref) {
    const char* sl = strrchr(ref, '/');
    if (!sl || sl == ref) return 0;
    int len = (int)(sl - ref);
    for (int i = P.nfolders - 1; i >= 1; i--) // (the walk adds a folder's files together: the last one is the usual hit)
        if ((int)strlen(P.folders[i]) == len && !strncmp(P.folders[i], ref, (size_t)len)) return i;
    if (P.nfolders >= PICK_FOLDERS) return 0;
    snprintf(P.folders[P.nfolders], PICK_REF, "%.*s", len, ref);
    return P.nfolders++;
}

static void add_entry(const char* label, const char* ref, int kind, int icon, int group, uint32_t user) {
    if (!reserve() || P.n >= PICK_MAX) return;
    Entry* x = &P.e[P.n++];
    snprintf(x->ref, PICK_REF, "%s", ref);
    snprintf(x->label, PICK_LABEL, "%s", label);
    x->kind = (uint8_t)kind;
    x->icon = (int8_t)icon;
    x->group = (uint8_t)group;
    x->user = user;
    x->tip = NULL;
    x->folder = (uint16_t)(ref[0] == '/' ? folder_index(ref) : 0);
    if (kind >= 0 && kind < 64) P.kinds_present |= AK(kind);
    P.dirty_sort = P.dirty_filter = true;
}

typedef struct Walk {
    char vpath[PICK_REF];
    int depth;
} Walk;

static void walk_one(void* user, const ReDirEntry* d) {
    Walk* w = user;
    if (d->name[0] == '.') return; // hidden: .git, a mesh's material (.Rock.rmesh.mat)
    if (d->is_dir) {
        if (w->depth >= 16) return;
        Walk sub = { "", w->depth + 1 };
        snprintf(sub.vpath, sizeof sub.vpath, "%s/%s", w->vpath, d->name);
        char abs[PICK_REF + 600];
        snprintf(abs, sizeof abs, "%s%s", content_root(), sub.vpath + 8);
        re_dir_walk(abs, walk_one, &sub);
        return;
    }
    AssetKind k = content_kind_of(d->name);
    if (k >= 64 || !(P.kinds & AK(k))) return;
    char ref[PICK_REF], label[PICK_LABEL];
    snprintf(ref, sizeof ref, "%s/%s", w->vpath, d->name);
    snprintf(label, sizeof label, "%s", d->name);
    char* dot = strrchr(label, '.');
    if (dot && dot != label) *dot = 0;
    add_entry(label, ref, (int)k, -1, 1, 0);
}

static int entry_cmp(const void* a, const void* b) {
    const Entry *x = a, *y = b;
    if (x->group != y->group) return x->group < y->group ? -1 : 1;
    int c = _stricmp(x->label, y->label);
    return c ? c : strcmp(x->ref, y->ref);
}

void asset_picker_open(const AssetPickerSpec* s) {
    if (!reserve() || !s) return;
    double t0 = re_time_seconds();
    snprintf(P.title, sizeof P.title, "%s", s->title ? s->title : TR("Pick Asset"));
    snprintf(P.hint, sizeof P.hint, "%s", s->hint ? s->hint : "");
    snprintf(P.current, sizeof P.current, "%s", s->current ? s->current : "");
    snprintf(P.ctx, sizeof P.ctx, "%s", s->ctx ? s->ctx : "");
    P.kinds = s->kinds, P.allow_none = s->allow_none, P.owner = s->owner, P.on_pick = s->on_pick;
    P.n = 0, P.nfolders = 1, P.kinds_present = 0, P.folder = 0, P.sel = -1, P.choose = -1, P.dbl = -1;
    P.folders[0][0] = 0;
    P.filter[0] = 0;
    if (P.kinds) {
        Walk w = { "/Content", 0 };
        re_dir_walk(content_root(), walk_one, &w);
    }
    P.kind_on = ~0ull;
    P.open = P.want_open = P.first_frame = true;
    P.dirty_sort = P.dirty_filter = true;
    P.scan_ms = (re_time_seconds() - t0) * 1000.0;
}

void asset_picker_add(const char* label, const char* ref, int kind, int icon, int group, uint32_t user) {
    if (P.open) add_entry(label, ref, kind, icon, group, user);
}
void asset_picker_add_tip(const char* tip) {
    if (P.open && P.n > 0) P.e[P.n - 1].tip = tip;
}
bool asset_picker_is_open(void) { return P.open; }
void asset_picker_close(void) { P.open = P.want_open = false; }

bool asset_picker_result(uint32_t owner, char* ref, size_t cap, uint32_t* user) {
    if (!owner || P.result_owner != owner) return false;
    if (ref && cap) snprintf(ref, cap, "%s", P.result_ref);
    if (user) *user = P.result_user;
    P.result_owner = 0;
    return true;
}

// The answer: the field's (read by its owner next frame) or the callback's
static void pick(int i) {
    const char* ref = i >= 0 ? P.e[i].ref : "";
    uint32_t user = i >= 0 ? P.e[i].user : 0;
    P.open = false;
    if (P.owner) {
        P.result_owner = P.owner;
        snprintf(P.result_ref, sizeof P.result_ref, "%s", ref);
        P.result_user = user;
        P.result_frame = igGetFrameCount();
    }
    if (P.on_pick) {
        char r[PICK_REF], c[512];
        snprintf(r, sizeof r, "%s", ref); // (the callback may open the picker again: its own copies)
        snprintf(c, sizeof c, "%s", P.ctx);
        P.on_pick(r, user, c);
    }
}

// --- Filtering ---

// Every word of the search in the name or in the folder, any case ("mann sk" finds SK_Mannequin)
static bool words_match(const Entry* x, const char* q) {
    char w[64];
    while (*q) {
        while (*q == ' ') q++;
        int n = 0;
        while (q[n] && q[n] != ' ' && n < 63) w[n] = q[n], n++;
        if (!n) break;
        w[n] = 0, q += n;
        bool hit = false;
        for (int pass = 0; pass < 2 && !hit; pass++) {
            const char* h = pass ? x->ref : x->label;
            for (; *h && !hit; h++) {
                int k = 0;
                while (k < n && h[k] && tolower((unsigned char)h[k]) == tolower((unsigned char)w[k])) k++;
                hit = k == n;
            }
        }
        if (!hit) return false;
    }
    return true;
}

static void refilter(void) {
    if (P.dirty_sort) {
        char cur[PICK_REF];
        snprintf(cur, sizeof cur, "%s", P.sel >= 0 ? P.e[P.sel].ref : P.current);
        qsort(P.e, (size_t)P.n, sizeof(Entry), entry_cmp);
        P.sel = -1;
        for (int i = 0; i < P.n && cur[0]; i++)
            if (!strcmp(P.e[i].ref, cur)) P.sel = i, P.scroll_to_sel = true;
        P.dirty_sort = false;
    }
    P.nvis = 0;
    for (int i = 0; i < P.n; i++) {
        const Entry* x = &P.e[i];
        if (x->kind < 64 && !(P.kind_on & AK(x->kind))) continue;
        if (P.folder > 0) { // the folder and the ones under it
            size_t fl = strlen(P.folders[P.folder]);
            if (strncmp(x->ref, P.folders[P.folder], fl) || x->ref[fl] != '/') continue;
        }
        if (P.filter[0] && !words_match(x, P.filter)) continue;
        P.vis[P.nvis++] = i;
    }
    P.dirty_filter = false;
}

// --- The view (editor.ini) ---

void asset_picker_set_view(int view, int size) {
    if (view >= 0 && view < PICKER_VIEW_COUNT) P.view = view;
    if (size >= 0 && size < 3) P.size = size;
}
void asset_picker_get_view(int* view, int* size) {
    if (view) *view = P.view;
    if (size) *size = P.size;
}
void asset_picker_write_prefs(FILE* f) { fprintf(f, "picker_view=%d\npicker_size=%d\n", P.view, P.size); }
bool asset_picker_read_pref(const char* key, const char* v) {
    if (!strcmp(key, "picker_view")) asset_picker_set_view(atoi(v), -1);
    else if (!strcmp(key, "picker_size")) asset_picker_set_view(-1, atoi(v));
    else return false;
    return true;
}

// --- Drawing ---

static void thumb(ImDrawList* dl, const Entry* x, float x0, float y0, float x1, float y1, bool hovered) {
    ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, TH(TK_N4), 3.0f, 0);
    float s = x1 - x0;
    if (x->icon < 0 && x->ref[0] == '/' && content_thumb(dl, x0 + 2, y0 + 2, x1 - 2, y1 - 2, x->ref, x->kind, hovered)) return;
    if (x->icon < 0 && !strncmp(x->ref, "#mesh/", 6) && asset_thumb(dl, x0 + 2, y0 + 2, x1 - 2, y1 - 2, x->ref, x->kind)) return; // (a basic shape)
    int ic = x->icon >= 0 ? x->icon : content_kind_icon(x->kind, x->ref);
    float is = s * 0.55f;
    ui_icon(dl, (UiIcon)ic, x0 + (s - is) * 0.5f, y0 + (y1 - y0 - is) * 0.5f, is, TH(content_kind_color((AssetKind)x->kind)));
}

// A bigger preview in a tooltip (the name-only rows, a small tile)
static void preview_tooltip(const Entry* x) {
    if (!igBeginTooltip()) return;
    float s = igGetFontSize() * 10.0f;
    ImVec2 q = igGetCursorScreenPos();
    igDummy((ImVec2){ s, s });
    thumb(igGetWindowDrawList(), x, q.x, q.y, q.x + s, q.y + s, true);
    igTextUnformatted(x->label, NULL);
    igTextDisabled("%s", asset_kind_name((AssetKind)x->kind));
    if (x->ref[0] == '/') igTextDisabled("%s", x->ref);
    if (x->tip) {
        igPushTextWrapPos(igGetFontSize() * 22.0f);
        igTextUnformatted(x->tip, NULL);
        igPopTextWrapPos();
    }
    igEndTooltip();
}

static void cell(int i, float x, float y, float w, float h, float tile) {
    Entry* xe = &P.e[i];
    ImDrawList* dl = igGetWindowDrawList();
    igPushID_Int(i);
    igSetCursorScreenPos((ImVec2){ x, y });
    igInvisibleButton("##pick", (ImVec2){ w, h }, ImGuiButtonFlags_MouseButtonLeft); // tips: none (a canvas or a tile drawn with its own name: no description)
    bool hov = igIsItemHovered(0), sel = P.sel == i;
    if (igIsItemClicked(ImGuiMouseButton_Left)) {
        P.sel = i;
        if (igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) P.dbl = i; // (picked after the loop: entries stay put)
    }
    uint32_t bg = sel ? TH(TK_ACCENT_DIM) : hov ? TH(TK_N8) : TH(TK_N6);
    ImDrawList_AddRectFilled(dl, (ImVec2){ x, y }, (ImVec2){ x + w, y + h }, bg, 4.0f, 0);
    uint32_t bar = TH(content_kind_color((AssetKind)xe->kind));
    if (P.view == PICKER_VIEW_NAME) { // a row: the kind's icon (or a small thumbnail), the name, the type and the folder
        float s = h - 4.0f;
        thumb(dl, xe, x + 2, y + 2, x + 2 + s, y + 2 + s, hov);
        ImDrawList_AddRectFilled(dl, (ImVec2){ x + 2 + s + 2, y + 3 }, (ImVec2){ x + 2 + s + 5, y + h - 3 }, bar, 1.0f, 0);
        float tx = x + s + 14.0f, ty = y + (h - igGetTextLineHeight()) * 0.5f;
        ImDrawList_AddText_Vec2(dl, (ImVec2){ tx, ty }, TH(TK_N14), xe->label, NULL);
        const char* kn = asset_kind_name((AssetKind)xe->kind);
        float kx = x + w * 0.55f;
        ImDrawList_AddText_Vec2(dl, (ImVec2){ kx, ty }, TH(TK_N11), kn, NULL);
        if (xe->folder) ImDrawList_AddText_Vec2(dl, (ImVec2){ x + w * 0.75f, ty }, TH(TK_N10), P.folders[xe->folder], NULL);
        if (hov) preview_tooltip(xe);
    } else { // a tile: the thumbnail, the type's bar, the name under it (Preview and name)
        thumb(dl, xe, x + 3, y + 3, x + tile - 3, y + tile - 3, hov);
        ImDrawList_AddRectFilled(dl, (ImVec2){ x + 3, y + tile - 5 }, (ImVec2){ x + tile - 3, y + tile - 2 }, bar, 0, 0);
        if (P.view == PICKER_VIEW_BOTH) {
            char shown[PICK_LABEL + 4];
            snprintf(shown, sizeof shown, "%s", xe->label);
            float maxw = w - 8.0f, dots = igCalcTextSize("...", NULL, false, -1).x;
            if (igCalcTextSize(shown, NULL, false, -1).x > maxw) {
                size_t k = strlen(shown);
                while (k > 1 && igCalcTextSize(shown, NULL, false, -1).x > maxw - dots) shown[--k] = 0;
                strcat(shown, "...");
            }
            ImDrawList_AddText_Vec2(dl, (ImVec2){ x + 4, y + tile + 2 }, TH(TK_N14), shown, NULL);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ x + 4, y + tile + 2 + igGetTextLineHeight() }, TH(TK_N11),
                                    asset_kind_name((AssetKind)xe->kind), NULL);
            if (hov && xe->tip) preview_tooltip(xe);
            else if (hov) igSetItemTooltip("%s\n%s", xe->label, xe->ref[0] == '/' ? xe->ref : asset_kind_name((AssetKind)xe->kind));
        } else if (hov) preview_tooltip(xe);
    }
    if (sel) ImDrawList_AddRect(dl, (ImVec2){ x, y }, (ImVec2){ x + w, y + h }, TH(TK_ACCENT), 4.0f, 2.0f, 0);
    igPopID();
}

// The corner button: what each asset shows and how big
static void view_menu(void) {
    if (ui_icon_button("pick_view", ICON_EYE, NULL, TH(TK_N14), false, TR("View: preview, name, size"))) igOpenPopup_Str("pick_view_menu", 0);
    if (igBeginPopup("pick_view_menu", 0)) {
        static const char* const views[PICKER_VIEW_COUNT] = { "Preview only", "Name only", "Preview and name" };
        static const char* const sizes[3] = { "Small", "Medium", "Large" };
        igSeparatorText(TR("Show"));
        for (int v = 0; v < PICKER_VIEW_COUNT; v++)
            if (igMenuItem_Bool(TR(views[v]), NULL, P.view == v, true) && P.view != v) P.view = v, editor_prefs_changed();
        igSeparatorText(TR("Preview Size"));
        for (int s = 0; s < 3; s++)
            if (igMenuItem_Bool(TR(sizes[s]), NULL, P.size == s, true) && P.size != s) P.size = s, editor_prefs_changed();
        igEndPopup();
    }
}

static void type_filters(void) {
    int shown = 0;
    for (int k = 0; k < ASSET_KIND_COUNT; k++) shown += (P.kinds_present & AK(k)) != 0;
    if (shown < 2) return; // (one type: nothing to filter)
    for (int k = 0; k < ASSET_KIND_COUNT; k++) {
        if (!(P.kinds_present & AK(k))) continue;
        bool on = (P.kind_on & AK(k)) != 0;
        igPushID_Int(k);
        igPushStyleColor_U32(ImGuiCol_Button, on ? TH(TK_N8) : TH(TK_N5));
        igPushStyleColor_U32(ImGuiCol_Text, on ? TH(TK_N14) : TH(TK_N10));
        ImVec2 p = igGetCursorScreenPos();
        if (igSmallButton(asset_kind_name((AssetKind)k))) {
            if (igGetIO_Nil()->KeyCtrl) P.kind_on ^= AK(k); // Ctrl: this one on or off
            else P.kind_on = (P.kind_on & P.kinds_present) == AK(k) ? ~0ull : AK(k); // a click: only this one (again: all)
            P.dirty_filter = true;
        }
        igSetItemTooltip("%s", TR("Only this type (again: every type). Ctrl+click: this type on or off."));
        ImVec2 q = igGetItemRectMax();
        ImDrawList_AddRectFilled(igGetWindowDrawList(), (ImVec2){ p.x + 2, q.y - 2 }, (ImVec2){ q.x - 2, q.y },
                                 on ? TH(content_kind_color((AssetKind)k)) : TH(TK_N6), 0, 0);
        igPopStyleColor(2);
        igPopID();
        igSameLine(0, 4);
    }
    igNewLine();
}

void asset_picker_draw(void) {
    if (!P.open) return;
    double t0 = re_time_seconds();
    if (P.want_open) igOpenPopup_Str("###re_asset_picker", 0), P.want_open = false;
    if (P.dirty_sort || P.dirty_filter) refilter();
    ImGuiViewport* vp = igGetMainViewport();
    igSetNextWindowSize((ImVec2){ fminf(vp->WorkSize.x * 0.62f, igGetFontSize() * 64.0f), fminf(vp->WorkSize.y * 0.7f, igGetFontSize() * 44.0f) },
                        ImGuiCond_Appearing);
    igSetNextWindowPos((ImVec2){ vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f }, ImGuiCond_Appearing,
                       (ImVec2){ 0.5f, 0.5f });
    char wt[160];
    snprintf(wt, sizeof wt, "%s###re_asset_picker", P.title);
    bool keep = true;
    if (!igBeginPopupModal(wt, &keep, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar)) {
        P.open = false; // (closed by ImGui: Esc on a modal, its X)
        return;
    }
    if (P.hint[0]) igTextDisabled("%s", P.hint);

    // the search, the folder, the view button in the corner
    float fs = igGetFontSize();
    if (P.first_frame) igSetKeyboardFocusHere(0);
    // (a search set from outside, asset_picker_set_filter: the field, being edited, would write its own text back)
    if (P.filter_set && igGetActiveID() == igGetID_Str("##pick_search")) igClearActiveID();
    P.filter_set = false;
    igSetNextItemWidth(-fs * 16.0f);
    if (igInputTextWithHint("##pick_search", TR("Search (name or folder)"), P.filter, sizeof P.filter, 0, NULL, NULL)) P.dirty_filter = true;
    igSameLine(0, 6);
    igSetNextItemWidth(fs * 12.0f);
    if (igBeginCombo("##pick_folder", P.folder ? P.folders[P.folder] : TR("All folders"), ImGuiComboFlags_HeightLarge)) {
        if (igSelectable_Bool(TR("All folders"), P.folder == 0, 0, (ImVec2){ 0, 0 })) P.folder = 0, P.dirty_filter = true;
        for (int f = 1; f < P.nfolders; f++)
            if (igSelectable_Bool(P.folders[f], P.folder == f, 0, (ImVec2){ 0, 0 })) P.folder = f, P.dirty_filter = true;
        igEndCombo();
    }
    igSetItemTooltip("%s", TR("Only the assets in this folder (and the ones under it)."));
    igSameLine(0, 6);
    view_menu();
    type_filters();
    if (P.dirty_filter) refilter();

    // the assets: tiles or rows, only the visible ones drawn
    float footer = igGetFrameHeightWithSpacing() * 2.0f + 6.0f;
    if (igBeginChild_Str("##pick_list", (ImVec2){ 0, -footer }, ImGuiChildFlags_Borders, 0)) {
        static const float tile_fs[3] = { 4.5f, 7.0f, 11.0f }, row_fs[3] = { 1.5f, 2.2f, 3.2f };
        float avail = igGetContentRegionAvail().x, gap = 6.0f;
        float tile = fs * tile_fs[P.size];
        float cw, ch;
        int per_row;
        if (P.view == PICKER_VIEW_NAME) per_row = 1, cw = avail, ch = fs * row_fs[P.size], gap = 2.0f;
        else {
            cw = tile, ch = tile + (P.view == PICKER_VIEW_BOTH ? igGetTextLineHeight() * 2.0f + 6.0f : 0.0f);
            per_row = (int)((avail + gap) / (cw + gap));
            if (per_row < 1) per_row = 1;
        }
        float row_h = ch + gap;
        P.per_row = per_row, P.row_h = row_h;
        int rows = (P.nvis + per_row - 1) / per_row;
        ImVec2 origin = igGetCursorScreenPos();
        if (P.scroll_to_sel && P.sel >= 0) { // the current asset in view when it opens
            for (int v = 0; v < P.nvis; v++)
                if (P.vis[v] == P.sel) {
                    igSetScrollY_Float(fmaxf((float)(v / per_row) * row_h - igGetWindowHeight() * 0.4f, 0.0f));
                    break;
                }
            P.scroll_to_sel = false;
        }
        float scroll = igGetScrollY(), view_h = igGetWindowHeight();
        int r0 = (int)(scroll / row_h), r1 = (int)((scroll + view_h) / row_h) + 1;
        if (r0 < 0) r0 = 0;
        if (r1 > rows) r1 = rows;
        for (int r = r0; r < r1; r++)
            for (int c = 0; c < per_row; c++) {
                int v = r * per_row + c;
                if (v >= P.nvis) break;
                cell(P.vis[v], origin.x + c * (cw + gap), origin.y + r * row_h, cw, ch, tile);
            }
        igSetCursorScreenPos(origin);
        igDummy((ImVec2){ avail, rows ? rows * row_h : 1.0f });
        if (!P.nvis) {
            igSetCursorScreenPos((ImVec2){ origin.x + 8, origin.y + 8 });
            igTextDisabled("%s", P.n ? TR("No results.") : TR("No asset of this type in the project yet."));
        }
    }
    igEndChild();

    // the chosen one, None / Cancel / Select
    const Entry* s = P.sel >= 0 && P.sel < P.n ? &P.e[P.sel] : NULL;
    igTextDisabled(TR("%d of %d assets"), P.nvis, P.n);
    if (s) {
        igSameLine(0, 12);
        igText("%s", s->label);
        igSameLine(0, 8);
        igTextDisabled("%s", s->ref[0] == '/' ? s->ref : asset_kind_name((AssetKind)s->kind));
    }
    float bw = fs * 7.0f;
    igSetCursorPosX(igGetWindowWidth() - igGetStyle()->WindowPadding.x - bw * (P.allow_none ? 3.0f : 2.0f) - 12.0f);
    int chosen = -1;
    bool done = false;
    if (P.allow_none) {
        if (igButton(TR("None"), (ImVec2){ bw, 0 })) chosen = -1, done = true;
        igSetItemTooltip("%s", TR("Leaves the field empty."));
        igSameLine(0, 6);
    }
    if (igButton(TR("Cancel"), (ImVec2){ bw, 0 })) keep = false;
    igSameLine(0, 6);
    igBeginDisabled(!s);
    if (igButton(TR("Select"), (ImVec2){ bw, 0 }) || (s && igIsKeyPressed_Bool(ImGuiKey_Enter, false))) chosen = P.sel, done = true;
    igEndDisabled();
    if (P.dbl >= 0) chosen = P.dbl, done = true, P.dbl = -1; // (a double click)
    if (P.choose >= 0) { // (a test: the visible one picked as by a double click)
        if (P.choose < P.nvis) chosen = P.vis[P.choose], done = true;
        P.choose = -1;
    }
    if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) keep = false;
    P.first_frame = false;
    if (done || !keep) igCloseCurrentPopup();
    igEndPopup();
    if (done) pick(chosen);
    else if (!keep) P.open = false;
    P.draw_ms = (re_time_seconds() - t0) * 1000.0;
}

// --- The Details field ---

static bool g_field_open_next;
void asset_field_open_next(void) { g_field_open_next = true; }

int asset_field(const char* id, const char* shown, const char* cur_ref, int kind, uint64_t kinds, bool allow_none, char* out, size_t cap) {
    igPushID_Str(id);
    ImGuiID owner = igGetID_Str("##field");
    int ret = ASSET_FIELD_IDLE;
    float fs = igGetFontSize(), h = fs * 2.2f, w = igGetContentRegionAvail().x;
    ImVec2 p = igGetCursorScreenPos();
    ImDrawList* dl = igGetWindowDrawList();
    bool click = igInvisibleButton("##field", (ImVec2){ w > h * 3.0f ? w : h * 3.0f, h }, 0);
    bool hov = igIsItemHovered(0);
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + h }, hov ? TH(TK_N8) : TH(TK_N6), 3.0f, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2){ p.x + 1, p.y + 1 }, (ImVec2){ p.x + h - 1, p.y + h - 1 }, TH(TK_N4), 3.0f, 0);
    bool has = cur_ref && cur_ref[0];
    bool th = has && ((cur_ref[0] == '/' && content_thumb(dl, p.x + 2, p.y + 2, p.x + h - 2, p.y + h - 2, cur_ref, kind, hov)) ||
                      (!strncmp(cur_ref, "#mesh/", 6) && asset_thumb(dl, p.x + 2, p.y + 2, p.x + h - 2, p.y + h - 2, cur_ref, kind)));
    if (has && !th) {
        float is = h * 0.55f;
        ui_icon(dl, (UiIcon)content_kind_icon(kind, cur_ref), p.x + (h - is) * 0.5f, p.y + (h - is) * 0.5f, is, TH(content_kind_color((AssetKind)kind)));
    }
    float ty = p.y + (h - igGetTextLineHeight()) * 0.5f;
    ImDrawList_PushClipRect(dl, (ImVec2){ p.x + h + 4, p.y }, (ImVec2){ p.x + w - fs * 1.6f, p.y + h }, true);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + h + 6, ty }, has ? TH(TK_N14) : TH(TK_N11), shown && shown[0] ? shown : TR("None"), NULL);
    ImDrawList_PopClipRect(dl);
    ui_icon(dl, ICON_SEARCH, p.x + w - fs * 1.4f, p.y + (h - fs) * 0.5f, fs, TH(TK_N12)); // (it opens the picker)
    if (hov) igSetItemTooltip("%s%s%s", shown && shown[0] ? shown : TR("None"), has && cur_ref[0] == '/' ? "\n" : "", has && cur_ref[0] == '/' ? cur_ref : "");
    if (click || g_field_open_next) {
        g_field_open_next = false;
        AssetPickerSpec s = { .title = TR("Pick Asset"), .kinds = kinds, .current = cur_ref, .allow_none = allow_none, .owner = owner };
        char title[96];
        snprintf(title, sizeof title, TR("Pick %s"), asset_kind_name((AssetKind)kind));
        s.title = title;
        asset_picker_open(&s);
        ret = ASSET_FIELD_OPENED;
    }
    if (igBeginDragDropTarget()) { // an asset dragged from the Content Browser
        const ImGuiPayload* pl = igAcceptDragDropPayload(RE_PAYLOAD_ASSET, 0);
        if (pl && pl->DataSize == (int)sizeof(AssetPayload)) {
            const AssetPayload* ap = pl->Data;
            if ((int)ap->kind < 64 && ((kinds ? kinds : AK(kind)) & AK(ap->kind)) && out && cap) snprintf(out, cap, "%s", ap->vpath), ret = ASSET_FIELD_PICKED;
        }
        igEndDragDropTarget();
    }
    if (ret == ASSET_FIELD_IDLE && asset_picker_result(owner, out, cap, NULL)) ret = ASSET_FIELD_PICKED;
    igPopID();
    return ret;
}

// --- The tests' hooks ---

void asset_picker_set_filter(const char* text) {
    snprintf(P.filter, sizeof P.filter, "%s", text ? text : "");
    P.dirty_filter = P.filter_set = true;
    if (P.open) refilter();
}
int asset_picker_count(void) { return P.n; }
int asset_picker_visible(void) {
    if (P.open && (P.dirty_filter || P.dirty_sort)) refilter();
    return P.nvis;
}
const char* asset_picker_visible_ref(int i) { return i >= 0 && i < P.nvis ? P.e[P.vis[i]].ref : ""; }
void asset_picker_choose(int visible_index) { P.choose = visible_index; }
void asset_picker_layout(int* per_row, float* row_h) {
    if (per_row) *per_row = P.per_row;
    if (row_h) *row_h = P.row_h;
}
double asset_picker_scan_ms(void) { return P.scan_ms; }
double asset_picker_draw_ms(void) { return P.draw_ms; }
