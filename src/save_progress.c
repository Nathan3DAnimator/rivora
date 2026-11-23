// Save and load the progress (save_progress.h, notas-dev/salvar-progresso.md)
#include "save_progress.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "components.h"
#include "debug_viewer.h"
#include "game_settings.h"
#include "health.h"
#include "inventory.h"
#include "level.h"
#include "online.h"
#include "platform.h"
#include "platform_files.h"
#include "project.h"
#include "steam.h"
#include "vkctx.h"
#include "dialogue.h"
#include "quest.h"
#include "game_rules.h"
#include "ability.h"

// --- The file -----------------------------------------------------------------------------------------------------------
// A header of HEADER_BYTES (little-endian), then chunks: a tag, its size, its bytes. The checksum (FNV-1a 64) covers the
// whole file with its own 8 bytes read as zeros: one byte changed anywhere and the file is refused.
//   0 magic "RESLOT\r\n" (a text-mode copy breaks it)   8 version   12 header bytes   16 kind   20 flags
//   24 checksum   32 payload bytes   36 actors   40 time (int64)   48 play time (double)   56 picture w, h (u16)
//   60 picture's offset (0: none)   64 display name [64]   128 level [128]
// Chunks: PROG (play time, checkpoint, reached checkpoints), SGOB (the Save Game object's class and variables), ACTR (the
// Saveables and the player), DEST (the destroyed), VARS (one actor's variables: Save Game to Slot), THMB (the picture),
// DLGM (what the conversations said and chose: dialogue.c's record, as a VARS stream has it), QSTL (the Quest Logs:
// quest.c's "#Quests" records, as a VARS stream has them), RULE (the match: game_rules.c's phase, round, clock, teams and
// the player's score and lives; its own version byte first), ABIL (the Ability
// Systems: attributes, active effects with their time, abilities with their cooldowns, by the actors' keys: ability.c).
// Version 1: the old Save Game to Slot file ("RESAV001" + the variables), read as a VARS-only save.
#define SAVE_MAGIC "RESLOT\r\n"
#define HEADER_BYTES 256u
#define LEGACY_MAGIC "RESAV001"
#define TAG(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define T_PROG TAG('P', 'R', 'O', 'G')
#define T_SGOB TAG('S', 'G', 'O', 'B')
#define T_ACTR TAG('A', 'C', 'T', 'R')
#define T_DEST TAG('D', 'E', 'S', 'T')
#define T_VARS TAG('V', 'A', 'R', 'S')
#define T_THMB TAG('T', 'H', 'M', 'B')
#define T_DLGM TAG('D', 'L', 'G', 'M')
#define T_QSTL TAG('Q', 'S', 'T', 'L')
#define T_RULE TAG('R', 'U', 'L', 'E')
#define T_ABIL TAG('A', 'B', 'I', 'L')
#define MAX_FILE (256u << 20)    // a save past it is not one of ours
#define PLAYER_KEY 0xFFFFFFFFu   // the player's record
#define MAX_KEYS 65536u          // Saveables a level keeps track of
#define HT_SIZE (MAX_KEYS * 2u)  // the keys' hash table (open addressing)
#define MAX_REACHED 256u
#define VARS_CAP (64u * 1024u)   // one actor's variables
// a record's parts
#define R_TRANSFORM 1u
#define R_HEALTH 2u
#define R_INVENTORY 4u
#define R_VARS 8u
#define R_HIDDEN 16u
#define R_CONTROL 32u            // a Character's control rotation and velocity reset
#define R_SPAWNED 64u            // spawned while playing: spawned again by its class

typedef struct Buf { uint8_t* p; uint32_t n, cap; bool bad; } Buf;
typedef struct Rd { const uint8_t* p; uint32_t n, at; bool bad; } Rd;

static struct {
    VkCtx* vk;
    float view[4];
    // the session (a Play, the game): carried from level to level
    bool session, began_once;
    double play_time;
    char current[SAVE_SLOT_NAME];
    float interval;            // Set Auto Save Interval (-1: the project's)
    double auto_timer;
    int auto_next;
    bool auto_pending;         // a level change's auto save, once the level began
    // the level's Saveables when it began: their keys (and flags), a hash table key -> index
    uint32_t nkeys;
    uint32_t keys[MAX_KEYS], kflags[MAX_KEYS], seen[MAX_KEYS], seen_gen;
    uint32_t ht[HT_SIZE];      // index + 1 (0: empty)
    // the progress's Save Game object, its variables between levels
    Entity sg;
    uint32_t sg_class;
    uint8_t sg_blob[VARS_CAP];
    uint32_t sg_len;
    // the checkpoint
    bool cp_valid;
    uint32_t cp_key, cp_flags;
    float cp_pos[3], cp_yaw, cp_delay;
    char cp_slot[32];
    uint32_t reached[MAX_REACHED], nreached;
    float dead_time;
    bool start_known;
    float start_pos[3], start_yaw;
    bool prime_inside;         // after a load: who is inside a Checkpoint now doesn't count as walking in
    // a load: 1 waiting for its level to begin, 2 the level's actors put back (the player and the spawned next)
    uint8_t* load;
    uint32_t load_len;
    int load_stage;
    char load_slot[SAVE_SLOT_NAME];
    double load_t0, load_apply_ms;
    // a save waiting for its picture
    Buf save;
    bool save_pending, save_written;
    int save_wait;
    char save_slot[SAVE_SLOT_NAME];
    uint32_t save_flags;
    // the pictures read back (the slot list)
    struct { char slot[SAVE_SLOT_NAME]; uint8_t px[SAVE_THUMB_W * SAVE_THUMB_H * 3]; uint32_t w, h, gen; int64_t mtime; bool has; } thumbs[8];
    uint32_t thumb_next, gen;
    char dir[600];
    ProgressStats st;
} P = { .interval = -1.0f };
static void drop_picture(void);
static void picture_arrived(void);

// --- Little helpers ---------------------------------------------------------------------------------------------------

