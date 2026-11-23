#include "report_upload.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crashlog.h"
#include "endpoints.h"
#include "io_json.h"
#include "net_http.h"
#include "platform.h"
#include "platform_files.h"

#ifndef RE_VERSION_STRING
#define RE_VERSION_STRING "dev"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static const char* const k_ids[UPLOAD_KIND_COUNT] = { "bug", "crash", "suggestion" };

const char* upload_kind_id(UploadKind k) { return k >= 0 && k < UPLOAD_KIND_COUNT ? k_ids[k] : "bug"; }

static int kind_from_id(const char* s) {
    for (int k = 0; k < UPLOAD_KIND_COUNT; k++)
        if (!strcmp(s, k_ids[k])) return k;
    return -1;
}

void upload_target_load(UploadTarget* t) {
    memset(t, 0, sizeof *t);
    const char* base = endpoints_get("reports", "report_url");
    if (base[0]) {
        size_t n = strlen(base);
        snprintf(t->url, sizeof t->url, "%.*s/report", (int)(n && base[n - 1] == '/' ? n - 1 : n), base);
    }
    snprintf(t->key, sizeof t->key, "%s", endpoints_get("reports", "app_key"));
}

bool upload_target_enabled(const UploadTarget* t) { return t->url[0] != 0; }

// --- Privacy ----------------------------------------------------------------------------------------------------------


typedef struct Buf {
    char* p;
    size_t n, cap;
} Buf;

static void buf_put(Buf* b, const char* s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->n + n + 1) nc *= 2;
        char* np = realloc(b->p, nc);
        if (!np) return;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

// Case-insensitive match of needle at s, '/' and '\' the same.
static size_t match_at(const char* s, const char* needle) {
    size_t i = 0;
    for (; needle[i]; i++) {
        char a = s[i], b = needle[i];
        if (!a) return 0;
        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
        if (tolower((unsigned char)a) != tolower((unsigned char)b)) return 0;
    }
    return i;
}

static bool word_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == '+' || (unsigned char)c >= 0x80; }

// An e-mail address starting at s (a word, '@', a word with a dot): its length, 0 if none.
static size_t email_at(const char* s, const char* start) {
    if (s > start && word_char(s[-1])) return 0;
    size_t i = 0;
    while (word_char(s[i]) && s[i] != '@') i++;
    if (i == 0 || s[i] != '@') return 0;
    size_t at = i++;
    bool dot = false;
    while (word_char(s[i])) {
        if (s[i] == '.' && i > at + 1) dot = true;
        i++;
    }
    while (i > at + 1 && s[i - 1] == '.') i--; // "nome@site.com." ends a sentence
    return dot ? i : 0;
}

// "X:\Users\<name>" (or with '/'): its length up to the end of the name, 0 if none. "Public" and "Default" are no one.
static size_t users_path_at(const char* s) {
    if (!isalpha((unsigned char)s[0]) || s[1] != ':' || (s[2] != '\\' && s[2] != '/')) return 0;
    if (_strnicmp(s + 3, "Users", 5) != 0 || (s[8] != '\\' && s[8] != '/')) return 0;
    size_t i = 9;
    while (s[i] && s[i] != '\\' && s[i] != '/' && s[i] != '"' && s[i] != '\'' && s[i] != '\n' && s[i] != '\r' && s[i] != '`')
        i++;
    // (a name can have spaces: the segment runs to the next separator, more rather than less)
    if (i == 9 || (!_strnicmp(s + 9, "Public", 6) && !isalnum((unsigned char)s[15])) ||
        (!_strnicmp(s + 9, "Default", 7) && !isalnum((unsigned char)s[16])))
        return 0;
    return i;
}

