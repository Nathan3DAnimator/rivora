// Dialogue (--dialogue-test; notas-dev/dialogo.md): first the rules on a made-up dialogue (dlg_selftest: the editor's
// preview), then in a new project two Item Definitions (a coin, a potion), a Dialogue made with the API — a greeting in
// three languages, a Choice whose "buy" Option needs a coin (it takes the coin, gives the potion and adds a tag), a
// "secret" Option hidden by a Gameplay Tag the player lacks, an "again" Option that jumps back to the Choice, "goodbye"
// to an End that calls the NPC's Custom Event with an input — and a Blueprint NPC with a Dialogue Speaker whose events
// write variables. While playing: Start Dialogue (the node, by the NPC's Custom Event Talk), the letters appearing, the
// widget's Continue clicked, the Options with the hidden one, the language switched, the jump, an Option taken by the
// keyboard through UMG's navigation, the item and the tag, Save / Load Game to Slot of what was said, the end and its
// event, the pause and the camera; then the cost of a conversation running.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "components.h"
#include "dialogue.h"
#include "ecs.h"
#include "editor.h"
#include "game_settings.h"
#include "gameplay_tags.h"
#include "inventory.h"
#include "online.h"
#include "platform.h"
#include "platform_files.h"
#include "ui_widget.h"

static Entity g_npc, g_hero;
static int g_state, g_wait = 10;
static bool g_ok = true, g_done;
static uint32_t g_coin, g_potion, g_dlg;
static double g_tick_ms, g_draw_ms;
static int g_tick_n;
static double g_start_us, g_choose_us, g_t0;

#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: dialogue: " __VA_ARGS__); printf("\n"); g_ok = false; } } while (0)

static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    printf("[RealEngine] FAIL: dialogue: node %u has no %s pin \"%s\"\n", id, out ? "output" : "input", name);
    g_ok = false;
    return 0;
}
static void link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    if (!bp_link(g, a, (uint8_t)pin(g, a, 1, ap), b, (uint8_t)pin(g, b, 0, bpn))) printf("[RealEngine] FAIL: dialogue: link %s -> %s refused\n", ap, bpn), g_ok = false;
}
static uint32_t node(BpAsset* a, uint16_t kind, float x, float y, const char* text) {
    BpNode* n = bp_add_node(a, &a->graphs[0], kind, x, y, text, NULL);
    if (!n) g_ok = false;
    return n ? n->id : 0;
}
static BpAsset* item(const char* name, const char* display, int stack) {
    BpAsset* a = bp_create(BPA_DATA, "/Content/DlgTest", name, "Item Definition");
    if (!a) return NULL;
    int k = bp_var_index(a, "Display Name");
    if (k >= 0) snprintf(a->vars[k].sdef, sizeof a->vars[k].sdef, "%s", display);
    k = bp_var_index(a, "Max Stack");
    if (k >= 0) a->vars[k].def.v[0] = (float)stack;
    bp_save(a);
    return a;
}
static bool var_text(World* w, Entity e, const char* name, char* out, size_t cap) {
    BpValue v[4];
    uint32_t wd = 0;
    bool is_text = false;
    out[0] = 0;
    return bp_get_instance_var(w, e, name, v, 4, &wd, out, cap, &is_text) && is_text;
}
static float var_num(World* w, Entity e, const char* name) {
    BpValue v[4];
    uint32_t wd = 0;
    bool is_text = false;
    char t[8];
    return bp_get_instance_var(w, e, name, v, 4, &wd, t, sizeof t, &is_text) ? v[0].v[0] : -1.0f;
}
static int option_index(const DlgView* v, const char* text) {
    for (uint32_t i = 0; i < v->noptions; i++)
        if (!strcmp(v->options[i], text)) return (int)i;
    return -1;
}

