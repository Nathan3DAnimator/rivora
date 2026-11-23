// Async tasks (async.h): a table of 64, a lock (SRW: short sections, the status strings copied in and out), a fixed
// pool of low priority workers that sleep on a condition variable. The main thread finishes them (async_update).
#define WIN32_LEAN_AND_MEAN
#include "re_sync.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "async.h"
#include "platform.h"

#define MAX_TASKS 64
#define MAX_WORKERS 2

enum { K_WORK, K_STEPS, K_TRACK };

struct AsyncTask {
    uint32_t id, flags;
    uint8_t kind, state;
    bool used, work_over, done_called, dismissed, failed, cancel_sent;
    volatile long cancel;
    AsyncWorkFn work;
    AsyncStepFn step;
    AsyncDoneFn done;
    AsyncPollFn poll;
    void (*cancel_fn)(void* arg);
    void* arg;
    float progress;
    int ndone, ntotal;
    double t_start, t_end;
    char label[96], status[160];
};

static struct {
    ReLock lock;
    ReCond wake;
    AsyncTask t[MAX_TASKS];
    uint32_t next_id;
    ReThread* workers[MAX_WORKERS];
    int nworkers;
    volatile long quit;
    bool ready;
    uint32_t rr; // the step tasks' round robin
    AsyncStats stats;
} A;

static __declspec(thread) AsyncTask* t_cur;

static void lock(void) { re_lock(&A.lock); }
static void unlock(void) { re_unlock(&A.lock); }

static double linger(const AsyncTask* t) { // how long a finished card stays
    return t->state == ASYNC_FAILED ? 8.0 : t->state == ASYNC_CANCELLED ? 1.5 : 2.5;
}

static int worker_main(void* unused) {
    (void)unused;
    re_thread_set_low_priority(true); // (the editor's frame first: the work takes what is left of the cores)
    for (;;) {
        lock();
        AsyncTask* pick = NULL;
        while (!A.quit) {
            for (int i = 0; i < MAX_TASKS; i++) {
                AsyncTask* t = &A.t[i];
                if (t->used && t->kind == K_WORK && t->state == ASYNC_QUEUED && (!pick || t->id < pick->id)) pick = t;
            }
            if (pick) break;
            re_cond_wait(&A.wake, &A.lock);
        }
        if (A.quit) {
            unlock();
            return 0;
        }
        pick->state = ASYNC_RUNNING;
        pick->t_start = re_time_seconds(); // (the card's time: from when it really started)
        unlock();
        t_cur = pick;
        if (!pick->cancel) pick->work(pick, pick->arg);
        t_cur = NULL;
        lock();
        pick->work_over = true;
        unlock();
    }
}

void async_init(void) {
    if (A.ready) return;
    memset(&A, 0, sizeof A);
    re_lock_init(&A.lock);
    re_cond_init(&A.wake);
    A.next_id = 1;
    A.ready = true;
    int n = re_cpu_count() >= 4 ? MAX_WORKERS : 1;
    for (int i = 0; i < n; i++)
        if ((A.workers[A.nworkers] = re_thread_start(worker_main, NULL)) != NULL) A.nworkers++;
}

bool async_ready(void) { return A.ready && A.nworkers > 0 && !A.quit; }

void async_shutdown(void) {
    if (!A.ready) return;
    lock();
    A.quit = 1;
    for (int i = 0; i < MAX_TASKS; i++) // the queued work is dropped; the running finishes (it checks the flag)
        if (A.t[i].used) re_xchg32(&A.t[i].cancel, 1);
    re_cond_wake_all(&A.wake);
    unlock();
    for (int i = 0; i < A.nworkers; i++) re_thread_join(A.workers[i]);
    A.nworkers = 0;
    A.ready = false;
}

static AsyncTask* take(const char* label, uint8_t kind, uint32_t flags) {
    if (!A.ready || A.quit) return NULL;
    for (int i = 0; i < MAX_TASKS; i++) {
        AsyncTask* t = &A.t[i];
        if (t->used) continue;
        memset(t, 0, sizeof *t);
        t->used = true;
        t->id = A.next_id++;
        t->kind = kind, t->flags = flags;
        t->progress = -1.0f;
        t->t_start = re_time_seconds();
        snprintf(t->label, sizeof t->label, "%s", label ? label : "");
        A.stats.started++;
        return t;
    }
    return NULL;
}

