// The project windows (editor.h; notas-dev/projetos.md): the Project Browser (Unreal's: the recent projects with their
// thumbnails, the templates of a new one), Edit > Project Settings (the open project's .rproject) and
// Edit > Editor Preferences (%APPDATA%/RealEngine/editor.ini). Plain ImGui windows; nothing here runs while they are
// closed.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <GLFW/glfw3.h>

#include "upscale.h"
#include "theme.h"
#include "blueprint.h"
#include "online.h"
#include "steam.h"
#include "bp_preview.h"
#include "content.h"
#include "editor.h"
#include "i18n.h"
#include "platform.h"
#include "platform_files.h"
#include "project.h"
#include "graph_style.h"
#include "theme_editor.h"
#include "gameplay_tags.h"
#include "audio.h"
#include "render.h"
#include "input.h"
#include "ui_icons.h"
#include "brand.h"
#include "asset_viewer.h"
#include "vkctx.h"
#include "vr_hand.h"

// --- Thumbnails: a level's look as shapes (<project>/Saved/Thumbnail.rthumb), drawn by bp_preview like a class ---

#define THUMB_MAX 160
typedef struct ThumbRec {
    uint32_t shape;
    float ax[3][3], o[3], color[3], roughness, metallic;
} ThumbRec;

typedef struct Thumb {
    BpPreviewShape s[THUMB_MAX];
    uint32_t n;
    BpPreviewCam cam;
} Thumb;

static void thumb_path(const char* root, char* out, size_t cap) { snprintf(out, cap, "%s/Saved/Thumbnail.rthumb", root); }

static int size_desc(const void* a, const void* b) {
    float x = ((const float*)a)[0], y = ((const float*)b)[0];
    return x < y ? 1 : x > y ? -1 : 0;
}

void project_save_thumbnail(Editor* ed) {
    if (!project_file()[0]) return;
    // the biggest built-in shapes of the level (the imported meshes belong to the open project's renderer)
    static struct { float size; uint32_t ent; ThumbRec r; } all[8192];
    uint32_t n = 0;
    const uint64_t need = COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_TRANSFORM);
    for (uint32_t ai = 0; ai < ed->world.archetype_count; ai++) {
        const EcsArchetype* a = &ed->world.archetypes[ai];
        if ((a->mask & need) != need) continue;
        const CStaticMesh* m = ecs_arch_column(a, COMP_STATIC_MESH);
        const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        for (uint32_t i = 0; i < a->count && n < 8192; i++) {
            if (m[i].mesh >= SHAPE_COUNT) continue;
            ThumbRec* r = &all[n].r;
            float axes[3][3];
            rotator_axes(t[i].rotation, axes);
            for (int k = 0; k < 3; k++)
                for (int j = 0; j < 3; j++) r->ax[k][j] = axes[k][j] * t[i].scale[k];
            r->shape = m[i].mesh;
            memcpy(r->o, t[i].location, 12), memcpy(r->color, m[i].base_color, 12);
            r->roughness = m[i].roughness, r->metallic = m[i].metallic;
            all[n].size = fmaxf(fmaxf(fabsf(t[i].scale[0]), fabsf(t[i].scale[1])), fabsf(t[i].scale[2]));
            n++;
        }
    }
    qsort(all, n, sizeof all[0], size_desc);
    if (n > THUMB_MAX) n = THUMB_MAX;
    char root[600], dir[700], path[760];
    project_root(root, sizeof root);
    snprintf(dir, sizeof dir, "%s/Saved", root);
    re_make_dirs(dir);
    thumb_path(root, path, sizeof path);
    FILE* f = re_fopen(path, "wb");
    if (!f) return;
    fwrite("RTH1", 1, 4, f);
    fwrite(&n, 4, 1, f);
    for (uint32_t i = 0; i < n; i++) fwrite(&all[i].r, sizeof(ThumbRec), 1, f);
    fclose(f);
}

// The camera on the shapes; the floors (big planes) cut to what stands on them
static void thumb_frame(Thumb* t) {
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    bool any = false;
    for (int pass = 0; pass < 2 && !any; pass++) // the walls and big blocks out of the framing, unless nothing else
    for (uint32_t i = 0; i < t->n; i++) {
        if (t->s[i].shape == SHAPE_PLANE) continue;
        float big = 0.0f;
        for (int k = 0; k < 3; k++) big = fmaxf(big, sqrtf(t->s[i].ax[k][0] * t->s[i].ax[k][0] + t->s[i].ax[k][1] * t->s[i].ax[k][1] + t->s[i].ax[k][2] * t->s[i].ax[k][2]));
        if (pass == 0 && big > 8.0f) continue;
        for (int k = 0; k < 3; k++) {
            float e = 50.0f * (fabsf(t->s[i].ax[0][k]) + fabsf(t->s[i].ax[1][k]) + fabsf(t->s[i].ax[2][k]));
            mn[k] = fminf(mn[k], t->s[i].o[k] - e), mx[k] = fmaxf(mx[k], t->s[i].o[k] + e);
        }
        any = true;
    }
    if (!any) mn[0] = mn[1] = -300, mn[2] = 0, mx[0] = mx[1] = 300, mx[2] = 100;
    float half = fmaxf(0.7f * fmaxf(mx[0] - mn[0], mx[1] - mn[1]), 400.0f) / 50.0f;
    for (uint32_t i = 0; i < t->n; i++) {
        if (t->s[i].shape != SHAPE_PLANE) continue;
        memset(t->s[i].ax, 0, sizeof t->s[i].ax);
        t->s[i].ax[0][0] = t->s[i].ax[1][1] = half, t->s[i].ax[2][2] = 1.0f;
        t->s[i].o[0] = 0.5f * (mn[0] + mx[0]), t->s[i].o[1] = 0.5f * (mn[1] + mx[1]);
        for (int k = 0; k < 3; k++) t->s[i].color[k] = fminf(t->s[i].color[k], 0.07f); // a floor reads darker than the props
        t->s[i].roughness = 0.9f;
    }
    memset(&t->cam, 0, sizeof t->cam);
    bp_preview_frame_bounds(&t->cam, mn, mx);
}

