// Dialogue (dialogue.h, notas-dev/dialogo.md): the Dialogue assets (.rdlg, JSON), the rules that walk them (one core for
// the game and the editor's preview), the game's conversation (its widget, camera, voice, pause, events) and the
// memory of what was said, saved with Save Game to Slot.
#include "dialogue.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "anim.h"
#include "audio.h"
#include "blueprint.h"
#include "bp_internal.h"
#include "components.h"
#include "game_settings.h"
#include "gameplay_tags.h"
#include "health.h"
#include "i18n.h"
#include "inventory.h"
#include "io_json.h"
#include "platform.h"
#include "platform_files.h"
#include "ui_widget.h"

#define DEG2RAD 0.017453293f
#define MAX_ENTERS 64        // nodes passed in one step at most (a loop of Jumps with nothing to show ends)
#define MEMORY_MAX 192       // Dialogues remembered at once
#define WIDGET_VPATH "/Content/UI/WBP_Dialogue.rbp"
#define WIDGET_OPTIONS 6     // WBP_Dialogue's Option buttons
#define SAVE_NAME "#Dialogue"
#define SAVE_MAGIC 0x31474C44u // "DLG1"

const char* const g_dlg_kind_names[DLG_KIND_COUNT] = { "Start", "Line", "Choice", "Option", "Jump", "End" };
const char* const g_dlg_cond_names[DLC_KIND_COUNT] = { "None", "Has Gameplay Tag", "Has Item", "Variable", "Health", "Said Before" };
const char* const g_dlg_act_names[DLA_KIND_COUNT] = { "None", "Give Item", "Take Item", "Add Gameplay Tag", "Remove Gameplay Tag",
                                                      "Call Custom Event", "Set Variable", "Open Shop" };
const char* const g_dlg_op_names[DLG_OP_COUNT] = { ">=", ">", "==", "!=", "<", "<=" };
const char* const g_dlg_who_names[DLG_WHO_COUNT] = { "Player", "Speaker" };

static struct {
    char root[600];
    bool scanned;
    Dialogue* list[DLG_MAX_ASSETS];
    uint32_t n;
    int lang;                  // Set Dialogue Language's (-1: the game's)
    DlgHooks hooks;
    DlgStats stats;
    struct { uint32_t hash, bits[DLG_MAX_NODES / 32]; } mem[MEMORY_MAX];
    uint32_t nmem;
} S = { .lang = -1 };

// The game's conversation
static struct {
    bool active;
    Dialogue* d;
    uint32_t version;
    DlgPreview st;             // the walk's state (node, choosing, options, letters); its made-up player unused
    World* w;
    Entity speaker, player, widget;
    Entity kept;               // the widget of the last conversation (shown again by the next one of its class)
    uint32_t kept_cls;
    char display[DLG_NAME];
    bool paused_by_us, mouse_before, camera;
    float cam_distance, cam_height, cam_angle;
    float cam0[3], yaw0, pitch0, cam_t; // the gameplay camera when it started; the blend's time
    bool have_cam0;
    AudioHandle voice;
    char shown_text[DLG_TEXT];
    uint32_t shown_bytes, last_bytes;
    int last_node, last_nopts, last_lang;
    bool last_choosing;
    float idle;                // seconds since all the line showed (a Line without wait)
} R;

// --- Small helpers ---

