// Artificial intelligence (ai.h, notas-dev/ia.md): the assets (Behavior Trees, Brains), the navigation grid, the
// agents' perception, Blackboards, trees and brains, and the movement they ask of their pawns.
#include "ai.h"
#include "i18n.h" // (item 21: the example Brains' names)
#include "landscape.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "bp_internal.h"
#include "components.h"
#include "io_json.h"
#include "navmesh.h"
#include "platform.h"
#include "platform_files.h"
#include "project.h"
#include "render.h"

#define DEG2RAD 0.017453293f
#define MAX_ASSETS 128
#define MAX_AGENTS 4096
#define MAX_PATH 64
#define STEP_H 45.0f      // cm a walker climbs
#define CLEARANCE 170.0f  // cm of head room a cell needs
#define MAX_SPANS 8
#define MAX_NOISES 32
#define SENSE_PERIOD 0.15f // s between two looks

const char* const g_bt_kind_names[BT_KIND_COUNT] = { "Root", "Selector", "Sequence", "Move To", "Wait", "Rotate to face BB entry",
                                                     "Find Random Location", "Set Blackboard Value", "Clear Blackboard Value",
                                                     "Run Event", "Play Montage", "Predict Target Location", "Find Surround Spot" };
const char* const g_btd_names[BTD_KIND_COUNT] = { "None", "Blackboard", "Cooldown", "Loop", "Time Limit", "Force Success" };
const char* const g_bts_names[BTS_KIND_COUNT] = { "None", "Default Focus", "Set Movement Speed" };
const char* const g_brain_input_names[BRAIN_IN_COUNT] = { "Distance to Target", "Sees Target", "Time Since Seen", "Heard Noise",
                                                          "Blackboard Float", "Blueprint Variable", "Random", "Time in Behavior",
                                                          "Has Target", "Squad Size", "Target Speed" };
const char* const g_curve_names[CURVE_COUNT] = { "Linear", "Inverse", "Step", "Smooth", "Bell" };
const char* const g_brain_action_names[BRAIN_ACT_COUNT] = { "Idle", "Wander", "Chase", "Flee", "Keep Distance", "Investigate",
                                                            "Patrol", "Attack", "Run Behavior Tree", "Blueprint Event", "Intercept",
                                                            "Surround" };
const char* const g_bb_type_names[BB_TYPE_COUNT] = { "Bool", "Float", "Vector", "Actor" };

static struct {
    char root[600];
    bool scanned;
    BehaviorTree* trees[MAX_ASSETS];
    Brain* brains[MAX_ASSETS];
    uint32_t ntrees, nbrains;
} S;

// --- Assets ---

static uint32_t vpath_hash(const char* vpath) {
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) h = (h ^ (uint8_t)tolower((unsigned char)*c)) * 16777619u;
    return h ? h : 1u;
}
static void disk_of(const char* vpath, char* out, size_t cap) {
    if (!strncmp(vpath, "/Content", 8)) snprintf(out, cap, "%s%s", S.root, vpath + 8);
    else snprintf(out, cap, "%s", vpath);
}
static void vpath_of(const char* disk, char* out, size_t cap) {
    size_t rl = strlen(S.root);
    if (!strncmp(disk, S.root, rl)) snprintf(out, cap, "/Content%s", disk + rl);
    else snprintf(out, cap, "%s", disk);
}
static void name_of(const char* path, char* out) {
    const char* s = strrchr(path, '/');
    snprintf(out, AI_NAME, "%s", s ? s + 1 : path);
    char* dot = strrchr(out, '.');
    if (dot) *dot = 0;
}
static char* read_text(const char* path, size_t* len) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = n > 0 ? malloc((size_t)n + 1) : NULL; // a load, not a frame
    if (t && fread(t, 1, (size_t)n, f) != (size_t)n) free(t), t = NULL;
    fclose(f);
    if (t) t[n] = 0, *len = (size_t)n;
    return t;
}
static bool write_text(const char* path, JsonWriter* w) {
    if (w->fail) return false;
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = re_fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(w->buf, 1, w->len, f) == w->len;
    ok = fclose(f) == 0 && ok;
    return ok && re_replace_file(tmp, path);
}