char* upload_scrub(const char* text, const UploadPrivacy* who) {
    char user[256] = "", home[512] = "", computer[256] = "";
    if (who) {
        snprintf(user, sizeof user, "%s", who->user ? who->user : "");
        snprintf(home, sizeof home, "%s", who->home ? who->home : "");
        snprintf(computer, sizeof computer, "%s", who->computer ? who->computer : "");
    } else {
#ifdef _WIN32
        wchar_t w[512];
        DWORD n;
        if (GetEnvironmentVariableW(L"USERNAME", w, 512)) WideCharToMultiByte(CP_UTF8, 0, w, -1, user, sizeof user, NULL, NULL);
        if (GetEnvironmentVariableW(L"USERPROFILE", w, 512)) WideCharToMultiByte(CP_UTF8, 0, w, -1, home, sizeof home, NULL, NULL);
        n = 512;
        if (GetComputerNameW(w, &n)) WideCharToMultiByte(CP_UTF8, 0, w, -1, computer, sizeof computer, NULL, NULL);
#endif
    }
    Buf b = { 0 };
    const char* s = text ? text : "";
    buf_put(&b, "", 0);
    while (*s) {
        size_t m;
        if (home[0] && (m = match_at(s, home)) > 0) {
            buf_put(&b, "%USERPROFILE%", 13);
            s += m;
        } else if ((m = users_path_at(s)) > 0) { // another account's folder (C:\Users\<name>): the same
            buf_put(&b, "%USERPROFILE%", 13);
            s += m;
        } else if ((m = email_at(s, text)) > 0) {
            buf_put(&b, "<email>", 7);
            s += m;
        } else if (strlen(user) >= 2 && (m = match_at(s, user)) > 0 && (s == text || !isalnum((unsigned char)s[-1])) &&
                   !isalnum((unsigned char)s[m])) {
            buf_put(&b, "<usuário>", strlen("<usuário>"));
            s += m;
        } else if (strlen(computer) >= 2 && (m = match_at(s, computer)) > 0 && (s == text || !isalnum((unsigned char)s[-1])) &&
                   !isalnum((unsigned char)s[m])) {
            buf_put(&b, "<computador>", 12);
            s += m;
        } else {
            buf_put(&b, s, 1);
            s++;
        }
    }
    return b.p;
}

// --- Files ------------------------------------------------------------------------------------------------------------

static char* read_all(const char* path, size_t* len) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = n >= 0 ? malloc((size_t)n + 1) : NULL;
    size_t got = 0;
    if (t) {
        got = fread(t, 1, (size_t)n, f);
        t[got] = 0;
    }
    fclose(f);
    if (len) *len = got;
    return t;
}

static bool write_all(const char* path, const void* data, size_t n) {
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = re_fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    return ok && re_replace_file(tmp, path);
}

static const char* base_name(const char* p) {
    const char* s = p;
    for (const char* c = p; *c; c++)
        if (*c == '/' || *c == '\\') s = c + 1;
    return s;
}

static bool has_ext(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !_stricmp(name + n - e, ext);
}

static bool text_file(const char* name) { return has_ext(name, ".txt") || has_ext(name, ".md") || has_ext(name, ".log"); }

bool upload_file_name(const char* path, char* out, size_t cap) {
    static const char* const exts[] = { ".png", ".jpg", ".jpeg", ".txt", ".md", ".log", ".dmp", ".rlevel", ".json" };
    const char* name = base_name(path);
    bool ok = false;
    for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++) ok |= has_ext(name, exts[i]);
    size_t o = 0, max = cap < 65 ? cap - 1 : 64;
    const char* ext = strrchr(name, '.');
    size_t ext_len = ext ? strlen(ext) : 0;
    for (const unsigned char* c = (const unsigned char*)name; *c && o < max; c++) {
        if (ext && (const char*)c < ext && o + ext_len >= max) { // a long name keeps its extension
            c = (const unsigned char*)ext - 1;
            continue;
        }
        unsigned char ch = *c;
        if (ch >= 0x80) { // UTF-8: one '_' per character (accents dropped)
            while ((c[1] & 0xC0) == 0x80) c++;
            ch = '_';
        }
        out[o++] = (char)((isalnum(ch) || ch == '.' || ch == '_' || ch == '-') ? ch : '_');
    }
    out[o] = 0;
    if (!_stricmp(out, "report.md")) snprintf(out, cap, "relatorio.md"); // the Worker writes report.md itself
    return ok && o > 0;
}

