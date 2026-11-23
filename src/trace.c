// Trace (B14b): the recording and the file (trace.h). One buffer of TRACE_MAX_EVENTS made by trace_start; each event
// takes its slot with one atomic add (the workers record their parallel jobs at once); trace_stop hands the buffer to a
// thread that writes the JSON and frees it.
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform_files.h"

#if defined(_MSC_VER)
#define RE_TLS __declspec(thread)
#else
#define RE_TLS _Thread_local
#endif

enum { EV_SPAN = 0, EV_COUNTER = 1 };
typedef struct Ev { double t0, t1; const char* name; uint8_t track, kind; } Ev; // counter: t1 = its value

volatile bool g_trace_on;
static struct {
    Ev* ev;
    volatile int32_t n;
    volatile int32_t tracks;           // tracks taken (0: the GPU, 1: the thread that started)
    const char* track_name[32];
    char worker_names[32][16];
    double start;
    char path[700];
    volatile int32_t writing;
    char written[700];
} T;
static RE_TLS int t_track = -1;
static RE_TLS uint32_t t_gen;           // (the recording a thread's track belongs to)
static uint32_t g_gen;

static int my_track(void) {
    if (t_track >= 0 && t_gen == g_gen) return t_track;
    int32_t k = re_atomic_add(&T.tracks, 1) - 1;
    if (k >= 32) k = 31; // (more threads than tracks: the last one shared)
    else if (!T.track_name[k]) {
        snprintf(T.worker_names[k], sizeof T.worker_names[k], "Worker %d", k - 1);
        T.track_name[k] = T.worker_names[k];
    }
    t_track = k, t_gen = g_gen;
    return k;
}

static void push(const char* name, double t0, double t1, int track, uint8_t kind) {
    int32_t i = re_atomic_add(&T.n, 1) - 1;
    if (i >= (int32_t)TRACE_MAX_EVENTS) return; // full: the rest dropped (trace_events says so)
    Ev* e = &T.ev[i];
    e->t0 = t0, e->t1 = t1, e->name = name, e->track = (uint8_t)track, e->kind = kind;
}

void trace_scope(const char* name, double t0) {
    if (!g_trace_on || t0 <= 0.0) return;
    push(name, t0, re_time_seconds(), my_track(), EV_SPAN);
}
void trace_span(const char* name, double t0, double t1) {
    if (g_trace_on) push(name, t0, t1, my_track(), EV_SPAN);
}
void trace_counter(const char* name, double value) {
    if (g_trace_on) push(name, re_time_seconds(), value, my_track(), EV_COUNTER);
}
void trace_name_thread(const char* name) {
    if (g_trace_on) T.track_name[my_track()] = name;
}

void trace_frame(double t0, double t1, const char* const* gpu_names, const double* gpu_ms, uint32_t ngpu) {
    if (!g_trace_on) return;
    push("Frame", t0, t1, my_track(), EV_SPAN);
    // the GPU's frames one after the other (each measured, not when it began): from the frame's start, or where the
    // last one ended (a GPU bound frame is longer than the CPU's; behind by over 50 ms: back to the frame's start)
    static double gpu_end;
    double at = gpu_end > t0 && gpu_end < t0 + 0.05 ? gpu_end : t0;
    const double g0 = at;
    for (uint32_t i = 0; i < ngpu; i++) {
        if (gpu_ms[i] <= 0.0) continue;
        push(gpu_names[i], at, at + gpu_ms[i] * 0.001, 0, EV_SPAN);
        at += gpu_ms[i] * 0.001;
    }
    if (at > g0) push("GPU Frame", g0, at, 0, EV_SPAN), gpu_end = at;
    push("FPS", t1, t1 > t0 ? 1.0 / (t1 - t0) : 0.0, my_track(), EV_COUNTER);
}

static void pool_span(double t0, double t1, int items) {
    (void)items;
    trace_span("Parallel Work", t0, t1);
}

