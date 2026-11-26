// Quests and objectives (quest.h, notas-dev/missoes.md): the Quest assets (.rquest, JSON), the Quest Logs while playing
// (fixed pools), the rules that move the objectives (the world's notifications, Go To and Collect checked by quest_tick),
// the rewards, the events (queued, sent once a change is whole), the saves' record, WBP_QuestTracker and the markers'
// list, the Debug Viewer and the self-test.
#include "loc.h" // (2026-11-24) the game's localization
#include "quest.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "components.h"
#include "debug_viewer.h"
#include "dialogue.h"
#include "gameplay_tags.h"
#include "i18n.h"
#include "inventory.h"
#include "io_json.h"
#include "platform.h"
#include "platform_files.h"
#include "ui_widget.h"

#define DEG2RAD 0.017453293f
#define TRACKER_VPATH "/Content/UI/WBP_QuestTracker.rbp"
#define TRACKER_ROWS 8          // WBP_QuestTracker's Objective1..8
#define SAVE_NAME "#Quests"
#define SAVE_MAGIC 0x31545351u  // "QST1"
#define EVQ 512                 // events waiting to be sent
#define GOTO_DEFAULT 200.0f     // Go To without a radius nor a Collision on the target: 2 m around it

const char* const g_qobj_kind_names[QOBJ_KIND_COUNT] = { "Go To", "Interact", "Collect", "Kill", "Talk", "Manual" };
const char* const g_qst_state_names[QST_STATE_COUNT] = { "Not Started", "Active", "Completed", "Failed" };

// --- The assets ---
static struct {
    char root[600];
    bool scanned;
    Quest* list[QST_MAX_ASSETS];
    uint32_t n;
} S;

// --- The logs while playing ---
typedef struct QstEntry {
    Quest* q;                   // (not saved: found again by the hash)
    uint32_t quest;             // its hash
    uint8_t state, stage, pad[2];
    uint32_t done;              // objectives done (bits)
    uint32_t stage_done;        // stages done (bits; parallel)
    uint32_t resolved;          // objectives whose target / marker was looked for (bits)
    float time;                 // seconds active
    int16_t progress[QST_MAX_OBJ];
    Entity target[QST_MAX_OBJ]; // the Go To's / marker's actor (ENTITY_NONE: none found)
    Entity mark[QST_MAX_OBJ];
} QstEntry;
typedef struct QstLog {
    bool used, is_default;
    Entity owner;               // the Quest Log's actor (ENTITY_NONE: unbound / the game's own)
    char key[RE_NAME_MAX];      // the owner's name (a saved log finds it again; the default: "")
    uint32_t n, tracked, revision;
    uint32_t inv_rev;           // the subject's Inventory revision last read (Collect)
    bool inv_check;             // Collect read again on the next tick
    QstEntry e[QST_MAX_ENTRIES];
} QstLog;
typedef struct QEv {
    uint8_t ev;
    int16_t index;
    int32_t progress, count;
    Entity owner;
    char quest[QST_NAME], obj[QST_NAME];
} QEv;
static struct {
    World* w;
    bool playing;
    QuestHooks hooks;
    QstLog logs[QST_MAX_LOGS];
    QEv evq[EVQ];
    uint32_t evh, evt;
    bool flushing;
    QuestStats st;
    uint32_t frame, default_frame;
    // the HUD
    Entity widget;
    uint32_t widget_cls, shown_rev, shown_track, shown_lang;
    QuestMarker markers[QST_MAX_MARKERS];
    uint32_t nmarkers;
} G;

// --- Small helpers ---

uint32_t quest_vpath_hash(const char* vpath) {
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) h = (h ^ (uint8_t)tolower((unsigned char)*c)) * 16777619u;
    return h ? h : 1u;
}
bool quest_is_path(const char* vpath) {
    const char* dot = vpath ? strrchr(vpath, '.') : NULL;
    return dot && !_stricmp(dot, ".rquest");
}
static void disk_of(const char* vpath, char* out, size_t cap) {
    if (!strncmp(vpath, "/Content", 8)) snprintf(out, cap, "%s%s", S.root, vpath + 8);
    else snprintf(out, cap, "%s", vpath);
}
static void vpath_of(const char* disk, char* out, size_t cap) {
    size_t rl = strlen(S.root);
    if (rl && !strncmp(disk, S.root, rl)) snprintf(out, cap, "/Content%s", disk + rl);
    else snprintf(out, cap, "%s", disk);
}
static void name_of(const char* path, char* out) {
    const char* s = strrchr(path, '/');
    snprintf(out, QST_NAME, "%s", s ? s + 1 : path);
    char* dot = strrchr(out, '.');
    if (dot) *dot = 0;
}
static char* read_text(const char* path, size_t* len) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = n > 0 ? malloc((size_t)n + 1) : NULL; // (a load, not a frame)
    if (t && fread(t, 1, (size_t)n, f) != (size_t)n) free(t), t = NULL;
    fclose(f);
    if (t) t[n] = 0, *len = (size_t)n;
    return t;
}
static int kind_named(const char* s, const char* const* names, int count, int def) {
    for (int k = 0; k < count; k++)
        if (!_stricmp(names[k], s)) return k;
    return def;
}
static void text3(char (*t)[QST_TITLE], const char* en, const char* pt, const char* es) {
    snprintf(t[0], QST_TITLE, "%s", en ? en : ""), snprintf(t[1], QST_TITLE, "%s", pt ? pt : ""), snprintf(t[2], QST_TITLE, "%s", es ? es : "");
}
static bool counted(uint8_t kind) { return kind == QOBJ_COLLECT || kind == QOBJ_KILL || kind == QOBJ_INTERACT || kind == QOBJ_MANUAL; }
static int obj_count(const QuestObjective* o) { return counted(o->kind) && o->count > 1 ? o->count : 1; }

// --- Assets: read and write ---

void quest_init(const char* content_root) {
    snprintf(S.root, sizeof S.root, "%s", content_root ? content_root : "");
    for (char* c = S.root; *c; c++)
        if (*c == '\\') *c = '/';
    for (uint32_t i = 0; i < S.n; i++) free(S.list[i]); // (another project: its own)
    S.n = 0, S.scanned = false;
    memset(G.logs, 0, sizeof G.logs); // (the pointers into the old assets go)
}

static void write_langs(JsonWriter* w, const char* key, const char* t, size_t stride) {
    jw_key(w, key);
    jw_arr_begin(w);
    for (int l = 0; l < QST_LANGS; l++) jw_str(w, t + (size_t)l * stride);
    jw_arr_end(w);
}
static void read_langs(const JsonValue* o, const char* key, char* t, size_t stride) {
    const JsonValue* a = json_get(o, key);
    for (int l = 0; l < QST_LANGS; l++) snprintf(t + (size_t)l * stride, stride, "%s", json_str(json_at(a, (size_t)l), ""));
}

bool quest_save(Quest* q) {
    if (!q) return false;
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1);
    write_langs(&w, "title", q->title[0], QST_TITLE);
    write_langs(&w, "description", q->desc[0], QST_DESC);
    jw_kstr(&w, "category", q->category);
    jw_kbool(&w, "parallel", q->parallel != 0);
    jw_kbool(&w, "repeatable", q->repeatable != 0);
    jw_kfloat(&w, "time_limit", q->time_limit);
    jw_key(&w, "stages");
    jw_arr_begin(&w);
    for (uint32_t s = 0; s < q->nstages; s++) {
        const QuestStage* st = &q->stages[s];
        jw_obj_begin(&w);
        jw_kbool(&w, "in_order", st->in_order != 0);
        write_langs(&w, "text", st->text[0], QST_TITLE);
        jw_key(&w, "objectives");
        jw_arr_begin(&w);
        for (uint32_t i = 0; i < q->nobj; i++) {
            const QuestObjective* o = &q->obj[i];
            if (o->stage != s) continue;
            jw_obj_begin(&w);
            jw_kstr(&w, "kind", g_qobj_kind_names[o->kind % QOBJ_KIND_COUNT]);
            jw_kstr(&w, "id", o->id);
            write_langs(&w, "text", o->text[0], QST_TITLE);
            jw_kstr(&w, "target", o->target);
            jw_kint(&w, "count", o->count);
            jw_kfloat(&w, "radius", o->radius);
            jw_kbool(&w, "optional", (o->flags & QO_OPTIONAL) != 0);
            jw_kstr(&w, "condition", o->condition);
            jw_kbool(&w, "condition_not", (o->flags & QO_COND_NOT) != 0);
            jw_kbool(&w, "by_owner", (o->flags & QO_BY_OWNER) != 0);
            jw_kbool(&w, "take_items", (o->flags & QO_TAKE_ITEMS) != 0);
            jw_kstr(&w, "marker", o->marker);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "rewards");
    jw_obj_begin(&w);
    jw_key(&w, "items");
    jw_arr_begin(&w);
    for (int k = 0; k < QST_REWARDS; k++) {
        if (!q->reward_item[k][0]) continue;
        jw_obj_begin(&w);
        jw_kstr(&w, "item", q->reward_item[k]);
        jw_kint(&w, "count", q->reward_count[k]);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "tags");
    jw_arr_begin(&w);
    for (int k = 0; k < QST_REWARDS; k++)
        if (q->reward_tag[k][0]) jw_str(&w, q->reward_tag[k]);
    jw_arr_end(&w);
    jw_kstr(&w, "event", q->reward_event);
    jw_kstr(&w, "event_target", q->event_target);
    jw_kstr(&w, "next_quest", q->next_quest);
    jw_obj_end(&w);
    jw_obj_end(&w);
    bool ok = !w.fail;
    if (ok) {
        char tmp[700];
        snprintf(tmp, sizeof tmp, "%s.tmp", q->path);
        FILE* f = re_fopen(tmp, "wb");
        ok = f && fwrite(w.buf, 1, w.len, f) == w.len;
        if (f) ok = fclose(f) == 0 && ok;
        ok = ok && re_replace_file(tmp, q->path);
    }
    jw_free(&w);
    return ok;
}