bool upload_dir(const char* sub, char* out, size_t cap) {
    char dir[1024];
    if (!re_config_dir(dir, sizeof dir)) return false;
    snprintf(out, cap, "%s/%s", dir, sub);
    return re_make_dirs(out);
}

// The text cut to the Worker's limit (not inside a UTF-8 character).
static void cut_text(char* t) {
    size_t n = strlen(t);
    static const char note[] = "\n\n[cortado: o texto inteiro ficou no computador]";
    if (n <= UPLOAD_MAX_BODY) return;
    size_t at = UPLOAD_MAX_BODY - sizeof note;
    while (at > 0 && ((unsigned char)t[at] & 0xC0) == 0x80) at--;
    memcpy(t + at, note, sizeof note);
}

bool upload_enqueue(const char* root, const UploadItem* it, char* out_dir, size_t dir_cap, char* err, size_t err_cap) {
    if (!re_make_dirs(root)) {
        snprintf(err, err_cap, "não foi possível criar %s", root);
        return false;
    }
    char stamp[64], dir[1100];
    crash_timestamp(stamp, sizeof stamp);
    for (int n = 1;; n++) {
        if (n == 1) snprintf(dir, sizeof dir, "%s/%s_%s", root, stamp, upload_kind_id(it->kind));
        else snprintf(dir, sizeof dir, "%s/%s_%s_%d", root, stamp, upload_kind_id(it->kind), n);
        if (!re_file_stat(dir, NULL, NULL, NULL)) break;
        if (n > 999) {
            snprintf(err, err_cap, "fila cheia");
            return false;
        }
    }
    if (!re_make_dirs(dir)) {
        snprintf(err, err_cap, "não foi possível criar %s", dir);
        return false;
    }
    char* title = upload_scrub(it->title, NULL);
    char* body = upload_scrub(it->body, NULL);
    if (body) cut_text(body);
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kstr(&w, "kind", upload_kind_id(it->kind));
    jw_kstr(&w, "title", title ? title : "");
    jw_kstr(&w, "body", body ? body : "");
    jw_kstr(&w, "name", it->name ? it->name : "");
    jw_kstr(&w, "version", RE_VERSION_STRING);
    jw_kstr(&w, "created", stamp);
    // the files within the Worker's limits; the rest listed with the reason
    char skipped[1024] = "";
    size_t sk = 0;
    int count = 0;
    long long total = 0;
    jw_key(&w, "files");
    jw_arr_begin(&w);
    bool ok = true;
    char names[UPLOAD_MAX_FILES][72];
    for (int i = 0; i < it->file_count && ok; i++) {
        char name[72];
        const char* why = NULL;
        uint64_t size = 0;
        if (!upload_file_name(it->files[i], name, sizeof name)) why = "tipo de arquivo não aceito";
        else if (!re_file_stat(it->files[i], &size, NULL, NULL)) why = "não encontrado";
        else if (size > UPLOAD_MAX_FILE) why = "maior que 3 MB";
        else if (count >= UPLOAD_MAX_FILES) why = "mais de 8 arquivos";
        else if (total + (long long)size > UPLOAD_MAX_TOTAL) why = "passaria de 8 MB no total";
        if (!why) { // the same name twice: a number before the extension
            for (int k = 0, again = 1; again && k < 100; k++) {
                again = 0;
                for (int j = 0; j < count; j++)
                    if (!_stricmp(names[j], name)) again = 1;
                if (again) {
                    char* dot = strrchr(name, '.');
                    char tmp[72];
                    snprintf(tmp, sizeof tmp, "%.*s_%d%s", (int)(dot ? dot - name : (long long)strlen(name)), name, k + 2,
                             dot ? dot : "");
                    snprintf(name, sizeof name, "%.64s", tmp);
                }
            }
        }
        if (why) {
            sk += (size_t)snprintf(skipped + sk, sk < sizeof skipped ? sizeof skipped - sk : 0, "%s: %s\n",
                                   base_name(it->files[i]), why);
            if (sk >= sizeof skipped) sk = sizeof skipped - 1;
            continue;
        }
        char dst[1300];
        snprintf(dst, sizeof dst, "%s/%s", dir, name);
        if (text_file(name)) { // a text attachment is scrubbed like the body
            char* t = read_all(it->files[i], NULL);
            char* c = t ? upload_scrub(t, NULL) : NULL;
            if (c) cut_text(c);
            ok = c && write_all(dst, c, strlen(c));
            if (c) size = strlen(c);
            free(t);
            free(c);
        } else {
            size_t n = 0;
            char* d = read_all(it->files[i], &n);
            ok = d && write_all(dst, d, n);
            free(d);
        }
        if (!ok) {
            snprintf(err, err_cap, "não foi possível copiar %s", it->files[i]);
            break;
        }
        snprintf(names[count++], sizeof names[0], "%s", name);
        total += (long long)size;
        jw_str(&w, name);
    }
    jw_arr_end(&w);
    jw_kstr(&w, "skipped", skipped);
    jw_obj_end(&w);
    free(title);
    free(body);
    if (ok && (w.fail || !w.buf)) {
        snprintf(err, err_cap, "sem memória");
        ok = false;
    }
    char path[1200];
    snprintf(path, sizeof path, "%s/item.json", dir);
    if (ok && !write_all(path, w.buf, w.len)) {
        snprintf(err, err_cap, "não foi possível gravar %s", path);
        ok = false;
    }
    jw_free(&w);
    if (!ok) re_remove_tree(dir);
    else if (out_dir) snprintf(out_dir, dir_cap, "%s", dir);
    return ok;
}