static void write_keys(JsonWriter* w, const BbKey* k, uint32_t n) {
    jw_key(w, "blackboard");
    jw_arr_begin(w);
    for (uint32_t i = 0; i < n; i++) {
        jw_obj_begin(w);
        jw_kstr(w, "name", k[i].name);
        jw_kint(w, "type", k[i].type);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}
static uint32_t merge_perception(BbKey* k, uint32_t n);
static uint32_t read_keys(const JsonValue* r, BbKey* k) {
    const JsonValue* a = json_get(r, "blackboard");
    uint32_t n = 0;
    for (size_t i = 0; i < json_len(a) && n < AI_MAX_KEYS; i++) {
        snprintf(k[n].name, AI_NAME, "%s", json_get_str(json_at(a, i), "name", "Key"));
        k[n].type = (uint8_t)json_get_int(json_at(a, i), "type", BB_FLOAT);
        if (k[n].type >= BB_TYPE_COUNT) k[n].type = BB_FLOAT;
        n++;
    }
    return n;
}

bool ai_tree_save(BehaviorTree* t) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1);
    write_keys(&w, t->keys, t->nkeys);
    jw_key(&w, "nodes");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < t->nnodes; i++) {
        const BtNode* n = &t->nodes[i];
        jw_obj_begin(&w);
        jw_kint(&w, "kind", n->kind);
        jw_kint(&w, "parent", n->parent);
        jw_kfloats(&w, "pos", (float[]){ n->x, n->y }, 2);
        jw_kstr(&w, "key", n->key);
        jw_kfloats(&w, "value", (float[]){ n->value, n->value2 }, 2);
        jw_kstr(&w, "text", n->text);
        jw_key(&w, "decos");
        jw_arr_begin(&w);
        for (uint32_t d = 0; d < n->ndecos; d++) {
            const BtDeco* q = &n->decos[d];
            jw_obj_begin(&w);
            jw_kint(&w, "kind", q->kind);
            jw_kint(&w, "op", q->op);
            jw_kint(&w, "aborts", q->aborts);
            jw_kbool(&w, "inverse", q->inverse);
            jw_kstr(&w, "key", q->key);
            jw_kfloat(&w, "value", q->value);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_key(&w, "services");
        jw_arr_begin(&w);
        for (uint32_t s = 0; s < n->nsvcs; s++) {
            jw_obj_begin(&w);
            jw_kint(&w, "kind", n->svcs[s].kind);
            jw_kstr(&w, "key", n->svcs[s].key);
            jw_kfloat(&w, "value", n->svcs[s].value);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_kstr(&w, "comment", n->comment);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    bool ok = write_text(t->path, &w);
    jw_free(&w);
    return ok;
}

BehaviorTree* ai_tree_load(const char* vpath) {
    uint32_t h = vpath_hash(vpath);
    for (uint32_t i = 0; i < S.ntrees; i++)
        if (S.trees[i]->hash == h) return S.trees[i];
    if (S.ntrees >= MAX_ASSETS) return NULL;
    char path[700];
    disk_of(vpath, path, sizeof path);
    size_t len;
    char* text = read_text(path, &len);
    if (!text) return NULL;
    JsonDoc doc;
    char err[160];
    BehaviorTree* t = NULL;
    if (json_parse(text, len, &doc, err, sizeof err)) {
        t = calloc(1, sizeof *t); // an asset, not a frame
        if (t) {
            snprintf(t->path, sizeof t->path, "%s", path);
            name_of(path, t->name);
            t->hash = h;
            t->nkeys = merge_perception(t->keys, read_keys(&doc.root, t->keys));
            const JsonValue* ns = json_get(&doc.root, "nodes");
            for (size_t i = 0; i < json_len(ns) && t->nnodes < AI_MAX_NODES; i++) {
                const JsonValue* o = json_at(ns, i);
                BtNode* n = &t->nodes[t->nnodes];
                memset(n, 0, sizeof *n);
                n->kind = (uint8_t)json_get_int(o, "kind", BT_WAIT);
                if (n->kind >= BT_KIND_COUNT) n->kind = BT_WAIT;
                int p = json_get_int(o, "parent", -1);
                n->parent = (int16_t)(p >= 0 && p < (int)t->nnodes ? p : p == BT_DETACHED ? BT_DETACHED : -1);
                float pos[2] = { 0, 0 }, val[2] = { 0, 0 };
                json_get_floats(o, "pos", pos, 2);
                json_get_floats(o, "value", val, 2);
                n->x = pos[0], n->y = pos[1], n->value = val[0], n->value2 = val[1];
                snprintf(n->key, AI_NAME, "%s", json_get_str(o, "key", ""));
                snprintf(n->text, AI_NAME, "%s", json_get_str(o, "text", ""));
                const JsonValue* ds = json_get(o, "decos");
                for (size_t d = 0; d < json_len(ds) && n->ndecos < BT_MAX_DECOS; d++) {
                    const JsonValue* q = json_at(ds, d);
                    BtDeco* x = &n->decos[n->ndecos++];
                    x->kind = (uint8_t)json_get_int(q, "kind", 0);
                    x->op = (uint8_t)json_get_int(q, "op", 0);
                    x->aborts = (uint8_t)json_get_int(q, "aborts", BTA_BOTH);
                    x->inverse = json_get_bool(q, "inverse", false);
                    snprintf(x->key, AI_NAME, "%s", json_get_str(q, "key", ""));
                    x->value = (float)json_get_num(q, "value", 0.0);
                    if (x->kind >= BTD_KIND_COUNT) n->ndecos--;
                }
                const JsonValue* ss = json_get(o, "services");
                for (size_t s = 0; s < json_len(ss) && n->nsvcs < BT_MAX_SVCS; s++) {
                    const JsonValue* q = json_at(ss, s);
                    BtService* x = &n->svcs[n->nsvcs++];
                    x->kind = (uint8_t)json_get_int(q, "kind", 0);
                    snprintf(x->key, AI_NAME, "%s", json_get_str(q, "key", ""));
                    x->value = (float)json_get_num(q, "value", 0.0);
                    if (!x->kind || x->kind >= BTS_KIND_COUNT) n->nsvcs--;
                }
                if (!ds) { // (the first files: one decorator and one service per node)
                    int dk = json_get_int(o, "deco", 0);
                    if (dk > 0 && dk < BTD_KIND_COUNT) {
                        BtDeco* x = &n->decos[n->ndecos++];
                        x->kind = (uint8_t)dk, x->op = (uint8_t)json_get_int(o, "deco_op", 0), x->aborts = BTA_BOTH;
                        snprintf(x->key, AI_NAME, "%s", json_get_str(o, "deco_key", ""));
                        x->value = (float)json_get_num(o, "deco_value", 0.0);
                    }
                    int sk = json_get_int(o, "service", 0);
                    if (sk > 0 && sk < BTS_KIND_COUNT) {
                        BtService* x = &n->svcs[n->nsvcs++];
                        x->kind = (uint8_t)sk;
                        snprintf(x->key, AI_NAME, "%s", json_get_str(o, "service_key", ""));
                        x->value = (float)json_get_num(o, "service_value", 0.0);
                    }
                }
                snprintf(n->comment, sizeof n->comment, "%s", json_get_str(o, "comment", ""));
                t->nnodes++;
            }
            if (!t->nnodes) t->nodes[t->nnodes++] = (BtNode){ .kind = BT_ROOT, .parent = -1 };
            S.trees[S.ntrees++] = t;
        }
        json_free(&doc);
    }
    free(text);
    return t;
}

bool ai_brain_save(Brain* b) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1);
    write_keys(&w, b->keys, b->nkeys);
    jw_kfloat(&w, "momentum", b->momentum);
    jw_kfloat(&w, "think_interval", b->think_interval);
    jw_key(&w, "behaviors");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < b->nbehaviors; i++) {
        const BrainBehavior* h = &b->behaviors[i];
        jw_obj_begin(&w);
        jw_kstr(&w, "name", h->name);
        jw_kint(&w, "action", h->action);
        jw_kfloat(&w, "weight", h->weight);
        jw_kfloat(&w, "speed", h->speed);
        jw_kfloats(&w, "param", (float[]){ h->param, h->param2 }, 2);
        jw_kstr(&w, "text", h->text);
        jw_key(&w, "considerations");
        jw_arr_begin(&w);
        for (uint32_t c = 0; c < h->ncons; c++) {
            const Consideration* k = &h->cons[c];
            jw_obj_begin(&w);
            jw_kint(&w, "input", k->input);
            jw_kint(&w, "curve", k->curve);
            jw_kstr(&w, "key", k->key);
            jw_kfloats(&w, "range", (float[]){ k->lo, k->hi }, 2);
            jw_kfloat(&w, "param", k->param);
            jw_kbool(&w, "invert", k->invert);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    bool ok = write_text(b->path, &w);
    jw_free(&w);
    return ok;
}

Brain* ai_brain_load(const char* vpath) {
    uint32_t h = vpath_hash(vpath);
    for (uint32_t i = 0; i < S.nbrains; i++)
        if (S.brains[i]->hash == h) return S.brains[i];
    if (S.nbrains >= MAX_ASSETS) return NULL;
    char path[700];
    disk_of(vpath, path, sizeof path);
    size_t len;
    char* text = read_text(path, &len);
    if (!text) return NULL;
    JsonDoc doc;
    char err[160];
    Brain* b = NULL;
    if (json_parse(text, len, &doc, err, sizeof err)) {
        b = calloc(1, sizeof *b); // an asset, not a frame
        if (b) {
            snprintf(b->path, sizeof b->path, "%s", path);
            name_of(path, b->name);
            b->hash = h;
            b->nkeys = merge_perception(b->keys, read_keys(&doc.root, b->keys));
            b->momentum = (float)json_get_num(&doc.root, "momentum", 0.25);
            b->think_interval = (float)json_get_num(&doc.root, "think_interval", 0.25);
            const JsonValue* hs = json_get(&doc.root, "behaviors");
            for (size_t i = 0; i < json_len(hs) && b->nbehaviors < 16; i++) {
                const JsonValue* o = json_at(hs, i);
                BrainBehavior* x = &b->behaviors[b->nbehaviors++];
                memset(x, 0, sizeof *x);
                snprintf(x->name, AI_NAME, "%s", json_get_str(o, "name", "Behavior"));
                x->action = (uint8_t)json_get_int(o, "action", 0);
                if (x->action >= BRAIN_ACT_COUNT) x->action = 0;
                x->weight = (float)json_get_num(o, "weight", 1.0);
                x->speed = (float)json_get_num(o, "speed", 0.5);
                float pr[2] = { 0, 0 };
                json_get_floats(o, "param", pr, 2);
                x->param = pr[0], x->param2 = pr[1];
                snprintf(x->text, AI_NAME, "%s", json_get_str(o, "text", ""));
                const JsonValue* cs = json_get(o, "considerations");
                for (size_t c = 0; c < json_len(cs) && x->ncons < 4; c++) {
                    const JsonValue* q = json_at(cs, c);
                    Consideration* k = &x->cons[x->ncons++];
                    k->input = (uint8_t)json_get_int(q, "input", 0);
                    if (k->input >= BRAIN_IN_COUNT) k->input = 0;
                    k->curve = (uint8_t)json_get_int(q, "curve", 0);
                    if (k->curve >= CURVE_COUNT) k->curve = 0;
                    snprintf(k->key, AI_NAME, "%s", json_get_str(q, "key", ""));
                    float rg[2] = { 0, 1 };
                    json_get_floats(q, "range", rg, 2);
                    k->lo = rg[0], k->hi = rg[1];
                    k->param = (float)json_get_num(q, "param", 0.5);
                    k->invert = json_get_bool(q, "invert", false);
                }
            }
            S.brains[S.nbrains++] = b;
        }
        json_free(&doc);
    }
    free(text);
    return b;
}

static void unique_path(const char* folder, const char* name, const char* ext, char* vpath, size_t vcap, char* path, size_t pcap) {
    for (int i = 0; i < 1000; i++) {
        if (i) snprintf(vpath, vcap, "%s/%s%d.%s", folder, name, i, ext);
        else snprintf(vpath, vcap, "%s/%s.%s", folder, name, ext);
        disk_of(vpath, path, pcap);
        if (!re_file_stat(path, NULL, NULL, NULL)) return;
    }
}

static void add_key(BbKey* k, uint32_t* n, const char* name, uint8_t type) {
    if (*n >= AI_MAX_KEYS) return;
    snprintf(k[*n].name, AI_NAME, "%s", name);
    k[(*n)++].type = type;
}
// The keys perception writes (an agent's Blackboard gets them all)
static void perception_keys(BbKey* k, uint32_t* n) {
    add_key(k, n, "TargetActor", BB_ACTOR);
    add_key(k, n, "CanSeeTarget", BB_BOOL);
    add_key(k, n, "LastKnownLocation", BB_VECTOR);
    add_key(k, n, "DistanceToTarget", BB_FLOAT);
    add_key(k, n, "HeardNoise", BB_BOOL);
    add_key(k, n, "NoiseLocation", BB_VECTOR);
    add_key(k, n, "SelfLocation", BB_VECTOR);
    add_key(k, n, "PredictedLocation", BB_VECTOR);
    add_key(k, n, "FlankLocation", BB_VECTOR);
}
// An asset from before a perception key existed: the missing ones added (its own keys kept)
static uint32_t merge_perception(BbKey* k, uint32_t n) {
    BbKey p[AI_MAX_KEYS];
    uint32_t np = 0;
    perception_keys(p, &np);
    for (uint32_t i = 0; i < np; i++) {
        bool have = false;
        for (uint32_t j = 0; j < n && !have; j++) have = !strcmp(k[j].name, p[i].name);
        if (!have && n < AI_MAX_KEYS) k[n++] = p[i];
    }
    return n;
}

BehaviorTree* ai_tree_create(const char* folder_vpath, const char* name) {
    if (S.ntrees >= MAX_ASSETS) return NULL;
    BehaviorTree* t = calloc(1, sizeof *t);
    if (!t) return NULL;
    char vpath[700];
    unique_path(folder_vpath, name, "rbt", vpath, sizeof vpath, t->path, sizeof t->path);
    name_of(t->path, t->name);
    t->hash = vpath_hash(vpath);
    perception_keys(t->keys, &t->nkeys);
    add_key(t->keys, &t->nkeys, "Destination", BB_VECTOR);
    t->nodes[t->nnodes++] = (BtNode){ .kind = BT_ROOT, .parent = -1, .x = 0, .y = 0 };
    if (!ai_tree_save(t)) {
        free(t);
        return NULL;
    }
    S.trees[S.ntrees++] = t;
    return t;
}

Brain* ai_brain_create(const char* folder_vpath, const char* name) {
    if (S.nbrains >= MAX_ASSETS) return NULL;
    Brain* b = calloc(1, sizeof *b);
    if (!b) return NULL;
    char vpath[700];
    unique_path(folder_vpath, name, "rbrain", vpath, sizeof vpath, b->path, sizeof b->path);
    name_of(b->path, b->name);
    b->hash = vpath_hash(vpath);
    perception_keys(b->keys, &b->nkeys);
    b->momentum = 0.25f, b->think_interval = 0.25f;
    BrainBehavior* idle = &b->behaviors[b->nbehaviors++];
    memset(idle, 0, sizeof *idle);
    snprintf(idle->name, AI_NAME, "Idle");
    idle->action = BRAIN_ACT_IDLE, idle->weight = 0.2f;
    if (!ai_brain_save(b)) {
        free(b);
        return NULL;
    }
    S.brains[S.nbrains++] = b;
    return b;
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
    const char* dot = strrchr(e->name, '.');
    if (!dot) return;
    vpath_of(path, vpath, sizeof vpath);
    if (!strcmp(dot, ".rbt")) ai_tree_load(vpath);
    else if (!strcmp(dot, ".rbrain")) ai_brain_load(vpath);
}
static void scan_dir(const char* disk, int depth) {
    if (depth <= 12) re_dir_walk(disk, scan_entry, (void*)disk);
}
static void scan(void) {
    if (!S.scanned) S.scanned = true, scan_dir(S.root, 0);
}

BehaviorTree* ai_tree(uint32_t hash) {
    scan();
    for (uint32_t i = 0; i < S.ntrees; i++)
        if (S.trees[i]->hash == hash) return S.trees[i];
    return NULL;
}
Brain* ai_brain(uint32_t hash) {
    scan();
    for (uint32_t i = 0; i < S.nbrains; i++)
        if (S.brains[i]->hash == hash) return S.brains[i];
    return NULL;
}
BehaviorTree* ai_tree_name(const char* name) {
    scan();
    for (uint32_t i = 0; i < S.ntrees; i++)
        if (!strcmp(S.trees[i]->name, name)) return S.trees[i];
    return NULL;
}
uint32_t ai_tree_count(void) { return scan(), S.ntrees; }
BehaviorTree* ai_tree_at(uint32_t i) { return i < S.ntrees ? S.trees[i] : NULL; }
uint32_t ai_brain_count(void) { return scan(), S.nbrains; }
Brain* ai_brain_at(uint32_t i) { return i < S.nbrains ? S.brains[i] : NULL; }

// The examples: a tree that chases what it sees and wanders otherwise; a guard brain that patrols, investigates noises,
// chases, keeps its distance when it can't win
static BtNode* bt_add(BehaviorTree* t, uint8_t kind, int parent, float x, float y) {
    BtNode* n = &t->nodes[t->nnodes++];
    memset(n, 0, sizeof *n);
    n->kind = kind, n->parent = (int16_t)parent, n->x = x, n->y = y;
    return n;
}

// (2026-11-25, item 21) the example Brains' behaviour names (data: the user renames them) in the editor's language when
// they are made; the actions and inputs show through TR anyway
static void example_names(Brain* b) {
    for (uint32_t i = 0; i < b->nbehaviors; i++) {
        char t[AI_NAME];
        snprintf(t, sizeof t, "%s", TR(b->behaviors[i].name));
        snprintf(b->behaviors[i].name, sizeof b->behaviors[i].name, "%s", t);
    }
}

static void make_defaults(void) {
    char dir[700], path[800];
    snprintf(dir, sizeof dir, "%s/AI", S.root);
    snprintf(path, sizeof path, "%s/BT_Chase.rbt", dir);
    if (!re_file_stat(path, NULL, NULL, NULL) && re_make_dirs(dir)) {
        BehaviorTree* t = ai_tree_create("/Content/AI", "BT_Chase");
        if (t) {
            BtNode* sel = bt_add(t, BT_SELECTOR, 0, 0, 120);
            (void)sel;
            BtNode* chase = bt_add(t, BT_SEQUENCE, 1, -220, 240);
            chase->ndecos = 1;
            chase->decos[0] = (BtDeco){ BTD_BLACKBOARD, BB_IS_SET, BTA_BOTH, false, "CanSeeTarget", 0 };
            snprintf(chase->comment, sizeof chase->comment, "Sees someone: run after them");
            BtNode* mv = bt_add(t, BT_MOVE_TO, 2, -220, 360);
            snprintf(mv->key, AI_NAME, "TargetActor");
            mv->value = 90.0f;
            mv->nsvcs = 1, mv->svcs[0] = (BtService){ BTS_SPEED, "", 1.0f };
            BtNode* wander = bt_add(t, BT_SEQUENCE, 1, 220, 240);
            snprintf(wander->comment, sizeof wander->comment, "Otherwise: walk around");
            BtNode* fr = bt_add(t, BT_FIND_RANDOM, 4, 60, 360);
            snprintf(fr->key, AI_NAME, "Destination");
            fr->value = 700.0f;
            BtNode* m2 = bt_add(t, BT_MOVE_TO, 4, 220, 360);
            snprintf(m2->key, AI_NAME, "Destination");
            m2->value = 60.0f;
            m2->nsvcs = 1, m2->svcs[0] = (BtService){ BTS_SPEED, "", 0.35f };
            BtNode* wt = bt_add(t, BT_WAIT, 4, 380, 360);
            wt->value = 1.5f, wt->value2 = 0.5f;
            ai_tree_save(t);
        }
    }
    snprintf(path, sizeof path, "%s/Brain_Guard.rbrain", dir);
    if (!re_file_stat(path, NULL, NULL, NULL) && re_make_dirs(dir)) {
        Brain* b = ai_brain_create("/Content/AI", "Brain_Guard");
        if (b) {
            b->nbehaviors = 0;
            BrainBehavior* p = &b->behaviors[b->nbehaviors++];
            *p = (BrainBehavior){ "Patrol", BRAIN_ACT_PATROL, 1, 0.5f, 0.3f, 600.0f, 0.0f, "", { { 0 } } };
            p->cons[0] = (Consideration){ BRAIN_IN_HAS_TARGET, CURVE_LINEAR, "", 0, 1, 0, true };
            BrainBehavior* c = &b->behaviors[b->nbehaviors++];
            *c = (BrainBehavior){ "Chase", BRAIN_ACT_CHASE, 2, 1.0f, 1.0f, 0.0f, 0.0f, "", { { 0 } } };
            c->cons[0] = (Consideration){ BRAIN_IN_SEES_TARGET, CURVE_STEP, "", 0, 1, 0.5f, false };
            c->cons[1] = (Consideration){ BRAIN_IN_DISTANCE, CURVE_INVERSE, "", 0, 2500, 0, false };
            BrainBehavior* inv = &b->behaviors[b->nbehaviors++];
            *inv = (BrainBehavior){ "Investigate", BRAIN_ACT_INVESTIGATE, 2, 0.8f, 0.5f, 0.0f, 0.0f, "", { { 0 } } };
            inv->cons[0] = (Consideration){ BRAIN_IN_TIME_SINCE_SEEN, CURVE_INVERSE, "", 0, 10, 0, false };
            inv->cons[1] = (Consideration){ BRAIN_IN_HAS_TARGET, CURVE_LINEAR, "", 0, 1, 0, false };
            BrainBehavior* hear = &b->behaviors[b->nbehaviors++];
            *hear = (BrainBehavior){ "Check Noise", BRAIN_ACT_INVESTIGATE, 1, 0.7f, 0.45f, 0.0f, 0.0f, "", { { 0 } } };
            hear->cons[0] = (Consideration){ BRAIN_IN_HEARD_NOISE, CURVE_STEP, "", 0, 1, 0.5f, false };
            BrainBehavior* idle = &b->behaviors[b->nbehaviors++];
            *idle = (BrainBehavior){ "Rest", BRAIN_ACT_IDLE, 0, 0.1f, 0.0f, 0.0f, 0.0f, "", { { 0 } } };
            example_names(b); // (item 21: the example's behaviours named in the editor's language)
            ai_brain_save(b);
        }
    }
    // a squad's member: far, it cuts the target off (Intercept); near, its squad closes a ring (Surround); in the ring,
    // it attacks (the pawn's "Attack" event) from its side
    snprintf(path, sizeof path, "%s/Brain_Squad.rbrain", dir);
    if (!re_file_stat(path, NULL, NULL, NULL) && re_make_dirs(dir)) {
        Brain* b = ai_brain_create("/Content/AI", "Brain_Squad");
        if (b) {
            b->nbehaviors = 0;
            BrainBehavior* x = &b->behaviors[b->nbehaviors++];
            *x = (BrainBehavior){ "Search", BRAIN_ACT_WANDER, 1, 0.3f, 0.35f, 900.0f, 0.0f, "", { { 0 } } };
            x->cons[0] = (Consideration){ BRAIN_IN_HAS_TARGET, CURVE_LINEAR, "", 0, 1, 0, true };
            x = &b->behaviors[b->nbehaviors++];
            *x = (BrainBehavior){ "Intercept", BRAIN_ACT_INTERCEPT, 2, 1.0f, 1.0f, 0.0f, 0.0f, "", { { 0 } } };
            x->cons[0] = (Consideration){ BRAIN_IN_HAS_TARGET, CURVE_STEP, "", 0, 1, 0.5f, false };
            x->cons[1] = (Consideration){ BRAIN_IN_DISTANCE, CURVE_STEP, "", 0, 3000, 0.3f, false }; // (over 9 m)
            x = &b->behaviors[b->nbehaviors++];
            *x = (BrainBehavior){ "Surround", BRAIN_ACT_SURROUND, 2, 1.0f, 0.9f, 0.0f, 0.0f, "", { { 0 } } };
            x->cons[0] = (Consideration){ BRAIN_IN_HAS_TARGET, CURVE_STEP, "", 0, 1, 0.5f, false };
            x->cons[1] = (Consideration){ BRAIN_IN_DISTANCE, CURVE_STEP, "", 0, 3000, 0.3f, true }; // (under 9 m)
            x = &b->behaviors[b->nbehaviors++];
            *x = (BrainBehavior){ "Attack", BRAIN_ACT_ATTACK, 2, 2.0f, 0.6f, 420.0f, 1.2f, "Attack", { { 0 } } };
            x->cons[0] = (Consideration){ BRAIN_IN_HAS_TARGET, CURVE_STEP, "", 0, 1, 0.5f, false };
            x->cons[1] = (Consideration){ BRAIN_IN_DISTANCE, CURVE_STEP, "", 0, 1000, 0.42f, true }; // (under 4.2 m)
            b->momentum = 0.15f;
            example_names(b); // (item 21: the example's behaviours named in the editor's language; "Attack", the event, stays)
            ai_brain_save(b);
        }
    }
}

void ai_init(const char* content_root, bool defaults) {
    snprintf(S.root, sizeof S.root, "%s", content_root);
    for (char* c = S.root; *c; c++)
        if (*c == '\\') *c = '/';
    S.scanned = false;
    if (defaults) make_defaults();
}

float ai_curve(uint8_t curve, float x, float p) {
    x = fminf(fmaxf(x, 0.0f), 1.0f);
    switch (curve) {
    case CURVE_INVERSE: return 1.0f - x;
    case CURVE_STEP: return x >= p ? 1.0f : 0.0f;
    case CURVE_SMOOTH: return x * x * (3.0f - 2.0f * x);
    case CURVE_BELL: {
        float w = p > 0.01f ? p : 0.25f, d = (x - 0.5f) / w;
        return expf(-d * d);
    }
    default: return x;
    }
}

// --- Navigation grid ---

static struct {
    float origin[2], cell;
    uint32_t w, h, walkable;
    float* height;   // the floor per cell (< -1e29: none)
    double ms;
    NavInfo info;
    // A*'s arrays (made with the grid)
    float* g;
    int32_t* from;
    uint32_t* heap;
    uint32_t* stamp;
    uint32_t search;
} N;

typedef struct NBox { float mn[3], mx[3]; } NBox;

static bool cell_ok(int x, int y) { return x >= 0 && y >= 0 && (uint32_t)x < N.w && (uint32_t)y < N.h && N.height[(uint32_t)y * N.w + (uint32_t)x] > -1e29f; }
static float cell_h(int x, int y) { return N.height[(uint32_t)y * N.w + (uint32_t)x]; }
static bool step_ok(int x0, int y0, int x1, int y1) { return cell_ok(x0, y0) && cell_ok(x1, y1) && fabsf(cell_h(x0, y0) - cell_h(x1, y1)) <= STEP_H; }

bool ai_nav_build(World* w) {
    double t0 = re_time_seconds();
    free(N.height), free(N.g), free(N.from), free(N.heap), free(N.stamp);
    memset(&N, 0, sizeof N);
    // the level's solid boxes (meshes not simulating, not a Character's, not an animated part)
    uint32_t nb = 0, cap = 0;
    NBox* boxes = NULL;
    float mn[2] = { 1e30f, 1e30f }, mx[2] = { -1e30f, -1e30f };
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need || (a->mask & (COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_AI_AGENT)))) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
        const CRigidBody* rb = (a->mask & COMP_BIT(COMP_RIGID_BODY)) ? ecs_arch_column(a, COMP_RIGID_BODY) : NULL;
        const CAttach* pat = (a->mask & COMP_BIT(COMP_ATTACH)) ? ecs_arch_column(a, COMP_ATTACH) : NULL; // (a component: only a prototype's boxes)
        for (uint32_t i = 0; i < a->count; i++) {
            if ((rb && rb[i].simulate) || (tr[i].flags & TRANSFORM_HIDDEN_GAME) || (pat && pat[i].kind != ATTACH_PROTO)) continue;
            if (nb == cap) {
                cap = cap ? cap * 2 : 1024;
                NBox* nbx = realloc(boxes, sizeof(NBox) * cap); // a build, not a frame
                if (!nbx) break;
                boxes = nbx;
            }
            float lmn[3], lmx[3], rows[3][4];
            render_mesh_bounds(sm[i].mesh, lmn, lmx);
            render_transform_rows(&tr[i], rows);
            NBox* b = &boxes[nb++];
            for (int k = 0; k < 3; k++) {
                float c = rows[k][3], e = 0.0f;
                for (int j = 0; j < 3; j++) c += rows[k][j] * (lmn[j] + lmx[j]) * 0.5f, e += fabsf(rows[k][j]) * (lmx[j] - lmn[j]) * 0.5f;
                b->mn[k] = c - fmaxf(e, 0.5f), b->mx[k] = c + fmaxf(e, 0.5f);
            }
            for (int k = 0; k < 2; k++) mn[k] = fminf(mn[k], b->mn[k]), mx[k] = fmaxf(mx[k], b->mx[k]);
        }
    }
    float lmn[2], lmx[2];
    bool land = landscape_bounds(lmn, lmx); // (the landscapes' ground too)
    if (land)
        for (int k = 0; k < 2; k++) mn[k] = fminf(mn[k], lmn[k]), mx[k] = fmaxf(mx[k], lmx[k]);
    if (!nb && !land) {
        free(boxes);
        return false;
    }
    // 25 cm cells, coarser for big levels (at most ~2M cells)
    float cell = 25.0f;
    while ((mx[0] - mn[0]) / cell * ((mx[1] - mn[1]) / cell) > 2.0e6f) cell *= 2.0f;
    N.cell = cell, N.origin[0] = mn[0], N.origin[1] = mn[1];
    N.w = (uint32_t)ceilf((mx[0] - mn[0]) / cell) + 1u, N.h = (uint32_t)ceilf((mx[1] - mn[1]) / cell) + 1u;
    size_t nc = (size_t)N.w * N.h;
    N.height = malloc(sizeof(float) * nc);
    float* spans = malloc(sizeof(float) * 2 * MAX_SPANS * nc); // the solid spans per cell [bottom, top]
    uint8_t* nspan = calloc(nc, 1);
    if (!N.height || !spans || !nspan) {
        free(boxes), free(spans), free(nspan), free(N.height), N.height = NULL;
        return false;
    }
    // every box into the cells under it, merged with the spans it touches
    for (uint32_t i = 0; i < nb; i++) {
        const NBox* b = &boxes[i];
        int x0 = (int)floorf((b->mn[0] - mn[0]) / cell), x1 = (int)floorf((b->mx[0] - mn[0]) / cell);
        int y0 = (int)floorf((b->mn[1] - mn[1]) / cell), y1 = (int)floorf((b->mx[1] - mn[1]) / cell);
        for (int y = y0 < 0 ? 0 : y0; y <= y1 && (uint32_t)y < N.h; y++)
            for (int x = x0 < 0 ? 0 : x0; x <= x1 && (uint32_t)x < N.w; x++) {
                // only the cells whose middle the box covers
                float cx = mn[0] + ((float)x + 0.5f) * cell, cy = mn[1] + ((float)y + 0.5f) * cell;
                if (cx < b->mn[0] || cx > b->mx[0] || cy < b->mn[1] || cy > b->mx[1]) continue;
                size_t c = (size_t)y * N.w + (size_t)x;
                float lo = b->mn[2], hi = b->mx[2];
                float* sp = &spans[c * 2 * MAX_SPANS];
                uint32_t n = nspan[c], k = 0;
                for (uint32_t j = 0; j < n; j++) { // merge every span it touches into it
                    if (sp[j * 2] <= hi + 1.0f && sp[j * 2 + 1] >= lo - 1.0f) {
                        lo = fminf(lo, sp[j * 2]), hi = fmaxf(hi, sp[j * 2 + 1]);
                        continue;
                    }
                    sp[k * 2] = sp[j * 2], sp[k * 2 + 1] = sp[j * 2 + 1], k++;
                }
                if (k < MAX_SPANS) sp[k * 2] = lo, sp[k * 2 + 1] = hi, k++;
                nspan[c] = (uint8_t)k;
            }
    }
    // the landscapes: a span from far below up to the ground at each cell's middle (merged with the boxes' there)
    for (uint32_t y = 0; land && y < N.h; y++)
        for (uint32_t x = 0; x < N.w; x++) {
            float cx = mn[0] + ((float)x + 0.5f) * cell, cy = mn[1] + ((float)y + 0.5f) * cell, gz;
            if (!landscape_height(cx, cy, &gz)) continue;
            size_t c = (size_t)y * N.w + (size_t)x;
            float lo = gz - 2000.0f, hi = gz, *sp = &spans[c * 2 * MAX_SPANS];
            uint32_t n = nspan[c], k = 0;
            for (uint32_t j = 0; j < n; j++) {
                if (sp[j * 2] <= hi + 1.0f && sp[j * 2 + 1] >= lo - 1.0f) {
                    lo = fminf(lo, sp[j * 2]), hi = fmaxf(hi, sp[j * 2 + 1]);
                    continue;
                }
                sp[k * 2] = sp[j * 2], sp[k * 2 + 1] = sp[j * 2 + 1], k++;
            }
            if (k < MAX_SPANS) sp[k * 2] = lo, sp[k * 2 + 1] = hi, k++;
            nspan[c] = (uint8_t)k;
        }
    // the floor: the lowest top with head room above it
    for (size_t c = 0; c < nc; c++) {
        N.height[c] = -1e30f;
        const float* sp = &spans[c * 2 * MAX_SPANS];
        float best = 1e30f;
        for (uint32_t j = 0; j < nspan[c]; j++) {
            float top = sp[j * 2 + 1], above = 1e30f;
            for (uint32_t k = 0; k < nspan[c]; k++)
                if (sp[k * 2] > top && sp[k * 2] < above) above = sp[k * 2];
            if (above - top >= CLEARANCE && top < best) best = top;
        }
        if (best < 1e29f) N.height[c] = best;
    }
    // a walker's width: a cell next to a wall (a neighbor too high or missing) is left out
    uint8_t* near = calloc(nc, 1);
    if (near) {
        for (uint32_t y = 0; y < N.h; y++)
            for (uint32_t x = 0; x < N.w; x++) {
                size_t c = (size_t)y * N.w + x;
                if (N.height[c] < -1e29f) continue;
                for (int dy = -1; dy <= 1 && !near[c]; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = (int)x + dx, ny = (int)y + dy;
                        if (nx < 0 || ny < 0 || (uint32_t)nx >= N.w || (uint32_t)ny >= N.h) continue; // (the level's edge)
                        float hn = N.height[(size_t)ny * N.w + (size_t)nx];
                        if (hn < -1e29f || hn - N.height[c] > STEP_H) {
                            near[c] = 1;
                            break;
                        }
                    }
            }
        for (size_t c = 0; c < nc; c++)
            if (near[c]) N.height[c] = -1e30f;
        free(near);
    }
    for (size_t c = 0; c < nc; c++) N.walkable += N.height[c] > -1e29f;
    free(boxes), free(spans), free(nspan);
    N.g = malloc(sizeof(float) * nc);
    N.from = malloc(sizeof(int32_t) * nc);
    N.heap = malloc(sizeof(uint32_t) * nc);
    N.stamp = calloc(nc, sizeof(uint32_t));
    N.ms = (re_time_seconds() - t0) * 1000.0;
    N.info = (NavInfo){ { N.origin[0], N.origin[1] }, N.cell, N.w, N.h, N.walkable, N.ms, N.height };
    return N.g && N.from && N.heap && N.stamp && N.walkable;
}