static bool thumb_load(const char* rproject, Thumb* t) {
    t->n = 0;
    char root[600], path[760];
    snprintf(root, sizeof root, "%s", rproject);
    char* s = strrchr(root, '/');
    char* b = strrchr(root, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
    thumb_path(root, path, sizeof path);
    FILE* f = re_fopen(path, "rb");
    if (!f) return false;
    char magic[4];
    uint32_t n = 0;
    bool ok = fread(magic, 1, 4, f) == 4 && !memcmp(magic, "RTH1", 4) && fread(&n, 4, 1, f) == 1 && n <= THUMB_MAX;
    for (uint32_t i = 0; ok && i < n; i++) {
        ThumbRec r;
        if (fread(&r, sizeof r, 1, f) != 1 || r.shape >= SHAPE_COUNT) {
            ok = false;
            break;
        }
        BpPreviewShape* p = &t->s[t->n++];
        memset(p, 0, sizeof *p);
        p->shape = r.shape;
        memcpy(p->ax, r.ax, sizeof p->ax), memcpy(p->o, r.o, 12), memcpy(p->color, r.color, 12);
        p->roughness = r.roughness, p->metallic = r.metallic;
    }
    fclose(f);
    if (!ok) t->n = 0;
    thumb_frame(t);
    return ok;
}

// --- The templates' thumbnails: their levels in a few shapes ---

static void shp(Thumb* t, unsigned shape, float x, float y, float z, float sx, float sy, float sz, float pitch, uint32_t rgb) {
    if (t->n >= THUMB_MAX) return;
    BpPreviewShape* s = &t->s[t->n++];
    memset(s, 0, sizeof *s);
    s->shape = shape;
    float axes[3][3];
    rotator_axes((float[3]){ pitch, 0, 0 }, axes);
    const float sc[3] = { sx, sy, sz };
    for (int k = 0; k < 3; k++)
        for (int j = 0; j < 3; j++) s->ax[k][j] = axes[k][j] * sc[k];
    s->o[0] = x, s->o[1] = y, s->o[2] = z;
    s->color[0] = (float)((rgb >> 16) & 255) / 255.0f, s->color[1] = (float)((rgb >> 8) & 255) / 255.0f;
    s->color[2] = (float)(rgb & 255) / 255.0f;
    s->roughness = 0.55f;
}

static void character(Thumb* t, float x, float y, uint32_t rgb) {
    shp(t, SHAPE_CYLINDER, x, y, 70, 0.55f, 0.55f, 1.2f, 0, rgb);
    shp(t, SHAPE_SPHERE, x, y, 150, 0.4f, 0.4f, 0.4f, 0, 0xE0C0A0);
}

static void template_thumb(int tpl, Thumb* t) {
    t->n = 0;
    const uint32_t grey = 0x9E9EA3, blue = 0x406BBF;
    shp(t, SHAPE_PLANE, 0, 0, 0, 12, 12, 1, 0, 0x707070);
    switch (tpl) {
    case PROJECT_TEMPLATE_THIRD_PERSON:
        shp(t, SHAPE_CUBE, 350, 0, 100, 2, 3, 2, 0, grey);
        shp(t, SHAPE_CUBE, 170, 0, 45, 1.6f, 3, 0.3f, 18, grey);
        for (int i = 0; i < 4; i++) shp(t, SHAPE_CUBE, -300.0f + (float)i * 120.0f, -260, 50, 1, 1, 1, 0, blue);
        character(t, -80, 60, 0xC8C8D0);
        break;
    case PROJECT_TEMPLATE_FIRST_PERSON:
        shp(t, SHAPE_CUBE, 350, 0, 100, 2, 3, 2, 0, grey);
        for (int i = 0; i < 4; i++) shp(t, SHAPE_CUBE, 100.0f + (float)i * 60.0f, -220, 50, 1, 1, 1, 0, blue);
        shp(t, SHAPE_CUBE, -180, 60, 120, 1.6f, 0.25f, 0.3f, 0, 0x202020); // the weapon
        for (int i = 0; i < 3; i++) shp(t, SHAPE_SPHERE, -20.0f + (float)i * 120.0f, 40.0f - (float)i * 10.0f, 130.0f + (float)i * 10.0f, 0.15f, 0.15f, 0.15f, 0, 0xF28C1A);
        break;
    case PROJECT_TEMPLATE_TOP_DOWN:
        for (int i = 0; i < 3; i++) shp(t, SHAPE_CUBE, -300.0f + (float)i * 300.0f, 250.0f - (float)(i % 2) * 500.0f, 50, 1.5f, 1.5f, 1, 0, grey);
        character(t, 0, 0, 0xC8C8D0);
        character(t, 380, 300, 0xC03020);
        character(t, -350, -320, 0xC03020);
        break;
    case PROJECT_TEMPLATE_PHYSICS:
        for (int row = 0; row < 4; row++)
            for (int i = 0; i < 4 - row; i++) shp(t, SHAPE_CUBE, -150.0f + (float)i * 51.0f + (float)row * 25.5f, 0, 25.0f + (float)row * 50.5f, 0.5f, 0.5f, 0.5f, 0, 0x9E6B40);
        for (int k = 0; k < 3; k++) shp(t, SHAPE_SPHERE, 200, -150.0f + (float)k * 80.0f, 25, 0.5f, 0.5f, 0.5f, 0, 0xBF1F14);
        shp(t, SHAPE_CUBE, 150, 220, 30, 0.6f, 0.6f, 0.6f, 25, 0xD9CCB3);
        break;
    case PROJECT_TEMPLATE_RPG:
        for (int i = 0; i < 3; i++) shp(t, SHAPE_CUBE, -300.0f + (float)i * 300.0f, 300, 80, 1.8f, 1.5f, 1.6f, 0, 0xB89E7A);
        for (int i = 0; i < 4; i++) shp(t, SHAPE_CYLINDER, -250.0f + (float)i * 160.0f, -120, 20, 0.3f, 0.3f, 0.05f, 0, 0xFFC833);
        character(t, 0, 0, 0x4060C0);
        character(t, 330, -300, 0xC03020);
        break;
    case PROJECT_TEMPLATE_HORROR:
        shp(t, SHAPE_CUBE, 0, 150, 120, 5, 0.2f, 2.4f, 0, 0x403C38);
        shp(t, SHAPE_CUBE, 250, -60, 115, 0.15f, 1.8f, 2.3f, 0, 0x33241A);
        shp(t, SHAPE_CUBE, -150, -100, 30, 0.3f, 0.3f, 0.3f, 0, 0xFFC033);
        character(t, 120, -260, 0x202020);
        break;
    case PROJECT_TEMPLATE_VR: // (Q25) a table with cubes, a door, a platform, a player with two hands
        shp(t, SHAPE_CUBE, 150, 0, 40, 0.8f, 1.6f, 0.8f, 0, 0x6B4A2E);
        for (int i = 0; i < 3; i++) shp(t, SHAPE_CUBE, 150, -50.0f + (float)i * 50.0f, 92, 0.2f, 0.2f, 0.2f, 0, 0x2E9BD9);
        shp(t, SHAPE_CUBE, 450, -250, 110, 0.15f, 1.0f, 2.2f, 0, 0x8C6A4A);
        shp(t, SHAPE_CUBE, -350, 300, 20, 2.0f, 2.0f, 0.4f, 0, 0x5A5F66);
        character(t, -100, -60, 0x404858);
        break;
    case PROJECT_TEMPLATE_AR: // boxes placed on a floor, a phone looking at them
        for (int i = 0; i < 3; i++) shp(t, SHAPE_CUBE, -150.0f + (float)i * 150.0f, 40.0f - (float)(i % 2) * 120.0f, 15, 0.3f, 0.3f, 0.3f, 0, 0xD95914);
        shp(t, SHAPE_CUBE, 0, -320, 120, 0.08f, 0.75f, 1.5f, -20, 0x202024);
        break;
    case PROJECT_TEMPLATE_FIGHTING:
        shp(t, SHAPE_CUBE, 0, 0, 30, 6, 6, 0.6f, 0, 0x26408C);
        character(t, -120, 0, 0x4060C0);
        character(t, 120, 0, 0xC03020);
        break;
    default:
        shp(t, SHAPE_CUBE, 0, 0, 50, 1, 1, 1, 0, 0xB3B3B8);
        shp(t, SHAPE_CUBE, -260, 200, 1, 0.3f, 0.3f, 0.02f, 0, 0x3060D0); // the Player Start's spot
        break;
    }
    thumb_frame(t);
    t->cam.yaw += 10.0f;
    if (tpl == PROJECT_TEMPLATE_TOP_DOWN) t->cam.pitch = -60.0f;
}

// --- The Project Browser ---

static const char* const g_tpl_names[PROJECT_TEMPLATE_COUNT] = { "Blank", "Third Person", "First Person", "Top Down",
                                                                  "Physics Playground", "RPG (Demo)", "Horror (Demo)", "Fighting (Demo)",
                                                                  "Augmented Reality", "Virtual Reality" };
static const char* const g_tpl_tips[PROJECT_TEMPLATE_COUNT] = {
    "An empty level: a floor, the sun and the sky, a Player Start. No classes.",
    "A Character seen from behind: a camera on a Spring Arm that turns with the mouse, WASD to move, Space to jump. "
    "A course with a ramp, steps and boxes to push.",
    "The camera at the eyes, turned by the mouse. The left mouse button fires a BP_Projectile that bounces "
    "(Projectile Movement), made by the Event Graph.",
    "The camera high above at a fixed angle; WASD moves. Three guards (the Real Brain) patrol, see the player and chase.",
    "Rigid bodies: a pyramid of boxes, a tower, balls on a ramp and a box that breaks. A Third Person Character to "
    "walk among them.",
    "A complete little RPG: a village, coins, a merchant who trades potions for gold (E), bandits with AI that attack, "
    "a sword (Left Mouse) and potions (Q). Its logic: BP_RPGHero's Event Graph.",
    "A complete little horror game: a dark house at night with fog, a flashlight (F), keys that open locked doors, a "
    "monster with AI that hunts by sight and noise (running is loud), and the exit. Its logic: BP_HorrorPlayer's Event Graph.",
    "A complete little fighting game: a ring, a rival with AI, punch (Left Mouse), kick (Right Mouse), block (Left "
    "Shift), health and K.O. Its logic: BP_Fighter's Event Graph.",
    "A phone game in the real world (ARCore): the camera's picture behind the level, the real floor taking the shadows, "
    "the real things hiding the virtual ones. Touch the floor: BP_ARPlacer places a BP_ARObject there (Event Touch, AR "
    "Line Trace). Packaged for Android with AR on; --ar-sim on a PC.",
    "A VR room: teleport (left stick) and snap turns (right stick), hands, a laser that uses "
    "the door and the lever, cubes to grab and throw (grip), a ball held by its grip, a Grab Socket on the table. Start in "
    "VR on; Play > VR Preview, or Build > Package for Meta Quest.",
};

static struct {
    bool init;
    int page;          // 0 the recent projects, 1 the templates
    int sel_recent, sel_tpl;
    char recent[16][600];
    int nrecent;
    Thumb recent_thumb[16], tpl_thumb[PROJECT_TEMPLATE_COUNT];
    char location[512], name[64], error[256];
    int scalability;
    bool ray_tracing;
    int render_mode; // PROJECT_RENDER_*
} B;

static int g_new_mode = -1; // --render-mode (a test's new project: no browser)
void project_set_new_render_mode(int mode) { g_new_mode = mode; }

// The Rendering Mode and the game's scalability (the Project Settings' Rendering page, Package Game's Graphics): Quality
// takes Cinematic, Performance leaves it for Epic; the path tracer in the game only in Quality
// (2026-11-10) the Stylized style under the user's control: a preset to start from, then every part of the look (the
// light's bands, the colors, the outline, the animations' frame rate); the viewport shows each change at once
static bool stylized_settings(ProjectSettings* p) {
    bool ch = false;
    igIndent(igGetFontSize());
    igPushID_Str("stylized");
    igSeparatorText(TR("Stylized"));
    static const char* const presets[] = { "Cel (Anime)", "Comic (Ink Outline)", "Painted (Soft Bands)", "Stop-Motion (12 fps)", "Toon (Bright)" };
    static int pick = -1;
    const char* pl[5];
    for (int i = 0; i < 5; i++) pl[i] = TR(presets[i]);
    if (igCombo_Str_arr(TR("Start From Preset"), &pick, pl, 5, -1) && pick >= 0) {
        // bands, softness, saturation, shadow tint, outline px, its color, its depth step, animation fps
        static const struct { int b; float s, sat, tint[3], ol, olc[3], od; int fps; } P[5] = {
            { 3, 0.05f, 1.25f, { 0.85f, 0.85f, 1.0f }, 1.5f, { 0.05f, 0.04f, 0.06f }, 0.04f, 0 },
            { 2, 0.0f, 1.4f, { 0.75f, 0.7f, 0.85f }, 3.0f, { 0.0f, 0.0f, 0.0f }, 0.03f, 0 },
            { 6, 0.6f, 1.15f, { 0.9f, 0.85f, 1.0f }, 0.0f, { 0.1f, 0.08f, 0.06f }, 0.05f, 0 },
            { 4, 0.15f, 1.2f, { 0.9f, 0.88f, 0.95f }, 1.0f, { 0.04f, 0.03f, 0.03f }, 0.04f, 12 },
            { 3, 0.1f, 1.6f, { 1.0f, 0.9f, 0.95f }, 2.0f, { 0.08f, 0.04f, 0.1f }, 0.04f, 0 },
        };
        p->stylized_bands = P[pick].b, p->stylized_softness = P[pick].s, p->stylized_saturation = P[pick].sat;
        memcpy(p->stylized_shadow_tint, P[pick].tint, 12), memcpy(p->stylized_outline_color, P[pick].olc, 12);
        p->stylized_outline = P[pick].ol, p->stylized_outline_depth = P[pick].od, p->stylized_anim_fps = P[pick].fps;
        ch = true;
    }
    igTextDisabled("%s", TR("Light"));
    ch |= igSliderInt(TR("Bands"), &p->stylized_bands, 2, 12, "%d", 0);
    igSetItemTooltip("%s", TR("How many flat steps the light falls into (2: light and shadow only)."));
    ch |= igSliderFloat(TR("Band Softness"), &p->stylized_softness, 0.0f, 1.0f, "%.2f", 0);
    igSetItemTooltip("%s", TR("0: hard edges between the bands (cel shading); 1: each band eases into the next (painted)."));
    ch |= igColorEdit3(TR("Shadow Tint"), p->stylized_shadow_tint, ImGuiColorEditFlags_Float);
    igSetItemTooltip("%s", TR("The color the darker bands lean to (white: none; a cool blue or purple is the classic toon shadow)."));
    igTextDisabled("%s", TR("Color"));
    ch |= igSliderFloat(TR("Saturation"), &p->stylized_saturation, 0.0f, 2.5f, "%.2f", 0);
    igTextDisabled("%s", TR("Outline"));
    ch |= igSliderFloat(TR("Outline Width"), &p->stylized_outline, 0.0f, 6.0f, "%.1f px", 0);
    igSetItemTooltip("%s", TR("The ink line around the silhouettes, in pixels at 1440p (0: no outline)."));
    ch |= igColorEdit3(TR("Outline Color"), p->stylized_outline_color, ImGuiColorEditFlags_Float);
    ch |= igSliderFloat(TR("Outline Sensitivity"), &p->stylized_outline_depth, 0.005f, 0.5f, "%.3f", ImGuiSliderFlags_Logarithmic);
    igSetItemTooltip("%s", TR("How far apart in depth two surfaces must be for a line between them (a share of the distance: smaller draws more lines)."));
    igTextDisabled("%s", TR("Animation"));
    ch |= igSliderInt(TR("Animation Frame Rate"), &p->stylized_anim_fps, 0, 30, p->stylized_anim_fps ? "%d fps" : "smooth", 0);
    igSetItemTooltip("%s", TR("The characters' poses held for 1/N s (12: stop-motion, animation on twos); 0 smooth. The game itself keeps its frame rate."));
    igPopID();
    igUnindent(igGetFontSize());
    return ch;
}

static bool render_mode_fields(ProjectSettings* p) {
    bool ch = false;
    static const char* const modes[2] = { "Performance", "Quality" };
    const char* mt[2] = { TR(modes[0]), TR(modes[1]) };
    int m = p->render_mode == PROJECT_RENDER_QUALITY;
    if (igCombo_Str_arr(TR("Rendering Mode"), &m, mt, 2, -1)) {
        p->render_mode = m;
        if (m) p->game_scalability = 4;
        else if (p->game_scalability > 3) p->game_scalability = 3, p->game_path_tracing = false;
        ch = true;
    }
    const char* qt[RENDER_SCALABILITY_LEVELS];
    for (int i = 0; i < RENDER_SCALABILITY_LEVELS; i++) qt[i] = TR(render_scalability_name(i));
    ch |= igCombo_Str_arr(TR("Game Scalability"), &p->game_scalability, qt, RENDER_SCALABILITY_LEVELS, -1);
    { // the look, apart from the cost (render.h; the editor's viewport shows it at once)
        const char* rs[RENDER_STYLE_COUNT];
        for (int i = 0; i < RENDER_STYLE_COUNT; i++) rs[i] = TR(render_style_name(i));
        ch |= igCombo_Str_arr(TR("Render Style"), &p->render_style, rs, RENDER_STYLE_COUNT, -1);
        if (p->render_style == RENDER_STYLE_STYLIZED) ch |= stylized_settings(p);
        // (2026-11-10, "opções extras a todos os estilos") every style: the shadows' noise; Video-realistic: its lens
        if (igTreeNodeEx_Str(TR("Style Options"), ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* sd[4] = { TR("Low"), TR("Medium"), TR("High"), TR("Epic") };
            ch |= igCombo_Str_arr(TR("Shadow Denoise"), &p->shadow_denoise, sd, 4, -1);
            igSetItemTooltip("%s", TR("Filters the noise of the ray traced shadows of the local lights over a wider window and more frames. Low: sharper, noisier; Epic: smooth (a little softer contact). The ghosting stays away at every level."));
            if (p->render_style == RENDER_STYLE_VIDEO) {
                ch |= igSliderFloat(TR("Lens Distortion"), &p->video_distortion, 0.0f, 3.0f, "%.2f", 0);
                igSetItemTooltip("%s", TR("The lens's barrel distortion (1: as the style has it, 0: none)."));
                ch |= igSliderFloat(TR("Lens Fringes"), &p->video_fringe, 0.0f, 3.0f, "%.2f", 0);
                igSetItemTooltip("%s", TR("The color fringes toward the edges (chromatic aberration)."));
                ch |= igSliderFloat(TR("Halation"), &p->video_halation, 0.0f, 3.0f, "%.2f", 0);
                igSetItemTooltip("%s", TR("Film's red halo around bright lights."));
                ch |= igSliderFloat(TR("Vignette"), &p->video_vignette, 0.0f, 3.0f, "%.2f", 0);
                igSetItemTooltip("%s", TR("The corners darker, as through a real lens."));
            }
            igTreePop();
        }
        // (2026-11-10) the picture's noise: the grain (an effect: the Video-realistic camera's) and the traced light's denoiser
        ch |= igSliderFloat(TR("Film Grain"), &p->film_grain, 0.0f, 2.0f, "%.2f", 0);
        igSetItemTooltip("%s", TR("The film / sensor grain (Video-realistic, and the Post Process Volumes' Grain): 0 a clean picture, 1 as the style has it."));
        static const char* const dn[3] = { "Automatic", "On", "Off" };
        const char* dl[3] = { TR(dn[0]), TR(dn[1]), TR(dn[2]) };
        ch |= igCombo_Str_arr(TR("Denoise"), &p->image_denoise, dl, 3, -1);
        igSetItemTooltip("%s", TR("Filters the noise of the traced light (GI, rough reflections, soft shadows) over a few frames, keeping only the history of the same object, facing the same way, at the same depth: no ghosting. Automatic: in Photorealistic and Video-realistic. Needs ray tracing."));
    }
    ch |= igCheckbox(TR("Variable Rate Shading"), &p->vrs); // (A9.7b: render.c; the GPUs with VK_KHR_fragment_shading_rate)
    // (Q20, notas-dev/gi-estavel.md) no trail behind a moving actor: the TSR's and the denoiser's histories
    ch |= igCheckbox(TR("Reject Uncovered History"), &p->tsr_disocclusion);
    ch |= igCheckbox(TR("Denoiser History Fix"), &p->den_history_fix);
    ch |= igCheckbox(TR("Async Compute"), &p->async_compute); // (A9.7c: render.c; the GPUs with a compute-only queue)
    { // upscaling (upscale.h): the upscalers whose runtime is missing are shown greyed with why
        const char* ut[UPS_COUNT];
        for (int i = 0; i < UPS_COUNT; i++) ut[i] = upscale_kind_name(i);
        if (igBeginCombo(TR("Upscaler"), ut[p->upscaler >= 0 && p->upscaler < UPS_COUNT ? p->upscaler : 0], 0)) {
            for (int i = 0; i < UPS_COUNT; i++) {
                const char* why = NULL;
                bool on = upscale_available(i, &why);
                if (igSelectable_Bool(ut[i], p->upscaler == i, on ? 0 : ImGuiSelectableFlags_Disabled, (ImVec2){ 0, 0 })) p->upscaler = i, ch = true;
                if (!on && why && igIsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) igSetTooltip("%s", TR(why));
            }
            igEndCombo();
        }
        const char* um[UPM_COUNT];
        for (int i = 0; i < UPM_COUNT; i++) um[i] = TR(upscale_mode_name(i));
        ch |= igCombo_Str_arr(TR("Upscaling Mode"), &p->upscale_mode, um, UPM_COUNT, -1);
        ch |= igCheckbox(TR("Dynamic Resolution"), &p->dyn_res);
        igBeginDisabled(!p->dyn_res);
        ch |= igSliderInt(TR("Dynamic Resolution Target FPS"), &p->dyn_res_fps, 30, 240, "%d", 0);
        ch |= igSliderInt(TR("Dynamic Resolution Min"), &p->dyn_res_min, 0, 100, p->dyn_res_min > 0 ? "%d%%" : TR("Auto"), 0);
        igEndDisabled();
    }
    ch |= igCheckbox(TR("Support Hardware Ray Tracing"), &p->ray_tracing);
    igBeginDisabled(p->render_mode != PROJECT_RENDER_QUALITY || !p->ray_tracing);
    ch |= igCheckbox(TR("Path Tracing in the Game"), &p->game_path_tracing);
    igEndDisabled();
    return ch;
}

static void browser_refresh(void) {
    B.nrecent = project_recent(B.recent, 16);
    for (int i = 0; i < B.nrecent; i++) thumb_load(B.recent[i], &B.recent_thumb[i]);
    if (B.sel_recent >= B.nrecent) B.sel_recent = B.nrecent - 1;
}

static void browser_init(void) {
    B.init = true;
    char home[512];
    if (re_home_dir(home, sizeof home)) snprintf(B.location, sizeof B.location, "%s\\Documents\\Rivora Projects", home);
    else snprintf(B.location, sizeof B.location, "C:\\Rivora Projects");
    B.sel_tpl = PROJECT_TEMPLATE_THIRD_PERSON;
    B.scalability = 3, B.ray_tracing = true, B.render_mode = PROJECT_RENDER_PERFORMANCE;
    for (int i = 0; i < PROJECT_TEMPLATE_COUNT; i++) template_thumb(i, &B.tpl_thumb[i]);
    browser_refresh();
    B.page = B.nrecent ? 0 : 1;
}

static void base_name(const char* path, char* out, size_t cap) {
    const char* s = strrchr(path, '/');
    const char* b = strrchr(path, '\\');
    if (b > s) s = b;
    snprintf(out, cap, "%s", s ? s + 1 : path);
    char* dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

// Unreal's rule: a letter first, then letters, digits and '_', at most 20
static bool valid_name(const char* n) {
    size_t len = strlen(n);
    if (!len || len > 20 || !isalpha((unsigned char)n[0])) return false;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)n[i]) && n[i] != '_') return false;
    return true;
}

