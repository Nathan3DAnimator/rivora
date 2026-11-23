#include "content.h"
#include "editor_look.h"
#include "theme.h"
#include "source_control.h"
#include "particle_editor.h"
#include "ikrig_editor.h"
#include "vehicle_editor.h"
#include "dialogue.h"
#include "quest.h"
#include "physmat.h"
#include "camshake.h"
#include "pcg.h"
#include "particles.h"
#include "udim.h" // (Q18) a UDIM set imported into one .rudim
#include "async.h"
void tlend_import_set_mirror(const char* src_root, const char* dst_vroot); // (2026-11-23: a dropped folder's, tlend_import.c)

#include <ctype.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "blueprint.h"
#include "bp_editor.h"
#include "bp_preview.h"
#include "mat_editor.h"
#include "asset_viewer.h"
#include "anim_editor.h"
#include "ai_editor.h"
#include "sequencer_ui.h"
#include "audio_editor.h"
#include "script.h"
#include "script_ide.h"
#include "input_editor.h"
#include "components.h"
#include "i18n.h"
#include "level.h"
#include "mem.h"
#include "anim.h"
#include "plugins.h"
#include "platform.h"
#include "platform_files.h"
#include "ui_icons.h"

#define MAX_ITEMS 4096
#define MAX_FOLDERS 1024
#define MAX_HISTORY 32
#define VPATH 512

typedef struct Item {
    char vpath[VPATH];
    uint16_t name_off; // the name inside vpath
    uint8_t kind;      // AssetKind
    uint8_t shape;
    uint8_t bp_state;  // a Blueprint asset: 0 not looked at, 1 loaded (bp_load finds it in memory), 2 unreadable
    uint64_t size;
    int64_t mtime;
} Item;

typedef struct Folder {
    char vpath[VPATH];
    uint16_t name_off;
    uint16_t depth;
    uint16_t has_children;
} Folder;

static struct {
    char root[VPATH];
    char cur[VPATH];
    char history[MAX_HISTORY][VPATH];
    int hist_n, hist_i;
    ReVmBuf mem; // items, folders and the selection, reserved once
    Item* items;
    Folder* folders;
    bool* sel;
    int nitems, nfolders, anchor;
    bool dirty, tree_dirty;
    char filter[128];
    float tile;
    int renaming;
    char rename_buf[128];
    char pending_rename[128]; // after a rescan: start renaming this item (a new folder)
    bool rename_focus, rename_live; // the rename field asks for the keyboard until ImGui activates it (a frame later)
    int rename_asked;
    bool ask_delete;
    char message[256];
    bool focused;
} S;

static const char* const g_kind_names[ASSET_KIND_COUNT] = {
    "Folder", "Static Mesh", "Real Asset", "Level", "Texture Source", "Mesh Source", "Sound Source", "Blueprint Class",
    "Structure", "Enumeration", "Blueprint Interface", "Data Asset", "Material", "Material Instance", "Skeletal Mesh",
    "Animation Sequence", "Anim Montage", "Static Mesh", "Behavior Tree", "Real Brain", "Input Action", "Input Mapping Context",
    "Blueprint Macro Library", "Level Sequence", "Sound Wave", "Sound Cue", "Sound Attenuation", "Sound Class", "Sound Mix",
    "Sound Concurrency", "Submix", "Script", "Particle System", "IK Rig", "Physics Asset", "Vehicle", "Dialogue", "Quest", "Sound Graph", "Physical Material", "Camera Shake", "PCG Graph", "File"
};
// The asset families' colors (theme tokens: the bar under a tile, the icon)
static const uint8_t g_kind_colors[ASSET_KIND_COUNT] = {
    TK_ASSET_FOLDER, TK_ASSET_MESH, TK_ASSET_DATA, TK_ASSET_LEVEL, TK_ASSET_TEXTURE, TK_ASSET_MESH, TK_ASSET_AUDIO, TK_ASSET_BLUEPRINT,
    TK_ASSET_DATA, TK_ASSET_DATA, TK_ASSET_BLUEPRINT, TK_ASSET_DATA, TK_ASSET_MATERIAL, TK_ASSET_MATERIAL, TK_ASSET_ANIMATION,
    TK_ASSET_ANIMATION, TK_ASSET_ANIMATION, TK_ASSET_MESH, TK_ASSET_DATA, TK_ASSET_DATA, TK_ASSET_DATA, TK_ASSET_DATA,
    TK_ASSET_BLUEPRINT, TK_ASSET_LEVEL,
    TK_ASSET_AUDIO, TK_ASSET_AUDIO, TK_ASSET_AUDIO, TK_ASSET_AUDIO, TK_ASSET_AUDIO, TK_ASSET_AUDIO, TK_ASSET_AUDIO, // (the sounds)
    TK_ASSET_BLUEPRINT, // (a script: code)
    TK_ASSET_FX,        // (a particle system)
    TK_ASSET_ANIMATION, TK_LIME, // (an IK Rig: the Skeletal Mesh's; a Physics Asset: green)
    TK_ASSET_MESH,      // (a vehicle)
    TK_ASSET_UI,        // (a dialogue)
    TK_ASSET_UI,        // (a quest)
    TK_ASSET_AUDIO,     // (a MetaSound)
    TK_LIME,            // (a Physical Material: the physics' green)
    TK_ASSET_FX,        // (Q12: a Camera Shake)
    TK_LIME,            // (Q15: a PCG Graph: the plants' green)
    TK_ASSET_DATA,
};

const char* asset_kind_name(AssetKind k) { return k < ASSET_KIND_COUNT ? TR(g_kind_names[k]) : "?"; }
const char* content_root(void) { return S.root; }
void content_mark_dirty(void) { S.dirty = S.tree_dirty = true; }
bool content_focused(void) { return S.focused; }

static bool is_engine(const char* vpath) { return strncmp(vpath, "/Engine", 7) == 0; }
static bool is_content(const char* vpath) { return strncmp(vpath, "/Content", 8) == 0; }

// "/Content/Props/Rock.png" -> "<root>/Props/Rock.png"
static void to_abs(const char* vpath, char* out, size_t cap) {
    if (is_content(vpath)) snprintf(out, cap, "%s%s", S.root, vpath + 8);
    else snprintf(out, cap, "%s", vpath);
}

static void mesh_mat_path(const char* mesh_disk, char* out, size_t cap) {
    const char* sl = strrchr(mesh_disk, '/');
    const char* bs = strrchr(mesh_disk, '\\');
    if (bs && (!sl || bs > sl)) sl = bs;
    if (sl) snprintf(out, cap, "%.*s/.%s.mat", (int)(sl - mesh_disk), mesh_disk, sl + 1);
    else snprintf(out, cap, ".%s.mat", mesh_disk);
}
void content_mesh_material_set_disk(const char* mesh_disk_path, uint32_t mat_hash) {
    char p[1100];
    mesh_mat_path(mesh_disk_path, p, sizeof p);
    if (!mat_hash) {
        remove(p);
        return;
    }
    FILE* f = re_fopen(p, "wb");
    if (!f) return;
    fprintf(f, "%08x\n", mat_hash);
    fclose(f);
}
uint32_t content_mesh_material(const char* mesh_vpath) {
    char d[1000], p[1100];
    to_abs(mesh_vpath, d, sizeof d);
    mesh_mat_path(d, p, sizeof p);
    FILE* f = re_fopen(p, "rb");
    if (!f) return 0;
    unsigned int h = 0;
    if (fscanf(f, "%x", &h) != 1) h = 0;
    fclose(f);
    return h;
}

