// --async-test (the test async, notas-dev/async.md): the editor keeps drawing while the heavy work runs in the background.
// In a project with the heavy test's meshes (2 M triangles: their reading is slow), at once:
//   - the meshes read again (render_meshes_reload_async), after one read the old way (its freeze measured),
//   - a glTF imported and placed (editor_import_async),
//   - every material baked again (a few per frame, their images decoded by the workers),
//   - a step task (30 slices on the main thread) and a long task cancelled from its card's button (async_cancel).
// While they run: the worst frame, the cards drawn (full, then thin). When all are done: the results are in place.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "async.h"
#include "async_ui.h"
#include "bp_editor.h"
#include "content.h"
#include "editor.h"
#include "heavy_test.h"
#include "material.h"
#include "platform.h"
#include "project.h"
#include "render.h"
#include "vkctx.h"

static struct {
    int phase; // 0 waiting for the project, 1 running, 2 done
    int frames_wait;
    double t0, t_end, worst_ms, sync_ms;
    int frames, card_frames, thin_frames;
    uint32_t meshes0, alive0, cancel_id, steps_done;
    int shot_frame;
    bool cancel_sent;
    AsyncStats s0;
    char src[1024]; // the file imported (--async-test <file>)
} T;

void async_test_set_source(const char* path) { snprintf(T.src, sizeof T.src, "%s", path); }

static void long_work(AsyncTask* t, void* arg) { // (stopped by its Cancel; 20 s if not)
    (void)arg;
    async_status(t, "%s", "Waiting to be cancelled");
    for (int i = 0; i < 2000 && !async_cancelled(t); i++) {
        async_progress(t, (float)i / 2000.0f);
        re_sleep_seconds(0.01);
    }
}

static bool steps_step(AsyncTask* t, void* arg) {
    (void)arg;
    T.steps_done++;
    async_progress_n(t, (int)T.steps_done, 30);
    async_status(t, "Slice %u", T.steps_done);
    return T.steps_done >= 30;
}

void async_test_frame(Editor* ed, double dt) {
    if (T.phase == 0) {
        if (!project_file()[0] || ++T.frames_wait < 5) return;
        HeavyAssets ha;
        if (!heavy_test_assets(content_root(), &ha)) printf("[RealEngine] FAIL: async test: the heavy assets not written\n");
        // the old way: the editor stopped while the meshes are read and sent
        double s = re_time_seconds();
        render_meshes_reload(ed->vk);
        T.sync_ms = (re_time_seconds() - s) * 1000.0;
        T.meshes0 = render_mesh_count();
        T.alive0 = ed->world.alive;
        T.s0 = async_stats();
        // everything at once, in the background
        T.t0 = re_time_seconds();
        render_meshes_reload_async(ed->vk);
        editor_import_async(ed, T.src, "/Content/AsyncImport", true);
        for (uint32_t i = 0; i < mat_count(); i++) mat_mark_dirty(mat_at(i));
        async_steps("Async test: steps", steps_step, NULL, NULL, 0);
        T.cancel_id = async_run("Async test: cancelled", long_work, NULL, NULL, ASYNC_CANCELLABLE);
        async_ui_set_compact(false);
        T.phase = 1;
        printf("[RealEngine] Async test: started (the meshes read the old way first: the editor stopped %.1f ms)\n", T.sync_ms);
        return;
    }
    if (T.phase != 1) return;
    double now = re_time_seconds();
    if (T.frames > 0 && dt * 1000.0 > T.worst_ms) T.worst_ms = dt * 1000.0;
    if (T.frames > 0 && dt * 1000.0 > 30.0) { // (what the long frames had: the last slice of the tasks, the materials left)
        int dirty = 0;
        for (uint32_t i = 0; i < mat_count(); i++) dirty += mat_at(i)->dirty;
        printf("[RealEngine] Async test: frame %d took %.1f ms (tasks' slice %.1f ms, %d materials to bake, %d tasks)\n", T.frames,
               dt * 1000.0, async_stats().main_ms_last, dirty, async_busy());
    }
    T.frames++;
    if (async_ui_cards_shown() > 0) T.card_frames++;
    double el = now - T.t0;
    // REALENGINE_ASYNC_SHOT=<path without .png>: the cards and the thin bars as pictures (notas-dev/async.md); three
    // example tasks of different speeds show the stack (a picture takes ~0.8 s: the steps follow the pictures, not the clock)
    const char* shot = getenv("REALENGINE_ASYNC_SHOT");
    static int shots;
    static uint32_t demo[3];
    static char sp[2][600];
    if (shot && !demo[0]) {
        static const char* const names[3] = { "Exemplo: importando Personagem.fbx", "Exemplo: compilando 12 materiais", "Exemplo: empacotando o jogo" };
        for (int k = 0; k < 3; k++) demo[k] = async_run(names[k], long_work, NULL, NULL, ASYNC_CANCELLABLE);
    }
    if (shot && shots == 0 && el > 0.5) snprintf(sp[0], sizeof sp[0], "%s_cards.png", shot), vkctx_request_shot(ed->vk, sp[0]), shots = 1, T.shot_frame = T.frames;
    if (shot && shots == 1 && T.frames > T.shot_frame + 3) async_ui_set_compact(true), shots = 2, T.shot_frame = T.frames;
    if (shot && shots == 2 && T.frames > T.shot_frame + 3) snprintf(sp[1], sizeof sp[1], "%s_thin.png", shot), vkctx_request_shot(ed->vk, sp[1]), shots = 3, T.shot_frame = T.frames;
    if (shot && shots == 3 && T.frames > T.shot_frame + 2) {
        for (int k = 0; k < 3; k++) async_cancel(demo[k]);
        async_ui_set_compact(false), shots = 4;
    }
    if (!T.cancel_sent && el > 1.2 && (!shot || shots == 4)) async_cancel(T.cancel_id), T.cancel_sent = true; // (its card's Cancel)
    // the thin mode a moment (its bars drawn), then the cards again
    bool thin = shot ? shots == 2 || shots == 3 : (el >= 0.7 && el < 0.95);
    if (!shot) async_ui_set_compact(thin);
    if (thin && async_ui_cards_shown() > 0) T.thin_frames++;
    if (async_idle() && T.cancel_sent && !render_meshes_reloading()) {
        T.t_end = now;
        T.phase = 2;
        async_ui_set_compact(false);
        printf("[RealEngine] Async test: all done in %.2f s, %d frames (worst %.1f ms; the meshes' main-thread part %.1f ms)\n",
               T.t_end - T.t0, T.frames, T.worst_ms, render_meshes_apply_ms());
    }
}