uint32_t dlg_vpath_hash(const char* vpath) {
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) h = (h ^ (uint8_t)tolower((unsigned char)*c)) * 16777619u;
    return h ? h : 1u;
}
bool dlg_is_path(const char* vpath) {
    const char* dot = vpath ? strrchr(vpath, '.') : NULL;
    return dot && !_stricmp(dot, ".rdlg");
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
    snprintf(out, DLG_NAME, "%s", s ? s + 1 : path);
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
// UTF-8: the bytes of the first n code points of s
static uint32_t utf8_prefix(const char* s, uint32_t n) {
    uint32_t b = 0;
    while (s[b] && n) {
        b++;
        while (((uint8_t)s[b] & 0xC0u) == 0x80u) b++;
        n--;
    }
    return b;
}
static uint32_t utf8_len(const char* s) {
    uint32_t n = 0;
    for (; *s; s++) n += ((uint8_t)*s & 0xC0u) != 0x80u;
    return n;
}

// --- Assets ---

void dlg_init(const char* content_root) {
    snprintf(S.root, sizeof S.root, "%s", content_root ? content_root : "");
    for (char* c = S.root; *c; c++)
        if (*c == '\\') *c = '/';
    for (uint32_t i = 0; i < S.n; i++) free(S.list[i]); // (another project: its own)
    S.n = 0, S.scanned = false;
    if (R.active) R.active = false, R.d = NULL;
}

static void write_text_arr(JsonWriter* w, const char* key, const char (*t)[DLG_TEXT]) {
    jw_key(w, key);
    jw_arr_begin(w);
    for (int l = 0; l < DLG_LANGS; l++) jw_str(w, t[l]);
    jw_arr_end(w);
}

bool dlg_save(Dialogue* d) {
    if (!d) return false;
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1);
    jw_kfloat(&w, "chars_per_second", d->chars_per_second);
    jw_kbool(&w, "pause", d->pause != 0);
    jw_kstr(&w, "player_name", d->player_name);
    jw_key(&w, "nodes");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < d->nnodes; i++) {
        const DlgNode* n = &d->nodes[i];
        jw_obj_begin(&w);
        jw_kstr(&w, "kind", g_dlg_kind_names[n->kind % DLG_KIND_COUNT]);
        jw_kfloats(&w, "pos", (float[]){ n->x, n->y }, 2);
        if (n->speaker[0]) jw_kstr(&w, "speaker", n->speaker);
        write_text_arr(&w, "text", (const char(*)[DLG_TEXT])n->text);
        if (n->voice[0]) jw_kstr(&w, "voice", n->voice);
        if (n->anim[0]) jw_kstr(&w, "anim", n->anim);
        jw_kfloat(&w, "duration", n->duration);
        jw_kbool(&w, "wait", n->wait != 0);
        if (n->once) jw_kbool(&w, "once", true);
        if (n->kind == DLG_JUMP) jw_kint(&w, "jump", n->jump);
        jw_key(&w, "links");
        jw_arr_begin(&w);
        for (uint32_t k = 0; k < n->nlinks; k++) jw_int(&w, n->links[k]);
        jw_arr_end(&w);
        jw_key(&w, "conditions");
        jw_arr_begin(&w);
        for (uint32_t k = 0; k < n->nconds; k++) {
            const DlgCond* c = &n->conds[k];
            jw_obj_begin(&w);
            jw_kstr(&w, "kind", g_dlg_cond_names[c->kind % DLC_KIND_COUNT]);
            jw_kint(&w, "op", c->op);
            jw_kint(&w, "who", c->who);
            jw_kbool(&w, "not", c->invert);
            jw_kstr(&w, "key", c->key);
            jw_kfloat(&w, "value", c->value);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_key(&w, "actions");
        jw_arr_begin(&w);
        for (uint32_t k = 0; k < n->nacts; k++) {
            const DlgAct* a = &n->acts[k];
            jw_obj_begin(&w);
            jw_kstr(&w, "kind", g_dlg_act_names[a->kind % DLA_KIND_COUNT]);
            jw_kint(&w, "who", a->who);
            jw_kstr(&w, "key", a->key);
            jw_key(&w, "args");
            jw_arr_begin(&w);
            jw_str(&w, a->arg[0]), jw_str(&w, a->arg[1]);
            jw_arr_end(&w);
            jw_kfloat(&w, "value", a->value);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        if (n->comment[0]) jw_kstr(&w, "comment", n->comment);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    bool ok = !w.fail;
    if (ok) {
        char tmp[700];
        snprintf(tmp, sizeof tmp, "%s.tmp", d->path);
        FILE* f = re_fopen(tmp, "wb");
        ok = f && fwrite(w.buf, 1, w.len, f) == w.len;
        if (f) ok = fclose(f) == 0 && ok;
        ok = ok && re_replace_file(tmp, d->path);
    }
    jw_free(&w);
    return ok;
}

static int kind_named(const char* s, const char* const* names, int count, int def) {
    for (int k = 0; k < count; k++)
        if (!_stricmp(names[k], s)) return k;
    return def;
}

static void read_dialogue(Dialogue* d, const JsonValue* r) {
    d->chars_per_second = (float)json_get_num(r, "chars_per_second", 40.0);
    d->pause = json_get_bool(r, "pause", false);
    snprintf(d->player_name, DLG_NAME, "%s", json_get_str(r, "player_name", ""));
    const JsonValue* ns = json_get(r, "nodes");
    d->nnodes = 0;
    for (size_t i = 0; i < json_len(ns) && d->nnodes < DLG_MAX_NODES; i++) {
        const JsonValue* o = json_at(ns, i);
        DlgNode* n = &d->nodes[d->nnodes++];
        memset(n, 0, sizeof *n);
        n->kind = (uint8_t)kind_named(json_get_str(o, "kind", "Line"), g_dlg_kind_names, DLG_KIND_COUNT, DLG_LINE);
        float p[2] = { 0, 0 };
        json_get_floats(o, "pos", p, 2);
        n->x = p[0], n->y = p[1];
        snprintf(n->speaker, DLG_NAME, "%s", json_get_str(o, "speaker", ""));
        const JsonValue* tx = json_get(o, "text");
        for (int l = 0; l < DLG_LANGS; l++) snprintf(n->text[l], DLG_TEXT, "%s", json_str(json_at(tx, (size_t)l), ""));
        snprintf(n->voice, DLG_PATH, "%s", json_get_str(o, "voice", ""));
        snprintf(n->anim, DLG_NAME, "%s", json_get_str(o, "anim", ""));
        n->duration = (float)json_get_num(o, "duration", 0.0);
        n->wait = json_get_bool(o, "wait", true);
        n->once = json_get_bool(o, "once", false);
        n->jump = (int16_t)json_get_int(o, "jump", -1);
        const JsonValue* ls = json_get(o, "links");
        for (size_t k = 0; k < json_len(ls) && n->nlinks < DLG_MAX_LINKS; k++) n->links[n->nlinks++] = (int16_t)json_int(json_at(ls, k), -1);
        const JsonValue* cs = json_get(o, "conditions");
        for (size_t k = 0; k < json_len(cs) && n->nconds < DLG_MAX_CONDS; k++) {
            const JsonValue* q = json_at(cs, k);
            DlgCond* c = &n->conds[n->nconds++];
            c->kind = (uint8_t)kind_named(json_get_str(q, "kind", "None"), g_dlg_cond_names, DLC_KIND_COUNT, DLC_NONE);
            c->op = (uint8_t)(json_get_int(q, "op", 0) % DLG_OP_COUNT);
            c->who = (uint8_t)(json_get_int(q, "who", 0) % DLG_WHO_COUNT);
            c->invert = json_get_bool(q, "not", false);
            snprintf(c->key, DLG_NAME, "%s", json_get_str(q, "key", ""));
            c->value = (float)json_get_num(q, "value", 0.0);
        }
        const JsonValue* as = json_get(o, "actions");
        for (size_t k = 0; k < json_len(as) && n->nacts < DLG_MAX_ACTS; k++) {
            const JsonValue* q = json_at(as, k);
            DlgAct* a = &n->acts[n->nacts++];
            a->kind = (uint8_t)kind_named(json_get_str(q, "kind", "None"), g_dlg_act_names, DLA_KIND_COUNT, DLA_NONE);
            a->who = (uint8_t)(json_get_int(q, "who", 0) % DLG_WHO_COUNT);
            snprintf(a->key, DLG_NAME, "%s", json_get_str(q, "key", ""));
            const JsonValue* ar = json_get(q, "args");
            for (int j = 0; j < 2; j++) snprintf(a->arg[j], DLG_NAME, "%s", json_str(json_at(ar, (size_t)j), ""));
            a->value = (float)json_get_num(q, "value", 1.0);
        }
        snprintf(n->comment, sizeof n->comment, "%s", json_get_str(o, "comment", ""));
    }
    // the links and jumps that point nowhere dropped; a Start first
    for (uint32_t i = 0; i < d->nnodes; i++) {
        DlgNode* n = &d->nodes[i];
        uint32_t k = 0;
        for (uint32_t j = 0; j < n->nlinks; j++)
            if (n->links[j] > 0 && n->links[j] < (int)d->nnodes && n->links[j] != (int)i) n->links[k++] = n->links[j];
        n->nlinks = (uint8_t)k;
        if (n->jump >= (int)d->nnodes) n->jump = -1;
    }
    if (!d->nnodes || d->nodes[0].kind != DLG_START) {
        if (d->nnodes >= DLG_MAX_NODES) d->nnodes = DLG_MAX_NODES - 1;
        memmove(&d->nodes[1], &d->nodes[0], sizeof(DlgNode) * d->nnodes);
        memset(&d->nodes[0], 0, sizeof(DlgNode));
        d->nodes[0].kind = DLG_START, d->nodes[0].jump = -1;
        d->nnodes++;
        for (uint32_t i = 1; i < d->nnodes; i++) { // (the indices moved by one)
            for (uint32_t j = 0; j < d->nodes[i].nlinks; j++) d->nodes[i].links[j]++;
            if (d->nodes[i].jump >= 0) d->nodes[i].jump++;
        }
    }
}

Dialogue* dlg_load(const char* vpath) {
    if (!vpath || !vpath[0]) return NULL;
    uint32_t h = dlg_vpath_hash(vpath);
    for (uint32_t i = 0; i < S.n; i++)
        if (S.list[i]->hash == h) return S.list[i];
    if (S.n >= DLG_MAX_ASSETS) return NULL;
    char path[700];
    disk_of(vpath, path, sizeof path);
    size_t len = 0;
    char* text = read_text(path, &len);
    if (!text) return NULL;
    JsonDoc doc;
    char err[160];
    Dialogue* d = NULL;
    if (json_parse(text, len, &doc, err, sizeof err)) {
        d = calloc(1, sizeof *d); // (an asset, not a frame)
        if (d) {
            snprintf(d->path, sizeof d->path, "%s", path);
            name_of(path, d->name);
            d->hash = h;
            read_dialogue(d, &doc.root);
            S.list[S.n++] = d;
        }
        json_free(&doc);
    } else
        printf("[RealEngine] Dialogue: %s: %s\n", vpath, err);
    free(text);
    return d;
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
    if (!dlg_is_path(e->name)) return;
    vpath_of(path, vpath, sizeof vpath);
    dlg_load(vpath);
}
static void scan_dir(const char* disk, int depth) {
    if (depth <= 12) re_dir_walk(disk, scan_entry, (void*)disk);
}
static void scan(void) {
    if (!S.scanned && S.root[0]) S.scanned = true, scan_dir(S.root, 0);
}

Dialogue* dlg_find(uint32_t hash) {
    if (!hash) return NULL;
    for (uint32_t i = 0; i < S.n; i++)
        if (S.list[i]->hash == hash) return S.list[i];
    scan();
    for (uint32_t i = 0; i < S.n; i++)
        if (S.list[i]->hash == hash) return S.list[i];
    return NULL;
}
Dialogue* dlg_find_name(const char* name) {
    if (!name || !name[0]) return NULL;
    scan();
    for (uint32_t i = 0; i < S.n; i++)
        if (!_stricmp(S.list[i]->name, name)) return S.list[i];
    return NULL;
}
uint32_t dlg_count(void) {
    scan();
    return S.n;
}
Dialogue* dlg_at(uint32_t i) { return i < S.n ? S.list[i] : NULL; }

int dlg_add_node(Dialogue* d, uint8_t kind, float x, float y) {
    if (!d || d->nnodes >= DLG_MAX_NODES || kind >= DLG_KIND_COUNT) return -1;
    DlgNode* n = &d->nodes[d->nnodes];
    memset(n, 0, sizeof *n);
    n->kind = kind, n->x = x, n->y = y, n->jump = -1, n->wait = 1;
    d->version++;
    return (int)d->nnodes++;
}
void dlg_remove_node(Dialogue* d, int idx) {
    if (!d || idx <= 0 || idx >= (int)d->nnodes) return;
    memmove(&d->nodes[idx], &d->nodes[idx + 1], sizeof(DlgNode) * (d->nnodes - (uint32_t)idx - 1));
    d->nnodes--;
    for (uint32_t i = 0; i < d->nnodes; i++) {
        DlgNode* n = &d->nodes[i];
        uint32_t k = 0;
        for (uint32_t j = 0; j < n->nlinks; j++) {
            int t = n->links[j];
            if (t == idx) continue;
            n->links[k++] = (int16_t)(t > idx ? t - 1 : t);
        }
        n->nlinks = (uint8_t)k;
        if (n->jump == idx) n->jump = -1;
        else if (n->jump > idx) n->jump--;
        for (uint32_t c = 0; c < n->nconds; c++)
            if (n->conds[c].kind == DLC_SAID) {
                if ((int)n->conds[c].value == idx) n->conds[c].value = -1.0f;
                else if ((int)n->conds[c].value > idx) n->conds[c].value -= 1.0f;
            }
    }
    d->version++;
}
bool dlg_link(Dialogue* d, int from, int to) {
    if (!d || from < 0 || to <= 0 || from >= (int)d->nnodes || to >= (int)d->nnodes || from == to) return false;
    DlgNode* n = &d->nodes[from];
    if (n->kind == DLG_END || n->kind == DLG_JUMP) return false; // (an End ends; a Jump goes by its target)
    for (uint32_t j = 0; j < n->nlinks; j++)
        if (n->links[j] == to) return false;
    if (n->nlinks >= DLG_MAX_LINKS) return false;
    n->links[n->nlinks++] = (int16_t)to;
    d->version++;
    return true;
}
void dlg_unlink(Dialogue* d, int from, int to) {
    if (!d || from < 0 || from >= (int)d->nnodes) return;
    DlgNode* n = &d->nodes[from];
    uint32_t k = 0;
    for (uint32_t j = 0; j < n->nlinks; j++)
        if (n->links[j] != to) n->links[k++] = n->links[j];
    n->nlinks = (uint8_t)k;
    d->version++;
}

static void set_text(DlgNode* n, const char* en, const char* pt, const char* es) {
    snprintf(n->text[0], DLG_TEXT, "%s", en);
    snprintf(n->text[1], DLG_TEXT, "%s", pt ? pt : "");
    snprintf(n->text[2], DLG_TEXT, "%s", es ? es : "");
}

// The example: a merchant greets, asks; buying needs a coin (hidden without one), gives a potion and tags the player;
// "who are you" jumps back to the question; goodbye ends
static void make_example(Dialogue* d) {
    int hi = dlg_add_node(d, DLG_LINE, 300, 0);
    set_text(&d->nodes[hi], "Welcome, traveler! What brings you here?", "Bem-vindo, viajante! O que o traz aqui?", "¡Bienvenido, viajero! ¿Qué te trae por aquí?");
    int ch = dlg_add_node(d, DLG_CHOICE, 650, 0);
    int buy = dlg_add_node(d, DLG_OPTION, 1000, -180);
    set_text(&d->nodes[buy], "I want to buy a potion (1 coin).", "Quero comprar uma poção (1 moeda).", "Quiero comprar una poción (1 moneda).");
    DlgCond* c = &d->nodes[buy].conds[d->nodes[buy].nconds++];
    c->kind = DLC_HAS_ITEM, c->who = DLG_WHO_PLAYER, c->value = 1.0f;
    snprintf(c->key, DLG_NAME, "DA_Coin");
    DlgAct* a = &d->nodes[buy].acts[d->nodes[buy].nacts++];
    a->kind = DLA_TAKE_ITEM, a->who = DLG_WHO_PLAYER, a->value = 1.0f;
    snprintf(a->key, DLG_NAME, "DA_Coin");
    a = &d->nodes[buy].acts[d->nodes[buy].nacts++];
    a->kind = DLA_GIVE_ITEM, a->who = DLG_WHO_PLAYER, a->value = 1.0f;
    snprintf(a->key, DLG_NAME, "DA_Potion");
    a = &d->nodes[buy].acts[d->nodes[buy].nacts++];
    a->kind = DLA_ADD_TAG, a->who = DLG_WHO_PLAYER;
    snprintf(a->key, DLG_NAME, "Quest.Merchant.Bought");
    int thanks = dlg_add_node(d, DLG_LINE, 1350, -180);
    set_text(&d->nodes[thanks], "Here you are. Come back any time!", "Aqui está. Volte quando quiser!", "Aquí tienes. ¡Vuelve cuando quieras!");
    int who = dlg_add_node(d, DLG_OPTION, 1000, 0);
    set_text(&d->nodes[who], "Who are you?", "Quem é você?", "¿Quién eres?");
    d->nodes[who].once = 1;
    int me = dlg_add_node(d, DLG_LINE, 1350, 0);
    set_text(&d->nodes[me], "Just a humble merchant. Anything else?", "Só um humilde mercador. Mais alguma coisa?", "Solo un humilde mercader. ¿Algo más?");
    int back = dlg_add_node(d, DLG_JUMP, 1700, 0);
    d->nodes[back].jump = (int16_t)ch;
    int bye = dlg_add_node(d, DLG_OPTION, 1000, 180);
    set_text(&d->nodes[bye], "Goodbye.", "Tchau.", "Adiós.");
    int end = dlg_add_node(d, DLG_END, 1700, 180);
    dlg_link(d, 0, hi), dlg_link(d, hi, ch);
    dlg_link(d, ch, buy), dlg_link(d, ch, who), dlg_link(d, ch, bye);
    dlg_link(d, buy, thanks), dlg_link(d, thanks, end);
    dlg_link(d, who, me), dlg_link(d, me, back);
    dlg_link(d, bye, end);
}

Dialogue* dlg_create(const char* folder_vpath, const char* name, bool example) {
    Dialogue* d = calloc(1, sizeof *d);
    if (!d) return NULL;
    char vpath[700];
    for (int i = 0; i < 1000; i++) {
        if (i) snprintf(vpath, sizeof vpath, "%s/%s%d.rdlg", folder_vpath, name, i);
        else snprintf(vpath, sizeof vpath, "%s/%s.rdlg", folder_vpath, name);
        disk_of(vpath, d->path, sizeof d->path);
        if (!re_file_stat(d->path, NULL, NULL, NULL)) break;
    }
    name_of(d->path, d->name);
    d->hash = dlg_vpath_hash(vpath);
    d->chars_per_second = 40.0f;
    d->nodes[0] = (DlgNode){ .kind = DLG_START, .jump = -1 };
    d->nnodes = 1;
    if (example) make_example(d);
    if (!dlg_save(d)) {
        free(d);
        return NULL;
    }
    for (uint32_t i = 0; i < S.n; i++) // (one loaded from that path before, its file deleted since: this one in its place)
        if (S.list[i]->hash == d->hash) {
            *S.list[i] = *d;
            free(d);
            return S.list[i];
        }
    if (S.n >= DLG_MAX_ASSETS) {
        free(d);
        return NULL;
    }
    S.list[S.n++] = d;
    return d;
}

// --- Languages ---

int dlg_language_from_code(const char* code) {
    if (!code || !code[0]) return -1;
    if (!_strnicmp(code, "es", 2) || !_stricmp(code, "spanish") || !_stricmp(code, "espanol") || !_stricmp(code, "español")) return 2;
    if (!_strnicmp(code, "pt", 2) || !_stricmp(code, "portuguese") || !_stricmp(code, "portugues") || !_stricmp(code, "português")) return 1;
    if (!_strnicmp(code, "en", 2) || !_stricmp(code, "english")) return 0;
    return -1;
}
static bool system_spanish(void) {
#ifdef _WIN32
    return (GetUserDefaultUILanguage() & 0x3FF) == 0x0A; // (LANG_SPANISH)
#else
    return false;
#endif
}
int dlg_language(void) {
    if (S.lang >= 0) return S.lang;
    if (gus_applied()->language < 0 && system_spanish()) return 2; // (the game follows the system, and it is Spanish)
    return gus_language() == LANG_PT_BR ? 1 : 0;
}
void dlg_set_language(int lang) { S.lang = lang >= 0 && lang < DLG_LANGS ? lang : -1; }

const char* dlg_node_text(const Dialogue* d, const DlgNode* n, int lang) {
    (void)d;
    if (!n) return "";
    if (lang > 0 && lang < DLG_LANGS && n->text[lang][0]) return n->text[lang];
    if (lang == 1) return tr_lang(n->text[0], LANG_PT_BR); // (i18n's table: "Yes", "Goodbye"... already translated)
    return n->text[0];
}
static const char* player_name(const Dialogue* d, int lang) {
    if (d && d->player_name[0]) return d->player_name;
    static const char* const you[DLG_LANGS] = { "You", "Você", "Tú" };
    return you[lang >= 0 && lang < DLG_LANGS ? lang : 0];
}

// --- Memory ---

static uint32_t* mem_bits(uint32_t hash, bool make) {
    for (uint32_t i = 0; i < S.nmem; i++)
        if (S.mem[i].hash == hash) return S.mem[i].bits;
    if (!make || S.nmem >= MEMORY_MAX) return NULL;
    S.mem[S.nmem].hash = hash;
    memset(S.mem[S.nmem].bits, 0, sizeof S.mem[0].bits);
    return S.mem[S.nmem++].bits;
}
bool dlg_said(uint32_t dialogue, int node) {
    const uint32_t* b = mem_bits(dialogue, false);
    return b && node >= 0 && node < DLG_MAX_NODES && (b[node >> 5] >> (node & 31) & 1u);
}
void dlg_memory_clear(void) { S.nmem = 0; }

uint32_t dlg_save_append(uint8_t* buf, uint32_t len, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < S.nmem; i++) n += S.mem[i].hash != 0;
    if (!n) return len;
    uint8_t rec[8 + MEMORY_MAX * 20];
    uint32_t at = 8, hdr[2] = { SAVE_MAGIC, 0 };
    for (uint32_t i = 0; i < S.nmem && at + 20 <= sizeof rec; i++) {
        if (!S.mem[i].hash) continue;
        memcpy(rec + at, &S.mem[i].hash, 4), memcpy(rec + at + 4, S.mem[i].bits, 16), at += 20;
        hdr[1]++;
    }
    memcpy(rec, hdr, 8);
    uint32_t wd = (at + 15) / 16;
    if (wd > 255) wd = 255, hdr[1] = (255 * 16 - 8) / 20, memcpy(rec, hdr, 8);
    memset(rec + at, 0, wd * 16 > at ? wd * 16 - at : 0);
    uint32_t nl = (uint32_t)strlen(SAVE_NAME), need = 1 + nl + 2 + wd * 16;
    if (len + need > cap) return len;
    buf[len++] = (uint8_t)nl;
    memcpy(buf + len, SAVE_NAME, nl), len += nl;
    buf[len++] = 0, buf[len++] = (uint8_t)wd;
    memcpy(buf + len, rec, wd * 16), len += wd * 16;
    return len;
}

bool dlg_save_read(const uint8_t* d, uint32_t len) {
    if (len < 8 || memcmp(d, "RESAV001", 8)) return false;
    uint32_t at = 8, nl0 = (uint32_t)strlen(SAVE_NAME);
    while (at + 3 < len) { // (the records: the variables', the inventory's, ours)
        uint32_t nl = d[at++];
        if (at + nl + 2 > len) return false;
        bool mine = nl == nl0 && !memcmp(d + at, SAVE_NAME, nl);
        at += nl;
        uint8_t txt = d[at++], wd = d[at++];
        uint32_t size;
        if (txt) {
            if (at + 2 > len) return false;
            size = 2u + (uint32_t)(d[at] | d[at + 1] << 8);
        } else size = wd * 16u;
        if (at + size > len) return false;
        if (mine && !txt && size >= 8) {
            uint32_t hdr[2];
            memcpy(hdr, d + at, 8);
            if (hdr[0] != SAVE_MAGIC || 8 + hdr[1] * 20 > size) return false;
            S.nmem = 0;
            for (uint32_t i = 0; i < hdr[1] && S.nmem < MEMORY_MAX; i++) {
                memcpy(&S.mem[S.nmem].hash, d + at + 8 + i * 20, 4);
                memcpy(S.mem[S.nmem].bits, d + at + 12 + i * 20, 16);
                S.nmem++;
            }
            return true;
        }
        at += size;
    }
    return false;
}

// --- The rules (one core: the game's world or the preview's made-up player) ---

typedef struct Ctx {
    World* w;                  // NULL: the preview
    DlgPreview* p;             // the walk's state
    Entity speaker, player;
} Ctx;

static void plog(Ctx* c, const char* fmt, ...) {
    if (c->w || !c->p) return;
    DlgPreview* p = c->p;
    if (p->nlog >= 16) memmove(p->log[0], p->log[1], sizeof p->log[0] * 15), p->nlog = 15;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->log[p->nlog++], sizeof p->log[0], fmt, ap);
    va_end(ap);
}

