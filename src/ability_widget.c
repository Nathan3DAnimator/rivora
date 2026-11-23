// UMG's Ability Slot and Active Effects (ui_widget.h UIW_ABILITY_SLOT / UIW_EFFECT_LIST, ability.h; notas-dev/habilidades.md):
// a slot shows an actor's granted ability (its icon or a tile with its initials), the cooldown as a dark sweep with the
// seconds, the cast as a ring, the charges, the key; a click activates it. The list shows the active effects (buffs green,
// debuffs red) with their stacks and the time left. The input of a frame is read against the slots the frame before
// drew: nothing allocated, a few rectangles.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ability.h"
#include "blueprint.h"
#include "brand.h"
#include "components.h"
#include "i18n.h"
#include "ui_widget.h"

#define COL(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define MAX_SLOTS 32
typedef struct Slot { Entity owner; int slot; float x0, y0, x1, y1; } Slot;
static struct {
    Slot s[2][MAX_SLOTS];
    int n[2], cur;
    World* w;
} W;
static struct { bool on, clicked; float xy[2]; } T; // (the tests' click)

void abil_widget_test_click(bool on, float x, float y) { T.on = on, T.clicked = on, T.xy[0] = x, T.xy[1] = y; }
bool abil_widget_slot_center(Entity owner, int slot, float xy[2]) {
    for (int i = 0; i < W.n[W.cur]; i++) {
        const Slot* s = &W.s[W.cur][i];
        if (s->owner == owner && s->slot == slot) {
            xy[0] = (s->x0 + s->x1) * 0.5f, xy[1] = (s->y0 + s->y1) * 0.5f;
            return true;
        }
    }
    return false;
}

void abil_widget_frame(World* w, const float mouse[2], bool clicked) {
    float tm[2];
    if (T.on) tm[0] = T.xy[0], tm[1] = T.xy[1], mouse = tm, clicked = T.clicked, T.clicked = false;
    W.cur ^= 1;
    W.n[W.cur] = 0;
    W.w = w;
    if (!w || !mouse || !clicked) return;
    int p = W.cur ^ 1;
    for (int i = W.n[p] - 1; i >= 0; i--) {
        const Slot* s = &W.s[p][i];
        if (mouse[0] < s->x0 || mouse[0] > s->x1 || mouse[1] < s->y0 || mouse[1] > s->y1) continue;
        AbilSlotInfo si;
        if (abil_slot_info(w, s->owner, s->slot, &si)) abil_try_activate(w, s->owner, si.def->hash);
        return;
    }
}