static void read_quest(Quest* q, const JsonValue* r) {
    read_langs(r, "title", q->title[0], QST_TITLE);
    read_langs(r, "description", q->desc[0], QST_DESC);
    snprintf(q->category, QST_NAME, "%s", json_get_str(r, "category", ""));
    q->parallel = json_get_bool(r, "parallel", false);
    q->repeatable = json_get_bool(r, "repeatable", false);
    q->time_limit = (float)json_get_num(r, "time_limit", 0.0);
    if (!(q->time_limit >= 0.0f)) q->time_limit = 0.0f;
    const JsonValue* ss = json_get(r, "stages");
    q->nstages = q->nobj = 0;
    for (size_t s = 0; s < json_len(ss) && q->nstages < QST_MAX_STAGES; s++) {
        const JsonValue* so = json_at(ss, s);
        QuestStage* st = &q->stages[q->nstages];
        st->in_order = json_get_bool(so, "in_order", false);
        read_langs(so, "text", st->text[0], QST_TITLE);
        const JsonValue* os = json_get(so, "objectives");
        for (size_t i = 0; i < json_len(os) && q->nobj < QST_MAX_OBJ; i++) {
            const JsonValue* oo = json_at(os, i);
            QuestObjective* o = &q->obj[q->nobj++];
            memset(o, 0, sizeof *o);
            o->kind = (uint8_t)kind_named(json_get_str(oo, "kind", "Manual"), g_qobj_kind_names, QOBJ_KIND_COUNT, QOBJ_MANUAL);
            o->stage = q->nstages;
            snprintf(o->id, QST_NAME, "%s", json_get_str(oo, "id", ""));
            read_langs(oo, "text", o->text[0], QST_TITLE);
            snprintf(o->target, QST_NAME, "%s", json_get_str(oo, "target", ""));
            o->count = json_get_int(oo, "count", 1);
            if (o->count < 1) o->count = 1;
            if (o->count > 30000) o->count = 30000; // (the progress is 16 bits)
            o->radius = (float)json_get_num(oo, "radius", 0.0);
            if (!(o->radius >= 0.0f)) o->radius = 0.0f;
            if (json_get_bool(oo, "optional", false)) o->flags |= QO_OPTIONAL;
            snprintf(o->condition, QST_NAME, "%s", json_get_str(oo, "condition", ""));
            if (json_get_bool(oo, "condition_not", false)) o->flags |= QO_COND_NOT;
            if (json_get_bool(oo, "by_owner", false)) o->flags |= QO_BY_OWNER;
            if (json_get_bool(oo, "take_items", false)) o->flags |= QO_TAKE_ITEMS;
            snprintf(o->marker, QST_NAME, "%s", json_get_str(oo, "marker", ""));
        }
        q->nstages++;
    }
    const JsonValue* rw = json_get(r, "rewards");
    const JsonValue* its = json_get(rw, "items");
    for (size_t k = 0, n = 0; k < json_len(its) && n < QST_REWARDS; k++) {
        const JsonValue* io = json_at(its, k);
        snprintf(q->reward_item[n], QST_NAME, "%s", json_get_str(io, "item", ""));
        q->reward_count[n] = json_get_int(io, "count", 1);
        if (q->reward_item[n][0]) n++;
    }
    const JsonValue* tg = json_get(rw, "tags");
    for (size_t k = 0, n = 0; k < json_len(tg) && n < QST_REWARDS; k++) {
        snprintf(q->reward_tag[n], QST_NAME, "%s", json_str(json_at(tg, k), ""));
        if (q->reward_tag[n][0]) n++;
    }
    snprintf(q->reward_event, QST_NAME, "%s", json_get_str(rw, "event", ""));
    snprintf(q->event_target, QST_NAME, "%s", json_get_str(rw, "event_target", ""));
    snprintf(q->next_quest, QST_NAME, "%s", json_get_str(rw, "next_quest", ""));
}

Quest* quest_load(const char* vpath) {
    if (!vpath || !vpath[0]) return NULL;
    uint32_t h = quest_vpath_hash(vpath);
    for (uint32_t i = 0; i < S.n; i++)
        if (S.list[i]->hash == h) return S.list[i];
    if (S.n >= QST_MAX_ASSETS) return NULL;
    char path[700];
    disk_of(vpath, path, sizeof path);
    size_t len = 0;
    char* text = read_text(path, &len);
    if (!text) return NULL;
    JsonDoc doc;
    char err[160];
    Quest* q = NULL;
    if (json_parse(text, len, &doc, err, sizeof err)) {
        q = calloc(1, sizeof *q); // (an asset, not a frame)
        if (q) {
            snprintf(q->path, sizeof q->path, "%s", path);
            name_of(path, q->name);
            q->hash = h;
            read_quest(q, &doc.root);
            S.list[S.n++] = q;
        }
        json_free(&doc);
    } else
        printf("[RealEngine] Quest: %s: %s\n", vpath, err);
    free(text);
    return q;
}

static void scan_dir(const char* disk, int depth);
static void scan_entry(void* user, const ReDirEntry* e) {
    const char* dir = user;
    char path[800], vpath[800];
    snprintf(path, sizeof path, "%s/%s", dir, e->name);
    if (e->is_dir) {
        scan_dir(path, 1);
        return;
    }
    if (!quest_is_path(e->name)) return;
    vpath_of(path, vpath, sizeof vpath);
    quest_load(vpath);
}
static void scan_dir(const char* disk, int depth) {
    if (depth <= 12) re_dir_walk(disk, scan_entry, (void*)disk);
}
static void scan(void) {
    if (!S.scanned && S.root[0]) S.scanned = true, scan_dir(S.root, 0);
}

Quest* quest_find(uint32_t hash) {
    if (!hash) return NULL;
    for (uint32_t i = 0; i < S.n; i++)
        if (S.list[i]->hash == hash) return S.list[i];
    scan();
    for (uint32_t i = 0; i < S.n; i++)
        if (S.list[i]->hash == hash) return S.list[i];
    return NULL;
}
Quest* quest_find_name(const char* name) {
    if (!name || !name[0]) return NULL;
    for (uint32_t i = 0; i < S.n; i++)
        if (!_stricmp(S.list[i]->name, name)) return S.list[i];
    scan();
    for (uint32_t i = 0; i < S.n; i++)
        if (!_stricmp(S.list[i]->name, name)) return S.list[i];
    if (strchr(name, '/')) return quest_load(name); // (a vpath)
    return NULL;
}
uint32_t quest_count(void) {
    scan();
    return S.n;
}
Quest* quest_at(uint32_t i) { return i < S.n ? S.list[i] : NULL; }

// --- Editing ---

int quest_add_stage(Quest* q) {
    if (!q || q->nstages >= QST_MAX_STAGES) return -1;
    memset(&q->stages[q->nstages], 0, sizeof q->stages[0]);
    q->version++;
    return q->nstages++;
}
void quest_remove_stage(Quest* q, int s) {
    if (!q || s < 0 || s >= q->nstages) return;
    uint32_t k = 0;
    for (uint32_t i = 0; i < q->nobj; i++) {
        if (q->obj[i].stage == s) continue;
        QuestObjective o = q->obj[i];
        if (o.stage > s) o.stage--;
        q->obj[k++] = o;
    }
    q->nobj = (uint8_t)k;
    memmove(&q->stages[s], &q->stages[s + 1], sizeof q->stages[0] * (size_t)(q->nstages - s - 1));
    q->nstages--;
    q->version++;
}
int quest_add_objective(Quest* q, int stage, uint8_t kind) {
    if (!q || q->nobj >= QST_MAX_OBJ || kind >= QOBJ_KIND_COUNT) return -1;
    if (!q->nstages) quest_add_stage(q);
    if (stage < 0 || stage >= q->nstages) stage = q->nstages - 1;
    int at = 0; // after the stage's last one (the objectives stay in the stages' order)
    for (uint32_t i = 0; i < q->nobj; i++)
        if (q->obj[i].stage <= stage) at = (int)i + 1;
    memmove(&q->obj[at + 1], &q->obj[at], sizeof q->obj[0] * (size_t)(q->nobj - at));
    QuestObjective* o = &q->obj[at];
    memset(o, 0, sizeof *o);
    o->kind = kind, o->stage = (uint8_t)stage, o->count = 1;
    snprintf(o->id, QST_NAME, "obj%d", (int)q->nobj + 1);
    for (bool clash = true; clash;) { // (a unique id)
        clash = false;
        for (uint32_t i = 0; i <= q->nobj; i++)
            if ((int)i != at && !_stricmp(q->obj[i].id, o->id)) clash = true;
        if (clash) snprintf(o->id, QST_NAME, "obj%u", (unsigned)(rand() % 9000 + 1000));
    }
    q->nobj++;
    q->version++;
    return at;
}
void quest_remove_objective(Quest* q, int o) {
    if (!q || o < 0 || o >= q->nobj) return;
    memmove(&q->obj[o], &q->obj[o + 1], sizeof q->obj[0] * (size_t)(q->nobj - o - 1));
    q->nobj--;
    q->version++;
}

// The example: "The Lost Amulet" with every kind of objective in three stages, a reward and an optional one
static void make_example(Quest* q) {
    text3(q->title, "The Lost Amulet", "O Amuleto Perdido", "El Amuleto Perdido");
    snprintf(q->desc[0], QST_DESC, "The village elder lost her amulet near the old well. Find it and bring it back.");
    snprintf(q->desc[1], QST_DESC, "A anciã da vila perdeu o amuleto perto do poço velho. Encontre-o e traga de volta.");
    snprintf(q->desc[2], QST_DESC, "La anciana del pueblo perdió su amuleto cerca del pozo viejo. Encuéntralo y tráelo de vuelta.");
    snprintf(q->category, QST_NAME, "Side");
    quest_add_stage(q), quest_add_stage(q), quest_add_stage(q);
    text3(q->stages[0].text, "Find the amulet", "Encontre o amuleto", "Encuentra el amuleto");
    text3(q->stages[1].text, "Deal with the wolves", "Cuide dos lobos", "Ocúpate de los lobos");
    text3(q->stages[2].text, "Return to the elder", "Volte para a anciã", "Vuelve con la anciana");
    q->stages[0].in_order = 1;
    int o = quest_add_objective(q, 0, QOBJ_GO_TO);
    snprintf(q->obj[o].id, QST_NAME, "reach_well"), snprintf(q->obj[o].target, QST_NAME, "OldWell"), q->obj[o].radius = 300.0f;
    text3(q->obj[o].text, "Go to the old well", "Vá até o poço velho", "Ve al pozo viejo");
    o = quest_add_objective(q, 0, QOBJ_INTERACT);
    snprintf(q->obj[o].id, QST_NAME, "search_chest"), snprintf(q->obj[o].target, QST_NAME, "Treasure Chest");
    text3(q->obj[o].text, "Search the chest", "Procure no baú", "Busca en el cofre");
    o = quest_add_objective(q, 0, QOBJ_COLLECT);
    snprintf(q->obj[o].id, QST_NAME, "herbs"), snprintf(q->obj[o].target, QST_NAME, "DA_Herb"), q->obj[o].count = 3;
    q->obj[o].flags = QO_OPTIONAL | QO_TAKE_ITEMS;
    text3(q->obj[o].text, "Gather herbs for the elder", "Junte ervas para a anciã", "Recoge hierbas para la anciana");
    o = quest_add_objective(q, 1, QOBJ_KILL);
    snprintf(q->obj[o].id, QST_NAME, "wolves"), snprintf(q->obj[o].target, QST_NAME, "Enemy.Wolf"), q->obj[o].count = 2;
    text3(q->obj[o].text, "Kill the wolves", "Mate os lobos", "Mata a los lobos");
    o = quest_add_objective(q, 2, QOBJ_TALK);
    snprintf(q->obj[o].id, QST_NAME, "talk_elder"), snprintf(q->obj[o].target, QST_NAME, "Elder");
    text3(q->obj[o].text, "Talk to the elder", "Fale com a anciã", "Habla con la anciana");
    snprintf(q->reward_item[0], QST_NAME, "DA_Coin"), q->reward_count[0] = 25;
    snprintf(q->reward_tag[0], QST_NAME, "Quest.LostAmulet.Done");
    snprintf(q->reward_event, QST_NAME, "OnQuestReward");
}