static bool said(Ctx* c, int node) {
    if (node < 0 || node >= DLG_MAX_NODES) return false;
    if (!c->w) return (c->p->said[node >> 5] >> (node & 31) & 1u) != 0;
    return dlg_said(c->p->d->hash, node);
}
static void mark_said(Ctx* c, int node) {
    if (node < 0 || node >= DLG_MAX_NODES) return;
    if (!c->w) {
        c->p->said[node >> 5] |= 1u << (node & 31);
        return;
    }
    uint32_t* b = mem_bits(c->p->d->hash, true);
    if (b) b[node >> 5] |= 1u << (node & 31);
}

static bool compare(float a, uint8_t op, float b) {
    switch (op) {
    case DLG_OP_GT: return a > b;
    case DLG_OP_EQ: return fabsf(a - b) < 1e-4f;
    case DLG_OP_NE: return fabsf(a - b) >= 1e-4f;
    case DLG_OP_LT: return a < b;
    case DLG_OP_LE: return a <= b;
    default: return a >= b;
    }
}

// the preview's made-up player
static int pv_item(DlgPreview* p, const char* name, bool make) {
    for (int i = 0; i < 8; i++)
        if (p->items[i][0] && !_stricmp(p->items[i], name)) return i;
    if (!make) return -1;
    for (int i = 0; i < 8; i++)
        if (!p->items[i][0]) return snprintf(p->items[i], DLG_NAME, "%s", name), p->counts[i] = 0, i;
    return -1;
}
static int pv_tag(DlgPreview* p, const char* tag, bool exact) {
    for (int i = 0; i < 8; i++)
        if (p->tags[i][0] && gt_matches(p->tags[i], tag, exact)) return i;
    return -1;
}
static int pv_var(DlgPreview* p, const char* var, bool make) {
    for (int i = 0; i < 6; i++)
        if (p->vars[i][0] && !_stricmp(p->vars[i], var)) return i;
    if (!make) return -1;
    for (int i = 0; i < 6; i++)
        if (!p->vars[i][0]) return snprintf(p->vars[i], DLG_NAME, "%s", var), p->var_values[i][0] = 0, i;
    return -1;
}