const NavInfo* ai_nav_info(void) { return &N.info; }

static bool to_cell(const float p[3], int* x, int* y) {
    if (!N.height) return false;
    *x = (int)floorf((p[0] - N.origin[0]) / N.cell), *y = (int)floorf((p[1] - N.origin[1]) / N.cell);
    if (cell_ok(*x, *y)) return true;
    // the nearest walkable cell within 1.5 m (a goal on a wall's edge, an actor in the eroded band)
    int r = (int)ceilf(150.0f / N.cell), bx = 0, by = 0;
    float bd = 1e30f;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (cell_ok(*x + dx, *y + dy) && (float)(dx * dx + dy * dy) < bd) bd = (float)(dx * dx + dy * dy), bx = *x + dx, by = *y + dy;
    if (bd > 1e29f) return false;
    *x = bx, *y = by;
    return true;
}

static void cell_center(int x, int y, float out[3]) {
    out[0] = N.origin[0] + ((float)x + 0.5f) * N.cell;
    out[1] = N.origin[1] + ((float)y + 0.5f) * N.cell;
    out[2] = cell_h(x, y);
}

// A straight walk between two cells (the path's shortcuts): every cell on the line walkable, no step too high
static bool line_walkable(int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    int px = x0, py = y0;
    for (int guard = 0; guard < 100000; guard++) {
        if (!step_ok(px, py, x0, y0)) return false;
        px = x0, py = y0;
        if (x0 == x1 && y0 == y1) return true;
        int e2 = 2 * e;
        if (e2 >= dy) e += dy, x0 += sx;
        if (e2 <= dx) e += dx, y0 += sy;
    }
    return false;
}