static uint64_t fnv64(const uint8_t* d, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ d[i]) * 0x100000001B3ull;
    return h;
}
static uint64_t file_checksum(const uint8_t* d, uint32_t n) { // (bytes 24..31, the checksum itself, as zeros)
    static const uint8_t zero[8] = { 0 };
    uint64_t h = fnv64(d, n < 24 ? n : 24, 0xCBF29CE484222325ull);
    if (n <= 24) return h;
    h = fnv64(zero, n < 32 ? n - 24 : 8, h);
    return n > 32 ? fnv64(d + 32, n - 32, h) : h;
}
static uint32_t fnv32(const char* s) {
    uint32_t h = 2166136261u;
    for (; s && *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static bool buf_need(Buf* b, uint32_t more) {
    if (b->bad) return false;
    if (b->n + more <= b->cap) return true;
    uint32_t cap = b->cap ? b->cap : 64u * 1024u;
    while (cap < b->n + more) cap *= 2;
    uint8_t* p = cap <= MAX_FILE ? realloc(b->p, cap) : NULL; // (a save, not a frame)
    if (!p) return b->bad = true, false;
    b->p = p, b->cap = cap;
    return true;
}
static void put(Buf* b, const void* d, uint32_t n) {
    if (!buf_need(b, n)) return;
    if (d) memcpy(b->p + b->n, d, n);
    else memset(b->p + b->n, 0, n);
    b->n += n;
}
static void put32(Buf* b, uint32_t v) { put(b, &v, 4); }
static void putf(Buf* b, float v) { put(b, &v, 4); }
static uint32_t chunk_begin(Buf* b, uint32_t tag) {
    put32(b, tag), put32(b, 0);
    return b->n;
}
static void chunk_end(Buf* b, uint32_t at) {
    if (b->bad) return;
    uint32_t sz = b->n - at;
    memcpy(b->p + at - 4, &sz, 4);
}

static bool get(Rd* r, void* out, uint32_t n) {
    if (r->bad || r->at + n > r->n || r->at + n < r->at) {
        r->bad = true;
        if (out) memset(out, 0, n);
        return false;
    }
    if (out) memcpy(out, r->p + r->at, n);
    r->at += n;
    return true;
}
static uint32_t get32(Rd* r) {
    uint32_t v = 0;
    get(r, &v, 4);
    return v;
}
static float getf(Rd* r) {
    float v = 0.0f;
    get(r, &v, 4);
    return isfinite(v) ? v : 0.0f;
}

// A chunk's bytes (the first of its tag); false: none
static bool find_chunk(const uint8_t* d, uint32_t n, uint32_t hdr, uint32_t tag, Rd* out) {
    uint32_t at = hdr;
    while (at + 8 <= n) {
        uint32_t t, sz;
        memcpy(&t, d + at, 4), memcpy(&sz, d + at + 4, 4);
        at += 8;
        if (sz > n - at) return false;
        if (t == tag) {
            *out = (Rd){ d + at, sz, 0, false };
            return true;
        }
        at += sz; // (an unknown chunk: a newer engine's, skipped)
    }
    return false;
}

// "Slot 1/../x" -> "Slot 1_.._x": a file name, never a path
static void clean_slot(const char* s, char* out, size_t cap) {
    size_t o = 0;
    for (; s && *s && o + 1 < cap; s++) {
        char c = *s;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ' || (c == '.' && o > 0);
        out[o++] = ok ? c : '_';
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;
    out[o] = 0;
}

const char* progress_dir(void) {
    const char* env = getenv("REALENGINE_SAVE_DIR"); // (the tests: their own folder)
    snprintf(P.dir, sizeof P.dir, "%s", env && env[0] ? env : "Saved/SaveGames"); // (the game's folder: the working one; Android: the app's files)
    return P.dir;
}
void progress_slot_path(const char* slot, char* out, size_t cap) {
    char s[SAVE_SLOT_NAME];
    clean_slot(slot, s, sizeof s);
    snprintf(out, cap, "%s/%s.sav", progress_dir(), s);
}

void progress_format_date(int64_t t, char* out, size_t cap) {
    time_t tt = (time_t)t;
    struct tm lt;
#ifdef _WIN32
    if (localtime_s(&lt, &tt) != 0) memset(&lt, 0, sizeof lt);
#else
    if (!localtime_r(&tt, &lt)) memset(&lt, 0, sizeof lt);
#endif
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
}
void progress_format_duration(double s, char* out, size_t cap) {
    long t = s > 0.0 ? (long)s : 0;
    if (t >= 3600) snprintf(out, cap, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
    else snprintf(out, cap, "%ld:%02ld", t / 60, t % 60);
}

// --- Reading and writing a slot -------------------------------------------------------------------------------------------

static void header_fill(Buf* b, uint32_t kind, uint32_t flags, uint32_t actors, const char* display, const char* level) {
    if (b->bad || b->n < HEADER_BYTES) return;
    uint8_t* h = b->p;
    memset(h, 0, HEADER_BYTES);
    memcpy(h, SAVE_MAGIC, 8);
    uint32_t v[7] = { SAVE_VERSION, HEADER_BYTES, kind, flags, 0, 0, actors };
    memcpy(h + 8, &v[0], 16);
    memcpy(h + 36, &v[6], 4);
    int64_t now = re_unix_time();
    double pt = P.play_time;
    memcpy(h + 40, &now, 8), memcpy(h + 48, &pt, 8);
    snprintf((char*)h + 64, SAVE_DISPLAY_NAME, "%s", display ? display : "");
    snprintf((char*)h + 128, SAVE_LEVEL_NAME, "%s", level ? level : "");
}

// The checksum in, then the file (a temp file replaced: a crash mid-save keeps the old one) and Steam Cloud's copy
// The disk's part on a thread of its own (a temp file written, then put in place with a write-through: ~4 ms the game
// never waits for); one at a time, and every read of the saves' folder waits for it first (writer_wait)
static struct { ReThread* t; uint8_t* data; uint32_t n; char dir[600], path[700], tmp[720]; volatile int ok; double ms; } g_wr;
static int writer(void* arg) {
    (void)arg;
    double t0 = re_time_seconds();
    re_make_dirs(g_wr.dir); // (~1 ms of the file system's calls: here, not in the frame)
    FILE* f = re_fopen(g_wr.tmp, "wb");
    bool ok = f && fwrite(g_wr.data, 1, g_wr.n, f) == g_wr.n;
    if (f) ok = fclose(f) == 0 && ok;
    ok = ok && re_replace_file(g_wr.tmp, g_wr.path);
    if (!ok) remove(g_wr.tmp);
    g_wr.ms = (re_time_seconds() - t0) * 1000.0;
    g_wr.ok = ok ? 1 : 0;
    return 0;
}
static void writer_wait(void) {
    if (!g_wr.t) return;
    re_thread_join(g_wr.t);
    g_wr.t = NULL;
    P.st.disk_ms = g_wr.ms;
    if (!g_wr.ok) printf("[RealEngine] Save Game: %s could not be written\n", g_wr.path);
}

static bool write_file(const char* slot, Buf* b) {
    if (b->bad || b->n < HEADER_BYTES) return false;
    uint32_t payload = b->n - HEADER_BYTES;
    memcpy(b->p + 32, &payload, 4);
    uint64_t sum = file_checksum(b->p, b->n);
    memcpy(b->p + 24, &sum, 8);
    writer_wait(); // (the last one first: its file in place)
    snprintf(g_wr.dir, sizeof g_wr.dir, "%s", progress_dir());
    progress_slot_path(slot, g_wr.path, sizeof g_wr.path);
    snprintf(g_wr.tmp, sizeof g_wr.tmp, "%s.tmp", g_wr.path);
    if (g_wr.n < b->n || !g_wr.data) { // (the writer's own copy: the save's buffer goes on: its picture)
        uint8_t* p = realloc(g_wr.data, b->n);
        if (!p) return false;
        g_wr.data = p;
    }
    memcpy(g_wr.data, b->p, b->n);
    g_wr.n = b->n, g_wr.ok = 0;
    static bool hooked;
    if (!hooked) hooked = true, atexit(writer_wait); // (the game closing: the last save on disk first)
    g_wr.t = re_thread_start(writer, NULL);
    bool ok = true;
    if (!g_wr.t) writer(NULL), ok = g_wr.ok != 0; // (no thread: here)
    if (online_cloud_saves()) { // and into Steam Cloud (the same file name: another PC's progress)
        char s[SAVE_SLOT_NAME], cloud[SAVE_SLOT_NAME + 8];
        clean_slot(slot, s, sizeof s);
        snprintf(cloud, sizeof cloud, "%s.sav", s);
        ok = steam_cloud_write(cloud, b->p, (int)b->n) || ok;
    }
    for (int i = 0; i < 8; i++) // (its picture read again next time)
        if (!strcmp(P.thumbs[i].slot, slot)) P.thumbs[i].slot[0] = 0;
    P.gen++;
    if (ok) P.st.bytes = b->n;
    return ok;
}

// The slot's bytes (Steam Cloud's first when on); malloc'd (a load, not a frame). NULL: none
static uint8_t* read_raw(const char* slot, uint32_t* len) {
    writer_wait();
    char s[SAVE_SLOT_NAME];
    clean_slot(slot, s, sizeof s);
    *len = 0;
    if (!s[0]) return NULL;
    if (online_cloud_saves()) {
        char cloud[SAVE_SLOT_NAME + 8];
        snprintf(cloud, sizeof cloud, "%s.sav", s);
        int n = steam_cloud_size(cloud);
        uint8_t* d = n > 0 && (uint32_t)n <= MAX_FILE ? malloc((size_t)n) : NULL;
        if (d && steam_cloud_read(cloud, d, n) == n) return *len = (uint32_t)n, d;
        free(d);
    }
    char path[700];
    progress_slot_path(s, path, sizeof path);
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = n > 0 && (unsigned long)n <= MAX_FILE ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) free(d), d = NULL;
    fclose(f);
    if (d) *len = (uint32_t)n;
    return d;
}

// What a file is: false when it is refused (too short, not ours, a newer version, the checksum wrong, the chunks broken).
// A version 1 file is migrated: *mig gets a version 2 save of kind Object holding its variables (malloc'd)
static bool check_file(const uint8_t* d, uint32_t n, SaveSlotInfo* info, uint8_t** mig, uint32_t* mig_len, const char** why) {
    static const char* dummy;
    if (!why) why = &dummy;
    *why = "";
    if (mig) *mig = NULL;
    if (n >= 8 && !memcmp(d, LEGACY_MAGIC, 8)) { // version 1: the variables' stream only (no metadata, no checksum)
        if (info) memset(info, 0, sizeof *info), info->version = 1, info->kind = SAVE_KIND_OBJECT, info->bytes = n;
        if (mig) { // into version 2: a header and one VARS chunk with the stream as it was
            Buf b = { 0 };
            put(&b, NULL, HEADER_BYTES);
            uint32_t c = chunk_begin(&b, T_VARS);
            put(&b, d, n);
            chunk_end(&b, c);
            double keep = P.play_time;
            P.play_time = 0.0;
            header_fill(&b, SAVE_KIND_OBJECT, 0, 0, "", "");
            P.play_time = keep;
            if (b.bad) {
                free(b.p);
                return *why = "out of memory", false;
            }
            memset(b.p + 40, 0, 8); // (no date: unknown)
            uint32_t payload = b.n - HEADER_BYTES;
            memcpy(b.p + 32, &payload, 4);
            uint64_t sum = file_checksum(b.p, b.n);
            memcpy(b.p + 24, &sum, 8);
            *mig = b.p, *mig_len = b.n;
        }
        return true;
    }
    if (n < HEADER_BYTES || memcmp(d, SAVE_MAGIC, 8)) return *why = "not a save of this engine", false;
    uint32_t v[7];
    memcpy(v, d + 8, 28);
    uint32_t version = v[0], hdr = v[1], payload = v[6];
    if (version > SAVE_VERSION) return *why = "made by a newer version of the game", false;
    if (version < 2 || hdr < HEADER_BYTES || hdr > n || payload != n - hdr) return *why = "damaged (its size)", false;
    uint64_t sum;
    memcpy(&sum, d + 24, 8);
    if (sum != file_checksum(d, n)) return *why = "damaged (the checksum does not match)", false;
    // the chunks must chain to the end exactly
    uint32_t at = hdr;
    while (at + 8 <= n) {
        uint32_t sz;
        memcpy(&sz, d + at + 4, 4);
        if (sz > n - at - 8) return *why = "damaged (a chunk)", false;
        at += 8 + sz;
    }
    if (at != n) return *why = "damaged (its end)", false;
    if (info) {
        memset(info, 0, sizeof *info);
        info->version = version, info->kind = v[2], info->flags = v[3], info->bytes = n;
        memcpy(&info->actors, d + 36, 4);
        memcpy(&info->time, d + 40, 8), memcpy(&info->play_time, d + 48, 8);
        memcpy(&info->thumb_w, d + 56, 2), memcpy(&info->thumb_h, d + 58, 2);
        memcpy(info->display, d + 64, SAVE_DISPLAY_NAME), info->display[SAVE_DISPLAY_NAME - 1] = 0;
        memcpy(info->level, d + 128, SAVE_LEVEL_NAME), info->level[SAVE_LEVEL_NAME - 1] = 0;
        if (!isfinite(info->play_time) || info->play_time < 0.0) info->play_time = 0.0;
    }
    return true;
}

// The slot read and checked (a version 1 file migrated); *len its bytes. NULL: missing or refused (said why, once)
static uint8_t* read_slot(const char* slot, uint32_t* len, SaveSlotInfo* info) {
    progress_flush(); // (a save waiting for its picture: on disk first)
    uint32_t n = 0;
    uint8_t* d = read_raw(slot, &n);
    if (!d) return NULL;
    uint8_t* mig = NULL;
    uint32_t mlen = 0;
    const char* why = "";
    if (!check_file(d, n, info, &mig, &mlen, &why)) {
        printf("[RealEngine] Save Game: the slot \"%s\" is refused: %s\n", slot, why);
        P.st.refused++;
        free(d);
        return NULL;
    }
    if (mig) {
        free(d);
        d = mig, n = mlen;
    }
    *len = n;
    return d;
}

bool progress_slot_info(const char* slot, SaveSlotInfo* out) {
    SaveSlotInfo si;
    uint32_t n = 0;
    progress_flush();
    uint8_t* d = read_raw(slot, &n);
    bool ok = d && check_file(d, n, &si, NULL, NULL, NULL);
    free(d);
    if (!ok) return false;
    clean_slot(slot, si.slot, sizeof si.slot);
    if (out) *out = si;
    return true;
}

bool progress_slot_exists(const char* slot) {
    writer_wait();
    char path[700], s[SAVE_SLOT_NAME], cloud[SAVE_SLOT_NAME + 8];
    clean_slot(slot, s, sizeof s);
    if (!s[0]) return false;
    if (P.save_pending && !strcmp(P.save_slot, s)) return true;
    progress_slot_path(s, path, sizeof path);
    snprintf(cloud, sizeof cloud, "%s.sav", s);
    return re_file_stat(path, NULL, NULL, NULL) || (online_cloud_saves() && steam_cloud_exists(cloud));
}

bool progress_slot_delete(const char* slot) {
    writer_wait();
    char path[700], s[SAVE_SLOT_NAME], cloud[SAVE_SLOT_NAME + 8];
    clean_slot(slot, s, sizeof s);
    if (!s[0]) return false;
    if (P.save_pending && !strcmp(P.save_slot, s)) P.save_written = true, drop_picture(); // (a save waiting for its picture: never written)
    progress_slot_path(s, path, sizeof path);
    snprintf(cloud, sizeof cloud, "%s.sav", s);
    bool a = remove(path) == 0, b = online_cloud_saves() && steam_cloud_delete(cloud);
    for (int i = 0; i < 8; i++)
        if (!strcmp(P.thumbs[i].slot, s)) P.thumbs[i].slot[0] = 0;
    if (!strcmp(P.current, s)) P.current[0] = 0;
    P.gen++;
    return a || b;
}

typedef struct ListCtx { SaveSlotInfo* out; uint32_t cap, n; bool all; } ListCtx;
static void list_one(void* user, const ReDirEntry* e) {
    ListCtx* c = user;
    size_t l = strlen(e->name);
    if (e->is_dir || l < 5 || l - 4 >= SAVE_SLOT_NAME || _stricmp(e->name + l - 4, ".sav") || c->n >= c->cap) return;
    char slot[SAVE_SLOT_NAME];
    memcpy(slot, e->name, l - 4), slot[l - 4] = 0;
    char path[700];
    progress_slot_path(slot, path, sizeof path);
    FILE* f = re_fopen(path, "rb");
    if (!f) return;
    uint8_t h[HEADER_BYTES];
    size_t got = fread(h, 1, sizeof h, f);
    fclose(f);
    SaveSlotInfo si;
    memset(&si, 0, sizeof si);
    if (got >= 8 && !memcmp(h, LEGACY_MAGIC, 8)) si.version = 1, si.kind = SAVE_KIND_OBJECT, si.time = e->mtime;
    else if (got == HEADER_BYTES && !memcmp(h, SAVE_MAGIC, 8)) { // (the header only: listing stays cheap; Load checks the whole file)
        uint32_t v[4];
        memcpy(v, h + 8, 16);
        si.version = v[0], si.kind = v[2], si.flags = v[3];
        memcpy(&si.actors, h + 36, 4), memcpy(&si.time, h + 40, 8), memcpy(&si.play_time, h + 48, 8);
        memcpy(&si.thumb_w, h + 56, 2), memcpy(&si.thumb_h, h + 58, 2);
        memcpy(si.display, h + 64, SAVE_DISPLAY_NAME), si.display[SAVE_DISPLAY_NAME - 1] = 0;
        memcpy(si.level, h + 128, SAVE_LEVEL_NAME), si.level[SAVE_LEVEL_NAME - 1] = 0;
        if (!isfinite(si.play_time) || si.play_time < 0.0) si.play_time = 0.0;
    } else return;
    if (!c->all && si.kind != SAVE_KIND_PROGRESS) return;
    snprintf(si.slot, sizeof si.slot, "%s", slot);
    si.bytes = (uint32_t)e->size;
    c->out[c->n++] = si;
}
uint32_t progress_list(SaveSlotInfo* out, uint32_t cap, bool all) {
    progress_flush();
    writer_wait();
    ListCtx c = { out, cap, 0, all };
    if (!out || !cap) return 0;
    re_dir_walk(progress_dir(), list_one, &c);
    for (uint32_t i = 1; i < c.n; i++) { // the newest first (then by name: the same order each time)
        SaveSlotInfo k = out[i];
        uint32_t j = i;
        while (j > 0 && (out[j - 1].time < k.time || (out[j - 1].time == k.time && strcmp(out[j - 1].slot, k.slot) > 0))) out[j] = out[j - 1], j--;
        out[j] = k;
    }
    return c.n;
}

bool progress_latest(char* slot, size_t cap) {
    SaveSlotInfo one[SAVE_MAX_SLOTS];
    uint32_t n = progress_list(one, SAVE_MAX_SLOTS, false);
    if (slot && cap) slot[0] = 0;
    if (!n) return false;
    if (slot && cap) snprintf(slot, cap, "%s", one[0].slot);
    return true;
}

const uint8_t* progress_thumb(const char* slot, uint32_t* w, uint32_t* h, uint32_t* gen) {
    writer_wait();
    char s[SAVE_SLOT_NAME];
    clean_slot(slot, s, sizeof s);
    if (!s[0]) return NULL;
    char path[700];
    progress_slot_path(s, path, sizeof path);
    int64_t mt = 0;
    if (!re_file_stat(path, NULL, &mt, NULL)) return NULL;
    int k = -1;
    for (int i = 0; i < 8 && k < 0; i++)
        if (!strcmp(P.thumbs[i].slot, s) && P.thumbs[i].mtime == mt) k = i;
    if (k < 0) { // read: its header and its picture (the offset in the header), not the whole file
        k = (int)(P.thumb_next++ % 8u);
        snprintf(P.thumbs[k].slot, sizeof P.thumbs[k].slot, "%s", s);
        P.thumbs[k].mtime = mt, P.thumbs[k].has = false, P.thumbs[k].gen = ++P.gen;
        FILE* f = re_fopen(path, "rb");
        uint8_t hd[HEADER_BYTES];
        if (f && fread(hd, 1, sizeof hd, f) == sizeof hd && !memcmp(hd, SAVE_MAGIC, 8)) {
            uint16_t tw, th;
            uint32_t off;
            memcpy(&tw, hd + 56, 2), memcpy(&th, hd + 58, 2), memcpy(&off, hd + 60, 4);
            if (off && tw && th && tw <= SAVE_THUMB_W && th <= SAVE_THUMB_H && fseek(f, (long)off, SEEK_SET) == 0 &&
                fread(P.thumbs[k].px, 1, (size_t)tw * th * 3, f) == (size_t)tw * th * 3)
                P.thumbs[k].w = tw, P.thumbs[k].h = th, P.thumbs[k].has = true;
        }
        if (f) fclose(f);
    }
    if (!P.thumbs[k].has) return NULL;
    if (w) *w = P.thumbs[k].w;
    if (h) *h = P.thumbs[k].h;
    if (gen) *gen = P.thumbs[k].gen;
    return P.thumbs[k].px;
}

// --- An actor's (or a Save Game object's) variables: Save Game to Slot ------------------------------------------------------

// The stream Save Game to Slot always wrote: "RESAV001", the variables (bp_vars_dump), the Inventory's record
static uint32_t object_stream(World* w, Entity e, uint8_t* buf, uint32_t cap, bool save_only) {
    uint32_t n = bp_vars_dump_ex(w, e, buf, cap, save_only);
    if (!n && cap >= 8 && inv_slot_count(w, e)) memcpy(buf, LEGACY_MAGIC, 8), n = 8; // (no Blueprint, an Inventory)
    if (n) n = inv_save_append(w, e, buf, n, cap);
    bool actor = n != 0;
    if (!n && cap >= 8) memcpy(buf, LEGACY_MAGIC, 8), n = 8; // (nothing of the actor's: the conversations' record may still go)
    n = dlg_save_append(buf, n, cap); // what the conversations said and chose (dialogue.c)
    n = quest_save_append(buf, n, cap); // the Quest Logs (quest.c)
    return actor || n > 8 ? n : 0;
}

bool progress_save_object(World* w, Entity e, const char* slot) {
    char s[SAVE_SLOT_NAME];
    clean_slot(slot, s, sizeof s);
    if (!s[0] || !w || !ecs_alive(w, e)) return false;
    static uint8_t vars[VARS_CAP];
    uint32_t n = object_stream(w, e, vars, sizeof vars, false);
    if (!n) return false;
    Buf b = { 0 };
    put(&b, NULL, HEADER_BYTES);
    uint32_t c = chunk_begin(&b, T_VARS);
    put(&b, vars, n);
    chunk_end(&b, c);
    const CBlueprint* bp = ecs_get(w, e, COMP_BLUEPRINT);
    const BpAsset* cls = bp ? bp_find_hash(bp->asset_hash) : NULL;
    header_fill(&b, SAVE_KIND_OBJECT, 0, 1, cls ? cls->name : "", level_stream_persistent());
    bool ok = write_file(s, &b);
    free(b.p);
    return ok;
}

bool progress_load_object(World* w, Entity e, const char* slot) {
    if (!w || !ecs_alive(w, e)) return false;
    uint32_t n = 0;
    SaveSlotInfo si;
    uint8_t* d = read_slot(slot, &n, &si);
    if (!d) return false;
    Rd r;
    bool ok = false;
    if (find_chunk(d, n, HEADER_BYTES, T_VARS, &r)) { // (an actor's / a Save Game object's)
        bool inv = inv_save_read(w, e, r.p, r.n), dlg = dlg_save_read(r.p, r.n); // (its Inventory's, the conversations')
        bool qst = quest_save_read(r.p, r.n); // (the Quest Logs)
        ok = bp_vars_load(w, e, r.p, r.n) || inv || dlg || qst;
    } else if (find_chunk(d, n, HEADER_BYTES, T_SGOB, &r) && r.n > 8) { // a progress: its Save Game object's variables
        get32(&r);
        uint32_t len = get32(&r);
        ok = len && len <= r.n - r.at && bp_vars_load(w, e, r.p + r.at, len);
    }
    free(d);
    return ok;
}

static bool is_save_game_class(const BpAsset* c) { return c && c->kind == BPA_CLASS && bp_native_of(c) == BPN_SAVE_GAME; }

Entity progress_create_save_object(World* w, uint32_t class_hash) {
    BpAsset* c = bp_find_hash(class_hash);
    if (!w || !is_save_game_class(c)) {
        if (class_hash) printf("[RealEngine] Create Save Game Object: %s is not a Save Game class\n", c ? c->name : "the class");
        return ENTITY_NONE;
    }
    if (!c->compiled && !bp_compile(c)) return ENTITY_NONE;
    static const float zero[3] = { 0, 0, 0 };
    return bp_spawn(w, c, zero, zero);
}

Entity progress_save_game(World* w) {
    if (!P.session) return ENTITY_NONE; // (only while playing: never an actor in the editor's level)
    if (w && P.sg && ecs_alive(w, P.sg)) return P.sg;
    P.sg = ENTITY_NONE;
    if (!P.sg_class) {
        const char* name = project()->save_game_class;
        BpAsset* c = name[0] ? bp_find_name(BPA_CLASS, name) : NULL;
        if (!is_save_game_class(c)) return ENTITY_NONE;
        P.sg_class = c->hash, P.sg_len = 0;
    }
    P.sg = progress_create_save_object(w, P.sg_class);
    if (P.sg && P.sg_len) bp_vars_load(w, P.sg, P.sg_blob, P.sg_len); // (its variables from the last level / the load)
    return P.sg;
}

bool progress_set_save_game(World* w, Entity obj) {
    const CBlueprint* b = w && ecs_alive(w, obj) ? ecs_get(w, obj, COMP_BLUEPRINT) : NULL;
    if (!b || !is_save_game_class(bp_find_hash(b->asset_hash))) return false;
    P.sg = obj, P.sg_class = b->asset_hash, P.sg_len = 0;
    return true;
}

// --- The level's Saveables: their keys ----------------------------------------------------------------------------------

static uint32_t ht_slot(uint32_t key) { return (key * 2654435761u) & (HT_SIZE - 1); }
static int32_t key_find(uint32_t key) {
    for (uint32_t s = ht_slot(key), k = 0; k < HT_SIZE; s = (s + 1) & (HT_SIZE - 1), k++) {
        uint32_t i = P.ht[s];
        if (!i) return -1;
        if (P.keys[i - 1] == key) return (int32_t)(i - 1);
    }
    return -1;
}
static bool key_add(uint32_t key, uint32_t flags) {
    if (P.nkeys >= MAX_KEYS) return false;
    uint32_t s = ht_slot(key);
    while (P.ht[s]) s = (s + 1) & (HT_SIZE - 1);
    P.keys[P.nkeys] = key, P.kflags[P.nkeys] = flags, P.seen[P.nkeys] = 0;
    P.ht[s] = ++P.nkeys;
    return true;
}
static uint32_t name_key(World* w, Entity e) {
    const CName* n = ecs_get(w, e, COMP_NAME);
    uint32_t k = fnv32(n ? n->text : "Actor");
    return k == 0 || k == PLAYER_KEY ? 1u : k;
}

// Every Saveable of the world (not the components of a Blueprint): its key, the same each time for the same level
static void assign_keys(World* w) {
    memset(P.ht, 0, sizeof P.ht);
    P.nkeys = 0;
    P.st.saveables = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_SAVEABLE)) || (a->mask & COMP_BIT(COMP_ATTACH)) || !a->count) continue;
        CSaveable* s = ecs_arch_column(a, COMP_SAVEABLE);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            uint32_t k = s[i].id ? s[i].id : name_key(w, ents[i]);
            while (k == 0 || k == PLAYER_KEY || key_find(k) >= 0) k = k * 2654435761u + 0x9E3779B9u; // (two alike: the later one moves on)
            s[i].key = key_add(k, s[i].flags) ? k : 0, s[i].spawned = 0;
            P.st.saveables++;
        }
    }
}