Quest* quest_create(const char* folder_vpath, const char* name, bool example) {
    Quest* q = calloc(1, sizeof *q);
    if (!q) return NULL;
    char vpath[700];
    for (int i = 0; i < 1000; i++) {
        if (i) snprintf(vpath, sizeof vpath, "%s/%s%d.rquest", folder_vpath, name, i);
        else snprintf(vpath, sizeof vpath, "%s/%s.rquest", folder_vpath, name);
        disk_of(vpath, q->path, sizeof q->path);
        if (!re_file_stat(q->path, NULL, NULL, NULL)) break;
    }
    name_of(q->path, q->name);
    q->hash = quest_vpath_hash(vpath);
    if (example) make_example(q);
    else {
        text3(q->title, "New Quest", "Nova Missão", "Nueva Misión");
        quest_add_stage(q);
        int o = quest_add_objective(q, 0, QOBJ_MANUAL);
        if (o >= 0) text3(q->obj[o].text, "Do something", "Faça algo", "Haz algo");
    }
    if (!quest_save(q)) {
        free(q);
        return NULL;
    }
    for (uint32_t i = 0; i < S.n; i++) // (one loaded from that path before, its file deleted since: this one in its place)
        if (S.list[i]->hash == q->hash) {
            *S.list[i] = *q;
            free(q);
            return S.list[i];
        }
    if (S.n >= QST_MAX_ASSETS) {
        free(q);
        return NULL;
    }
    S.list[S.n++] = q;
    return q;
}

// --- Texts (the language: dialogue.h's) ---

int quest_language(void) { return dlg_language(); }
static const char* pick(const char* t, size_t stride, int lang) {
    if (lang > 0 && lang < QST_LANGS && t[(size_t)lang * stride]) return t + (size_t)lang * stride;
    if ((lang == 1 || lang == 2) && t[0]) return tr_lang(t, lang == 1 ? LANG_PT_BR : LANG_ES); // (i18n's table)
    return t;
}
// (2026-11-24) the project's localization first (loc.h: Q:<quest>, title / desc / obj<i>); NULL: not there
static const char* qloc(const Quest* q, const char* key) {
    if (!q || !loc_active()) return NULL;
    char ns[LOC_NS];
    snprintf(ns, sizeof ns, "Q:%s", q->name);
    return loc_text(ns, key, NULL);
}
const char* quest_title(const Quest* q, int lang) {
    if (!q) return "";
    const char* lt = qloc(q, "title");
    if (lt) return lt;
    const char* t = pick(q->title[0], QST_TITLE, lang);
    return t[0] ? t : q->name;
}
const char* quest_desc(const Quest* q, int lang) {
    const char* lt = qloc(q, "desc");
    return lt ? lt : q ? pick(q->desc[0], QST_DESC, lang) : "";
}
const char* quest_obj_text(const Quest* q, const QuestObjective* o, int lang) {
    if (!o) return "";
    if (q && o >= q->obj && o < q->obj + 16) {
        char key[16];
        snprintf(key, sizeof key, "obj%d", (int)(o - q->obj));
        const char* lt = qloc(q, key);
        if (lt) return lt;
    }
    const char* t = pick(o->text[0], QST_TITLE, lang);
    if (t[0]) return t;
    static char made[QST_TITLE]; // (no text: made from the kind and the target)
    static const char* const verbs[QST_LANGS][QOBJ_KIND_COUNT] = {
        { "Go to %s", "Use %s", "Collect %s", "Defeat %s", "Talk to %s", "%s" },
        { "Vá até %s", "Use %s", "Colete %s", "Derrote %s", "Fale com %s", "%s" },
        { "Ve a %s", "Usa %s", "Recoge %s", "Derrota %s", "Habla con %s", "%s" },
    };
    int l = lang >= 0 && lang < QST_LANGS ? lang : 0;
    const char* tg = o->target[0] ? o->target : o->id;
    if (o->kind == QOBJ_COLLECT) { // (the item's Display Name)
        const ItemDef* it = inv_item_by_name(o->target);
        if (it && it->display[0]) tg = it->display;
    }
    snprintf(made, sizeof made, verbs[l][o->kind % QOBJ_KIND_COUNT], tg);
    return made;
}
int quest_obj_index(const Quest* q, const char* id) {
    if (!q || !id || !id[0]) return -1;
    for (uint32_t i = 0; i < q->nobj; i++)
        if (!_stricmp(q->obj[i].id, id)) return (int)i;
    char* end = NULL;
    long v = strtol(id, &end, 10);
    if (end && end != id && !*end && v >= 0 && v < q->nobj) return (int)v;
    return -1;
}

// --- The events: queued, sent once a change is whole ---

static void push_ev(uint8_t ev, const QstLog* l, const Quest* q, int obj, int progress, int count) {
    if (G.evt - G.evh >= EVQ) return; // (full: an event storm; dropped)
    QEv* e = &G.evq[G.evt++ % EVQ];
    e->ev = ev, e->index = (int16_t)obj, e->progress = progress, e->count = count, e->owner = l->owner;
    snprintf(e->quest, QST_NAME, "%s", q->name);
    snprintf(e->obj, QST_NAME, "%s", obj >= 0 && obj < q->nobj ? q->obj[obj].id : "");
}
static void flush(void) {
    if (G.flushing) return;
    G.flushing = true;
    while (G.evh != G.evt) {
        QEv e = G.evq[G.evh++ % EVQ];
        G.st.events++;
        if (G.hooks.event && G.w) G.hooks.event(G.w, e.ev, e.owner, e.quest, e.obj, e.index, e.progress, e.count);
    }
    G.evh = G.evt = 0;
    G.flushing = false;
}

// --- The world ---

static Entity player(World* w) { return G.hooks.player && w ? G.hooks.player(w) : ENTITY_NONE; }
static bool name_is(World* w, Entity e, const char* key) {
    const CName* n = ecs_get(w, e, COMP_NAME);
    return n && !_stricmp(n->text, key);
}
// an actor named key, else one with the Gameplay Tag key (ENTITY_NONE: none)
static Entity find_actor(World* w, const char* key) {
    if (!w || !key || !key[0]) return ENTITY_NONE;
    Entity tagged = ENTITY_NONE;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_NAME))) continue;
        const CName* nm = ecs_arch_column(a, COMP_NAME);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            if (!_stricmp(nm[i].text, key)) return ents[i];
        if (!tagged && (a->mask & COMP_BIT(COMP_GAMEPLAY_TAGS)))
            for (uint32_t i = 0; i < a->count && !tagged; i++)
                if (gt_has(w, ents[i], key, false)) tagged = ents[i];
    }
    return tagged;
}
// what an objective's target means for an actor: its name, or a Gameplay Tag on it ("" matches anything)
static bool matches(World* w, Entity e, const char* key) {
    if (!key[0]) return true;
    return e && ecs_alive(w, e) && (name_is(w, e, key) || gt_has(w, e, key, false));
}

// --- The logs ---

static const CQuestLog* comp_of(World* w, const QstLog* l) {
    return w && l->owner && ecs_alive(w, l->owner) ? ecs_get(w, l->owner, COMP_QUEST_LOG) : NULL;
}
static uint32_t log_flags(World* w, const QstLog* l) {
    const CQuestLog* c = comp_of(w, l);
    return c ? c->flags : (QL_TRACKER | QL_MARKERS | QL_AUTO_TRACK); // (the game's own log: the defaults)
}
// who the quests are about: a pawn's log, its pawn; a Game State's (or the game's own), the player
static Entity subject(World* w, const QstLog* l) {
    if (l->owner && ecs_alive(w, l->owner) && ecs_get(w, l->owner, COMP_CHARACTER)) return l->owner;
    Entity p = player(w);
    return p ? p : (l->owner && ecs_alive(w, l->owner) ? l->owner : ENTITY_NONE);
}
static bool bound_elsewhere(Entity e) {
    for (int i = 0; i < QST_MAX_LOGS; i++)
        if (G.logs[i].used && !G.logs[i].is_default && G.logs[i].owner == e) return true;
    return false;
}
// the default log's actor: the player's Quest Log, else the first Quest Log (a Game State), else none (the game's own)
static Entity default_owner(World* w) {
    if (!w) return ENTITY_NONE;
    Entity p = player(w);
    if (p && ecs_get(w, p, COMP_QUEST_LOG)) return p;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_QUEST_LOG))) continue;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            if (!bound_elsewhere(ents[i])) return ents[i];
    }
    return ENTITY_NONE;
}
static QstLog* new_log(bool is_default, Entity owner, const char* key) {
    for (int i = 0; i < QST_MAX_LOGS; i++)
        if (!G.logs[i].used) {
            QstLog* l = &G.logs[i];
            memset(l, 0, sizeof *l);
            l->used = true, l->is_default = is_default, l->owner = owner, l->inv_check = true;
            snprintf(l->key, sizeof l->key, "%s", key ? key : "");
            return l;
        }
    return NULL;
}
static QstLog* default_log(World* w, bool make) {
    QstLog* l = NULL;
    for (int i = 0; i < QST_MAX_LOGS && !l; i++)
        if (G.logs[i].used && G.logs[i].is_default) l = &G.logs[i];
    if (!l && make) l = new_log(true, ENTITY_NONE, "");
    if (l && w && G.default_frame != G.frame + 1) { // (bound again once a frame: the player possessed, a Game State made)
        G.default_frame = G.frame + 1;
        Entity o = default_owner(w);
        if (o != l->owner) l->owner = o, l->revision++, l->inv_check = true;
    }
    return l;
}
// the log of the actor (ENTITY_NONE or one without a Quest Log: the default one)
static QstLog* log_get(World* w, Entity owner, bool make) {
    if (!owner || !w || !ecs_alive(w, owner) || !ecs_get(w, owner, COMP_QUEST_LOG)) return default_log(w, make);
    QstLog* d = default_log(w, make);
    if (d && d->owner == owner) return d;
    for (int i = 0; i < QST_MAX_LOGS; i++)
        if (G.logs[i].used && !G.logs[i].is_default && G.logs[i].owner == owner) return &G.logs[i];
    for (int i = 0; i < QST_MAX_LOGS; i++) // (a saved log, or the last level's: found by its actor's name)
        if (G.logs[i].used && !G.logs[i].is_default && (!G.logs[i].owner || !ecs_alive(w, G.logs[i].owner)) && name_is(w, owner, G.logs[i].key)) {
            G.logs[i].owner = owner, G.logs[i].inv_check = true;
            return &G.logs[i];
        }
    if (!make) return NULL;
    const CName* n = ecs_get(w, owner, COMP_NAME);
    return new_log(false, owner, n ? n->text : "");
}
static QstEntry* entry_of(QstLog* l, uint32_t hash) {
    for (uint32_t i = 0; l && i < l->n; i++)
        if (l->e[i].quest == hash) return &l->e[i];
    return NULL;
}
static Quest* named(const char* quest) {
    Quest* q = quest_find_name(quest);
    if (!q && quest && quest[0]) printf("[RealEngine] Quest: no Quest \"%s\"\n", quest);
    return q;
}
static QstEntry* entry_named(World* w, Entity owner, const char* quest, QstLog** lo) {
    QstLog* l = log_get(w, owner, false);
    Quest* q = quest_find_name(quest);
    if (lo) *lo = l;
    return q ? entry_of(l, q->hash) : NULL;
}