static bool cond_ok(Ctx* c, const DlgCond* k) {
    Entity e = k->who == DLG_WHO_SPEAKER ? c->speaker : c->player;
    bool r = true;
    switch (k->kind) {
    case DLC_HAS_TAG:
        if (!c->w) r = k->who == DLG_WHO_PLAYER && pv_tag(c->p, k->key, false) >= 0;
        else r = gt_has(c->w, e, k->key, false);
        break;
    case DLC_HAS_ITEM: {
        int need = k->value >= 1.0f ? (int)k->value : 1, have = 0;
        if (!c->w) {
            int i = k->who == DLG_WHO_PLAYER ? pv_item(c->p, k->key, false) : -1;
            have = i >= 0 ? c->p->counts[i] : 0;
        } else {
            const ItemDef* it = inv_item_by_name(k->key);
            have = it ? inv_count(c->w, e, it->hash) : 0;        }
        r = have >= need;
        break;
    }
    case DLC_VARIABLE: {
        float num = 0.0f;
        char text[DLG_NAME] = "";
        if (!c->w) {
            int i = pv_var(c->p, k->key, false);
            if (i >= 0) num = (float)atof(c->p->var_values[i]), snprintf(text, sizeof text, "%s", c->p->var_values[i]);
        } else if (S.hooks.get_var)
            S.hooks.get_var(c->w, e, k->key, &num, text, sizeof text);
        r = compare(num, k->op, k->value);
        break;
    }
    case DLC_HEALTH: {
        float pc = !c->w ? (k->who == DLG_WHO_PLAYER ? c->p->health : 100.0f) : health_percent(c->w, e) * 100.0f;
        r = compare(pc, k->op, k->value);
        break;
    }
    case DLC_SAID: r = said(c, (int)k->value); break;
    default: break;
    }
    return k->invert ? !r : r;
}
static bool node_ok(Ctx* c, const DlgNode* n) {
    for (uint32_t k = 0; k < n->nconds; k++)
        if (!cond_ok(c, &n->conds[k])) return false;
    return true;
}