static Entity find_by_key(World* w, uint32_t key) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_SAVEABLE)) || (a->mask & COMP_BIT(COMP_ATTACH))) continue;
        const CSaveable* s = ecs_arch_column(a, COMP_SAVEABLE);
        for (uint32_t i = 0; i < a->count; i++)
            if (s[i].key == key) return ecs_arch_entities(a)[i];
    }
    return ENTITY_NONE;
}

// --- Saving the progress --------------------------------------------------------------------------------------------------

static void record(World* w, Buf* b, Entity e, uint32_t key, uint32_t want, bool spawned) {
    static uint8_t tmp[VARS_CAP];
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    const CHealth* h = (want & SAVEABLE_HEALTH) ? ecs_get(w, e, COMP_HEALTH) : NULL;
    const CCharacter* ch = ecs_get(w, e, COMP_CHARACTER);
    const CBlueprint* bp = ecs_get(w, e, COMP_BLUEPRINT);
    uint32_t parts = (t && (want & SAVEABLE_TRANSFORM) ? R_TRANSFORM : 0) | (h ? R_HEALTH : 0) | (t && (want & SAVEABLE_HIDDEN) ? R_HIDDEN : 0) |
                     (ch && (want & SAVEABLE_TRANSFORM) ? R_CONTROL : 0) | (spawned ? R_SPAWNED : 0);
    uint32_t inv_n = 0, var_n = 0;
    if ((want & SAVEABLE_INVENTORY) && inv_slot_count(w, e)) {
        memcpy(tmp, LEGACY_MAGIC, 8);
        inv_n = inv_save_append(w, e, tmp, 8, sizeof tmp);
        if (inv_n > 8) parts |= R_INVENTORY;
        else inv_n = 0;
    }
    put32(b, key), put32(b, bp ? bp->asset_hash : 0), put32(b, parts);
    uint32_t size_at = b->n;
    put32(b, 0);
    uint32_t start = b->n;
    if (parts & R_TRANSFORM) put(b, t->location, 12), put(b, t->rotation, 12), put(b, t->scale, 12);
    if (parts & R_CONTROL) putf(b, ch->control_yaw), putf(b, ch->control_pitch);
    if (parts & R_HEALTH) putf(b, h->max_health), putf(b, h->health), putf(b, h->max_shield), putf(b, h->shield), put32(b, h->flags & HEALTH_DEAD);
    if (parts & R_HIDDEN) put32(b, (t->flags & TRANSFORM_HIDDEN_GAME) ? 1u : 0u);
    if (parts & R_INVENTORY) put32(b, inv_n), put(b, tmp, inv_n);
    if ((want & (SAVEABLE_VARIABLES | SAVEABLE_ALL_VARIABLES)) && bp) { // (last: it reuses tmp)
        var_n = bp_vars_dump_ex(w, e, tmp, sizeof tmp, !(want & SAVEABLE_ALL_VARIABLES));
        if (var_n > 8) {
            put32(b, var_n), put(b, tmp, var_n);
            if (!b->bad) {
                uint32_t p2 = parts | R_VARS;
                memcpy(b->p + size_at - 4, &p2, 4);
            }
        }
    }
    if (!b->bad) {
        uint32_t sz = b->n - start;
        memcpy(b->p + size_at, &sz, 4);
    }
}