bool project_create(Editor* ed, const char* folder, const char* name, int tpl) {
    if (!valid_name(name)) return false;
    char dir[700], content[760], file[800];
    snprintf(dir, sizeof dir, "%s\\%s", folder, name);
    snprintf(file, sizeof file, "%s\\%s.rproject", dir, name);
    if (re_file_stat(file, NULL, NULL, NULL)) return false;
    snprintf(content, sizeof content, "%s\\Content", dir);
    if (!re_make_dirs(content)) return false;
    ProjectSettings* p = project();
    project_defaults(p);
    snprintf(p->name, sizeof p->name, "%s", name);
    snprintf(p->window_title, sizeof p->window_title, "%s", name);
    p->render_mode = g_new_mode >= 0 ? g_new_mode : B.init ? B.render_mode : PROJECT_RENDER_PERFORMANCE;
    p->game_scalability = g_new_mode >= 0 ? (g_new_mode ? 4 : 3) : B.init ? B.scalability : 3;
    p->ray_tracing = B.init ? B.ray_tracing : true;
    // the Quality mode's project opens the editor at Cinematic too (its look is the one the game will have)
    if (p->render_mode == PROJECT_RENDER_QUALITY) {
        if (g_new_mode >= 0 && ed->saved_scalability < 0) ed->saved_scalability = ed->scalability; // (a test's: not kept in editor.ini)
        editor_set_scalability(ed, 4, re_time_seconds());
    }
    if (!project_save(file)) return false;
    editor_open_project_later(ed, file, tpl);
    return true;
}

// A tile: the thumbnail, the name under it; true when clicked (double: *open). (2026-11-23, the user: "the project
// browser better, in the blue R's style") a card: rounded, a soft shadow, lifted a little and outlined in the accent
// under the mouse, the accent's border and glow when selected, the picture over a dark gradient, the name strong
static bool tile(const char* id, const char* label, Thumb* t, bool selected, float w, bool* open) {
    ImVec2 p0 = igGetCursorScreenPos();
    float th = w * 0.62f, h = th + igGetTextLineHeightWithSpacing() * 1.9f, r = igGetFontSize() * 0.55f;
    igPushID_Str(id);
    bool clicked = igInvisibleButton("##tile", (ImVec2){ w, h }, 0); // tips: none (a canvas or a tile drawn with its own name: no description over the work)
    bool hovered = igIsItemHovered(0);
    if (hovered && igIsMouseDoubleClicked_Nil(0)) *open = true;
    igPopID();
    ImDrawList* dl = igGetWindowDrawList();
    float lift = hovered && !selected ? 2.0f : 0.0f;
    ImVec2 p = { p0.x, p0.y - lift }, q = { p0.x + w, p0.y + h - lift };
    for (int k = 3; k >= 1; k--) // (its shadow: three soft layers under it)
        ImDrawList_AddRectFilled(dl, (ImVec2){ p.x - (float)k, p.y + (float)k * 1.5f }, (ImVec2){ q.x + (float)k, q.y + (float)k * 2.0f }, THA(TK_N0, 40), r + (float)k, 0);
    if (selected) ImDrawList_AddRect(dl, (ImVec2){ p.x - 3, p.y - 3 }, (ImVec2){ q.x + 3, q.y + 3 }, THA(TK_ACCENT, 70), r + 3.0f, 4.0f, 0); // (its glow)
    ImDrawList_AddRectFilled(dl, p, q, selected ? TH(TK_BG_SELECTED_DIM) : hovered ? TH(TK_BG_HOVER) : TH(TK_BG_RAISED), r, 0);
    ImDrawList_AddRectFilledMultiColor(dl, (ImVec2){ p.x + 1, p.y + 1 }, (ImVec2){ q.x - 1, p.y + th }, TH(TK_N4), TH(TK_N4), TH(TK_N2), TH(TK_N2));
    bp_preview_shapes(dl, p.x + 3, p.y + 3, q.x - 3, p.y + th, &t->cam, t->s, t->n, NULL, 0, -1, false);
    ImDrawList_AddLine(dl, (ImVec2){ p.x + 1, p.y + th }, (ImVec2){ q.x - 1, p.y + th }, selected ? TH(TK_ACCENT) : TH(TK_BORDER), selected ? 2.0f : 1.0f);
    ImVec2 ts = igCalcTextSize(label, NULL, true, w - 8.0f);
    ImDrawList_PushClipRect(dl, p, q, true);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + fmaxf((w - ts.x) * 0.5f, 6.0f), p.y + th + (h - th - ts.y) * 0.5f }, selected ? TH(TK_TEXT_STRONG) : TH(TK_TEXT), label, NULL);
    ImDrawList_PopClipRect(dl);
    ImDrawList_AddRect(dl, p, q, selected ? TH(TK_ACCENT) : hovered ? THA(TK_ACCENT, 140) : TH(TK_BORDER), r, selected ? 2.0f : 1.0f, 0);
    return clicked;
}
// (2026-11-23) the screen's main action in the accent (Create, Open), as the R's blue
static void primary_push(void) {
    igPushStyleColor_U32(ImGuiCol_Button, TH(TK_ACCENT));
    igPushStyleColor_U32(ImGuiCol_ButtonHovered, TH(TK_ACCENT_STRONG));
    igPushStyleColor_U32(ImGuiCol_ButtonActive, TH(TK_ACCENT_STRONG));
    igPushStyleColor_U32(ImGuiCol_Text, TH(TK_TEXT_ON_ACCENT));
    igPushStyleVar_Float(ImGuiStyleVar_FrameRounding, igGetFontSize() * 0.4f);
}
static void primary_pop(void) { igPopStyleVar(1), igPopStyleColor(4); }
// (2026-11-23) a left column's entry: its icon and name, the accent's bar and a soft fill when it is the page shown
static bool nav_item(UiIcon icon, const char* label, bool on) {
    float fs = igGetFontSize(), h = fs * 2.2f, w = igGetContentRegionAvail().x;
    ImVec2 p = igGetCursorScreenPos();
    igPushID_Str(label);
    bool clicked = igInvisibleButton("##nav", (ImVec2){ w, h }, 0); // tips: none (a page of the Project Browser, named on it)
    bool hov = igIsItemHovered(0);
    igPopID();
    ImDrawList* dl = igGetWindowDrawList();
    if (on || hov) ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + h }, on ? TH(TK_ACCENT_DIM) : TH(TK_BG_HOVER), fs * 0.4f, 0);
    if (on) ImDrawList_AddRectFilled(dl, (ImVec2){ p.x, p.y + h * 0.2f }, (ImVec2){ p.x + 3.0f, p.y + h * 0.8f }, TH(TK_ACCENT), 2.0f, 0);
    ui_icon(dl, icon, p.x + fs * 0.7f, p.y + (h - fs * 1.1f) * 0.5f, fs * 1.1f, on ? TH(TK_ACCENT) : TH(TK_TEXT_DIM));
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + fs * 2.4f, p.y + (h - igGetTextLineHeight()) * 0.5f }, on ? TH(TK_TEXT_STRONG) : TH(TK_TEXT), label, NULL);
    return clicked;
}

static void open_recent(Editor* ed, int i, double now) {
    char path[600];
    snprintf(path, sizeof path, "%s", B.recent[i]);
    editor_open_project_later(ed, path, -1); // (between frames: the loading screen; a failure: project_ui_open_failed)
    (void)now;
}