static void text3(DlgNode* n, const char* en, const char* pt, const char* es) {
    snprintf(n->text[0], DLG_TEXT, "%s", en), snprintf(n->text[1], DLG_TEXT, "%s", pt ? pt : ""), snprintf(n->text[2], DLG_TEXT, "%s", es ? es : "");
}

// The test's Dialogue: 0 Start -> 1 hello -> 2 Choice: 3 buy (needs a coin) -> 9 thanks -> 8 End; 4 secret (needs
// Quest.Secret); 5 again -> 6 Jump to 2; 7 goodbye -> 8 End (calls the NPC's OnDlgDone(42))
static Dialogue* make_dialogue(void) {
    Dialogue* d = dlg_create("/Content/DlgTest", "DLG_Test", false);
    if (!d) return NULL;
    d->chars_per_second = 30.0f;
    int hi = dlg_add_node(d, DLG_LINE, 300, 0), ch = dlg_add_node(d, DLG_CHOICE, 600, 0);
    text3(&d->nodes[hi], "Hello, traveler. Do you need anything?", "Olá, viajante. Precisa de algo?", "Hola, viajero. ¿Necesitas algo?");
    int buy = dlg_add_node(d, DLG_OPTION, 900, -200), secret = dlg_add_node(d, DLG_OPTION, 900, -60), again = dlg_add_node(d, DLG_OPTION, 900, 60);
    int jump = dlg_add_node(d, DLG_JUMP, 1200, 60), bye = dlg_add_node(d, DLG_OPTION, 900, 200), end = dlg_add_node(d, DLG_END, 1500, 200);
    int thanks = dlg_add_node(d, DLG_LINE, 1200, -200);
    text3(&d->nodes[buy], "I want a potion.", "Quero uma poção.", "Quiero una poción.");
    text3(&d->nodes[secret], "Tell me the secret.", "Conte o segredo.", "Cuéntame el secreto.");
    text3(&d->nodes[again], "Say that again?", "Pode repetir?", "¿Puedes repetirlo?");
    text3(&d->nodes[bye], "Goodbye", "", ""); // (Portuguese / Spanish: the engine's table, "Tchau")
    text3(&d->nodes[thanks], "Here it is.", "Aqui está.", "Aquí está.");
    DlgCond* c = &d->nodes[buy].conds[d->nodes[buy].nconds++];
    c->kind = DLC_HAS_ITEM, c->value = 1.0f, snprintf(c->key, DLG_NAME, "DA_DlgCoin");
    c = &d->nodes[secret].conds[d->nodes[secret].nconds++];
    c->kind = DLC_HAS_TAG, snprintf(c->key, DLG_NAME, "Quest.Secret");
    DlgAct* a = &d->nodes[buy].acts[d->nodes[buy].nacts++];
    a->kind = DLA_TAKE_ITEM, a->value = 1.0f, snprintf(a->key, DLG_NAME, "DA_DlgCoin");
    a = &d->nodes[buy].acts[d->nodes[buy].nacts++];
    a->kind = DLA_GIVE_ITEM, a->value = 1.0f, snprintf(a->key, DLG_NAME, "DA_DlgPotion");
    a = &d->nodes[buy].acts[d->nodes[buy].nacts++];
    a->kind = DLA_ADD_TAG, snprintf(a->key, DLG_NAME, "Quest.Merchant.Bought");
    a = &d->nodes[end].acts[d->nodes[end].nacts++];
    a->kind = DLA_EVENT, a->who = DLG_WHO_SPEAKER, snprintf(a->key, DLG_NAME, "OnDlgDone"), snprintf(a->arg[0], DLG_NAME, "42");
    d->nodes[jump].jump = (int16_t)ch;
    dlg_link(d, 0, hi), dlg_link(d, hi, ch);
    dlg_link(d, ch, buy), dlg_link(d, ch, secret), dlg_link(d, ch, again), dlg_link(d, ch, bye);
    dlg_link(d, buy, thanks), dlg_link(d, thanks, end), dlg_link(d, again, jump), dlg_link(d, bye, end);
    if (!dlg_save(d)) return NULL;
    return d;
}

