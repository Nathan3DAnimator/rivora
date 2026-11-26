// (2026-11-25, item 18) The kit's Artist plugins (plugin_kit.h): Batch Rename, Color Palette, Material Swap, Mesh Audit.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_kit.h"

#include "content.h"
#include "i18n.h"
#include "platform.h"
#include "platform_files.h"
#include "theme.h"
#include "ui_number.h"

// ---------------------------------------------------------------------------------------------------------------
// Batch Rename: the selected actors renamed at once (a prefix, a suffix, a find and replace, or a base name numbered)
enum { RN_PREFIX, RN_SUFFIX, RN_REPLACE, RN_NUMBER };
typedef struct RenameOpts { int mode; char a[48], b[48]; int start, digits; } RenameOpts;
static RenameOpts g_rn = { RN_NUMBER, "Prop_", "", 1, 2 };

static void rename_one(const RenameOpts* o, const char* old, uint32_t index, char* out, size_t cap) {
    switch (o->mode) {
    case RN_PREFIX: snprintf(out, cap, "%s%s", o->a, old); break;
    case RN_SUFFIX: snprintf(out, cap, "%s%s", old, o->a); break;
    case RN_REPLACE: {
        size_t n = 0, la = strlen(o->a);
        out[0] = 0;
        for (const char* s = old; *s && n + 1 < cap;) {
            if (la && !strncmp(s, o->a, la)) {
                n += (size_t)snprintf(out + n, cap - n, "%s", o->b);
                s += la;
            } else out[n++] = *s++, out[n] = 0;
            if (n >= cap) n = cap - 1;
        }
        break;
    }
    default: snprintf(out, cap, "%s%0*u", o->a, o->digits < 1 ? 1 : o->digits > 6 ? 6 : o->digits, (unsigned)(o->start + (int)index)); break;
    }
    if (!out[0]) snprintf(out, cap, "%s", old); // (never an empty name)
}

static uint32_t rename_apply(Editor* ed, const RenameOpts* o) {
    if (!ed->nsel) return 0;
    editor_undo_push(ed);
    uint32_t n = 0;
    for (uint32_t i = 0; i < ed->nsel; i++) {
        CName* nm = ecs_get(&ed->world, ed->sel[i], COMP_NAME);
        if (!nm) continue;
        char fresh[RE_NAME_MAX];
        rename_one(o, nm->text, n, fresh, sizeof fresh);
        snprintf(nm->text, sizeof nm->text, "%s", fresh);
        ecs_touch(&ed->world, ed->sel[i]);
        n++;
    }
    ed->dirty = true;
    return n;
}

static void rename_window(Editor* ed, double now) {
    static const char* const modes[] = { "Add Prefix", "Add Suffix", "Find and Replace", "Base Name and Number" };
    const char* shown[4];
    for (int i = 0; i < 4; i++) shown[i] = TR(modes[i]);
    igCombo_Str_arr(TR("Rename Mode"), &g_rn.mode, shown, 4, -1);
    igInputText(g_rn.mode == RN_REPLACE ? TR("Find") : g_rn.mode == RN_NUMBER ? TR("Base Name") : TR("Text"), g_rn.a, sizeof g_rn.a, 0, NULL, NULL);
    if (g_rn.mode == RN_REPLACE) igInputText(TR("Replace With"), g_rn.b, sizeof g_rn.b, 0, NULL, NULL);
    if (g_rn.mode == RN_NUMBER) {
        ui_num_int(TR("Start Number"), &g_rn.start, 0.2f, 0, 999, 0, 0, "%d", 0);
        ui_num_int(TR("Digits"), &g_rn.digits, 0.05f, 1, 6, 1, 6, "%d", 0);
    }
    igSeparatorText(TR("Preview"));
    if (!ed->nsel) igTextDisabled("%s", TR("Select the actors to rename."));
    for (uint32_t i = 0, k = 0; i < ed->nsel && i < 12; i++) {
        const char* old = kit_actor_name(&ed->world, ed->sel[i]);
        char fresh[RE_NAME_MAX];
        rename_one(&g_rn, old, k++, fresh, sizeof fresh);
        igText("%s  ->  %s", old, fresh);
    }
    if (ed->nsel > 12) igTextDisabled(TR("... and %u more"), ed->nsel - 12);
    igBeginDisabled(!ed->nsel || ed->playing);
    if (igButton(TR("Rename Selected"), (ImVec2){ -1, 0 })) editor_status(ed, now, TR("Renamed %u actors."), rename_apply(ed, &g_rn));
    igEndDisabled();
}