uint32_t async_run(const char* label, AsyncWorkFn work, AsyncDoneFn done, void* arg, uint32_t flags) {
    lock();
    AsyncTask* t = A.nworkers ? take(label, K_WORK, flags) : NULL;
    if (t) {
        t->work = work, t->done = done, t->arg = arg;
        t->state = ASYNC_QUEUED;
        re_cond_wake_one(&A.wake);
    }
    uint32_t id = t ? t->id : 0;
    unlock();
    if (!t) { // no room (or no workers): right away, here
        AsyncTask tmp;
        memset(&tmp, 0, sizeof tmp);
        AsyncTask* keep = t_cur;
        t_cur = &tmp;
        work(&tmp, arg);
        t_cur = keep;
        if (done) done(&tmp, arg, !tmp.failed);
    }
    return id;
}

uint32_t async_steps(const char* label, AsyncStepFn step, AsyncDoneFn done, void* arg, uint32_t flags) {
    lock();
    AsyncTask* t = take(label, K_STEPS, flags);
    if (t) t->step = step, t->done = done, t->arg = arg, t->state = ASYNC_RUNNING;
    uint32_t id = t ? t->id : 0;
    unlock();
    if (!t) {
        AsyncTask tmp;
        memset(&tmp, 0, sizeof tmp);
        while (!step(&tmp, arg)) {}
        if (done) done(&tmp, arg, !tmp.failed);
    }
    return id;
}

uint32_t async_track(const char* label, AsyncPollFn poll, void (*cancel)(void* arg), void* arg, uint32_t flags) {
    lock();
    AsyncTask* t = take(label, K_TRACK, flags | (cancel ? ASYNC_CANCELLABLE : 0u));
    if (t) t->poll = poll, t->cancel_fn = cancel, t->arg = arg, t->state = ASYNC_RUNNING;
    uint32_t id = t ? t->id : 0;
    unlock();
    return id;
}