static void browser(Editor* ed, double now) {
    if (!B.init) browser_init();
    ImGuiViewport* vp = igGetMainViewport();
    igSetNextWindowPos(vp->Pos, ImGuiCond_Always, (ImVec2){ 0, 0 });
    igSetNextWindowSize(vp->Size, ImGuiCond_Always);
    igPushStyleColor_U32(ImGuiCol_WindowBg, TH(TK_BG_APP));
    igBegin("##project_browser", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    igPopStyleColor(1);
    float fs = igGetFontSize();
    // (2026-11-23) the header: a band from the accent's dim to the app's background, the blue R (the menu bar's mark: the
    // accent with its lighter top), the name and the version, the page's title
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 o = igGetCursorScreenPos(), wp = igGetWindowPos(), ws = igGetWindowSize();
    float band = fs * 4.6f;
    ImDrawList_AddRectFilledMultiColor(dl, wp, (ImVec2){ wp.x + ws.x, wp.y + band }, TH(TK_ACCENT_DIM), TH(TK_BG_APP), TH(TK_BG_APP), TH(TK_ACCENT_DIM));
    ImDrawList_AddLine(dl, (ImVec2){ wp.x, wp.y + band }, (ImVec2){ wp.x + ws.x, wp.y + band }, THA(TK_ACCENT, 90), 1.0f);
    void* mark = brand_mark();
    float ms = fs * 3.0f;
    if (mark) {
        ImTextureRef_c tex = { NULL, (ImTextureID)(uintptr_t)mark };
        ImVec2 a = { o.x + fs * 0.2f, o.y + fs * 0.1f }, b = { a.x + ms, a.y + ms };
        ImDrawList_AddImage(dl, tex, a, b, (ImVec2){ 0, 0 }, (ImVec2){ 1, 1 }, TH(TK_ACCENT));
        ImDrawList_AddImage(dl, tex, a, (ImVec2){ b.x, (a.y + b.y) * 0.5f }, (ImVec2){ 0, 0 }, (ImVec2){ 1, 0.5f }, THA(TK_TEXT_STRONG, 70));
    } else
        ui_icon(dl, ICON_GAMEPAD, o.x, o.y, ms, TH(TK_ACCENT));
    igSetCursorScreenPos((ImVec2){ o.x + ms + fs * 0.9f, o.y + fs * 0.05f });
    igPushFont(NULL, fs * 1.7f);
    igTextUnformatted("Rivora Engine", NULL);
    igSameLine(0, fs * 0.5f);
    igPushStyleColor_U32(ImGuiCol_Text, TH(TK_ACCENT));
    igTextUnformatted(RE_VERSION_STRING, NULL);
    igPopStyleColor(1);
    igPopFont();
    igSetCursorScreenPos((ImVec2){ o.x + ms + fs * 0.9f, o.y + fs * 2.05f });
    igTextDisabled("%s", TR(B.page == 0 ? "Open a recent project, or start one from a template." : "Pick a template, name the project and create it."));
    igSetCursorScreenPos((ImVec2){ o.x, wp.y + band + fs * 0.6f });

    ImVec2 avail = igGetContentRegionAvail();
    float bottom_h = fs * 6.2f, left_w = fs * 15.5f, right_w = fs * 20.0f;
    float mid_h = avail.y - bottom_h;
    bool open_now = false;
    // the categories (Unreal's left column)
    igBeginChild_Str("cats", (ImVec2){ left_w, mid_h }, 0, 0);
    igTextDisabled("%s", TR("RECENT PROJECTS"));
    char lbl[96];
    snprintf(lbl, sizeof lbl, "%s (%d)", TR("Recent Projects"), B.nrecent);
    if (nav_item(ICON_OPEN, lbl, B.page == 0)) B.page = 0;
    igDummy((ImVec2){ 0, fs * 0.6f });
    igTextDisabled("%s", TR("NEW PROJECT CATEGORIES"));
    if (nav_item(ICON_GAMEPAD, TR("Games"), B.page == 1)) B.page = 1;
    igEndChild();
    igSameLine(0, fs);
    // the tiles
    float grid_w = avail.x - left_w - right_w - fs * 2.0f;
    igBeginChild_Str("tiles", (ImVec2){ grid_w, mid_h }, 0, 0);
    float tw = fmaxf((float)ed->thumb_size * 2.4f, 160.0f);
    int per_row = (int)fmaxf(1.0f, floorf((grid_w - fs) / (tw + fs * 0.6f)));
    if (B.page == 0) {
        if (!B.nrecent) igTextWrapped("%s", TR("No recent projects. Pick a template under Games to create one, or Browse for a .rproject."));
        for (int i = 0; i < B.nrecent; i++) {
            char nm[128], id[16];
            base_name(B.recent[i], nm, sizeof nm);
            snprintf(id, sizeof id, "r%d", i);
            if (i % per_row) igSameLine(0, fs * 0.6f);
            if (tile(id, nm, &B.recent_thumb[i], B.sel_recent == i, tw, &open_now)) B.sel_recent = i;
            if (igBeginPopupContextItem(id, ImGuiPopupFlags_MouseButtonRight)) {
                if (ui_menu_item(ICON_CROSS, TR("Remove from the list"), NULL, false, true)) {
                    project_recent_remove(B.recent[i]);
                    browser_refresh();
                }
                if (ui_menu_item(ICON_OPEN, TR("Show in Explorer"), NULL, false, true)) {
                    char dir[600];
                    snprintf(dir, sizeof dir, "%s", B.recent[i]);
                    char* s = strrchr(dir, '/');
                    char* b = strrchr(dir, '\\');
                    if (b > s) s = b;
                    if (s) *s = 0;
                    re_show_in_explorer(dir);
                }
                igEndPopup();
            }
            if (i < B.nrecent && igIsItemHovered(ImGuiHoveredFlags_DelayNormal)) igSetTooltip("%s", B.recent[i]);
            if (open_now) {
                B.sel_recent = i;
                break;
            }
        }
    } else {
        for (int i = 0; i < PROJECT_TEMPLATE_COUNT; i++) {
            char id[16];
            snprintf(id, sizeof id, "t%d", i);
            if (i % per_row) igSameLine(0, fs * 0.6f);
            if (tile(id, TR(g_tpl_names[i]), &B.tpl_thumb[i], B.sel_tpl == i, tw, &open_now)) B.sel_tpl = i;
            if (open_now) {
                B.sel_tpl = i;
                break;
            }
        }
    }
    igEndChild();
    igSameLine(0, fs);
    // the description (Unreal's right column) and the project defaults
    igBeginChild_Str("desc", (ImVec2){ right_w, mid_h }, 0, 0);
    if (B.page == 1) {
        Thumb* t = &B.tpl_thumb[B.sel_tpl];
        ImVec2 p = igGetCursorScreenPos();
        bp_preview_shapes(igGetWindowDrawList(), p.x, p.y, p.x + right_w - 4, p.y + right_w * 0.62f, &t->cam, t->s, t->n, NULL, 0, -1, true);
        igDummy((ImVec2){ right_w - 4, right_w * 0.62f });
        igSeparatorText(TR(g_tpl_names[B.sel_tpl]));
        igTextWrapped("%s", TR(g_tpl_tips[B.sel_tpl]));
        igSeparatorText(TR("Project Defaults"));
        static const char* const modes[2] = { "Performance", "Quality" };
        const char* mt[2] = { TR(modes[0]), TR(modes[1]) };
        igSetNextItemWidth(right_w * 0.5f);
        if (igCombo_Str_arr(TR("Rendering Mode"), &B.render_mode, mt, 2, -1)) // (Quality: Cinematic; Performance: Epic)
            B.scalability = B.render_mode == PROJECT_RENDER_QUALITY ? 4 : B.scalability > 3 ? 3 : B.scalability;
        igTextDisabled("%s", TR(B.render_mode == PROJECT_RENDER_QUALITY ? "Cinema GI and reflections, ~90-120 fps; path tracing in the game"
                                                                    : "250+ fps: the base for VR and weaker PCs"));
        const char* qt[RENDER_SCALABILITY_LEVELS];
        for (int i = 0; i < RENDER_SCALABILITY_LEVELS; i++) qt[i] = TR(render_scalability_name(i));
        igSetNextItemWidth(right_w * 0.5f);
        igCombo_Str_arr(TR("Quality Preset"), &B.scalability, qt, RENDER_SCALABILITY_LEVELS, -1);
        igCheckbox(TR("Ray Tracing"), &B.ray_tracing);
    } else if (B.sel_recent >= 0 && B.sel_recent < B.nrecent) {
        Thumb* t = &B.recent_thumb[B.sel_recent];
        ImVec2 p = igGetCursorScreenPos();
        bp_preview_shapes(igGetWindowDrawList(), p.x, p.y, p.x + right_w - 4, p.y + right_w * 0.62f, &t->cam, t->s, t->n, NULL, 0, -1, true);
        igDummy((ImVec2){ right_w - 4, right_w * 0.62f });
        char nm[128];
        base_name(B.recent[B.sel_recent], nm, sizeof nm);
        igSeparatorText(nm);
        igTextWrapped("%s", B.recent[B.sel_recent]);
    }
    igEndChild();

    // the bottom: where and what (a new project), or open / browse
    igSeparator();
    if (B.error[0]) igTextColored(THV(TK_ERROR), "%s", B.error);
    else igDummy((ImVec2){ 0, igGetTextLineHeight() });
    float bw = fs * 9.0f;
    const char* close = project_file()[0] ? TR("Cancel") : TR("Continue without a project");
    float cw = fmaxf(bw, igCalcTextSize(close, NULL, true, -1).x + fs * 1.5f);
    if (B.page == 1) {
        igText("%s", TR("Project Location"));
        igSameLine(fs * 9.0f, -1);
        igSetNextItemWidth(avail.x * 0.45f);
        igInputText("##loc", B.location, sizeof B.location, 0, NULL, NULL);
        igSameLine(0, -1);
        if (igButton("...##loc", (ImVec2){ 0, 0 })) {
            char pick[512];
            snprintf(pick, sizeof pick, "%s", B.location);
            if (platform_folder_dialog(ed->window, TR("Project Location"), pick, sizeof pick)) {
                size_t l = strlen(pick);
                if (l && (pick[l - 1] == '\\' || pick[l - 1] == '/')) pick[l - 1] = 0;
                snprintf(B.location, sizeof B.location, "%s", pick);
            }
        }
        igText("%s", TR("Project Name"));
        igSameLine(fs * 9.0f, -1);
        igSetNextItemWidth(fs * 16.0f);
        if (!B.name[0]) snprintf(B.name, sizeof B.name, "MyProject");
        igInputText("##name", B.name, sizeof B.name, ImGuiInputTextFlags_CharsNoBlank, NULL, NULL);
        bool ok = valid_name(B.name);
        if (!ok) {
            igSameLine(0, fs);
            igTextColored(THV(TK_WARN), "%s", TR("A letter first, then letters, digits or _ (20 at most)."));
        }
        igSameLine(avail.x - bw - cw - fs, -1);
        igBeginDisabled(!ok);
        primary_push();
        bool create = igButton(TR("Create"), (ImVec2){ bw, fs * 1.9f }) || (open_now && ok);
        primary_pop();
        igEndDisabled();
        if (create) {
            B.error[0] = 0;
            if (!project_create(ed, B.location, B.name, B.sel_tpl))
                snprintf(B.error, sizeof B.error, TR("Could not create %s\\%s (does it exist already?)."), B.location, B.name);
            else
                browser_refresh();
        }
    } else {
        if (ui_icon_button("browse", ICON_OPEN, TR("Browse..."), TH(TK_N14), false, TR("Open a .rproject from anywhere"))) {
            char path[1024] = "";
            if (platform_file_dialog_ext(ed->window, false, TR("Rivora Engine Project"), "rproject", path, sizeof path)) {
                B.error[0] = 0;
                editor_open_project_later(ed, path, -1);
            }
        }
        igSameLine(avail.x - bw - cw - fs, -1);
        igBeginDisabled(B.sel_recent < 0 || B.sel_recent >= B.nrecent);
        primary_push();
        bool open_b = igButton(TR("Open"), (ImVec2){ bw, fs * 1.9f });
        primary_pop();
        if (open_b || open_now) {
            B.error[0] = 0;
            if (B.sel_recent >= 0 && B.sel_recent < B.nrecent) open_recent(ed, B.sel_recent, now);
        }
        igEndDisabled();
    }
    igSameLine(0, fs * 0.5f);
    igPushStyleVar_Float(ImGuiStyleVar_FrameRounding, fs * 0.4f);
    if (igButton(close, (ImVec2){ cw, fs * 1.9f })) ed->show_browser = false;
    igPopStyleVar(1);
    if (igIsKeyPressed_Bool(ImGuiKey_Escape, false) && project_file()[0]) ed->show_browser = false;
    igEnd();
}

// --- Project Settings ---

static int g_ps_page;

// A file picked from anywhere copied into the project's Content/<folder> ("/Content/<folder>/<name>" into out; false:
// cancelled or not copied)
static bool pick_into_content(Editor* ed, double now, const char* label, const char* exts, const char* folder, char* out, size_t cap) {
    char pick[1024] = "";
    if (!platform_file_dialog_ext(ed->window, false, label, exts, pick, sizeof pick)) return false;
    char root[600], dst[900], dir2[700];
    project_root(root, sizeof root);
    const char* base = strrchr(pick, '\\');
    const char* b2 = strrchr(pick, '/');
    if (b2 > base) base = b2;
    base = base ? base + 1 : pick;
    snprintf(dir2, sizeof dir2, "%s/Content/%s", root, folder);
    re_make_dirs(dir2);
    snprintf(dst, sizeof dst, "%s/%s", dir2, base);
    if (_stricmp(pick, dst)) re_remove_file(dst); // (a newer one of the same name replaces it)
    if (!re_copy_file(pick, dst)) {
        editor_status(ed, now, TR("Could not copy %s."), pick);
        return false;
    }
    snprintf(out, cap, "/Content/%s/%s", folder, base);
    content_mark_dirty();
    return true;
}

// The loading screen: an image or a video (one from outside the project is copied into Content/Splash: it goes with
// the game), the engine's intro before it (Project Settings > Packaging and Package Game)
static bool splash_fields(Editor* ed, double now) {
    ProjectSettings* p = project();
    bool ch = false;
    float fs = igGetFontSize();
    igSetNextItemWidth(fs * 22.0f);
    igInputText(TR("Splash Image or Video"), p->splash, sizeof p->splash, ImGuiInputTextFlags_ReadOnly, NULL, NULL);
    igSameLine(0, -1);
    if (igButton("...##splash", (ImVec2){ 0, 0 }))
        ch |= pick_into_content(ed, now, TR("Image or Video"), "png;jpg;jpeg;bmp;tga;mp4;wmv;mov;avi;mkv;webm", "Splash", p->splash, sizeof p->splash);
    igSameLine(0, -1);
    if (igButton(TR("Engine's"), (ImVec2){ 0, 0 })) p->splash[0] = 0, ch = true;
    igTextDisabled("%s", TR("Shown while the project opens and while the packaged game loads (its shaders, its level)."));
    ch |= igCheckbox(TR("Show the Engine Intro"), &p->show_intro);
    return ch;
}
static int g_key_wait = -1; // the key binding waiting for a key

static const char* key_name(int k) {
    static char buf[16];
    switch (k) {
    case GLFW_KEY_SPACE: return "Space";
    case GLFW_KEY_LEFT_SHIFT: return "Left Shift";
    case GLFW_KEY_RIGHT_SHIFT: return "Right Shift";
    case GLFW_KEY_LEFT_CONTROL: return "Left Ctrl";
    case GLFW_KEY_RIGHT_CONTROL: return "Right Ctrl";
    case GLFW_KEY_LEFT_ALT: return "Left Alt";
    case GLFW_KEY_UP: return "Up";
    case GLFW_KEY_DOWN: return "Down";
    case GLFW_KEY_LEFT: return "Left";
    case GLFW_KEY_RIGHT: return "Right";
    case GLFW_KEY_ENTER: return "Enter";
    case GLFW_KEY_TAB: return "Tab";
    case GLFW_KEY_BACKSPACE: return "Backspace";
    }
    if ((k >= GLFW_KEY_A && k <= GLFW_KEY_Z) || (k >= GLFW_KEY_0 && k <= GLFW_KEY_9)) return snprintf(buf, sizeof buf, "%c", (char)k), buf;
    if (k >= GLFW_KEY_F1 && k <= GLFW_KEY_F12) return snprintf(buf, sizeof buf, "F%d", k - GLFW_KEY_F1 + 1), buf;
    return snprintf(buf, sizeof buf, "#%d", k), buf;
}

// A key binding: a button with the key; clicked, it waits for the next key (Esc: unchanged)
static bool key_field(Editor* ed, const char* label, int* key, int id) {
    bool ch = false;
    igPushID_Int(id);
    char b[64];
    snprintf(b, sizeof b, "%s##k", g_key_wait == id ? TR("Press a key...") : key_name(*key));
    if (igButton(b, (ImVec2){ igGetFontSize() * 9.0f, 0 })) g_key_wait = id;
    igSameLine(0, -1);
    igText("%s", label);
    if (g_key_wait == id) {
        for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST; k++) {
            if (glfwGetKey(ed->window, k) != GLFW_PRESS) continue;
            if (k != GLFW_KEY_ESCAPE) *key = k, ch = true;
            g_key_wait = -1;
            break;
        }
    }
    igPopID();
    return ch;
}

// A Blueprint class field: the classes whose root is that native class (or any, for the Game Instance's)
static bool class_field(const char* label, char* value, size_t cap, const char* native) {
    bool ch = false;
    if (igBeginCombo(label, value[0] ? value : TR("None (the engine's)"), 0)) {
        if (igSelectable_Bool(TR("None (the engine's)"), !value[0], 0, (ImVec2){ 0, 0 })) value[0] = 0, ch = true;
        bp_find_name(BPA_CLASS, ""); // the Content folder scanned
        for (uint32_t i = 0; i < bp_asset_count(); i++) {
            BpAsset* a = bp_asset_at(i);
            if (!a || a->kind != BPA_CLASS || !bp_is_child_of(a, native)) continue;
            if (igSelectable_Bool(a->name, !strcmp(a->name, value), 0, (ImVec2){ 0, 0 })) snprintf(value, cap, "%s", a->name), ch = true;
        }
        igEndCombo();
    }
    return ch;
}

// A map field: a path under /Content, "Use Current" puts the open level's
static bool map_field(Editor* ed, const char* label, char* value, size_t cap) {
    bool ch = false;
    igPushID_Str(label);
    igSetNextItemWidth(igGetFontSize() * 18.0f);
    ch |= igInputText("##m", value, cap, 0, NULL, NULL);
    igSameLine(0, -1);
    const char* root = content_root();
    size_t rl = strlen(root);
    bool can = ed->level_path[0] && !_strnicmp(ed->level_path, root, rl);
    igBeginDisabled(!can);
    if (igButton(TR("Use Current"), (ImVec2){ 0, 0 })) {
        snprintf(value, cap, "/Content%s", ed->level_path + rl);
        for (char* c = value; *c; c++)
            if (*c == '\\') *c = '/';
        ch = true;
    }
    igEndDisabled();
    igSameLine(0, -1);
    igText("%s", label);
    igPopID();
    return ch;
}