static bool rename_test(Editor* ed, char* msg, size_t cap) {
    Entity a = kit_spawn_test_cube(ed, "Rock", 0, 0, 0), b = kit_spawn_test_cube(ed, "Rock", 100, 0, 0);
    editor_select(ed, a, SEL_REPLACE), editor_select(ed, b, SEL_ADD);
    RenameOpts o = { RN_NUMBER, "Cliff_", "", 7, 3 };
    rename_apply(ed, &o);
    bool num = !strcmp(kit_actor_name(&ed->world, a), "Cliff_007") && !strcmp(kit_actor_name(&ed->world, b), "Cliff_008");
    RenameOpts r = { RN_REPLACE, "Cliff", "Stone", 0, 0 };
    rename_apply(ed, &r);
    bool rep = !strcmp(kit_actor_name(&ed->world, a), "Stone_007");
    snprintf(msg, cap, "%s, %s", kit_actor_name(&ed->world, a), kit_actor_name(&ed->world, b));
    kit_destroy(ed, a), kit_destroy(ed, b);
    return num && rep;
}

const KitPlugin kit_batch_rename = { "BatchRename", "Batch Rename", "Artist", "RENAME",
    "Renames the selected actors at once: a prefix, a suffix, a find and replace, or a base name with numbers (Prop_01, Prop_02...). Undoable.",
    false, rename_window, NULL, NULL, NULL, rename_test };

// ---------------------------------------------------------------------------------------------------------------
// Color Palette: the project's colors (Config/ColorPalette.txt), applied to the selected meshes and lights, copied as hex
#define PAL_MAX 32
typedef struct PalColor { float c[3]; char name[32]; } PalColor;
static PalColor g_pal[PAL_MAX];
static int g_npal = -1; // (-1: not read yet)
static float g_pal_new[3] = { 0.8f, 0.35f, 0.1f };
static char g_pal_name[32] = "Accent";