static void run_action(Ctx* c, const DlgAct* a) {
    Entity e = a->who == DLG_WHO_SPEAKER ? c->speaker : c->player;
    int count = a->value >= 1.0f ? (int)a->value : 1;
    const char* who = g_dlg_who_names[a->who % DLG_WHO_COUNT];
    switch (a->kind) {
    case DLA_GIVE_ITEM:
    case DLA_TAKE_ITEM:
        if (!c->w) {
            int i = pv_item(c->p, a->key, a->kind == DLA_GIVE_ITEM);
            if (i >= 0) c->p->counts[i] = a->kind == DLA_GIVE_ITEM ? c->p->counts[i] + count : (c->p->counts[i] > count ? c->p->counts[i] - count : 0);
            if (i >= 0 && !c->p->counts[i]) c->p->items[i][0] = 0;
            plog(c, "%s %d %s (%s)", a->kind == DLA_GIVE_ITEM ? "+" : "-", count, a->key, who);
        } else {
            const ItemDef* it = inv_item_by_name(a->key);
            int got = !it ? 0 : a->kind == DLA_GIVE_ITEM ? inv_add(c->w, e, it->hash, count) : inv_remove(c->w, e, it->hash, count);
            if (!it) printf("[RealEngine] Dialogue: no Item Definition \"%s\"\n", a->key);
            (void)got;
        }
        break;
    case DLA_ADD_TAG:
    case DLA_REMOVE_TAG:
        if (!c->w) {
            int i = pv_tag(c->p, a->key, true);
            if (a->kind == DLA_ADD_TAG && i < 0)
                for (int j = 0; j < 8; j++)
                    if (!c->p->tags[j][0]) {
                        snprintf(c->p->tags[j], DLG_NAME, "%s", a->key);
                        break;
                    }
            if (a->kind == DLA_REMOVE_TAG && i >= 0) c->p->tags[i][0] = 0;
            plog(c, "%s %s (%s)", a->kind == DLA_ADD_TAG ? "+tag" : "-tag", a->key, who);
        } else if (a->kind == DLA_ADD_TAG) gt_add(c->w, e, a->key);
        else gt_remove(c->w, e, a->key);
        break;
    case DLA_EVENT: {
        const char* args[2] = { a->arg[0], a->arg[1] };
        uint32_t na = a->arg[1][0] ? 2 : a->arg[0][0] ? 1 : 0;
        if (!c->w) plog(c, "%s.%s(%s%s%s)", who, a->key, a->arg[0], na > 1 ? ", " : "", na > 1 ? a->arg[1] : "");
        else if (!S.hooks.call || !S.hooks.call(c->w, e, a->key, args, na))
            printf("[RealEngine] Dialogue: the %s has no Custom Event \"%s\"\n", who, a->key);
        break;
    }
    case DLA_SET_VAR:
        if (!c->w) {
            int i = pv_var(c->p, a->key, true);
            if (i >= 0) snprintf(c->p->var_values[i], DLG_NAME, "%s", a->arg[0]);
            plog(c, "%s.%s = %s", who, a->key, a->arg[0]);
        } else if (S.hooks.set_var)
            S.hooks.set_var(c->w, e, a->key, a->arg[0]);
        break;
    case DLA_OPEN_SHOP:
        if (!c->w) {
            plog(c, "Open Shop %s", a->key[0] ? a->key : "(OpenShop)");
            break;
        }
        { // the shop's Widget Blueprint on the screen with the cursor out; the speaker's Custom Event OpenShop (its own shop)
            BpAsset* cls = a->key[0] ? bp_find_name(BPA_CLASS, a->key) : NULL;
            Entity wg = cls ? ui_create_widget(c->w, cls) : ENTITY_NONE;
            if (wg) ui_add_to_viewport(wg, 60), ui_show_mouse(true), R.mouse_before = true;
            if (S.hooks.call) S.hooks.call(c->w, c->speaker, "OpenShop", NULL, 0);
        }
        break;
    default: break;
    }
}

// Passing a node: remembered, its actions run
static void pass(Ctx* c, int idx) {
    const DlgNode* n = &c->p->d->nodes[idx];
    mark_said(c, idx);
    for (uint32_t k = 0; k < n->nacts; k++) run_action(c, &n->acts[k]);
}

static void finish(Ctx* c);
static void show_line(Ctx* c, int idx);
static void enter(Ctx* c, int idx, int depth);

// The first of the node's links whose node passes its conditions (none: the end)
static void follow(Ctx* c, int idx, int depth) {
    const Dialogue* d = c->p->d;
    const DlgNode* n = &d->nodes[idx];
    for (uint32_t j = 0; j < n->nlinks; j++) {
        int t = n->links[j];
        if (t <= 0 || t >= (int)d->nnodes || d->nodes[t].kind == DLG_OPTION) continue; // (Options: only under a Choice)
        if (node_ok(c, &d->nodes[t])) {
            enter(c, t, depth + 1);
            return;
        }
    }
    finish(c);
}

static void enter(Ctx* c, int idx, int depth) {
    const Dialogue* d = c->p->d;
    if (idx < 0 || idx >= (int)d->nnodes || depth > MAX_ENTERS) {
        finish(c);
        return;
    }
    const DlgNode* n = &d->nodes[idx];
    pass(c, idx);
    switch (n->kind) {
    case DLG_START: follow(c, idx, depth); break;
    case DLG_LINE: show_line(c, idx); break;
    case DLG_CHOICE: {
        DlgPreview* p = c->p;
        p->nopts = 0;
        for (uint32_t j = 0; j < n->nlinks && p->nopts < DLG_MAX_OPTIONS; j++) {
            int t = n->links[j];
            if (t <= 0 || t >= (int)d->nnodes || d->nodes[t].kind != DLG_OPTION) continue;
            if (d->nodes[t].once && said(c, t)) continue; // (chosen once already)
            if (node_ok(c, &d->nodes[t])) p->opts[p->nopts++] = (int16_t)t;
        }
        if (!p->nopts) { // nothing to choose: its other links, else the end
            follow(c, idx, depth);
            break;
        }
        show_line(c, idx);
        p->choosing = true;
        break;
    }
    case DLG_OPTION: follow(c, idx, depth); break; // (chosen: dlg_choose passed it)
    case DLG_JUMP:
        if (n->jump > 0 && n->jump < (int)d->nnodes && n->jump != idx) enter(c, n->jump, depth + 1);
        else follow(c, idx, depth);
        break;
    default: finish(c); break; // the End
    }
}

static void walk_choose(Ctx* c, int option) {
    DlgPreview* p = c->p;
    if (!p->choosing || option < 0 || option >= p->nopts) return;
    int t = p->opts[option];
    p->choosing = false;
    const DlgNode* o = &p->d->nodes[t];
    if (c->w && S.hooks.event) S.hooks.event(c->w, DLG_EV_CHOICE, c->speaker, "", dlg_node_text(p->d, o, dlg_language()), option);
    else plog(c, "> %s", dlg_node_text(p->d, o, p->lang));
    S.stats.choices++;
    pass(c, t);
    follow(c, t, 0);
}

static void walk_next(Ctx* c) {
    DlgPreview* p = c->p;
    if (p->node < 0 || p->choosing) return;
    follow(c, p->node, 0);
}

// --- The preview's side of the core ---

int dlg_shown_node(const DlgPreview* p) {
    if (!p->d || p->node < 0) return -1;
    const DlgNode* n = &p->d->nodes[p->node];
    return n->kind == DLG_CHOICE && !n->text[0][0] && p->last_line >= 0 ? p->last_line : p->node;
}

static void show_line(Ctx* c, int idx) {
    DlgPreview* p = c->p;
    const DlgNode* n = &p->d->nodes[idx];
    bool echo = n->kind == DLG_CHOICE && !n->text[0][0]; // (the question already shown: it stays, whole)
    p->node = idx, p->choosing = false, p->t = 0.0f, p->shown = 0.0f;
    if (p->d->chars_per_second <= 0.0f || echo) p->shown = 1e9f;
    if (n->kind == DLG_LINE) p->last_line = (int16_t)idx;
    if (echo) return;
    S.stats.lines++;
    if (!c->w) return;
    // the game's: its event, its voice and animation, the widget follows in dlg_tick
    int lang = dlg_language();
    Entity who = !_stricmp(n->speaker, "Player") ? c->player : c->speaker;
    const char* name = !n->speaker[0] ? R.display : !_stricmp(n->speaker, "Player") ? player_name(p->d, lang) : n->speaker;
    if (R.voice) audio_stop(R.voice, 0.1f), R.voice = 0;
    if (n->voice[0]) {
        AudioPlay ap;
        memset(&ap, 0, sizeof ap);
        ap.sound = audio_vpath_hash(n->voice), ap.volume = 1.0f, ap.pitch = 1.0f, ap.bone = -1, ap.ui = true; // (heard while paused)
        ap.follow = who, ap.owner = who;
        const CTransform* t = who ? ecs_get(c->w, who, COMP_TRANSFORM) : NULL;
        if (t) ap.spatial = true, memcpy(ap.pos, t->location, 12);
        R.voice = audio_play(&ap);
    }
    if (n->anim[0] && who && !anim_play_montage(c->w, who, n->anim, 1.0f)) anim_play(c->w, who, n->anim, false);
    R.idle = 0.0f;
    if (S.hooks.event) S.hooks.event(c->w, DLG_EV_LINE, c->speaker, name, dlg_node_text(p->d, n, lang), idx);
}

static void finish(Ctx* c) {
    DlgPreview* p = c->p;
    bool was = p->node >= 0 || p->choosing;
    p->node = -1, p->choosing = false, p->nopts = 0;
    if (!c->w) {
        if (was) plog(c, "(End)");
        return;
    }
    if (R.active) dlg_end(c->w);
}

