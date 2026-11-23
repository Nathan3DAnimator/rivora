// Quests (--quest-test; notas-dev/missoes.md): first the rules on made-up quests (quest_selftest: in order, parallel,
// optional, a condition, the time limit, the save record, 200 quests timed), then in a new project: two Item Definitions
// (an herb, a coin), a Dialogue for the elder, three Quests made with the API and a Blueprint that listens to the quest
// events. The hero's Quest Log starts the main quest when play begins: Go To a Trigger Box, a Door used (Interaction),
// three herbs (two picked before the objective is active: counted when it starts), two wolves killed by Apply Damage
// (their tag Enemy.Wolf.Grey under Enemy.Wolf), an optional objective left undone, Save Progress, the level opened again
// by Load Progress (the progress back), Save / Load Game to Slot, a talk with the elder's Dialogue, a Manual objective by
// the Blueprint's Advance Objective; the rewards (coins, the tag, the Custom Event, the herbs handed in) and the next
// quest with a marker; a timed quest failing; WBP_QuestTracker's texts in English and Portuguese. Then the cost of 200
// quests with 600 objectives running while the editor plays.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "components.h"
#include "dialogue.h"
#include "ecs.h"
#include "editor.h"
#include "gameplay_tags.h"
#include "health.h"
#include "interaction.h"
#include "inventory.h"
#include "level.h"
#include "online.h"
#include "platform.h"
#include "platform_files.h"
#include "quest.h"
#include "save_progress.h"
#include "ui_widget.h"

#define Y 6000.0f
static int g_state, g_wait = 10;
static bool g_ok = true, g_done;
static uint32_t g_herb, g_coin;
static double g_tick_ms, g_t0;
static int g_tick_n;
static uint32_t g_loads;

#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: quest: " __VA_ARGS__); printf("\n"); g_ok = false; } } while (0)

static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    printf("[RealEngine] FAIL: quest: node %u has no %s pin \"%s\"\n", id, out ? "output" : "input", name);
    g_ok = false;
    return 0;
}
static void link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    if (!bp_link(g, a, (uint8_t)pin(g, a, 1, ap), b, (uint8_t)pin(g, b, 0, bpn))) printf("[RealEngine] FAIL: quest: link %s -> %s refused\n", ap, bpn), g_ok = false;
}
static uint32_t node(BpAsset* a, uint16_t kind, float x, float y, const char* text) {
    BpNode* n = bp_add_node(a, &a->graphs[0], kind, x, y, text, NULL);
    if (!n) g_ok = false;
    return n ? n->id : 0;
}
static void lit_str(BpGraph* g, uint32_t id, const char* pin_name, const char* s) {
    BpNode* n = bp_node(g, id);
    int k = pin(g, id, 0, pin_name);
    if (n) snprintf(n->pins[k].slit, sizeof n->pins[k].slit, "%s", s);
}
static void lit_num(BpGraph* g, uint32_t id, const char* pin_name, float v) {
    BpNode* n = bp_node(g, id);
    int k = pin(g, id, 0, pin_name);
    if (n) n->pins[k].lit.v[0] = v;
}
static BpAsset* item(const char* name, const char* display, int stack) {
    BpAsset* a = bp_create(BPA_DATA, "/Content/QstTest", name, "Item Definition");
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
static Entity by_name(World* w, const char* name) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_NAME)) || (a->mask & COMP_BIT(COMP_ATTACH))) continue;
        const CName* nm = ecs_arch_column(a, COMP_NAME);
        for (uint32_t i = 0; i < a->count; i++)
            if (!strcmp(nm[i].text, name)) return ecs_arch_entities(a)[i];
    }
    return ENTITY_NONE;
}
static Entity actor(World* w, uint64_t extra, const char* name, float x, float y, float z) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | extra);
    if (!e) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = z, t->flags = MOBILITY_MOVABLE;
    return e;
}
static void place(World* w, Entity e, float x, float y, float z) {
    CTransform* t = ecs_alive(w, e) ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
    if (t) t->location[0] = x, t->location[1] = y, t->location[2] = z, ecs_touch(w, e);
}
static int progress_of(World* w, const char* quest, const char* obj) {
    int p = -1;
    quest_objective(w, ENTITY_NONE, quest, obj, &p, NULL, NULL, NULL);
    return p;
}
static bool done_of(World* w, const char* quest, const char* obj) {
    bool d = false;
    quest_objective(w, ENTITY_NONE, quest, obj, NULL, NULL, &d, NULL);
    return d;
}
static int stage_of(World* w, const char* quest) {
    int s = -1;
    quest_state(w, ENTITY_NONE, quest, &s);
    return s;
}
static bool text_has(const char* widget, const char* part) {
    const char* t = ui_test_text(widget);
    return t && strstr(t, part);
}
static const char* text_of(const char* widget) {
    const char* t = ui_test_text(widget);
    return t ? t : "(none)";
}