// --- The rules ---

static bool stage_active(const QstEntry* e, const Quest* q, int s) {
    return q->parallel ? !(e->stage_done >> s & 1u) : s == e->stage;
}
static bool cond_ok(World* w, const QstLog* l, const QuestObjective* o) {
    if (!o->condition[0]) return true;
    Entity s = w ? subject(w, l) : ENTITY_NONE;
    bool has = s && gt_has(w, s, o->condition, false);
    return (o->flags & QO_COND_NOT) ? !has : has;
}
// it can move now: its quest active, its stage on, not done, the earlier ones done (a stage in order), its condition
static bool obj_active(World* w, const QstLog* l, const QstEntry* e, int i) {
    const Quest* q = e->q;
    if (e->state != QST_ACTIVE || !q || i < 0 || i >= q->nobj || (e->done >> i & 1u)) return false;
    const QuestObjective* o = &q->obj[i];
    if (!stage_active(e, q, o->stage)) return false;
    if (q->stages[o->stage].in_order)
        for (int j = 0; j < i; j++)
            if (q->obj[j].stage == o->stage && !(q->obj[j].flags & QO_OPTIONAL) && !(e->done >> j & 1u)) return false;
    return cond_ok(w, l, o);
}
static bool stage_done(const QstEntry* e, const Quest* q, int s) {
    for (uint32_t i = 0; i < q->nobj; i++)
        if (q->obj[i].stage == s && !(q->obj[i].flags & QO_OPTIONAL) && !(e->done >> i & 1u)) return false;
    return true;
}

static void complete(World* w, QstLog* l, QstEntry* e);
static void collect_check(World* w, QstLog* l, QstEntry* e);

static void enter_stage(QstLog* l, QstEntry* e) {
    e->resolved = 0; // (its targets and markers looked for again)
    l->inv_check = true, l->revision++;
}
// the stages after a change: done ones passed (one after another) or marked (parallel); all done: the quest completes
static void check_stages(World* w, QstLog* l, QstEntry* e) {
    const Quest* q = e->q;
    if (e->state != QST_ACTIVE || !q) return;
    if (q->parallel) {
        for (int s = 0; s < q->nstages; s++)
            if (!(e->stage_done >> s & 1u) && stage_done(e, q, s)) e->stage_done |= 1u << s, l->revision++;
        if (q->nstages && e->stage_done == (1u << q->nstages) - 1u) complete(w, l, e);
        else if (!q->nstages) complete(w, l, e);
        return;
    }
    bool moved = false;
    while (e->stage < q->nstages && stage_done(e, q, e->stage)) e->stage++, moved = true;
    if (e->stage >= q->nstages) {
        complete(w, l, e);
        return;
    }
    if (moved) {
        enter_stage(l, e);
        collect_check(w, l, e); // (the items may be there already)
    }
}

// an objective's progress to v (clamped to its count): its events; done, the stages after it
static bool set_progress(World* w, QstLog* l, QstEntry* e, int i, int v) {
    const Quest* q = e->q;
    if (!obj_active(w, l, e, i)) return false;
    int n = obj_count(&q->obj[i]);
    if (v < 0) v = 0;
    if (v > n) v = n;
    if (v == e->progress[i]) return v >= n;
    e->progress[i] = (int16_t)v;
    l->revision++;
    push_ev(QST_EV_OBJ_UPDATED, l, q, i, v, n);
    if (v >= n) {
        e->done |= 1u << i;
        l->inv_check = true; // (an objective in order after it may be a Collect: the items held count from now)
        push_ev(QST_EV_OBJ_COMPLETED, l, q, i, v, n);
        check_stages(w, l, e);
        return true;
    }
    return false;
}

static void collect_check(World* w, QstLog* l, QstEntry* e) {
    const Quest* q = e->q;
    Entity s = w ? subject(w, l) : ENTITY_NONE;
    for (int i = 0; q && i < q->nobj && e->state == QST_ACTIVE; i++) {
        if (q->obj[i].kind != QOBJ_COLLECT || !obj_active(w, l, e, i)) continue;
        const ItemDef* it = inv_item_by_name(q->obj[i].target);
        int have = it && s ? inv_count(w, s, it->hash) : 0;
        set_progress(w, l, e, i, have);
    }
}

static void untrack(QstLog* l, uint32_t hash) {
    if (l->tracked != hash) return;
    l->tracked = 0;
    for (uint32_t i = 0; i < l->n; i++) // (the next active one)
        if (l->e[i].state == QST_ACTIVE && l->e[i].quest != hash) {
            l->tracked = l->e[i].quest;
            break;
        }
    l->revision++;
}

static bool start(World* w, QstLog* l, Quest* q) {
    if (!l || !q) return false;
    QstEntry* e = entry_of(l, q->hash);
    if (e && (e->state == QST_ACTIVE || (e->state == QST_COMPLETED && !q->repeatable))) return false;
    if (!e) {
        if (l->n >= QST_MAX_ENTRIES) {
            printf("[RealEngine] Quest: the log is full (%d quests)\n", QST_MAX_ENTRIES);
            return false;
        }
        e = &l->e[l->n++];
    }
    memset(e, 0, sizeof *e);
    e->q = q, e->quest = q->hash, e->state = QST_ACTIVE;
    push_ev(QST_EV_STARTED, l, q, -1, 0, 0);
    if (!l->tracked && (log_flags(w, l) & QL_AUTO_TRACK)) l->tracked = q->hash;
    enter_stage(l, e);
    check_stages(w, l, e); // (empty stages pass; a quest with nothing to do completes)
    if (e->state == QST_ACTIVE) collect_check(w, l, e);
    return true;
}

static void complete(World* w, QstLog* l, QstEntry* e) {
    Quest* q = e->q;
    if (e->state != QST_ACTIVE || !q) return;
    e->state = QST_COMPLETED;
    l->revision++;
    Entity s = w ? subject(w, l) : ENTITY_NONE;
    if (w && s) {
        for (int i = 0; i < q->nobj; i++) // the items handed in
            if (q->obj[i].kind == QOBJ_COLLECT && (q->obj[i].flags & QO_TAKE_ITEMS) && (e->done >> i & 1u)) {
                const ItemDef* it = inv_item_by_name(q->obj[i].target);
                if (it) inv_remove(w, s, it->hash, obj_count(&q->obj[i]));
            }
        for (int k = 0; k < QST_REWARDS; k++) { // the rewards
            if (q->reward_item[k][0]) {
                const ItemDef* it = inv_item_by_name(q->reward_item[k]);
                if (it) inv_add(w, s, it->hash, q->reward_count[k] > 0 ? q->reward_count[k] : 1);
                else printf("[RealEngine] Quest %s: no Item Definition \"%s\" (reward)\n", q->name, q->reward_item[k]);
            }
            if (q->reward_tag[k][0]) gt_add(w, s, q->reward_tag[k]);
        }
    }
    push_ev(QST_EV_COMPLETED, l, q, -1, 0, 0);
    if (w && q->reward_event[0] && G.hooks.call) {
        const char* args[1] = { q->name };
        Entity who = l->owner && ecs_alive(w, l->owner) ? l->owner : s;
        bool any = who && G.hooks.call(w, who, q->reward_event, args, 1);
        if (who != s && s) any = G.hooks.call(w, s, q->reward_event, args, 1) || any;
        Entity t = q->event_target[0] ? find_actor(w, q->event_target) : ENTITY_NONE;
        if (t && t != who && t != s) any = G.hooks.call(w, t, q->reward_event, args, 1) || any;
        if (!any) printf("[RealEngine] Quest %s: no Custom Event \"%s\" on its log's actor\n", q->name, q->reward_event);
    }
    uint32_t hash = q->hash;
    untrack(l, hash);
    if (q->next_quest[0]) start(w, l, named(q->next_quest)); // (e may move: nothing of it is read after)
}

static bool fail(World* w, QstLog* l, QstEntry* e) {
    (void)w;
    if (!e || e->state != QST_ACTIVE) return false;
    e->state = QST_FAILED;
    l->revision++;
    push_ev(QST_EV_FAILED, l, e->q, -1, 0, 0);
    untrack(l, e->quest);
    return true;
}

// --- What the nodes do ---

static void op_begin(double* t0) { *t0 = re_time_seconds(); }
static void op_end(double t0) {
    flush();
    G.st.op_us = (re_time_seconds() - t0) * 1e6;
}

bool quest_start(World* w, Entity log, const char* quest) {
    double t0;
    op_begin(&t0);
    bool ok = start(w, log_get(w, log, true), named(quest));
    op_end(t0);
    return ok;
}
bool quest_complete(World* w, Entity log, const char* quest) {
    double t0;
    op_begin(&t0);
    QstLog* l;
    QstEntry* e = entry_named(w, log, quest, &l);
    bool ok = e && e->state == QST_ACTIVE;
    if (ok) complete(w, l, e);
    op_end(t0);
    return ok;
}
bool quest_fail(World* w, Entity log, const char* quest) {
    double t0;
    op_begin(&t0);
    QstLog* l;
    QstEntry* e = entry_named(w, log, quest, &l);
    bool ok = e && fail(w, l, e);
    op_end(t0);
    return ok;
}
bool quest_advance(World* w, Entity log, const char* quest, const char* obj, int amount, bool set) {
    double t0;
    op_begin(&t0);
    QstLog* l;
    QstEntry* e = entry_named(w, log, quest, &l);
    int i = e ? quest_obj_index(e->q, obj) : -1;
    bool done = false;
    if (i >= 0) {
        if (!set && amount <= 0) amount = 1;
        done = set_progress(w, l, e, i, set ? amount : e->progress[i] + amount);
        done = done || (e->done >> i & 1u);
    } else if (e)
        printf("[RealEngine] Quest %s: no objective \"%s\"\n", e->q ? e->q->name : "?", obj ? obj : "");
    op_end(t0);
    return done;
}
int quest_state(World* w, Entity log, const char* quest, int* stage) {
    QstEntry* e = entry_named(w, log, quest, NULL);
    if (stage) *stage = e ? e->stage : 0;
    return e ? e->state : QST_NOT_STARTED;
}
bool quest_objective(World* w, Entity log, const char* quest, const char* obj, int* progress, int* count, bool* done, const char** text) {
    QstEntry* e = entry_named(w, log, quest, NULL);
    Quest* q = e ? e->q : quest_find_name(quest);
    int i = q ? quest_obj_index(q, obj) : -1;
    if (progress) *progress = e && i >= 0 ? e->progress[i] : 0;
    if (count) *count = i >= 0 ? obj_count(&q->obj[i]) : 0;
    if (done) *done = e && i >= 0 && (e->done >> i & 1u);
    if (text) *text = i >= 0 ? quest_obj_text(q, &q->obj[i], quest_language()) : "";
    return i >= 0;
}
bool quest_track(World* w, Entity log, const char* quest) {
    QstLog* l;
    QstEntry* e = entry_named(w, log, quest, &l);
    if (!quest || !quest[0]) { // (none tracked)
        l = log_get(w, log, false);
        if (l) l->tracked = 0, l->revision++;
        return l != NULL;
    }
    if (!e || e->state != QST_ACTIVE) return false;
    l->tracked = e->quest, l->revision++;
    return true;
}
const Quest* quest_tracked(World* w, Entity log) {
    QstLog* l = log_get(w, log, false);
    QstEntry* e = l ? entry_of(l, l->tracked) : NULL;
    return e && e->state == QST_ACTIVE ? e->q : NULL;
}