static void pal_path(char* out, size_t cap) { kit_project_file(out, cap, "Config/ColorPalette.txt"); }
static void pal_load(const char* path) {
    g_npal = 0;
    FILE* f = re_fopen(path, "rb");
    if (!f) return;
    char line[160];
    while (g_npal < PAL_MAX && fgets(line, sizeof line, f)) {
        PalColor* p = &g_pal[g_npal];
        int used = 0;
        if (line[0] == ';' || sscanf(line, "%f %f %f %n", &p->c[0], &p->c[1], &p->c[2], &used) < 3) continue;
        snprintf(p->name, sizeof p->name, "%s", line + used);
        p->name[strcspn(p->name, "\r\n")] = 0;
        g_npal++;
    }
    fclose(f);
}
static bool pal_save(const char* path) {
    char dir[900];
    snprintf(dir, sizeof dir, "%s", path);
    char* sl = strrchr(dir, '/');
    if (sl) *sl = 0, re_make_dirs(dir);
    FILE* f = re_fopen(path, "wb");
    if (!f) return false;
    fputs("; the project's color palette (Tools > Plugins > Color Palette): r g b (linear 0..1) name\n", f);
    for (int i = 0; i < g_npal; i++) fprintf(f, "%.4f %.4f %.4f %s\n", g_pal[i].c[0], g_pal[i].c[1], g_pal[i].c[2], g_pal[i].name);
    return fclose(f) == 0;
}
static uint32_t pal_apply(Editor* ed, const float c[3]) {
    if (!ed->nsel) return 0;
    editor_undo_push(ed);
    uint32_t n = 0;
    for (uint32_t i = 0; i < ed->nsel; i++) {
        CStaticMesh* m = ecs_get(&ed->world, ed->sel[i], COMP_STATIC_MESH);
        CLight* l = ecs_get(&ed->world, ed->sel[i], COMP_LIGHT);
        if (m) memcpy(m->base_color, c, sizeof m->base_color), n++;
        if (l) memcpy(l->color, c, sizeof l->color), n++;
        if (m || l) ecs_touch(&ed->world, ed->sel[i]);
    }
    ed->dirty = true;
    return n;
}
static void pal_window(Editor* ed, double now) {
    char path[900];
    pal_path(path, sizeof path);
    if (g_npal < 0) pal_load(path);
    igTextWrapped("%s", TR("Click a swatch to give its color to the selected meshes (their Base Color) and lights. Right click: copy it as hex."));
    float sz = 34.0f;
    int del = -1;
    for (int i = 0; i < g_npal; i++) {
        igPushID_Int(i);
        if (i % 8) igSameLine(0, 6.0f);
        ImVec4 col = { g_pal[i].c[0], g_pal[i].c[1], g_pal[i].c[2], 1.0f };
        if (igColorButton(g_pal[i].name, col, ImGuiColorEditFlags_NoTooltip, (ImVec2){ sz, sz }))
            editor_status(ed, now, TR("%s applied to %u actors."), g_pal[i].name, pal_apply(ed, g_pal[i].c));
        if (igIsItemHovered(0)) igSetTooltip("%s\n%.3f %.3f %.3f", g_pal[i].name, g_pal[i].c[0], g_pal[i].c[1], g_pal[i].c[2]);
        if (igBeginPopupContextItem("pal", ImGuiPopupFlags_MouseButtonRight)) {
            if (igMenuItem_Bool(TR("Copy as Hex"), NULL, false, true)) {
                char hex[16];
                snprintf(hex, sizeof hex, "#%02X%02X%02X", (int)(g_pal[i].c[0] * 255.0f + 0.5f), (int)(g_pal[i].c[1] * 255.0f + 0.5f), (int)(g_pal[i].c[2] * 255.0f + 0.5f));
                igSetClipboardText(hex);
            }
            if (igMenuItem_Bool(TR("Remove Color"), NULL, false, true)) del = i;
            igEndPopup();
        }
        igPopID();
    }
    if (del >= 0) {
        memmove(&g_pal[del], &g_pal[del + 1], sizeof g_pal[0] * (size_t)(g_npal - del - 1)), g_npal--;
        pal_save(path);
    }
    if (!g_npal) igTextDisabled("%s", TR("No colors yet: add the first one below."));
    igSeparatorText(TR("New Color"));
    igColorEdit3(TR("Color"), g_pal_new, ImGuiColorEditFlags_Float);
    igInputText(TR("Color Name"), g_pal_name, sizeof g_pal_name, 0, NULL, NULL);
    igBeginDisabled(g_npal >= PAL_MAX);
    if (igButton(TR("Add to Palette"), (ImVec2){ 0, 0 })) {
        PalColor* p = &g_pal[g_npal++];
        memcpy(p->c, g_pal_new, sizeof p->c);
        snprintf(p->name, sizeof p->name, "%s", g_pal_name[0] ? g_pal_name : "Color");
        pal_save(path);
    }
    igEndDisabled();
    igSameLine(0, -1);
    igBeginDisabled(ed->selected == ENTITY_NONE);
    if (igButton(TR("Pick from Selected"), (ImVec2){ 0, 0 })) {
        CStaticMesh* m = ecs_get(&ed->world, ed->selected, COMP_STATIC_MESH);
        CLight* l = ecs_get(&ed->world, ed->selected, COMP_LIGHT);
        if (m) memcpy(g_pal_new, m->base_color, sizeof g_pal_new);
        else if (l) memcpy(g_pal_new, l->color, sizeof g_pal_new);
    }
    igEndDisabled();
    igTextDisabled("%s", path);
}
static bool pal_test(Editor* ed, char* msg, size_t cap) {
    char path[900];
    snprintf(path, sizeof path, "%s/kit_palette_test.txt", content_root());
    int keep_n = g_npal;
    PalColor keep[PAL_MAX];
    memcpy(keep, g_pal, sizeof keep);
    g_npal = 2;
    g_pal[0] = (PalColor){ { 1.0f, 0.5f, 0.25f }, "Rust" };
    g_pal[1] = (PalColor){ { 0.1f, 0.2f, 0.9f }, "Deep Blue" };
    bool saved = pal_save(path);
    g_npal = 0;
    pal_load(path);
    remove(path);
    bool read = g_npal == 2 && !strcmp(g_pal[1].name, "Deep Blue") && g_pal[0].c[1] == 0.5f;
    Entity e = kit_spawn_test_cube(ed, "PalCube", 0, 0, 0);
    editor_select(ed, e, SEL_REPLACE);
    pal_apply(ed, g_pal[1].c);
    CStaticMesh* m = ecs_get(&ed->world, e, COMP_STATIC_MESH);
    bool applied = m && m->base_color[2] == 0.9f;
    kit_destroy(ed, e);
    snprintf(msg, cap, "2 colors saved and read back, Deep Blue on a cube");
    memcpy(g_pal, keep, sizeof keep), g_npal = keep_n;
    return saved && read && applied;
}
static void pal_off(Editor* ed) { (void)ed, g_npal = -1; }