static const char* base_name(const char* p) {
    const char* s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static AssetKind kind_of(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return ASSET_OTHER;
    char ext[16];
    size_t n = 0;
    for (const char* c = dot + 1; *c && n < sizeof ext - 1; c++) ext[n++] = (char)tolower((unsigned char)*c);
    ext[n] = 0;
    if (!strcmp(ext, "rasset")) return ASSET_RASSET;
    if (!strcmp(ext, "rlevel")) return ASSET_LEVEL;
    if (!strcmp(ext, "rbp")) return ASSET_BLUEPRINT;
    if (!strcmp(ext, "rstruct")) return ASSET_BP_STRUCT;
    if (!strcmp(ext, "renum")) return ASSET_BP_ENUM;
    if (!strcmp(ext, "rinterface")) return ASSET_BP_INTERFACE;
    if (!strcmp(ext, "rdata")) return ASSET_DATA_ASSET;
    if (!strcmp(ext, "rmat")) return ASSET_MATERIAL;
    if (!strcmp(ext, "rmi")) return ASSET_MATERIAL_INSTANCE;
    if (!strcmp(ext, "rskm")) return ASSET_SKELETAL_MESH;
    if (!strcmp(ext, "ranim")) return ASSET_ANIMATION;
    if (!strcmp(ext, "rmontage")) return ASSET_ANIM_MONTAGE;
    if (!strcmp(ext, "rbt")) return ASSET_BEHAVIOR_TREE;
    if (!strcmp(ext, "rmesh")) return ASSET_STATIC_MESH;
    if (!strcmp(ext, "rbrain")) return ASSET_BRAIN;
    if (!strcmp(ext, "rinputaction")) return ASSET_INPUT_ACTION;
    if (!strcmp(ext, "rimc")) return ASSET_INPUT_CONTEXT;
    if (!strcmp(ext, "rmacros")) return ASSET_MACRO_LIB;
    if (!strcmp(ext, "rseq")) return ASSET_LEVEL_SEQUENCE;
    if (!strcmp(ext, "rsound")) return ASSET_SOUND_WAVE;
    if (!strcmp(ext, "rscue")) return ASSET_SOUND_CUE;
    if (!strcmp(ext, "rmsnd")) return ASSET_METASOUND;
    if (!strcmp(ext, "rsatt")) return ASSET_SOUND_ATTENUATION;
    if (!strcmp(ext, "rsclass")) return ASSET_SOUND_CLASS;
    if (!strcmp(ext, "rsmix")) return ASSET_SOUND_MIX;
    if (!strcmp(ext, "rsconc")) return ASSET_SOUND_CONCURRENCY;
    if (!strcmp(ext, "rsubmix")) return ASSET_SUBMIX;
    if (!strcmp(ext, "real") || !strcmp(ext, "cs") || !strcmp(ext, "py") || !strcmp(ext, "c")) return ASSET_SCRIPT;
    if (!strcmp(ext, "rparticle")) return ASSET_PARTICLE_SYSTEM;
    if (!strcmp(ext, "rikrig")) return ASSET_IK_RIG;
    if (!strcmp(ext, "rphys")) return ASSET_PHYSICS_ASSET;
    if (!strcmp(ext, "rphysmat")) return ASSET_PHYS_MATERIAL;
    if (!strcmp(ext, "rshake")) return ASSET_CAMERA_SHAKE; // (Q12)
    if (!strcmp(ext, "rpcg")) return ASSET_PCG_GRAPH; // (Q15)
    if (!strcmp(ext, "rvehicle")) return ASSET_VEHICLE;
    if (!strcmp(ext, "rdlg")) return ASSET_DIALOGUE;
    if (!strcmp(ext, "rquest")) return ASSET_QUEST;
    if (!strcmp(ext, "rudim")) return ASSET_TEXTURE_SOURCE; // (Q18) a UDIM texture: a texture like the others (its first tile's preview)
    if (!strcmp(ext, "png") || !strcmp(ext, "jpg") || !strcmp(ext, "jpeg") || !strcmp(ext, "tga") ||
        !strcmp(ext, "exr") || !strcmp(ext, "hdr") || !strcmp(ext, "bmp") || !strcmp(ext, "dds") ||
        !strcmp(ext, "tif") || !strcmp(ext, "tiff") || !strcmp(ext, "gif")) // (Q23: TIFF and GIF too, as the viewer and WIC read them)
        return ASSET_TEXTURE_SOURCE;
    if (!strcmp(ext, "fbx") || !strcmp(ext, "obj") || !strcmp(ext, "gltf") || !strcmp(ext, "glb") || !strcmp(ext, "usd"))
        return ASSET_MESH_SOURCE;
    if (!strcmp(ext, "wav") || !strcmp(ext, "ogg") || !strcmp(ext, "mp3") || !strcmp(ext, "flac") || !strcmp(ext, "m4a") ||
        !strcmp(ext, "aac") || !strcmp(ext, "wma") || !strcmp(ext, "opus"))
        return ASSET_SOUND_SOURCE;
    return ASSET_OTHER;
}

// A Blueprint asset of the folder (bp_load's cache; an unreadable file is not read again every frame)
static BpAsset* item_bp(Item* it) {
    if (it->kind != ASSET_BLUEPRINT || it->bp_state == 2) return NULL;
    BpAsset* a = bp_load(it->vpath);
    it->bp_state = a ? 1 : 2;
    return a;
}

// A class's icon by what it is at its root (Unreal's: Actor, Pawn, Character, Game Mode)
static UiIcon class_icon(const BpAsset* a) {
    switch (a ? bp_native_of(a) : BPN_ACTOR) {
    case BPN_CHARACTER: return ICON_CHARACTER;
    case BPN_PAWN: return ICON_PAWN;
    case BPN_GAME_MODE: return ICON_GAMEMODE;
    default: return ICON_BLUEPRINT;
    }
}

static UiIcon kind_icon(Item* it) { // (Q5) every kind its own icon (ui_icons.c ui_asset_icon); a Blueprint by its parent
    switch (it->kind) {
    case ASSET_BASIC_SHAPE: return (UiIcon)(ICON_CUBE + it->shape);
    case ASSET_BLUEPRINT: return class_icon(item_bp(it));
    default: return ui_asset_icon(it->kind);
    }
}

static bool name_matches(const char* name, const char* filter) {
    if (!filter[0]) return true;
    for (const char* s = name; *s; s++) {
        size_t k = 0;
        while (filter[k] && s[k] && tolower((unsigned char)s[k]) == tolower((unsigned char)filter[k])) k++;
        if (!filter[k]) return true;
    }
    return false;
}

// --- Reading the folders ---

static void add_item(const char* dir_vpath, const char* name, AssetKind kind, int shape, uint64_t size, int64_t mtime) {
    if (S.nitems >= MAX_ITEMS) return;
    Item* it = &S.items[S.nitems++];
    snprintf(it->vpath, VPATH, "%s/%s", dir_vpath, name);
    it->name_off = (uint16_t)(strlen(it->vpath) - strlen(name));
    it->kind = (uint8_t)kind;
    it->shape = (uint8_t)shape;
    it->bp_state = 0;
    it->size = size;
    it->mtime = mtime;
}

typedef struct WalkCtx {
    char vpath[VPATH];
    int depth;
    bool recursive;
} WalkCtx;

static void walk_one(void* user, const ReDirEntry* e) {
    WalkCtx* c = user;
    if (e->name[0] == '.') return; // hidden: .git, .vs
    const char* ext = e->is_dir ? NULL : strrchr(e->name, '.');
    if (ext && (!_stricmp(ext, ".rlevelbp") || !_stricmp(ext, ".rlight") || !_stricmp(ext, ".tmp"))) return; // a level's parts (Level Blueprint, baked light)
    if (name_matches(e->name, S.filter) || !c->recursive)
        add_item(c->vpath, e->name, e->is_dir ? ASSET_FOLDER : kind_of(e->name), 0, e->size, e->mtime);
    if (c->recursive && e->is_dir && c->depth < 16) {
        WalkCtx sub = { "", c->depth + 1, true };
        snprintf(sub.vpath, VPATH, "%s/%s", c->vpath, e->name);
        char abs[VPATH + 64];
        to_abs(sub.vpath, abs, sizeof abs);
        re_dir_walk(abs, walk_one, &sub);
    }
}

static int item_cmp(const void* a, const void* b) {
    const Item *x = a, *y = b;
    if ((x->kind == ASSET_FOLDER) != (y->kind == ASSET_FOLDER)) return x->kind == ASSET_FOLDER ? -1 : 1;
    return _stricmp(x->vpath + x->name_off, y->vpath + y->name_off);
}

static void scan_items(void) {
    S.nitems = 0;
    bool search = S.filter[0] != 0;
    if (is_engine(S.cur)) {
        if (!strcmp(S.cur, "/Engine")) add_item("/Engine", "BasicShapes", ASSET_FOLDER, 0, 0, 0);
        else if (!strcmp(S.cur, "/Engine/BasicShapes"))
            for (int s = 0; s < SHAPE_COUNT; s++)
                if (name_matches(g_shape_names[s], S.filter)) add_item(S.cur, g_shape_names[s], ASSET_BASIC_SHAPE, s, 0, 0);
    } else {
        WalkCtx c = { "", 0, search };
        snprintf(c.vpath, VPATH, "%s", S.cur);
        char abs[VPATH + 64];
        to_abs(S.cur, abs, sizeof abs);
        if (!re_dir_walk(abs, walk_one, &c) && strcmp(S.cur, "/Content") != 0) {
            snprintf(S.cur, VPATH, "/Content"); // the folder is gone (deleted outside): back to the root
            scan_items();
            return;
        }
    }
    qsort(S.items, (size_t)S.nitems, sizeof *S.items, item_cmp);
    memset(S.sel, 0, sizeof(bool) * MAX_ITEMS);
    S.renaming = -1;
    if (S.pending_rename[0]) {
        for (int i = 0; i < S.nitems; i++)
            if (!strcmp(S.items[i].vpath + S.items[i].name_off, S.pending_rename)) {
                S.renaming = i;
                S.sel[i] = true;
                snprintf(S.rename_buf, sizeof S.rename_buf, "%s", S.pending_rename);
                S.rename_focus = true, S.rename_live = false, S.rename_asked = 0;
            }
        S.pending_rename[0] = 0;
    }
    S.dirty = false;
}

static void tree_walk(const char* vpath, int depth);

static void tree_one(void* user, const ReDirEntry* e) {
    const WalkCtx* c = user;
    if (!e->is_dir || e->name[0] == '.' || S.nfolders >= MAX_FOLDERS) return;
    char sub[VPATH];
    snprintf(sub, VPATH, "%s/%s", c->vpath, e->name);
    tree_walk(sub, c->depth);
}

static void tree_walk(const char* vpath, int depth) {
    if (S.nfolders >= MAX_FOLDERS) return;
    int me = S.nfolders++;
    Folder* f = &S.folders[me];
    snprintf(f->vpath, VPATH, "%s", vpath);
    f->name_off = (uint16_t)(base_name(f->vpath) - f->vpath);
    f->depth = (uint16_t)depth;
    f->has_children = 0;
    if (is_engine(vpath)) return;
    WalkCtx c = { "", depth + 1, false };
    snprintf(c.vpath, VPATH, "%s", vpath);
    char abs[VPATH + 64];
    to_abs(vpath, abs, sizeof abs);
    re_dir_walk(abs, tree_one, &c);
    S.folders[me].has_children = S.nfolders > me + 1;
}

static void scan_tree(void) {
    S.nfolders = 0;
    tree_walk("/Content", 0);
    int engine = S.nfolders;
    tree_walk("/Engine", 0);
    tree_walk("/Engine/BasicShapes", 1);
    S.folders[engine].has_children = 1;
    S.tree_dirty = false;
}

// --- Navigation and file operations ---

static void navigate(const char* vpath, bool record) {
    if (!strcmp(vpath, S.cur) && !S.filter[0]) return;
    snprintf(S.cur, VPATH, "%s", vpath);
    S.filter[0] = 0;
    if (record) {
        if (S.hist_n == 0 || strcmp(S.history[S.hist_i], vpath) != 0) {
            S.hist_i = S.hist_n ? S.hist_i + 1 : 0;
            if (S.hist_i >= MAX_HISTORY) {
                memmove(S.history[0], S.history[1], sizeof S.history[0] * (MAX_HISTORY - 1));
                S.hist_i = MAX_HISTORY - 1;
            }
            snprintf(S.history[S.hist_i], VPATH, "%s", vpath);
            S.hist_n = S.hist_i + 1;
        }
    }
    S.dirty = true;
}

// Browse to an asset (the Blueprint Editor's Browse): its folder shown, the asset selected once the folder is read
static char g_browse_select[VPATH];
static const char* g_test_hover; // --hover: the browsed asset as if under the pointer (the animated thumbnail's test)
void content_test_hover(const char* vpath) { g_test_hover = vpath; }

void content_browse_to(const char* vpath) {
    const char* s = strrchr(vpath, '/');
    if (!s) return;
    char folder[VPATH];
    snprintf(folder, sizeof folder, "%.*s", (int)(s - vpath), vpath);
    S.filter[0] = 0;
    navigate(folder, true);
    snprintf(g_browse_select, VPATH, "%s", vpath);
    S.dirty = true;
}

static void go_history(int d) {
    int i = S.hist_i + d;
    if (i < 0 || i >= S.hist_n) return;
    S.hist_i = i;
    navigate(S.history[i], false);
}

static void message(const char* fmt, const char* arg) {
    snprintf(S.message, sizeof S.message, fmt, arg);
    printf("[RealEngine] Content: %s\n", S.message);
    fflush(stdout);
}

static bool valid_name(const char* n) {
    if (!n[0] || !strcmp(n, ".") || !strcmp(n, "..")) return false;
    for (const char* c = n; *c; c++)
        if (strchr("\\/:*?\"<>|", *c)) return false;
    return true;
}

static void new_folder(void) {
    if (!is_content(S.cur)) return;
    char name[64], abs[VPATH + 128];
    for (int i = 0; i < 1000; i++) {
        snprintf(name, sizeof name, i ? "NewFolder%d" : "NewFolder", i);
        char vp[VPATH + 64];
        snprintf(vp, sizeof vp, "%s/%s", S.cur, name);
        to_abs(vp, abs, sizeof abs);
        if (!re_file_stat(abs, NULL, NULL, NULL)) break;
    }
    if (re_make_dirs(abs)) {
        snprintf(S.pending_rename, sizeof S.pending_rename, "%s", name);
        content_mark_dirty();
    } else {
        message(TR("Could not create the folder in %s."), S.cur);
    }
}

// The renamed item's new path (the open after a rename follows it)
static char s_renamed_to[VPATH + 128];

static void rename_item(int i, const char* typed) {
    Item* it = &S.items[i];
    char new_name[192];
    const char* ext = it->kind != ASSET_FOLDER ? strrchr(it->vpath + it->name_off, '.') : NULL;
    snprintf(new_name, sizeof new_name, "%s%s", typed, ext && !strrchr(typed, '.') ? ext : "");
    snprintf(s_renamed_to, sizeof s_renamed_to, "%s", it->vpath);
    if (!is_content(it->vpath) || !strcmp(new_name, it->vpath + it->name_off)) return;
    if (!valid_name(new_name)) {
        message(TR("Invalid name: %s"), new_name);
        return;
    }
    char src[VPATH + 64], dst[VPATH + 192], vp[VPATH + 128];
    to_abs(it->vpath, src, sizeof src);
    snprintf(vp, sizeof vp, "%.*s%s", (int)it->name_off, it->vpath, new_name);
    to_abs(vp, dst, sizeof dst);
    if (!re_move_path(src, dst)) message(TR("Could not rename to %s (the name exists or the file is open)."), new_name);
    else {
        if (it->kind >= ASSET_BLUEPRINT && it->kind <= ASSET_DATA_ASSET) bp_renamed(it->vpath, vp); // the asset follows its file
        snprintf(s_renamed_to, sizeof s_renamed_to, "%s", vp);
        if (!strcmp(ext ? ext : "", ".rlevel")) { // its baked lighting follows it
            char ls[VPATH + 64], ld[VPATH + 192];
            level_rlight_path(src, ls, sizeof ls);
            level_rlight_path(dst, ld, sizeof ld);
            re_move_path(ls, ld);
        }
    }
    content_mark_dirty();
}

static void move_into(const char* src_vpath, const char* dir_vpath) {
    if (!is_content(src_vpath) || !is_content(dir_vpath)) return;
    size_t n = strlen(src_vpath);
    if (!strncmp(dir_vpath, src_vpath, n) && (dir_vpath[n] == 0 || dir_vpath[n] == '/')) return; // into itself
    char src[VPATH + 64], dst[VPATH * 2 + 64], vp[VPATH * 2];
    to_abs(src_vpath, src, sizeof src);
    snprintf(vp, sizeof vp, "%s/%s", dir_vpath, base_name(src_vpath));
    to_abs(vp, dst, sizeof dst);
    if (!strcmp(src, dst)) return;
    if (!re_move_path(src, dst)) message(TR("Could not move %s (a file with the name exists there)."), base_name(src_vpath));
    else {
        bool is_bp;
        bp_kind_of_path(src_vpath, &is_bp);
        if (is_bp) bp_renamed(src_vpath, vp);
    }
    content_mark_dirty();
}

static void delete_selected(void) {
    int done = 0;
    for (int i = 0; i < S.nitems; i++) {
        if (!S.sel[i] || !is_content(S.items[i].vpath)) continue;
        char abs[VPATH + 64];
        to_abs(S.items[i].vpath, abs, sizeof abs);
        if (re_trash_path(abs)) done++;
        else message(TR("Could not delete %s."), S.items[i].vpath + S.items[i].name_off);
    }
    if (done) content_mark_dirty();
}

static struct { char ext[16]; ContentImporter fn; } g_importers[24];
static int g_nimporters;
void content_set_importer(const char* ext, ContentImporter fn) {
    int i = 0;
    while (i < g_nimporters && _stricmp(g_importers[i].ext, ext)) i++;
    if (i == g_nimporters && g_nimporters < 24) g_nimporters++;
    if (i < g_nimporters) snprintf(g_importers[i].ext, sizeof g_importers[i].ext, "%s", ext), g_importers[i].fn = fn;
}

static ContentImporter importer_of(const char* name) {
    const char* ext = strrchr(name, '.');
    ContentImporter fn = NULL;
    for (int i = 0; ext && i < g_nimporters; i++)
        if (!_stricmp(ext + 1, g_importers[i].ext)) fn = g_importers[i].fn;
    return fn;
}

// The files an importer converts wait for the Import Options window (Unreal's): only assets by default, the level
// untouched; "Place in Level" also puts their objects in it
#define IMPORT_QUEUE 256
static struct {
    char src[IMPORT_QUEUE][1024], dir[IMPORT_QUEUE][VPATH];
    char root_src[IMPORT_QUEUE][1024], root_dst[IMPORT_QUEUE][VPATH]; // (2026-11-23) a dropped folder's: its textures used in place
    int n;
    bool shown, place;
} g_imp;

static bool import_queue_root(const char* dir, const char* src, const char* name, const char* root_src, const char* root_dst) {
    if (!importer_of(name)) return false;
    if (g_imp.n >= IMPORT_QUEUE) return true;
    snprintf(g_imp.src[g_imp.n], sizeof g_imp.src[0], "%s", src);
    snprintf(g_imp.dir[g_imp.n], VPATH, "%s", dir);
    snprintf(g_imp.root_src[g_imp.n], sizeof g_imp.root_src[0], "%s", root_src ? root_src : "");
    snprintf(g_imp.root_dst[g_imp.n], VPATH, "%s", root_dst ? root_dst : "");
    g_imp.n++;
    return true;
}
static bool import_queue(const char* dir, const char* src, const char* name) { return import_queue_root(dir, src, name, NULL, NULL); }

static const char* file_name(const char* path) {
    const char* name = path + strlen(path);
    while (name > path && name[-1] != '/' && name[-1] != '\\') name--;
    return name;
}

// A file an importer turns into assets (the Tlender's scenes, OBJ, glTF, FBX) into <dir>/<file's name>
static void import_converted(const char* dir, const char* src, bool place) {
    const char* name = file_name(src);
    const char* ext = strrchr(name, '.');
    ContentImporter fn = importer_of(name);
    if (!fn || !ext) return;
    char folder[VPATH], stem[128], msg[300];
    snprintf(stem, sizeof stem, "%.*s", (int)(ext - name), name);
    for (char* c = stem; *c; c++)
        if (*c == ' ' || *c == '.') *c = '_';
    snprintf(folder, sizeof folder, "%s/%s", dir, stem);
    if (fn(src, folder, place, msg, sizeof msg)) message("%s", msg);
    else message(TR("Could not import %s."), name);
    content_mark_dirty();
}

bool content_reimport(const char* vpath) {
    const SkelMesh* m = vpath ? anim_mesh_load(vpath) : NULL;
    if (!m) {
        printf("[RealEngine] Reimport: %s isn't a Skeletal Mesh\n", vpath ? vpath : "");
        return false;
    }
    char src[1024];
    snprintf(src, sizeof src, "%s", m->source);
    if (!src[0] || !re_file_stat(src, NULL, NULL, NULL)) { // (imported before the source was kept, or moved: asked)
        if (!platform_file_dialog_ext(NULL, false, TR("Reimport from"), "tlend;fbx;glb;gltf", src, sizeof src)) return false;
    }
    ContentImporter fn = importer_of(file_name(src));
    if (!fn) {
        message(TR("Could not import %s."), file_name(src));
        return false;
    }
    char folder[VPATH], msg[300];
    snprintf(folder, sizeof folder, "%s", vpath);
    char* slash = strrchr(folder, '/');
    if (slash) *slash = 0;
    printf("[RealEngine] Reimport %s from %s into %s\n", vpath, src, folder);
    if (fn(src, folder, false, msg, sizeof msg)) message("%s", msg);
    else message(TR("Could not import %s."), file_name(src));
    return true;
}

static void import_pop(void) {
    g_imp.n--;
    memmove(g_imp.src[0], g_imp.src[1], sizeof g_imp.src[0] * (size_t)g_imp.n);
    memmove(g_imp.dir[0], g_imp.dir[1], sizeof g_imp.dir[0] * (size_t)g_imp.n);
    memmove(g_imp.root_src[0], g_imp.root_src[1], sizeof g_imp.root_src[0] * (size_t)g_imp.n);
    memmove(g_imp.root_dst[0], g_imp.root_dst[1], sizeof g_imp.root_dst[0] * (size_t)g_imp.n);
}
// the queue's first file imported (its dropped folder's textures used where they were mirrored)
static void import_front(bool place) {
    tlend_import_set_mirror(g_imp.root_src[0], g_imp.root_dst[0]);
    import_converted(g_imp.dir[0], g_imp.src[0], place);
    tlend_import_set_mirror(NULL, NULL);
}

void content_import_options(void) {
    if (!g_imp.n) return;
    const char* id = TR_ID("Import Options");
    if (!g_imp.shown) igOpenPopup_Str(id, 0), g_imp.shown = true;
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 30.0f, 0 }, ImGuiCond_Appearing);
    if (!igBeginPopupModal(id, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        g_imp.shown = false; // (closed some other way: asked again)
        return;
    }
    const char* name = file_name(g_imp.src[0]);
    igTextUnformatted(TR("Current Asset"), NULL);
    igSameLine(0, 8);
    igTextColored(THV(TK_WARN), "%s", name);
    if (g_imp.n > 1) igTextDisabled(TR("%d more file(s) after this one"), g_imp.n - 1);
    igSeparatorText(TR("Import"));
    igTextDisabled(TR("Into %s"), g_imp.dir[0]);
    igBulletText("%s", TR("Static Meshes, materials and textures"));
    igBulletText("%s", TR("Skeletal Meshes and animations (armatures, skins)"));
    igSeparatorText(TR("Level"));
    igCheckbox(TR("Place in Level"), &g_imp.place);
    igTextDisabled("%s", g_imp.place ? TR("The objects, lights and cameras also become actors where they were in the file.")
                                     : TR("Only the assets are made; drag them from the Content Browser into the level."));
    igSpacing();
    bool all = false, one = igButton(TR("Import"), (ImVec2){ 110, 0 });
    igSameLine(0, 8);
    if (g_imp.n > 1) {
        all = igButton(TR("Import All"), (ImVec2){ 110, 0 });
        igSameLine(0, 8);
    }
    bool cancel = igButton(TR("Cancel"), (ImVec2){ 110, 0 }) || igIsKeyPressed_Bool(ImGuiKey_Escape, false);
    if (one || all) {
        do {
            import_front(g_imp.place);
            import_pop();
        } while (all && g_imp.n);
    } else if (cancel)
        import_pop();
    if (one || all || cancel) {
        igCloseCurrentPopup();
        g_imp.shown = false; // (the next file: its own window)
    }
    igEndPopup();
}