void dlg_preview_start(DlgPreview* p, const Dialogue* d) {
    int lang = p->lang;
    memset(p->said, 0, sizeof p->said);
    p->d = d, p->node = -1, p->choosing = false, p->nopts = 0, p->nlog = 0, p->lang = lang, p->last_line = -1;
    if (!d || !d->nnodes) return;
    Ctx c = { NULL, p, ENTITY_NONE, ENTITY_NONE };
    p->node = 0; // (so the end is told)
    enter(&c, 0, 0);
}
void dlg_preview_tick(DlgPreview* p, float dt) {
    if (!p->d || p->node < 0) return;
    p->t += dt;
    float cps = p->d->chars_per_second;
    if (cps > 0.0f) p->shown += cps * dt;
    const DlgNode* n = &p->d->nodes[dlg_shown_node(p)];
    uint32_t len = utf8_len(dlg_node_text(p->d, n, p->lang));
    if (!p->choosing && n->kind == DLG_LINE && !n->wait && p->shown >= (float)len) { // (a timed line goes on by itself)
        float stay = n->duration > 0.0f ? n->duration : 2.0f;
        if (p->t - (cps > 0.0f ? (float)len / cps : 0.0f) >= stay) dlg_preview_skip(p);
    }
}
void dlg_preview_skip(DlgPreview* p) {
    if (!p->d || p->node < 0) return;
    uint32_t len = utf8_len(dlg_node_text(p->d, &p->d->nodes[dlg_shown_node(p)], p->lang));
    if (p->shown < (float)len) {
        p->shown = 1e9f;
        return;
    }
    if (p->choosing) return;
    Ctx c = { NULL, p, ENTITY_NONE, ENTITY_NONE };
    walk_next(&c);
}
void dlg_preview_choose(DlgPreview* p, int option) {
    if (!p->d) return;
    Ctx c = { NULL, p, ENTITY_NONE, ENTITY_NONE };
    walk_choose(&c, option);
}

// --- The game's conversation ---

void dlg_set_hooks(const DlgHooks* h) { S.hooks = h ? *h : (DlgHooks){ 0 }; }
const DlgStats* dlg_stats(void) {
    S.stats.assets = S.n;
    return &S.stats;
}

static Ctx game_ctx(World* w) { return (Ctx){ w, &R.st, R.speaker, R.player }; }

bool dlg_active(Entity* speaker) {
    if (speaker) *speaker = R.active ? R.speaker : ENTITY_NONE;
    return R.active;
}

void dlg_view_get(DlgView* v) {
    memset(v, 0, sizeof *v);
    v->node = -1;
    if (!R.active || !R.d || R.st.node < 0) return;
    int lang = dlg_language();
    const DlgNode* n = &R.d->nodes[dlg_shown_node(&R.st)];
    v->active = true, v->choosing = R.st.choosing, v->node = R.st.node;
    v->speaker = !n->speaker[0] ? R.display : !_stricmp(n->speaker, "Player") ? player_name(R.d, lang) : n->speaker;
    v->text = dlg_node_text(R.d, n, lang);
    v->shown_bytes = utf8_prefix(v->text, R.st.shown >= 1e8f ? UINT32_MAX : (uint32_t)R.st.shown);
    for (int i = 0; R.st.choosing && i < R.st.nopts; i++) v->options[v->noptions++] = dlg_node_text(R.d, &R.d->nodes[R.st.opts[i]], lang);
}

static void ent_name(World* w, Entity e, char* out, size_t cap) {
    const CName* nm = e ? ecs_get(w, e, COMP_NAME) : NULL;
    snprintf(out, cap, "%s", nm ? nm->text : "");
}

bool dlg_start(World* w, Dialogue* d, const DlgStartInfo* info) {
    if (!w || !d || !d->nnodes) return false;
    double t0 = re_time_seconds();
    if (R.active) dlg_end(w); // (one at a time: the last one ends)
    DlgStartInfo in = info ? *info : (DlgStartInfo){ 0 };
    memset(&R.st, 0, sizeof R.st);
    R.st.last_line = -1;
    R.d = d, R.version = d->version, R.w = w;
    R.speaker = in.speaker;
    R.player = in.player ? in.player : S.hooks.player ? S.hooks.player(w) : ENTITY_NONE;
    if (in.display_name && in.display_name[0]) snprintf(R.display, sizeof R.display, "%s", in.display_name);
    else ent_name(w, R.speaker, R.display, sizeof R.display);
    R.camera = in.camera && R.speaker;
    R.cam_distance = in.cam_distance > 1.0f ? in.cam_distance : 260.0f;
    R.cam_height = in.cam_height != 0.0f ? in.cam_height : 60.0f;
    R.cam_angle = in.cam_angle;
    R.have_cam0 = R.camera && S.hooks.camera && S.hooks.camera(w, R.cam0, &R.yaw0, &R.pitch0); // (before it is active: the gameplay view)
    R.cam_t = 0.0f;
    R.voice = 0, R.last_node = -2, R.last_bytes = UINT32_MAX, R.last_nopts = -1, R.last_choosing = false;
    R.active = true;
    S.stats.conversations++;
    // the widget, the cursor out (its Options navigable by keyboard / gamepad), the pause
    BpAsset* cls = bp_find_hash(in.widget ? in.widget : dlg_widget_asset());
    if (cls && R.kept && R.kept_cls == cls->hash && ecs_alive(w, R.kept)) R.widget = R.kept; // (no new entity a conversation)
    else R.widget = cls ? ui_create_widget(w, cls) : ENTITY_NONE, R.kept = R.widget, R.kept_cls = cls ? cls->hash : 0u;
    if (R.widget) ui_add_to_viewport(R.widget, 50);
    R.mouse_before = ui_mouse_shown();
    ui_show_mouse(true);
    bool pause = in.pause < 0 ? d->pause != 0 : in.pause != 0;
    R.paused_by_us = pause && !game_paused();
    if (R.paused_by_us) game_set_paused(true);
    Ctx c = game_ctx(w);
    R.st.d = d, R.st.node = 0;
    enter(&c, 0, 0);
    S.stats.advance_us = (re_time_seconds() - t0) * 1e6;
    return R.active;
}

bool dlg_start_speaker(World* w, Entity speaker, const char* dialogue, Entity player) {
    const CDialogueSpeaker* s = speaker ? ecs_get(w, speaker, COMP_DIALOGUE_SPEAKER) : NULL;
    Dialogue* d = dialogue && dialogue[0] ? dlg_find_name(dialogue) : NULL;
    if (!d && dialogue && dialogue[0]) d = dlg_load(dialogue); // (a vpath)
    if (!d && s) d = dlg_find(s->dialogue);
    if (!d) return false;
    DlgStartInfo in = { speaker, player, s && s->display_name[0] ? s->display_name : NULL, -1, false, 0, 0, 0, 0 };
    if (s) {
        in.pause = s->pause == DLG_PAUSE_YES ? 1 : s->pause == DLG_PAUSE_NO ? 0 : -1;
        in.camera = (s->flags & DLG_SPK_CAMERA) != 0;
        in.cam_distance = s->cam_distance, in.cam_height = s->cam_height, in.cam_angle = s->cam_angle;
        in.widget = s->widget;
        if (s->flags & DLG_SPK_FACE) { // the speaker turns to the player
            Entity p = player ? player : S.hooks.player ? S.hooks.player(w) : ENTITY_NONE;
            CTransform* st = ecs_get(w, speaker, COMP_TRANSFORM);
            const CTransform* pt = p ? ecs_get(w, p, COMP_TRANSFORM) : NULL;
            if (st && pt) {
                float dx = pt->location[0] - st->location[0], dy = pt->location[1] - st->location[1];
                if (dx * dx + dy * dy > 1.0f) st->rotation[1] = atan2f(dy, dx) / DEG2RAD, ecs_touch(w, speaker);
            }
        }
    }
    return dlg_start(w, d, &in);
}

bool dlg_choose(World* w, int option) {
    if (!R.active || !R.st.choosing || option < 0 || option >= R.st.nopts) return false;
    double t0 = re_time_seconds();
    Ctx c = game_ctx(w);
    walk_choose(&c, option);
    S.stats.advance_us = (re_time_seconds() - t0) * 1e6;
    return true;
}

void dlg_skip(World* w) {
    if (!R.active || R.st.node < 0) return;
    const DlgNode* n = &R.d->nodes[dlg_shown_node(&R.st)];
    uint32_t len = utf8_len(dlg_node_text(R.d, n, dlg_language()));
    if (R.st.shown < (float)len) { // the text all at once first
        R.st.shown = 1e9f;
        return;
    }
    if (R.st.choosing) return;
    double t0 = re_time_seconds();
    Ctx c = game_ctx(w);
    walk_next(&c);
    S.stats.advance_us = (re_time_seconds() - t0) * 1e6;
}

void dlg_end(World* w) {
    if (!R.active) return;
    R.active = false; // (first: an event that starts another conversation sees this one over)
    Entity sp = R.speaker;
    char name[DLG_NAME];
    snprintf(name, sizeof name, "%s", R.d ? R.d->name : "");
    R.st.node = -1, R.st.choosing = false;
    if (R.voice) audio_stop(R.voice, 0.2f), R.voice = 0;
    if (R.widget) ui_remove_from_parent(R.widget), R.widget = ENTITY_NONE;
    ui_show_mouse(R.mouse_before);
    if (R.paused_by_us) game_set_paused(false), R.paused_by_us = false;
    if (S.hooks.event && w) S.hooks.event(w, DLG_EV_END, sp, name, "", 0);
}