const KitPlugin kit_color_palette = { "ColorPalette", "Color Palette", "Artist", "THEME",
    "The project's own colors in one place (Config/ColorPalette.txt): one click gives a color to the selected meshes and lights; copy it as hex for the textures.",
    false, pal_window, NULL, NULL, pal_off, pal_test };

// ---------------------------------------------------------------------------------------------------------------
// Material Swap: every actor with one material given another (the selection or the whole level)
static int g_ms_from = -1, g_ms_to = -1;
static bool g_ms_sel_only;

static uint32_t material_swap(Editor* ed, uint32_t from, uint32_t to, bool sel_only) {
    World* w = &ed->world;
    uint32_t n = 0;
    bool pushed = false;
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        CStaticMesh* m = e ? ecs_get(w, e, COMP_STATIC_MESH) : NULL;
        if (!m || m->material != from || (sel_only && !editor_is_selected(ed, e))) continue;
        if (!pushed) editor_undo_push(ed), pushed = true;
        m->material = to;
        ecs_touch(w, e);
        n++;
    }
    if (n) ed->dirty = true;
    return n;
}
static const char* mat_label(uint32_t h) {
    if (!h) return TR("None (the mesh's colors)");
    const char* vp = kit_asset_by_hash(h);
    return vp[0] ? kit_file_name(vp) : TR("(unknown material)");
}
static void swap_window(Editor* ed, double now) {
    const KitAsset* as;
    uint32_t na = kit_assets(&as, igIsWindowAppearing());
    // the materials in use (and how many actors each), then every material of the project
    static uint32_t used[256], count[256];
    uint32_t nused = 0;
    World* w = &ed->world;
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        CStaticMesh* m = e ? ecs_get(w, e, COMP_STATIC_MESH) : NULL;
        if (!m) continue;
        uint32_t k = 0;
        while (k < nused && used[k] != m->material) k++;
        if (k == nused && nused < 256) used[nused] = m->material, count[nused++] = 0;
        if (k < nused) count[k]++;
    }
    igSeparatorText(TR("Replace"));
    char prev[160];
    snprintf(prev, sizeof prev, "%s", g_ms_from >= 0 && (uint32_t)g_ms_from < nused ? mat_label(used[g_ms_from]) : TR("(pick a material in use)"));
    if (igBeginCombo(TR("Material in Use"), prev, 0)) {
        for (uint32_t k = 0; k < nused; k++) {
            char l[200];
            snprintf(l, sizeof l, "%s  (%u)##u%u", mat_label(used[k]), count[k], k);
            if (igSelectable_Bool(l, g_ms_from == (int)k, 0, (ImVec2){ 0, 0 })) g_ms_from = (int)k;
        }
        igEndCombo();
    }
    snprintf(prev, sizeof prev, "%s", g_ms_to >= 0 && (uint32_t)g_ms_to < na ? kit_file_name(as[g_ms_to].vpath) : g_ms_to == -2 ? TR("None (the mesh's colors)") : TR("(pick the new material)"));
    if (igBeginCombo(TR("New Material"), prev, ImGuiComboFlags_HeightLarge)) {
        if (igSelectable_Bool(TR("None (the mesh's colors)"), g_ms_to == -2, 0, (ImVec2){ 0, 0 })) g_ms_to = -2;
        for (uint32_t k = 0; k < na; k++) {
            if (as[k].kind != ASSET_MATERIAL && as[k].kind != ASSET_MATERIAL_INSTANCE) continue;
            igPushID_Int((int)k);
            if (igSelectable_Bool(kit_file_name(as[k].vpath), g_ms_to == (int)k, 0, (ImVec2){ 0, 0 })) g_ms_to = (int)k;
            if (igIsItemHovered(0)) igSetTooltip("%s", as[k].vpath);
            igPopID();
        }
        igEndCombo();
    }
    igCheckbox(TR("Only the Selected Actors"), &g_ms_sel_only);
    bool can = g_ms_from >= 0 && (uint32_t)g_ms_from < nused && (g_ms_to == -2 || (g_ms_to >= 0 && (uint32_t)g_ms_to < na)) && !ed->playing;
    igBeginDisabled(!can);
    if (igButton(TR("Swap Materials"), (ImVec2){ -1, 0 })) {
        uint32_t to = g_ms_to == -2 ? 0 : as[g_ms_to].hash;
        editor_status(ed, now, TR("Material swapped on %u actors."), material_swap(ed, used[g_ms_from], to, g_ms_sel_only));
    }
    igEndDisabled();
}
static bool swap_test(Editor* ed, char* msg, size_t cap) {
    Entity a = kit_spawn_test_cube(ed, "SwapA", 0, 0, 0), b = kit_spawn_test_cube(ed, "SwapB", 100, 0, 0), c = kit_spawn_test_cube(ed, "SwapC", 200, 0, 0);
    CStaticMesh* ma = ecs_get(&ed->world, a, COMP_STATIC_MESH);
    CStaticMesh* mb = ecs_get(&ed->world, b, COMP_STATIC_MESH);
    CStaticMesh* mc = ecs_get(&ed->world, c, COMP_STATIC_MESH);
    if (!ma || !mb || !mc) return false;
    ma->material = mb->material = 0xA11CE5u, mc->material = 0x5EAu;
    editor_select(ed, a, SEL_REPLACE);
    uint32_t n_sel = material_swap(ed, 0xA11CE5u, 0xB0Bu, true);
    uint32_t n_all = material_swap(ed, 0xA11CE5u, 0xB0Bu, false);
    ma = ecs_get(&ed->world, a, COMP_STATIC_MESH), mb = ecs_get(&ed->world, b, COMP_STATIC_MESH), mc = ecs_get(&ed->world, c, COMP_STATIC_MESH);
    bool ok = n_sel == 1 && n_all == 1 && ma->material == 0xB0Bu && mb->material == 0xB0Bu && mc->material == 0x5EAu;
    snprintf(msg, cap, "%u selected, then %u more in the level; the other material kept", n_sel, n_all);
    kit_destroy(ed, a), kit_destroy(ed, b), kit_destroy(ed, c);
    return ok;
}
static void swap_off(Editor* ed) { (void)ed, g_ms_from = g_ms_to = -1; }