static bool grid_path(const float from[3], const float to[3], float (*out)[3], uint32_t cap, uint32_t* n) {
    *n = 0;
    int sx, sy, gx, gy;
    if (!N.height || !to_cell(from, &sx, &sy) || !to_cell(to, &gx, &gy)) return false;
    if (line_walkable(sx, sy, gx, gy)) { // straight there
        cell_center(gx, gy, out[0]);
        out[0][0] = to[0], out[0][1] = to[1];
        *n = 1;
        return true;
    }
    // A* over 8 neighbors; the open set a binary heap ordered by f = g + h (kept in a parallel array)
    static float* f = NULL;
    static size_t fcap = 0;
    size_t nc = (size_t)N.w * N.h;
    if (fcap < nc) {
        free(f);
        f = malloc(sizeof(float) * nc); // (once per grid size)
        fcap = f ? nc : 0;
        if (!f) return false;
    }
    if (++N.search == 0) memset(N.stamp, 0, sizeof(uint32_t) * nc), N.search = 1;
    uint32_t start = (uint32_t)sy * N.w + (uint32_t)sx, goal = (uint32_t)gy * N.w + (uint32_t)gx, heap_n = 0;
#define H(c) (N.cell * sqrtf((float)(((int)((c) % N.w) - gx) * ((int)((c) % N.w) - gx) + ((int)((c) / N.w) - gy) * ((int)((c) / N.w) - gy))))
#define PUSH(c)                                                                                                         \
    do {                                                                                                                \
        uint32_t i_ = heap_n++;                                                                                         \
        N.heap[i_] = (c);                                                                                               \
        while (i_ > 0 && f[N.heap[(i_ - 1) / 2]] > f[N.heap[i_]]) {                                                     \
            uint32_t p_ = (i_ - 1) / 2, t_ = N.heap[p_];                                                                \
            N.heap[p_] = N.heap[i_], N.heap[i_] = t_, i_ = p_;                                                          \
        }                                                                                                               \
    } while (0)
    N.stamp[start] = N.search, N.g[start] = 0.0f, N.from[start] = -1, f[start] = H(start);
    PUSH(start);
    bool found = false;
    uint32_t expanded = 0;
    while (heap_n && expanded < 400000u) {
        uint32_t c = N.heap[0];
        N.heap[0] = N.heap[--heap_n];
        for (uint32_t i = 0;;) { // sift down
            uint32_t l = i * 2 + 1, r = l + 1, m = i;
            if (l < heap_n && f[N.heap[l]] < f[N.heap[m]]) m = l;
            if (r < heap_n && f[N.heap[r]] < f[N.heap[m]]) m = r;
            if (m == i) break;
            uint32_t t = N.heap[m];
            N.heap[m] = N.heap[i], N.heap[i] = t, i = m;
        }
        if (c == goal) {
            found = true;
            break;
        }
        expanded++;
        int cx = (int)(c % N.w), cy = (int)(c / N.w);
        for (int k = 0; k < 8; k++) {
            static const int ox[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, oy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
            int nx = cx + ox[k], ny = cy + oy[k];
            if (!step_ok(cx, cy, nx, ny)) continue;
            if (k >= 4 && (!step_ok(cx, cy, nx, cy) || !step_ok(cx, cy, cx, ny))) continue; // no corner cutting
            uint32_t nn = (uint32_t)ny * N.w + (uint32_t)nx;
            float ng = N.g[c] + N.cell * (k >= 4 ? 1.41421356f : 1.0f);
            if (N.stamp[nn] == N.search && N.g[nn] <= ng) continue;
            N.stamp[nn] = N.search, N.g[nn] = ng, N.from[nn] = (int32_t)c, f[nn] = ng + H(nn);
            PUSH(nn);
        }
    }
#undef PUSH
#undef H
    if (!found) return false;
    // the cells back from the goal, then only the corners a straight walk can't skip (string pulling)
    static uint32_t* chain = NULL;
    static size_t chain_cap = 0;
    uint32_t len = 0;
    for (int32_t c = (int32_t)goal; c >= 0; c = N.from[c]) len++;
    if (chain_cap < len) {
        free(chain);
        chain = malloc(sizeof(uint32_t) * len); // (a long path: once)
        chain_cap = chain ? len : 0;
        if (!chain) return false;
    }
    uint32_t k = len;
    for (int32_t c = (int32_t)goal; c >= 0; c = N.from[c]) chain[--k] = (uint32_t)c;
    uint32_t at = 0;
    while (at + 1 < len && *n < cap) {
        uint32_t far = at + 1;
        int ax = (int)(chain[at] % N.w), ay = (int)(chain[at] / N.w);
        for (uint32_t j = len - 1; j > at + 1; j--) {
            if (line_walkable(ax, ay, (int)(chain[j] % N.w), (int)(chain[j] / N.w))) {
                far = j;
                break;
            }
        }
        cell_center((int)(chain[far] % N.w), (int)(chain[far] / N.w), out[(*n)++]);
        at = far;
    }
    if (*n) out[*n - 1][0] = to[0], out[*n - 1][1] = to[1];
    return *n > 0;
}

static uint32_t g_rng = 0x2545F491u;
static float rnd01(void) {
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 17, g_rng ^= g_rng << 5;
    return (float)(g_rng & 0xFFFFFF) / 16777216.0f;
}

// The NavMesh once it is in (navmesh.h); the grid while it is built, or where it is not (a Procedural World)
bool ai_nav_path_ex(const float from[3], const float to[3], float (*out)[3], uint8_t* flags, uint32_t cap, uint32_t* n) {
    if (navmesh_ready() && navmesh_path(from, to, out, flags, cap, n)) return true;
    if (flags) memset(flags, 0, cap);
    return grid_path(from, to, out, cap, n);
}
bool ai_nav_path(const float from[3], const float to[3], float (*out)[3], uint32_t cap, uint32_t* n) { return ai_nav_path_ex(from, to, out, NULL, cap, n); }

bool ai_nav_random_point(const float around[3], float radius, float out[3]) {
    if (navmesh_ready() && navmesh_random_point(around, radius, out)) return true;
    int sx, sy;
    if (!N.height || !to_cell(around, &sx, &sy)) return false;
    for (int tries = 0; tries < 24; tries++) {
        float a = rnd01() * 6.2831853f, r = sqrtf(rnd01()) * radius;
        float p[3] = { around[0] + cosf(a) * r, around[1] + sinf(a) * r, around[2] };
        int x = (int)floorf((p[0] - N.origin[0]) / N.cell), y = (int)floorf((p[1] - N.origin[1]) / N.cell);
        if (cell_ok(x, y) && fabsf(cell_h(x, y) - cell_h(sx, sy)) < 200.0f) {
            p[2] = cell_h(x, y);
            memcpy(out, p, 12);
            return true;
        }
    }
    return false;
}

bool ai_nav_project(const float p[3], const float extent[3], float out[3]) {
    if (navmesh_ready()) return navmesh_project(p, extent, out);
    int x, y;
    if (!N.height || !to_cell(p, &x, &y)) return false;
    cell_center(x, y, out);
    return fabsf(out[0] - p[0]) <= extent[0] + N.cell && fabsf(out[1] - p[1]) <= extent[1] + N.cell && fabsf(out[2] - p[2]) <= extent[2];
}

float ai_nav_path_length(const float from[3], const float to[3]) {
    float pts[MAX_PATH][3];
    uint32_t n = 0;
    if (!ai_nav_path(from, to, pts, MAX_PATH, &n) || !n) return -1.0f;
    float len = 0.0f;
    const float* prev = from;
    for (uint32_t i = 0; i < n; i++) {
        float d[3] = { pts[i][0] - prev[0], pts[i][1] - prev[1], pts[i][2] - prev[2] };
        len += sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        prev = pts[i];
    }
    return len;
}

// --- The agents ---

typedef struct BbValue { float v[4]; bool set; } BbValue;

typedef struct Agent {
    Entity pawn;
    uint32_t mode;
    BehaviorTree* tree;
    Brain* brain;
    BbKey keys[AI_MAX_KEYS * 2];
    BbValue bb[AI_MAX_KEYS * 2];
    uint32_t nkeys;
    // perception
    Entity target;
    bool sees, heard;
    float last_seen, heard_at, noise[3], last_known[3];
    float sense_timer;
    float spot_t;              // s it has been seeing a new target (the reaction)
    float tvel[2], vel_from[3], vel_t; // the target's velocity (estimated from what it saw)
    float predicted[3], flank[3];      // where the target will be; its slot in the squad's ring
    uint32_t squad;            // its team's agents after the same target (itself included)
    bool flanks;               // the squad sends it round (coordination), not straight
    // movement
    float path[MAX_PATH][3];
    uint8_t pflags[MAX_PATH];  // NAV_PT_ (navmesh.h): where a Nav Link starts
    uint32_t npath, ipath;
    uint8_t link;              // crossing a Nav Link: 1 jumping
    float link_t, link_len, link_in, link_dir[2], link_to[3];
    float goal[3], accept, speed;
    bool moving;
    Entity goal_actor;
    float repath;
    // the tree
    int16_t running;           // the task running (-1: none)
    float task_t, task_len;
    uint8_t idx[AI_MAX_NODES];  // each composite's child in progress
    uint16_t loops[AI_MAX_NODES];
    float cool_until[AI_MAX_NODES];
    int16_t kids[AI_MAX_NODES][8]; // the children, left to right
    uint8_t nkids[AI_MAX_NODES];
    float bt_check;
    BehaviorTree* sub_tree;    // a Brain's Run Behavior Tree
    // the brain
    int active;
    float scores[16];
    float think, since_change;
    float home[3];
    uint32_t patrol_i;
    float attack_at;
    bool entered;
    const char* what;
    float yaw_goal;
    bool face;
    float face_at[3];
    float damaged_at;          // when the damage sense last told it who hit it (it turns to them a moment)
} Agent;

static struct {
    bool playing;
    Agent* agents;
    uint32_t nagents;
    struct { float at[3], loud, t; Entity who; } noises[MAX_NOISES];
    uint32_t nnoises;
    float time;
    AiStats stats;
    bool nav_ok;
} R;

// Local avoidance (Unreal's RVO avoidance, Q24): every frame the pawns' places and velocities in a coarse hash (no
// allocation: static arrays); an agent heading into another within AVOID_HORIZON s turns aside (away from where they
// would meet; head-on, each to its own right, so they pass), and one already touching is pushed apart. Its goal actor
// and its target are not avoided (it is going to them). The player's pawn is in the hash too (agents go round it).
#define AVOID_CELL 300.0f    // cm: the hash's cells (3 x 3 of them cover AVOID_LOOK)
#define AVOID_LOOK 280.0f    // cm: the others it looks at
#define AVOID_HORIZON 1.2f   // s: how far ahead a meeting matters
#define AVOID_HASH 1024
static struct {
    bool off, init;
    uint32_t n;              // R.nagents + 1 (the player's slot); 0: off this frame
    int32_t head[AVOID_HASH], next[MAX_AGENTS + 1];
    Entity e[MAX_AGENTS + 1];
    float p[MAX_AGENTS + 1][2], v[MAX_AGENTS + 1][2], r[MAX_AGENTS + 1];
} V;

void ai_set_avoidance(bool on) { V.off = !on, V.init = true; }
bool ai_avoidance(void) {
    if (!V.init) {
        const char* e = getenv("REALENGINE_AI_AVOID");
        V.off = e && e[0] == '0', V.init = true;
    }
    return !V.off;
}

static int key_index(const Agent* a, const char* name) {
    for (uint32_t i = 0; i < a->nkeys; i++)
        if (!strcmp(a->keys[i].name, name)) return (int)i;
    return -1;
}
static void bb_set(Agent* a, const char* name, const float* v, int n) {
    int k = key_index(a, name);
    if (k < 0) return;
    memset(a->bb[k].v, 0, sizeof a->bb[k].v);
    memcpy(a->bb[k].v, v, sizeof(float) * (size_t)n);
    a->bb[k].set = true;
    if (a->keys[k].type == BB_BOOL) a->bb[k].set = v[0] != 0.0f;
    if (a->keys[k].type == BB_ACTOR) { // (an entity: its bits in the float, as the VM keeps actors)
        uint32_t u;
        memcpy(&u, &v[0], 4);
        a->bb[k].set = u != 0u;
    }
}
static void bb_clear(Agent* a, const char* name) {
    int k = key_index(a, name);
    if (k >= 0) memset(&a->bb[k], 0, sizeof a->bb[k]);
}
static void add_keys(Agent* a, const BbKey* k, uint32_t n) {
    for (uint32_t i = 0; i < n && a->nkeys < AI_MAX_KEYS * 2; i++)
        if (key_index(a, k[i].name) < 0) a->keys[a->nkeys++] = k[i];
}

static void prepare_tree(Agent* a, const BehaviorTree* t) {
    memset(a->nkids, 0, sizeof a->nkids);
    memset(a->idx, 0, sizeof a->idx);
    memset(a->loops, 0, sizeof a->loops);
    memset(a->cool_until, 0, sizeof a->cool_until);
    a->running = -1;
    if (!t) return;
    add_keys(a, t->keys, t->nkeys);
    for (uint32_t i = 1; i < t->nnodes; i++) {
        if (t->nodes[i].parent == BT_DETACHED) continue; // (not wired: the editor's loose node)
        int p = t->nodes[i].parent < 0 ? 0 : t->nodes[i].parent;
        if (a->nkids[p] >= 8) continue;
        // left to right (Unreal's order)
        uint8_t k = a->nkids[p]++;
        while (k > 0 && t->nodes[a->kids[p][k - 1]].x > t->nodes[i].x) a->kids[p][k] = a->kids[p][k - 1], k--;
        a->kids[p][k] = (int16_t)i;
    }
}

static const CTransform* pawn_xf(World* w, Entity e) { return ecs_get(w, e, COMP_TRANSFORM); }

static void eye_of(World* w, Entity e, float out[3]) {
    const CTransform* t = pawn_xf(w, e);
    const CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    memcpy(out, t ? t->location : (float[3]){ 0, 0, 0 }, 12);
    out[2] += c ? c->half_height * 0.8f : 60.0f;
}

static void stop_moving(Agent* a) { a->moving = false, a->npath = a->ipath = 0, a->goal_actor = ENTITY_NONE; }
static bool start_move(World* w, Agent* a, const float to[3], float accept);

// On the navigation (within 1.5 m of it, as the grid's nearest cell): the NavMesh puts the point on it
static bool on_nav(float p[3]) {
    if (navmesh_ready()) {
        float ext[3] = { 150.0f, 150.0f, 300.0f }, q[3];
        if (!navmesh_project(p, ext, q)) return false;
        memcpy(p, q, 12);
        return true;
    }
    int cx, cy;
    return !R.nav_ok || (to_cell(p, &cx, &cy) && cell_ok(cx, cy));
}

static bool start_move(World* w, Agent* a, const float to[3], float accept) {
    const CTransform* t = pawn_xf(w, a->pawn);
    if (!t) return false;
    memcpy(a->goal, to, 12);
    a->accept = accept > 1.0f ? accept : 60.0f;
    a->ipath = 0;
    bool nav = R.nav_ok || navmesh_ready();
    a->moving = nav ? ai_nav_path_ex(t->location, to, a->path, a->pflags, MAX_PATH, &a->npath) : false;
    if (!nav) { // no navigation (a level of no meshes): straight there
        memcpy(a->path[0], to, 12);
        a->npath = 1, a->moving = true, a->pflags[0] = 0;
    }
    a->repath = 0.6f;
    R.stats.paths_this_frame++;
    return a->moving;
}

// Walks the pawn along its path: a Character gets its input, a flying pawn too, anything else is moved
static float dist2d(const float a[3], const float b[3]) { return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1])); }