static ImU32 colf(const float c[4], float a) {
    float x[4];
    for (int i = 0; i < 4; i++) x[i] = fminf(fmaxf(c[i], 0.0f), 1.0f);
    return COL((int)(x[0] * 255), (int)(x[1] * 255), (int)(x[2] * 255), (int)(x[3] * a * 255));
}
static Entity owner_of(World* w, const char* name) { // the widget's actor: by its name, else the player
    if (!w) return ENTITY_NONE;
    if (!name || !name[0]) return bp_player(w);
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_NAME) | COMP_BIT(COMP_ABILITY_SYSTEM);
        if ((a->mask & need) != need) continue;
        const CName* n = ecs_arch_column(a, COMP_NAME);
        for (uint32_t i = 0; i < a->count; i++)
            if (!_stricmp(n[i].text, name)) return ecs_arch_entities(a)[i];
    }
    return ENTITY_NONE;
}
static void text_at(ImDrawList* dl, float size, float x, float y, float ax, float ay, ImU32 col, const char* s, bool shadow) {
    ImFont* font = igGetFont();
    ImVec2 ts = ImFont_CalcTextSizeA(font, size, 1e9f, 0.0f, s, NULL, NULL);
    ImVec2 p = { x - ts.x * ax, y - ts.y * ay };
    if (shadow) ImDrawList_AddText_FontPtr(dl, font, size, (ImVec2){ p.x + 1.5f, p.y + 1.5f }, COL(0, 0, 0, (col >> 24) * 3 / 4), s, NULL, 0.0f, NULL);
    ImDrawList_AddText_FontPtr(dl, font, size, p, col, s, NULL, 0.0f, NULL);
}
static void initials(const char* s, char out[4]) {
    int n = 0;
    bool start = true;
    for (; *s && n < 2; s++) {
        bool alpha = (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9');
        if (alpha && start) out[n++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
        start = !alpha;
    }
    out[n] = 0;
}
// the ability's picture: its icon, else a tile of a color from its name with its initials
static void tile(ImDrawList* dl, const char* icon, const char* name, ImVec2 a, ImVec2 b, float rad, float al) {
    float aspect = 1.0f;
    void* tex = icon && icon[0] ? brand_ui_image(ui_content_root(), icon, &aspect) : NULL;
    if (tex) {
        ImTextureRef_c tr = { NULL, (ImTextureID)(uintptr_t)tex };
        ImDrawList_AddImageRounded(dl, tr, a, b, (ImVec2){ 0, 0 }, (ImVec2){ 1, 1 }, COL(255, 255, 255, (int)(255 * al)), rad, 0);
        return;
    }
    uint32_t h = 2166136261u;
    for (const char* p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    float hue = (float)(h % 360u) / 60.0f, c[4] = { 0, 0, 0, 0.95f };
    int k = (int)hue;
    float f = hue - (float)k, v = 0.62f, lo = 0.22f, up = lo + (v - lo) * f, dn = v - (v - lo) * f;
    const float rgb[6][3] = { { v, up, lo }, { dn, v, lo }, { lo, v, up }, { lo, dn, v }, { up, lo, v }, { v, lo, dn } };
    memcpy(c, rgb[k % 6], 12);
    ImDrawList_AddRectFilledMultiColor(dl, a, b, colf(c, al), colf(c, al), colf((float[4]){ c[0] * 0.55f, c[1] * 0.55f, c[2] * 0.55f, 1.0f }, al),
                                       colf((float[4]){ c[0] * 0.55f, c[1] * 0.55f, c[2] * 0.55f, 1.0f }, al));
    char ini[4];
    initials(name, ini);
    text_at(dl, (b.y - a.y) * 0.42f, (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, 0.5f, 0.5f, COL(255, 255, 255, (int)(235 * al)), ini, true);
}
// the cooldown's dark sweep: frac of a turn from 12 o'clock, clockwise, in wedges of at most a quarter (each convex)
static void sweep(ImDrawList* dl, ImVec2 c, float r, float frac, ImU32 col) {
    const float pi2 = 6.2831853f, start = -1.5707963f;
    float a0 = start + pi2 * (1.0f - frac), a1 = start + pi2;
    for (float a = a0; a < a1 - 1e-4f; a += pi2 * 0.25f) {
        float b = fminf(a + pi2 * 0.25f, a1);
        ImDrawList_PathLineTo(dl, c);
        ImDrawList_PathArcTo(dl, c, r, a, b, 12);
        ImDrawList_PathFillConvex(dl, col);
    }
}

static void draw_slot(ImDrawList* dl, const UiWidget* x, ImVec2 a, ImVec2 b, float k, float al, bool designer) {
    World* w = designer ? NULL : W.w;
    Entity owner = owner_of(w, x->text);
    int slot = (int)x->value;
    AbilSlotInfo si;
    bool has = !designer && abil_slot_info(w, owner, slot, &si);
    float rad = fmaxf(x->radius, 0.0f) * k, pad = 3.0f * k;
    ImDrawList_AddRectFilled(dl, a, b, colf(x->bg, al), rad, 0);
    const float accent[4] = { 1.0f, 0.62f, 0.15f, 1.0f };
    char key[8] = "";
    if (designer) snprintf(key, sizeof key, "%d", slot + 1);
    if (!has && !designer) { // (an empty slot: only its frame and key)
        ImDrawList_AddRect(dl, a, b, colf(x->border_col, al), rad, fmaxf(x->border_w * k, 1.0f), 0);
        return;
    }
    ImVec2 ia = { a.x + pad, a.y + pad }, ib = { b.x - pad, b.y - pad };
    if (designer) tile(dl, NULL, slot == 0 ? "Fire Ball" : slot == 1 ? "Dash" : "Heal", ia, ib, rad * 0.8f, al);
    else tile(dl, si.def->icon, si.def->display, ia, ib, rad * 0.8f, al);
    ImVec2 c = { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
    float size = b.y - a.y, r = sqrtf((b.x - a.x) * (b.x - a.x) + size * size) * 0.5f;
    float cd_left = designer ? (slot == 1 ? 2.4f : 0.0f) : si.cooldown_left, cd = designer ? 4.0f : si.cooldown;
    int charges = designer ? (slot == 1 ? 1 : 2) : si.charges, maxc = designer ? (slot == 1 ? 2 : 1) : si.max_charges;
    if (!designer && !si.affordable) ImDrawList_AddRectFilled(dl, ia, ib, COL(20, 40, 120, (int)(120 * al)), rad * 0.8f, 0); // (can't pay)
    if (cd_left > 0.0f && cd > 0.0f) {
        ImDrawList_PushClipRect(dl, ia, ib, true);
        sweep(dl, c, r, fminf(cd_left / cd, 1.0f), COL(0, 0, 0, (int)((charges ? 110 : 165) * al)));
        ImDrawList_PopClipRect(dl);
        if (!charges) { // (the seconds left, when it can't be used)
            char t[16];
            if (cd_left < 10.0f) snprintf(t, sizeof t, "%.1f", cd_left);
            else snprintf(t, sizeof t, "%.0f", ceilf(cd_left));
            text_at(dl, size * 0.32f, c.x, c.y, 0.5f, 0.5f, COL(255, 255, 255, (int)(255 * al)), t, true);
        }
    }
    if (!designer && si.casting && si.cast_time > 0.0f) { // the cast: a ring filling up
        float f = 1.0f - si.cast_left / si.cast_time, rr = size * 0.5f - pad * 2.0f;
        ImDrawList_PathArcTo(dl, c, rr, -1.5707963f, -1.5707963f + 6.2831853f * fminf(fmaxf(f, 0.0f), 1.0f), 32);
        ImDrawList_PathStroke(dl, colf(accent, al), 4.0f * k, 0);
    }
    bool hot = !designer && (si.active || si.casting);
    ImDrawList_AddRect(dl, a, b, hot ? colf(accent, al) : colf(x->border_col, al), rad, fmaxf((hot ? 3.0f : x->border_w) * k, 1.0f), 0);
    float fs = fmaxf(x->font_size * k, 8.0f);
    if (maxc > 1) { // the charges, bottom right
        char t[8];
        snprintf(t, sizeof t, "%d", charges);
        text_at(dl, fs * 1.15f, b.x - pad * 2.0f, b.y - pad, 1.0f, 1.0f, colf(x->color, al), t, true);
    }
    if (!designer && si.key) snprintf(key, sizeof key, "%c", si.key);
    if (key[0]) { // the key, top left in a little box
        ImVec2 ts = ImFont_CalcTextSizeA(igGetFont(), fs, 1e9f, 0.0f, key, NULL, NULL);
        ImVec2 k0 = { a.x + pad, a.y + pad }, k1 = { k0.x + ts.x + 8.0f * k, k0.y + ts.y + 2.0f * k };
        ImDrawList_AddRectFilled(dl, k0, k1, COL(0, 0, 0, (int)(170 * al)), 3.0f * k, 0);
        text_at(dl, fs, (k0.x + k1.x) * 0.5f, (k0.y + k1.y) * 0.5f, 0.5f, 0.5f, colf(x->color, al), key, false);
    }
    if (!designer && W.n[W.cur] < MAX_SLOTS) W.s[W.cur][W.n[W.cur]++] = (Slot){ owner, slot, a.x, a.y, b.x, b.y };
}

static void draw_effects(ImDrawList* dl, const UiWidget* x, ImVec2 a, ImVec2 b, float k, float al, bool designer) {
    AbilEffectInfo fx[24];
    uint32_t n = 0;
    EffectDef fake[3];
    if (designer) { // (a preview: a buff, a debuff with stacks, an infinite one)
        memset(fake, 0, sizeof fake);
        snprintf(fake[0].display, sizeof fake[0].display, "Haste"), snprintf(fake[1].display, sizeof fake[1].display, "Poison");
        snprintf(fake[2].display, sizeof fake[2].display, "Mana Regeneration"), fake[1].debuff = true;
        fx[0] = (AbilEffectInfo){ &fake[0], 3.2f, 5.0f, 1 }, fx[1] = (AbilEffectInfo){ &fake[1], 1.5f, 4.0f, 3 }, fx[2] = (AbilEffectInfo){ &fake[2], -1.0f, 0.0f, 1 };
        n = 3;
    } else {
        World* w = W.w;
        n = abil_effects_of(w, owner_of(w, x->text), fx, 24);
    }
    float fs = fmaxf(x->font_size * k, 8.0f), row = fs * 2.0f, gap = fmaxf(x->padding, 0.0f) * k;
    float y = a.y;
    for (uint32_t i = 0; i < n && y + row <= b.y + 0.5f; i++) {
        const EffectDef* d = fx[i].def;
        const float good[4] = { 0.25f, 0.78f, 0.4f, 1.0f }, bad[4] = { 0.92f, 0.28f, 0.22f, 1.0f };
        const float* tone = d->debuff ? bad : good;
        ImVec2 r0 = { a.x, y }, r1 = { b.x, y + row };
        ImDrawList_AddRectFilled(dl, r0, r1, colf(x->bg, al), fmaxf(x->radius, 0.0f) * k, 0);
        ImDrawList_AddRectFilled(dl, r0, (ImVec2){ r0.x + 4.0f * k, r1.y }, colf(tone, al), 2.0f * k, 0);
        float ic = row - 6.0f * k;
        tile(dl, d->icon, d->display, (ImVec2){ r0.x + 8.0f * k, y + 3.0f * k }, (ImVec2){ r0.x + 8.0f * k + ic, y + 3.0f * k + ic }, 3.0f * k, al);
        char name[96];
        if (fx[i].stacks > 1) snprintf(name, sizeof name, "%s  x%d", d->display, fx[i].stacks);
        else snprintf(name, sizeof name, "%s", d->display);
        text_at(dl, fs, r0.x + 14.0f * k + ic, y + row * 0.45f, 0.0f, 0.5f, colf(x->color, al), name, true);
        if (fx[i].remaining >= 0.0f) {
            char t[16];
            snprintf(t, sizeof t, fx[i].remaining < 10.0f ? "%.1f s" : "%.0f s", fx[i].remaining);
            text_at(dl, fs, r1.x - 8.0f * k, y + row * 0.45f, 1.0f, 0.5f, colf(x->color, al), t, true);
            float f = fx[i].duration > 0.0f ? fminf(fx[i].remaining / fx[i].duration, 1.0f) : 0.0f;
            float bx0 = r0.x + 14.0f * k + ic, bx1 = r1.x - 8.0f * k;
            ImDrawList_AddRectFilled(dl, (ImVec2){ bx0, r1.y - 5.0f * k }, (ImVec2){ bx1, r1.y - 3.0f * k }, COL(255, 255, 255, (int)(30 * al)), 0, 0);
            ImDrawList_AddRectFilled(dl, (ImVec2){ bx0, r1.y - 5.0f * k }, (ImVec2){ bx0 + (bx1 - bx0) * f, r1.y - 3.0f * k }, colf(tone, al), 0, 0);
        }
        y += row + gap;
    }
}

void abil_widget_draw(ImDrawList* dl, const UiWidget* x, float x0, float y0, float x1, float y1, float k, float al, bool designer) {
    if (x->kind == UIW_ABILITY_SLOT) draw_slot(dl, x, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, k, al, designer);
    else draw_effects(dl, x, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, k, al, designer);
}