static void project_settings(Editor* ed, double now) {
    igSetNextWindowSize((ImVec2){ 960, 640 }, ImGuiCond_FirstUseEver);
    if (!igBegin(TR("Project Settings"), &ed->show_project_settings, ImGuiWindowFlags_NoCollapse)) {
        igEnd();
        return;
    }
    ProjectSettings* p = project();
    float fs = igGetFontSize();
    static const char* const pages[] = { "Description", "Maps & Modes", "Physics", "Input", "Rendering", "Windows", "Packaging", "Online", "Audio", "Android", "Gameplay Tags", "VR" };
    static const char* const groups[] = { "Project", "Project", "Engine", "Engine", "Engine", "Platforms", "Project", "Engine", "Engine", "Platforms", "Project", "Platforms" };
    igBeginChild_Str("pages", (ImVec2){ fs * 12.0f, 0 }, ImGuiChildFlags_Borders, 0);
    const char* last = NULL;
    static const int order[] = { 0, 1, 6, 10, 2, 3, 8, 4, 7, 9, 5, 11 };
    for (int j = 0; j < (int)(sizeof order / sizeof order[0]); j++) {
        int i = order[j];
        if (!last || strcmp(last, groups[i])) igSeparatorText(TR(groups[i])), last = groups[i];
        if (igSelectable_Bool(TR(pages[i]), g_ps_page == i, 0, (ImVec2){ 0, 0 })) g_ps_page = i;
    }
    igEndChild();
    igSameLine(0, -1);
    igBeginChild_Str("page", (ImVec2){ 0, 0 }, 0, 0);
    if (project_file()[0]) igTextDisabled("%s", project_file());
    else igTextColored(THV(TK_WARN), "%s", TR("No project open: the changes last until the editor closes."));
    bool ch = false;
    igPushItemWidth(fs * 16.0f);
    switch (g_ps_page) {
    case 0:
        igSeparatorText(TR("About"));
        ch |= igInputText(TR("Project Name"), p->name, sizeof p->name, 0, NULL, NULL);
        ch |= igInputText(TR("Company Name"), p->company, sizeof p->company, 0, NULL, NULL);
        ch |= igInputText(TR("Project Version"), p->version, sizeof p->version, 0, NULL, NULL);
        ch |= igInputTextMultiline(TR("Description"), p->description, sizeof p->description, (ImVec2){ fs * 24.0f, fs * 5.0f }, 0, NULL, NULL);
        break;
    case 1:
        igSeparatorText(TR("Default Maps"));
        ch |= map_field(ed, TR("Editor Startup Map"), p->editor_startup_map, sizeof p->editor_startup_map);
        ch |= map_field(ed, TR("Game Default Map"), p->game_default_map, sizeof p->game_default_map);
        ch |= map_field(ed, TR("Main Menu Map"), p->main_menu_map, sizeof p->main_menu_map);
        igSeparatorText(TR("Pause Menu"));
        ch |= igCheckbox(TR("Pause Menu (Esc)"), &p->pause_menu);
        ch |= class_field(TR("Pause Menu Widget"), p->pause_menu_widget, sizeof p->pause_menu_widget, "User Widget");
        igSetNextItemWidth(fs * 8.0f);
        ch |= igInputText(TR("Pause Key in Play"), p->pie_pause_key, sizeof p->pie_pause_key, 0, NULL, NULL);
        igTextDisabled("%s", TR("Esc opens the pause menu in the packaged game; in the editor's Play Esc stops it, this key opens the menu."));
        igSeparatorText(TR("Save Game")); // (save_progress.h, notas-dev/salvar-progresso.md)
        ch |= class_field(TR("Save Game Class"), p->save_game_class, sizeof p->save_game_class, "Save Game");
        ch |= igDragFloat(TR("Auto Save Interval"), &p->autosave_interval, 5.0f, 0.0f, 7200.0f, p->autosave_interval > 0.0f ? "%.0f s" : TR("Off"), 0);
        ch |= igSliderInt(TR("Auto Save Slots"), &p->autosave_slots, 1, 5, "%d", 0);
        ch |= igCheckbox(TR("Auto Save on Level Change"), &p->autosave_on_level);
        ch |= igCheckbox(TR("Picture of the Screen in Saves"), &p->save_thumbnail);
        igTextDisabled("%s", TR("The Save Game Class's variables go into every Save Progress and from level to level (Get Progress Save Game)."));
        igSeparatorText(TR("Default Modes"));
        ch |= class_field(TR("Default GameMode"), p->game_mode, sizeof p->game_mode, "Game Mode");
        igIndent(fs);
        ch |= class_field(TR("Default Pawn Class"), p->default_pawn, sizeof p->default_pawn, "Pawn");
        ch |= class_field(TR("Player Controller Class"), p->player_controller, sizeof p->player_controller, "Player Controller");
        ch |= class_field(TR("HUD Class"), p->hud, sizeof p->hud, "HUD");
        ch |= class_field(TR("Game State Class"), p->game_state, sizeof p->game_state, "Game State");
        ch |= class_field(TR("Player State Class"), p->player_state, sizeof p->player_state, "Player State");
        igUnindent(fs);
        igSeparatorText(TR("Game Instance"));
        ch |= class_field(TR("Game Instance Class"), p->game_instance, sizeof p->game_instance, "Game Instance");
        igTextDisabled("%s", TR("The Default Pawn is placed at the Player Start when the level gives the player none."));
        break;
    case 2:
        igSeparatorText(TR("Constants"));
        ch |= igDragFloat(TR("Default Gravity Z"), &p->gravity_z, 5.0f, 0.0f, 10000.0f, "%.0f cm/s²", 0);
        igSameLine(0, -1);
        if (igButton(TR("Earth"), (ImVec2){ 0, 0 })) p->gravity_z = 980.0f, ch = true;
        igSeparatorText(TR("Simulation"));
        { // (Q8 leftover) the preset: the steps a second and the solver's substeps together (Box2D's own default is Medium)
            static const int rate[4] = { 60, 60, 90, 120 }, sub[4] = { 2, 4, 4, 4 };
            const char* qn[5] = { TR("Low"), TR("Medium"), TR("High"), TR("Epic"), TR("Custom") };
            int q = 4;
            for (int k = 0; k < 4; k++)
                if (p->physics_rate == rate[k] && p->solver_substeps == sub[k]) q = k;
            if (igCombo_Str_arr(TR("Physics Quality"), &q, qn, 5, -1) && q < 4) p->physics_rate = rate[q], p->solver_substeps = sub[q], ch = true;
        }
        ch |= igSliderInt(TR("Physics Rate"), &p->physics_rate, 30, 240, "%d Hz", 0);
        ch |= igSliderInt(TR("Max Substeps"), &p->max_substeps, 1, 16, "%d", 0);
        ch |= igSliderInt(TR("Solver Substeps"), &p->solver_substeps, 1, 8, "%d", 0); // (Q8: the soft step's substeps in each step)
        igTextDisabled("%s", TR("Fixed steps (Physics Rate a second); a slow frame runs at most Max Substeps of them (then the simulation slows down)."));
        break;
    case 3:
        igSeparatorText(TR("Axis Mappings"));
        ch |= key_field(ed, TR("MoveForward (+1)"), &p->key_forward, 1);
        ch |= key_field(ed, TR("MoveForward (-1)"), &p->key_back, 2);
        ch |= key_field(ed, TR("MoveRight (-1)"), &p->key_left, 3);
        ch |= key_field(ed, TR("MoveRight (+1)"), &p->key_right, 4);
        igSeparatorText(TR("Action Mappings"));
        ch |= key_field(ed, TR("Jump"), &p->key_jump, 5);
        igSeparatorText(TR("Mouse"));
        ch |= igSliderFloat(TR("Mouse Sensitivity"), &p->mouse_sensitivity, 0.02f, 1.0f, "%.2f °/px", 0);
        ch |= igCheckbox(TR("Invert Mouse Y"), &p->invert_mouse_y);
        igSeparatorText(TR("Enhanced Input"));
        if (igBeginCombo(TR("Default Mapping Context"), p->default_imc[0] ? p->default_imc : TR("None"), 0)) {
            if (igSelectable_Bool(TR("None"), !p->default_imc[0], 0, (ImVec2){ 0, 0 })) p->default_imc[0] = 0, ch = true;
            for (uint32_t i = 0; i < input_context_count(); i++) {
                const InputContext* c = input_context_at(i);
                if (igSelectable_Bool(c->name, !strcmp(c->name, p->default_imc), 0, (ImVec2){ 0, 0 }))
                    snprintf(p->default_imc, sizeof p->default_imc, "%s", c->name), ch = true;
            }
            igEndCombo();
        }
        igTextDisabled("%s", TR("Added when play starts (Blueprints add more with Add Mapping Context)."));
        break;
    case 4: {
        igSeparatorText(TR("Default Settings"));
        ch |= render_mode_fields(p);
        ch |= igCheckbox(TR("Allow Static Lighting"), &p->static_lighting);
        igSeparatorText(TR("Exposure"));
        ch |= igCheckbox(TR("Auto Exposure"), &p->auto_exposure);
        ch |= igSliderFloat(TR("Exposure Compensation"), &p->exposure_compensation, -5.0f, 5.0f, "%.1f EV", 0);
        break;
    }
    case 9: { // Platforms - Android (notas-dev/android.md)
        igSeparatorText(TR("APK Packaging"));
        ch |= igInputTextWithHint(TR("Android Package Name"), "com.company.game", p->android_package, sizeof p->android_package, 0, NULL, NULL);
        igTextDisabled("%s", TR("Empty: com.realengine.<game name>. Letters, digits and dots; it names the app on the device and the store."));
        const char* ors[3] = { TR("Landscape"), TR("Portrait"), TR("Either (sensor)") };
        ch |= igCombo_Str_arr(TR("Orientation"), &p->android_orientation, ors, 3, -1);
        ch |= igInputInt(TR("Version Code"), &p->android_version_code, 1, 1, 0);
        if (p->android_version_code < 1) p->android_version_code = 1;
        igSeparatorText(TR("Signing"));
        ch |= igInputTextWithHint(TR("Keystore"), TR("(empty: a debug key)"), p->android_keystore, sizeof p->android_keystore, 0, NULL, NULL);
        ch |= igInputText(TR("Key Alias"), p->android_key_alias, sizeof p->android_key_alias, 0, NULL, NULL);
        ch |= igInputText(TR("Key Password File"), p->android_key_pass_file, sizeof p->android_key_pass_file, 0, NULL, NULL);
        igTextDisabled("%s", TR("A store upload needs your own keystore; the debug key installs on your devices only."));
        igSeparatorText(TR("Performance"));
        const char* qs[4] = { TR("Automatic (by the device)"), TR("Low"), TR("Medium"), TR("High") };
        ch |= igCombo_Str_arr(TR("Mobile Quality"), &p->android_quality, qs, 4, -1);
        static const int fps_v[4] = { 30, 60, 90, 120 };
        const char* fps_s[4] = { "30", "60", "90", "120" };
        int fi = 1;
        for (int i = 0; i < 4; i++) if (p->android_target_fps == fps_v[i]) fi = i;
        if (igCombo_Str_arr(TR("Target Frame Rate"), &fi, fps_s, 4, -1)) p->android_target_fps = fps_v[fi], ch = true;
        ch |= igCheckbox(TR("Dynamic Resolution"), &p->android_dynamic_res);
        ch |= igSliderFloat(TR("Mobile Content Scale"), &p->android_content_scale, 0.0f, 1.0f, p->android_content_scale < 0.3f ? TR("Automatic") : "%.2f", 0);
        igTextDisabled("%s", TR("The game picks Low, Medium or High by the phone's GPU and memory, lowers the resolution to hold the frame rate, slows down before the phone overheats and drops to 30 fps on the battery saver."));
        igSeparatorText(TR("Touch"));
        ch |= igCheckbox(TR("Touch Controls"), &p->android_touch_controls);
        igTextDisabled("%s", TR("Adds WBP_TouchControls (a joystick, a look area, Jump, Interact, Sprint and Pause) on touch screens."));
        igSeparatorText(TR("Augmented Reality"));
        ch |= igCheckbox(TR("Augmented Reality (ARCore)"), &p->android_ar);
        if (p->android_ar) {
            igTextDisabled("%s", TR("The camera's picture behind the level, the phone tracked in the real world, the real floor receiving the shadows. Phones without Google Play Services for AR run the game without AR."));
            ch |= igCheckbox(TR("Occlusion by Real Objects"), &p->android_ar_occlusion);
        }
        igSeparatorText(TR("Google Play"));
        ch |= igCheckbox(TR("Raise the Version Code on each bundle"), &p->android_auto_version);
        ch |= igInputText(TR("Icon Background"), p->android_icon_bg, sizeof p->android_icon_bg, 0, NULL, NULL);
        ch |= igInputTextWithHint(TR("Privacy Policy URL"), "https://...", p->android_privacy_url, sizeof p->android_privacy_url, 0, NULL, NULL);
        if (igButton(TR("Create Upload Key"), (ImVec2){ 0, 0 })) editor_android_upload_key(ed, now);
        igSameLine(0, -1);
        igTextDisabled("%s", TR("Made once; keep a copy of it and of its .pass outside this PC."));
        { // the store's checklist: what the Play Console refuses or asks, checked here before the upload
            bool own_pkg = p->android_package[0] && strncmp(p->android_package, "com.realengine.", 15) && strncmp(p->android_package, "com.example", 11) &&
                           strncmp(p->android_package, "com.mycompany", 13);
            bool key = p->android_keystore[0] && re_file_stat(p->android_keystore, NULL, NULL, NULL) && p->android_key_pass_file[0];
            bool icon = p->icon[0] != 0;
            struct { bool ok; const char* what; } items[] = {
                { own_pkg, TR("Your own package name (com.yourcompany.game: it can never change after the first upload)") },
                { key, TR("Your upload key (Create Upload Key)") },
                { true, TR("Target SDK 36 (Android 16), 64-bit (arm64-v8a), 16 KB pages: the engine does it") },
                { true, TR("Android App Bundle (.aab); game files past 150 MB go in an install-time asset pack") },
                { icon, TR("The game's icon (Package Game > Icon): the launcher's and the store's 512 px one") },
                { p->android_privacy_url[0] != 0, TR("A privacy policy URL (asked in the Play Console for online games)") },
            };
            for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
                igTextColored(items[i].ok ? THV(TK_OK) : THV(TK_WARN), "%s", items[i].ok ? "OK" : "--");
                igSameLine(0, -1);
                igTextWrapped("%s", items[i].what);
            }
            igTextDisabled("%s", TR("Build > Package for Google Play Store: AAB writes the bundle, an APK to test and the listing's pictures (icon 512 px, feature graphic 1024 x 500)."));
        }
        igSeparatorText(TR("Requirements"));
        igTextDisabled("%s", TR("Android 11 or newer with Vulkan 1.3 (arm64). The Android SDK (build-tools 34+, platform 34) and a JDK on this PC."));
        break;
    }
    case 11: { // (Q25) Platforms - VR (xr.h, vr_pawn.h, notas-dev/xr.md)
        ProjectVr* v = &p->vr;
        igSeparatorText(TR("Start"));
        ch |= igCheckbox(TR("Start in VR"), &v->start_in_vr);
        igTextDisabled("%s", TR("The packaged game opens on the headset (OpenXR); with none connected it plays on the monitor."));
        ch |= igCheckbox(TR("PC VR (OpenXR)"), &v->pc);
        igSameLine(0, fs * 2.0f);
        ch |= igCheckbox(TR("Meta Quest (standalone)"), &v->quest);
        igTextDisabled("%s", TR("PC VR: Quest Link, Air Link, SteamVR. Meta Quest: Build > Package for Meta Quest (an APK installed on the headset)."));
        static const int hz_v[4] = { 0, 72, 90, 120 };
        const char* hz_s[4] = { TR("Headset's Own"), "72 Hz", "90 Hz", "120 Hz" };
        int hi = 0;
        for (int i = 0; i < 4; i++) if (v->refresh_hz == hz_v[i]) hi = i;
        if (igCombo_Str_arr(TR("Refresh Rate"), &hi, hz_s, 4, -1)) v->refresh_hz = hz_v[hi], ch = true;
        const char* tr_s[2] = { TR("Floor (Standing, Room Scale)"), TR("Seated") };
        ch |= igCombo_Str_arr(TR("Tracking Space"), &v->tracking, tr_s, 2, -1);
        if (v->tracking == VR_TRACK_SEATED) ch |= igDragFloat(TR("Seated Eye Height"), &v->seated_height, 1.0f, 60.0f, 200.0f, "%.0f cm", 0);
        igSeparatorText(TR("Locomotion"));
        const char* lo_s[3] = { TR("Teleport"), TR("Smooth"), TR("Teleport and Smooth") };
        ch |= igCombo_Str_arr(TR("Locomotion"), &v->locomotion, lo_s, 3, -1);
        const char* tu_s[3] = { TR("Snap Turn"), TR("Smooth Turn"), TR("No Turning") };
        ch |= igCombo_Str_arr(TR("Turning"), &v->turn, tu_s, 3, -1);
        if (v->turn == VR_TURN_SNAP) ch |= igSliderFloat(TR("Snap Turn Angle"), &v->snap_angle, 15.0f, 90.0f, "%.0f deg", 0);
        if (v->turn == VR_TURN_SMOOTH) ch |= igSliderFloat(TR("Turn Speed"), &v->turn_speed, 30.0f, 360.0f, "%.0f deg/s", 0);
        if (v->locomotion != VR_LOCO_TELEPORT) ch |= igSliderFloat(TR("Move Speed"), &v->move_speed, 50.0f, 600.0f, "%.0f cm/s", 0);
        if (v->locomotion != VR_LOCO_SMOOTH) ch |= igSliderFloat(TR("Teleport Range"), &v->teleport_range, 200.0f, 3000.0f, "%.0f cm", 0);
        ch |= igSliderFloat(TR("Comfort Vignette"), &v->vignette, 0.0f, 1.0f, v->vignette <= 0.0f ? TR("Off") : "%.2f", 0);
        ch |= igSliderFloat(TR("Teleport Fade"), &v->teleport_fade, 0.0f, 0.5f, v->teleport_fade <= 0.0f ? TR("Off") : "%.2f s", 0);
        ch |= igCheckbox(TR("Blink on Snap Turn"), &v->turn_fade);
        const char* dh_s[2] = { TR("Right"), TR("Left") };
        ch |= igCombo_Str_arr(TR("Dominant Hand"), &v->dominant_hand, dh_s, 2, -1);
        const char* md_s[2] = { TR("Head"), TR("Hand") };
        ch |= igCombo_Str_arr(TR("Smooth Move Direction"), &v->move_direction, md_s, 2, -1);
        igTextDisabled("%s", TR("Left stick: teleport (push and release) or walk; right stick: turn. The vignette narrows the view while moving: less motion sickness."));
        igSeparatorText(TR("Hands and Interaction"));
        ch |= igCheckbox(TR("Show Hands"), &v->hands);
        igSameLine(0, fs * 2.0f);
        ch |= igCheckbox(TR("Hand Tracking"), &v->hand_tracking);
        igSameLine(0, fs * 2.0f);
        ch |= igCheckbox(TR("Laser Pointer"), &v->laser);
        igSameLine(0, fs * 2.0f);
        ch |= igCheckbox(TR("Drag to Move"), &v->drag_locomotion);
        const char* gg_s[VR_GESTURE_MODES] = { TR("Fist"), TR("Pinch (Thumb to Index)"), TR("Any Finger to Index"), TR("Fist or Any Finger to Index") };
        ch |= igCombo_Str_arr(TR("Grab Gesture"), &v->grab_gesture, gg_s, VR_GESTURE_MODES, -1);
        ch |= igCombo_Str_arr(TR("Drag Gesture"), &v->drag_gesture, gg_s, VR_GESTURE_MODES, -1);
        ch |= igCheckbox(TR("Physical Hands"), &v->physical_hands);
        igSameLine(0, fs * 2.0f);
        igBeginDisabled(!v->physical_hands);
        ch |= igCheckbox(TR("Hands Collide with Objects"), &v->hand_objects);
        igEndDisabled();
        igSameLine(0, fs * 2.0f);
        ch |= igCheckbox(TR("Hands Push Bodies"), &v->hand_push);
        igSameLine(0, fs * 2.0f);
        ch |= igCheckbox(TR("Held Objects Collide"), &v->held_collide);
        const char* hc_s[3] = { TR("Off"), TR("Fade to Black"), TR("Push Back") };
        ch |= igCombo_Str_arr(TR("Head in Walls"), &v->head_collision, hc_s, 3, -1);
        ch |= igCheckbox(TR("Hide the Pawn's Body"), &v->hide_body);
        igSameLine(0, fs * 2.0f);
        ch |= igCheckbox(TR("Safety Net"), &v->safety_net);
        ch |= igSliderFloat(TR("Grab Reach"), &v->grab_reach, 2.0f, 40.0f, "%.0f cm", 0);
        igSameLine(0, fs * 2.0f);
        ch |= igCheckbox(TR("Distance Grab"), &v->distance_grab);
        ch |= igSliderFloat(TR("Throw Strength"), &v->throw_scale, 0.2f, 3.0f, "%.2fx", 0);
        ch |= igSliderFloat(TR("Haptics"), &v->haptics, 0.0f, 1.0f, v->haptics <= 0.0f ? TR("Off") : "%.2f", 0);
        ch |= igSliderFloat(TR("Laser Length"), &v->laser_length, 100.0f, 5000.0f, "%.0f cm", 0);
        ch |= igSliderFloat(TR("Menu Distance"), &v->menu_distance, 50.0f, 400.0f, "%.0f cm", 0);
        ch |= igSliderFloat(TR("Menu Width"), &v->menu_width, 40.0f, 400.0f, "%.0f cm", 0);
        igSeparatorText(TR("Hand Meshes"));
        ch |= igInputTextWithHint(TR("Left Hand Mesh"), TR("(the engine's hands)"), v->hand_mesh[0], sizeof v->hand_mesh[0], 0, NULL, NULL);
        ch |= igInputTextWithHint(TR("Right Hand Mesh"), TR("(the engine's hands)"), v->hand_mesh[1], sizeof v->hand_mesh[1], 0, NULL, NULL);
        ch |= igDragFloat(TR("Hand Mesh Scale"), &v->hand_mesh_scale, 0.01f, 0.05f, 10.0f, "%.2f", 0);
        ch |= igDragFloat3(TR("Hand Mesh Rotation"), v->hand_mesh_rot, 1.0f, -180.0f, 180.0f, "%.0f", 0);
        // (2026-11-20) the base hand (vr_hand.h): for a modeler and a rigger to start from, or used as it is
        if (igButton(TR("Export Base Hand (FBX)..."), (ImVec2){ 0, 0 })) {
            char dir[700] = "", err[300];
            if (platform_folder_dialog(ed->window, TR("Export Base Hand (FBX)..."), dir, sizeof dir)) {
                if (vrh_export_folder(dir, err, sizeof err)) editor_status(ed, now, TR("Base hand exported to %s (both hands, the poses, a read-me)"), dir);
                else editor_status(ed, now, TR("Could not export the base hand: %s"), err);
            }
        }
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("Writes RealEngine_BaseHand_L.fbx and _R.fbx: a skinned hand on OpenXR's 26 joints with the poses Open, Relaxed, Fist, Grip, Point, Pinch and Thumbs Up. Give them to a modeler and a rigger: keep the bones' names and the engine moves the new hand by them."));
        igSameLine(0, fs);
        if (igButton(TR("Use the Base Hand"), (ImVec2){ 0, 0 })) {
            extern bool vr_hand_use_base(Editor * ed, char vpaths[2][128], char* err, size_t cap);
            char vp[2][128], err[300];
            if (vr_hand_use_base(ed, vp, err, sizeof err)) {
                for (int h = 0; h < 2; h++) snprintf(v->hand_mesh[h], sizeof v->hand_mesh[h], "%s", vp[h]);
                v->hand_mesh_scale = 1.0f, v->hand_mesh_rot[0] = v->hand_mesh_rot[1] = v->hand_mesh_rot[2] = 0.0f;
                ch = true;
                editor_status(ed, now, TR("The base hand is the VR hands (/Content/VR/Hands)"));
            } else
                editor_status(ed, now, TR("Could not use the base hand: %s"), err);
        }
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("Imports the base hand into /Content/VR/Hands and sets it as both hands: tracked hands bend its fingers by the real joints, controllers by the trigger, the grip and the buttons."));
        igTextDisabled("%s", TR("A Static Mesh (.rmesh) drawn at each hand's grip, or a Skeletal Mesh (.rskm) whose bones carry OpenXR's joint names (the base hand's): its fingers follow the tracked joints or the controller."));
        igTextDisabled("%s", TR("Grip (or a fist) grabs what has an Interactable set to Grabbable (thrown with the hand's speed, snapped into Grab Sockets); the trigger (or a pinch) uses Interactables and clicks widgets under the laser; the index tip touching a door, lever or button uses it. Drag to Move: a hand closed in the air pulls the player along."));
        igSeparatorText(TR("Recording and Streaming"));
        const char* mm_s[3] = { TR("Both Eyes"), TR("One Eye"), TR("Spectator Camera") };
        ch |= igCombo_Str_arr(TR("Window Mirror"), &v->mirror_mode, mm_s, 3, -1);
        if (v->mirror_mode == VR_MIRROR_SPECTATOR) {
            ch |= igSliderFloat(TR("Spectator FOV"), &v->spectator_fov, 50.0f, 120.0f, "%.0f deg", 0);
            ch |= igSliderFloat(TR("Spectator Smoothing"), &v->spectator_smooth, 0.0f, 0.95f, "%.2f", 0);
        }
        igTextDisabled("%s", TR("What the PC's window shows while playing in VR: one eye filling the window (free), or a steady camera following the head (one more view drawn: the GPU's cost)."));
        igSeparatorText(TR("Quality and Performance"));
        const char* q_s[VR_QUALITY_COUNT] = { TR("Low"), TR("Medium"), TR("High"), TR("Epic"), TR("Quest (standalone)") };
        ch |= igCombo_Str_arr(TR("VR Quality"), &v->quality, q_s, VR_QUALITY_COUNT, -1);
        const char* fv_s[4] = { TR("Off"), TR("Low"), TR("Medium"), TR("High") };
        ch |= igCombo_Str_arr(TR("Foveated Rendering"), &v->foveation, fv_s, 4, -1);
        ch |= igCheckbox(TR("Dynamic Foveation"), &v->foveation_dynamic);
        ch |= igCheckbox(TR("Application SpaceWarp"), &v->spacewarp);
        ch |= igSliderFloat(TR("VR Render Scale"), &v->render_scale, 0.5f, 1.5f, "%.2f", 0);
        static const int ms_v[4] = { 1, 2, 4, 8 };
        const char* ms_s[4] = { "1x", "2x", "4x", "8x" };
        int mi = 2;
        for (int i = 0; i < 4; i++) if (v->msaa == ms_v[i]) mi = i;
        if (igCombo_Str_arr(TR("VR MSAA"), &mi, ms_s, 4, -1)) v->msaa = ms_v[mi], ch = true;
        ch |= igSliderInt(TR("Headroom Target"), &v->headroom_fps, 0, 240, v->headroom_fps <= 0 ? TR("The Display's") : "%d fps", 0);
        igTextDisabled("%s", TR("Every option here is also a console variable (vr.SnapAngle, vr.Locomotion, vr.PhysicalHands...): Set Console Variable changes it while playing."));
        igTextDisabled("%s", TR("Quest: lightmaps and the baked volume for the indirect light, MSAA 4x, no screen-space effects, both eyes sharing the frame's work. Dynamic Foveation shades the edges coarser only while the GPU is short of the headset's time."));
        // (Q25b) mixed reality (xr_mr.h): the passthrough, the room (Space Setup), the real depth
        igSeparatorText(TR("Mixed Reality"));
        ch |= igCheckbox(TR("Mixed Reality (Passthrough)"), &v->mr_enabled);
        igTextDisabled("%s", TR("The real room through the headset's cameras (Quest 3 / 3S / Pro), the Space Setup's room as actors that collide, the real depth hiding the virtual. PC by Link: Meta Horizon Link > Settings > Beta > Developer Runtime Features, then Passthrough and Spatial Data over Meta Quest Link."));
        if (v->mr_enabled) {
            ch |= igCheckbox(TR("Passthrough at Start"), &v->mr_passthrough);
            const char* bg_s[2] = { TR("The Room (the Level's Sky Gone)"), TR("The Level's Sky (Windows Only)") };
            ch |= igCombo_Str_arr(TR("Passthrough Background"), &v->mr_background, bg_s, 2, -1);
            ch |= igSliderFloat(TR("Passthrough Opacity"), &v->mr_opacity, 0.0f, 1.0f, "%.2f", 0);
            const char* st_s[MR_STYLE_COUNT] = { TR("Color"), TR("Black & White"), TR("One Color"), TR("Adjusted") };
            ch |= igCombo_Str_arr(TR("Passthrough Style"), &v->mr_style, st_s, MR_STYLE_COUNT, -1);
            if (v->mr_style == MR_STYLE_TINT) ch |= igColorEdit3(TR("Passthrough Tint"), v->mr_tint, ImGuiColorEditFlags_Float);
            if (v->mr_style == MR_STYLE_ADJUSTED) {
                ch |= igSliderFloat(TR("Passthrough Brightness"), &v->mr_brightness, -100.0f, 100.0f, "%.0f", 0);
                ch |= igSliderFloat(TR("Passthrough Contrast"), &v->mr_contrast, 0.0f, 3.0f, "%.2f", 0);
                ch |= igSliderFloat(TR("Passthrough Saturation"), &v->mr_saturation, 0.0f, 3.0f, "%.2f", 0);
            }
            ch |= igColorEdit4(TR("Passthrough Edge Color"), v->mr_edge, ImGuiColorEditFlags_Float);
            ch |= igSliderFloat(TR("Virtual Opacity"), &v->mr_virtual_opacity, 0.0f, 1.0f, "%.2f", 0);
            ch |= igCheckbox(TR("Load the Room"), &v->mr_scene);
            igSameLine(0, fs * 2.0f);
            ch |= igCheckbox(TR("Room Actors"), &v->mr_scene_actors);
            igSameLine(0, fs * 2.0f);
            ch |= igCheckbox(TR("Room Mesh"), &v->mr_room_mesh);
            ch |= igCheckbox(TR("Ask for the Room Setup"), &v->mr_ask_scan);
            igSameLine(0, fs * 2.0f);
            ch |= igCheckbox(TR("Align the Level to the Room"), &v->mr_align);
            igSameLine(0, fs * 2.0f);
            ch |= igCheckbox(TR("Real Windows and Doors"), &v->mr_windows_real);
            const char* oc_s[3] = { TR("Off"), TR("The Room's Planes and Furniture"), TR("Real Depth") };
            ch |= igCombo_Str_arr(TR("Real World Occlusion"), &v->mr_occlusion, oc_s, 3, -1);
            if (v->mr_occlusion != MR_OCC_NONE) ch |= igSliderFloat(TR("Occlusion Softness"), &v->mr_occlusion_soft, 0.5f, 30.0f, "%.1f cm", 0);
            if (v->mr_occlusion == MR_OCC_DEPTH) {
                ch |= igCheckbox(TR("Hand Removal"), &v->mr_hand_removal);
                igSameLine(0, fs * 2.0f);
                ch |= igCheckbox(TR("Real Depth Rows From the Bottom"), &v->mr_depth_flip);
            }
            ch |= igSliderFloat(TR("Shadows on the Room"), &v->mr_shadows, 0.0f, 1.0f, v->mr_shadows <= 0.0f ? TR("Off") : "%.2f", 0);
            const char* li_s[2] = { TR("A Room's Light"), TR("The Level's Lights") };
            ch |= igCombo_Str_arr(TR("Mixed Reality Lighting"), &v->mr_light, li_s, 2, -1);
            ch |= igSliderFloat(TR("Mixed Reality Exposure"), &v->mr_ev100, 0.0f, 16.0f, v->mr_ev100 <= 0.0f ? TR("Automatic") : "EV %.1f", 0);
            ch |= igCheckbox(TR("Hide the Level's Floor"), &v->mr_hide_floor);
            igTextDisabled("%s", TR("Blueprints: Set Passthrough Enabled / Opacity / Style / Background / Blend / Window, Get Room Planes, Line Trace Real World, Event On Room Loaded. Console: vr.Passthrough*, vr.MR*. Without a headset, Play > VR Preview's simulated headset draws a stand-in room."));
        }
        igSeparatorText(TR("Requirements"));
        igTextDisabled("%s", TR("PC VR: an OpenXR runtime (Meta Horizon Link, SteamVR). Quest: developer mode on and the headset allowed in adb (USB debugging)."));
        break;
    }
    case 8: { // Engine - Audio
        igSeparatorText(TR("Mixing"));
        ch |= igSliderInt(TR("Max Channels"), &p->audio_max_channels, 8, 256, "%d", 0);
        igTextDisabled("%s", TR("The voices mixed at once; the quietest beyond them are virtual (their time runs, nothing mixed)."));
        ch |= igSliderFloat(TR("Master Volume"), &p->audio_master_volume, 0.0f, 1.0f, "%.2f", 0);
        igSeparatorText(TR("Spatialization"));
        ch |= igCheckbox(TR("Binaural (Headphones)"), &p->audio_binaural);
        igTextDisabled("%s", TR("3D sounds through the head's delay and shadow between the ears; off: panning (speakers)."));
        igTextDisabled("%s", TR("Surround outputs (5.1, 7.1) are mixed in their own speakers (VBAP, the subwoofer's bleed); binaural is for stereo."));
        AudioStats st;
        audio_stats(&st);
        igSeparatorText(TR("Device"));
        igText("%s  |  %.3f ms / %.1f ms  |  %u %s, %u %s", st.device, st.mix_ms, st.period_ms, st.real, TR("real"), st.virt, TR("virtual"));
        if (ch) project_apply_audio();
        break;
    }
    case 5:
        igSeparatorText(TR("Window"));
        ch |= igInputText(TR("Window Title"), p->window_title, sizeof p->window_title, 0, NULL, NULL);
        ch |= igInputInt(TR("Resolution Width"), &p->resolution_w, 0, 0, 0);
        ch |= igInputInt(TR("Resolution Height"), &p->resolution_h, 0, 0, 0);
        ch |= igCheckbox(TR("Fullscreen"), &p->fullscreen);
        ch |= igCheckbox(TR("VSync"), &p->vsync);
        ch |= igCheckbox(TR("Capture the Mouse"), &p->mouse_capture);
        ch |= igCheckbox(TR("HDR Output (HDR10)"), &p->hdr_output); // (A9.4: when the player's display has it)
        if (p->hdr_output) {
            if (p->hdr_paper <= 0.0f) p->hdr_paper = 200.0f;
            if (p->hdr_peak <= 0.0f) p->hdr_peak = 1000.0f;
            ch |= igDragFloat(TR("Paper White (nits)"), &p->hdr_paper, 1.0f, 80.0f, 500.0f, "%.0f", 0);
            ch |= igDragFloat(TR("Peak Brightness (nits)"), &p->hdr_peak, 10.0f, 400.0f, 10000.0f, "%.0f", 0);
        }
        if (p->resolution_w < 320) p->resolution_w = 320;
        if (p->resolution_h < 200) p->resolution_h = 200;
        break;
    case 6:
        igSeparatorText(TR("Packaging"));
        igSetNextItemWidth(fs * 22.0f);
        ch |= igInputText(TR("Staging Directory"), p->package_dir, sizeof p->package_dir, 0, NULL, NULL);
        igSameLine(0, -1);
        if (igButton("...##pkg", (ImVec2){ 0, 0 })) {
            char pick[512];
            snprintf(pick, sizeof pick, "%s", p->package_dir);
            if (platform_folder_dialog(ed->window, TR("Staging Directory"), pick, sizeof pick)) snprintf(p->package_dir, sizeof p->package_dir, "%s", pick), ch = true;
        }
        ch |= igCheckbox(TR("Include Every Asset of Content"), &p->include_all_assets);
        igTextDisabled("%s", TR("Off: only what the levels and classes use."));
        igSeparatorText(TR("Loading Screen"));
        ch |= splash_fields(ed, now);
        break;
    case 7: { // Online (online.h): Steam, the session, the network emulation for Play
        igSeparatorText(TR("Steam"));
        ch |= igCheckbox(TR("Use Steam"), &p->online_steam);
        int app = (int)p->steam_app_id;
        if (igInputInt(TR("Steam App ID"), &app, 1, 100, 0)) p->steam_app_id = (uint32_t)(app < 0 ? 0 : app), ch = true;
        ch |= igCheckbox(TR("Saves in Steam Cloud"), &p->steam_cloud);
        igTextDisabled("%s", TR("480 (Spacewar) works for tests. The game needs steam_api64.dll from the Steamworks SDK next to its .exe, and Steam open."));
        igTextDisabled("%s: %s", TR("Steam now"), steam_available() ? TR("connected") : steam_status());
        igSeparatorText(TR("Session"));
        ch |= igInputInt(TR("Port (LAN / IP)"), &p->net_port, 1, 10, 0);
        ch |= igSliderInt(TR("Max Players"), &p->net_max_players, 2, ONLINE_MAX_PLAYERS, "%d", 0);
        ch |= igSliderInt(TR("Tick Rate"), &p->net_tick_rate, 10, 120, "%d Hz", 0);
        ch |= igSliderFloat(TR("Interpolation Delay"), &p->net_interp_ms, 33.0f, 300.0f, "%.0f ms", 0);
        ch |= igSliderFloat(TR("Relevancy Distance"), &p->net_relevancy_m, 0.0f, 2000.0f, p->net_relevancy_m > 0.0f ? "%.0f m" : TR("everything"), 0);
        ch |= igSliderInt(TR("Bandwidth per Client"), &p->net_budget_kbps, 0, 20000, p->net_budget_kbps > 0 ? "%d kbps" : TR("no limit"), 0);
        igSeparatorText(TR("Network Emulation (Play)"));
        ch |= igSliderFloat(TR("Added Latency"), &p->emu_latency_ms, 0.0f, 500.0f, "%.0f ms", 0);
        ch |= igSliderFloat(TR("Jitter"), &p->emu_jitter_ms, 0.0f, 100.0f, "%.0f ms", 0);
        float loss = p->emu_loss * 100.0f;
        if (igSliderFloat(TR("Packet Loss"), &loss, 0.0f, 50.0f, "%.0f %%", 0)) p->emu_loss = loss * 0.01f, ch = true;
        igTextDisabled("%s", TR("Makes the network worse on purpose while playing in the editor, to see the game under lag and lost packets."));
        if (ch) online_apply_project();
        break;
    }
    case 10: // Project - Gameplay Tags (gameplay_tags.h: its own file, Config/DefaultGameplayTags.ini)
        igPopItemWidth();
        gt_settings_page(&ed->world);
        igPushItemWidth(fs * 16.0f);
        break;
    }
    igPopItemWidth();
    igEndChild();
    igEnd();
    if (ch && project_file()[0] && !project_save(NULL)) editor_status(ed, now, TR("Could not save %s."), project_file());
}