const KitPlugin kit_material_swap = { "MaterialSwap", "Material Swap", "Artist", "MATERIAL",
    "Every actor using one material gets another, in the whole level or only the selection: re-skin a level in one click. Undoable.",
    false, swap_window, NULL, NULL, swap_off, swap_test };

// ---------------------------------------------------------------------------------------------------------------
// Mesh Audit: the level's meshes, how many actors use each, the shadow casters and the ones without a material
typedef struct MeshRow { uint32_t mesh, actors, shadows, no_mat, hidden; } MeshRow;
static MeshRow* g_mr;
static uint32_t g_nmr, g_cap_mr;

static uint32_t mesh_audit(Editor* ed) {
    World* w = &ed->world;
    g_nmr = 0;
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        CStaticMesh* m = e ? ecs_get(w, e, COMP_STATIC_MESH) : NULL;
        if (!m) continue;
        uint32_t k = 0;
        while (k < g_nmr && g_mr[k].mesh != m->mesh) k++;
        if (k == g_nmr) {
            if (g_nmr == g_cap_mr) {
                uint32_t nc = g_cap_mr ? g_cap_mr * 2 : 64;
                MeshRow* r = kit_realloc(g_mr, sizeof(MeshRow) * g_cap_mr, sizeof(MeshRow) * nc);
                if (!r) return g_nmr;
                g_mr = r, g_cap_mr = nc;
            }
            g_mr[g_nmr++] = (MeshRow){ m->mesh, 0, 0, 0, 0 };
        }
        MeshRow* r = &g_mr[k];
        r->actors++;
        r->shadows += m->cast_shadow != 0;
        r->no_mat += m->material == 0;
        const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        r->hidden += t && (t->flags & TRANSFORM_HIDDEN);
    }
    return g_nmr;
}
static const char* mesh_label(uint32_t mesh) {
    static const char* const shapes[SHAPE_COUNT] = { "Cube", "Sphere", "Cylinder", "Cone", "Plane" };
    if (mesh < SHAPE_COUNT) return shapes[mesh];
    const char* vp = kit_asset_by_hash(mesh);
    return vp[0] ? kit_file_name(vp) : "?";
}
static void audit_window(Editor* ed, double now) {
    (void)now;
    static double at;
    if (igIsWindowAppearing() || re_time_seconds() - at > 0.5) mesh_audit(ed), at = re_time_seconds(), kit_assets(NULL, igIsWindowAppearing());
    uint32_t actors = 0;
    for (uint32_t k = 0; k < g_nmr; k++) actors += g_mr[k].actors;
    igText(TR("%u meshes on %u actors"), g_nmr, actors);
    igTextDisabled("%s", TR("A mesh on many actors is cheap (instanced); one used once may be worth merging. Click a row to select its actors."));
    if (igBeginTable("meshes", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable, (ImVec2){ 0, 0 }, 0)) {
        igTableSetupScrollFreeze(0, 1);
        igTableSetupColumn(TR("Mesh"), 0, 0, 0);
        igTableSetupColumn(TR("Actors"), ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_DefaultSort, 0, 1);
        igTableSetupColumn(TR("Shadows"), 0, 0, 2);
        igTableSetupColumn(TR("No Material"), 0, 0, 3);
        igTableSetupColumn(TR("Hidden"), 0, 0, 4);
        igTableHeadersRow();
        ImGuiTableSortSpecs* ss = igTableGetSortSpecs();
        if (ss && ss->SpecsCount) { // (a small insertion sort: a few hundred rows at most)
            int col = ss->Specs[0].ColumnUserID;
            bool desc = ss->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            for (uint32_t i = 1; i < g_nmr; i++) {
                MeshRow r = g_mr[i];
                uint32_t j = i;
                for (; j > 0; j--) {
                    const uint32_t* va = &g_mr[j - 1].mesh;
                    uint32_t a = col == 0 ? 0 : va[col], b = col == 0 ? 0 : (&r.mesh)[col];
                    bool before = col == 0 ? strcmp(mesh_label(g_mr[j - 1].mesh), mesh_label(r.mesh)) > 0 : desc ? a < b : a > b;
                    if (!before) break;
                    g_mr[j] = g_mr[j - 1];
                }
                g_mr[j] = r;
            }
        }
        for (uint32_t k = 0; k < g_nmr; k++) {
            const MeshRow* r = &g_mr[k];
            igTableNextRow(0, 0);
            igTableNextColumn();
            igPushID_Int((int)k);
            if (igSelectable_Bool(mesh_label(r->mesh), false, ImGuiSelectableFlags_SpanAllColumns, (ImVec2){ 0, 0 })) {
                World* w = &ed->world;
                editor_select_none(ed);
                for (uint32_t s = 1; s < w->slot_count; s++) {
                    Entity e = ecs_entity_at(w, s);
                    CStaticMesh* m = e ? ecs_get(w, e, COMP_STATIC_MESH) : NULL;
                    if (m && m->mesh == r->mesh) editor_select(ed, e, SEL_ADD);
                }
            }
            igPopID();
            igTableNextColumn(), igText("%u", r->actors);
            igTableNextColumn(), igText("%u", r->shadows);
            igTableNextColumn(), r->no_mat ? igTextColored(THV(TK_WARN), "%u", r->no_mat) : igTextDisabled("0");
            igTableNextColumn(), igText("%u", r->hidden);
        }
        igEndTable();
    }
}
static bool audit_test(Editor* ed, char* msg, size_t cap) {
    Entity e[3] = { kit_spawn_test_cube(ed, "AuditA", 0, 0, 0), kit_spawn_test_cube(ed, "AuditB", 100, 0, 0), kit_spawn_test_cube(ed, "AuditC", 200, 0, 0) };
    CStaticMesh* m = ecs_get(&ed->world, e[2], COMP_STATIC_MESH);
    if (m) m->mesh = SHAPE_SPHERE, m->material = 77;
    mesh_audit(ed);
    uint32_t cubes = 0, spheres_mat = 0;
    for (uint32_t k = 0; k < g_nmr; k++) {
        if (g_mr[k].mesh == SHAPE_CUBE) cubes = g_mr[k].actors;
        if (g_mr[k].mesh == SHAPE_SPHERE) spheres_mat = g_mr[k].actors - g_mr[k].no_mat;
    }
    snprintf(msg, cap, "%u meshes, cubes on %u actors", g_nmr, cubes);
    for (int i = 0; i < 3; i++) kit_destroy(ed, e[i]);
    return cubes >= 2 && spheres_mat >= 1;
}
static void audit_off(Editor* ed) {
    (void)ed;
    kit_free(g_mr, sizeof(MeshRow) * g_cap_mr);
    g_mr = NULL, g_nmr = g_cap_mr = 0;
}

const KitPlugin kit_mesh_audit = { "MeshAudit", "Mesh Audit", "Artist", "STATIC_MESH",
    "The level's meshes in a table: how many actors use each, how many cast shadows, which have no material, which are hidden. A click selects them.",
    false, audit_window, NULL, NULL, audit_off, audit_test };
