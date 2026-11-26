// (2026-11-24, item 15) The localization's automatic translation (loc.h): the offline Pseudo-localization (Unreal's), and
// the providers the user sets in the dashboard (LibreTranslate at its URL, DeepL or Google with the user's key), by HTTP
// on a worker (async.h: a card with the progress, Cancel). Off by default; nothing is sent until the user picks a
// provider, fills it and clicks Translate Automatically. The key stays in the user's editor.ini, never in the project.
#include "loc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "async.h"
#include "io_json.h"
#include "net_http.h"

const char* const g_loc_provider_names[LOC_PROV_COUNT] = { "Off", "Pseudo-localization (offline)", "LibreTranslate", "DeepL", "Google Translate" };

static LocProvider g_prov; // (kind 0: off)
LocProvider* loc_provider(void) { return &g_prov; }

void loc_write_prefs(void* file) {
    FILE* f = file;
    fprintf(f, "loc_provider=%d\nloc_provider_url=%s\nloc_provider_key=%s\n", g_prov.kind, g_prov.url, g_prov.key);
}
bool loc_read_pref(const char* key, const char* v) {
    if (!strcmp(key, "loc_provider")) g_prov.kind = atoi(v) >= 0 && atoi(v) < LOC_PROV_COUNT ? atoi(v) : 0;
    else if (!strcmp(key, "loc_provider_url")) snprintf(g_prov.url, sizeof g_prov.url, "%s", v);
    else if (!strcmp(key, "loc_provider_key")) snprintf(g_prov.key, sizeof g_prov.key, "%s", v);
    else return false;
    return true;
}

// the language part a provider takes ("pt-BR" -> "pt"; DeepL: its own upper-case codes)
static void lang_code(const char* c, bool deepl, bool target, char* out, size_t cap) {
    snprintf(out, cap, "%s", c);
    char* dash = strchr(out, '-');
    if (deepl) {
        bool keep = target && dash && (!_strnicmp(c, "pt", 2) || !_strnicmp(c, "en", 2)); // (DeepL's targets: EN-US, PT-BR...)
        if (!keep && dash) *dash = 0;
        if (target && !dash && !_stricmp(out, "en")) snprintf(out, cap, "EN-US");
        if (target && !dash && !_stricmp(out, "pt")) snprintf(out, cap, "PT-BR");
        for (char* p = out; *p; p++) *p = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    } else if (dash && _strnicmp(c, "zh", 2))
        *dash = 0;
}

// One text through the provider (a worker's): false with the reason
static bool translate_one(const LocProvider* pv, const char* from, const char* to, const char* text, char* out, size_t cap, char* err, size_t ecap) {
    char src[16], dst[16], url[512], headers[400];
    bool deepl = pv->kind == LOC_PROV_DEEPL;
    lang_code(from, deepl, false, src, sizeof src), lang_code(to, deepl, true, dst, sizeof dst);
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    if (pv->kind == LOC_PROV_LIBRE) {
        const char* base = pv->url[0] ? pv->url : "http://localhost:5000";
        size_t bl = strlen(base);
        snprintf(url, sizeof url, "%.*s/translate", (int)(bl && base[bl - 1] == '/' ? bl - 1 : bl), base);
        snprintf(headers, sizeof headers, "Content-Type: application/json\r\n");
        jw_kstr(&w, "q", text), jw_kstr(&w, "source", src), jw_kstr(&w, "target", dst), jw_kstr(&w, "format", "text");
        if (pv->key[0]) jw_kstr(&w, "api_key", pv->key);
    } else if (deepl) {
        size_t kl = strlen(pv->key);
        bool free_key = kl > 3 && !strcmp(pv->key + kl - 3, ":fx");
        snprintf(url, sizeof url, "%s", pv->url[0] ? pv->url : free_key ? "https://api-free.deepl.com/v2/translate" : "https://api.deepl.com/v2/translate");
        snprintf(headers, sizeof headers, "Content-Type: application/json\r\nAuthorization: DeepL-Auth-Key %s\r\n", pv->key);
        jw_key(&w, "text"), jw_arr_begin(&w), jw_str(&w, text), jw_arr_end(&w);
        jw_kstr(&w, "source_lang", src), jw_kstr(&w, "target_lang", dst);
    } else {
        char k[400];
        http_url_encode(pv->key, k, sizeof k);
        snprintf(url, sizeof url, "%s?key=%s", pv->url[0] ? pv->url : "https://translation.googleapis.com/language/translate/v2", k);
        snprintf(headers, sizeof headers, "Content-Type: application/json\r\n");
        jw_kstr(&w, "q", text), jw_kstr(&w, "source", src), jw_kstr(&w, "target", dst), jw_kstr(&w, "format", "text");
    }
    jw_obj_end(&w);
    HttpRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.method = "POST", rq.url = url, rq.headers = headers, rq.body = w.buf, rq.body_len = w.len, rq.timeout_ms = 20000;
    HttpResponse rs;
    memset(&rs, 0, sizeof rs);
    bool ok = http_request(&rq, &rs);
    jw_free(&w);
    if (!ok || rs.status != 200 || !rs.body) {
        snprintf(err, ecap, "HTTP %d %s", rs.status, rs.error[0] ? rs.error : rs.body ? (const char*)rs.body : "");
        http_response_free(&rs);
        return false;
    }
    JsonDoc d;
    char perr[120];
    ok = json_parse((const char*)rs.body, rs.len, &d, perr, sizeof perr);
    http_response_free(&rs);
    if (!ok) return snprintf(err, ecap, "the answer isn't JSON: %s", perr), false;
    const char* t = NULL;
    if (pv->kind == LOC_PROV_LIBRE) t = json_get_str(&d.root, "translatedText", NULL);
    else if (deepl) t = json_get_str(json_at(json_get(&d.root, "translations"), 0), "text", NULL);
    else t = json_get_str(json_at(json_get(json_get(&d.root, "data"), "translations"), 0), "translatedText", NULL);
    if (t) snprintf(out, cap, "%s", t);
    else snprintf(err, ecap, "no translation in the answer");
    json_free(&d);
    return t != NULL;
}