void async_status(AsyncTask* t, const char* fmt, ...) {
    if (!t) return;
    char s[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    lock();
    memcpy(t->status, s, sizeof s);
    unlock();
}

void async_progress(AsyncTask* t, float p) {
    if (!t) return;
    lock();
    t->progress = p > 1.0f ? 1.0f : p;
    unlock();
}

void async_progress_n(AsyncTask* t, int done, int total) {
    if (!t) return;
    lock();
    t->ndone = done, t->ntotal = total;
    t->progress = total > 0 ? (float)done / (float)total : -1.0f;
    unlock();
}

bool async_cancelled(const AsyncTask* t) { return t && (t->cancel || A.quit); }

void async_fail(AsyncTask* t, const char* fmt, ...) {
    if (!t) return;
    char s[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    lock();
    t->failed = true;
    memcpy(t->status, s, sizeof s);
    unlock();
}

AsyncTask* async_current(void) { return t_cur; }

// main thread: a task finished (its done callback with the lock released: it may start other tasks)
static void finish(AsyncTask* t, double now) {
    bool ok = !t->failed && !t->cancel;
    AsyncDoneFn done = t->done;
    t->done_called = true;
    if (done) {
        AsyncTask* keep = t_cur;
        t_cur = t;
        unlock();
        done(t, t->arg, ok);
        lock();
        t_cur = keep;
    }
    t->state = t->failed ? ASYNC_FAILED : t->cancel ? ASYNC_CANCELLED : ASYNC_DONE;
    t->t_end = now;
    if (t->state == ASYNC_DONE) t->progress = 1.0f;
    A.stats.finished++;
    A.stats.failed += t->state == ASYNC_FAILED;
    A.stats.cancelled += t->state == ASYNC_CANCELLED;
}

void async_update(double budget_ms) {
    if (!A.ready) return;
    double t0 = re_time_seconds(), now = t0;
    lock();
    // the finished work: its result put in place
    for (int i = 0; i < MAX_TASKS; i++) {
        AsyncTask* t = &A.t[i];
        if (t->used && t->kind == K_WORK && t->work_over && !t->done_called) finish(t, re_time_seconds());
    }
    // the tracked: their progress (and a Cancel passed on)
    for (int i = 0; i < MAX_TASKS; i++) {
        AsyncTask* t = &A.t[i];
        if (!t->used || t->kind != K_TRACK || t->done_called) continue;
        if (t->cancel && !t->cancel_sent && t->cancel_fn) {
            t->cancel_sent = true;
            unlock();
            t->cancel_fn(t->arg);
            lock();
        }
        char st[160] = "";
        AsyncPollFn poll = t->poll;
        void* arg = t->arg;
        unlock();
        float p = poll(arg, st, sizeof st);
        lock();
        if (st[0]) memcpy(t->status, st, sizeof st);
        if (p < 0.0f) finish(t, re_time_seconds());
        else t->progress = p;
    }
    // the steps: each at least once, then round robin within the budget
    int nsteps = 0;
    AsyncTask* steps[MAX_TASKS];
    for (int i = 0; i < MAX_TASKS; i++) {
        AsyncTask* t = &A.t[(i + A.rr) % MAX_TASKS];
        if (t->used && t->kind == K_STEPS && !t->done_called) steps[nsteps++] = t;
    }
    A.rr++;
    for (int round = 0; nsteps > 0; round++) {
        bool any = false;
        for (int i = 0; i < nsteps; i++) {
            AsyncTask* t = steps[i];
            if (!t || t->done_called) continue;
            if (round > 0 && (re_time_seconds() - t0) * 1000.0 >= budget_ms) break;
            bool over = t->cancel != 0;
            if (!over) {
                unlock();
                AsyncTask* keep = t_cur;
                t_cur = t;
                over = t->step(t, t->arg);
                t_cur = keep;
                lock();
            }
            if (over) finish(t, re_time_seconds()), steps[i] = NULL;
            else any = true;
        }
        if (!any || (re_time_seconds() - t0) * 1000.0 >= budget_ms) break;
    }
    // the finished cards' slots back (a hidden one at once; the rest when their card is gone)
    now = re_time_seconds();
    for (int i = 0; i < MAX_TASKS; i++) {
        AsyncTask* t = &A.t[i];
        if (t->used && t->done_called && ((t->flags & ASYNC_HIDDEN) || t->dismissed || now > t->t_end + linger(t))) t->used = false;
    }
    unlock();
    double ms = (re_time_seconds() - t0) * 1000.0;
    A.stats.main_ms_last = ms;
    if (ms > A.stats.main_ms_max) A.stats.main_ms_max = ms;
}

int async_busy(void) {
    int n = 0;
    lock();
    for (int i = 0; i < MAX_TASKS; i++) n += A.t[i].used && !A.t[i].done_called;
    unlock();
    return n;
}

bool async_idle(void) { return async_busy() == 0; }

void async_wait_all(double max_s) {
    double t0 = re_time_seconds();
    while (!async_idle() && re_time_seconds() - t0 < max_s) {
        async_update(50.0);
        if (!async_idle()) re_sleep_ms(1);
    }
}

void async_cancel(uint32_t id) {
    lock();
    for (int i = 0; i < MAX_TASKS; i++)
        if (A.t[i].used && A.t[i].id == id && !A.t[i].done_called) re_xchg32(&A.t[i].cancel, 1);
    unlock();
}

bool async_running(uint32_t id) {
    bool r = false;
    if (!id) return false;
    lock();
    for (int i = 0; i < MAX_TASKS; i++)
        if (A.t[i].used && A.t[i].id == id) r = !A.t[i].done_called;
    unlock();
    return r;
}

int async_snapshot(AsyncView* out, int cap, double now) {
    int n = 0;
    lock();
    // oldest first (by id)
    uint32_t last = 0;
    for (;;) {
        AsyncTask* best = NULL;
        for (int i = 0; i < MAX_TASKS; i++) {
            AsyncTask* t = &A.t[i];
            if (t->used && t->id > last && (!best || t->id < best->id)) best = t;
        }
        if (!best || n >= cap) break;
        last = best->id;
        AsyncTask* t = best;
        if ((t->flags & ASYNC_HIDDEN) || t->dismissed) continue;
        double took = (t->done_called ? t->t_end : now) - t->t_start;
        double show_after = (t->flags & ASYNC_QUIET) ? 0.4 : 0.15; // (quick ones never show: no flicker)
        if (t->state == ASYNC_QUEUED || took < show_after) {
            if (!(t->state == ASYNC_QUEUED && now - t->t_start > 0.15)) continue; // (waiting long for a worker: shown)
        }
        if (t->done_called && now > t->t_end + linger(t)) continue;
        AsyncView* v = &out[n++];
        v->id = t->id, v->flags = t->flags, v->state = t->state;
        v->progress = t->progress, v->done = t->ndone, v->total = t->ntotal;
        v->t_start = t->t_start, v->t_end = t->done_called ? t->t_end : 0.0;
        memcpy(v->label, t->label, sizeof v->label);
        memcpy(v->status, t->status, sizeof v->status);
    }
    unlock();
    return n;
}

void async_dismiss(uint32_t id) {
    lock();
    for (int i = 0; i < MAX_TASKS; i++)
        if (A.t[i].used && A.t[i].id == id && A.t[i].done_called) A.t[i].dismissed = true;
    unlock();
}

AsyncStats async_stats(void) { return A.stats; }