// --- The world's notifications ---

static void notify(World* w, uint8_t kind, Entity actor, Entity instigator, const char* dialogue) {
    if (!G.playing || !w) return;
    double t0;
    op_begin(&t0);
    for (int li = 0; li < QST_MAX_LOGS; li++) {
        QstLog* l = &G.logs[li];
        if (!l->used || !l->n) continue;
        Entity s = subject(w, l);
        if (kind == QOBJ_KILL && actor == s) continue; // (the player's own death is not a kill)
        uint32_t n = l->n; // (a quest this starts — the next one — doesn't count this same deed)
        for (uint32_t k = 0; k < n; k++) {
            QstEntry* e = &l->e[k];
            const Quest* q = e->q;
            if (e->state != QST_ACTIVE || !q) continue;
            for (int i = 0; i < q->nobj && e->state == QST_ACTIVE; i++) {
                const QuestObjective* o = &q->obj[i];
                if (o->kind != kind || !obj_active(w, l, e, i)) continue;
                bool hit = matches(w, actor, o->target);
                if (kind == QOBJ_TALK && !hit && dialogue && o->target[0] && !_stricmp(dialogue, o->target)) hit = true;
                if (kind == QOBJ_KILL && (o->flags & QO_BY_OWNER) && instigator != s && instigator != l->owner) hit = false;
                if (kind == QOBJ_INTERACT && instigator && s && instigator != s && instigator != l->owner) hit = false; // (another's use)
                if (hit) set_progress(w, l, e, i, e->progress[i] + 1);
            }
        }
    }
    op_end(t0);
}
void quest_notify_death(World* w, Entity dead, Entity killer) { notify(w, QOBJ_KILL, dead, killer, NULL); }
void quest_notify_interact(World* w, Entity target, Entity instigator) { notify(w, QOBJ_INTERACT, target, instigator, NULL); }
void quest_notify_talk(World* w, Entity speaker, const char* dialogue) { notify(w, QOBJ_TALK, speaker, ENTITY_NONE, dialogue); }

// --- Play ---

void quest_set_hooks(const QuestHooks* h) { G.hooks = h ? *h : (QuestHooks){ 0 }; }
const QuestStats* quest_stats(void) {
    G.st.assets = S.n;
    return &G.st;
}

void quest_begin_play(World* w) {
    G.w = w, G.playing = true;
    G.evh = G.evt = 0, G.flushing = false;
    G.widget = ENTITY_NONE, G.widget_cls = 0, G.shown_rev = UINT32_MAX, G.nmarkers = 0;
    G.default_frame = 0;
    G.st.events = 0, G.st.tick_ms = G.st.op_us = 0.0;
    for (int i = 0; i < QST_MAX_LOGS; i++) { // (a level change keeps the logs; their actors are found again)
        QstLog* l = &G.logs[i];
        if (!l->used) continue;
        l->owner = ENTITY_NONE, l->inv_check = true, l->inv_rev = 0, l->revision++;
        for (uint32_t k = 0; k < l->n; k++) {
            l->e[k].q = quest_find(l->e[k].quest), l->e[k].resolved = 0;
            if (!l->e[k].q) l->e[k].state = QST_NOT_STARTED; // (its asset is gone)
        }
    }
}

static void show_tracker(World* w, const QstLog* l);
void quest_after_begin_play(World* w) {
    if (!w) return;
    double t0;
    op_begin(&t0);
    // every Quest Log's starting quests (its own log): the actors first (the events may move the archetypes)
    Entity owners[QST_MAX_LOGS];
    uint32_t no = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_QUEST_LOG))) continue;
        for (uint32_t i = 0; i < a->count && no < QST_MAX_LOGS; i++) owners[no++] = ecs_arch_entities(a)[i];
    }
    for (uint32_t j = 0; j < no; j++) {
        QstLog* l = log_get(w, owners[j], true);
        for (int k = 0; l && k < QL_START_QUESTS; k++) {
            const CQuestLog* c = ecs_alive(w, owners[j]) ? ecs_get(w, owners[j], COMP_QUEST_LOG) : NULL;
            Quest* q = c && c->start[k] ? quest_find(c->start[k]) : NULL;
            if (q && !entry_of(l, q->hash)) start(w, l, q);
        }
    }
    op_end(t0);
    QstLog* d = default_log(w, false);
    if (d && (log_flags(w, d) & QL_TRACKER)) show_tracker(w, d);
}

void quest_end_play(World* w) {
    (void)w;
    G.playing = false, G.w = NULL;
    G.widget = ENTITY_NONE, G.nmarkers = 0;
    G.evh = G.evt = 0;
    for (int i = 0; i < QST_MAX_LOGS; i++) G.logs[i].owner = ENTITY_NONE;
}
void quest_clear(void) {
    memset(G.logs, 0, sizeof G.logs);
    G.shown_rev = UINT32_MAX, G.default_frame = 0;
}
void quest_session_end(void) { quest_clear(); }

// Go To: the subject inside the target's box (its Collision), or within the radius
static bool inside(World* w, Entity t, const float p[3], float radius) {
    const CTransform* tt = ecs_get(w, t, COMP_TRANSFORM);
    if (!tt) return false;
    float d[3] = { p[0] - tt->location[0], p[1] - tt->location[1], p[2] - tt->location[2] };
    if (radius > 0.0f) return d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= radius * radius;
    const CCollision* c = ecs_get(w, t, COMP_COLLISION);
    if (!c) return d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= GOTO_DEFAULT * GOTO_DEFAULT;
    float sx = fabsf(tt->scale[0]), sy = fabsf(tt->scale[1]), sz = fabsf(tt->scale[2]);
    if (c->shape == COLLISION_SPHERE) {
        float r = c->extent[0] * fmaxf(sx, fmaxf(sy, sz));
        return d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= r * r;
    }
    float yaw = tt->rotation[1] * DEG2RAD, cy = cosf(yaw), sn = sinf(yaw); // (the box turned by its yaw)
    float lx = d[0] * cy + d[1] * sn, ly = -d[0] * sn + d[1] * cy;
    float ex = c->extent[0] * sx, ey = c->extent[1] * sy, ez = c->extent[2] * sz;
    if (c->shape == COLLISION_CAPSULE) ex = ey = c->extent[0] * fmaxf(sx, sy), ez = c->extent[1] * sz;
    return fabsf(lx) <= ex && fabsf(ly) <= ey && fabsf(d[2]) <= ez;
}

// the Go To's target and the marker's actor looked for (by name or tag): once, again when one died, and every 64 frames
// while one is missing (spawned later)
static void resolve(World* w, QstEntry* e, int i) {
    const QuestObjective* o = &e->q->obj[i];
    bool place = o->kind == QOBJ_GO_TO || o->kind == QOBJ_INTERACT || o->kind == QOBJ_TALK;
    if (e->resolved >> i & 1u) {
        bool alive = (!e->target[i] || ecs_alive(w, e->target[i])) && (!e->mark[i] || ecs_alive(w, e->mark[i]));
        bool missing = (place && o->target[0] && !e->target[i]) || (o->marker[0] && strcmp(o->marker, "-") && !e->mark[i]);
        if (alive && (!missing || (G.frame & 63u))) return;
    }
    e->target[i] = place ? find_actor(w, o->target) : ENTITY_NONE;
    e->mark[i] = !strcmp(o->marker, "-") ? ENTITY_NONE : o->marker[0] ? find_actor(w, o->marker) : e->target[i];
    e->resolved |= 1u << i;
}

static void markers_of(World* w, const QstLog* l, QstEntry* e, Entity s) {
    G.nmarkers = 0;
    if (!e || e->state != QST_ACTIVE || !e->q || !(log_flags(w, l) & QL_MARKERS)) return;
    const CQuestLog* c = comp_of(w, l);
    float maxd = c ? c->marker_distance : 0.0f;
    const CTransform* st = s ? ecs_get(w, s, COMP_TRANSFORM) : NULL;
    for (int i = 0; i < e->q->nobj && G.nmarkers < QST_MAX_MARKERS; i++) {
        if (!obj_active(w, l, e, i)) continue;
        resolve(w, e, i);
        Entity m = e->mark[i];
        const CTransform* mt = m && ecs_alive(w, m) ? ecs_get(w, m, COMP_TRANSFORM) : NULL;
        if (!mt || (mt->flags & TRANSFORM_HIDDEN)) continue;
        QuestMarker* k = &G.markers[G.nmarkers];
        const CCharacter* ch = ecs_get(w, m, COMP_CHARACTER);
        memcpy(k->pos, mt->location, 12);
        k->pos[2] += ch ? ch->half_height + 40.0f : 120.0f; // (over its head)
        float dx = st ? mt->location[0] - st->location[0] : 0.0f, dy = st ? mt->location[1] - st->location[1] : 0.0f;
        float dz = st ? mt->location[2] - st->location[2] : 0.0f;
        k->dist = sqrtf(dx * dx + dy * dy + dz * dz);
        k->optional = (e->q->obj[i].flags & QO_OPTIONAL) != 0;
        if (maxd > 0.0f && k->dist > maxd) continue;
        G.nmarkers++;
    }
}