// A Nav Link over a gap or up: a jump that lands on its other end (Unreal's links only walk; a Character here leaps). The
// launch: up enough to clear the edge, across no faster than the Character walks (its air control keeps that speed)
static bool start_link(Agent* a, const CTransform* t, CCharacter* ch, const float to[3]) {
    if (!ch->on_ground) return false;
    float g = project()->gravity_z * ch->gravity_scale;
    if (g < 1.0f) g = 980.0f;
    float d[2] = { to[0] - t->location[0], to[1] - t->location[1] }, len = sqrtf(d[0] * d[0] + d[1] * d[1]);
    float feet = t->location[2] - ch->half_height, dz = to[2] - feet;
    float vmax = ch->max_walk_speed > 50.0f ? ch->max_walk_speed : 300.0f;
    float h = fmaxf(dz, 0.0f) + 60.0f, vz = sqrtf(2.0f * g * h);
    float T = (vz + sqrtf(fmaxf(vz * vz - 2.0f * g * dz, 0.0f))) / g;
    if (len / T > vmax) T = len / vmax, vz = (dz + 0.5f * g * T * T) / T; // (longer: higher, slower across)
    float vh = len / T;
    a->link = 1, a->link_t = 0.0f, a->link_len = T, a->link_in = vh / vmax;
    a->link_dir[0] = len > 1e-3f ? d[0] / len : 0.0f, a->link_dir[1] = len > 1e-3f ? d[1] / len : 0.0f;
    memcpy(a->link_to, to, 12);
    ch->velocity[0] = a->link_dir[0] * vh, ch->velocity[1] = a->link_dir[1] * vh, ch->velocity[2] = vz;
    ch->on_ground = 0;
    a->ipath++; // (on to the landing point)
    return true;
}
static bool follow_link(World* w, Agent* a, CTransform* t, CCharacter* ch, float dt) {
    (void)w;
    a->link_t += dt;
    bool landed = ch && ch->on_ground && a->link_t > 0.1f;
    if (!ch || landed || a->link_t > a->link_len + 1.0f) {
        a->link = 0;
        if (!landed && dist2d(t->location, a->link_to) > 120.0f && a->moving) start_move(w, a, a->goal, a->accept); // (it fell short: a new way)
        return false;
    }
    ch->input[0] += a->link_dir[0] * a->link_in, ch->input[1] += a->link_dir[1] * a->link_in; // (the air control keeps the leap's speed)
    return true;
}

static uint32_t avoid_hash(int32_t cx, int32_t cy) { return ((uint32_t)cx * 73856093u ^ (uint32_t)cy * 19349663u) & (AVOID_HASH - 1); }

static void avoid_put(World* w, uint32_t i, Entity e) {
    const CTransform* t = pawn_xf(w, e);
    if (!t) return;
    const CCharacter* ch = ecs_get(w, e, COMP_CHARACTER);
    const CFloating* fl = ch ? NULL : ecs_get(w, e, COMP_FLOATING);
    V.e[i] = e;
    V.p[i][0] = t->location[0], V.p[i][1] = t->location[1];
    V.v[i][0] = ch ? ch->velocity[0] : fl ? fl->velocity[0] : 0.0f;
    V.v[i][1] = ch ? ch->velocity[1] : fl ? fl->velocity[1] : 0.0f;
    V.r[i] = ch && ch->radius > 1.0f ? ch->radius : 40.0f;
    uint32_t h = avoid_hash((int32_t)floorf(V.p[i][0] / AVOID_CELL), (int32_t)floorf(V.p[i][1] / AVOID_CELL));
    V.next[i] = V.head[h], V.head[h] = (int32_t)i;
}

// The places at the frame's start (before anyone moves): the agents, then the player's pawn
static void avoid_prepare(World* w) {
    V.n = 0;
    if (!ai_avoidance() || R.nagents < 1) return;
    memset(V.head, 0xFF, sizeof V.head); // (-1: empty)
    for (uint32_t i = 0; i <= R.nagents; i++) V.e[i] = ENTITY_NONE, V.next[i] = -1;
    for (uint32_t i = 0; i < R.nagents; i++)
        if (ecs_alive(w, R.agents[i].pawn)) avoid_put(w, i, R.agents[i].pawn);
    Entity player = bp_player(w);
    if (player && ecs_alive(w, player) && !ecs_get(w, player, COMP_AI_AGENT)) avoid_put(w, R.nagents, player);
    V.n = R.nagents + 1;
}

// Turns the way an agent walks (in: a unit direction times s) aside from the others it is about to meet
static bool avoid_steer(const Agent* a, uint32_t self, float in[2], float s, float max_speed) {
    if (self >= V.n || V.e[self] == ENTITY_NONE) return false;
    float ux = in[0] / s, uy = in[1] / s;              // (its direction)
    float px = V.p[self][0], py = V.p[self][1], ra = V.r[self];
    float vx = ux * max_speed * s, vy = uy * max_speed * s; // (the velocity it wants)
    float fx = 0.0f, fy = 0.0f;
    int32_t cx = (int32_t)floorf(px / AVOID_CELL), cy = (int32_t)floorf(py / AVOID_CELL);
    for (int32_t oy = -1; oy <= 1; oy++)
        for (int32_t ox = -1; ox <= 1; ox++)
            for (int32_t j = V.head[avoid_hash(cx + ox, cy + oy)]; j >= 0; j = V.next[j]) {
                if ((uint32_t)j == self || V.e[j] == ENTITY_NONE || V.e[j] == a->goal_actor || V.e[j] == a->target) continue;
                float dx = V.p[j][0] - px, dy = V.p[j][1] - py, d2 = dx * dx + dy * dy;
                if (d2 > AVOID_LOOK * AVOID_LOOK) continue; // (also another cell hashed to the same slot)
                float dist = sqrtf(d2), rr = ra + V.r[j];
                if (dist < rr * 1.05f) { // touching: apart (on top of each other: to its right)
                    float k = (rr * 1.05f - dist) / rr * 2.0f;
                    if (dist > 1.0f) fx -= dx / dist * k, fy -= dy / dist * k;
                    else fx += uy * k, fy -= ux * k;
                }
                float rvx = vx - V.v[j][0], rvy = vy - V.v[j][1], rv2 = rvx * rvx + rvy * rvy;
                if (rv2 < 1.0f) continue;
                float t = (dx * rvx + dy * rvy) / rv2; // (the closest approach, if both keep on)
                if (t <= 0.0f || t > AVOID_HORIZON) continue;
                float qx = dx - rvx * t, qy = dy - rvy * t, q = sqrtf(qx * qx + qy * qy), clear = rr * 1.5f;
                if (q >= clear) continue;
                float k = (1.0f - t / AVOID_HORIZON) * (clear - q) / clear * 1.6f;
                if (q > 2.0f) fx -= qx / q * k, fy -= qy / q * k; // (away from where they'd meet)
                else fx += uy * k, fy -= ux * k;                   // (head-on: each to its own right)
            }
    if (fx == 0.0f && fy == 0.0f) return false;
    float nx = ux + fx, ny = uy + fy, l = sqrtf(nx * nx + ny * ny);
    if (l < 1e-4f) return false;
    in[0] = nx / l * s, in[1] = ny / l * s;
    return true;
}

static void follow(World* w, Agent* a, float dt) {
    CTransform* t = ecs_get(w, a->pawn, COMP_TRANSFORM);
    CCharacter* ch = ecs_get(w, a->pawn, COMP_CHARACTER);
    CFloating* fl = ch ? NULL : ecs_get(w, a->pawn, COMP_FLOATING);
    if (!t) return;
    if (a->moving && a->goal_actor && ecs_alive(w, a->goal_actor)) { // a moving goal
        const CTransform* gt = pawn_xf(w, a->goal_actor);
        if (gt && a->npath && a->ipath + 1 >= a->npath) { // on the last leg: straight at where it is now, every frame
            memcpy(a->path[a->npath - 1], gt->location, 12);
            memcpy(a->goal, gt->location, 12);
        }
        if ((a->repath -= dt) <= 0.0f) {
            if (gt && dist2d(gt->location, a->goal) > 80.0f) start_move(w, a, gt->location, a->accept);
            a->repath = 0.25f;
        }
    }
    if (a->link && follow_link(w, a, t, ch, dt)) {
        // (in the air over a Nav Link's gap)
    } else if (!a->moving || a->ipath >= a->npath) {
        a->moving = false;
    } else if (dist2d(t->location, a->goal) <= a->accept) {
        stop_moving(a);
    } else {
        while (a->ipath + 1 < a->npath && dist2d(t->location, a->path[a->ipath]) < 45.0f) {
            if ((a->pflags[a->ipath] & NAV_PT_LINK_JUMP) && ch && start_link(a, t, ch, a->path[a->ipath + 1])) break;
            a->ipath++;
        }
        const float* p = a->path[a->ipath];
        float d[2] = { p[0] - t->location[0], p[1] - t->location[1] }, l = sqrtf(d[0] * d[0] + d[1] * d[1]);
        if (l < 1e-3f) {
            a->ipath++;
        } else {
            float s = fminf(fmaxf(a->speed, 0.15f), 1.0f);
            float in[3] = { d[0] / l * s, d[1] / l * s, 0.0f };
            if ((ch || fl) && V.n && avoid_steer(a, (uint32_t)(a - R.agents), in, s, ch ? fmaxf(ch->max_walk_speed, 1.0f) : fmaxf(fl->max_speed, 1.0f)))
                R.stats.avoiding++;
            if (ch) ch->input[0] += in[0], ch->input[1] += in[1];
            else if (fl) fl->input[0] += in[0], fl->input[1] += in[1];
            else {
                float v = 300.0f * s * dt;
                t->location[0] += in[0] / s * fminf(v, l), t->location[1] += in[1] / s * fminf(v, l);
                t->rotation[1] = atan2f(d[1], d[0]) / DEG2RAD;
                ecs_touch(w, a->pawn);
            }
        }
    }
    bool hit = a->damaged_at > 0.0f && R.time - a->damaged_at < 2.0f && !a->sees && a->target; // (the damage sense: who hit it)
    if (!a->moving && (a->face || hit)) { // turn toward what it watches
        const float* at = hit ? a->last_known : a->face_at;
        float d[2] = { at[0] - t->location[0], at[1] - t->location[1] };
        if (d[0] * d[0] + d[1] * d[1] > 1.0f) {
            float want = atan2f(d[1], d[0]) / DEG2RAD, diff = remainderf(want - t->rotation[1], 360.0f);
            float turn = 360.0f * dt;
            t->rotation[1] += fabsf(diff) < turn ? diff : (diff > 0 ? turn : -turn);
            ecs_touch(w, a->pawn);
        }
    }
}