typedef struct Names {
    char (*v)[256];
    int n, cap;
} Names;

static void add_name(void* user, const ReDirEntry* e) {
    Names* l = user;
    if (!e->is_dir || l->n >= l->cap) return;
    snprintf(l->v[l->n++], 256, "%s", e->name);
}

static int cmp_names(const void* a, const void* b) { return strcmp(a, b); }

static int list_items(const char* root, Names* l) {
    l->cap = 512;
    l->n = 0;
    l->v = malloc(sizeof *l->v * (size_t)l->cap);
    if (!l->v) return 0;
    re_dir_walk(root, add_name, l);
    qsort(l->v, (size_t)l->n, sizeof *l->v, cmp_names); // the stamp first: oldest first
    return l->n;
}

int upload_outbox_count(const char* root) {
    Names l;
    int n = list_items(root, &l);
    free(l.v);
    return n;
}

typedef struct Item {
    JsonDoc doc;
    char* text;
    int kind;
} Item;

static bool item_load(const char* dir, Item* it) {
    memset(it, 0, sizeof *it);
    char path[1200];
    snprintf(path, sizeof path, "%s/item.json", dir);
    size_t n = 0;
    it->text = read_all(path, &n);
    char err[128];
    if (!it->text || !json_parse(it->text, n, &it->doc, err, sizeof err)) {
        free(it->text);
        it->text = NULL;
        return false;
    }
    it->kind = kind_from_id(json_get_str(&it->doc.root, "kind", ""));
    return it->kind >= 0;
}

static void item_free(Item* it) {
    if (it->text) json_free(&it->doc);
    free(it->text);
    it->text = NULL;
}