void quest_tick(World* w, float dt) {
    if (!G.playing || !w) return;
    double t0 = re_time_seconds();
    G.frame++;
    uint32_t active = 0, completed = 0, failed = 0, objs = 0, logs = 0;
    for (int li = 0; li < QST_MAX_LOGS; li++) {
        QstLog* l = &G.logs[li];
        if (!l->used) continue;
        if (l->is_default) default_log(w, false); // (its actor bound)
        else if (l->owner && !ecs_alive(w, l->owner)) l->owner = ENTITY_NONE;
        logs++;
        Entity s = subject(w, l);
        const CTransform* st = s ? ecs_get(w, s, COMP_TRANSFORM) : NULL;
        float sp[3] = { 0, 0, 0 };
        if (st) memcpy(sp, st->location, 12);
        uint32_t rev = s ? inv_revision(w, s) : 0;
        bool inv = l->inv_check || rev != l->inv_rev || !(G.frame & 31u); // (and twice a second: a condition's tag may have come)
        l->inv_rev = rev, l->inv_check = false;
        for (uint32_t k = 0; k < l->n; k++) {
            QstEntry* e = &l->e[k];
            const Quest* q = e->q;
            if (e->state != QST_ACTIVE || !q) {
                completed += e->state == QST_COMPLETED, failed += e->state == QST_FAILED;
                continue;
            }
            e->time += dt;
            if (q->time_limit > 0.0f && e->time >= q->time_limit) {
                fail(w, l, e);
                failed++;
                continue;
            }
            for (int i = 0; i < q->nobj && e->state == QST_ACTIVE; i++) {
                if (!obj_active(w, l, e, i)) continue;
                objs++;
                uint8_t kind = q->obj[i].kind;
                if (kind == QOBJ_GO_TO && st) {
                    resolve(w, e, i);
                    if (e->target[i] && inside(w, e->target[i], sp, q->obj[i].radius)) set_progress(w, l, e, i, 1);
                } else if (kind == QOBJ_COLLECT && inv) {
                    const ItemDef* it = inv_item_by_name(q->obj[i].target);
                    set_progress(w, l, e, i, it && s ? inv_count(w, s, it->hash) : 0);
                }
            }
            active += e->state == QST_ACTIVE, completed += e->state == QST_COMPLETED, failed += e->state == QST_FAILED;
        }
    }
    flush();
    G.st.logs = logs, G.st.active = active, G.st.completed = completed, G.st.failed = failed, G.st.objectives = objs;
    // the HUD: the default log's tracked quest
    QstLog* d = default_log(w, false);
    QstEntry* te = d ? entry_of(d, d->tracked) : NULL;
    if (d) markers_of(w, d, te, subject(w, d));
    else G.nmarkers = 0;
    if (d && (log_flags(w, d) & QL_TRACKER)) show_tracker(w, d);
    G.st.tick_ms = (re_time_seconds() - t0) * 1000.0;
}

const QuestMarker* quest_markers(uint32_t* count) {
    if (count) *count = G.playing ? G.nmarkers : 0;
    return G.markers;
}
bool quest_markers_shown(void) { return G.playing && G.nmarkers > 0; }

// --- WBP_QuestTracker: the tracked quest's title, its stage, the objectives with their counts (top right) ---

static bool g_bad;
static int W(UiTree* t, uint8_t kind, int parent, const char* name) {
    int i = ui_tree_add(t, kind, parent);
    if (i < 0) return g_bad = true, 0;
    if (name) snprintf(t->w[i].name, UI_NAME, "%s", name);
    return i;
}
static void rgba(float* c, float r, float g, float b, float a) { c[0] = r, c[1] = g, c[2] = b, c[3] = a; }

uint32_t quest_tracker_asset(void) {
    BpAsset* a = bp_load(TRACKER_VPATH);
    if (a) return a->hash;
    const char* root = ui_content_root();
    if (!root[0]) return 0;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/UI", root);
    re_make_dirs(dir);
    a = bp_create(BPA_CLASS, "/Content/UI", "WBP_QuestTracker", "User Widget");
    if (!a) return 0;
    if (!a->ui) a->ui = calloc(1, sizeof(UiTree)); // (an edit, not a frame)
    if (!a->ui) return 0;
    UiTree* t = a->ui;
    ui_tree_default(t);
    g_bad = false;
    // a column of rows, each with its own background: a hidden row collapses, so the panel is as tall as what it shows
    const float PW = 470.0f, PH = 48.0f + 32.0f + TRACKER_ROWS * 34.0f + 12.0f;
    int panel = W(t, UIW_VBOX, 0, "Panel");
    UiWidget* p = &t->w[panel];
    p->anchor[0] = p->anchor[2] = 1.0f, p->anchor[1] = p->anchor[3] = 0.0f; // (the top right)
    p->align[0] = p->align[1] = 0.0f;
    p->pos[0] = -PW - 36.0f, p->pos[1] = 150.0f, p->size[0] = PW, p->size[1] = PH;
    p->padding = 0.0f, p->bg[3] = 0.0f;
    static const float bg[4] = { 0.02f, 0.025f, 0.035f, 0.62f };
    typedef struct Row { const char* row; const char* text; float h, font; } Row;
    Row rows[2 + TRACKER_ROWS + 1];
    char names[TRACKER_ROWS][2][24];
    int nr = 0;
    rows[nr++] = (Row){ "TitleRow", "Title", 48.0f, 27.0f };
    rows[nr++] = (Row){ "StageRow", "Stage", 32.0f, 19.0f };
    for (int i = 0; i < TRACKER_ROWS; i++) {
        snprintf(names[i][0], 24, "Objective%dRow", i + 1), snprintf(names[i][1], 24, "Objective%d", i + 1);
        rows[nr++] = (Row){ names[i][0], names[i][1], 34.0f, 21.0f };
    }
    rows[nr++] = (Row){ "BottomRow", NULL, 12.0f, 0.0f };
    for (int r = 0; r < nr; r++) {
        int b = W(t, UIW_BORDER, panel, rows[r].row);
        memcpy(t->w[b].bg, bg, sizeof bg), t->w[b].size[1] = rows[r].h, t->w[b].radius = 0.0f, t->w[b].padding = 0.0f, t->w[b].border_w = 0.0f;
        if (!rows[r].text) continue;
        int x = W(t, UIW_TEXT, b, rows[r].text);
        UiWidget* tx = &t->w[x];
        tx->font_size = rows[r].font, tx->align[0] = 0.0f, tx->align[1] = 0.5f, tx->slot_pad[0] = 18.0f, tx->slot_pad[2] = 12.0f;
        if (r == 0) snprintf(tx->text, sizeof tx->text, "Quest Title"), tx->bold = 1, rgba(tx->color, 1.0f, 0.80f, 0.38f, 1.0f), tx->slot_pad[1] = 6.0f;
        else if (r == 1) snprintf(tx->text, sizeof tx->text, "The stage"), rgba(tx->color, 0.72f, 0.74f, 0.78f, 1.0f);
        else snprintf(tx->text, sizeof tx->text, "\xE2\x80\xA2 Objective  0/%d", r - 1);
    }
    if (g_bad) printf("[RealEngine] Quest: WBP_QuestTracker was made with errors (%u widgets)\n", t->n);
    if (!bp_compile(a)) printf("[RealEngine] Quest: WBP_QuestTracker: %s\n", a->error);
    bp_save(a);
    return a->hash;
}