// --- Editor Preferences ---

// (2026-11-10) Graph Style: the nodes' styles and the wires' styles as two rows of live previews; a click picks one (the
// open graphs change at once). Each row's previews use the other row's current choice, so a pair is seen before it is kept
static bool graph_style_gallery(void) {
    bool ch = false;
    float fs = igGetFontSize(), tw = fs * 11.0f, th = fs * 6.0f;
    for (int row = 0; row < 2; row++) {
        igSeparatorText(TR(row == 0 ? "Node Style" : "Wire Style"));
        int count = row == 0 ? GS_NODE_COUNT : GS_LINK_COUNT;
        int cur = row == 0 ? graph_node_style() : graph_link_style();
        int per_row = (int)((igGetContentRegionAvail().x + 10.0f) / (tw + 10.0f));
        if (per_row < 1) per_row = 1;
        for (int i = 0; i < count; i++) {
            igPushID_Int(row * 100 + i);
            if (i % per_row) igSameLine(0, 10.0f);
            igBeginGroup();
            ImVec2 p0 = igGetCursorScreenPos(), p1 = { p0.x + tw, p0.y + th };
            ImDrawList* dl = igGetWindowDrawList();
            if (igInvisibleButton("##style", (ImVec2){ tw, th }, 0)) {
                if (row == 0) graph_set_styles(i, graph_link_style());
                else graph_set_styles(graph_node_style(), i);
                ch = true;
            }
            bool hov = igIsItemHovered(0);
            ImDrawList_PushClipRect(dl, p0, p1, true);
            graph_style_preview(dl, p0, p1, row == 0 ? i : graph_node_style(), row == 0 ? graph_link_style() : i);
            ImDrawList_PopClipRect(dl);
            ImDrawList_AddRect(dl, p0, p1, i == cur ? TH(TK_ACCENT) : hov ? TH(TK_N12) : TH(TK_N8), 4.0f, i == cur ? 3.0f : 1.0f, 0);
            const char* nm = TR(row == 0 ? graph_node_style_name(i) : graph_link_style_name(i));
            ImVec2 ts = igCalcTextSize(nm, NULL, false, -1);
            igSetCursorScreenPos((ImVec2){ p0.x + (tw - ts.x) * 0.5f, p1.y + 3.0f });
            if (i == cur) igTextColored(THV(TK_INFO), "%s", nm);
            else igTextUnformatted(nm, NULL);
            igEndGroup();
            igPopID();
        }
    }
    igSpacing();
    igTextDisabled("%s", TR("Used by the Blueprint graphs (nodes and wires) and the wires of the Material, Animation, Sound Cue and Dialogue graphs."));
    return ch;
}