// Sight and hearing a few times a second; the Blackboard keys perception owns
static void sense(World* w, Agent* a, const CAIAgent* c) {
    const CTransform* t = pawn_xf(w, a->pawn);
    if (!t) return;
    float eye[3];
    eye_of(w, a->pawn, eye);
    float fwd[2] = { cosf(t->rotation[1] * DEG2RAD), sinf(t->rotation[1] * DEG2RAD) };
    float cosh = cosf(fminf(c->sight_angle, 360.0f) * 0.5f * DEG2RAD);
    Entity best = ENTITY_NONE;
    float best_d = 1e30f;
    // the candidates: the player's pawn, the agents of another team
    Entity cand[64];
    uint32_t nc = 0;
    Entity player = bp_player(w);
    if (player && player != a->pawn) cand[nc++] = player;
    for (uint32_t i = 0; i < R.nagents && nc < 64; i++) {
        const Agent* o = &R.agents[i];
        if (o->pawn == a->pawn || !ecs_alive(w, o->pawn)) continue;
        const CAIAgent* oc = ecs_get(w, o->pawn, COMP_AI_AGENT);
        if (oc && c->team && oc->team && oc->team != c->team) cand[nc++] = o->pawn;
    }
    for (uint32_t i = 0; i < nc; i++) {
        float te[3];
        eye_of(w, cand[i], te);
        float d[3] = { te[0] - eye[0], te[1] - eye[1], te[2] - eye[2] }, l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l > c->sight_radius || l < 1.0f) continue;
        float l2 = sqrtf(d[0] * d[0] + d[1] * d[1]);
        // behind it (very near: felt; the target it just saw, a bit farther: it doesn't vanish by stepping out of the cone)
        bool recent = cand[i] == a->target && R.time - a->last_seen < 3.0f;
        if (l2 > 1.0f && (d[0] * fwd[0] + d[1] * fwd[1]) / l2 < cosh && l > (recent ? 450.0f : 150.0f)) continue;
        if (bp_trace_boxes(eye, te, 0.0f) < 0.97f) continue; // a wall between
        if (l < best_d) best_d = l, best = cand[i];
    }
    // the reaction: a new target is noticed after seeing it reaction s (one it already follows: at once)
    if (best == ENTITY_NONE) a->spot_t = fmaxf(a->spot_t - SENSE_PERIOD, 0.0f); // (out of sight: it fades)
    else if (best != a->target && (a->spot_t += SENSE_PERIOD) < c->reaction) best = ENTITY_NONE;
    a->sees = best != ENTITY_NONE;
    if (a->sees) {
        if (best != a->target) a->tvel[0] = a->tvel[1] = 0.0f, a->vel_t = 0.0f;
        a->target = best, a->last_seen = R.time, a->spot_t = c->reaction;
        const CTransform* tt = pawn_xf(w, best);
        if (tt) {
            memcpy(a->last_known, tt->location, 12);
            float span = R.time - a->vel_t;
            if (a->vel_t > 0.0f && span > 0.05f) { // (smoothed: the velocity between two looks)
                for (int k = 0; k < 2; k++) a->tvel[k] += ((tt->location[k] - a->vel_from[k]) / span - a->tvel[k]) * 0.5f;
            }
            if (a->vel_t <= 0.0f || span > 0.05f) memcpy(a->vel_from, tt->location, 12), a->vel_t = R.time;
        }
    } else if (a->target && R.time - a->last_seen > c->lose_sight_time)
        a->target = ENTITY_NONE;
    // hearing: the loudest recent noise within reach
    a->heard = false;
    for (uint32_t i = 0; i < R.nnoises; i++) {
        if (R.noises[i].who == a->pawn || R.time - R.noises[i].t > 0.6f) continue;
        float r = c->hearing_radius * R.noises[i].loud;
        if (dist2d(R.noises[i].at, t->location) <= r) a->heard = true, a->heard_at = R.time, memcpy(a->noise, R.noises[i].at, 12);
    }
    if (R.time - a->heard_at < 4.0f) a->heard = true; // (it keeps the noise in mind a while)
    float tgt, one = 1.0f, zero = 0.0f;
    memcpy(&tgt, &a->target, 4);
    if (a->target) {
        bb_set(a, "TargetActor", &tgt, 1);
        float dd = dist2d(a->last_known, t->location);
        bb_set(a, "DistanceToTarget", &dd, 1);
    } else
        bb_clear(a, "TargetActor"), bb_clear(a, "DistanceToTarget");
    bb_set(a, "CanSeeTarget", a->sees ? &one : &zero, 1);
    if (a->last_seen > 0.0f) bb_set(a, "LastKnownLocation", a->last_known, 3);
    bb_set(a, "HeardNoise", a->heard ? &one : &zero, 1);
    if (a->heard_at > 0.0f) bb_set(a, "NoiseLocation", a->noise, 3);
    bb_set(a, "SelfLocation", t->location, 3);
}

// --- Teamwork (each frame, after the senses): the team shares what it sees, each squad (a team's agents after the same
// target) shares out the ring around it, every one aims where the target will be ---

// Where the target will be when this agent can reach it: the intercept of a runner at speed s with the target's
// velocity, no further ahead than prediction s; on the grid (off it: the target's own place)
static void predict(World* w, Agent* a, const CAIAgent* c) {
    memcpy(a->predicted, a->last_known, 12);
    const CTransform* self = pawn_xf(w, a->pawn);
    if (!self || c->prediction <= 0.0f) return;
    const CCharacter* ch = ecs_get(w, a->pawn, COMP_CHARACTER);
    float s = ch && ch->max_walk_speed > 1.0f ? ch->max_walk_speed : 300.0f;
    float rx = a->last_known[0] - self->location[0], ry = a->last_known[1] - self->location[1];
    float vx = a->tvel[0], vy = a->tvel[1];
    // |r + v t| = s t: (v.v - s^2) t^2 + 2 (r.v) t + r.r = 0, its smallest positive root
    float qa = vx * vx + vy * vy - s * s, qb = 2.0f * (rx * vx + ry * vy), qc = rx * rx + ry * ry, t = c->prediction;
    if (fabsf(qa) > 1e-3f) {
        float disc = qb * qb - 4.0f * qa * qc;
        if (disc >= 0.0f) {
            float sq = sqrtf(disc), t1 = (-qb - sq) / (2.0f * qa), t2 = (-qb + sq) / (2.0f * qa);
            float tt = t1 > 0.0f && (t1 < t2 || t2 <= 0.0f) ? t1 : t2;
            if (tt > 0.0f) t = fminf(tt, c->prediction);
        }
    }
    float p[3] = { a->last_known[0] + vx * t, a->last_known[1] + vy * t, a->last_known[2] };
    if (on_nav(p)) memcpy(a->predicted, p, 12);
}

typedef struct SquadItem { uint32_t agent; uint32_t target; float angle, dist; } SquadItem;
static int squad_cmp(const void* x, const void* y) {
    const SquadItem *a = x, *b = y;
    if (a->target != b->target) return a->target < b->target ? -1 : 1;
    return a->angle < b->angle ? -1 : a->angle > b->angle;
}

static void teamwork(World* w) {
    // the team hears what one sees (within share_radius): the target, where it is, how it moves
    for (uint32_t i = 0; i < R.nagents; i++) {
        Agent* a = &R.agents[i];
        const CAIAgent* c = ecs_get(w, a->pawn, COMP_AI_AGENT);
        const CTransform* t = pawn_xf(w, a->pawn);
        if (!a->sees || !c || !t || !c->team || c->share_radius <= 0.0f) continue;
        for (uint32_t j = 0; j < R.nagents; j++) {
            Agent* b = &R.agents[j];
            if (b == a || b->sees || !ecs_alive(w, b->pawn)) continue;
            const CAIAgent* bc = ecs_get(w, b->pawn, COMP_AI_AGENT);
            const CTransform* bt = pawn_xf(w, b->pawn);
            if (!bc || !bt || bc->team != c->team || dist2d(bt->location, t->location) > c->share_radius) continue;
            if (b->target && b->target != a->target && R.time - b->last_seen < 0.5f) continue; // (busy with its own)
            b->target = a->target, b->last_seen = R.time;
            memcpy(b->last_known, a->last_known, 12);
            b->tvel[0] = a->tvel[0], b->tvel[1] = a->tvel[1];
        }
    }
    // the squads: by target, then by angle around it; the ring's slots in that order (neighbors stay neighbors, nobody
    // crosses), turned to fit where they already are; the nearest charge, the others go round (coordination)
    static SquadItem items[MAX_AGENTS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < R.nagents; i++) {
        Agent* a = &R.agents[i];
        a->squad = 0, a->flanks = false;
        const CAIAgent* c = ecs_get(w, a->pawn, COMP_AI_AGENT);
        const CTransform* t = pawn_xf(w, a->pawn);
        if (!a->target || !c || !t || !ecs_alive(w, a->pawn)) continue;
        predict(w, a, c);
        uint32_t tg;
        memcpy(&tg, &a->target, 4);
        float dx = t->location[0] - a->last_known[0], dy = t->location[1] - a->last_known[1];
        items[n++] = (SquadItem){ i, c->team ? tg ^ c->team * 0x9E3779B9u : tg ^ (i + 1) * 0x85EBCA6Bu, atan2f(dy, dx), sqrtf(dx * dx + dy * dy) };
    }
    qsort(items, n, sizeof *items, squad_cmp);
    for (uint32_t s = 0; s < n;) {
        uint32_t e = s;
        while (e < n && items[e].target == items[s].target) e++;
        uint32_t k = e - s;
        const float two_pi = 6.2831853f, step = two_pi / (float)k;
        // the ring's turn: the circular mean of (angle - slot order)
        float sx = 0.0f, sy = 0.0f;
        for (uint32_t i = 0; i < k; i++) {
            float off = items[s + i].angle - step * (float)i;
            sx += cosf(off), sy += sinf(off);
        }
        float base = atan2f(sy, sx);
        // who charges: the nearest (1 - coordination) share, at least one
        float coord = 0.0f;
        for (uint32_t i = 0; i < k; i++) {
            const CAIAgent* c = ecs_get(w, R.agents[items[s + i].agent].pawn, COMP_AI_AGENT);
            coord += c ? c->coordination : 0.0f;
        }
        coord /= (float)k;
        uint32_t charge = k - (uint32_t)floorf(coord * (float)(k - 1) + 0.5f);
        for (uint32_t i = 0; i < k; i++) {
            Agent* a = &R.agents[items[s + i].agent];
            const CAIAgent* c = ecs_get(w, a->pawn, COMP_AI_AGENT);
            float r = c && c->surround_radius > 10.0f ? c->surround_radius : 350.0f, ang = base + step * (float)i;
            float p[3] = { a->predicted[0] + cosf(ang) * r, a->predicted[1] + sinf(ang) * r, a->predicted[2] };
            if (!on_nav(p)) memcpy(p, a->predicted, 12); // (a wall there)
            memcpy(a->flank, p, 12);
            a->squad = k;
            uint32_t nearer = 0; // (its rank by distance in the squad)
            for (uint32_t j = 0; j < k; j++) nearer += items[s + j].dist < items[s + i].dist;
            a->flanks = k > 1 && nearer >= charge;
        }
        s = e;
    }
    for (uint32_t i = 0; i < R.nagents; i++) {
        Agent* a = &R.agents[i];
        if (!a->target) continue;
        bb_set(a, "PredictedLocation", a->predicted, 3);
        bb_set(a, "FlankLocation", a->flank, 3);
    }
}

// --- The Behavior Tree ---

static bool bb_cond(Agent* a, const BtDeco* d) {
    int k = key_index(a, d->key);
    bool r;
    if (k < 0) r = d->op == BB_IS_NOT_SET;
    else {
        const BbValue* v = &a->bb[k];
        switch (d->op) {
        case BB_IS_SET: r = v->set; break;
        case BB_IS_NOT_SET: r = !v->set; break;
        case BB_GREATER: r = v->v[0] > d->value; break;
        case BB_LESS: r = v->v[0] < d->value; break;
        default: r = fabsf(v->v[0] - d->value) < 1e-4f; break;
        }
    }
    return d->inverse ? !r : r;
}

// Every decorator of the node lets it run (Unreal's: all of them must pass)
static bool deco_ok(World* w, Agent* a, const BehaviorTree* t, int n) {
    const BtNode* nd = &t->nodes[n];
    (void)w;
    for (uint32_t i = 0; i < nd->ndecos; i++) {
        const BtDeco* d = &nd->decos[i];
        bool ok = true;
        switch (d->kind) {
        case BTD_BLACKBOARD: ok = bb_cond(a, d); break;
        case BTD_COOLDOWN: ok = R.time >= a->cool_until[n]; break;
        case BTD_TIME_LIMIT: ok = a->running != n || a->task_t < d->value; break;
        default: break;
        }
        if (!ok) return false;
    }
    return true;
}
static bool has_deco(const BtNode* nd, uint8_t kind, float* value) {
    for (uint32_t i = 0; i < nd->ndecos; i++)
        if (nd->decos[i].kind == kind) {
            if (value) *value = nd->decos[i].value;
            return true;
        }
    return false;
}

// n finished (ok or not): its parent's bookkeeping; the next node to go down from (-1: the tree is done this pass)
static int bt_finish(Agent* a, const BehaviorTree* t, int n, bool ok) {
    for (;;) {
        const BtNode* nd = &t->nodes[n];
        float dv = 0.0f;
        if (has_deco(nd, BTD_COOLDOWN, &dv)) a->cool_until[n] = R.time + dv;
        if (has_deco(nd, BTD_FORCE_SUCCESS, NULL)) ok = true;
        if (has_deco(nd, BTD_LOOP, &dv) && ok && (dv <= 0.0f || ++a->loops[n] < (uint16_t)dv)) return n;
        a->loops[n] = 0;
        int p = nd->parent < 0 ? 0 : nd->parent;
        if (p == 0) return -1;
        uint8_t kind = t->nodes[p].kind;
        if (kind == BT_SEQUENCE) {
            if (ok && ++a->idx[p] < a->nkids[p]) return p;
            a->idx[p] = 0;
        } else { // a selector: the first child that works
            if (!ok && ++a->idx[p] < a->nkids[p]) return p;
            a->idx[p] = 0;
        }
        n = p;
    }
}

// Down from n to the leaf to run (-1: nothing to run this pass)
static int bt_pick(World* w, Agent* a, const BehaviorTree* t, int n) {
    for (int guard = 0; guard < 256 && n >= 0; guard++) {
        if (!deco_ok(w, a, t, n)) {
            n = bt_finish(a, t, n, false);
            continue;
        }
        uint8_t kind = t->nodes[n].kind;
        if (kind == BT_SELECTOR || kind == BT_SEQUENCE) {
            if (a->idx[n] >= a->nkids[n]) {
                n = bt_finish(a, t, n, kind == BT_SEQUENCE);
                continue;
            }
            n = a->kids[n][a->idx[n]];
            continue;
        }
        return n;
    }
    return -1;
}