// (Q18) A UDIM set (a folder of tiles, or one tile's file with its siblings: <name>.1001.png ...) into one .rudim, on a
// worker (a card with its progress; the tiles compressed to BC7 / BC5 with their mips)
typedef struct UdimJob { char src[1024], out[1024]; UdimImportReport r; bool ok; } UdimJob;
static void udim_job_work(AsyncTask* t, void* arg) {
    (void)t;
    UdimJob* j = arg;
    j->ok = udim_import(j->src, j->out, NULL, &j->r);
}
static void udim_job_done(AsyncTask* t, void* arg, bool ok) {
    (void)t;
    UdimJob* j = arg;
    if (ok && j->ok) {
        char m[400];
        snprintf(m, sizeof m, TR("Imported the UDIM texture %s (%d tiles)."), file_name(j->out), j->r.tiles);
        message("%s", m);
    }
    else message(TR("Could not import %s."), file_name(j->src));
    content_mark_dirty();
    free(j);
}
static bool import_udim(const char* dir_abs, const char* src) {
    char pat[1024];
    bool is_dir = false;
    if (re_file_stat(src, NULL, NULL, &is_dir) && is_dir) snprintf(pat, sizeof pat, "%s", src); // (udim_import picks the folder's set)
    else if (!udim_detect(src, pat, sizeof pat)) return false;
    UdimJob* j = calloc(1, sizeof *j);
    if (!j) return true;
    snprintf(j->src, sizeof j->src, "%s", pat);
    if (is_dir) snprintf(j->out, sizeof j->out, "%s/%s.rudim", dir_abs, file_name(src));
    else udim_asset_name(pat, dir_abs, j->out, sizeof j->out);
    async_run(TR("Importing a UDIM texture"), udim_job_work, udim_job_done, j, 0);
    return true;
}