bool trace_start(const char* path) {
    if (g_trace_on || re_atomic_load(&T.writing)) return false;
    Ev* ev = malloc(sizeof(Ev) * TRACE_MAX_EVENTS); // (once per recording, never in the frame)
    if (!ev) return false;
    memset(&T.track_name, 0, sizeof T.track_name);
    T.ev = ev, T.n = 0, T.tracks = 1;
    T.track_name[0] = "GPU";
    g_gen++;
    t_track = -1;
    int me = my_track();
    T.track_name[me] = "Game Thread";
    snprintf(T.path, sizeof T.path, "%s", path);
    T.start = re_time_seconds();
    T.written[0] = 0;
    g_re_pool_span = pool_span;
    g_trace_on = true;
    return true;
}

bool trace_active(void) { return g_trace_on; }
bool trace_writing(void) { return re_atomic_load(&T.writing) != 0; }
uint32_t trace_events(void) {
    int32_t n = T.n;
    return n > (int32_t)TRACE_MAX_EVENTS ? TRACE_MAX_EVENTS : (uint32_t)n;
}
double trace_seconds(void) { return g_trace_on ? re_time_seconds() - T.start : 0.0; }

typedef struct WriteJob { Ev* ev; uint32_t n; double start; const char* names[32]; uint32_t ntracks; char path[700]; } WriteJob;

static void put_name(FILE* f, const char* s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        if ((unsigned char)*s >= 0x20) fputc(*s, f);
    }
    fputc('"', f);
}

static int write_thread(void* arg) {
    WriteJob* j = arg;
    re_thread_set_low_priority(true);
    FILE* f = re_fopen(j->path, "wb");
    if (f) {
        setvbuf(f, NULL, _IOFBF, 1 << 20);
        fputs("{\"displayTimeUnit\":\"ms\",\"otherData\":{\"engine\":\"Real Engine\"},\"traceEvents\":[\n", f);
        fputs("{\"ph\":\"M\",\"name\":\"process_name\",\"pid\":1,\"tid\":0,\"args\":{\"name\":\"Real Engine\"}}", f);
        for (uint32_t k = 0; k < j->ntracks; k++) {
            fprintf(f, ",\n{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":1,\"tid\":%u,\"args\":{\"name\":", k);
            put_name(f, j->names[k] ? j->names[k] : "Thread");
            fprintf(f, "}}");
            fprintf(f, ",\n{\"ph\":\"M\",\"name\":\"thread_sort_index\",\"pid\":1,\"tid\":%u,\"args\":{\"sort_index\":%u}}", k, k == 0 ? 1000u : k);
        }
        for (uint32_t i = 0; i < j->n; i++) {
            const Ev* e = &j->ev[i];
            if (!e->name) continue;
            fputs(",\n{\"ph\":\"", f);
            fputs(e->kind == EV_COUNTER ? "C\",\"name\":" : "X\",\"name\":", f);
            put_name(f, e->name);
            if (e->kind == EV_COUNTER)
                fprintf(f, ",\"ts\":%.3f,\"pid\":1,\"tid\":%u,\"args\":{\"value\":%.4f}}", (e->t0 - j->start) * 1e6, e->track, e->t1);
            else
                fprintf(f, ",\"ts\":%.3f,\"dur\":%.3f,\"pid\":1,\"tid\":%u}", (e->t0 - j->start) * 1e6,
                        (e->t1 > e->t0 ? e->t1 - e->t0 : 0.0) * 1e6, e->track);
        }
        fputs("\n]}\n", f);
        fclose(f);
    }
    free(j->ev);
    free(j);
    re_atomic_store(&T.writing, 0);
    return 0;
}