static void show_tracker(World* w, const QstLog* l) {
    const CQuestLog* c = comp_of(w, l);
    uint32_t cls_hash = c && c->widget ? c->widget : 0u;
    if (!G.widget || !ecs_alive(w, G.widget)) {
        BpAsset* cls = bp_find_hash(cls_hash ? cls_hash : quest_tracker_asset());
        G.widget = cls ? ui_create_widget(w, cls) : ENTITY_NONE;
        if (!G.widget) return;
        G.widget_cls = cls->hash;
        ui_add_to_viewport(G.widget, 5);
        G.shown_rev = UINT32_MAX;
    }
    int lang = quest_language();
    if (G.shown_rev == l->revision && G.shown_track == l->tracked && G.shown_lang == (uint32_t)lang && (G.frame & 31u)) return; // (nothing changed; a
                                                                                                                     // condition's tag: read twice a second)
    G.shown_rev = l->revision, G.shown_track = l->tracked, G.shown_lang = (uint32_t)lang;
    QstEntry* e = NULL;
    for (uint32_t k = 0; k < l->n && !e; k++)
        if (l->e[k].quest == l->tracked && l->e[k].state == QST_ACTIVE) e = (QstEntry*)&l->e[k];
    ui_set_visible(G.widget, "Panel", e != NULL);
    if (!e) return;
    const Quest* q = e->q;
    ui_set_text(G.widget, "Title", quest_title(q, lang));
    const char* stage = !q->parallel && e->stage < q->nstages ? pick(q->stages[e->stage].text[0], QST_TITLE, lang) : "";
    ui_set_text(G.widget, "Stage", stage);
    ui_set_visible(G.widget, "Stage", stage[0] != 0);
    ui_set_visible(G.widget, "StageRow", stage[0] != 0); // (WBP_QuestTracker's row; a widget of the user's may have none)
    static const char* const opt[QST_LANGS] = { "(Optional)", "(Opcional)", "(Opcional)" };
    int row = 0;
    for (int i = 0; i < q->nobj && row < TRACKER_ROWS; i++) {
        const QuestObjective* o = &q->obj[i];
        bool done = (e->done >> i & 1u) != 0;
        if (!done && !obj_active(w, l, e, i)) continue; // (a later stage, an objective waiting its turn, its condition unmet)
        if (done && !stage_active(e, q, o->stage) && !q->parallel) continue; // (the stages passed)
        char line[QST_TITLE + 48], name[24], rname[32];
        int n = obj_count(o);
        if (n > 1) snprintf(line, sizeof line, "\xE2\x80\xA2 %s  %d/%d%s%s", quest_obj_text(q, o, lang), e->progress[i], n, (o->flags & QO_OPTIONAL) ? " " : "",
                            (o->flags & QO_OPTIONAL) ? opt[lang % QST_LANGS] : "");
        else snprintf(line, sizeof line, "\xE2\x80\xA2 %s%s%s", quest_obj_text(q, o, lang), (o->flags & QO_OPTIONAL) ? " " : "", (o->flags & QO_OPTIONAL) ? opt[lang % QST_LANGS] : "");
        snprintf(name, sizeof name, "Objective%d", row + 1), snprintf(rname, sizeof rname, "Objective%dRow", row + 1);
        ui_set_text(G.widget, name, line);
        ui_set_visible(G.widget, name, true), ui_set_visible(G.widget, rname, true);
        float col[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        if (done) col[0] = 0.55f, col[1] = 0.85f, col[2] = 0.55f, col[3] = 0.85f;
        else if (o->flags & QO_OPTIONAL) col[0] = col[1] = col[2] = 0.78f;
        ui_set_color(G.widget, name, col);
        row++;
    }
    for (; row < TRACKER_ROWS; row++) {
        char name[24], rname[32];
        snprintf(name, sizeof name, "Objective%d", row + 1), snprintf(rname, sizeof rname, "Objective%dRow", row + 1);
        ui_set_visible(G.widget, name, false), ui_set_visible(G.widget, rname, false);
    }
}

// --- Saves: "#Quests" records, one log (or a part of a big one) each ---
// a record: magic, version (u8), flags (u8: 1 the default log, 2 the log's first record), entries (u16), tracked (u32),
// the owner's name [48], then each entry: quest (u32), state, stage, objectives (u8 each), pad, done, stage done (u32),
// time (f32), progress (i16 each)

static uint32_t put_record(uint8_t* buf, uint32_t len, uint32_t cap, const uint8_t* rec, uint32_t n) {
    uint32_t wd = (n + 15) / 16, nl = (uint32_t)strlen(SAVE_NAME), need = 1 + nl + 2 + wd * 16;
    if (wd > 255 || len + need > cap) return len;
    buf[len++] = (uint8_t)nl;
    memcpy(buf + len, SAVE_NAME, nl), len += nl;
    buf[len++] = 0, buf[len++] = (uint8_t)wd;
    memcpy(buf + len, rec, n), memset(buf + len + n, 0, wd * 16 - n), len += wd * 16;
    return len;
}

uint32_t quest_save_append(uint8_t* buf, uint32_t len, uint32_t cap) {
    uint8_t rec[255 * 16];
    for (int li = 0; li < QST_MAX_LOGS; li++) {
        const QstLog* l = &G.logs[li];
        if (!l->used || !l->n) continue;
        char key[48] = "";
        if (!l->is_default) {
            const CName* nm = G.w && l->owner && ecs_alive(G.w, l->owner) ? ecs_get(G.w, l->owner, COMP_NAME) : NULL;
            snprintf(key, sizeof key, "%s", nm ? nm->text : l->key);
        }
        uint32_t k = 0;
        bool first = true;
        while (k < l->n || first) {
            uint32_t n = 60, count = 0;
            uint32_t magic = SAVE_MAGIC;
            for (; k < l->n; k++) {
                const QstEntry* e = &l->e[k];
                uint8_t no = e->q ? e->q->nobj : 0;
                uint32_t sz = 24 + 2u * no;
                if (n + sz > sizeof rec) break;
                uint8_t* p = rec + n;
                memcpy(p, &e->quest, 4), p[4] = e->state, p[5] = e->stage, p[6] = no, p[7] = 0;
                memcpy(p + 8, &e->done, 4), memcpy(p + 12, &e->stage_done, 4), memcpy(p + 16, &e->time, 4);
                memset(p + 20, 0, 4);
                memcpy(p + 24, e->progress, 2u * no);
                n += sz, count++;
            }
            memcpy(rec, &magic, 4), rec[4] = 1, rec[5] = (uint8_t)((l->is_default ? 1u : 0u) | (first ? 2u : 0u));
            uint16_t c16 = (uint16_t)count;
            memcpy(rec + 6, &c16, 2), memcpy(rec + 8, &l->tracked, 4);
            memset(rec + 12, 0, 48), memcpy(rec + 12, key, strlen(key));
            uint32_t nl = put_record(buf, len, cap, rec, n);
            if (nl == len) return len; // (no room)
            len = nl, first = false;
            if (!count && k < l->n) break; // (an entry bigger than a record: never)
        }
    }
    return len;
}

bool quest_save_read(const uint8_t* d, uint32_t len) {
    if (len < 8 || memcmp(d, "RESAV001", 8)) return false;
    uint32_t at = 8, nl0 = (uint32_t)strlen(SAVE_NAME);
    bool any = false;
    QstLog* cur = NULL;
    while (at + 3 < len) { // (the records: the variables', the inventory's, the conversations', ours)
        uint32_t nl = d[at++];
        if (at + nl + 2 > len) break;
        bool mine = nl == nl0 && !memcmp(d + at, SAVE_NAME, nl);
        at += nl;
        uint8_t txt = d[at++], wd = d[at++];
        uint32_t size;
        if (txt) {
            if (at + 2 > len) break;
            size = 2u + (uint32_t)(d[at] | d[at + 1] << 8);
        } else size = wd * 16u;
        if (at + size > len) break;
        const uint8_t* r = d + at;
        at += size;
        if (!mine || txt || size < 60) continue;
        uint32_t magic;
        memcpy(&magic, r, 4);
        if (magic != SAVE_MAGIC || r[4] != 1) continue;
        if (!any) quest_clear(), any = true; // (the save's logs in place of these)
        bool is_default = r[5] & 1u, first = (r[5] & 2u) != 0;
        uint16_t count;
        uint32_t tracked;
        memcpy(&count, r + 6, 2), memcpy(&tracked, r + 8, 4);
        char key[49];
        memcpy(key, r + 12, 48), key[48] = 0;
        if (first || !cur) {
            cur = NULL;
            for (int i = 0; i < QST_MAX_LOGS && !cur; i++)
                if (G.logs[i].used && G.logs[i].is_default == is_default && (is_default || !_stricmp(G.logs[i].key, key))) cur = &G.logs[i];
            if (!cur) cur = new_log(is_default, ENTITY_NONE, key);
            if (!cur) continue;
            cur->tracked = tracked;
        }
        uint32_t p = 60;
        for (uint32_t k = 0; k < count && p + 24 <= size; k++) {
            const uint8_t* e = r + p;
            uint8_t no = e[6];
            if (p + 24 + 2u * no > size) break;
            p += 24 + 2u * no;
            uint32_t qh;
            memcpy(&qh, e, 4);
            Quest* q = quest_find(qh);
            if (!q || cur->n >= QST_MAX_ENTRIES || e[4] >= QST_STATE_COUNT) continue; // (its asset gone: dropped)
            QstEntry* x = &cur->e[cur->n++];
            memset(x, 0, sizeof *x);
            x->q = q, x->quest = qh, x->state = e[4], x->stage = e[5] < q->nstages ? e[5] : (q->nstages ? q->nstages - 1 : 0);
            memcpy(&x->done, e + 8, 4), memcpy(&x->stage_done, e + 12, 4), memcpy(&x->time, e + 16, 4);
            if (!isfinite(x->time) || x->time < 0.0f) x->time = 0.0f;
            memcpy(x->progress, e + 24, 2u * (no < QST_MAX_OBJ ? no : QST_MAX_OBJ));
            if (q->nobj < 32) x->done &= (1u << q->nobj) - 1u; // (an objective removed since)
        }
        cur->revision++, cur->inv_check = true;
    }
    if (any) G.shown_rev = UINT32_MAX;
    return any;
}

// --- The Debug Viewer: the tick's cost and each active quest ---
static void collect(DvList* l, const DvContext* ctx) {
    (void)ctx;
    l->gpu = false, l->estimated = true;
    l->ms = G.st.tick_ms;
    snprintf(l->note, sizeof l->note, "%u logs: %u active, %u completed, %u failed quests; %u objectives moving; %u events since play; last operation %.1f us",
             G.st.logs, G.st.active, G.st.completed, G.st.failed, G.st.objectives, G.st.events, G.st.op_us);
    dv_add(l, "Go To, Collect, time limits, tracker", "quest_tick", (float)G.st.tick_ms, ENTITY_NONE);
    float each = G.st.active ? (float)(G.st.tick_ms / (double)G.st.active) : 0.0f;
    for (int li = 0; li < QST_MAX_LOGS; li++) {
        const QstLog* g = &G.logs[li];
        for (uint32_t k = 0; g->used && k < g->n; k++) {
            const QstEntry* e = &g->e[k];
            if (e->state != QST_ACTIVE || !e->q) continue;
            char info[48];
            int done = 0;
            for (int i = 0; i < e->q->nobj; i++) done += (e->done >> i & 1u) != 0;
            snprintf(info, sizeof info, "stage %d/%d, %d/%d done%s", e->stage + 1, e->q->nstages, done, e->q->nobj, g->tracked == e->quest ? ", tracked" : "");
            dv_add(l, e->q->name, info, each, g->owner);
        }
    }
}
void quest_register_debug(void) { dv_register("Quests", collect); }

// --- The self-test: the rules on made-up quests in a world of its own ---

static struct { uint32_t n[QST_EV_COUNT]; char last_obj[QST_NAME]; char last_quest[QST_NAME]; int calls; } g_rec;
static Entity g_tplayer;
static void t_event(World* w, int ev, Entity owner, const char* quest, const char* obj, int index, int progress, int count) {
    (void)w, (void)owner, (void)index, (void)progress, (void)count;
    if (ev >= 0 && ev < QST_EV_COUNT) g_rec.n[ev]++;
    snprintf(g_rec.last_quest, QST_NAME, "%s", quest), snprintf(g_rec.last_obj, QST_NAME, "%s", obj);
}
static bool t_call(World* w, Entity e, const char* event, const char* const* args, uint32_t nargs) {
    (void)w, (void)e, (void)args, (void)nargs;
    if (!strcmp(event, "OnReward")) g_rec.calls++;
    return true;
}
static Entity t_player(World* w) { (void)w; return g_tplayer; }
static Entity t_actor(World* w, const char* name, float x, float y, const char* tag) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
    if (!e) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    if (tag) gt_add(w, e, tag);
    return e;
}
static Quest* t_quest(const char* name) {
    Quest* q = calloc(1, sizeof *q);
    if (!q) return NULL;
    snprintf(q->name, QST_NAME, "%s", name);
    snprintf(q->path, sizeof q->path, "(selftest)");
    q->hash = quest_vpath_hash(name);
    return q;
}
static int t_obj(Quest* q, int stage, uint8_t kind, const char* id, const char* target, int count) {
    while (q->nstages <= stage) quest_add_stage(q);
    int o = quest_add_objective(q, stage, kind);
    if (o < 0) return o;
    snprintf(q->obj[o].id, QST_NAME, "%s", id), snprintf(q->obj[o].target, QST_NAME, "%s", target ? target : "");
    q->obj[o].count = count;
    return o;
}