static void fire_saved(World* w, const char* slot, uint32_t flags) {
    BpValue a[2];
    memset(a, 0, sizeof a);
    bp_make_string(&a[0], slot);
    a[1].v[0] = (flags & (SAVE_FLAG_AUTO | SAVE_FLAG_CHECKPOINT)) ? 1.0f : 0.0f;
    bp_fire_event(w, BPK_EVENT_GAME_SAVED, a, 2);
}

// A save waits one frame for its picture, then its file is written once with it. A read of the slot before (a list, a
// load, Get Save Slot Info) writes it at once without the picture; the picture then comes as a second write. No picture
// after a few frames (a minimized window): written without it
static World* g_w; // (the world of the waiting save: its On Game Saved)

static bool first_write(void) {
    if (!P.save_pending || P.save_written) return true;
    P.save_written = true;
    double t0 = re_time_seconds();
    bool ok = write_file(P.save_slot, &P.save);
    P.st.write_ms = (re_time_seconds() - t0) * 1000.0;
    P.st.save_ms = P.st.build_ms + P.st.write_ms;
    if (!ok) {
        printf("[RealEngine] Save Game: the slot \"%s\" could not be written in %s\n", P.save_slot, progress_dir());
        return false;
    }
    P.st.saves++;
    if (P.save_flags & SAVE_FLAG_AUTO) P.st.auto_saves++;
    snprintf(P.current, sizeof P.current, "%s", P.save_slot);
    if (g_w) fire_saved(g_w, P.save_slot, P.save_flags);
    return true;
}