bool upload_preview_load(const char* item_dir, UploadPreview* p) {
    memset(p, 0, sizeof *p);
    Item it;
    if (!item_load(item_dir, &it)) return false;
    const JsonValue* r = &it.doc.root;
    p->kind = it.kind;
    snprintf(p->title, sizeof p->title, "%s", json_get_str(r, "title", ""));
    snprintf(p->name, sizeof p->name, "%s", json_get_str(r, "name", ""));
    snprintf(p->skipped, sizeof p->skipped, "%s", json_get_str(r, "skipped", ""));
    const char* body = json_get_str(r, "body", "");
    p->body = malloc(strlen(body) + 1);
    if (p->body) strcpy(p->body, body);
    const JsonValue* files = json_get(r, "files");
    for (size_t i = 0; i < json_len(files) && p->file_count < UPLOAD_MAX_FILES; i++) {
        const char* name = json_str(json_at(files, i), "");
        char path[1300];
        snprintf(path, sizeof path, "%s/%s", item_dir, name);
        uint64_t size = 0;
        re_file_stat(path, &size, NULL, NULL);
        snprintf(p->files[p->file_count], sizeof p->files[0], "%s", name);
        p->sizes[p->file_count++] = (long long)size;
    }
    item_free(&it);
    return true;
}

void upload_preview_free(UploadPreview* p) {
    free(p->body);
    p->body = NULL;
}

bool upload_commit_draft(const char* draft_dir, const char* outbox, char* out_dir, size_t cap) {
    if (!re_make_dirs(outbox)) return false;
    char dst[1200];
    snprintf(dst, sizeof dst, "%s/%s", outbox, base_name(draft_dir));
    if (!re_move_path(draft_dir, dst)) return false;
    if (out_dir) snprintf(out_dir, cap, "%s", dst);
    return true;
}