static int g_pref_page;
void project_ui_pref_page(int page) { g_pref_page = page; }

static void preferences(Editor* ed, double now) {
    igSetNextWindowSize((ImVec2){ 900, 600 }, ImGuiCond_FirstUseEver);
    if (!igBegin(TR("Editor Preferences"), &ed->show_prefs, ImGuiWindowFlags_NoCollapse)) {
        igEnd();
        return;
    }
    float fs = igGetFontSize();
    static const char* const pages[] = { "General", "Viewport", "Performance", "Play", "Keyboard Shortcuts", "Graph Style", "Appearance" };
    igBeginChild_Str("pages", (ImVec2){ fs * 12.0f, 0 }, ImGuiChildFlags_Borders, 0);
    igSeparatorText(TR("General"));
    for (int i = 0; i < 7; i++)
        if (igSelectable_Bool(TR(pages[i]), g_pref_page == i, 0, (ImVec2){ 0, 0 })) g_pref_page = i;
    igEndChild();
    igSameLine(0, -1);
    igBeginChild_Str("page", (ImVec2){ 0, 0 }, 0, 0);
    bool ch = false;
    igPushItemWidth(fs * 14.0f);
    switch (g_pref_page) {
    case 5: ch |= graph_style_gallery(); break;
    case 6: ch |= theme_editor_page(ed->window); break; // (Q1: the themes and the Theme Editor)
    case 0: {
        igSeparatorText(TR("Region & Language"));
        int lang = i18n_language();
        const char* names[LANG_COUNT];
        for (int l = 0; l < LANG_COUNT; l++) names[l] = i18n_language_name(l);
        if (igCombo_Str_arr(TR("Editor Language"), &lang, names, LANG_COUNT, -1)) i18n_set_language(lang), ch = true;
        igSeparatorText(TR("Appearance"));
        ch |= igSliderFloat(TR("UI Scale"), &ed->ui_scale, 0.6f, 2.5f, "%.2f", 0);
        igSameLine(0, -1);
        if (igButton(TR("Reset"), (ImVec2){ 0, 0 })) ed->ui_scale = 1.0f, ch = true;
        ch |= igSliderInt(TR("Thumbnail Size"), &ed->thumb_size, 48, 256, "%d px", 0);
        igSeparatorText(TR("Loading & Saving"));
        ch |= igSliderInt(TR("Auto Save (minutes)"), &ed->autosave_minutes, 0, 60, ed->autosave_minutes ? "%d" : TR("Off"), 0);
        ch |= igCheckbox(TR("Show the Project Browser on start"), &ed->browser_on_start);
        ch |= igCheckbox(TR("Confirm before quitting with unsaved changes"), &ed->confirm_quit);
        break;
    }
    case 1:
        igSeparatorText(TR("Camera"));
        ch |= igSliderFloat(TR("Camera Speed"), &ed->cam.speed, 50.0f, 6000.0f, "%.0f cm/s", ImGuiSliderFlags_Logarithmic);
        if (igSliderFloat(TR("Field of View"), &ed->cam_fov, 30.0f, 150.0f, "%.0f°", 0)) ed->cam.fov_deg = ed->cam_fov, ch = true;
        ch |= igSliderFloat(TR("Mouse Sensitivity"), &ed->mouse_sens, 0.02f, 1.0f, "%.2f", 0);
        ch |= igCheckbox(TR("Invert Mouse Y"), &ed->invert_y);
        igSeparatorText(TR("Grid Snapping"));
        ch |= igDragFloat(TR("Move Step"), &ed->snap_move_step, 0.5f, 0.1f, 10000.0f, "%.1f cm", 0);
        ch |= igSliderFloat(TR("Gizmo Size"), &ed->gizmo_size, 40.0f, 300.0f, "%.0f px", 0); // (2026-11-23, the Tlender's prefs.gizmo_size; +/- while dragging)
        ch |= igDragFloat(TR("Rotate Step"), &ed->snap_rotate_step, 0.5f, 0.1f, 180.0f, "%.1f°", 0);
        ch |= igDragFloat(TR("Scale Step"), &ed->snap_scale_step, 0.01f, 0.01f, 10.0f, "%.2f", 0);
        igSeparatorText(TR("Show"));
        ch |= igCheckbox(TR("Grid"), &ed->show_grid);
        ch |= igCheckbox(TR("Actor Icons"), &ed->show_icons);
        ch |= igCheckbox(TR("Stats"), &ed->show_stats);
        break;
    case 2: {
        igSeparatorText(TR("Editor Performance"));
        ch |= igCheckbox(TR("Use Less CPU when in Background"), &ed->less_cpu_in_background);
        ch |= igSliderInt(TR("Frame Rate Limit"), &ed->frame_limit, 0, 1000, ed->frame_limit ? "%d fps" : TR("None"), 0);
        bool vs = ed->vk->vsync;
        if (igCheckbox(TR("VSync"), &vs)) editor_set_vsync(ed, vs, now), ch = true;
        const char* qt[RENDER_SCALABILITY_LEVELS];
        for (int i = 0; i < RENDER_SCALABILITY_LEVELS; i++) qt[i] = TR(render_scalability_name(i));
        int sc = ed->scalability;
        if (igCombo_Str_arr(TR("Engine Scalability"), &sc, qt, RENDER_SCALABILITY_LEVELS, -1)) editor_set_scalability(ed, sc, now), ch = true;
        igTextDisabled("%s", TR("The editor's; the packaged game's is in Project Settings > Rendering."));
        break;
    }
    case 3: {
        igSeparatorText(TR("Play in Editor"));
        int pos = ed->play_possessed ? 0 : 1;
        const char* modes[2] = { TR("Possessed (the player's view)"), TR("Ejected (the editor's camera)") };
        if (igCombo_Str_arr(TR("Start Play"), &pos, modes, 2, -1)) ed->play_possessed = pos == 0, ch = true;
        igTextDisabled("%s", TR("F8 switches while playing; Shift+F1 frees the mouse; Esc stops."));
        break;
    }
    case 4: {
        static const char* const keys[][2] = {
            { "Play in Editor", "Alt+P" }, { "Stop", "Esc" }, { "Possess / Eject", "F8" }, { "Free the Mouse", "Shift+F1" },
            { "Move / Rotate / Scale", "W / E / R" }, { "Focus Selected", "F" }, { "Fly", "Right Mouse + WASD" },
            { "Undo / Redo", "Ctrl+Z / Ctrl+Y" }, { "Duplicate", "Ctrl+D" }, { "Delete", "Delete" }, { "Save Level", "Ctrl+S" },
            { "Content Drawer", "Ctrl+Space" }, { "Game View", "G" }, { "Fullscreen", "F11" }, { "Debug Viewer", "F10" },
            { "AI Debug (playing)", "'" }, { "Copy / Cut / Paste", "Ctrl+C / Ctrl+X / Ctrl+V" }, { "Select All", "Ctrl+A" },
        };
        igSeparatorText(TR("Keyboard Shortcuts"));
        if (igBeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, (ImVec2){ 0, 0 }, 0)) {
            igTableSetupColumn(TR("Command"), ImGuiTableColumnFlags_WidthStretch, 0, 0);
            igTableSetupColumn(TR("Shortcut"), ImGuiTableColumnFlags_WidthStretch, 0, 0);
            igTableHeadersRow();
            for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
                igTableNextRow(0, 0);
                igTableNextColumn();
                igText("%s", TR(keys[i][0]));
                igTableNextColumn();
                igTextDisabled("%s", keys[i][1]);
            }
            igEndTable();
        }
        break;
    }
    }
    igPopItemWidth();
    igEndChild();
    igEnd();
    if (ch) editor_prefs_changed();
}