static void step(Editor* ed, double dt) {
    World* w = &ed->world;
    (void)dt;
    if (g_done) return;
    if (g_state >= 20) { // the measure: a conversation on the screen, its letters appearing, its widget
        if (dlg_active(NULL)) g_tick_ms += dlg_stats()->tick_ms, g_draw_ms += ui_stats()->draw_ms, g_tick_n++;
        return;
    }
    if (g_wait-- > 0) return;
    DlgView v;
    char t[96];
    switch (g_state) {
    case 0: // Start Dialogue (the node, in the NPC's Custom Event Talk): the greeting in English
        dlg_set_language(0);
        CHECK(inv_add(w, g_hero, g_coin, 1) == 1, "a coin for the hero");
        bp_set_player(w, g_hero);
        CHECK(bp_call_event(w, g_npc, "Talk"), "the NPC's Talk event");
        CHECK(dlg_active(NULL), "Start Dialogue did not start");
        dlg_view_get(&v);
        CHECK(v.node == 1 && !strcmp(v.text, "Hello, traveler. Do you need anything?") && v.speaker && !strcmp(v.speaker, "Merchant"),
              "the first line: node %d, \"%s\" by \"%s\"", v.node, v.text ? v.text : "", v.speaker ? v.speaker : "");
        CHECK(game_paused(), "the Speaker's Pause did not pause the game");
        {
            float pos[3], yaw, pitch;
            CHECK(bp_player_camera(w, pos, &yaw, &pitch), "the conversation camera");
        }
        g_state = 1, g_wait = 2, g_t0 = re_time_seconds();        break;
    case 1: { // letter by letter (30 a second: some after 0.3 s), then the widget's Continue: the whole line, then the Choice
        if (re_time_seconds() - g_t0 < 0.3) return;
        dlg_view_get(&v);
        CHECK(v.shown_bytes > 0 && v.shown_bytes < strlen(v.text), "the letters: %u of %u shown", v.shown_bytes, (unsigned)strlen(v.text));
        const char* lt = ui_test_text("Line");
        CHECK(lt && strlen(lt) == v.shown_bytes, "WBP_Dialogue's Line shows %u bytes (%u)", lt ? (unsigned)strlen(lt) : 0u, v.shown_bytes);
        CHECK(ui_test_shown("Continue") && !ui_test_shown("Option1"), "WBP_Dialogue: Continue shown, the Options not");
        CHECK(var_num(w, g_npc, "Lines") == 1.0f, "Event On Dialogue Line: Lines %g (1)", var_num(w, g_npc, "Lines"));
        CHECK(ui_test_click(w, "Continue"), "the Continue button");
        dlg_view_get(&v);
        CHECK(v.shown_bytes == strlen(v.text) && v.node == 1, "Skip Line shows the whole line first");
        ui_test_click(w, "Continue");
        g_state = 2, g_wait = 2;
        break;
    }
    case 2: { // the Choice: the secret hidden (no Quest.Secret); the question stays; the language switched
        dlg_view_get(&v);
        CHECK(v.choosing && v.noptions == 3, "the Choice: %u options (3: the secret hidden)", v.noptions);
        CHECK(v.text && !strcmp(v.text, "Hello, traveler. Do you need anything?"), "the question stays over the options");
        CHECK(option_index(&v, "Tell me the secret.") < 0 && option_index(&v, "I want a potion.") == 0, "the hidden option");
        CHECK(ui_test_shown("Option3") && !ui_test_shown("Option4") && !ui_test_shown("Continue"), "WBP_Dialogue: 3 Option buttons");
        const char* o1 = ui_test_text("Option1Text");
        CHECK(o1 && !strcmp(o1, "I want a potion."), "Option1's text \"%s\"", o1 ? o1 : "");
        dlg_set_language(1);
        g_state = 3, g_wait = 2;
        break;
    }
    case 3: { // in Portuguese (its own text, and "Goodbye" through the engine's table); then Spanish
        dlg_view_get(&v);
        CHECK(!strcmp(v.text, "Olá, viajante. Precisa de algo?"), "Portuguese: \"%s\"", v.text);
        CHECK(option_index(&v, "Quero uma poção.") == 0, "the option in Portuguese");
        CHECK(v.noptions == 3 && !strcmp(v.options[2], "Tchau"), "\"Goodbye\" by the engine's table: \"%s\"", v.noptions == 3 ? v.options[2] : "");
        const char* o1 = ui_test_text("Option1Text");
        CHECK(o1 && !strcmp(o1, "Quero uma poção."), "the widget in Portuguese: \"%s\"", o1 ? o1 : "");
        dlg_set_language(2);
        dlg_view_get(&v);
        CHECK(!strcmp(v.text, "Hola, viajero. ¿Necesitas algo?"), "Spanish: \"%s\"", v.text);
        // the jump: "again" goes back to the Choice
        int k = option_index(&v, "¿Puedes repetirlo?");
        CHECK(k == 1 && dlg_choose(w, k), "Choose Option (again)");
        dlg_view_get(&v);
        CHECK(v.choosing && v.node == 2 && v.noptions == 3, "the Jump back to the Choice (node %d, %u options)", v.node, v.noptions);
        CHECK(var_text(w, g_npc, "LastChoice", t, sizeof t) && !strcmp(t, "¿Puedes repetirlo?"), "Event On Dialogue Choice: \"%s\"", t);
        dlg_set_language(0);
        g_state = 4, g_wait = 2;
        break;
    }
    case 4: // the keyboard (UMG's navigation): Enter on the first Option, "buy"
        CHECK(ui_test_shown("Option1"), "the options after the jump");
        ui_test_nav(4);
        g_state = 5, g_wait = 2;
        break;
    case 5: // buy: the coin for a potion, a tag; Save Game to Slot keeps what was said
        dlg_view_get(&v);
        CHECK(v.node == 9 && !strcmp(v.text, "Here it is."), "after buying: node %d \"%s\" (9)", v.node, v.text ? v.text : "");
        CHECK(inv_count(w, g_hero, g_coin) == 0 && inv_count(w, g_hero, g_potion) == 1, "the action: coins %d (0), potions %d (1)",
              inv_count(w, g_hero, g_coin), inv_count(w, g_hero, g_potion));
        CHECK(gt_has(w, g_hero, "Quest.Merchant.Bought", true), "the action's tag Quest.Merchant.Bought");
        CHECK(dlg_said(g_dlg, 3) && !dlg_said(g_dlg, 4), "said: the buy option, not the secret");
        CHECK(online_save(w, g_npc, "DlgTest"), "Save Game to Slot");
        dlg_memory_clear();
        CHECK(!dlg_said(g_dlg, 3), "the memory cleared");
        CHECK(online_load(w, g_npc, "DlgTest") && dlg_said(g_dlg, 3) && dlg_said(g_dlg, 1), "Load Game from Slot: what was said back");
        ui_test_click(w, "Continue"), ui_test_click(w, "Continue"); // (the whole line, then the End)
        g_state = 6, g_wait = 3;
        break;
    case 6: // the End: its action called OnDlgDone(42) on the NPC; Event On Dialogue End; the widget gone, the game resumed
        CHECK(!dlg_active(NULL), "the conversation did not end");
        CHECK(var_num(w, g_npc, "DoneArg") == 42.0f, "the End's Custom Event with its input: DoneArg %g (42)", var_num(w, g_npc, "DoneArg"));
        CHECK(var_text(w, g_npc, "Ended", t, sizeof t) && !strcmp(t, "DLG_Test"), "Event On Dialogue End: \"%s\"", t);
        CHECK(var_num(w, g_npc, "Lines") == 2.0f, "two lines told (%g)", var_num(w, g_npc, "Lines"));
        CHECK(!ui_test_shown("Continue") && !ui_test_shown("Line"), "WBP_Dialogue still shown");
        CHECK(!game_paused(), "the game still paused");
        // the measure: a conversation running (its cost a frame), started and answered timed
        {
            double t0 = re_time_seconds();
            dlg_start_speaker(w, g_npc, "", g_hero);
            g_start_us = (re_time_seconds() - t0) * 1e6;
            dlg_skip(w), dlg_skip(w);
            t0 = re_time_seconds();
            dlg_view_get(&v);
            int k = option_index(&v, "Say that again?");
            CHECK(k >= 0 && dlg_choose(w, k), "the second conversation's Choice");
            g_choose_us = (re_time_seconds() - t0) * 1e6;
            CHECK(game_paused(), "paused again");
            game_set_paused(false); // (the measure's frames: the world runs too)
        }
        dlg_editor_open("/Content/DlgTest/DLG_Test.rdlg"); // the editor drawn too (its canvas, Details, the preview running)
        dlg_editor_test_preview();
        g_state = 20;
        break;
    }
}