static float bb_float(Agent* a, const char* key) {
    int k = key_index(a, key);
    return k >= 0 ? a->bb[k].v[0] : 0.0f;
}
// A key as a place: a vector's, an actor's location
static bool bb_place(World* w, Agent* a, const char* key, float out[3], Entity* actor) {
    int k = key_index(a, key);
    if (k < 0 || !a->bb[k].set) return false;
    if (actor) *actor = ENTITY_NONE;
    if (a->keys[k].type == BB_ACTOR) {
        Entity e;
        memcpy(&e, &a->bb[k].v[0], 4);
        const CTransform* t = ecs_alive(w, e) ? pawn_xf(w, e) : NULL;
        if (!t) return false;
        memcpy(out, t->location, 12);
        if (actor) *actor = e;
        return true;
    }
    memcpy(out, a->bb[k].v, 12);
    return true;
}

// Starts a task: true when it is done at once (ok tells how)
static bool task_start(World* w, Agent* a, const BehaviorTree* t, int n, bool* ok) {
    const BtNode* nd = &t->nodes[n];
    a->task_t = 0.0f;
    *ok = true;
    const CTransform* self = pawn_xf(w, a->pawn);
    switch (nd->kind) {
    case BT_MOVE_TO: {
        float at[3];
        Entity ga;
        if (!bb_place(w, a, nd->key, at, &ga) || !start_move(w, a, at, nd->value)) return *ok = false, true;
        a->goal_actor = ga;
        return false;
    }
    case BT_WAIT: a->task_len = fmaxf(nd->value + (rnd01() * 2.0f - 1.0f) * nd->value2, 0.0f); return false;
    case BT_ROTATE_TO: {
        if (!bb_place(w, a, nd->key, a->face_at, NULL)) return *ok = false, true;
        a->face = true;
        return false;
    }
    case BT_FIND_RANDOM: {
        float p[3];
        if (!self || !ai_nav_random_point(self->location, nd->value > 1.0f ? nd->value : 500.0f, p)) return *ok = false, true;
        bb_set(a, nd->key, p, 3);
        return true;
    }
    case BT_SET_KEY: bb_set(a, nd->key, (float[3]){ nd->value, nd->value2, 0 }, 3); return true;
    case BT_CLEAR_KEY: bb_clear(a, nd->key); return true;
    case BT_RUN_EVENT: bp_call_event(w, a->pawn, nd->text); return true;
    case BT_PLAY_MONTAGE: anim_play_montage(w, a->pawn, nd->text, 1.0f); return true;
    case BT_PREDICT: // the intercept (value: at most so many s ahead; 0: the agent's Prediction)
    case BT_SURROUND: { // its squad's slot (value: the ring's radius; 0: the agent's)
        if (!a->target) return *ok = false, true;
        CAIAgent* c = ecs_get(w, a->pawn, COMP_AI_AGENT);
        if (c && nd->kind == BT_PREDICT && nd->value > 0.0f) {
            float keep = c->prediction;
            c->prediction = nd->value, predict(w, a, c), c->prediction = keep;
        }
        float p[3];
        memcpy(p, nd->kind == BT_PREDICT ? a->predicted : a->flank, 12);
        if (nd->kind == BT_SURROUND && nd->value > 10.0f && c) { // (another radius: the same angle)
            float dx = a->flank[0] - a->predicted[0], dy = a->flank[1] - a->predicted[1], l = sqrtf(dx * dx + dy * dy);
            if (l > 1.0f) p[0] = a->predicted[0] + dx / l * nd->value, p[1] = a->predicted[1] + dy / l * nd->value;
        }
        bb_set(a, nd->key, p, 3);
        return true;
    }
    default: return *ok = false, true;
    }
}

// A running task this frame: true when done (ok tells how)
static bool task_tick(World* w, Agent* a, const BehaviorTree* t, int n, float dt, bool* ok) {
    const BtNode* nd = &t->nodes[n];
    a->task_t += dt;
    *ok = true;
    switch (nd->kind) {
    case BT_MOVE_TO:
        if (!a->moving) {
            const CTransform* s = pawn_xf(w, a->pawn);
            *ok = s && dist2d(s->location, a->goal) <= a->accept + 30.0f;
            return true;
        }
        return false;
    case BT_WAIT: return a->task_t >= a->task_len;
    case BT_ROTATE_TO: {
        const CTransform* s = pawn_xf(w, a->pawn);
        if (!s) return true;
        float want = atan2f(a->face_at[1] - s->location[1], a->face_at[0] - s->location[0]) / DEG2RAD;
        float diff = remainderf(want - s->rotation[1], 360.0f);
        return fabsf(diff) < 8.0f || a->task_t > 2.0f;
    }
    default: return true;
    }
}

// The services of the running branch (its ancestors' and its own)
static void services(World* w, Agent* a, const BehaviorTree* t, int n) {
    for (int guard = 0; n > 0 && guard < 64; guard++, n = t->nodes[n].parent) {
        const BtNode* nd = &t->nodes[n];
        for (uint32_t s = 0; s < nd->nsvcs; s++) {
            if (nd->svcs[s].kind == BTS_FOCUS) a->face = bb_place(w, a, nd->svcs[s].key, a->face_at, NULL);
            else if (nd->svcs[s].kind == BTS_SPEED) a->speed = nd->svcs[s].value;
        }
    }
}

// Something more important than the running task (a selector's earlier child whose condition now holds), or the
// running branch's condition failing: Unreal's "observer aborts"
static bool should_abort(World* w, Agent* a, const BehaviorTree* t, int n) {
    for (int guard = 0; n > 0 && guard < 64; guard++) {
        const BtNode* nd = &t->nodes[n];
        for (uint32_t d = 0; d < nd->ndecos; d++) // Self: the running branch's condition stopped holding
            if (nd->decos[d].kind == BTD_BLACKBOARD && (nd->decos[d].aborts == BTA_SELF || nd->decos[d].aborts == BTA_BOTH) &&
                !bb_cond(a, &nd->decos[d]))
                return true;
        int p = nd->parent < 0 ? 0 : nd->parent;
        if (t->nodes[p].kind == BT_SELECTOR) // Lower Priority: an earlier (more important) child's condition holds now
            for (uint32_t i = 0; i < a->nkids[p] && a->kids[p][i] != n; i++) {
                const BtNode* sib = &t->nodes[a->kids[p][i]];
                bool watches = false;
                for (uint32_t d = 0; d < sib->ndecos; d++)
                    watches |= sib->decos[d].kind == BTD_BLACKBOARD && (sib->decos[d].aborts == BTA_LOWER || sib->decos[d].aborts == BTA_BOTH);
                if (watches && deco_ok(w, a, t, a->kids[p][i])) return true;
            }
        n = p;
    }
    return false;
}

static void tree_tick(World* w, Agent* a, const BehaviorTree* t, float dt) {
    if (!t || t->nnodes < 2) return;
    if (a->running >= 0 && (a->bt_check -= dt) <= 0.0f) {
        a->bt_check = 0.1f;
        if (should_abort(w, a, t, a->running)) { // back to the top: the important branch gets its turn
            stop_moving(a);
            a->face = false;
            a->running = -1;
            memset(a->idx, 0, sizeof a->idx);
        }
    }
    for (int guard = 0; guard < 8; guard++) {
        if (a->running < 0) {
            int leaf = bt_pick(w, a, t, a->kids[0][0] >= 0 && a->nkids[0] ? a->kids[0][0] : -1);
            if (leaf < 0) return; // done this pass: again next frame
            bool ok;
            a->face = false;
            if (task_start(w, a, t, leaf, &ok)) {
                int next = bt_finish(a, t, leaf, ok);
                (void)next;
                continue;
            }
            a->running = (int16_t)leaf;
        }
        services(w, a, t, a->running);
        bool ok;
        if (!task_tick(w, a, t, a->running, dt, &ok)) {
            a->what = g_bt_kind_names[t->nodes[a->running].kind];
            return;
        }
        int leaf = a->running;
        a->running = -1;
        bt_finish(a, t, leaf, ok);
        stop_moving(a);
        dt = 0.0f;
    }
}

// --- The Brain ---

static float consider(World* w, Agent* a, const BrainBehavior* b, const Consideration* k) {
    const CTransform* self = pawn_xf(w, a->pawn);
    float x = 0.0f;
    switch (k->input) {
    case BRAIN_IN_DISTANCE: x = a->target && self ? dist2d(a->last_known, self->location) : 1e9f; break;
    case BRAIN_IN_SEES_TARGET: x = a->sees ? 1.0f : 0.0f; break;
    case BRAIN_IN_TIME_SINCE_SEEN: x = a->last_seen > 0.0f ? R.time - a->last_seen : 1e9f; break;
    case BRAIN_IN_HEARD_NOISE: x = a->heard ? 1.0f : 0.0f; break;
    case BRAIN_IN_KEY_FLOAT: x = bb_float(a, k->key); break;
    case BRAIN_IN_BP_VAR: {
        BpValue v;
        x = bp_instance_var(w, a->pawn, k->key, &v) ? v.v[0] : 0.0f;
        break;
    }
    case BRAIN_IN_RANDOM: x = rnd01() * (k->hi - k->lo) + k->lo; break;
    case BRAIN_IN_TIME_IN_BEHAVIOR: x = &a->brain->behaviors[a->active] == b ? a->since_change : 0.0f; break;
    case BRAIN_IN_HAS_TARGET: x = a->target ? 1.0f : 0.0f; break;
    case BRAIN_IN_SQUAD_SIZE: x = (float)a->squad; break;
    case BRAIN_IN_TARGET_SPEED: x = a->target ? sqrtf(a->tvel[0] * a->tvel[0] + a->tvel[1] * a->tvel[1]) : 0.0f; break;
    default: break;
    }
    float span = k->hi - k->lo, u = fabsf(span) > 1e-6f ? (x - k->lo) / span : (x >= k->lo ? 1.0f : 0.0f);
    float y = ai_curve(k->curve, u, k->param);
    return k->invert ? 1.0f - y : y;
}

static void brain_enter(World* w, Agent* a, const BrainBehavior* b) {
    stop_moving(a);
    a->face = false, a->entered = true, a->since_change = 0.0f;
    if (b->action == BRAIN_ACT_RUN_TREE) {
        a->sub_tree = ai_tree_name(b->text);
        prepare_tree(a, a->sub_tree);
    } else if (b->action == BRAIN_ACT_EVENT)
        bp_call_event(w, a->pawn, b->text);
}

static void brain_act(World* w, Agent* a, const BrainBehavior* b, float dt) {
    const CTransform* self = pawn_xf(w, a->pawn);
    if (!self) return;
    a->speed = b->speed > 0.0f ? b->speed : 0.5f;
    float tgt[3];
    bool has = a->target && ecs_alive(w, a->target);
    if (has) {
        const CTransform* tt = pawn_xf(w, a->target);
        memcpy(tgt, tt && a->sees ? tt->location : a->last_known, 12);
    }
    switch (b->action) {
    case BRAIN_ACT_WANDER:
        if (!a->moving) {
            float p[3];
            if (ai_nav_random_point(self->location, b->param > 1.0f ? b->param : 600.0f, p)) start_move(w, a, p, 60.0f);
        }
        break;
    case BRAIN_ACT_PATROL:
        if (!a->moving) { // the corners of a square around where it started
            float r = b->param > 1.0f ? b->param : 500.0f;
            static const float cx[4] = { 1, 1, -1, -1 }, cy[4] = { 1, -1, -1, 1 };
            float p[3] = { a->home[0] + cx[a->patrol_i % 4] * r, a->home[1] + cy[a->patrol_i % 4] * r, a->home[2] };
            a->patrol_i++;
            if (!start_move(w, a, p, 80.0f)) ai_nav_random_point(a->home, r, p), start_move(w, a, p, 80.0f);
        }
        break;
    case BRAIN_ACT_CHASE: // straight at it (its Prediction ahead); in a squad, the ones it sends round go to their flank first
        if (has && (!a->moving || (a->repath -= dt) <= 0.0f)) {
            const float* at = a->flanks && dist2d(self->location, a->flank) > 150.0f && dist2d(self->location, tgt) > 250.0f ? a->flank
                              : dist2d(self->location, tgt) > 250.0f                                                     ? a->predicted
                                                                                                                         : tgt;
            start_move(w, a, at, 90.0f);
            a->goal_actor = at == tgt && a->sees ? a->target : 0;
            a->repath = 0.3f;
        }
        break;
    case BRAIN_ACT_INTERCEPT: // the shortest way to where it will be (the shortcut that cuts it off), then at it
        if (has && (!a->moving || (a->repath -= dt) <= 0.0f)) {
            bool near = dist2d(self->location, tgt) < 250.0f;
            start_move(w, a, near ? tgt : a->predicted, 90.0f);
            a->goal_actor = near && a->sees ? a->target : 0;
            a->repath = 0.3f;
        }
        break;
    case BRAIN_ACT_SURROUND: { // its slot in the squad's ring around where the target will be, facing it
        if (!has) break;
        a->face = true;
        memcpy(a->face_at, tgt, 12);
        if (dist2d(self->location, a->flank) > 70.0f) {
            if (!a->moving || (a->repath -= dt) <= 0.0f) start_move(w, a, a->flank, 50.0f), a->repath = 0.35f;
        } else
            stop_moving(a);
        break;
    }
    case BRAIN_ACT_FLEE:
        if (has && !a->moving) {
            float d[2] = { self->location[0] - tgt[0], self->location[1] - tgt[1] }, l = sqrtf(d[0] * d[0] + d[1] * d[1]) + 1e-3f;
            float away = b->param > 1.0f ? b->param : 900.0f, p[3] = { self->location[0] + d[0] / l * away, self->location[1] + d[1] / l * away, self->location[2] };
            if (!ai_nav_random_point(p, 300.0f, p) || !start_move(w, a, p, 80.0f)) ai_nav_random_point(self->location, away, p), start_move(w, a, p, 80.0f);
        }
        break;
    case BRAIN_ACT_KEEP_DISTANCE: {
        if (!has) break;
        float d = dist2d(self->location, tgt), lo = b->param > 0.0f ? b->param : 400.0f, hi = b->param2 > lo ? b->param2 : lo + 400.0f;
        a->face = true;
        memcpy(a->face_at, tgt, 12);
        if (d < lo && !a->moving) {
            float dir[2] = { self->location[0] - tgt[0], self->location[1] - tgt[1] }, l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]) + 1e-3f;
            float p[3] = { self->location[0] + dir[0] / l * (lo - d + 150.0f), self->location[1] + dir[1] / l * (lo - d + 150.0f), self->location[2] };
            start_move(w, a, p, 60.0f);
        } else if (d > hi && (!a->moving || (a->repath -= dt) <= 0.0f))
            start_move(w, a, tgt, (lo + hi) * 0.5f);
        else if (d >= lo && d <= hi)
            stop_moving(a);
        break;
    }
    case BRAIN_ACT_INVESTIGATE: {
        const float* at = a->heard && (!a->last_seen || a->heard_at > a->last_seen) ? a->noise : a->last_known;
        if (!a->moving && dist2d(self->location, at) > 120.0f && a->since_change < 0.3f) start_move(w, a, at, 100.0f);
        if (!a->moving) a->face = true, a->face_at[0] = at[0] + 1.0f, a->face_at[1] = at[1], a->face_at[2] = at[2];
        break;
    }
    case BRAIN_ACT_ATTACK: {
        if (!has) break;
        float range = b->param > 1.0f ? b->param : 150.0f, cool = b->param2 > 0.0f ? b->param2 : 1.2f;
        // in a squad: from its own side of the ring (its slot's direction), not all from the same spot
        float spot[3], fx = a->flank[0] - a->predicted[0], fy = a->flank[1] - a->predicted[1], fl = sqrtf(fx * fx + fy * fy);
        bool side = a->squad > 1 && fl > 1.0f;
        if (side) spot[0] = tgt[0] + fx / fl * range * 0.7f, spot[1] = tgt[1] + fy / fl * range * 0.7f, spot[2] = tgt[2];
        bool in_range = dist2d(self->location, tgt) <= range;
        if (!in_range || (side && dist2d(self->location, spot) > 80.0f)) {
            if (!a->moving || (a->repath -= dt) <= 0.0f) {
                if (side) start_move(w, a, spot, 40.0f), a->goal_actor = 0, a->repath = 0.3f;
                else start_move(w, a, tgt, range * 0.8f), a->goal_actor = a->target;
            }
        } else
            stop_moving(a);
        if (in_range) {
            a->face = true;
            memcpy(a->face_at, tgt, 12);
            if (R.time - a->attack_at >= cool) a->attack_at = R.time, bp_call_event(w, a->pawn, b->text[0] ? b->text : "Attack");
        }
        break;
    }
    case BRAIN_ACT_RUN_TREE: tree_tick(w, a, a->sub_tree, dt); break;
    default: stop_moving(a); break;
    }
}