// (2026-11-23, the user: "drag a folder from Explorer into the Content Browser and import everything in it, its
// subfolders in order with their names, the textures and materials set up") A folder dropped: mirrored under the
// current folder (its subfolders with their names); its models (FBX, OBJ, glTF, .tlend, .rasset) queued into their
// mirrored folders - their textures inside the dropped folder used where they were mirrored, not copied again (the
// importers' mc_image: tlend_import_set_mirror); its images and sounds copied; a UDIM set one .rudim
typedef struct FolderImp {
    const char* root_src; char root_dst[VPATH];
    int models, textures, sounds, folders, unsupported, skipped;
    bool now, place;
    char udim_done[32][1024];
    int nudim;
} FolderImp;
typedef struct FolderWalk { FolderImp* st; char src[1024], dst[VPATH]; } FolderWalk;
static void folder_import_dir(FolderImp* st, const char* src, const char* dst);
static void folder_walk(void* user, const ReDirEntry* e) {
    FolderWalk* w = user;
    FolderImp* st = w->st;
    if (e->name[0] == '.') return;
    char src[1024], dst[VPATH], abs[VPATH + 64];
    snprintf(src, sizeof src, "%s/%s", w->src, e->name);
    if (e->is_dir) {
        snprintf(dst, sizeof dst, "%s/%s", w->dst, e->name);
        folder_import_dir(st, src, dst);
        return;
    }
    const char* dot = strrchr(e->name, '.');
    char ext[16] = "";
    if (dot) snprintf(ext, sizeof ext, "%s", dot + 1);
    for (char* c = ext; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (importer_of(e->name)) { // a model (or a sound: its importer makes the Sound Wave): queued (or now) into this folder
        if (st->now) {
            tlend_import_set_mirror(st->root_src, st->root_dst);
            import_converted(w->dst, src, st->place);
            tlend_import_set_mirror(NULL, NULL);
        } else
            import_queue_root(w->dst, src, e->name, st->root_src, st->root_dst);
        if (kind_of(e->name) == ASSET_SOUND_SOURCE) st->sounds++;
        else st->models++;
        return;
    }
    if (!strcmp(ext, "blend") || !strcmp(ext, "max") || !strcmp(ext, "ma") || !strcmp(ext, "mb") || !strcmp(ext, "c4d")) {
        st->unsupported++; // (a program's own scene: export it as FBX, glTF or the Tlender's .tlend)
        return;
    }
    AssetKind k = kind_of(e->name);
    if (k == ASSET_TEXTURE_SOURCE) {
        char pat[1024];
        if (udim_detect(src, pat, sizeof pat)) { // a UDIM tile: its set once, as one .rudim
            bool seen = false;
            for (int q = 0; q < st->nudim && !seen; q++) seen = !strcmp(st->udim_done[q], pat);
            if (!seen) {
                to_abs(w->dst, abs, sizeof abs);
                re_make_dirs(abs);
                if (import_udim(abs, src)) st->textures++;
                if (st->nudim < 32) snprintf(st->udim_done[st->nudim++], sizeof st->udim_done[0], "%s", pat);
            }
            return;
        }
    }
    if (k != ASSET_TEXTURE_SOURCE && k != ASSET_SOUND_SOURCE) {
        st->skipped++;
        return;
    }
    to_abs(w->dst, abs, sizeof abs);
    re_make_dirs(abs);
    char to[VPATH + 400];
    snprintf(to, sizeof to, "%s/%s", abs, e->name);
    if (re_file_stat(to, NULL, NULL, NULL) || re_copy_file(src, to)) {
        if (k == ASSET_TEXTURE_SOURCE) st->textures++;
        else st->sounds++;
    }
}
static void folder_import_dir(FolderImp* st, const char* src, const char* dst) {
    char abs[VPATH + 64];
    to_abs(dst, abs, sizeof abs);
    re_make_dirs(abs);
    st->folders++;
    FolderWalk w;
    w.st = st;
    snprintf(w.src, sizeof w.src, "%s", src);
    snprintf(w.dst, sizeof w.dst, "%s", dst);
    re_dir_walk(src, folder_walk, &w);
}
// a folder of only a UDIM set's tiles (the old drop: one .rudim)
typedef struct UdimOnly { int tiles, other; } UdimOnly;
static void udim_only_walk(void* user, const ReDirEntry* e) {
    UdimOnly* u = user;
    char pat[1024];
    if (e->is_dir || !udim_detect(e->name, pat, sizeof pat)) u->other++;
    else u->tiles++;
}
bool content_import_folder(const char* src, const char* dst_dir, bool now, bool place) {
    const char* name = file_name(src);
    char root[VPATH];
    snprintf(root, sizeof root, "%s/%s", dst_dir, name);
    FolderImp* st = calloc(1, sizeof *st);
    if (!st) return false;
    st->root_src = src, st->now = now, st->place = place;
    snprintf(st->root_dst, sizeof st->root_dst, "%s", root);
    folder_import_dir(st, src, root);
    char m[400];
    snprintf(m, sizeof m, TR("Folder %s: %d folder(s), %d model(s) %s, %d texture(s), %d sound(s)."), name, st->folders, st->models,
             now ? TR("imported") : TR("to import"), st->textures, st->sounds);
    message("%s", m);
    printf("[RealEngine] Import folder %s -> %s: %d folders, %d models, %d textures, %d sounds, %d unsupported, %d skipped\n", src, root, st->folders,
           st->models, st->textures, st->sounds, st->unsupported, st->skipped);
    if (st->unsupported) {
        snprintf(m, sizeof m, TR("%d file(s) of other programs' scenes (.blend, .max, .ma...) left out: export them as FBX, glTF or .tlend."), st->unsupported);
        message("%s", m);
    }
    content_mark_dirty();
    free(st);
    return true;
}

void content_import(int count, const char** paths) {
    const char* dir = is_content(S.cur) ? S.cur : "/Content";
    char dir_abs[VPATH + 64];
    to_abs(dir, dir_abs, sizeof dir_abs);
    int copied = 0;
    char udim_done[8][1024];
    int nudim = 0;
    for (int i = 0; i < count; i++) {
        const char* src = paths[i];
        const char* name = file_name(src);
        if (import_queue(dir, src, name)) continue;
        bool is_dir = false;
        if (re_file_stat(src, NULL, NULL, &is_dir) && is_dir) { // (2026-11-23) a folder: all of it (a UDIM set's own folder: one .rudim)
            UdimOnly u = { 0, 0 };
            re_dir_walk(src, udim_only_walk, &u);
            if (!(u.tiles && !u.other)) {
                content_import_folder(src, dir, false, false);
                continue;
            }
        }
        { // (Q18) a UDIM set's tiles (or their folder): one .rudim for the set, however many of its tiles were dropped
            char pat[1024];
            bool is_dir = false, seen = false;
            bool set = (re_file_stat(src, NULL, NULL, &is_dir) && is_dir) || udim_detect(src, pat, sizeof pat);
            if (set && !is_dir)
                for (int k = 0; k < nudim && !seen; k++) seen = !strcmp(udim_done[k], pat);
            if (set && !seen && import_udim(dir_abs, src) && !is_dir && nudim < 8) snprintf(udim_done[nudim++], sizeof udim_done[0], "%s", pat);
            if (set) continue;
        }
        char dst[VPATH + 300];
        snprintf(dst, sizeof dst, "%s/%s", dir_abs, name);
        for (int k = 2; re_file_stat(dst, NULL, NULL, NULL) && k < 1000; k++) { // "Rock (2).png"
            const char* dot = strrchr(name, '.');
            int stem = dot ? (int)(dot - name) : (int)strlen(name);
            snprintf(dst, sizeof dst, "%s/%.*s (%d)%s", dir_abs, stem, name, k, dot ? dot : "");
        }
        if (re_copy_file(src, dst)) copied++;
        else message(TR("Could not import %s."), name);
    }
    if (copied) {
        char m[64];
        snprintf(m, sizeof m, "%d", copied);
        message(TR("Imported %s file(s) (copied; conversion to .rasset comes in step 2)."), m);
    }
    content_mark_dirty();
}

static void import_dialog(void);
void content_import_dialog(void) { import_dialog(); }

typedef struct FindCtx { const char* filter; char (*out)[512]; int n, cap; char dir[VPATH]; int depth; } FindCtx;
static void find_walk(void* user, const ReDirEntry* e) {
    FindCtx* c = user;
    if (c->n >= c->cap || e->name[0] == '.') return;
    char vp[VPATH];
    snprintf(vp, sizeof vp, "%s/%s", c->dir, e->name);
    if (e->is_dir) {
        if (c->depth >= 12) return;
        FindCtx sub = *c;
        snprintf(sub.dir, VPATH, "%s", vp), sub.depth++;
        char abs[VPATH + 64];
        to_abs(vp, abs, sizeof abs);
        re_dir_walk(abs, find_walk, &sub);
        c->n = sub.n;
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (!dot || !strcmp(dot, ".png") || !strcmp(dot, ".jpg") || !strcmp(dot, ".tga") || !strcmp(dot, ".ini")) return; // (assets, not their files)
    if (!strcmp(dot, ".rmesh") && !strncmp(e->name, "SKP_", 4)) return; // (a Skeletal Mesh's parts)
    char a[256], b[64];
    snprintf(a, sizeof a, "%s", e->name), snprintf(b, sizeof b, "%s", c->filter);
    for (char* p = a; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (char* p = b; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (b[0] && !strstr(a, b)) return;
    snprintf(c->out[c->n++], 512, "%s", vp);
}
int content_search(const char* filter, char (*out)[512], int cap) {
    FindCtx c = { filter, out, 0, cap, "/Content", 0 };
    char abs[VPATH + 64];
    to_abs("/Content", abs, sizeof abs);
    re_dir_walk(abs, find_walk, &c);
    return c.n;
}

static void import_dialog(void) {
    static char paths[64][1024];
    int n = platform_file_dialog_multi(NULL, "Assets", "tlend;rasset;rlevel;png;jpg;jpeg;tga;tif;tiff;exr;hdr;dds;fbx;obj;gltf;glb;wav;ogg",
                                       paths, 64);
    const char* p[64];
    for (int i = 0; i < n; i++) p[i] = paths[i];
    if (n > 0) content_import(n, p);
}

void content_init(const char* root) {
    memset(&S, 0, sizeof S);
    snprintf(S.root, VPATH, "%s", root);
    re_make_dirs(root);
    size_t bytes = sizeof(Item) * MAX_ITEMS + sizeof(Folder) * MAX_FOLDERS + sizeof(bool) * MAX_ITEMS;
    if (!re_vm_reserve(&S.mem, bytes) || !re_vm_ensure(&S.mem, bytes)) return;
    S.items = (Item*)S.mem.base;
    S.folders = (Folder*)(S.items + MAX_ITEMS);
    S.sel = (bool*)(S.folders + MAX_FOLDERS);
    S.tile = 96.0f;
    S.renaming = -1;
    navigate("/Content", true);
    content_mark_dirty();
}

// --- Drawing ---

static int selected_count(void) {
    int n = 0;
    for (int i = 0; i < S.nitems; i++) n += S.sel[i];
    return n;
}

// (2026-11-23, "arrastar vários arquivos") A press on an item already selected with others keeps them all (for a drag);
// let go without dragging, only it stays selected
static int s_press_kept = -1;
static bool s_dragging;
int content_drag_paths(const char* vpath, char (*out)[512], int cap) {
    int n = 0;
    if (cap > 0) snprintf(out[n++], 512, "%s", vpath);
    bool in = false;
    for (int i = 0; i < S.nitems; i++) in |= S.sel[i] && !strcmp(S.items[i].vpath, vpath);
    for (int i = 0; in && i < S.nitems && n < cap; i++)
        if (S.sel[i] && strcmp(S.items[i].vpath, vpath)) snprintf(out[n++], 512, "%s", S.items[i].vpath);
    return n;
}
static void drag_source(Item* it) {
    if (!igBeginDragDropSource(0)) return;
    s_dragging = true;
    AssetPayload p = { (AssetKind)it->kind, it->shape, "" };
    snprintf(p.vpath, sizeof p.vpath, "%s", it->vpath);
    igSetDragDropPayload(RE_PAYLOAD_ASSET, &p, sizeof p, ImGuiCond_Once);
    ui_icon_item(kind_icon(it), igGetFontSize() * 1.6f, TH(TK_N14));
    igSameLine(0, 6);
    int n = 0;
    bool in = false;
    for (int i = 0; i < S.nitems; i++) n += S.sel[i], in |= S.sel[i] && &S.items[i] == it;
    if (in && n > 1) igText(TR("%s and %d more"), it->vpath + it->name_off, n - 1);
    else igText("%s", it->vpath + it->name_off);
    igEndDragDropSource();
}

// A folder (tree node or tile) accepts assets dragged onto it: they move there
static void folder_drop_target(const char* dir_vpath) {
    if (!is_content(dir_vpath) || !igBeginDragDropTarget()) return;
    const ImGuiPayload* pl = igAcceptDragDropPayload(RE_PAYLOAD_ASSET, 0);
    if (pl) {
        const AssetPayload* p = pl->Data;
        // the dragged item, and the others selected with it
        bool moved_sel = false;
        for (int i = 0; i < S.nitems; i++)
            if (S.sel[i] && !strcmp(S.items[i].vpath, p->vpath)) moved_sel = true;
        if (moved_sel) {
            for (int i = 0; i < S.nitems; i++)
                if (S.sel[i]) move_into(S.items[i].vpath, dir_vpath);
        } else {
            move_into(p->vpath, dir_vpath);
        }
    }
    igEndDragDropTarget();
}

static int draw_tree_node(int i) {
    const Folder* f = &S.folders[i];
    int next = i + 1;
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                            ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!f->has_children) fl |= ImGuiTreeNodeFlags_Leaf;
    if (!strcmp(f->vpath, S.cur)) fl |= ImGuiTreeNodeFlags_Selected;
    size_t n = strlen(f->vpath); // the path to the current folder stays open
    if (!strncmp(S.cur, f->vpath, n) && S.cur[n] == '/') igSetNextItemOpen(true, ImGuiCond_Always);
    else if (f->depth == 0) igSetNextItemOpen(true, ImGuiCond_Once);
    igPushID_Str(f->vpath);
    float icon = igGetFontSize();
    bool open = igTreeNodeEx_Str("##node", fl); // tips: none (a canvas or a tile drawn with its own name: no description over the work)
    ImVec2 a = igGetItemRectMin();
    float x = a.x + igGetTreeNodeToLabelSpacing();
    ui_icon(igGetWindowDrawList(), ICON_FOLDER, x, a.y + (igGetFrameHeight() - icon) * 0.5f - 1.0f, icon,
            is_engine(f->vpath) ? TH(TK_N12) : TH(TK_AMBER));
    ImDrawList_AddText_Vec2(igGetWindowDrawList(), (ImVec2){ x + icon + 5.0f, a.y + igGetStyle()->FramePadding.y * 0.0f },
                            igGetColorU32_Col(ImGuiCol_Text, 1.0f), f->vpath + f->name_off, NULL);
    if (igIsItemClicked(ImGuiMouseButton_Left) && !igIsItemToggledOpen()) navigate(f->vpath, true);
    folder_drop_target(f->vpath);
    if (igBeginPopupContextItem("tree_menu", ImGuiPopupFlags_MouseButtonRight)) {
        if (ui_menu_item(ICON_FOLDER, TR("New Folder"), NULL, false, is_content(f->vpath))) {
            navigate(f->vpath, true);
            new_folder();
        }
        if (ui_menu_item(ICON_OPEN, TR("Show in Explorer"), NULL, false, is_content(f->vpath))) {
            char abs[VPATH + 64];
            to_abs(f->vpath, abs, sizeof abs);
            re_show_in_explorer(abs);
        }
        igEndPopup();
    }
    igPopID();
    while (next < S.nfolders && S.folders[next].depth > f->depth) {
        if (open) next = draw_tree_node(next);
        else next++;
    }
    if (open) igTreePop();
    return next;
}

static void breadcrumb(void) {
    char acc[VPATH] = "";
    const char* p = S.cur;
    while (*p) {
        const char* s = strchr(p + 1, '/');
        size_t len = s ? (size_t)(s - S.cur) : strlen(S.cur);
        snprintf(acc, sizeof acc, "%.*s", (int)len, S.cur);
        const char* name = base_name(acc);
        igPushID_Str(acc);
        bool last = !s;
        if (last) igPushStyleColor_Vec4(ImGuiCol_Text, THV(TK_N15));
        if (igSmallButton(name)) navigate(acc, true);
        if (last) igPopStyleColor(1);
        folder_drop_target(acc);
        igPopID();
        if (!s) break;
        igSameLine(0, 2);
        igTextDisabled(TR(">"));
        igSameLine(0, 2);
        p = s;
    }
}

static void item_tooltip(const Item* it) {
    if (!igBeginItemTooltip()) return;
    igText("%s", it->vpath + it->name_off);
    igTextDisabled("%s", asset_kind_name((AssetKind)it->kind));
    igTextDisabled("%s", it->vpath);
    if (it->kind != ASSET_FOLDER && it->kind != ASSET_BASIC_SHAPE) {
        time_t t = (time_t)it->mtime;
        struct tm* tm = localtime(&t);
        char when[64] = "";
        if (tm) strftime(when, sizeof when, "%Y-%m-%d %H:%M", tm);
        igTextDisabled(TR("%.1f KB, modified %s"), (double)it->size / 1024.0, when);
    }
    if (it->kind == ASSET_BASIC_SHAPE) igTextDisabled(TR("Drag into the viewport to place it."));
    igEndTooltip();
}

static void click_item(int i) {
    ImGuiIO* io = igGetIO_Nil();
    if (io->KeyCtrl) {
        S.sel[i] = !S.sel[i];
    } else if (io->KeyShift && S.anchor >= 0 && S.anchor < S.nitems) {
        memset(S.sel, 0, sizeof(bool) * MAX_ITEMS);
        int a = S.anchor < i ? S.anchor : i, b = S.anchor < i ? i : S.anchor;
        for (int k = a; k <= b; k++) S.sel[k] = true;
        return;
    } else {
        memset(S.sel, 0, sizeof(bool) * MAX_ITEMS);
        S.sel[i] = true;
    }
    S.anchor = i;
}

static void begin_rename(int i) {
    if (i < 0 || i >= S.nitems || !is_content(S.items[i].vpath)) return;
    S.renaming = i;
    snprintf(S.rename_buf, sizeof S.rename_buf, "%s", S.items[i].vpath + S.items[i].name_off);
    if (S.items[i].kind != ASSET_FOLDER) { // the name without its extension (kept when renamed)
        char* dot = strrchr(S.rename_buf, '.');
        if (dot) *dot = 0;
    }
    S.rename_focus = true, S.rename_live = false, S.rename_asked = 0;
}

static void show_selected_in_explorer(int i) {
    char abs[VPATH + 64];
    to_abs(i >= 0 ? S.items[i].vpath : S.cur, abs, sizeof abs);
    if (is_content(i >= 0 ? S.items[i].vpath : S.cur)) re_show_in_explorer(abs);
}

static int g_open_item = -1; // a double click this frame

// --- Made, then named, then opened (Unreal's Add: the new asset's name is edited first) ---
static bool s_creating;            // the Add menus are running: an editor asked to open what they made waits
static bool s_open_after_rename;   // the item being renamed opens when the name is typed
static ContentLevelHooks s_level;

void content_set_level_hooks(const ContentLevelHooks* h) { s_level = h ? *h : (ContentLevelHooks){ 0 }; }

bool content_defer_open(const char* vpath) {
    if (!s_creating || !vpath) return false;
    snprintf(S.pending_rename, sizeof S.pending_rename, "%s", base_name(vpath));
    s_open_after_rename = true;
    content_mark_dirty();
    return true;
}

// --- (Q6) the asset picker's and the creations' share (content.h) ---
static int s_drawn_frame; // ImGui's frame when the panel was last drawn
AssetKind content_kind_of(const char* name) { return kind_of(name); }
int content_kind_color(AssetKind k) { return k < ASSET_KIND_COUNT ? g_kind_colors[k] : TK_ASSET_DATA; }
int content_kind_icon(int kind, const char* vpath) {
    Item it = { 0 };
    it.kind = (uint8_t)kind;
    it.bp_state = 2; // (a class: the Blueprint icon; the picker never reads a file to draw an icon)
    (void)vpath;
    return kind_icon(&it);
}
// The Blueprint files that could not be read (the picker draws its rows every frame: never read again each time)
static uint32_t s_bp_bad[64];
static unsigned s_bp_bad_n;
bool content_thumb(ImDrawList* dl, float x0, float y0, float x1, float y1, const char* vpath, int kind, bool hovered) {
    if (!vpath || vpath[0] != '/') return false;
    if (kind == ASSET_BLUEPRINT) {
        uint32_t h = bp_vpath_hash(vpath);
        for (unsigned i = 0; i < 64 && i < s_bp_bad_n; i++)
            if (s_bp_bad[i] == h) return false;
        BpAsset* a = bp_load(vpath); // (bp_load's cache)
        if (!a) s_bp_bad[s_bp_bad_n++ % 64] = h;
        BpPreviewCam cam = { 0 };
        return a && bp_preview_draw(dl, x0, y0, x1, y1, a, &cam, BP_PREVIEW_THUMB);
    }
    if (kind == ASSET_ANIMATION && hovered) asset_thumb_hover(vpath);
    if (kind == ASSET_SOUND_WAVE) return audio_editor_thumb(dl, x0, y0, x1, y1, vpath, hovered);
    return asset_thumb(dl, x0, y0, x1, y1, vpath, kind);
}
void content_message(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(S.message, sizeof S.message, fmt, ap);
    va_end(ap);
    printf("[RealEngine] Content: %s\n", S.message);
    fflush(stdout);
}
void content_created(const char* vpath) {
    const char* sl = strrchr(vpath, '/');
    content_mark_dirty();
    // (the panel drawn a frame ago, showing its folder: renamed there first; the drawer closed: opened now)
    bool shown = s_drawn_frame && igGetFrameCount() - s_drawn_frame <= 2;
    if (shown && sl && (size_t)(sl - vpath) == strlen(S.cur) && !strncmp(vpath, S.cur, (size_t)(sl - vpath))) {
        snprintf(S.pending_rename, sizeof S.pending_rename, "%s", sl + 1); // named first, opened after (like Add)
        s_open_after_rename = true;
    } else
        content_open_asset(vpath);
}

void content_open_asset(const char* vpath) {
    const char* dot = strrchr(vpath, '.');
    if (!dot) return;
    bool is_bp;
    bp_kind_of_path(vpath, &is_bp);
    if (is_bp) bp_editor_open(vpath);
    else if (!strcmp(dot, ".rmat") || !strcmp(dot, ".rmi")) mat_editor_open(vpath);
    else if (asset_viewer_handles(vpath)) asset_viewer_open(vpath);
    else if (!strcmp(dot, ".rskm") || !strcmp(dot, ".ranim") || !strcmp(dot, ".rmontage")) anim_editor_open(vpath);
    else if (!strcmp(dot, ".rbt") || !strcmp(dot, ".rbrain")) ai_editor_open(vpath);
    else if (!strcmp(dot, ".rinputaction") || !strcmp(dot, ".rimc")) input_editor_open(vpath);
    else if (!strcmp(dot, ".rseq")) seq_editor_open(vpath);
    else if (audio_editor_handles(vpath)) audio_editor_open(vpath);
    else if (script_is_path(vpath)) script_ide_open(vpath);
    else if (particle_editor_handles(vpath)) particle_editor_open(vpath);
    else if (ikrig_editor_handles(vpath)) ikrig_editor_open(vpath);
    else if (vehicle_editor_handles(vpath)) vehicle_editor_open(vpath);
    else if (dlg_is_path(vpath)) dlg_editor_open(vpath);
    else if (quest_is_path(vpath)) quest_editor_open(vpath);
    else if (physmat_is_path(vpath)) physmat_editor_open(vpath);
    else if (camshake_is_path(vpath)) camshake_editor_open(vpath); // (Q12)
    else if (pcg_is_path(vpath)) pcg_editor_open(vpath); // (Q15)
    else if (!strcmp(dot, ".rlevel") && s_level.open) {
        char abs[VPATH + 64];
        to_abs(vpath, abs, sizeof abs);
        s_level.open(abs);
    }
}

static bool create_child(const char* vpath) { // named first, opened after (like Add)
    s_creating = true;
    bool ok = bp_editor_create_child(vpath, true);
    s_creating = false;
    return ok;
}

// Add > Level: an empty level (the sky, the sun, a floor, a Player Start) named first, opened after
static void new_level_asset(void) {
    if (!is_content(S.cur) || !s_level.create) return;
    char name[64], vp[VPATH + 64], abs[VPATH + 128];
    for (int i = 0; i < 1000; i++) {
        snprintf(name, sizeof name, i ? "NewMap%d.rlevel" : "NewMap.rlevel", i + 1);
        snprintf(vp, sizeof vp, "%s/%s", S.cur, name);
        to_abs(vp, abs, sizeof abs);
        if (!re_file_stat(abs, NULL, NULL, NULL)) break;
    }
    if (!s_level.create(abs)) {
        message(TR("Could not create the level in %s."), S.cur);
        return;
    }
    snprintf(S.pending_rename, sizeof S.pending_rename, "%s", name);
    s_open_after_rename = true;
    content_mark_dirty();
}

// The Add menus (the toolbar's and the right click's): what an editor opens waits for its name
// (laid out as Unreal 5's: the basic assets, then the advanced ones by category, in submenus)
static void add_menus(bool editable) {
    s_creating = true;
    bool made = false;
    // each section its own ID scope: the basic "Material" item and the advanced "Material" submenu share a label
    igSeparatorText(TR("Create Basic Asset"));
    igPushID_Str("basic");
    made |= bp_editor_add_menu(S.cur, 0);
    if (ui_menu_item(ICON_LEVEL, TR("Level"), NULL, false, editable)) new_level_asset();
    made |= mat_editor_add_menu(S.cur, 0);
    igPopID();
    igSeparatorText(TR("Create Advanced Asset"));
    igPushID_Str("advanced");
    made |= anim_editor_add_menu(S.cur);
    made |= ai_editor_add_menu(S.cur);
    if (ui_begin_menu(ICON_BLUEPRINT, TR("Blueprint"), editable)) {
        made |= bp_editor_add_menu(S.cur, 1);
        igEndMenu();
    }
    made |= input_editor_add_menu(S.cur);
    made |= seq_editor_add_menu(S.cur);
    made |= audio_editor_add_menu(S.cur);
    made |= script_ide_add_menu(S.cur);
    if (ui_begin_menu(ICON_IMAGE, TR("Material"), editable)) {
        made |= mat_editor_add_menu(S.cur, 1);
        igEndMenu();
    }
    if (ui_begin_menu(ICON_PANEL, TR("User Interface"), editable)) {
        made |= bp_editor_add_menu(S.cur, 2);
        igEndMenu();
    }
    if (ui_begin_menu(ICON_DATA, TR("Miscellaneous"), editable)) {
        made |= bp_editor_add_menu(S.cur, 3);
        igEndMenu();
    }
    made |= particle_editor_add_menu(S.cur); // (Visual Effects: a Particle System from a template)
    made |= ikrig_editor_add_menu(S.cur);    // (an IK Rig or a Physics Asset for a Skeletal Mesh)
    made |= vehicle_editor_add_menu(S.cur);  // (a vehicle from a template)
    made |= physmat_editor_add_menu(S.cur);  // (Q8: a Physical Material from a preset)
    made |= camshake_editor_add_menu(S.cur); // (Q12: a Camera Shake from a preset)
    made |= pcg_editor_add_menu(S.cur);      // (Q15: a PCG Graph, empty or an example)
    if (plugins_gameplay_enabled()) { // (the Gameplay Framework plugin's assets: Edit > Plugins)
        made |= dlg_editor_add_menu(S.cur);      // (a Dialogue: empty or the example)
        made |= quest_editor_add_menu(S.cur);    // (a Quest: empty or the example)
    }
    igPopID();
    if (made) content_mark_dirty();
    s_creating = false;
}

static void draw_tile(int i, float x, float y, float tile, float row_h) {
    Item* it = &S.items[i];
    ImDrawList* dl = igGetWindowDrawList();
    igPushID_Int(i);
    igSetCursorScreenPos((ImVec2){ x, y });
    igInvisibleButton("tile", (ImVec2){ tile, row_h - 6.0f }, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight); // tips: none (a canvas, a node or a tile drawn with its own name and text)
    bool hovered = igIsItemHovered(0);
    if (igIsItemClicked(ImGuiMouseButton_Left)) {
        ImGuiIO* cio = igGetIO_Nil();
        if (S.sel[i] && !cio->KeyCtrl && !cio->KeyShift && selected_count() > 1) s_press_kept = i, s_dragging = false; // (kept for a drag)
        else click_item(i);
        if (igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) g_open_item = i;
    }
    if (s_press_kept == i && igIsMouseReleased_Nil(ImGuiMouseButton_Left)) { // (let go: a click, unless it was dragged)
        if (!s_dragging) click_item(i);
        s_press_kept = -1;
    }
    if (igIsItemClicked(ImGuiMouseButton_Right) && !S.sel[i]) click_item(i);
    drag_source(it);
    if (it->kind == ASSET_FOLDER) folder_drop_target(it->vpath);
    if (S.renaming != i) item_tooltip(it);
    if (igBeginPopupContextItem("tile_menu", ImGuiPopupFlags_MouseButtonRight)) {
        bool editable = is_content(it->vpath);
        if (it->kind == ASSET_FOLDER && ui_menu_item(ICON_OPEN, TR("Open"), "Enter", false, true)) g_open_item = i;
        if (it->kind >= ASSET_BLUEPRINT && it->kind <= ASSET_DATA_ASSET && ui_menu_item(ICON_OPEN, TR("Edit..."), "Enter", false, true))
            g_open_item = i;
        if (it->kind == ASSET_BLUEPRINT) { // Unreal's Create Child Blueprint Class: a class that inherits this one
            if (ui_menu_item(ICON_BLUEPRINT, TR("Create Child Blueprint Class"), NULL, false, editable && item_bp(it) != NULL) &&
                bp_editor_create_child(it->vpath, true))
                content_mark_dirty();
            igSeparator();
        }
        if (it->kind == ASSET_SKELETAL_MESH) { // (Unreal's Reimport: the source file read again into this folder)
            if (ui_menu_item(ICON_IMPORT, TR("Reimport"), NULL, false, editable)) content_reimport(it->vpath);
            igSeparator();
        }
        content_create_menu(it->vpath, it->kind, editable); // (Q6) what is made from it, by its type (content_create.c)
        if (ui_menu_item(ICON_RENAME, TR("Rename"), "F2", false, editable)) begin_rename(i);
        if (ui_menu_item(ICON_DELETE, TR("Delete"), "Delete", false, editable)) S.ask_delete = true;
        igSeparator();
        if (ui_menu_item(ICON_OPEN, TR("Show in Explorer"), NULL, false, editable)) show_selected_in_explorer(i);
        if (sc_active() && it->kind != ASSET_FOLDER) { // (B14) Git LFS's locks: one person edits a binary asset at a time
            if (ui_menu_item(ICON_LOCK, TR("Lock (Git LFS)"), NULL, false, editable && !sc_busy())) sc_lock(it->vpath, true);
            if (ui_menu_item(ICON_UNLOCK, TR("Unlock (Git LFS)"), NULL, false, editable && !sc_busy())) sc_lock(it->vpath, false);
        }
        if (ui_menu_item(ICON_COPY, TR("Copy Path"), NULL, false, true)) {
            char abs[VPATH + 64];
            to_abs(it->vpath, abs, sizeof abs);
            igSetClipboardText(abs);
        }
        igEndPopup();
    }

    // The tile: thumbnail square, type bar, name, type. (Q26, the mockups) a folder: a big solid folder in the folders'
    // color, no card; an asset: a dark card, its picture, a bar of its family's color under it
    bool sel = S.sel[i];
    bool folder = it->kind == ASSET_FOLDER;
    uint32_t bg = sel ? TH(TK_ACCENT_DIM) : hovered ? TH(TK_BG_HOVER) : folder ? THEME_NONE : TH(TK_BG_RAISED);
    float rr = TMV(TM_RADIUS_PANEL);
    ImDrawList_AddRectFilled(dl, (ImVec2){ x, y }, (ImVec2){ x + tile, y + row_h - 6.0f }, bg, rr, 0);
    if (!folder) ImDrawList_AddRectFilled(dl, (ImVec2){ x + 2, y + 2 }, (ImVec2){ x + tile - 2, y + tile - 2 }, TH(TK_BG_FIELD), rr - 1.0f, 0);
    float icon = tile * (it->kind == ASSET_FOLDER ? 0.62f : 0.5f);
    uint32_t icol = it->kind == ASSET_FOLDER && is_engine(it->vpath) ? TH(TK_TEXT_DIM) : TH(TK_TEXT); // (Q5: duotone, the fill by family)
    BpAsset* bp = item_bp(it);
    BpPreviewCam cam = { 0 };
    // a class with something to see: its thumbnail (bp_preview.c) and its kind's badge in the corner; a texture, a mesh,
    // a material: theirs (asset_viewer.c); else the kind's icon. An animation under the pointer plays
    if ((hovered || (g_test_hover && !strcmp(it->vpath, g_test_hover))) && it->kind == ASSET_ANIMATION) asset_thumb_hover(it->vpath);
    if ((bp && bp_preview_draw(dl, x + 2, y + 2, x + tile - 2, y + tile - 5, bp, &cam, BP_PREVIEW_THUMB)) ||
        (!bp && asset_thumb(dl, x + 4, y + 4, x + tile - 4, y + tile - 7, it->vpath, it->kind)) ||
        (it->kind == ASSET_SOUND_WAVE && audio_editor_thumb(dl, x + 4, y + 4, x + tile - 4, y + tile - 7, it->vpath, hovered))) {
        float bs = fmaxf(tile * 0.2f, 14.0f);
        ImDrawList_AddRectFilled(dl, (ImVec2){ x + 4, y + tile - 7 - bs - 4 }, (ImVec2){ x + 8 + bs, y + tile - 7 }, THA(TK_N4, 176), 3.0f, 0);
        ui_icon(dl, kind_icon(it), x + 6, y + tile - 9 - bs, bs, TH(TK_N14));
    } else if (folder) {
        float fsz = tile * 0.74f;
        ui_folder_solid(dl, x + (tile - fsz) * 0.5f, y + (tile - fsz) * 0.5f, fsz, is_engine(it->vpath) ? theme_scale(TH(TK_ASSET_FOLDER), 0.7f) : TH(TK_ASSET_FOLDER));
    } else {
        ui_icon(dl, kind_icon(it), x + (tile - icon) * 0.5f, y + (tile - icon) * 0.5f, icon, icol);
    }
    (void)icol;
    if (!folder) // (Q26, the mockups; Unreal's) the kind: a bar of its family's color under the picture
        ImDrawList_AddRectFilled(dl, (ImVec2){ x + 2, y + tile - 5.0f }, (ImVec2){ x + tile - 2, y + tile - 2 }, TH(ui_asset_family_token(it->kind)), 1.5f, 0);
    if (sel) ImDrawList_AddRect(dl, (ImVec2){ x, y }, (ImVec2){ x + tile, y + row_h - 6.0f }, TH(TK_ACCENT), 4.0f, 2.0f, 0);
    { // (B14) its Git state: a dot in the top right corner (green new, yellow modified, red deleted) as Unreal's checkmarks
        int scs = it->kind != ASSET_FOLDER ? sc_state(it->vpath) : SC_NONE;
        if (scs) {
            static const uint32_t c[] = { 0, TK_GREEN, TK_AMBER, TK_RED, TK_GREEN };
            ImDrawList_AddCircleFilled(dl, (ImVec2){ x + tile - 9.0f, y + 9.0f }, 5.0f, c[scs], 12);
            ImDrawList_AddCircle(dl, (ImVec2){ x + tile - 9.0f, y + 9.0f }, 5.5f, TH(TK_N2), 12, 1.0f);
        }
    }

    float ty = y + tile + 3.0f;
    if (S.renaming == i) {
        igSetCursorScreenPos((ImVec2){ x + 2, ty });
        if (S.rename_focus) igSetKeyboardFocusHere(0);
        igSetNextItemWidth(tile - 4);
        bool done = igInputText("##rename", S.rename_buf, sizeof S.rename_buf,
                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll, NULL, NULL);
        // (SetKeyboardFocusHere activates the field a frame later: until then it isn't active, and isn't over either;
        // the new asset's editor used to open at once, under its default name)
        // It ends once typed (Enter), left after typing (a click elsewhere, Esc), or, never activated, on a click away.
        bool active = igIsItemActive();
        if (active) S.rename_live = true, S.rename_focus = false;
        else if (S.rename_focus && ++S.rename_asked > 30) S.rename_focus = false; // (stops asking: the click below ends it)
        bool ended = done || (S.rename_live && !active) ||
                     (!S.rename_live && !S.rename_focus && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false) && !igIsItemHovered(0));
        if (ended) S.rename_live = false, S.rename_asked = 0;
        if (done) rename_item(i, S.rename_buf);
        else if (ended) snprintf(s_renamed_to, sizeof s_renamed_to, "%s", it->vpath);
        if (ended) {
            S.renaming = -1;
            if (s_open_after_rename) { // a new asset: its editor now, under its name
                s_open_after_rename = false;
                content_open_asset(s_renamed_to);
            }
        }
    } else {
        // the name, cut to the tile with "..."
        const char* name = it->vpath + it->name_off;
        char shown[96];
        snprintf(shown, sizeof shown, "%s", name);
        float maxw = tile - 8.0f;
        if (igCalcTextSize(shown, NULL, false, -1).x > maxw) {
            size_t n = strlen(shown);
            while (n > 1 && igCalcTextSize(shown, NULL, false, -1).x > maxw - igCalcTextSize("...", NULL, false, -1).x)
                shown[--n] = 0;
            strcat(shown, "...");
        }
        if (folder) { // (Q26) a folder: its name centered under it, no kind
            float nw = igCalcTextSize(shown, NULL, false, -1).x;
            ImDrawList_AddText_Vec2(dl, (ImVec2){ x + (tile - nw) * 0.5f, ty }, TH(TK_TEXT), shown, NULL);
        } else {
            ImDrawList_AddText_Vec2(dl, (ImVec2){ x + 5, ty }, TH(TK_N14), shown, NULL);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ x + 5, ty + igGetTextLineHeight() }, TH(TK_N11),
                                    asset_kind_name((AssetKind)it->kind), NULL);
        }
    }
    igPopID();
}