const char* trace_stop(void) {
    if (!g_trace_on) return "";
    g_trace_on = false;
    g_re_pool_span = NULL;
    re_sleep_seconds(0.002); // (a worker in the middle of its push ends it: the buffer is handed over after)
    WriteJob* j = malloc(sizeof *j);
    if (!j) {
        free(T.ev), T.ev = NULL;
        return "";
    }
    j->ev = T.ev, j->n = trace_events(), j->start = T.start;
    j->ntracks = T.tracks > 32 ? 32 : (uint32_t)T.tracks;
    memcpy((void*)j->names, (const void*)T.track_name, sizeof j->names);
    snprintf(j->path, sizeof j->path, "%s", T.path);
    T.ev = NULL;
    snprintf(T.written, sizeof T.written, "%s", T.path);
    {
        char dir[700];
        snprintf(dir, sizeof dir, "%s", T.path);
        char* s = strrchr(dir, '/');
        char* b = strrchr(dir, '\\');
        if (b > s) s = b;
        if (s) *s = 0, re_make_dirs(dir);
    }
    re_atomic_store(&T.writing, 1);
    ReThread* th = re_thread_start(write_thread, j);
    if (!th) write_thread(j); // (no thread: here)
    return T.written;
}

// --- reading a file back (only the shape trace_stop writes; Chrome's other fields are skipped) ---

static const char* field(const char* line, const char* key) {
    const char* p = strstr(line, key);
    return p ? p + strlen(key) : NULL;
}

static uint32_t intern(TraceFile* f, const char* s, size_t len, uint32_t* cap) {
    if (len > 47) len = 47;
    for (uint32_t i = 0; i < f->nnames; i++)
        if (!strncmp(f->names[i], s, len) && f->names[i][len] == 0) return i;
    if (f->nnames == *cap) {
        uint32_t nc = *cap ? *cap * 2 : 64;
        void* p = realloc(f->names, nc * sizeof f->names[0]);
        if (!p) return 0;
        f->names = p, *cap = nc;
    }
    memcpy(f->names[f->nnames], s, len);
    f->names[f->nnames][len] = 0;
    return f->nnames++;
}

static int ev_order(const void* a, const void* b) {
    const TraceEv* x = a;
    const TraceEv* y = b;
    if (x->track != y->track) return x->track < y->track ? -1 : 1;
    if (x->ts != y->ts) return x->ts < y->ts ? -1 : 1;
    return x->dur > y->dur ? -1 : x->dur < y->dur ? 1 : 0; // (the parent first: longer)
}