static void brain_tick(World* w, Agent* a, float dt) {
    Brain* br = a->brain;
    if (!br || !br->nbehaviors) return;
    a->since_change += dt;
    if ((a->think -= dt) <= 0.0f) {
        a->think = br->think_interval > 0.02f ? br->think_interval : 0.25f;
        int best = a->active;
        float best_s = -1.0f;
        for (uint32_t i = 0; i < br->nbehaviors; i++) {
            const BrainBehavior* b = &br->behaviors[i];
            float s = b->weight, mod = b->ncons ? 1.0f - 1.0f / (float)b->ncons : 0.0f;
            for (uint32_t c = 0; c < b->ncons && s > 0.0f; c++) {
                float v = consider(w, a, b, &b->cons[c]);
                s *= v + (1.0f - v) * mod * v; // the compensation: many considerations don't drown a good behavior
            }
            if ((int)i == a->active) s *= 1.0f + br->momentum; // momentum: the running one keeps going over a close call
            a->scores[i] = s;
            if (s > best_s) best_s = s, best = (int)i;
        }
        if (best != a->active || !a->entered) {
            a->active = best;
            brain_enter(w, a, &br->behaviors[best]);
        }
    }
    a->what = br->behaviors[a->active].name;
    brain_act(w, a, &br->behaviors[a->active], dt);
}

// --- Play ---

void ai_begin_play(World* w) {
    free(R.agents);
    memset(&R, 0, sizeof R);
    R.playing = true;
    uint32_t n = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++)
        if (w->archetypes[ai].mask & COMP_BIT(COMP_AI_AGENT)) n += w->archetypes[ai].count;
    if (!n) return;
    R.agents = calloc(MAX_AGENTS, sizeof(Agent)); // a play's start, not a frame
    if (!R.agents) return;
    R.nav_ok = ai_nav_build(w);
    printf("[RealEngine] Navigation: %ux%u cells of %.0f cm, %u walkable, %.1f ms\n", N.w, N.h, N.cell, N.walkable, N.ms);
    const NavMeshStats* ns = navmesh_stats();
    printf("[RealEngine] NavMesh: %u tiles, %u polygons, %u Nav Links (%u on the mesh), %s\n", ns->tiles, ns->polys, ns->offmesh, ns->offmesh_linked,
           navmesh_ready() ? "in use" : navmesh_building() ? "building (the grid meanwhile)" : "none (the grid)");
}

void ai_end_play(World* w) {
    (void)w;
    free(R.agents);
    R.agents = NULL;
    R.nagents = 0;
    R.playing = false;
}

static Agent* agent_of(World* w, Entity pawn) {
    for (uint32_t i = 0; i < R.nagents; i++)
        if (R.agents[i].pawn == pawn) return ecs_alive(w, pawn) ? &R.agents[i] : NULL;
    return NULL;
}

static void make_agent(World* w, Entity e, CAIAgent* c) {
    if (!R.agents || R.nagents >= MAX_AGENTS) return;
    Agent* a = &R.agents[R.nagents++];
    memset(a, 0, sizeof *a);
    a->pawn = e;
    a->mode = c->mode;
    a->active = 0, a->running = -1;
    for (uint32_t i = 0; i < AI_MAX_NODES; i++) a->kids[i][0] = -1;
    perception_keys(a->keys, &a->nkeys);
    if (c->mode == AI_MODE_TREE) a->tree = ai_tree(c->asset), prepare_tree(a, a->tree);
    else if (c->mode == AI_MODE_BRAIN) {
        a->brain = ai_brain(c->asset);
        if (a->brain) add_keys(a, a->brain->keys, a->brain->nkeys);
    }
    const CTransform* t = pawn_xf(w, e);
    if (t) memcpy(a->home, t->location, 12);
    a->speed = 0.5f;
    a->sense_timer = rnd01() * 0.15f; // (staggered)
    c->inst = R.nagents;
    CCharacter* ch = ecs_get(w, e, COMP_CHARACTER);
    if (ch) ch->auto_possess = 0; // an AI's pawn is not the player's
}

void ai_update(World* w, float dt) {
    R.stats.agents = R.nagents;
    R.stats.paths_this_frame = 0;
    if (!R.playing || !R.agents) {
        R.stats.ms = 0.0;
        return;
    }
    double t0 = re_time_seconds();
    R.time += dt;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) { // new agents (spawned while playing)
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_AI_AGENT))) continue;
        CAIAgent* c = ecs_arch_column(ar, COMP_AI_AGENT);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t r = 0; r < ar->count; r++)
            if (!c[r].inst) make_agent(w, ents[r], &c[r]);
    }
    for (uint32_t i = 0; i < R.nagents; i++) {
        Agent* a = &R.agents[i];
        if (!ecs_alive(w, a->pawn)) continue;
        const CAIAgent* c = ecs_get(w, a->pawn, COMP_AI_AGENT);
        if (!c) continue;
        if ((a->sense_timer -= dt) <= 0.0f) a->sense_timer = SENSE_PERIOD, sense(w, a, c);
        // the target's place every frame while seen (between two looks it lagged 0.15 s, and the path 0.5 s more), and a
        // moment after it went out of sight (its steps still tell where it went: the AI lost the player at every corner)
        if (a->target && ecs_alive(w, a->target) && (a->sees || R.time - a->last_seen < 1.5f)) {
            const CTransform* tt = pawn_xf(w, a->target);
            if (tt) memcpy(a->last_known, tt->location, 12);
        }
    }
    teamwork(w);
    R.stats.avoiding = 0;
    avoid_prepare(w);
    for (uint32_t i = 0; i < R.nagents; i++) {
        Agent* a = &R.agents[i];
        if (!ecs_alive(w, a->pawn) || !ecs_get(w, a->pawn, COMP_AI_AGENT)) continue;
        if (a->mode == AI_MODE_TREE) tree_tick(w, a, a->tree, dt);
        else if (a->mode == AI_MODE_BRAIN) brain_tick(w, a, dt);
        follow(w, a, dt);
    }
    // old noises go
    uint32_t keep = 0;
    for (uint32_t i = 0; i < R.nnoises; i++)
        if (R.time - R.noises[i].t < 0.6f) R.noises[keep++] = R.noises[i];
    R.nnoises = keep;
    R.stats.agents = R.nagents;
    R.stats.ms = (re_time_seconds() - t0) * 1000.0;
}

void ai_report_noise(World* w, const float at[3], float loudness, Entity instigator) {
    (void)w;
    if (!R.playing || !R.agents) return;
    uint32_t i = R.nnoises < MAX_NOISES ? R.nnoises++ : 0;
    memcpy(R.noises[i].at, at, 12);
    R.noises[i].loud = loudness > 0.0f ? loudness : 1.0f;
    R.noises[i].t = R.time;
    R.noises[i].who = instigator;
}

// The damage sense: no cone, no reaction time (a hit is felt at once); a team mate's hit (friendly fire) is not an
// enemy, nor is any hit to an agent of team 0 (it ignores the others). The instigator's place is followed 1.5 s (the
// update copies it while the memory is fresh), its noise is heard 4 s, the pawn turns to it 2 s while it doesn't see it.
void ai_report_damage(World* w, Entity victim, Entity instigator, float amount) {
    if (!R.playing || !R.agents || !(amount > 0.0f) || !instigator || instigator == victim || !ecs_alive(w, instigator)) return;
    Agent* a = agent_of(w, victim);
    const CAIAgent* c = a ? ecs_get(w, victim, COMP_AI_AGENT) : NULL;
    const CTransform* it = pawn_xf(w, instigator);
    if (!a || !c || !it || !c->team) return;
    const CAIAgent* ic = ecs_get(w, instigator, COMP_AI_AGENT);
    if (ic && ic->team == c->team) return;
    if (instigator != a->target) a->tvel[0] = a->tvel[1] = 0.0f, a->vel_t = 0.0f;
    a->target = instigator, a->last_seen = R.time, a->spot_t = c->reaction, a->damaged_at = R.time;
    memcpy(a->last_known, it->location, 12);
    a->heard = true, a->heard_at = R.time;
    memcpy(a->noise, it->location, 12);
    a->sense_timer = 0.0f; // (the keys at the next update)
    R.stats.damage_sensed++;
}

bool ai_bb_get(World* w, Entity pawn, const char* key, float out[4]) {
    Agent* a = agent_of(w, pawn);
    int k = a ? key_index(a, key) : -1;
    if (k < 0) return false;
    memcpy(out, a->bb[k].v, 16);
    return a->bb[k].set;
}

bool ai_bb_set(World* w, Entity pawn, const char* key, const float v[4]) {
    Agent* a = agent_of(w, pawn);
    if (!a || key_index(a, key) < 0) return false;
    bb_set(a, key, v, 4);
    return true;
}

bool ai_run_tree(World* w, Entity pawn, const char* tree) {
    Agent* a = agent_of(w, pawn);
    BehaviorTree* t = ai_tree_name(tree);
    if (!t) return false;
    if (!a) { // a pawn without an AI component: it becomes an agent (Unreal's Run Behavior Tree on an AI Controller)
        if (!ecs_add(w, pawn, COMP_AI_AGENT)) return false;
        CAIAgent* c = ecs_get(w, pawn, COMP_AI_AGENT);
        c->mode = AI_MODE_TREE, c->asset = t->hash;
        make_agent(w, pawn, c);
        return true;
    }
    a->mode = AI_MODE_TREE, a->tree = t;
    stop_moving(a);
    prepare_tree(a, t);
    return true;
}

bool ai_move_to(World* w, Entity pawn, const float at[3], float accept) {
    Agent* a = agent_of(w, pawn);
    if (!a) {
        if (!ecs_add(w, pawn, COMP_AI_AGENT)) return false;
        CAIAgent* c = ecs_get(w, pawn, COMP_AI_AGENT);
        c->mode = AI_MODE_NONE;
        make_agent(w, pawn, c);
        a = agent_of(w, pawn);
        if (!a) return false;
    }
    a->speed = 0.6f;
    return start_move(w, a, at, accept);
}

uint32_t ai_debug(AiDebug* out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < R.nagents && n < cap; i++) {
        const Agent* a = &R.agents[i];
        AiDebug* d = &out[n++];
        memset(d, 0, sizeof *d);
        d->pawn = a->pawn;
        d->what = a->what ? a->what : "-";
        if (a->brain) {
            d->nscores = a->brain->nbehaviors;
            memcpy(d->scores, a->scores, sizeof d->scores);
            d->active = (uint32_t)a->active;
        }
        d->path = (const float(*)[3])a->path + a->ipath;
        d->npath = a->moving && a->ipath < a->npath ? a->npath - a->ipath : 0;
        d->sees = a->sees;
        d->has_target = a->target != ENTITY_NONE, d->flanks = a->flanks, d->squad = a->squad;
        memcpy(d->predicted, a->predicted, 12), memcpy(d->flank, a->flank, 12);
    }
    return n;
}

const AiStats* ai_stats(void) { return &R.stats; }