static void drop_picture(void) {
    if (P.save_pending) first_write(), free(P.save.p);
    memset(&P.save, 0, sizeof P.save);
    P.save_pending = P.save_written = false;
    if (P.vk) P.vk->thumb_req = false;
}

void progress_flush(void) {
    if (!P.save_pending) return;
    if (P.vk && P.vk->thumb_ready) picture_arrived();
    else first_write();
}

bool progress_save(World* w, const char* slot, const char* display, uint32_t flags) {
    char s[SAVE_SLOT_NAME];
    clean_slot(slot, s, sizeof s);
    if (!s[0] || !w) return false;
    progress_flush();
    double t0 = re_time_seconds();
    Buf b = { 0 };
    put(&b, NULL, HEADER_BYTES);
    // the progress: the play time, the checkpoint, the checkpoints reached
    uint32_t c = chunk_begin(&b, T_PROG);
    put(&b, &P.play_time, 8);
    put32(&b, P.cp_valid), put32(&b, P.cp_key), put32(&b, P.cp_flags), put(&b, P.cp_pos, 12), putf(&b, P.cp_yaw), putf(&b, P.cp_delay);
    put(&b, P.cp_slot, sizeof P.cp_slot);
    put32(&b, P.nreached), put(&b, P.reached, P.nreached * 4u);
    chunk_end(&b, c);
    // the Save Game object (its variables now; between levels: as kept)
    Entity sg = P.sg && ecs_alive(w, P.sg) ? P.sg : ENTITY_NONE;
    if (sg || (P.sg_class && P.sg_len)) {
        static uint8_t vars[VARS_CAP];
        uint32_t n = sg ? bp_vars_dump(w, sg, vars, sizeof vars) : P.sg_len;
        c = chunk_begin(&b, T_SGOB);
        put32(&b, P.sg_class), put32(&b, n), put(&b, sg ? vars : P.sg_blob, n);
        chunk_end(&b, c);
    }
    // the Saveables (the level's by their keys, the spawned by their class) and the player
    Entity player = bp_player(w);
    uint32_t actors = 0, spawned = 0, nabil = 0;
    static Entity abil_e[MAX_KEYS]; // (the keyed actors with an Ability System: their records in ABIL, ability.c)
    static uint32_t abil_k[MAX_KEYS];
    if (++P.seen_gen == 0) P.seen_gen = 1;
    c = chunk_begin(&b, T_ACTR);
    uint32_t count_at = b.n;
    put32(&b, 0);
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_SAVEABLE)) || (a->mask & COMP_BIT(COMP_ATTACH)) || !a->count) continue;
        const CSaveable* sv = ecs_arch_column(a, COMP_SAVEABLE);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            int32_t k = sv[i].key ? key_find(sv[i].key) : -1;
            if (k >= 0) P.seen[k] = P.seen_gen;
            if (ents[i] == player) continue; // (its own record, below)
            bool sp = k < 0; // (no key: spawned while playing)
            if (sp && !(a->mask & COMP_BIT(COMP_BLUEPRINT))) continue; // (nothing to spawn it again from)
            record(w, &b, ents[i], sp ? 0u : sv[i].key, sv[i].flags, sp);
            actors++, spawned += sp;
            if (!sp && (a->mask & COMP_BIT(COMP_ABILITY_SYSTEM)) && nabil < MAX_KEYS) abil_e[nabil] = ents[i], abil_k[nabil++] = sv[i].key;
        }
    }
    if (player) {
        const CSaveable* ps = ecs_get(w, player, COMP_SAVEABLE);
        record(w, &b, player, PLAYER_KEY, ps ? ps->flags : SAVEABLE_DEFAULT, false);
        actors++;
        if (nabil < MAX_KEYS) abil_e[nabil] = player, abil_k[nabil++] = PLAYER_KEY;
    }
    if (!b.bad) memcpy(b.p + count_at, &actors, 4);
    chunk_end(&b, c);
    // the destroyed: the level's Saveables (Destroyed kept) that are gone
    c = chunk_begin(&b, T_DEST);
    uint32_t dest_at = b.n, ndest = 0;
    put32(&b, 0);
    for (uint32_t k = 0; k < P.nkeys; k++)
        if (P.seen[k] != P.seen_gen && (P.kflags[k] & SAVEABLE_DESTROYED)) put32(&b, P.keys[k]), ndest++;
    if (!b.bad) memcpy(b.p + dest_at, &ndest, 4);
    chunk_end(&b, c);
    { // what the conversations said and chose (dialogue.c): its record as a VARS stream has it
        static uint8_t dm[8 + 4352];
        memcpy(dm, LEGACY_MAGIC, 8);
        uint32_t dn = dlg_save_append(dm, 8, sizeof dm);
        if (dn > 8) {
            c = chunk_begin(&b, T_DLGM);
            put(&b, dm, dn);
            chunk_end(&b, c);
        }
    }
    { // the Quest Logs (quest.c): its records as a VARS stream has them
        static uint8_t qm[VARS_CAP];
        memcpy(qm, LEGACY_MAGIC, 8);
        uint32_t qn = quest_save_append(qm, 8, sizeof qm);
        if (qn > 8) {
            c = chunk_begin(&b, T_QSTL);
            put(&b, qm, qn);
            chunk_end(&b, c);
        }
    }
    { // the match (game_rules.c): its phase, clock, teams, the player's score and lives
        uint8_t rm[512];
        uint32_t rn = rules_save_write(w, rm, sizeof rm);
        if (rn) {
            c = chunk_begin(&b, T_RULE);
            put(&b, rm, rn);
            chunk_end(&b, c);
        }
    }
    if (nabil) { // the Ability Systems (ability.c): written straight into the chunk
        static uint8_t ab[1u << 20];
        uint32_t an = abil_save_write(w, abil_e, abil_k, nabil, ab, sizeof ab);
        if (an > 8) {
            c = chunk_begin(&b, T_ABIL);
            put(&b, ab, an);
            chunk_end(&b, c);
        }
    }
    const char* level = level_stream_persistent();
    header_fill(&b, SAVE_KIND_PROGRESS, flags, actors, display && display[0] ? display : level, level);
    if (b.bad) {
        free(b.p);
        printf("[RealEngine] Save Game: out of memory saving \"%s\"\n", s);
        return false;
    }
    P.st.actors = actors, P.st.destroyed = ndest, P.st.spawned = spawned;
    drop_picture(); // (a save before it still waiting: written now)
    P.st.build_ms = (re_time_seconds() - t0) * 1000.0;
    P.save = b, P.save_pending = true, P.save_written = false, P.save_flags = flags, g_w = w;
    snprintf(P.save_slot, sizeof P.save_slot, "%s", s);
    if (P.vk && project()->save_thumbnail && P.vk->device) { // the picture from the next frame (the widgets hidden)
        P.vk->thumb_ready = false;
        memcpy(P.vk->thumb_rect, P.view, sizeof P.view);
        P.vk->thumb_req = true;
        P.save_wait = 4; // (frames: a window off the screen still renders; a minimized one gives up after these)
        return true;
    }
    bool ok = first_write(); // (no renderer: the file now)
    drop_picture();
    return ok;
}

// The picture came (vkctx: the frame copied, scaled to SAVE_THUMB_W x SAVE_THUMB_H): into the waiting save, as its last chunk
static void picture_arrived(void) {
    uint16_t tw = P.vk->thumb_w, th = P.vk->thumb_h;
    if (!P.save_pending || P.save.bad || P.save.n < HEADER_BYTES || !tw || !th || tw > SAVE_THUMB_W || th > SAVE_THUMB_H) {
        drop_picture();
        return;
    }
    uint32_t c = chunk_begin(&P.save, T_THMB);
    uint32_t off = P.save.n;
    put(&P.save, P.vk->thumb_px, (uint32_t)tw * th * 3u);
    chunk_end(&P.save, c);
    if (P.save.bad) {
        drop_picture();
        return;
    }
    memcpy(P.save.p + 56, &tw, 2), memcpy(P.save.p + 58, &th, 2), memcpy(P.save.p + 60, &off, 4);
    P.st.thumb_ms = P.vk->thumb_ms;
    P.vk->thumb_ready = false;
    if (!P.save_written) first_write(); // (the usual: one write, with its picture)
    else if (!write_file(P.save_slot, &P.save)) printf("[RealEngine] Save Game: the picture of \"%s\" could not be written\n", P.save_slot);
    drop_picture();
}

// --- Loading the progress -------------------------------------------------------------------------------------------------

static void apply_record(World* w, Entity e, uint32_t parts, Rd* r) {
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (parts & R_TRANSFORM) {
        float v[9];
        for (int k = 0; k < 9; k++) v[k] = getf(r);
        if (t && !r->bad) memcpy(t->location, v, 12), memcpy(t->rotation, v + 3, 12), memcpy(t->scale, v + 6, 12), ecs_touch(w, e);
    }
    if (parts & R_CONTROL) {
        float y = getf(r), p = getf(r);
        CCharacter* ch = ecs_get(w, e, COMP_CHARACTER);
        if (ch && !r->bad) ch->control_yaw = y, ch->control_pitch = p, memset(ch->velocity, 0, sizeof ch->velocity);
    }
    if (parts & R_HEALTH) {
        float mh = getf(r), hh = getf(r), ms = getf(r), sh = getf(r);
        uint32_t dead = get32(r);
        CHealth* h = ecs_get(w, e, COMP_HEALTH);
        if (h && !r->bad) {
            h->max_health = fmaxf(mh, 0.0f), h->health = fminf(fmaxf(hh, 0.0f), h->max_health);
            h->max_shield = fmaxf(ms, 0.0f), h->shield = fminf(fmaxf(sh, 0.0f), h->max_shield);
            h->flags = (h->flags & ~(HEALTH_DEAD | HEALTH_DESTROYING)) | (dead ? HEALTH_DEAD : 0u);
            h->since_damage = 1e6f;
        }
    }
    if (parts & R_HIDDEN) {
        uint32_t hid = get32(r);
        if (t && !r->bad) t->flags = hid ? (t->flags | TRANSFORM_HIDDEN_GAME) : (t->flags & ~TRANSFORM_HIDDEN_GAME);
    }
    if (parts & R_INVENTORY) {
        uint32_t n = get32(r);
        if (n <= r->n - r->at && !r->bad) inv_save_read(w, e, r->p + r->at, n);
        get(r, NULL, n);
    }
    if (parts & R_VARS) {
        uint32_t n = get32(r);
        if (n <= r->n - r->at && !r->bad) bp_vars_load(w, e, r->p + r->at, n);
        get(r, NULL, n);
    }
}

