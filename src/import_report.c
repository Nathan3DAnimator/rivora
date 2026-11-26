// (Q19, notas-dev/importar-tlender.md) The conversion report of an import (import_report.h): the importer's items merged as
// they come (an import, not a frame: a growing array), written next to the assets, published to the editor's window.
#include "import_report.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "platform.h"
#include "platform_files.h"

typedef struct Report {
    ImpRepItem* items;
    int n, cap;
    char source[260], file[600];
    char times[512];
} Report;

static Report g_build, g_shown; // the importer's (its thread) and the editor's

static void report_free(Report* r) {
    free(r->items);
    memset(r, 0, sizeof *r);
}

void imprep_begin(const char* src) {
    report_free(&g_build);
    const char* base = src ? src + strlen(src) : "";
    while (src && base > src && base[-1] != '/' && base[-1] != '\\') base--;
    snprintf(g_build.source, sizeof g_build.source, "%s", base);
}

void imprep_add(int kind, int cat, const char* owner, const char* item, const char* reason_fmt, ...) {
    char reason[200] = "";
    if (reason_fmt) {
        va_list a;
        va_start(a, reason_fmt);
        vsnprintf(reason, sizeof reason, reason_fmt, a);
        va_end(a);
    }
    for (int i = 0; i < g_build.n; i++) { // the same thing again (a node in many materials): counted once
        ImpRepItem* it = &g_build.items[i];
        if (it->kind == kind && it->cat == cat && !strcmp(it->item, item ? item : "") && !strcmp(it->reason, reason)) {
            if (it->count < 65535) it->count++;
            return;
        }
    }
    if (g_build.n == g_build.cap) {
        int cap = g_build.cap ? g_build.cap * 2 : 64;
        ImpRepItem* ni = realloc(g_build.items, sizeof *ni * (size_t)cap);
        if (!ni) return;
        g_build.items = ni, g_build.cap = cap;
    }
    ImpRepItem* it = &g_build.items[g_build.n++];
    memset(it, 0, sizeof *it);
    it->kind = (unsigned char)kind, it->cat = (unsigned char)cat, it->count = 1;
    snprintf(it->owner, sizeof it->owner, "%s", owner ? owner : "");
    snprintf(it->item, sizeof it->item, "%s", item ? item : "");
    snprintf(it->reason, sizeof it->reason, "%s", reason);
}

void imprep_counts(unsigned out[IMPREP_KIND_COUNT]) {
    for (int k = 0; k < IMPREP_KIND_COUNT; k++) out[k] = 0;
    for (int i = 0; i < g_build.n; i++) out[g_build.items[i].kind] += g_build.items[i].count;
}

void imprep_time(const char* step, double ms) {
    size_t n = strlen(g_build.times);
    snprintf(g_build.times + n, sizeof g_build.times - n, "%s%s %.1f ms", n ? ", " : "", step, ms);
}

const char* imprep_kind_name(int kind) {
    static const char* const names[IMPREP_KIND_COUNT] = { N_("Same"), N_("Converted"), N_("Baked"), N_("Lost") };
    return kind >= 0 && kind < IMPREP_KIND_COUNT ? names[kind] : "";
}

const char* imprep_cat_name(int cat) {
    static const char* const names[IMPREP_CAT_COUNT] = { N_("Mesh"), N_("Modifier"), N_("Shader Node"), N_("Texture"), N_("Driver"),
                                                         N_("Rig"), N_("Scene") };
    return cat >= 0 && cat < IMPREP_CAT_COUNT ? names[cat] : "";
}

void imprep_summary(char* text, size_t text_cap) {
    unsigned c[IMPREP_KIND_COUNT];
    imprep_counts(c);
    if (!text || !text_cap || (!c[0] && !c[1] && !c[2] && !c[3])) return;
    size_t n = strlen(text);
    snprintf(text + n, text_cap > n ? text_cap - n : 0, "; report: %u same, %u converted, %u baked, %u lost", c[0], c[1], c[2], c[3]);
}

bool imprep_finish(const char* folder_disk, char* text, size_t text_cap) {
    (void)text, (void)text_cap; // (the summary: imprep_summary, once the importer is done with its line)
    unsigned c[IMPREP_KIND_COUNT];
    imprep_counts(c);
    if (g_build.times[0]) printf("[RealEngine] Import times: %s\n", g_build.times);
    if (!folder_disk) return false;
    // (2026-11-24) not in the Content folder any more (it showed up in the Content Browser): the project's
    // Saved/ImportReports/<source name>.txt, the project being what holds the Content folder the assets went to
    char proj[600];
    snprintf(proj, sizeof proj, "%s", folder_disk);
    char* cut = NULL;
    for (char* p = proj; *p; p++)
        if ((*p == '/' || *p == '\\') && !_strnicmp(p + 1, "Content", 7) && (p[8] == 0 || p[8] == '/' || p[8] == '\\')) cut = p;
    if (!cut) return false; // (not under a Content folder: no file; the window still shows the report)
    *cut = 0;
    char name[260];
    snprintf(name, sizeof name, "%s", g_build.source[0] ? g_build.source : "Import");
    char* dot = strrchr(name, '.');
    if (dot && dot != name) *dot = 0;
    char dir[700];
    snprintf(dir, sizeof dir, "%s/Saved/ImportReports", proj);
    re_make_dirs(dir);
    snprintf(g_build.file, sizeof g_build.file, "%s/%s.txt", dir, name);
    FILE* f = re_fopen(g_build.file, "wb");
    if (!f) return false;
    fprintf(f, "%s: %s\n", TR("Import Report"), g_build.source);
    fprintf(f, "%u %s, %u %s, %u %s, %u %s\n", c[0], TR("Same"), c[1], TR("Converted"), c[2], TR("Baked"), c[3], TR("Lost"));
    if (g_build.times[0]) fprintf(f, "%s\n", g_build.times);
    for (int k = 0; k < IMPREP_KIND_COUNT; k++) {
        bool head = false;
        for (int i = 0; i < g_build.n; i++) {
            const ImpRepItem* it = &g_build.items[i];
            if (it->kind != k) continue;
            if (!head) fprintf(f, "\n[%s]\n", TR(imprep_kind_name(k))), head = true;
            fprintf(f, "- %s: %s", TR(imprep_cat_name(it->cat)), it->item);
            if (it->owner[0]) fprintf(f, " (%s%s)", it->owner, it->count > 1 ? " +" : "");
            if (it->count > 1) fprintf(f, " x%u", it->count);
            if (it->reason[0]) fprintf(f, ": %s", it->reason);
            fputc('\n', f);
        }
    }
    fclose(f);
    printf("[RealEngine] Import report written: %s\n", g_build.file);
    // the lost ones also go to the log (the tests read them)
    for (int i = 0; i < g_build.n; i++)
        if (g_build.items[i].kind == IMPREP_LOST)
            printf("[RealEngine] Import lost: %s %s (%s): %s\n", imprep_cat_name(g_build.items[i].cat), g_build.items[i].item,
                   g_build.items[i].owner, g_build.items[i].reason);
    return true;
}

void imprep_publish(void) {
    if (!g_build.n && !g_build.source[0]) return;
    report_free(&g_shown);
    g_shown = g_build; // (the importer is done with it: one import at a time)
    memset(&g_build, 0, sizeof g_build);
}

const ImpRepItem* imprep_items(int* count, const char** source, const char** file) {
    if (count) *count = g_shown.n;
    if (source) *source = g_shown.source;
    if (file) *file = g_shown.file;
    return g_shown.items;
}