bool dialogue_test_setup(Editor* ed) {
    World* w = &ed->world;
    int fails = dlg_selftest();
    if (fails) {
        printf("[RealEngine] FAIL: dialogue: %d self-test failures\n", fails);
        g_ok = false;
    }
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/DlgTest", ui_content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    BpAsset* coin = item("DA_DlgCoin", "Coin", 99);
    BpAsset* pot = item("DA_DlgPotion", "Potion", 5);
    Dialogue* d = make_dialogue();
    BpAsset* c = bp_create(BPA_CLASS, "/Content/DlgTest", "BP_DlgNpc", "Actor");
    if (!coin || !pot || !d || !c) {
        printf("[RealEngine] FAIL: dialogue: could not create the assets in %s\n", dir);
        return false;
    }
    inv_items_refresh();
    const ItemDef* dc = inv_item_by_name("DA_DlgCoin");
    const ItemDef* dp = inv_item_by_name("DA_DlgPotion");
    if (!dc || !dp) {
        printf("[RealEngine] FAIL: dialogue: the Item Definitions\n");
        return false;
    }
    g_coin = dc->hash, g_potion = dp->hash, g_dlg = d->hash;
    // the NPC: its Dialogue Speaker (paused, the camera), Talk -> Start Dialogue; the events write variables
    c->comp_mask |= COMP_BIT(COMP_DIALOGUE_SPEAKER);
    CDialogueSpeaker* sp = (CDialogueSpeaker*)c->comp_data[COMP_DIALOGUE_SPEAKER];
    ecs_component_defaults(COMP_DIALOGUE_SPEAKER, sp);
    sp->dialogue = d->hash, sp->pause = DLG_PAUSE_YES, sp->flags |= DLG_SPK_CAMERA;
    snprintf(sp->display_name, sizeof sp->display_name, "Merchant");
    bp_add_var(c, "Lines", BPT_INT), bp_add_var(c, "LastChoice", BPT_STRING), bp_add_var(c, "Ended", BPT_NAME), bp_add_var(c, "DoneArg", BPT_INT);
    BpGraph* sig = bp_add_graph(c, "OnDlgDone", BPG_EVENT); // (a Custom Event with an input: Value)
    if (sig) memset(&sig->params[0], 0, sizeof sig->params[0]), snprintf(sig->params[0].name, BP_NAME, "Value"), sig->params[0].type = BPT_INT, sig->nparams = 1;
    BpGraph* g = &c->graphs[0];
    uint32_t talk = node(c, BPK_EVENT_CUSTOM, 0, 0, "Talk"), start = node(c, BPK_DLG_START, 300, 0, NULL);
    link(g, talk, "", start, "");
    uint32_t ln = node(c, BPK_EVENT_DLG_LINE, 0, 200, NULL), get_n = node(c, BPK_VAR_GET, 150, 250, "Lines");
    uint32_t inc = node(c, BPK_IADD, 250, 250, NULL), set_n = node(c, BPK_VAR_SET, 400, 200, "Lines");
    BpNode* incn = bp_node(g, inc);
    if (incn) incn->pins[1].lit.v[0] = 1.0f;
    link(g, ln, "", set_n, ""), link(g, get_n, "Lines", inc, incn ? incn->pins[0].name : "A"), link(g, inc, incn ? incn->pins[2].name : "", set_n, "Lines");
    uint32_t chv = node(c, BPK_EVENT_DLG_CHOICE, 0, 400, NULL), set_c = node(c, BPK_VAR_SET, 300, 400, "LastChoice");
    link(g, chv, "", set_c, ""), link(g, chv, "Text", set_c, "LastChoice");
    uint32_t endv = node(c, BPK_EVENT_DLG_END, 0, 600, NULL), set_e = node(c, BPK_VAR_SET, 300, 600, "Ended");
    link(g, endv, "", set_e, ""), link(g, endv, "Dialogue", set_e, "Ended");
    uint32_t done = node(c, BPK_EVENT_CUSTOM, 0, 800, "OnDlgDone"), set_d = node(c, BPK_VAR_SET, 300, 800, "DoneArg");
    link(g, done, "", set_d, ""), link(g, done, "Value", set_d, "DoneArg");
    if (!bp_save(c) || !bp_compile(c)) {
        printf("[RealEngine] FAIL: dialogue: BP_DlgNpc does not compile: %s\n", c->error);
        return false;
    }
    const float Y = 3000.0f;
    g_npc = bp_spawn(w, c, (float[3]){ 0, Y, 100 }, (float[3]){ 0, 0, 0 });
    g_hero = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_INVENTORY));
    if (!g_npc || !g_hero) {
        printf("[RealEngine] FAIL: dialogue: the actors\n");
        return false;
    }
    CTransform* ht = ecs_get(w, g_hero, COMP_TRANSFORM);
    ht->location[0] = 250.0f, ht->location[1] = Y, ht->location[2] = 100.0f, ht->flags = MOBILITY_MOVABLE;
    snprintf(((CName*)ecs_get(w, g_hero, COMP_NAME))->text, RE_NAME_MAX, "DlgHero");
    ed->cam.pos[0] = 300.0f, ed->cam.pos[1] = Y - 900.0f, ed->cam.pos[2] = 300.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -10.0f;
    ed->test_step = step;
    editor_play(ed, 0.0);
    return true;
}