// Stage 1 -> 2, as the level begins (before BeginPlay): the progress, the Save Game object, the level's Saveables, the destroyed
static void apply_level(World* w) {
    double t0 = re_time_seconds();
    P.sg_len = 0; // (the save's Save Game, or a fresh one: never the last level's)
    const uint8_t* d = P.load;
    uint32_t n = P.load_len;
    Rd r;
    if (find_chunk(d, n, HEADER_BYTES, T_PROG, &r)) {
        double pt = 0.0;
        get(&r, &pt, 8);
        P.play_time = isfinite(pt) && pt >= 0.0 ? pt : 0.0;
        P.cp_valid = get32(&r) != 0, P.cp_key = get32(&r), P.cp_flags = get32(&r);
        for (int k = 0; k < 3; k++) P.cp_pos[k] = getf(&r);
        P.cp_yaw = getf(&r), P.cp_delay = getf(&r);
        get(&r, P.cp_slot, sizeof P.cp_slot), P.cp_slot[sizeof P.cp_slot - 1] = 0;
        uint32_t nr = get32(&r);
        P.nreached = 0;
        for (uint32_t i = 0; i < nr && !r.bad; i++) {
            uint32_t k = get32(&r);
            if (P.nreached < MAX_REACHED && !r.bad) P.reached[P.nreached++] = k;
        }
        if (r.bad) P.cp_valid = false, P.nreached = 0;
    }
    if (find_chunk(d, n, HEADER_BYTES, T_SGOB, &r)) {
        uint32_t cls = get32(&r), len = get32(&r);
        if (!r.bad && len <= r.n - r.at && len <= sizeof P.sg_blob && is_save_game_class(bp_find_hash(cls))) {
            memcpy(P.sg_blob, r.p + r.at, len), P.sg_len = len, P.sg_class = cls;
            if (P.sg && ecs_alive(w, P.sg)) bp_vars_load(w, P.sg, P.sg_blob, P.sg_len);
            else progress_save_game(w);
        }
    }
    if (find_chunk(d, n, HEADER_BYTES, T_ACTR, &r)) {
        uint32_t count = get32(&r);
        for (uint32_t i = 0; i < count && !r.bad; i++) {
            uint32_t key = get32(&r), cls = get32(&r), parts = get32(&r), size = get32(&r);
            (void)cls;
            if (r.bad || size > r.n - r.at) break;
            Rd one = { r.p + r.at, size, 0, false };
            r.at += size;
            if (key == PLAYER_KEY || (parts & R_SPAWNED)) continue; // (the next stage's: the player may not be there yet)
            Entity e = find_by_key(w, key);
            if (e) apply_record(w, e, parts, &one);
        }
    }
    if (find_chunk(d, n, HEADER_BYTES, T_DLGM, &r)) dlg_save_read(r.p, r.n); // (the conversations' memory)
    else dlg_memory_clear();
    if (find_chunk(d, n, HEADER_BYTES, T_RULE, &r)) rules_save_read(w, r.p, r.n); // (the match: applied when it starts, the first frame)
    if (!find_chunk(d, n, HEADER_BYTES, T_QSTL, &r) || !quest_save_read(r.p, r.n)) quest_clear(); // (the Quest Logs; none: nothing started)
    if (find_chunk(d, n, HEADER_BYTES, T_ABIL, &r)) abil_save_read(w, r.p, r.n, find_by_key, ENTITY_NONE, false); // (the level's Ability Systems)
    uint32_t ndest = 0;
    if (find_chunk(d, n, HEADER_BYTES, T_DEST, &r)) { // (before BeginPlay: theirs never runs)
        uint32_t count = get32(&r);
        for (uint32_t i = 0; i < count && !r.bad; i++) {
            Entity e = find_by_key(w, get32(&r));
            if (e && !r.bad) bp_destroy_actor(w, e, false), ndest++; // (no End Play: it never began)
        }
    }
    P.st.destroyed = ndest;
    P.load_stage = 2;
    P.load_apply_ms = (re_time_seconds() - t0) * 1000.0;
}

// Stage 2 -> done, the first frame after BeginPlay: the player, the spawned, the event
static void apply_late(World* w) {
    double t0 = re_time_seconds();
    const uint8_t* d = P.load;
    uint32_t n = P.load_len, spawned = 0, actors = 0;
    Rd r;
    Entity player = bp_player(w);
    if (find_chunk(d, n, HEADER_BYTES, T_ACTR, &r)) {
        uint32_t count = get32(&r);
        for (uint32_t i = 0; i < count && !r.bad; i++) {
            uint32_t key = get32(&r), cls = get32(&r), parts = get32(&r), size = get32(&r);
            if (r.bad || size > r.n - r.at) break;
            Rd one = { r.p + r.at, size, 0, false };
            r.at += size;
            actors++;
            Entity e = ENTITY_NONE;
            if (key == PLAYER_KEY) e = player;
            else if (parts & R_SPAWNED) { // spawned again by its class (where it was: its transform's first bytes)
                BpAsset* c = bp_find_hash(cls);
                float at[3] = { 0, 0, 0 }, rot[3] = { 0, 0, 0 };
                if ((parts & R_TRANSFORM) && size >= 24) memcpy(at, one.p, 12), memcpy(rot, one.p + 12, 12);
                e = c && (c->compiled || bp_compile(c)) ? bp_spawn(w, c, at, rot) : ENTITY_NONE;
                spawned += e != ENTITY_NONE;
            }
            if (e) apply_record(w, e, parts, &one);
        }
    }
    if (player && find_chunk(d, n, HEADER_BYTES, T_ABIL, &r)) abil_save_read(w, r.p, r.n, NULL, player, true); // (the player's Ability System)
    P.st.actors = actors, P.st.spawned = spawned;
    P.load_apply_ms += (re_time_seconds() - t0) * 1000.0;
    P.st.apply_ms = P.load_apply_ms;
    P.st.load_ms = P.load_t0 + P.load_apply_ms;
    P.st.loads++;
    snprintf(P.current, sizeof P.current, "%s", P.load_slot);
    free(P.load);
    P.load = NULL, P.load_len = 0, P.load_stage = 0;
    P.prime_inside = true, P.dead_time = 0.0f;
    BpValue a[1];
    memset(a, 0, sizeof a);
    bp_make_string(&a[0], P.current);
    bp_fire_event(w, BPK_EVENT_GAME_LOADED, a, 1);
}

bool progress_load(World* w, const char* slot) {
    char s[SAVE_SLOT_NAME];
    clean_slot(slot, s, sizeof s);
    if (!s[0] || !w) return false;
    double t0 = re_time_seconds();
    uint32_t n = 0;
    SaveSlotInfo si;
    uint8_t* d = read_slot(s, &n, &si);
    if (!d) return false;
    if (si.kind != SAVE_KIND_PROGRESS) { // an actor's variables (an old save): onto the player (and the Save Game object)
        Rd r;
        bool ok = false;
        Entity pl = bp_player(w);
        if (find_chunk(d, n, HEADER_BYTES, T_VARS, &r)) {
            if (pl) ok = bp_vars_load(w, pl, r.p, r.n) | inv_save_read(w, pl, r.p, r.n);
            ok = dlg_save_read(r.p, r.n) || ok; // (the conversations' memory)
            ok = quest_save_read(r.p, r.n) || ok; // (the Quest Logs)
            Entity sg = P.sg && ecs_alive(w, P.sg) ? P.sg : ENTITY_NONE;
            if (sg) ok = bp_vars_load(w, sg, r.p, r.n) || ok;
        }
        free(d);
        if (ok) snprintf(P.current, sizeof P.current, "%s", s), P.st.loads++;
        return ok;
    }
    free(P.load);
    P.load = d, P.load_len = n, P.load_stage = 1;
    snprintf(P.load_slot, sizeof P.load_slot, "%s", s);
    P.load_t0 = (re_time_seconds() - t0) * 1000.0; // (the read and the checksum)
    // its level opened (the same one too: the world as the level file has it, then the save on top)
    char path[700];
    if (si.level[0] && level_find(si.level, path, sizeof path)) {
        level_open_request(si.level);
        if (level_open_pending()) return true;
    }
    // no such level (an editor level never saved): the state put on this world as it is
    printf("[RealEngine] Load Progress: the level \"%s\" is not in the project: the save is put on this level\n", si.level);
    assign_keys(w);
    apply_level(w);
    return true;
}

bool progress_new_game(World* w, const char* level) {
    if (!w) return false;
    progress_flush();
    P.play_time = 0.0, P.cp_valid = false, P.nreached = 0, P.current[0] = 0, P.auto_timer = 0.0, P.dead_time = 0.0f;
    free(P.load);
    P.load = NULL, P.load_len = 0, P.load_stage = 0;
    if (P.sg && ecs_alive(w, P.sg)) bp_destroy_actor(w, P.sg, true); // (a fresh one, its defaults, when asked for)
    P.sg = ENTITY_NONE, P.sg_len = 0;
    dlg_memory_clear(); // (nothing said yet)
    quest_clear(); // (no quest started)
    const char* go = level && level[0] ? level : level_stream_persistent();
    if (go && go[0]) level_open_request(go);
    return true;
}