// --- the job: the texts copied out (the table may change meanwhile: merged back by key) ---
typedef struct AutoJob {
    LocProvider pv;
    char from[LOC_CULTURE], to[LOC_CULTURE];
    int culture;
    uint32_t n, done;
    uint64_t* hash;
    char (*src)[LOC_TEXT];
    char (*out)[LOC_TEXT];
    bool* got;
    char err[300];
} AutoJob;
static AutoJob* g_job;
static volatile float g_job_progress;

static void job_free(AutoJob* j) {
    if (!j) return;
    free(j->hash), free(j->src), free(j->out), free(j->got), free(j);
}

static void auto_work(AsyncTask* t, void* arg) {
    AutoJob* j = arg;
    for (uint32_t i = 0; i < j->n && !(t && async_cancelled(t)); i++) {
        char err[300] = "";
        j->got[i] = translate_one(&j->pv, j->from, j->to, j->src[i], j->out[i], LOC_TEXT, err, sizeof err);
        if (!j->got[i] && !j->err[0]) snprintf(j->err, sizeof j->err, "%s", err);
        j->done = i + 1;
        g_job_progress = (float)(i + 1) / (float)j->n;
        if (t) async_progress_n(t, (int)i + 1, (int)j->n);
        if (!j->got[i] && i == 0) { // (the first one failed: the settings are wrong; stop asking)
            if (t) async_fail(t, "%s", err);
            return;
        }
    }
}

static uint32_t merge(AutoJob* j) {
    LocProject* p = loc_project();
    uint32_t n = 0;
    for (uint32_t i = 0; i < j->n; i++) {
        if (!j->got[i]) continue;
        for (uint32_t k = 0; k < p->n; k++)
            if (p->e[k].hash == j->hash[i] && !strcmp(p->e[k].source, j->src[i]) && p->e[k].tr[j->culture].status != LOC_REVIEWED) {
                loc_tr_set(&p->e[k], j->culture, j->out[i]);
                p->e[k].tr[j->culture].status = LOC_AUTO;
                p->dirty = true, n++;
                break;
            }
    }
    return n;
}

static void auto_done(AsyncTask* t, void* arg, bool ok) {
    (void)t, (void)ok;
    AutoJob* j = arg;
    uint32_t n = merge(j);
    printf("[RealEngine] Localization: %u text(s) translated to %s%s%s\n", n, j->to, j->err[0] ? " (" : "", j->err);
    if (g_job == j) g_job = NULL;
    job_free(j);
}

bool loc_auto_busy(float* progress) {
    if (progress) *progress = g_job ? g_job_progress : 0.0f;
    return g_job != NULL;
}

bool loc_auto_translate(int c, bool all) {
    LocProject* p = loc_project();
    if (c < 0 || c >= p->ntargets || g_job || g_prov.kind == LOC_PROV_OFF) return false;
    if (g_prov.kind != LOC_PROV_PSEUDO && g_prov.kind != LOC_PROV_LIBRE && !g_prov.key[0]) return false; // (DeepL / Google: the user's key)
    AutoJob* j = calloc(1, sizeof *j);
    if (!j) return false;
    for (uint32_t i = 0; i < p->n; i++) {
        const LocEntry* e = &p->e[i];
        if (e->stale || !e->source[0] || e->tr[c].status == LOC_REVIEWED || (!all && loc_tr(e, c)[0] && e->tr[c].status != LOC_MISSING)) continue;
        j->n++;
    }
    j->hash = malloc(sizeof *j->hash * (j->n ? j->n : 1));
    j->src = malloc(sizeof *j->src * (j->n ? j->n : 1));
    j->out = calloc(j->n ? j->n : 1, sizeof *j->out);
    j->got = calloc(j->n ? j->n : 1, sizeof *j->got);
    if (!j->hash || !j->src || !j->out || !j->got) return job_free(j), false;
    uint32_t k = 0;
    for (uint32_t i = 0; i < p->n; i++) {
        const LocEntry* e = &p->e[i];
        if (e->stale || !e->source[0] || e->tr[c].status == LOC_REVIEWED || (!all && loc_tr(e, c)[0] && e->tr[c].status != LOC_MISSING)) continue;
        j->hash[k] = e->hash;
        snprintf(j->src[k++], LOC_TEXT, "%s", e->source);
    }
    j->pv = g_prov, j->culture = c;
    snprintf(j->from, sizeof j->from, "%s", p->native), snprintf(j->to, sizeof j->to, "%s", p->targets[c]);
    if (g_prov.kind == LOC_PROV_PSEUDO || !j->n) { // (offline: at once)
        for (uint32_t i = 0; i < j->n; i++) loc_pseudo(j->src[i], j->out[i], LOC_TEXT), j->got[i] = true;
        auto_done(NULL, j, true);
        return true;
    }
    char label[96];
    snprintf(label, sizeof label, "Translating to %s (%s)", j->to, g_loc_provider_names[g_prov.kind]);
    g_job = j, g_job_progress = 0.0f;
    if (!async_run(label, auto_work, auto_done, j, ASYNC_CANCELLABLE)) { // (no workers: here)
        auto_work(NULL, j);
        auto_done(NULL, j, true);
    }
    return true;
}

void loc_frame(void) {} // (the merge runs in the async card's done, on the main thread)