int quest_selftest(void) {
    int fails = 0;
#define T(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: quest selftest: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
    static World w;
    if (!ecs_init(&w)) return 1;
    QuestHooks keep_hooks = G.hooks;
    uint32_t keep_n = S.n;
    bool keep_scanned = S.scanned;
    S.scanned = true; // (only the made-up quests)
    quest_clear();
    memset(&g_rec, 0, sizeof g_rec);
    quest_set_hooks(&(QuestHooks){ t_event, t_call, t_player });
    quest_begin_play(&w);
    g_tplayer = t_actor(&w, "Hero", 0, 0, NULL);
    Entity well = t_actor(&w, "OldWell", 1000, 0, NULL);
    Entity chest = t_actor(&w, "Chest", 0, 500, "Loot.Chest");
    Entity wolf1 = t_actor(&w, "Wolf1", 300, 300, "Enemy.Wolf.Grey"), wolf2 = t_actor(&w, "Wolf2", 300, 300, "Enemy.Wolf");
    Entity bear = t_actor(&w, "Bear", 300, 300, "Enemy.Bear"), elder = t_actor(&w, "Elder", -400, 0, NULL);
    (void)well;
    // quest A: stage 0 in order (Go To the well, then the chest by its tag), stage 1 (kill 2 wolves; an optional manual),
    // stage 2 (talk to the elder, only with the tag Rep.Friend); rewards: a tag, the event, the next quest B
    Quest* a = t_quest("Q_TestA");
    Quest* b = t_quest("Q_TestB");
    Quest* c = t_quest("Q_TestTimed");
    Quest* p = t_quest("Q_TestParallel");
    if (!a || !b || !c || !p) return free(a), free(b), free(c), free(p), ecs_shutdown(&w), fails + 1;
    t_obj(a, 0, QOBJ_GO_TO, "well", "OldWell", 1);
    a->stages[0].in_order = 1;
    a->obj[0].radius = 250.0f;
    t_obj(a, 0, QOBJ_INTERACT, "chest", "Loot.Chest", 1);
    t_obj(a, 1, QOBJ_KILL, "wolves", "Enemy.Wolf", 2);
    int opt = t_obj(a, 1, QOBJ_MANUAL, "bonus", NULL, 3);
    a->obj[opt].flags |= QO_OPTIONAL;
    int talk = t_obj(a, 2, QOBJ_TALK, "elder", "Elder", 1);
    snprintf(a->obj[talk].condition, QST_NAME, "Rep.Friend");
    snprintf(a->reward_tag[0], QST_NAME, "Quest.TestA.Done"), snprintf(a->reward_event, QST_NAME, "OnReward"), snprintf(a->next_quest, QST_NAME, "Q_TestB");
    t_obj(b, 0, QOBJ_MANUAL, "step", NULL, 1);
    t_obj(c, 0, QOBJ_MANUAL, "never", NULL, 1);
    c->time_limit = 1.0f;
    p->parallel = 1;
    t_obj(p, 0, QOBJ_MANUAL, "left", NULL, 1), t_obj(p, 1, QOBJ_MANUAL, "right", NULL, 2);
    S.list[S.n++] = a, S.list[S.n++] = b, S.list[S.n++] = c, S.list[S.n++] = p;
    T(quest_obj_index(a, "wolves") == 2 && quest_obj_index(a, "1") == 1 && quest_obj_index(a, "nope") < 0, "objectives by id and index");
    T(quest_start(&w, ENTITY_NONE, "Q_TestA") && g_rec.n[QST_EV_STARTED] == 1, "Start Quest and its event");
    T(!quest_start(&w, ENTITY_NONE, "Q_TestA"), "started twice");
    T(quest_state(&w, ENTITY_NONE, "Q_TestA", NULL) == QST_ACTIVE && quest_tracked(&w, ENTITY_NONE) == a, "active and tracked (auto)");
    // in order: the chest does not count before the well
    quest_notify_interact(&w, chest, g_tplayer);
    int pr = -1;
    quest_objective(&w, ENTITY_NONE, "Q_TestA", "chest", &pr, NULL, NULL, NULL);
    T(pr == 0, "in order: the chest counted before the well (%d)", pr);
    quest_tick(&w, 0.016f);
    CTransform* ht = ecs_get(&w, g_tplayer, COMP_TRANSFORM);
    ht->location[0] = 900.0f; // within 250 cm of the well
    quest_tick(&w, 0.016f);
    bool dn = false;
    quest_objective(&w, ENTITY_NONE, "Q_TestA", "well", NULL, NULL, &dn, NULL);
    T(dn && g_rec.n[QST_EV_OBJ_COMPLETED] == 1, "Go To within the radius");
    quest_notify_interact(&w, wolf1, g_tplayer); // (not the chest)
    quest_notify_interact(&w, chest, g_tplayer);
    int st = -1;
    T(quest_state(&w, ENTITY_NONE, "Q_TestA", &st) == QST_ACTIVE && st == 1, "the chest by its tag: stage 2 (stage %d)", st);
    // kills: by the tag hierarchy (Enemy.Wolf covers Enemy.Wolf.Grey); a bear doesn't count; the player's own death neither
    quest_notify_death(&w, bear, g_tplayer);
    quest_notify_death(&w, g_tplayer, wolf1);
    quest_objective(&w, ENTITY_NONE, "Q_TestA", "wolves", &pr, NULL, NULL, NULL);
    T(pr == 0, "a bear / the player counted as a wolf (%d)", pr);
    quest_notify_death(&w, wolf1, g_tplayer);
    quest_objective(&w, ENTITY_NONE, "Q_TestA", "wolves", &pr, NULL, NULL, NULL);
    T(pr == 1 && g_rec.n[QST_EV_OBJ_UPDATED] >= 3, "a wolf killed: %d (1)", pr);
    quest_advance(&w, ENTITY_NONE, "Q_TestA", "bonus", 2, false);
    quest_objective(&w, ENTITY_NONE, "Q_TestA", "bonus", &pr, NULL, NULL, NULL);
    T(pr == 2, "Advance Objective by 2: %d", pr);
    quest_notify_death(&w, wolf2, ENTITY_NONE);
    T(quest_state(&w, ENTITY_NONE, "Q_TestA", &st) == QST_ACTIVE && st == 2, "the wolves: the stage passes without the optional one (stage %d)", st);
    T(!quest_advance(&w, ENTITY_NONE, "Q_TestA", "bonus", 1, false), "an optional objective of a passed stage moved");
    // the condition: the elder only with Rep.Friend
    quest_notify_talk(&w, elder, "DLG_Elder");
    T(quest_state(&w, ENTITY_NONE, "Q_TestA", NULL) == QST_ACTIVE, "talked without the condition's tag");
    gt_add(&w, g_tplayer, "Rep.Friend.Close");
    // save: the log in a record, cleared, read back
    uint8_t buf[8192];
    memcpy(buf, "RESAV001", 8);
    uint32_t n = quest_save_append(buf, 8, sizeof buf);
    quest_clear();
    T(quest_state(&w, ENTITY_NONE, "Q_TestA", NULL) == QST_NOT_STARTED, "cleared");
    T(n > 8 && quest_save_read(buf, n), "the save record read");
    quest_objective(&w, ENTITY_NONE, "Q_TestA", "bonus", &pr, NULL, NULL, NULL);
    T(quest_state(&w, ENTITY_NONE, "Q_TestA", &st) == QST_ACTIVE && st == 2 && pr == 2 && quest_tracked(&w, ENTITY_NONE) == a,
      "the save: state, stage %d, progress %d, tracked", st, pr);
    quest_notify_talk(&w, ENTITY_NONE, "Elder"); // (by the Dialogue's name: the target)
    T(quest_state(&w, ENTITY_NONE, "Q_TestA", NULL) == QST_COMPLETED, "the talk with the tag completes A");
    T(g_rec.n[QST_EV_COMPLETED] == 1 && g_rec.calls >= 1 && gt_has(&w, g_tplayer, "Quest.TestA.Done", true), "the rewards: event %u, call %d, tag",
      g_rec.n[QST_EV_COMPLETED], g_rec.calls);
    T(quest_state(&w, ENTITY_NONE, "Q_TestB", NULL) == QST_ACTIVE && quest_tracked(&w, ENTITY_NONE) == b, "the next quest B started and tracked");
    T(!quest_start(&w, ENTITY_NONE, "Q_TestA"), "a completed quest started again (not repeatable)");
    // fail by the time limit, then again (a failed quest can start over)
    quest_start(&w, ENTITY_NONE, "Q_TestTimed");
    for (int i = 0; i < 70; i++) quest_tick(&w, 0.016f);
    T(quest_state(&w, ENTITY_NONE, "Q_TestTimed", NULL) == QST_FAILED && g_rec.n[QST_EV_FAILED] == 1, "the time limit fails it");
    T(quest_start(&w, ENTITY_NONE, "Q_TestTimed") && quest_fail(&w, ENTITY_NONE, "Q_TestTimed"), "a failed quest restarted, failed by the node");
    // parallel: both stages at once
    quest_start(&w, ENTITY_NONE, "Q_TestParallel");
    quest_advance(&w, ENTITY_NONE, "Q_TestParallel", "right", 2, true);
    T(quest_state(&w, ENTITY_NONE, "Q_TestParallel", NULL) == QST_ACTIVE, "parallel: one stage done");
    T(quest_advance(&w, ENTITY_NONE, "Q_TestParallel", "left", 1, false) && quest_state(&w, ENTITY_NONE, "Q_TestParallel", NULL) == QST_COMPLETED,
      "parallel: both done");
    T(quest_track(&w, ENTITY_NONE, "Q_TestB") && quest_tracked(&w, ENTITY_NONE) == b, "Track Quest");
    // 200 quests (each: a kill, a Go To, a manual) started, ticked, a death notified, timed
    quest_clear();
    Quest* many[200];
    uint32_t nmany = 0;
    for (int i = 0; i < 200 && S.n < QST_MAX_ASSETS; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "Q_Many%03d", i);
        Quest* q = t_quest(nm);
        if (!q) break;
        t_obj(q, 0, QOBJ_KILL, "k", "Enemy.Bear", 2), t_obj(q, 0, QOBJ_GO_TO, "g", "Elder", 1), t_obj(q, 0, QOBJ_MANUAL, "m", NULL, 1);
        q->obj[1].radius = 50.0f;
        many[nmany++] = q, S.list[S.n++] = q;
    }
    double t0 = re_time_seconds();
    for (uint32_t i = 0; i < nmany; i++) quest_start(&w, ENTITY_NONE, many[i]->name);
    double start_ms = (re_time_seconds() - t0) * 1000.0;
    double tick = 0.0;
    for (int f = 0; f < 60; f++) {
        quest_tick(&w, 0.016f);
        tick += G.st.tick_ms;
    }
    tick /= 60.0;
    t0 = re_time_seconds();
    quest_notify_death(&w, bear, g_tplayer);
    double death_ms = (re_time_seconds() - t0) * 1000.0;
    int prog = 0;
    quest_objective(&w, ENTITY_NONE, "Q_Many199", "k", &prog, NULL, NULL, NULL);
    T(nmany == 200 && G.st.active == 200 && prog == 1, "200 quests: %u active, the bear counted %d", G.st.active, prog);
    memcpy(buf, "RESAV001", 8);
    static uint8_t big[64 * 1024];
    memcpy(big, "RESAV001", 8);
    t0 = re_time_seconds();
    uint32_t bn = quest_save_append(big, 8, sizeof big);
    double save_us = (re_time_seconds() - t0) * 1e6;
    quest_clear();
    T(quest_save_read(big, bn) && quest_state(&w, ENTITY_NONE, "Q_Many150", NULL) == QST_ACTIVE, "200 quests saved in %u bytes and read back", bn);
    printf("[RealEngine] Quest selftest: 200 quests (600 objectives): started in %.3f ms, quest_tick %.4f ms/frame, a death notified in %.4f ms, saved in "
           "%.1f us (%u bytes)\n", start_ms, tick, death_ms, save_us, bn);
    T(tick < 0.2, "quest_tick with 200 quests: %.4f ms (0.2 at most)", tick);
    T(death_ms < 0.5, "a death with 200 kill objectives: %.4f ms (0.5 at most)", death_ms);
    // back as it was
    quest_clear();
    quest_end_play(&w);
    for (uint32_t i = keep_n; i < S.n; i++) free(S.list[i]);
    S.n = keep_n, S.scanned = keep_scanned;
    G.hooks = keep_hooks;
    g_tplayer = ENTITY_NONE;
    ecs_shutdown(&w);
#undef T
    return fails;
}