bool progress_auto_save(World* w) {
    Entity pl = bp_player(w);
    if (!w || (pl && health_is_dead(w, pl)) || P.load_stage) return false; // (never a dead player's, nor mid-load)
    int slots = project()->autosave_slots < 1 ? 1 : project()->autosave_slots > 5 ? 5 : project()->autosave_slots;
    char slot[32], display[96];
    snprintf(slot, sizeof slot, "AutoSave_%d", P.auto_next % slots + 1);
    P.auto_next = (P.auto_next + 1) % slots;
    snprintf(display, sizeof display, "%s (Auto Save)", level_stream_persistent());
    P.auto_timer = 0.0;
    return progress_save(w, slot, display, SAVE_FLAG_AUTO);
}

double progress_play_time(void) { return P.play_time; }
const char* progress_current_slot(void) { return P.current; }
void progress_set_autosave_interval(float seconds) { P.interval = seconds; }

// --- Checkpoints ------------------------------------------------------------------------------------------------------------

static bool reached(uint32_t key) {
    for (uint32_t i = 0; i < P.nreached; i++)
        if (P.reached[i] == key) return true;
    return false;
}
static void spawn_point(World* w, Entity cp, float pos[3], float* yaw) {
    const CTransform* t = ecs_get(w, cp, COMP_TRANSFORM);
    const CCheckpoint* c = ecs_get(w, cp, COMP_CHECKPOINT);
    float y = t ? t->rotation[1] * 0.017453292f : 0.0f, cy = cosf(y), sy = sinf(y);
    const float* o = c ? c->spawn_offset : (const float[3]){ 0, 0, 0 };
    float base[3] = { 0, 0, 0 };
    if (t) memcpy(base, t->location, 12);
    pos[0] = base[0] + o[0] * cy - o[1] * sy, pos[1] = base[1] + o[0] * sy + o[1] * cy, pos[2] = base[2] + o[2];
    *yaw = t ? t->rotation[1] : 0.0f;
}
static void mark(World* w, Entity cp) {
    const CCheckpoint* c = ecs_get(w, cp, COMP_CHECKPOINT);
    uint32_t key = name_key(w, cp);
    P.cp_valid = true, P.cp_key = key, P.cp_flags = c ? c->flags : 0u, P.cp_delay = c ? c->respawn_delay : 2.0f;
    snprintf(P.cp_slot, sizeof P.cp_slot, "%s", c && c->slot[0] ? c->slot : "Checkpoint");
    spawn_point(w, cp, P.cp_pos, &P.cp_yaw);
    if (!reached(key) && P.nreached < MAX_REACHED) P.reached[P.nreached++] = key;
}

Entity progress_checkpoint(World* w) {
    if (!w || !P.cp_valid) return ENTITY_NONE;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_CHECKPOINT))) continue;
        for (uint32_t i = 0; i < a->count; i++)
            if (name_key(w, ecs_arch_entities(a)[i]) == P.cp_key) return ecs_arch_entities(a)[i];
    }
    return ENTITY_NONE;
}

bool progress_set_checkpoint(World* w, Entity cp) {
    if (!w || !ecs_alive(w, cp) || !ecs_get(w, cp, COMP_CHECKPOINT)) return false;
    mark(w, cp);
    return true;
}

bool progress_respawn(World* w, Entity player, bool reload) {
    if (!w) return false;
    if (reload && P.cp_valid && (P.cp_flags & CHECKPOINT_SAVE) && progress_slot_exists(P.cp_slot)) return progress_load(w, P.cp_slot);
    if (!player || !ecs_alive(w, player)) return false;
    CTransform* t = ecs_get(w, player, COMP_TRANSFORM);
    if (!t || (!P.cp_valid && !P.start_known)) return false;
    float pos[3], yaw = P.cp_valid ? P.cp_yaw : P.start_yaw;
    memcpy(pos, P.cp_valid ? P.cp_pos : P.start_pos, 12);
    CCharacter* ch = ecs_get(w, player, COMP_CHARACTER);
    if (ch && P.cp_valid) pos[2] += ch->half_height + 2.0f; // (the offset is the feet: the capsule's center above them)
    memcpy(t->location, pos, 12), t->rotation[1] = yaw;
    if (ch) ch->control_yaw = yaw, memset(ch->velocity, 0, sizeof ch->velocity);
    ecs_touch(w, player);
    if (ecs_get(w, player, COMP_HEALTH)) health_revive(w, player, 0.0f);
    P.dead_time = 0.0f;
    return true;
}

static void checkpoints(World* w, Entity player) {
    const CTransform* pt = player ? ecs_get(w, player, COMP_TRANSFORM) : NULL;
    uint32_t n = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_CHECKPOINT) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need || !a->count) continue;
        CCheckpoint* c = ecs_arch_column(a, COMP_CHECKPOINT);
        const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        n += a->count;
        for (uint32_t i = 0; i < a->count && pt; i++) {
            float d[3] = { pt->location[0] - t[i].location[0], pt->location[1] - t[i].location[1], pt->location[2] - t[i].location[2] };
            float y = -t[i].rotation[1] * 0.017453292f, cy = cosf(y), sy = sinf(y); // (into the box's axes: its yaw)
            float lx = d[0] * cy - d[1] * sy, ly = d[0] * sy + d[1] * cy;
            bool in = fabsf(lx) <= c[i].extent[0] * fabsf(t[i].scale[0]) && fabsf(ly) <= c[i].extent[1] * fabsf(t[i].scale[1]) &&
                      fabsf(d[2]) <= c[i].extent[2] * fabsf(t[i].scale[2]);
            bool was = (c[i].reached & 2u) != 0;
            c[i].reached = (c[i].reached & ~2u) | (in ? 2u : 0u);
            if (!in || was || P.prime_inside) continue;
            uint32_t key = name_key(w, ents[i]);
            bool again = reached(key);
            if (again && (c[i].flags & CHECKPOINT_ONCE)) continue;
            c[i].reached |= 1u;
            mark(w, ents[i]);
            BpValue args[2];
            memset(args, 0, sizeof args);
            memcpy(&args[0].v[0], &ents[i], 4); // (an actor value: the entity's bits, as the VM's set_ent writes)
            memcpy(&args[1].v[0], &player, 4);
            if (c[i].flags & CHECKPOINT_SAVE) {
                char display[96];
                snprintf(display, sizeof display, "%s (Checkpoint)", level_stream_persistent());
                progress_save(w, P.cp_slot, display, SAVE_FLAG_CHECKPOINT);
            }
            bp_fire_event(w, BPK_EVENT_CHECKPOINT, args, 2);
        }
    }
    P.st.checkpoints = n;
    P.prime_inside = false;
}

// --- The engine's hooks ---------------------------------------------------------------------------------------------------

void progress_set_vk(VkCtx* vk) { P.vk = vk; }
void progress_set_view_rect(float x0, float y0, float x1, float y1) { P.view[0] = x0, P.view[1] = y0, P.view[2] = x1, P.view[3] = y1; }
bool progress_hide_ui(void) { return P.vk && P.vk->thumb_req; }

void progress_begin_play(World* w) {
    if (!P.session) { // a new Play / the game started: nothing from before
        P.session = true, P.began_once = false;
        P.play_time = 0.0, P.cp_valid = false, P.nreached = 0, P.current[0] = 0, P.auto_next = 0, P.interval = -1.0f;
        P.sg = ENTITY_NONE, P.sg_class = 0, P.sg_len = 0;
    }
    P.auto_timer = 0.0, P.dead_time = 0.0f, P.start_known = false;
    assign_keys(w);
    // the Save Game object back, as the last level left it (a load's own comes with the load)
    P.sg = ENTITY_NONE;
    if (P.sg_class && P.load_stage != 1) progress_save_game(w);
    if (P.load_stage == 1) apply_level(w);
    // the checkpoints reached (this level's: the Once ones don't save again)
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_CHECKPOINT))) continue;
        CCheckpoint* c = ecs_arch_column(a, COMP_CHECKPOINT);
        for (uint32_t i = 0; i < a->count; i++) c[i].reached = reached(name_key(w, ecs_arch_entities(a)[i])) ? 1u : 0u;
    }
    P.auto_pending = P.began_once && P.load_stage == 0 && project()->autosave_on_level;
    P.began_once = true;
}

void progress_update(World* w, float dt) {
    double t0 = re_time_seconds();
    if (P.save_pending) { // the picture of the frame drawn since the save (or none after a few frames)
        if (P.vk && P.vk->thumb_ready) picture_arrived();
        else if (--P.save_wait <= 0) drop_picture();
    }
    if (!w) return;
    if (P.load_stage == 2) apply_late(w);
    Entity player = bp_player(w);
    if (player && !P.start_known) { // (where the level started it: a respawn without a checkpoint)
        const CTransform* t = ecs_get(w, player, COMP_TRANSFORM);
        if (t) memcpy(P.start_pos, t->location, 12), P.start_yaw = t->rotation[1], P.start_known = true;
    }
    if (game_paused()) {
        P.st.update_ms = (re_time_seconds() - t0) * 1000.0;
        return;
    }
    P.play_time += dt;
    checkpoints(w, player);
    // the player's death: back at the checkpoint after its delay (its save reloaded when it has one)
    if (player && health_is_dead(w, player) && P.cp_valid && (P.cp_flags & CHECKPOINT_RESPAWN_ON_DEATH)) {
        P.dead_time += dt;
        if (P.dead_time >= P.cp_delay) progress_respawn(w, player, true), P.dead_time = 0.0f;
    } else P.dead_time = 0.0f;
    // auto save: the level change's, then the timed one (the player's AutoSave option)
    bool allowed = gus()->auto_save;
    if (P.auto_pending && allowed && player) P.auto_pending = false, progress_auto_save(w);
    float every = P.interval >= 0.0f ? P.interval : project()->autosave_interval;
    if (every > 0.0f && allowed && !P.load_stage) {
        P.auto_timer += dt;
        if (P.auto_timer >= every) progress_auto_save(w);
    }
    P.st.update_ms = (re_time_seconds() - t0) * 1000.0;
}