static QuestObjective* obj(Quest* q, int stage, uint8_t kind, const char* id, const char* target, int count, const char* en, const char* pt) {
    while (q->nstages <= stage) quest_add_stage(q);
    int o = quest_add_objective(q, stage, kind);
    if (o < 0) return g_ok = false, &q->obj[0];
    QuestObjective* x = &q->obj[o];
    snprintf(x->id, QST_NAME, "%s", id), snprintf(x->target, QST_NAME, "%s", target ? target : ""), x->count = count;
    snprintf(x->text[0], QST_TITLE, "%s", en ? en : ""), snprintf(x->text[1], QST_TITLE, "%s", pt ? pt : "");
    return x;
}
static Quest* quest(const char* name, const char* en, const char* pt) {
    Quest* q = quest_create("/Content/QstTest", name, false);
    if (!q) return NULL;
    q->nstages = q->nobj = 0;
    snprintf(q->title[0], QST_TITLE, "%s", en), snprintf(q->title[1], QST_TITLE, "%s", pt), q->title[2][0] = 0;
    return q;
}

static void step(Editor* ed, double dt) {
    World* w = &ed->world;
    (void)dt;
    if (g_done) return;
    if (g_state >= 20) { // the measure: 200 quests (600 objectives) and the main line's log running
        g_tick_ms += quest_stats()->tick_ms, g_tick_n++;
        return;
    }
    if (g_wait-- > 0) return;
    Entity hero = by_name(w, "QstHero"), watcher = by_name(w, "QstWatcher");
    char t[96];
    switch (g_state) {
    case 0: // the hero is the player (its log's subject: the markers' distances, the Collect's Inventory)
        bp_set_player(w, hero);
        dlg_set_language(0); // (the texts in English, whatever this PC's game language)
        g_state = 1, g_wait = 3;
        break;
    case 1: { // the Quest Log started the main quest; its tracker, the marker on the Trigger Box; herbs picked too early
        bp_set_player(w, hero);
        CHECK(hero && watcher, "the actors");
        CHECK(quest_state(w, ENTITY_NONE, "Q_TestMain", NULL) == QST_ACTIVE, "the Quest Log's starting quest is not active");
        CHECK(var_num(w, watcher, "Started") == 1.0f, "Event On Quest Started: %g (1)", var_num(w, watcher, "Started"));
        CHECK(var_text(w, watcher, "LastStarted", t, sizeof t) && !strcmp(t, "Q_TestMain"), "On Quest Started's Quest: \"%s\"", t);
        const Quest* tq = quest_tracked(w, ENTITY_NONE);
        CHECK(tq && !strcmp(tq->name, "Q_TestMain"), "tracked: %s", tq ? tq->name : "none");
        CHECK(ui_test_shown("Panel") && !strcmp(text_of("Title"), "Main Quest Test"), "WBP_QuestTracker's title: \"%s\"", text_of("Title"));
        CHECK(text_has("Stage", "Reach the house"), "the stage's text: \"%s\"", text_of("Stage"));
        CHECK(text_has("Objective1", "Enter the yard") && !ui_test_shown("Objective2"), "in order: only the first objective shown (\"%s\")", text_of("Objective1"));
        uint32_t nm = 0;
        const QuestMarker* mk = quest_markers(&nm);
        CHECK(nm == 1 && fabsf(mk[0].dist - 2000.0f) < 5.0f, "the marker on the Trigger Box: %u markers, %.0f cm (2000)", nm, nm ? mk[0].dist : 0.0f);
        CHECK(inv_add(w, hero, g_herb, 2) == 2, "two herbs picked");
        place(w, hero, 2000.0f, Y + 40.0f, 100.0f); // into the Trigger Box's Collision
        g_state = 2, g_wait = 3;
        break;
    }
    case 2: // Go To done; the herbs not counted yet (in order); the Door used
        CHECK(done_of(w, "Q_TestMain", "yard"), "Go To the Trigger Box");
        CHECK(var_text(w, watcher, "LastObj", t, sizeof t) && !strcmp(t, "yard"), "Event On Objective Completed: \"%s\"", t);
        CHECK(progress_of(w, "Q_TestMain", "herbs") == 0, "the herbs counted before their turn: %d", progress_of(w, "Q_TestMain", "herbs"));
        CHECK(text_has("Objective2", "Open the door"), "the second objective now: \"%s\"", text_of("Objective2"));
        CHECK(interaction_interact(w, hero, by_name(w, "QstDoor")), "the Door used");
        g_state = 3, g_wait = 3;
        break;
    case 3: // Interact done: the herbs held count now (2 of 3); one more
        CHECK(done_of(w, "Q_TestMain", "door"), "the Door's Interact objective");
        CHECK(progress_of(w, "Q_TestMain", "herbs") == 2, "the herbs held when it started: %d (2)", progress_of(w, "Q_TestMain", "herbs"));
        CHECK(text_has("Objective3", "2/3"), "the tracker's count: \"%s\"", text_of("Objective3"));
        CHECK(var_num(w, watcher, "LastProgress") == 2.0f, "Event On Objective Updated's Progress: %g (2)", var_num(w, watcher, "LastProgress"));
        inv_add(w, hero, g_herb, 1);
        g_state = 4, g_wait = 3;
        break;
    case 4: // stage 2: the wolves; a bear doesn't count, a grey wolf does (Enemy.Wolf); Save Progress and Save Game to Slot
        CHECK(stage_of(w, "Q_TestMain") == 1, "the second stage: %d", stage_of(w, "Q_TestMain"));
        CHECK(text_has("Objective1", "Kill the wolves") && text_has("Objective1", "0/2"), "the kill objective: \"%s\"", text_of("Objective1"));
        CHECK(text_has("Objective2", "(Optional)"), "the optional objective: \"%s\"", text_of("Objective2"));
        health_apply_damage(w, by_name(w, "QstBear"), 1e4f, "Damage.Physical", hero, hero);
        health_apply_damage(w, by_name(w, "QstWolf1"), 1e4f, "Damage.Physical", hero, hero);
        CHECK(health_is_dead(w, by_name(w, "QstWolf1")), "the wolf did not die");
        CHECK(progress_of(w, "Q_TestMain", "wolves") == 1, "one wolf (the bear not): %d", progress_of(w, "Q_TestMain", "wolves"));
        CHECK(bp_call_event(w, watcher, "Probe"), "the watcher's Probe");
        CHECK(var_num(w, watcher, "ProbeProg") == 1.0f && var_num(w, watcher, "ProbeActive") == 1.0f, "Get Objective Progress %g (1), Is Quest Active %g",
              var_num(w, watcher, "ProbeProg"), var_num(w, watcher, "ProbeActive"));
        CHECK(online_save(w, hero, "QstSlot"), "Save Game to Slot");
        CHECK(progress_save(w, "QstTest", "Quest test", 0), "Save Progress");
        g_state = 5, g_wait = 3;
        break;
    case 5: // the second wolf: the third stage; then Load Progress (the level opened again: the save's progress back)
        health_apply_damage(w, by_name(w, "QstWolf2"), 1e4f, "Damage.Physical", hero, hero);
        CHECK(stage_of(w, "Q_TestMain") == 2, "the third stage after the wolves (the optional one undone): %d", stage_of(w, "Q_TestMain"));
        g_loads = progress_stats()->loads;
        CHECK(progress_load(w, "QstTest"), "Load Progress");
        g_t0 = re_time_seconds();
        g_state = 6, g_wait = 2;
        break;
    case 6: // the level is back, and the quest where it was saved: stage 2, one wolf
        if (progress_stats()->loads == g_loads) {
            if (re_time_seconds() - g_t0 > 8.0) {
                CHECK(false, "the load did not finish");
                g_state = 20;
            }
            break;
        }
        bp_set_player(w, hero);
        dlg_set_language(0); // (the Open Level brought back the game's language)
        CHECK(stage_of(w, "Q_TestMain") == 1 && progress_of(w, "Q_TestMain", "wolves") == 1, "loaded: stage %d (1), wolves %d (1)",
              stage_of(w, "Q_TestMain"), progress_of(w, "Q_TestMain", "wolves"));
        CHECK(done_of(w, "Q_TestMain", "herbs"), "loaded: the herbs objective");
        CHECK(var_num(w, watcher, "Started") == 0.0f, "the load started the quest again (Started %g)", var_num(w, watcher, "Started"));
        // Save Game to Slot: cleared, read back
        quest_clear();
        CHECK(quest_state(w, ENTITY_NONE, "Q_TestMain", NULL) == QST_NOT_STARTED, "cleared");
        CHECK(online_load(w, hero, "QstSlot") && stage_of(w, "Q_TestMain") == 1 && progress_of(w, "Q_TestMain", "wolves") == 1,
              "Load Game from Slot: stage %d, wolves %d", stage_of(w, "Q_TestMain"), progress_of(w, "Q_TestMain", "wolves"));
        CHECK(inv_count(w, hero, g_herb) == 3, "Load Game from Slot: the hero's herbs %d (3)", inv_count(w, hero, g_herb)); // (Load Progress gives the player's record to the possessed pawn: none after the reopen; Save Game to Slot keeps the hero's Inventory)
        g_state = 7, g_wait = 3;
        break;
    case 7: // the wolf again (the level brought it back), the talk with the elder, the ritual by the Blueprint's Advance Objective
        CHECK(text_has("Objective1", "1/2"), "the tracker after the load: \"%s\"", text_of("Objective1"));
        health_apply_damage(w, by_name(w, "QstWolf2"), 1e4f, "Damage.Physical", hero, hero);
        CHECK(stage_of(w, "Q_TestMain") == 2, "the third stage: %d", stage_of(w, "Q_TestMain"));
        CHECK(dlg_start_speaker(w, by_name(w, "QstElder"), "", hero), "the elder's Dialogue");
        dlg_end(w);
        CHECK(done_of(w, "Q_TestMain", "elder"), "the Talk objective");
        CHECK(bp_call_event(w, watcher, "Ritual"), "the watcher's Ritual");
        CHECK(progress_of(w, "Q_TestMain", "ritual") == 1 && quest_state(w, ENTITY_NONE, "Q_TestMain", NULL) == QST_ACTIVE, "Advance Objective: %d (1)",
              progress_of(w, "Q_TestMain", "ritual"));
        bp_call_event(w, watcher, "Ritual");
        g_state = 8, g_wait = 3;
        break;
    case 8: { // completed: the rewards, the herbs handed in, the next quest tracked with its marker; a timed quest
        CHECK(quest_state(w, ENTITY_NONE, "Q_TestMain", NULL) == QST_COMPLETED, "the main quest is not completed");
        CHECK(var_text(w, watcher, "Completed", t, sizeof t) && !strcmp(t, "Q_TestMain"), "Event On Quest Completed: \"%s\"", t);
        CHECK(var_num(w, watcher, "Rewarded") == 1.0f, "the reward's Custom Event on the Event Target");
        CHECK(inv_count(w, hero, g_coin) == 5 && inv_count(w, hero, g_herb) == 0, "the rewards: coins %d (5), herbs %d (0: handed in)",
              inv_count(w, hero, g_coin), inv_count(w, hero, g_herb));
        CHECK(gt_has(w, hero, "Quest.Main.Done", true), "the reward's tag");
        CHECK(quest_state(w, ENTITY_NONE, "Q_TestNext", NULL) == QST_ACTIVE && var_num(w, watcher, "Started") == 1.0f, "the next quest (Started %g)",
              var_num(w, watcher, "Started"));
        const Quest* tq = quest_tracked(w, ENTITY_NONE);
        CHECK(tq && !strcmp(tq->name, "Q_TestNext") && !strcmp(text_of("Title"), "The Next Step"), "the tracker on the next quest: \"%s\"", text_of("Title"));
        uint32_t nm = 0;
        const QuestMarker* mk = quest_markers(&nm);
        CHECK(nm == 1 && mk[0].dist > 1000.0f, "the next quest's marker on the elder: %u, %.0f cm", nm, nm ? mk[0].dist : 0.0f);
        CHECK(!quest_start(w, ENTITY_NONE, "Q_TestMain"), "a completed quest started again");
        CHECK(quest_start(w, ENTITY_NONE, "Q_TestTimed"), "the timed quest");
        g_t0 = re_time_seconds();
        g_state = 9, g_wait = 1;
        break;
    }
    case 9: // the time limit (0.5 s) fails it; the tracker in Portuguese
        if (re_time_seconds() - g_t0 < 0.7) break;
        CHECK(quest_state(w, ENTITY_NONE, "Q_TestTimed", NULL) == QST_FAILED, "the time limit did not fail it");
        CHECK(var_text(w, watcher, "Failed", t, sizeof t) && !strcmp(t, "Q_TestTimed"), "Event On Quest Failed: \"%s\"", t);
        dlg_set_language(1);
        g_state = 10, g_wait = 40; // (the tracker reads the language within half a second)
        break;
    case 10:
        CHECK(!strcmp(text_of("Title"), "O Próximo Passo"), "the tracker in Portuguese: \"%s\"", text_of("Title"));
        CHECK(text_has("Objective1", "Vá até a anciã"), "the objective in Portuguese: \"%s\"", text_of("Objective1"));
        dlg_set_language(0);
        { // the measure: 200 more quests (a kill, a Go To and a manual each) in the hero's log
            double t0 = re_time_seconds();
            int started = 0;
            for (int i = 0; i < 200; i++) {
                char n[32];
                snprintf(n, sizeof n, "Q_Many%03d", i);
                started += quest_start(w, ENTITY_NONE, n);
            }
            double ms = (re_time_seconds() - t0) * 1000.0;
            t0 = re_time_seconds();
            health_apply_damage(w, by_name(w, "QstBear2"), 1e4f, "Damage.Physical", hero, hero);
            double death_ms = (re_time_seconds() - t0) * 1000.0;
            CHECK(started == 200 && progress_of(w, "Q_Many100", "k") == 1, "200 quests started: %d, the bear counted %d", started, progress_of(w, "Q_Many100", "k"));
            printf("[RealEngine] Quest: 200 quests started in %.3f ms; a death through 200 Kill objectives (and the Blueprints' events) %.3f ms\n", ms, death_ms);
            CHECK(death_ms < 2.0, "a death with 200 kill objectives costs %.3f ms (2 at most)", death_ms);
        }
        g_state = 20;
        break;
    }
}