void dlg_begin_play(World* w) {
    (void)w;
    memset(&R, 0, sizeof R);
    R.st.node = -1;
    S.nmem = 0;
    S.stats.lines = S.stats.choices = S.stats.conversations = 0;
    S.stats.tick_ms = S.stats.advance_us = 0.0;
}

void dlg_end_play(World* w) {
    if (R.active) {
        DlgHooks keep = S.hooks;
        S.hooks.event = NULL; // (the play is ending: no events into instances going away)
        dlg_end(w);
        S.hooks = keep;
    }
    memset(&R, 0, sizeof R);
    R.st.node = -1;
    S.lang = -1;
}

static void set_widget(World* w) {
    (void)w;
    if (!R.widget || !R.active || R.st.node < 0) return;
    DlgView v;
    dlg_view_get(&v);
    bool node_changed = R.last_node != v.node || R.last_choosing != v.choosing || R.last_lang != dlg_language();
    if (node_changed) {
        ui_set_text(R.widget, "Speaker", v.speaker ? v.speaker : "");
        ui_set_visible(R.widget, "Speaker", v.speaker && v.speaker[0]);
        static const char* const cont[DLG_LANGS] = { "Continue", "Continuar", "Continuar" };
        ui_set_text(R.widget, "Hint", cont[dlg_language()]);
    }
    if (node_changed || R.last_bytes != v.shown_bytes) { // the letters shown so far
        uint32_t nb = v.shown_bytes < DLG_TEXT - 1 ? v.shown_bytes : DLG_TEXT - 1;
        memcpy(R.shown_text, v.text, nb), R.shown_text[nb] = 0;
        ui_set_text(R.widget, "Line", R.shown_text);
        R.last_bytes = v.shown_bytes;
    }
    if (node_changed || R.last_nopts != (int)v.noptions) {
        for (int i = 0; i < WIDGET_OPTIONS; i++) {
            char b[24], t[32];
            snprintf(b, sizeof b, "Option%d", i + 1), snprintf(t, sizeof t, "Option%dText", i + 1);
            bool on = v.choosing && i < (int)v.noptions;
            ui_set_visible(R.widget, b, on);
            if (on) ui_set_text(R.widget, t, v.options[i]);
        }
        ui_set_visible(R.widget, "Choices", v.choosing);
        ui_set_visible(R.widget, "Continue", !v.choosing);
        ui_set_visible(R.widget, "Hint", !v.choosing);
        R.last_nopts = (int)v.noptions;
    }
    R.last_node = v.node, R.last_choosing = v.choosing, R.last_lang = dlg_language();
}

void dlg_tick(World* w, float dt) {
    if (!R.active) return;
    double t0 = re_time_seconds();
    if (!R.d || R.st.node < 0) {
        dlg_end(w);
        return;
    }
    R.st.t += dt, R.cam_t += dt;
    float cps = R.d->chars_per_second;
    const DlgNode* n = &R.d->nodes[dlg_shown_node(&R.st)];
    uint32_t len = utf8_len(dlg_node_text(R.d, n, dlg_language()));
    if (cps > 0.0f && R.st.shown < (float)len) R.st.shown += cps * dt;
    else R.st.shown = R.st.shown < (float)len ? (float)len : R.st.shown;
    if (!R.st.choosing && R.d->nodes[R.st.node].kind == DLG_LINE && !n->wait && R.st.shown >= (float)len) { // a timed line: on by itself
        R.idle += dt;
        float stay = n->duration > 0.0f ? n->duration : 2.0f;
        bool voice = R.voice && audio_is_playing(R.voice);
        if (R.idle >= stay && !(n->duration <= 0.0f && voice)) dlg_skip(w);
    }
    if (R.active) set_widget(w);
    S.stats.tick_ms = (re_time_seconds() - t0) * 1000.0;
}

// The conversation camera: beside the player, looking at the speaker's face, blended in from the gameplay view
bool dlg_view_camera(World* w, float pos[3], float* yaw, float* pitch) {
    if (!R.active || !R.camera || !w) return false;
    const CTransform* st = ecs_get(w, R.speaker, COMP_TRANSFORM);
    const CTransform* pt = R.player ? ecs_get(w, R.player, COMP_TRANSFORM) : NULL;
    if (!st) return false;
    float target[3] = { st->location[0], st->location[1], st->location[2] + R.cam_height };
    float dx = 1.0f, dy = 0.0f; // from the speaker toward the player (none: the speaker's front)
    if (pt) dx = pt->location[0] - st->location[0], dy = pt->location[1] - st->location[1];
    float l = sqrtf(dx * dx + dy * dy);
    if (l < 1.0f) dx = cosf(st->rotation[1] * DEG2RAD), dy = sinf(st->rotation[1] * DEG2RAD), l = 1.0f;
    dx /= l, dy /= l;
    float a = (R.cam_angle != 0.0f ? R.cam_angle : 25.0f) * DEG2RAD, ca = cosf(a), sa = sinf(a); // (over the player's shoulder)
    float ox = dx * ca - dy * sa, oy = dx * sa + dy * ca;
    float cp[3] = { target[0] + ox * R.cam_distance, target[1] + oy * R.cam_distance, target[2] + 10.0f };
    float fx = target[0] - cp[0], fy = target[1] - cp[1], fz = target[2] - cp[2];
    float cy = atan2f(fy, fx) / DEG2RAD, cpch = atan2f(fz, sqrtf(fx * fx + fy * fy)) / DEG2RAD;
    float k = R.have_cam0 ? fminf(R.cam_t / 0.5f, 1.0f) : 1.0f;
    k = k * k * (3.0f - 2.0f * k);
    if (k >= 1.0f) {
        memcpy(pos, cp, 12), *yaw = cy, *pitch = cpch;
        return true;
    }
    float dyaw = fmodf(cy - R.yaw0 + 540.0f, 360.0f) - 180.0f; // (the short way round)
    for (int i = 0; i < 3; i++) pos[i] = R.cam0[i] + (cp[i] - R.cam0[i]) * k;
    *yaw = R.yaw0 + dyaw * k, *pitch = R.pitch0 + (cpch - R.pitch0) * k;
    return true;
}

// --- WBP_Dialogue: the box at the bottom (the name, the line, the Options), its graph calling Skip Line / Choose Option ---

static bool g_bad;
static int W(UiTree* t, uint8_t kind, int parent, const char* name) {
    int i = ui_tree_add(t, kind, parent);
    if (i < 0) return g_bad = true, 0;
    if (name) snprintf(t->w[i].name, UI_NAME, "%s", name);
    return i;
}
static void rgba(float* c, float r, float g, float b, float a) { c[0] = r, c[1] = g, c[2] = b, c[3] = a; }
static void bottom(UiWidget* w, float x, float y, float wd, float ht) { // (anchored at the bottom's middle)
    w->anchor[0] = w->anchor[2] = 0.5f, w->anchor[1] = w->anchor[3] = 1.0f;
    w->align[0] = w->align[1] = 0.0f;
    w->pos[0] = x, w->pos[1] = y, w->size[0] = wd, w->size[1] = ht;
}
static int pin_named(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    g_bad = true;
    return 0;
}
static uint32_t N(BpAsset* a, BpGraph* g, uint16_t kind, float x, float y, const char* txt) {
    BpNode* n = bp_add_node(a, g, kind, x * 320.0f, y * 160.0f, txt, NULL);
    if (!n) g_bad = true;
    return n ? n->id : 0;
}