void progress_end_play(World* w) {
    progress_flush();
    if (w && P.sg && ecs_alive(w, P.sg)) { // (its variables for the next level)
        uint32_t n = bp_vars_dump(w, P.sg, P.sg_blob, sizeof P.sg_blob);
        P.sg_len = n;
    }
    P.sg = ENTITY_NONE;
    if (P.vk) P.vk->thumb_req = false;
}

void progress_session_end(void) {
    progress_flush();
    writer_wait();
    free(P.load);
    P.load = NULL, P.load_len = 0, P.load_stage = 0;
    P.session = false, P.began_once = false, P.auto_pending = false;
    P.sg = ENTITY_NONE, P.sg_class = 0, P.sg_len = 0;
    P.cp_valid = false, P.nreached = 0;
    P.interval = -1.0f;
    quest_session_end(); // (the Quest Logs: play stopped)
}

const ProgressStats* progress_stats(void) { return &P.st; }

static void collect(DvList* l, const DvContext* c) {
    (void)c;
    l->gpu = false, l->estimated = false;
    l->ms = P.st.update_ms;
    snprintf(l->note, sizeof l->note, "%u Saveables, %u Checkpoints; since play: %u saves (%u auto), %u loads, %u refused; play time %.0f s",
             P.st.saveables, P.st.checkpoints, P.st.saves, P.st.auto_saves, P.st.loads, P.st.refused, P.play_time);
    char info[48];
    dv_add(l, "Update", "checkpoints, auto save, play time", (float)P.st.update_ms, ENTITY_NONE);
    snprintf(info, sizeof info, "%u actors, %u destroyed, %u KB", P.st.actors, P.st.destroyed, P.st.bytes / 1024u);
    dv_add(l, "Last save", info, (float)P.st.save_ms, ENTITY_NONE);
    snprintf(info, sizeof info, "apply %.3f ms, %u spawned", P.st.apply_ms, P.st.spawned);
    dv_add(l, "Last load", info, (float)P.st.load_ms, ENTITY_NONE);
    dv_add(l, "Last picture", "the frame copied and scaled", (float)P.st.thumb_ms, ENTITY_NONE);
}
void progress_register_debug(void) { dv_register("Save Game", collect); }

// --- The self-test (the format, slots) --------------------------------------------------------------------------------------

int progress_selftest(void) {
    int fails = 0;
#define EXPECT(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: save: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
    // a progress file made by hand: a PROG chunk, an unknown chunk (a newer engine's), a DEST chunk
    Buf b = { 0 };
    put(&b, NULL, HEADER_BYTES);
    uint32_t c = chunk_begin(&b, T_PROG);
    double pt = 3723.0;
    put(&b, &pt, 8);
    for (int i = 0; i < 9; i++) put32(&b, 0);
    put(&b, NULL, 32), put32(&b, 0);
    chunk_end(&b, c);
    c = chunk_begin(&b, TAG('N', 'E', 'W', '!'));
    put(&b, "a newer engine's data", 21);
    chunk_end(&b, c);
    c = chunk_begin(&b, T_DEST);
    put32(&b, 2), put32(&b, 11), put32(&b, 22);
    chunk_end(&b, c);
    double keep = P.play_time;
    P.play_time = pt;
    header_fill(&b, SAVE_KIND_PROGRESS, SAVE_FLAG_AUTO, 0, "Self-test", "L_SelfTest");
    P.play_time = keep;
    EXPECT(!b.bad && write_file("__selftest_a", &b), "a slot could not be written in %s", progress_dir());
    SaveSlotInfo si;
    EXPECT(progress_slot_info("__selftest_a", &si) && si.kind == SAVE_KIND_PROGRESS && si.version == SAVE_VERSION && si.play_time == 3723.0 &&
               !strcmp(si.level, "L_SelfTest") && !strcmp(si.display, "Self-test") && (si.flags & SAVE_FLAG_AUTO) && si.time > 0,
           "its metadata read back wrong");
    char t[32];
    progress_format_duration(si.play_time, t, sizeof t);
    EXPECT(!strcmp(t, "1:02:03"), "the play time as text: \"%s\" (1:02:03)", t);
    uint32_t n = 0;
    uint8_t* d = read_slot("__selftest_a", &n, &si);
    Rd r;
    EXPECT(d && find_chunk(d, n, HEADER_BYTES, T_DEST, &r) && get32(&r) == 2 && get32(&r) == 11, "the chunk after an unknown one not found");
    free(d);
    // one byte changed anywhere: refused (the header's, a chunk's, the last), and a file cut short
    uint32_t refused0 = P.st.refused;
    uint32_t at[3] = { 70, HEADER_BYTES + 20, b.n - 1 };
    for (int k = 0; k < 3; k++) {
        b.p[at[k]] ^= 0x5A;
        char path[700];
        progress_slot_path("__selftest_bad", path, sizeof path);
        FILE* f = re_fopen(path, "wb");
        if (f) fwrite(b.p, 1, b.n, f), fclose(f);
        d = read_slot("__selftest_bad", &n, NULL);
        EXPECT(!d, "a save with byte %u changed was not refused", at[k]);
        free(d);
        b.p[at[k]] ^= 0x5A;
    }
    {
        char path[700];
        progress_slot_path("__selftest_bad", path, sizeof path);
        FILE* f = re_fopen(path, "wb");
        if (f) fwrite(b.p, 1, b.n - 10, f), fclose(f);
        d = read_slot("__selftest_bad", &n, NULL);
        EXPECT(!d, "a save cut short was not refused");
        free(d);
        f = re_fopen(path, "wb");
        if (f) fwrite("garbage", 1, 7, f), fclose(f);
        EXPECT(!read_slot("__selftest_bad", &n, NULL) && !progress_slot_info("__selftest_bad", NULL), "garbage was not refused");
        uint8_t newer[HEADER_BYTES];
        memcpy(newer, b.p, HEADER_BYTES);
        uint32_t v9 = 9;
        memcpy(newer + 8, &v9, 4);
        f = re_fopen(path, "wb");
        if (f) fwrite(newer, 1, sizeof newer, f), fclose(f);
        EXPECT(!read_slot("__selftest_bad", &n, NULL), "a newer version's save was not refused");
    }
    EXPECT(P.st.refused >= refused0 + 6, "the refusals were not counted (%u)", P.st.refused - refused0);
    free(b.p);
    // version 1 (the old Save Game to Slot): migrated into a VARS chunk with the same bytes
    {
        static const uint8_t v1[] = { 'R', 'E', 'S', 'A', 'V', '0', '0', '1', 5, 'S', 'c', 'o', 'r', 'e', 0, 1, 0, 0, 0x40, 0x41, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        char path[700];
        progress_slot_path("__selftest_v1", path, sizeof path);
        FILE* f = re_fopen(path, "wb");
        if (f) fwrite(v1, 1, sizeof v1, f), fclose(f);
        d = read_slot("__selftest_v1", &n, &si);
        EXPECT(d && si.version == 1 && si.kind == SAVE_KIND_OBJECT, "a version 1 save was not read");
        EXPECT(d && find_chunk(d, n, HEADER_BYTES, T_VARS, &r) && r.n == sizeof v1 && !memcmp(r.p, v1, sizeof v1), "the version 1 variables were not migrated");
        EXPECT(d && check_file(d, n, NULL, NULL, NULL, NULL), "the migrated save does not check");
        free(d);
    }
    // the slot list: the progress ones (newest first), all of them with the variable saves
    SaveSlotInfo list[SAVE_MAX_SLOTS];
    uint32_t np = progress_list(list, SAVE_MAX_SLOTS, false), na = progress_list(list, SAVE_MAX_SLOTS, true);
    bool has_a = false, has_v1 = false;
    for (uint32_t i = 0; i < na; i++) has_a |= !strcmp(list[i].slot, "__selftest_a"), has_v1 |= !strcmp(list[i].slot, "__selftest_v1");
    EXPECT(has_a && has_v1 && na > np, "the slot list (%u progress, %u in all)", np, na);
    EXPECT(progress_slot_exists("__selftest_a") && progress_slot_delete("__selftest_a") && !progress_slot_exists("__selftest_a"), "a slot was not deleted");
    char cl[SAVE_SLOT_NAME];
    clean_slot("../evil/..\\x:y", cl, sizeof cl);
    EXPECT(!strchr(cl, '/') && !strchr(cl, '\\') && !strchr(cl, ':'), "a slot name made a path: \"%s\"", cl);
    progress_slot_delete("__selftest_bad"), progress_slot_delete("__selftest_v1");
#undef EXPECT
    return fails;
}

uint32_t progress_new_id(void) { // (a Saveable added in the editor: its key from now on, renamed or not)
    static uint32_t n;
    uint64_t x = (uint64_t)re_unix_time() * 0x9E3779B97F4A7C15ull ^ (uint64_t)(re_time_seconds() * 1e6) ^ (uint64_t)(++n) * 0xBF58476D1CE4E5B9ull;
    x ^= x >> 31, x *= 0x94D049BB133111EBull, x ^= x >> 29;
    uint32_t id = (uint32_t)x;
    return id == 0 || id == PLAYER_KEY ? 0x5A5A5A5Au : id;
}