bool async_test_running(void) { return T.phase == 1; }

bool async_test_check(Editor* ed) {
    bool ok = true;
    AsyncStats s = async_stats();
    uint32_t finished = s.finished - T.s0.finished, failed = s.failed - T.s0.failed, cancelled = s.cancelled - T.s0.cancelled;
    printf("[RealEngine] Async test: %u tasks finished (%u failed, %u cancelled); meshes %u -> %u; actors %u -> %u; "
           "cards drawn in %d frames (thin %d); steps %u; worst frame %.1f ms (the old way stopped %.1f ms); main-thread slice max %.1f ms\n",
           finished, failed, cancelled, T.meshes0, render_mesh_count(), T.alive0, ed->world.alive, T.card_frames, T.thin_frames,
           T.steps_done, T.worst_ms, T.sync_ms, s.main_ms_max);
    if (T.phase != 2) printf("[RealEngine] FAIL: async test: the tasks did not finish\n"), ok = false;
    if (failed) printf("[RealEngine] FAIL: async test: %u tasks failed\n", failed), ok = false;
    if (cancelled < 1) printf("[RealEngine] FAIL: async test: the cancelled task was not cancelled\n"), ok = false;
    if (finished < 5) printf("[RealEngine] FAIL: async test: only %u tasks finished\n", finished), ok = false;
    if (T.steps_done != 30) printf("[RealEngine] FAIL: async test: the step task ran %u slices (30)\n", T.steps_done), ok = false;
    if (render_mesh_count() <= T.meshes0) printf("[RealEngine] FAIL: async test: the import's meshes are not in the renderer\n"), ok = false;
    if (ed->world.alive <= T.alive0) printf("[RealEngine] FAIL: async test: the import's actors were not placed\n"), ok = false;
    if (T.card_frames < 3 || T.thin_frames < 1) printf("[RealEngine] FAIL: async test: the cards were not drawn\n"), ok = false;
    int dirty = 0;
    for (uint32_t i = 0; i < mat_count(); i++) dirty += mat_at(i)->dirty;
    if (dirty) printf("[RealEngine] FAIL: async test: %d materials still not baked\n", dirty), ok = false;
    // the point: no frame near the old freeze, measured the same run (and none past 150 ms: the copies and BLAS of 2 M triangles are the most; another program on the GPU slows both)
    if (T.worst_ms > 150.0 || (T.sync_ms > 50.0 && T.worst_ms > T.sync_ms * 0.6))
        printf("[RealEngine] FAIL: async test: a frame took %.1f ms while the work ran (the old way: %.1f ms)\n", T.worst_ms, T.sync_ms), ok = false;
    if (ok) printf("[RealEngine] Async OK\n");
    return ok;
}