static void keyboard(void) {
    ImGuiIO* io = igGetIO_Nil();
    if (io->WantTextInput || !igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    int first = -1;
    for (int i = 0; i < S.nitems && first < 0; i++)
        if (S.sel[i]) first = i;
    if (igIsKeyPressed_Bool(ImGuiKey_Delete, false) && first >= 0) S.ask_delete = true;
    if (igIsKeyPressed_Bool(ImGuiKey_F2, false)) begin_rename(first);
    if (igIsKeyPressed_Bool(ImGuiKey_Enter, false) && first >= 0) g_open_item = first;
    if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_A))
        for (int i = 0; i < S.nitems; i++) S.sel[i] = true;
    if (igIsKeyChordPressed_Nil(ImGuiMod_Alt | ImGuiKey_LeftArrow) || igIsKeyPressed_Bool(ImGuiKey_Backspace, false)) go_history(-1);
    if (igIsKeyChordPressed_Nil(ImGuiMod_Alt | ImGuiKey_RightArrow)) go_history(1);
}

static void delete_modal(void) {
    if (S.ask_delete) {
        igOpenPopup_Str(TR_ID("Delete Assets"), 0);
        S.ask_delete = false;
    }
    if (igBeginPopupModal(TR_ID("Delete Assets"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        int n = 0;
        for (int i = 0; i < S.nitems; i++) n += S.sel[i] && is_content(S.items[i].vpath);
        igText(TR("Delete %d item(s)? They go to the Recycle Bin."), n);
        for (int i = 0, shown = 0; i < S.nitems && shown < 8; i++)
            if (S.sel[i] && is_content(S.items[i].vpath)) {
                igBulletText("%s", S.items[i].vpath + S.items[i].name_off);
                shown++;
            }
        igSpacing();
        if (igButton(TR("Delete"), (ImVec2){ 120, 0 })) {
            delete_selected();
            igCloseCurrentPopup();
        }
        igSameLine(0, 8);
        if (igButton(TR("Cancel"), (ImVec2){ 120, 0 }) || igIsKeyPressed_Bool(ImGuiKey_Escape, false)) igCloseCurrentPopup();
        igEndPopup();
    }
}

static float g_drawer_dy;
float content_drawer_drag(void) { return g_drawer_dy; }

bool content_draw(float x, float y, float w, float h, bool docked, bool* toggle_dock) {
    if (!S.items) return false;
    s_drawn_frame = igGetFrameCount();
    if (S.tree_dirty) scan_tree();
    if (S.dirty) scan_items();
    if (g_browse_select[0] && !S.dirty) {
        for (int i = 0; i < S.nitems; i++)
            if (!strcmp(S.items[i].vpath, g_browse_select)) {
                memset(S.sel, 0, sizeof(bool) * MAX_ITEMS);
                S.sel[i] = true;
                S.anchor = i;
            }
        g_browse_select[0] = 0;
    }
    bool keep = true;
    *toggle_dock = false;
    g_open_item = -1;

    bool panel = w <= 0.0f; // (Q2) docked: a panel of the editor's layout (editor_dock.c), where the user put it
    if (!panel) {
        igSetNextWindowPos((ImVec2){ x, y }, ImGuiCond_Always, (ImVec2){ 0, 0 });
        igSetNextWindowSize((ImVec2){ w, h }, ImGuiCond_Always);
    }
    ImGuiWindowFlags wf = panel ? ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar
                                : ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
    igPushStyleColor_Vec4(ImGuiCol_WindowBg, THV(panel ? TK_BG_PANEL : TK_N6));
    bool open_panel = true;
    igBegin(docked ? TR_ID("Content Browser") : TR_ID("Content Drawer"), panel ? &open_panel : NULL, wf);
    igPopStyleColor(1);
    if (!open_panel) keep = false;
    g_drawer_dy = 0.0f;
    if (!docked) { // the drawer's top edge: dragged, it grows or shrinks (Unreal's)
        ImVec2 back = igGetCursorScreenPos();
        igSetCursorScreenPos((ImVec2){ x, y });
        igInvisibleButton("##drawer_edge", (ImVec2){ w, 5.0f }, 0);
        if (igIsItemHovered(0) || igIsItemActive()) {
            igSetMouseCursor(ImGuiMouseCursor_ResizeNS);
            ImDrawList_AddRectFilled(igGetWindowDrawList(), (ImVec2){ x, y }, (ImVec2){ x + w, y + 3.0f }, TH(TK_ACCENT), 0, 0);
        }
        if (igIsItemActive()) g_drawer_dy = igGetIO_Nil()->MouseDelta.y;
        igSetCursorScreenPos(back);
    }
    bool editable = is_content(S.cur);

    // Toolbar: Add, Import, history, breadcrumb; dock / close on the right
    if (look_primary_button("cb_add", ICON_PLUS, TR("Add"), TR("New folder, new assets (Blueprint Class, Material, Level...)"))) igOpenPopup_Str("cb_add_menu", 0); // (Q26: the primary button)
    if (igBeginPopup("cb_add_menu", 0)) {
        if (ui_menu_item(ICON_FOLDER, TR("New Folder"), NULL, false, editable)) new_folder();
        igSeparator();
        add_menus(editable);
        igEndPopup();
    }
    igSameLine(0, 4);
    if (ui_icon_button("cb_import", ICON_IMPORT, TR("Import"), TH(TK_N14), false, TR("Copy files into this folder (or drop them from Explorer)")))
        import_dialog();
    igSameLine(0, 4);
    igBeginDisabled(!s_level.save_all);
    if (ui_icon_button("cb_save", ICON_SAVE, TR("Save All"), TH(TK_N14), false, TR("Save All (Ctrl+Shift+S): the level and every changed asset")) && s_level.save_all)
        s_level.save_all();
    igEndDisabled();
    igSameLine(0, 14);
    igBeginDisabled(S.hist_i <= 0);
    if (ui_icon_button("cb_back", ICON_BACK, NULL, TH(TK_N14), false, TR("Back (Alt+Left)"))) go_history(-1);
    igEndDisabled();
    igSameLine(0, 2);
    igBeginDisabled(S.hist_i >= S.hist_n - 1);
    if (ui_icon_button("cb_fwd", ICON_FORWARD, NULL, TH(TK_N14), false, TR("Forward (Alt+Right)"))) go_history(1);
    igEndDisabled();
    igSameLine(0, 10);
    igAlignTextToFramePadding();
    breadcrumb();
    const char* dock_label = docked ? TR("Undock") : TR("Dock in Layout");
    float bw = igCalcTextSize(dock_label, NULL, false, -1).x + igGetStyle()->FramePadding.x * 2;
    float cw = docked ? 0.0f : igGetFrameHeight() + 4.0f;
    igSameLine(0, 0);
    float rx = igGetCursorPosX() + igGetContentRegionAvail().x - bw - cw;
    if (rx > igGetCursorPosX()) igSetCursorPosX(rx);
    if (igButton(dock_label, (ImVec2){ 0, 0 })) *toggle_dock = true;
    if (!docked) {
        igSameLine(0, 4);
        if (igButton(TR("X##cb_close"), (ImVec2){ igGetFrameHeight(), 0 })) keep = false;
        igSetItemTooltip(TR("Close (Ctrl+Space)"));
    }
    igSeparator();

    // Folder tree
    float tree_w = igGetFontSize() * 13.0f;
    if (igBeginChild_Str("cb_tree", (ImVec2){ tree_w, 0 }, ImGuiChildFlags_Borders, 0)) {
        for (int i = 0; i < S.nfolders;) i = draw_tree_node(i);
    }
    igEndChild();
    igSameLine(0, 6);

    // Search and size, the tiles, the footer
    igBeginGroup();
    igSetNextItemWidth(-igGetFontSize() * 9.0f);
    char before[128];
    snprintf(before, sizeof before, "%s", S.filter);
    if (igInputTextWithHint("##cb_search", TR("Search (this folder and below)"), S.filter, sizeof S.filter, 0, NULL, NULL) &&
        strcmp(before, S.filter) != 0)
        S.dirty = true; // a new search reads the folders again (recursively), not every frame
    igSameLine(0, 6);
    igSetNextItemWidth(-1);
    igSliderFloat("##cb_size", &S.tile, 64.0f, 192.0f, TR("Size %.0f"), 0);

    float footer = igGetFrameHeightWithSpacing();
    if (igBeginChild_Str("cb_tiles", (ImVec2){ 0, -footer }, 0, 0)) {
        float tile = S.tile, gap = 8.0f, row_h = tile + igGetTextLineHeight() * 2.0f + 12.0f;
        float avail = igGetContentRegionAvail().x;
        int per_row = (int)((avail + gap) / (tile + gap));
        if (per_row < 1) per_row = 1;
        int rows = (S.nitems + per_row - 1) / per_row;
        ImVec2 origin = igGetCursorScreenPos();
        float scroll = igGetScrollY(), view_h = igGetWindowHeight();
        int r0 = (int)(scroll / row_h), r1 = (int)((scroll + view_h) / row_h) + 1;
        if (r1 > rows) r1 = rows;
        for (int r = r0 < 0 ? 0 : r0; r < r1; r++)
            for (int c = 0; c < per_row; c++) {
                int i = r * per_row + c;
                if (i >= S.nitems) break;
                draw_tile(i, origin.x + c * (tile + gap), origin.y + r * row_h, tile, row_h);
            }
        igSetCursorScreenPos(origin);
        igDummy((ImVec2){ avail, rows * row_h > 1 ? rows * row_h : 1 });
        if (S.nitems == 0) {
            igSetCursorScreenPos((ImVec2){ origin.x + 8, origin.y + 8 });
            igTextDisabled("%s", S.filter[0] ? TR("No results.") : editable ? TR("This folder is empty. Drop files here from Explorer, or Import.") : TR("Empty."));
        }
        // a click on the background clears the selection; its menu makes folders and imports
        if (igIsWindowHovered(0) && !igIsAnyItemHovered() && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false))
            memset(S.sel, 0, sizeof(bool) * MAX_ITEMS);
        if (igBeginPopupContextWindow("cb_bg_menu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            if (ui_menu_item(ICON_FOLDER, TR("New Folder"), NULL, false, editable)) new_folder();
            char imp[VPATH + 32];
            snprintf(imp, sizeof imp, TR("Import to %s..."), S.cur);
            if (ui_menu_item(ICON_IMPORT, imp, NULL, false, editable)) import_dialog();
            if (ui_menu_item(ICON_TEXTURE, TR("Import UDIM Folder..."), NULL, false, editable)) { // (Q18)
                char dir[1024] = "", dir_abs[VPATH + 64];
                to_abs(S.cur, dir_abs, sizeof dir_abs);
                if (platform_folder_dialog(NULL, TR("Import UDIM Folder..."), dir, sizeof dir) && !import_udim(dir_abs, dir))
                    message(TR("Could not import %s."), file_name(dir));
            }
            add_menus(editable);
            igSeparator();
            if (ui_menu_item(ICON_OPEN, TR("Show in Explorer"), NULL, false, editable)) show_selected_in_explorer(-1);
            if (ui_menu_item(ICON_REFRESH, TR("Refresh"), NULL, false, true)) content_mark_dirty();
            igEndPopup();
        }
    }
    igEndChild();
    int nsel = selected_count();
    igAlignTextToFramePadding();
    igTextDisabled(TR("%d items"), S.nitems);
    if (nsel) {
        igSameLine(0, 4);
        igTextDisabled(TR("(%d selected)"), nsel);
    }
    if (S.message[0]) {
        igSameLine(0, 16);
        igTextColored(THV(TK_WARN), "%s", S.message);
    }
    igEndGroup();

    S.focused = igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    keyboard();
    delete_modal();
    if (!docked && igIsKeyPressed_Bool(ImGuiKey_Escape, false) && !igGetIO_Nil()->WantTextInput) keep = false;
    igEnd();

    if (g_open_item >= 0 && g_open_item < S.nitems && S.items[g_open_item].kind >= ASSET_BLUEPRINT &&
        S.items[g_open_item].kind <= ASSET_DATA_ASSET)
        bp_editor_open(S.items[g_open_item].vpath); // its editor (Unreal's double click)
    if (g_open_item >= 0 && g_open_item < S.nitems &&
        (S.items[g_open_item].kind == ASSET_MATERIAL || S.items[g_open_item].kind == ASSET_MATERIAL_INSTANCE))
        mat_editor_open(S.items[g_open_item].vpath);
    if (g_open_item >= 0 && g_open_item < S.nitems && S.items[g_open_item].kind >= ASSET_SKELETAL_MESH &&
        S.items[g_open_item].kind <= ASSET_ANIM_MONTAGE)
        anim_editor_open(S.items[g_open_item].vpath);
    if (g_open_item >= 0 && g_open_item < S.nitems && (S.items[g_open_item].kind == ASSET_BEHAVIOR_TREE || S.items[g_open_item].kind == ASSET_BRAIN))
        ai_editor_open(S.items[g_open_item].vpath);
    if (g_open_item >= 0 && g_open_item < S.nitems && (S.items[g_open_item].kind == ASSET_TEXTURE_SOURCE || S.items[g_open_item].kind == ASSET_STATIC_MESH))
        asset_viewer_open(S.items[g_open_item].vpath); // (the Texture / Static Mesh viewers)
    if (g_open_item >= 0 && g_open_item < S.nitems && (S.items[g_open_item].kind == ASSET_LEVEL || S.items[g_open_item].kind == ASSET_INPUT_ACTION ||
                                                       S.items[g_open_item].kind == ASSET_INPUT_CONTEXT || S.items[g_open_item].kind == ASSET_MACRO_LIB ||
                                                       S.items[g_open_item].kind == ASSET_LEVEL_SEQUENCE ||
                                                       (S.items[g_open_item].kind >= ASSET_SOUND_WAVE && S.items[g_open_item].kind <= ASSET_QUEST) || S.items[g_open_item].kind == ASSET_PCG_GRAPH))
        content_open_asset(S.items[g_open_item].vpath);
    if (g_open_item >= 0 && g_open_item < S.nitems && S.items[g_open_item].kind == ASSET_FOLDER) {
        char vp[VPATH];
        snprintf(vp, VPATH, "%s", S.items[g_open_item].vpath);
        navigate(vp, true);
    }
    return keep;
}