// --- Package Game (sugestão #37: everything about the packaged game in one place, Unreal's Packaging + Windows pages
// and the project's description together, with the icon). The same settings as Project Settings: kept in the .rproject,
// used by --package too ---

static void section(const char* title) { // a section's title: a line, the name in the accent's color
    igDummy((ImVec2){ 0, 4 });
    igPushStyleColor_U32(ImGuiCol_Text, TH(TK_INFO));
    igSeparatorText(title);
    igPopStyleColor(1);
}

static void package_window(Editor* ed, double now) {
    float fs = igGetFontSize();
    igSetNextWindowSize((ImVec2){ fs * 46.0f, fs * 44.0f }, ImGuiCond_FirstUseEver);
    if (!igBegin(TR("Package Game"), &ed->show_package, ImGuiWindowFlags_NoCollapse)) {
        igEnd();
        return;
    }
    ProjectSettings* p = project();
    bool ch = false;
    // (2026-11-10) opened: the project's own settings, the platform it was packaged for last (never chosen: Android when
    // the project is set up for it, its package name or touch controls, else Windows)
    if (p->package_platform < 0 || p->package_platform >= PKG_PLATFORM_COUNT)
        p->package_platform = p->android_package[0] || p->android_touch_controls ? PKG_ANDROID_APK : PKG_WINDOWS;
    static const char* const plat_name[PKG_PLATFORM_COUNT] = { "Windows (64-bit)", "Android (APK)", "Android (Google Play)" };
    bool android = p->package_platform != PKG_WINDOWS;
    // the header: the icon as it will look, the game's name and the platform
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 o = igGetCursorScreenPos();
    float is = fs * 4.0f;
    ImDrawList_AddRectFilled(dl, o, (ImVec2){ o.x + is, o.y + is }, TH(TK_N5), 6.0f, 0);
    const char* ext = strrchr(p->icon, '.');
    bool picture = p->icon[0] && ext && _stricmp(ext, ".ico");
    if (!picture || !asset_thumb(dl, o.x + 4, o.y + 4, o.x + is - 4, o.y + is - 4, p->icon, ASSET_TEXTURE_SOURCE))
        ui_icon(dl, p->icon[0] ? ICON_IMAGE : ICON_GAMEPAD, o.x + is * 0.25f, o.y + is * 0.25f, is * 0.5f, TH(TK_N13));
    igDummy((ImVec2){ is, is });
    igSameLine(0, fs);
    igBeginGroup();
    const char* lvl = strrchr(ed->level_path, '\\');
    const char* lvl2 = strrchr(ed->level_path, '/');
    if (lvl2 > lvl) lvl = lvl2;
    char shown[96];
    snprintf(shown, sizeof shown, "%s", p->game_name[0] ? p->game_name : lvl ? lvl + 1 : "Game");
    char* dot = p->game_name[0] ? NULL : strrchr(shown, '.');
    if (dot) *dot = 0;
    const ImGuiStyle* st = igGetStyle();
    igPushFont(NULL, (st->FontSizeBase > 0 ? st->FontSizeBase : 13.0f) * 1.5f);
    igTextUnformatted(shown, NULL);
    igPopFont();
    igTextDisabled("%s  \xc2\xb7  %s %s  \xc2\xb7  %s", TR(plat_name[p->package_platform]), TR("Version"), p->version[0] ? p->version : "1.0", p->shipping ? TR("Shipping") : TR("Development"));
    igEndGroup();
    // the platform, as Unreal's Platforms menu: one card each, the chosen one lit (saved in the project)
    igDummy((ImVec2){ 0, 4 });
    {
        static const UiIcon plat_icon[PKG_PLATFORM_COUNT] = { ICON_GAMEPAD, ICON_PLAYER, ICON_GLOBE };
        static const char* const plat_tip[PKG_PLATFORM_COUNT] = {
            "A folder with the game's .exe for Windows PCs (Steam, itch.io, a zip).",
            "An .apk to install on Android phones and tablets (your devices, testers, stores without Google Play).",
            "An .aab for the Google Play Console, signed with your upload key (Project Settings > Android)." };
        float cw = (igGetContentRegionAvail().x - fs) / 3.0f, chh = fs * 3.4f;
        for (int i = 0; i < PKG_PLATFORM_COUNT; i++) {
            if (i) igSameLine(0, fs * 0.5f);
            igPushID_Int(i);
            ImVec2 c0 = igGetCursorScreenPos();
            bool on = p->package_platform == i;
            if (igInvisibleButton("plat", (ImVec2){ cw, chh }, 0)) p->package_platform = i, ch = true;
            bool hov = igIsItemHovered(0);
            igSetItemTooltip("%s", TR(plat_tip[i]));
            ImVec2 c1 = { c0.x + cw, c0.y + chh };
            ImDrawList_AddRectFilled(dl, c0, c1, on ? TH(TK_BLUE_D) : hov ? TH(TK_N7) : TH(TK_N6), 6.0f, 0);
            ImDrawList_AddRect(dl, c0, c1, on ? TH(TK_BLUE) : TH(TK_N8), 6.0f, on ? 2.0f : 1.0f, 0); // (rounding, thickness, flags: swapped before, the 0.29 crash #117-121)
            ui_icon(dl, plat_icon[i], c0.x + fs * 0.7f, c0.y + (chh - fs * 1.6f) * 0.5f, fs * 1.6f, on ? TH(TK_N15) : TH(TK_N13));
            ImDrawList_AddText_Vec2(dl, (ImVec2){ c0.x + fs * 3.0f, c0.y + (chh - fs) * 0.5f }, on ? TH(TK_N15) : TH(TK_N14), TR(plat_name[i]), NULL);
            igPopID();
        }
        android = p->package_platform != PKG_WINDOWS;
    }
    igBeginChild_Str("pkg_body", (ImVec2){ 0, -fs * 3.2f }, 0, 0);
    igPushItemWidth(fs * 20.0f);

    section(TR("Game"));
    ch |= igInputTextWithHint(TR("Game Name"), TR("(the level's name)"), p->game_name, sizeof p->game_name, 0, NULL, NULL);
    ch |= igInputText(TR("Project Name"), p->name, sizeof p->name, 0, NULL, NULL);
    ch |= igInputText(TR("Company Name"), p->company, sizeof p->company, 0, NULL, NULL);
    ch |= igInputTextWithHint(TR("Project Version"), "1.0", p->version, sizeof p->version, 0, NULL, NULL);
    ch |= igInputText(TR("Description"), p->description, sizeof p->description, 0, NULL, NULL);
    ch |= igInputTextWithHint(TR("Copyright"), "(c) 2026", p->copyright, sizeof p->copyright, 0, NULL, NULL);

    section(TR("Icon"));
    igSetNextItemWidth(fs * 20.0f);
    igInputTextWithHint(TR("Game Icon"), TR("(the engine's)"), p->icon, sizeof p->icon, ImGuiInputTextFlags_ReadOnly, NULL, NULL);
    igSameLine(0, -1);
    if (igButton("...##icon", (ImVec2){ 0, 0 }))
        ch |= pick_into_content(ed, now, TR("Icon"), "ico;png;jpg;jpeg;bmp;tga", "Branding", p->icon, sizeof p->icon);
    igSameLine(0, -1);
    igPushID_Str("icon");
    if (igButton(TR("Engine's"), (ImVec2){ 0, 0 })) p->icon[0] = 0, ch = true;
    igPopID();
    igTextDisabled("%s", TR("A .ico as it is, or a picture made into 256, 64, 48, 32 and 16 px: the file, the taskbar and the window show it."));

    section(TR("Levels"));
    ch |= map_field(ed, TR("Start Level"), p->game_default_map, sizeof p->game_default_map);
    igTextDisabled("%s", TR("Empty: the open level. Every level of Content goes with the game (Open Level finds them by name)."));
    ch |= igCheckbox(TR("Include Every Asset of Content"), &p->include_all_assets);

    if (android) { // the project's Android settings (the same as Project Settings > Platforms > Android)
        section(TR("Android"));
        ch |= igInputTextWithHint(TR("Android Package Name"), "com.company.game", p->android_package, sizeof p->android_package, 0, NULL, NULL);
        const char* ors[3] = { TR("Landscape"), TR("Portrait"), TR("Either (sensor)") };
        ch |= igCombo_Str_arr(TR("Orientation"), &p->android_orientation, ors, 3, -1);
        ch |= igInputInt(TR("Version Code"), &p->android_version_code, 1, 1, 0);
        if (p->android_version_code < 1) p->android_version_code = 1;
        const char* qs[4] = { TR("Automatic (by the device)"), TR("Low"), TR("Medium"), TR("High") };
        ch |= igCombo_Str_arr(TR("Mobile Quality"), &p->android_quality, qs, 4, -1);
        ch |= igCheckbox(TR("Touch Controls"), &p->android_touch_controls);
        if (p->package_platform == PKG_ANDROID_PLAY) {
            igTextDisabled("%s: %s", TR("Keystore"), p->android_keystore[0] ? p->android_keystore : TR("(none: Project Settings > Android > Create Upload Key)"));
        }
        if (igButton(TR("More Android Settings..."), (ImVec2){ 0, 0 })) g_ps_page = 9, ed->show_project_settings = true;
    } else {
    section(TR("Window"));
    ch |= igInputText(TR("Window Title"), p->window_title, sizeof p->window_title, 0, NULL, NULL);
    int res[2] = { p->resolution_w, p->resolution_h };
    if (igInputInt2(TR("Resolution"), res, 0)) p->resolution_w = res[0] < 320 ? 320 : res[0], p->resolution_h = res[1] < 200 ? 200 : res[1], ch = true;
    ch |= igCheckbox(TR("Fullscreen"), &p->fullscreen);
    igSameLine(0, fs * 2.0f);
    ch |= igCheckbox(TR("VSync"), &p->vsync);
    igSameLine(0, fs * 2.0f);
    ch |= igCheckbox(TR("Capture the Mouse"), &p->mouse_capture);
    }

    section(TR("Graphics"));
    ch |= render_mode_fields(p);

    section(TR("Loading Screen"));
    ch |= splash_fields(ed, now);

    section(TR("Build"));
    int cfg = p->shipping ? 1 : 0;
    const char* cfgs[2] = { TR("Development"), TR("Shipping") };
    if (igCombo_Str_arr(TR("Build Configuration"), &cfg, cfgs, 2, -1)) p->shipping = cfg == 1, ch = true;
    igTextDisabled("%s", TR(p->shipping ? "Shipping: what players get (no Debug Viewer on F10)." : "Development: the Debug Viewer (F10) stays in the game."));
    igSetNextItemWidth(fs * 20.0f);
    ch |= igInputTextWithHint(TR("Output Folder"), TR("(choose one)"), p->package_dir, sizeof p->package_dir, 0, NULL, NULL);
    igSameLine(0, -1);
    if (igButton("...##pkgout", (ImVec2){ 0, 0 })) {
        char pick[512];
        snprintf(pick, sizeof pick, "%s", p->package_dir);
        if (platform_folder_dialog(ed->window, TR("Output Folder"), pick, sizeof pick)) snprintf(p->package_dir, sizeof p->package_dir, "%s", pick), ch = true;
    }
    igPopItemWidth();
    igEndChild();

    // the footer: the package button (Unreal's blue), the last result
    static int result; // 0 none, 1 done, -1 failed
    igSeparator();
    igBeginDisabled(ed->playing || !p->package_dir[0]);
    igPushStyleColor_U32(ImGuiCol_Button, TH(TK_BLUE));
    igPushStyleColor_U32(ImGuiCol_ButtonHovered, TH(TK_BLUE));
    char btn[96];
    snprintf(btn, sizeof btn, "%s##pkg", p->package_platform == PKG_WINDOWS ? TR("Package Game") : p->package_platform == PKG_ANDROID_APK ? TR("Package APK") : TR("Package for Google Play"));
    if (igButton(btn, (ImVec2){ fs * 14.0f, fs * 2.0f })) {
        if (ch && project_file()[0]) project_save(NULL), ch = false;
        bool ok = p->package_platform == PKG_WINDOWS ? editor_package_game_async(ed, p->package_dir, true, now) // (a card shows the copies)
                                                     : editor_package_android_as(ed, p->package_dir, true, true, p->package_platform == PKG_ANDROID_PLAY, now);
        result = ok ? 1 : -1;
    }
    igPopStyleColor(2);
    igEndDisabled();
    igSameLine(0, fs);
    igAlignTextToFramePadding();
    if (ed->playing) igTextDisabled("%s", TR("Stop playing before packaging."));
    else if (!p->package_dir[0]) igTextDisabled("%s", TR("Choose the output folder first."));
    else if (result > 0) igTextColored(THV(TK_OK), "%s", TR("Packaging started: its card is at the bottom right; the folder opens when it is done."));
    else if (result < 0) igTextColored(THV(TK_ERROR), "%s", TR("Packaging failed: the status bar says why."));
    igEnd();
    if (ch && project_file()[0] && !project_save(NULL)) editor_status(ed, now, TR("Could not save %s."), project_file());
}

bool project_ui_frame(Editor* ed, double now) {
    if (ed->show_browser) browser(ed, now);
    if (ed->show_package) package_window(ed, now);
    if (ed->show_project_settings) project_settings(ed, now);
    if (ed->show_prefs) preferences(ed, now);
    return ed->show_browser;
}

void project_ui_browser_refresh(void) {
    if (B.init) browser_refresh();
}

void project_ui_show_games(void) {
    if (!B.init) browser_init();
    B.page = 1;
}

void project_ui_open_failed(const char* rproject) {
    if (!B.init) browser_init();
    snprintf(B.error, sizeof B.error, TR("Could not open %s."), rproject);
    project_recent_remove(rproject);
    browser_refresh();
}