char* upload_build_request(const char* item_dir, size_t* len) {
    Item it;
    if (!item_load(item_dir, &it)) return NULL;
    const JsonValue* r = &it.doc.root;
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kstr(&w, "kind", upload_kind_id((UploadKind)it.kind));
    // the product, for the relay shared with the Tlender (endpoints.ini [reports] produto)
    const char* produto = endpoints_get("reports", "produto");
    if (produto[0]) jw_kstr(&w, "produto", produto);
    jw_kstr(&w, "title", json_get_str(r, "title", ""));
    jw_kstr(&w, "body", json_get_str(r, "body", ""));
    jw_kstr(&w, "version", json_get_str(r, "version", ""));
    jw_kstr(&w, "name", json_get_str(r, "name", ""));
    jw_key(&w, "files");
    jw_arr_begin(&w);
    const JsonValue* files = json_get(r, "files");
    for (size_t i = 0; i < json_len(files); i++) {
        const char* name = json_str(json_at(files, i), "");
        char path[1500];
        snprintf(path, sizeof path, "%s/%s", item_dir, name);
        size_t n = 0;
        char* data = read_all(path, &n);
        if (!data) continue;
        char* b64 = malloc(4 * ((n + 2) / 3) + 1);
        if (b64) {
            http_base64((const uint8_t*)data, n, b64);
            jw_obj_begin(&w);
            jw_kstr(&w, "name", name);
            jw_kstr(&w, "data", b64);
            jw_obj_end(&w);
        }
        free(b64);
        free(data);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    item_free(&it);
    if (w.fail) {
        jw_free(&w);
        return NULL;
    }
    if (len) *len = w.len;
    return w.buf; // the caller frees it
}

static void bump_attempts(const char* item_dir, const char* why) {
    char path[1200];
    snprintf(path, sizeof path, "%s/state.txt", item_dir);
    char* old = read_all(path, NULL);
    int attempts = old ? atoi(old) : 0;
    free(old);
    char text[400];
    int n = snprintf(text, sizeof text, "%d\n%s\n", attempts + 1, why);
    write_all(path, text, (size_t)n);
}

UploadSendResult upload_send_one(const char* item_dir, const UploadTarget* t, const char* rejected_root, char* id, size_t id_cap,
                                 char* err, size_t err_cap) {
    if (id_cap) id[0] = 0;
    if (!upload_target_enabled(t)) {
        snprintf(err, err_cap, "envio não configurado");
        return UPLOAD_RETRY;
    }
    size_t len = 0;
    char* body = upload_build_request(item_dir, &len);
    if (!body) {
        snprintf(err, err_cap, "item da fila danificado");
        return UPLOAD_RETRY;
    }
    char headers[400];
    snprintf(headers, sizeof headers, "Content-Type: application/json\r\nX-Tlender-Key: %s\r\n", t->key);
    HttpRequest rq = { "POST", t->url, headers, body, len, NULL, NULL, NULL, 60000 };
    HttpResponse r;
    bool answered = http_request(&rq, &r);
    free(body);
    UploadSendResult res = UPLOAD_RETRY;
    char server_error[200] = "";
    if (answered && r.body && r.len) {
        JsonDoc doc;
        char jerr[64];
        if (json_parse((const char*)r.body, r.len, &doc, jerr, sizeof jerr)) {
            if (r.status >= 200 && r.status < 300 && json_get_bool(&doc.root, "ok", false) && id_cap)
                snprintf(id, id_cap, "%s", json_get_str(&doc.root, "id", ""));
            snprintf(server_error, sizeof server_error, "%s", json_get_str(&doc.root, "error", ""));
            json_free(&doc);
        }
    }
    if (answered && r.status >= 200 && r.status < 300) {
        res = UPLOAD_SENT;
        re_remove_tree(item_dir);
    } else if (answered && (r.status == 400 || r.status == 403 || r.status == 413)) {
        // the relay will never take it as it is: not sent again (kept aside for the user to see)
        snprintf(err, err_cap, "o servidor recusou (%d%s%s)", r.status, server_error[0] ? ": " : "", server_error);
        bump_attempts(item_dir, err);
        res = UPLOAD_REJECTED;
        char dst[1300];
        if (rejected_root && re_make_dirs(rejected_root)) {
            snprintf(dst, sizeof dst, "%s/%s", rejected_root, base_name(item_dir));
            if (!re_move_path(item_dir, dst)) re_remove_tree(item_dir);
        } else {
            re_remove_tree(item_dir);
        }
    } else {
        if (!answered) snprintf(err, err_cap, "%s", r.error);
        else snprintf(err, err_cap, "o servidor respondeu %d%s%s", r.status, server_error[0] ? ": " : "", server_error);
        bump_attempts(item_dir, err);
    }
    http_response_free(&r);
    return res;
}

static volatile int32_t g_flushing = 0;

void upload_flush(const char* outbox, const char* rejected_root, const UploadTarget* t, UploadResult* r) {
    memset(r, 0, sizeof *r);
    while (!re_atomic_cas(&g_flushing, 0, 1)) re_sleep_seconds(0.05);
    Names l;
    int n = list_items(outbox, &l);
    for (int i = 0; i < n; i++) {
        char dir[1400];
        snprintf(dir, sizeof dir, "%s/%s", outbox, l.v[i]);
        Item it;
        if (!item_load(dir, &it)) continue; // a half-written item (the program closed meanwhile): left alone
        item_free(&it);
        char err[256] = "", id[160] = "";
        UploadSendResult s = upload_send_one(dir, t, rejected_root, id, sizeof id, err, sizeof err);
        if (s == UPLOAD_SENT) {
            r->sent++;
            if (id[0]) snprintf(r->last_id, sizeof r->last_id, "%s", id);
        } else {
            if (s == UPLOAD_REJECTED) r->rejected++;
            else r->retry++;
            snprintf(r->last_error, sizeof r->last_error, "%s", err);
            if (s == UPLOAD_RETRY && strstr(err, "429")) break; // the relay's hourly limit: the rest waits too
        }
    }
    free(l.v);
    re_atomic_store(&g_flushing, 0);
}