bool trace_load(const char* path, TraceFile* f) {
    memset(f, 0, sizeof *f);
    FILE* fp = re_fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char* text = size > 0 ? malloc((size_t)size + 1) : NULL;
    if (!text || fread(text, 1, (size_t)size, fp) != (size_t)size) {
        free(text);
        fclose(fp);
        return false;
    }
    fclose(fp);
    text[size] = 0;
    uint32_t lines = 0;
    for (long i = 0; i < size; i++) lines += text[i] == '\n';
    f->ev = malloc(sizeof(TraceEv) * (lines + 1));
    uint32_t ncap = 0;
    if (!f->ev) {
        free(text);
        return false;
    }
    for (char* line = text; line && *line;) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        const char* ph = field(line, "\"ph\":\"");
        const char* nm = field(line, "\"name\":\"");
        const char* tid = field(line, "\"tid\":");
        if (ph && nm && tid) {
            const char* ne = strchr(nm, '"');
            uint32_t track = (uint32_t)atoi(tid);
            if (track < 32 && ne) {
                if (*ph == 'M' && !strncmp(nm, "thread_name", 11)) {
                    const char* a = field(line, "\"args\":{\"name\":\"");
                    const char* ae = a ? strchr(a, '"') : NULL;
                    if (ae) {
                        size_t len = (size_t)(ae - a) < 39 ? (size_t)(ae - a) : 39;
                        memcpy(f->tracks[track], a, len), f->tracks[track][len] = 0;
                        if (track + 1 > f->ntracks) f->ntracks = track + 1;
                    }
                } else if (*ph == 'X' || *ph == 'C') {
                    const char* ts = field(line, "\"ts\":");
                    TraceEv* e = &f->ev[f->n];
                    memset(e, 0, sizeof *e);
                    e->ts = ts ? atof(ts) * 1e-6 : 0.0;
                    e->track = (uint16_t)track;
                    e->name = intern(f, nm, (size_t)(ne - nm), &ncap);
                    if (*ph == 'X') {
                        const char* d = field(line, "\"dur\":");
                        e->dur = d ? atof(d) * 1e-6 : 0.0;
                    } else {
                        const char* v = field(line, "\"value\":");
                        e->counter = 1, e->value = v ? (float)atof(v) : 0.0f;
                    }
                    if (track + 1 > f->ntracks) f->ntracks = track + 1;
                    if (e->ts + e->dur > f->t_end) f->t_end = e->ts + e->dur;
                    f->n++;
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(text);
    qsort(f->ev, f->n, sizeof f->ev[0], ev_order);
    // the depth of each span on its track (a stack of the open spans' ends), and the frames
    double ends[64];
    int top = 0;
    uint16_t cur = 0xFFFF;
    uint32_t frames = 0;
    for (uint32_t i = 0; i < f->n; i++) {
        TraceEv* e = &f->ev[i];
        if (e->track != cur) cur = e->track, top = 0;
        if (e->counter) continue;
        while (top > 0 && e->ts >= ends[top - 1] - 1e-9) top--;
        e->depth = (uint8_t)(top < 255 ? top : 255);
        if (top < 64) ends[top++] = e->ts + e->dur;
        if (e->depth + 1 > f->track_depth[e->track]) f->track_depth[e->track] = (uint8_t)(e->depth + 1);
        frames += !strcmp(f->names[e->name], "Frame");
    }
    f->frames = malloc(sizeof(double) * (frames + 1));
    f->frame_ms = malloc(sizeof(float) * (frames + 1));
    if (f->frames && f->frame_ms)
        for (uint32_t i = 0; i < f->n; i++)
            if (!f->ev[i].counter && !strcmp(f->names[f->ev[i].name], "Frame"))
                f->frames[f->nframes] = f->ev[i].ts, f->frame_ms[f->nframes++] = (float)(f->ev[i].dur * 1000.0);
    return true;
}

void trace_free(TraceFile* f) {
    free(f->ev), free(f->names), free(f->frames), free(f->frame_ms);
    memset(f, 0, sizeof *f);
}

int trace_finish_check(void) {
    if (!g_trace_on) return 0;
    char path[700];
    snprintf(path, sizeof path, "%s", trace_stop());
    for (int k = 0; k < 6000 && trace_writing(); k++) re_sleep_seconds(0.005);
    TraceFile f;
    if (!path[0] || !trace_load(path, &f)) {
        printf("[RealEngine] FAIL: the trace %s could not be read back\n", path);
        return 1;
    }
    uint32_t gpu = 0, game = 0, workers = 0, tracks = 0;
    uint32_t seen[32] = { 0 };
    for (uint32_t i = 0; i < f.n; i++) {
        const TraceEv* e = &f.ev[i];
        seen[e->track]++;
        if (e->track == 0) gpu++;
        else if (e->track == 1) game++;
        else workers++;
    }
    for (uint32_t t = 0; t < 32; t++) tracks += seen[t] != 0;
    printf("[RealEngine] Trace: %u events, %u frames, %u tracks, %u names (game thread %u, workers %u, GPU %u) in %.1f s -> %s\n",
           f.n, f.nframes, tracks, f.nnames, game, workers, gpu, f.t_end, path);
    int fail = f.nframes < 10 || !game;
    if (fail) printf("[RealEngine] FAIL: the trace has too few frames or no game thread scopes\n");
    trace_free(&f);
    return fail;
}