bool dialogue_test_check(Editor* ed) {
    if (g_state < 20) printf("[RealEngine] FAIL: dialogue: stopped at step %d\n", g_state), g_ok = false;
    double tick = g_tick_n ? g_tick_ms / g_tick_n : 0.0;
    const DlgStats* st = dlg_stats();
    printf("[RealEngine] Dialogue cost: dlg_tick %.4f ms/frame (%d frames of a conversation on the screen); Start Dialogue %.1f us, "
           "Choose Option + Jump %.1f us; the widgets drawn in %.4f ms; %u lines, %u choices, %u conversations\n",
           tick, g_tick_n, g_start_us, g_choose_us, g_tick_n ? g_draw_ms / g_tick_n : 0.0, st->lines, st->choices, st->conversations);
    CHECK(g_tick_n > 10, "the measure's frames: %d", g_tick_n);
    CHECK(tick < 0.05, "dlg_tick costs %.4f ms a frame (0.05 at most)", tick);
    CHECK(g_start_us < 2000.0, "Start Dialogue costs %.1f us (2000 at most: it makes the widget)", g_start_us);
    CHECK(g_choose_us < 50.0, "Choose Option costs %.1f us (50 at most)", g_choose_us);
    editor_stop(ed, 0.0);
    g_done = true;
    if (g_ok) printf("[RealEngine] Dialogue test: OK\n");
    return g_ok;
}