bool quest_test_setup(Editor* ed) {
    World* w = &ed->world;
    int fails = quest_selftest();
    if (fails) printf("[RealEngine] FAIL: quest: %d self-test failures\n", fails), g_ok = false;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/QstTest", ui_content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    snprintf(dir, sizeof dir, "%s/QstTest/Many", ui_content_root());
    re_make_dirs(dir);
    progress_slot_delete("QstTest"), progress_slot_delete("QstSlot");
    BpAsset* herb = item("DA_QstHerb", "Herb", 10);
    BpAsset* coin = item("DA_QstCoin", "Coin", 99);
    inv_items_refresh();
    const ItemDef* dh = inv_item_by_name("DA_QstHerb");
    const ItemDef* dc = inv_item_by_name("DA_QstCoin");
    if (!herb || !coin || !dh || !dc) return printf("[RealEngine] FAIL: quest: the Item Definitions\n"), false;
    g_herb = dh->hash, g_coin = dc->hash;
    // the elder's Dialogue: one line
    Dialogue* d = dlg_create("/Content/QstTest", "DLG_QstElder", false);
    if (!d) return printf("[RealEngine] FAIL: quest: the Dialogue\n"), false;
    int ln = dlg_add_node(d, DLG_LINE, 300, 0);
    snprintf(d->nodes[ln].text[0], DLG_TEXT, "Thank you for coming back.");
    dlg_link(d, 0, ln);
    dlg_save(d);
    // the main quest: 1 (in order) Go To the yard, the door, 3 herbs (handed in); 2 two wolves, an optional bonus; 3 the
    // elder, the ritual (Manual, 2); rewards
    Quest* m = quest("Q_TestMain", "Main Quest Test", "Missão de Teste");
    Quest* nx = quest("Q_TestNext", "The Next Step", "O Próximo Passo");
    Quest* tm = quest("Q_TestTimed", "Hurry", "Depressa");
    if (!m || !nx || !tm) return printf("[RealEngine] FAIL: quest: the Quests could not be made\n"), false;
    obj(m, 0, QOBJ_GO_TO, "yard", "QstZone", 1, "Enter the yard", "Entre no quintal");
    obj(m, 0, QOBJ_INTERACT, "door", "QstDoor", 1, "Open the door", "Abra a porta");
    obj(m, 0, QOBJ_COLLECT, "herbs", "DA_QstHerb", 3, "Gather herbs", "Junte ervas")->flags |= QO_TAKE_ITEMS;
    obj(m, 1, QOBJ_KILL, "wolves", "Enemy.Wolf", 2, "Kill the wolves", "Mate os lobos");
    obj(m, 1, QOBJ_MANUAL, "bonus", NULL, 1, "Find the hidden cave", "Ache a caverna escondida")->flags |= QO_OPTIONAL;
    obj(m, 2, QOBJ_TALK, "elder", "QstElder", 1, "Talk to the elder", "Fale com a anciã");
    obj(m, 2, QOBJ_MANUAL, "ritual", NULL, 2, "Perform the ritual", "Faça o ritual");
    m->stages[0].in_order = 1;
    snprintf(m->stages[0].text[0], QST_TITLE, "Reach the house");
    snprintf(m->reward_item[0], QST_NAME, "DA_QstCoin"), m->reward_count[0] = 5;
    snprintf(m->reward_tag[0], QST_NAME, "Quest.Main.Done");
    snprintf(m->reward_event, QST_NAME, "OnQuestReward"), snprintf(m->event_target, QST_NAME, "QstWatcher");
    snprintf(m->next_quest, QST_NAME, "Q_TestNext");
    obj(nx, 0, QOBJ_GO_TO, "to_elder", "QstElder", 1, "Go to the elder", "Vá até a anciã")->radius = 100.0f;
    obj(tm, 0, QOBJ_MANUAL, "never", NULL, 1, "Wait", "Espere");
    tm->time_limit = 0.5f;
    if (!quest_save(m) || !quest_save(nx) || !quest_save(tm)) return printf("[RealEngine] FAIL: quest: the Quests could not be saved\n"), false;
    for (int i = 0; i < 200; i++) { // the measure's
        char n[32];
        snprintf(n, sizeof n, "Q_Many%03d", i);
        Quest* q = quest_create("/Content/QstTest/Many", n, false);
        if (!q) return printf("[RealEngine] FAIL: quest: %s\n", n), false;
        q->nstages = q->nobj = 0;
        obj(q, 0, QOBJ_KILL, "k", "Enemy.Bear", 2, "Hunt bears", NULL);
        obj(q, 0, QOBJ_GO_TO, "g", "QstElder", 1, NULL, NULL)->radius = 50.0f;
        obj(q, 0, QOBJ_MANUAL, "m", NULL, 1, "Something else", NULL);
        quest_save(q);
    }
    // the watcher: the quest events into variables; Ritual -> Advance Objective; Probe -> the getters; OnQuestReward
    BpAsset* c = bp_create(BPA_CLASS, "/Content/QstTest", "BP_QstWatcher", "Actor");
    if (!c) return printf("[RealEngine] FAIL: quest: BP_QstWatcher\n"), false;
    bp_add_var(c, "Started", BPT_INT), bp_add_var(c, "LastStarted", BPT_NAME), bp_add_var(c, "LastObj", BPT_NAME), bp_add_var(c, "LastProgress", BPT_INT);
    bp_add_var(c, "Completed", BPT_NAME), bp_add_var(c, "Failed", BPT_NAME), bp_add_var(c, "Rewarded", BPT_BOOL);
    bp_add_var(c, "ProbeProg", BPT_INT), bp_add_var(c, "ProbeActive", BPT_BOOL);
    BpGraph* g = &c->graphs[0];
    uint32_t sv = node(c, BPK_EVENT_QST_STARTED, 0, 0, NULL), gs = node(c, BPK_VAR_GET, 150, 60, "Started");
    uint32_t inc = node(c, BPK_IADD, 250, 60, NULL), ss = node(c, BPK_VAR_SET, 400, 0, "Started"), sl = node(c, BPK_VAR_SET, 700, 0, "LastStarted");
    BpNode* incn = bp_node(g, inc);
    if (incn) incn->pins[1].lit.v[0] = 1.0f;
    link(g, sv, "", ss, ""), link(g, gs, "Started", inc, incn ? incn->pins[0].name : "A"), link(g, inc, incn ? incn->pins[2].name : "", ss, "Started");
    link(g, ss, "", sl, ""), link(g, sv, "Quest", sl, "LastStarted");
    uint32_t oc = node(c, BPK_EVENT_QST_OBJ_COMPLETED, 0, 200, NULL), so = node(c, BPK_VAR_SET, 300, 200, "LastObj");
    link(g, oc, "", so, ""), link(g, oc, "Objective", so, "LastObj");
    uint32_t ou = node(c, BPK_EVENT_QST_OBJ_UPDATED, 0, 300, NULL), sp = node(c, BPK_VAR_SET, 300, 300, "LastProgress");
    link(g, ou, "", sp, ""), link(g, ou, "Progress", sp, "LastProgress");
    uint32_t qc = node(c, BPK_EVENT_QST_COMPLETED, 0, 400, NULL), sc = node(c, BPK_VAR_SET, 300, 400, "Completed");
    link(g, qc, "", sc, ""), link(g, qc, "Quest", sc, "Completed");
    uint32_t qf = node(c, BPK_EVENT_QST_FAILED, 0, 500, NULL), sf = node(c, BPK_VAR_SET, 300, 500, "Failed");
    link(g, qf, "", sf, ""), link(g, qf, "Quest", sf, "Failed");
    uint32_t ri = node(c, BPK_EVENT_CUSTOM, 0, 600, "Ritual"), adv = node(c, BPK_QST_ADVANCE, 300, 600, NULL);
    link(g, ri, "", adv, "");
    lit_str(g, adv, "Quest", "Q_TestMain"), lit_str(g, adv, "Objective", "ritual"), lit_num(g, adv, "Amount", 1.0f);
    uint32_t pr = node(c, BPK_EVENT_CUSTOM, 0, 700, "Probe"), gp = node(c, BPK_QST_GET_PROGRESS, 150, 760, NULL), ia = node(c, BPK_QST_IS_ACTIVE, 150, 860, NULL);
    uint32_t s1 = node(c, BPK_VAR_SET, 400, 700, "ProbeProg"), s2 = node(c, BPK_VAR_SET, 700, 700, "ProbeActive");
    lit_str(g, gp, "Quest", "Q_TestMain"), lit_str(g, gp, "Objective", "wolves"), lit_str(g, ia, "Quest", "Q_TestMain");
    link(g, pr, "", s1, ""), link(g, s1, "", s2, ""), link(g, gp, "Progress", s1, "ProbeProg"), link(g, ia, "Return Value", s2, "ProbeActive");
    uint32_t rw = node(c, BPK_EVENT_CUSTOM, 0, 900, "OnQuestReward"), sr = node(c, BPK_VAR_SET, 300, 900, "Rewarded");
    link(g, rw, "", sr, ""), lit_num(g, sr, "Rewarded", 1.0f);
    if (!bp_save(c) || !bp_compile(c)) return printf("[RealEngine] FAIL: quest: BP_QstWatcher does not compile: %s\n", c->error), false;
    // the level: the watcher, the hero with a Quest Log (the main quest at start), a Trigger Box, a Door, wolves, a bear, the elder
    Entity wt = bp_spawn(w, c, (float[3]){ -600, Y, 100 }, (float[3]){ 0, 0, 0 });
    if (wt) snprintf(((CName*)ecs_get(w, wt, COMP_NAME))->text, RE_NAME_MAX, "QstWatcher");
    Entity hero = actor(w, COMP_BIT(COMP_INVENTORY) | COMP_BIT(COMP_QUEST_LOG), "QstHero", 0, Y, 100);
    Entity zone = actor(w, COMP_BIT(COMP_COLLISION), "QstZone", 2000, Y, 100);
    Entity door = actor(w, COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_INTERACTABLE) | COMP_BIT(COMP_DEVICE), "QstDoor", 0, Y + 600, 0);
    Entity elder = actor(w, COMP_BIT(COMP_DIALOGUE_SPEAKER) | COMP_BIT(COMP_STATIC_MESH), "QstElder", -1500, Y + 900, 50);
    if (!wt || !hero || !zone || !door || !elder) return printf("[RealEngine] FAIL: quest: the actors\n"), false;
    CQuestLog* ql = ecs_get(w, hero, COMP_QUEST_LOG);
    ql->start[0] = m->hash;
    CCollision* col = ecs_get(w, zone, COMP_COLLISION);
    col->shape = COLLISION_BOX, col->extent[0] = col->extent[1] = col->extent[2] = 150.0f, col->generate_overlap_events = 1;
    ecs_component_defaults(COMP_INTERACTABLE, ecs_get(w, door, COMP_INTERACTABLE));
    ecs_component_defaults(COMP_DEVICE, ecs_get(w, door, COMP_DEVICE));
    interaction_setup_device(w, door, DEV_DOOR);
    snprintf(((CName*)ecs_get(w, door, COMP_NAME))->text, RE_NAME_MAX, "QstDoor");
    CDialogueSpeaker* spk = ecs_get(w, elder, COMP_DIALOGUE_SPEAKER);
    spk->dialogue = d->hash, spk->pause = DLG_PAUSE_NO, spk->flags = 0;
    static const char* const beasts[4][2] = { { "QstWolf1", "Enemy.Wolf.Grey" }, { "QstWolf2", "Enemy.Wolf" }, { "QstBear", "Enemy.Bear" }, { "QstBear2", "Enemy.Bear" } };
    for (int i = 0; i < 4; i++) {
        Entity b = actor(w, COMP_BIT(COMP_HEALTH) | COMP_BIT(COMP_STATIC_MESH), beasts[i][0], -800.0f - 300.0f * (float)i, Y + 400.0f, 50.0f);
        if (!b) return printf("[RealEngine] FAIL: quest: %s\n", beasts[i][0]), false;
        gt_add(w, b, beasts[i][1]);
    }
    // the level into a file (Load Progress opens it again)
    char lp[1100];
    snprintf(lp, sizeof lp, "%s/QstTest/L_QuestTest.rlevel", ui_content_root());
    if (!editor_save_level(ed, lp, 0.0)) return printf("[RealEngine] FAIL: quest: the level could not be saved in %s\n", lp), false;
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = Y - 1500.0f, ed->cam.pos[2] = 600.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -18.0f;
    ed->test_step = step;
    editor_play(ed, 0.0);
    return true;
}

bool quest_test_check(Editor* ed) {
    if (g_state < 20) printf("[RealEngine] FAIL: quest: stopped at step %d\n", g_state), g_ok = false;
    double tick = g_tick_n ? g_tick_ms / g_tick_n : 0.0;
    const QuestStats* st = quest_stats();
    printf("[RealEngine] Quest cost: quest_tick %.4f ms/frame with %u active quests (%u objectives moving), %d frames; %u completed, %u failed; "
           "last operation %.1f us; %u events\n", tick, st->active, st->objectives, g_tick_n, st->completed, st->failed, st->op_us, st->events);
    CHECK(g_tick_n > 10, "the measure's frames: %d", g_tick_n);
    CHECK(st->active >= 200, "the measure's quests: %u active (200)", st->active);
    CHECK(tick < 0.1, "quest_tick with 200 quests costs %.4f ms a frame (0.1 at most)", tick);
    editor_stop(ed, 0.0);
    g_done = true;
    if (g_ok) printf("[RealEngine] Quest test: OK\n");
    return g_ok;
}