uint32_t dlg_widget_asset(void) {
    BpAsset* a = bp_load(WIDGET_VPATH);
    if (a) return a->hash;
    const char* root = ui_content_root();
    if (!root[0]) return 0;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/UI", root);
    re_make_dirs(dir);
    a = bp_create(BPA_CLASS, "/Content/UI", "WBP_Dialogue", "User Widget");
    if (!a) return 0;
    if (!a->ui) a->ui = calloc(1, sizeof(UiTree)); // (an edit, not a frame)
    if (!a->ui) return 0;
    UiTree* t = a->ui;
    ui_tree_default(t);
    g_bad = false;
    const float PW = 1300.0f, PH = 386.0f, PY = -30.0f - PH;
    int panel = W(t, UIW_BORDER, 0, "Panel");
    bottom(&t->w[panel], -PW * 0.5f, PY, PW, PH);
    rgba(t->w[panel].bg, 0.03f, 0.035f, 0.045f, 0.9f), t->w[panel].radius = 14.0f, t->w[panel].padding = 24.0f;
    t->w[panel].border_w = 1.0f, rgba(t->w[panel].border_col, 1, 1, 1, 0.1f);
    int rows = W(t, UIW_VBOX, panel, "Rows");
    t->w[rows].padding = 6.0f, t->w[rows].bg[3] = 0.0f;
    int sp = W(t, UIW_TEXT, rows, "Speaker");
    snprintf(t->w[sp].text, sizeof t->w[sp].text, "Speaker");
    t->w[sp].font_size = 30.0f, t->w[sp].bold = 1, t->w[sp].size[1] = 42.0f, t->w[sp].align[0] = 0.0f, t->w[sp].align[1] = 0.5f;
    rgba(t->w[sp].color, 1.0f, 0.78f, 0.36f, 1.0f);
    int line = W(t, UIW_TEXT, rows, "Line");
    snprintf(t->w[line].text, sizeof t->w[line].text, "The line appears here, letter by letter.");
    t->w[line].font_size = 27.0f, t->w[line].wrap = 1, t->w[line].size[1] = 72.0f, t->w[line].align[0] = 0.0f, t->w[line].align[1] = 0.0f;
    int choices = W(t, UIW_VBOX, rows, "Choices");
    t->w[choices].padding = 4.0f, t->w[choices].bg[3] = 0.0f, t->w[choices].size_rule = 1;
    for (int i = 0; i < WIDGET_OPTIONS; i++) {
        char bn[24], tn[32];
        snprintf(bn, sizeof bn, "Option%d", i + 1), snprintf(tn, sizeof tn, "Option%dText", i + 1);
        int b = W(t, UIW_BUTTON, choices, bn);
        t->w[b].size[1] = 32.0f, t->w[b].radius = 6.0f, t->w[b].padding = 4.0f;
        rgba(t->w[b].bg, 1, 1, 1, 0.04f), rgba(t->w[b].hover, 0.20f, 0.42f, 0.78f, 0.95f), rgba(t->w[b].press, 0.12f, 0.30f, 0.62f, 1.0f);
        t->w[b].border_w = 0.0f;
        int l = W(t, UIW_TEXT, b, tn);
        snprintf(t->w[l].text, sizeof t->w[l].text, "%d. Option", i + 1);
        t->w[l].font_size = 24.0f, t->w[l].size[1] = 30.0f, t->w[l].align[0] = 0.0f, t->w[l].align[1] = 0.5f;
        t->w[l].slot_pad[0] = 12.0f;
    }
    // the whole box clicks to go on (over the texts, under nothing: transparent)
    int cont = W(t, UIW_BUTTON, 0, "Continue");
    bottom(&t->w[cont], -PW * 0.5f, PY, PW, PH);
    rgba(t->w[cont].bg, 1, 1, 1, 0.0f), rgba(t->w[cont].hover, 1, 1, 1, 0.03f), rgba(t->w[cont].press, 1, 1, 1, 0.06f);
    t->w[cont].border_w = 0.0f, t->w[cont].radius = 14.0f;
    int hint = W(t, UIW_TEXT, 0, "Hint");
    bottom(&t->w[hint], PW * 0.5f - 300.0f, -30.0f - 50.0f, 280.0f, 34.0f);
    snprintf(t->w[hint].text, sizeof t->w[hint].text, "Continue");
    t->w[hint].font_size = 19.0f, t->w[hint].align[0] = 1.0f, t->w[hint].align[1] = 0.5f;
    rgba(t->w[hint].color, 0.65f, 0.65f, 0.68f, 1.0f);
    // the graph: OnClicked_Continue -> Skip Line; OnClicked_OptionN -> Choose Option (N - 1)
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t ev = N(a, g, BPK_EVENT_CUSTOM, 0, 0, "OnClicked_Continue"), sk = N(a, g, BPK_DLG_SKIP, 1, 0, NULL);
    if (!bp_link(g, ev, (uint8_t)pin_named(g, ev, 1, ""), sk, (uint8_t)pin_named(g, sk, 0, ""))) g_bad = true;
    for (int i = 0; i < WIDGET_OPTIONS; i++) {
        char en[48];
        snprintf(en, sizeof en, "OnClicked_Option%d", i + 1);
        uint32_t e = N(a, g, BPK_EVENT_CUSTOM, 0, (float)(i + 1), en), c = N(a, g, BPK_DLG_CHOOSE, 1, (float)(i + 1), NULL);
        if (!bp_link(g, e, (uint8_t)pin_named(g, e, 1, ""), c, (uint8_t)pin_named(g, c, 0, ""))) g_bad = true;
        BpNode* cn = bp_node(g, c);
        if (cn) cn->pins[pin_named(g, c, 0, "Index")].lit.v[0] = (float)i;
    }
    if (g_bad) printf("[RealEngine] Dialogue: WBP_Dialogue was made with errors (%u widgets)\n", t->n);
    if (!bp_compile(a)) printf("[RealEngine] Dialogue: WBP_Dialogue: %s\n", a->error);
    bp_save(a);
    return a->hash;
}

// --- The self-test: the rules on a made-up dialogue in the preview ---

int dlg_selftest(void) {
    int fails = 0;
#define T(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: dialogue selftest: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
    Dialogue* d = calloc(1, sizeof *d);
    DlgPreview* p = calloc(1, sizeof *p);
    if (!d || !p) return free(d), free(p), 1;
    snprintf(d->name, DLG_NAME, "DLG_SelfTest");
    d->hash = 12345u, d->chars_per_second = 20.0f;
    d->nodes[0] = (DlgNode){ .kind = DLG_START, .jump = -1 };
    d->nnodes = 1;
    make_example(d); // (the merchant: buy needs DA_Coin; who-are-you is Once and jumps back)
    p->lang = 0, p->health = 100.0f;
    dlg_preview_start(p, d);
    T(p->node == 1 && !p->choosing, "the first line (node %d)", p->node);
    T(!strcmp(dlg_node_text(d, &d->nodes[p->node], 0), "Welcome, traveler! What brings you here?"), "the English text");
    T(!strcmp(dlg_node_text(d, &d->nodes[p->node], 1), "Bem-vindo, viajante! O que o traz aqui?"), "the Portuguese text");
    T(!strncmp(dlg_node_text(d, &d->nodes[p->node], 2), "\xC2\xA1" "Bienvenido", 11), "the Spanish text");
    dlg_preview_tick(p, 0.5f); // (10 letters)
    T(p->shown >= 9.9f && p->shown <= 10.1f, "the letters shown: %.1f (10)", p->shown);
    dlg_preview_skip(p); // all at once
    T(p->shown > 1000.0f && p->node == 1, "Skip shows the whole line first");
    dlg_preview_skip(p); // to the Choice
    T(p->choosing && p->nopts == 2, "no coin: buy hidden, %d options (2)", p->nopts);
    dlg_preview_choose(p, 0); // who are you (Once) -> its line
    T(!p->choosing && p->node == 6, "the option's line (node %d, 6)", p->node);
    dlg_preview_skip(p), dlg_preview_skip(p); // the line, then the Jump back to the Choice
    T(p->choosing && p->nopts == 1, "the Jump back: the Once option gone, %d options (1)", p->nopts);
    // a coin: the buy option shows; it takes the coin, gives a potion, tags the player
    snprintf(p->items[0], DLG_NAME, "DA_Coin"), p->counts[0] = 1;
    dlg_preview_start(p, d);
    dlg_preview_skip(p), dlg_preview_skip(p);
    T(p->choosing && p->nopts == 3, "with a coin: %d options (3)", p->nopts);
    dlg_preview_choose(p, 0);
    int potion = pv_item(p, "DA_Potion", false);
    T(potion >= 0 && p->counts[potion] == 1 && pv_item(p, "DA_Coin", false) < 0, "the coin for a potion");
    T(pv_tag(p, "Quest.Merchant", false) >= 0, "the tag (Quest.Merchant.Bought) under Quest.Merchant");
    dlg_preview_skip(p), dlg_preview_skip(p);
    T(p->node < 0, "the end after thanks (node %d)", p->node);
    // a timed line goes on by itself
    d->nodes[1].wait = 0, d->nodes[1].duration = 0.5f;
    dlg_preview_start(p, d);
    for (int i = 0; i < 40; i++) dlg_preview_tick(p, 0.1f);
    T(p->choosing, "a timed line did not go on");
    // the save record: what was said, back
    S.nmem = 0;
    uint32_t* b = mem_bits(777u, true);
    if (b) b[0] = 0x15u, b[3] = 0x80000000u;
    uint8_t buf[512];
    memcpy(buf, "RESAV001", 8);
    uint32_t n = dlg_save_append(buf, 8, sizeof buf);
    S.nmem = 0;
    T(n > 8 && dlg_save_read(buf, n) && dlg_said(777u, 0) && dlg_said(777u, 4) && !dlg_said(777u, 1) && dlg_said(777u, 127),
      "the memory's save record");
    S.nmem = 0;
    T(dlg_language_from_code("es_ES") == 2 && dlg_language_from_code("pt_BR") == 1 && dlg_language_from_code("en") == 0, "language codes");
    free(d), free(p);
#undef T
    return fails;
}
