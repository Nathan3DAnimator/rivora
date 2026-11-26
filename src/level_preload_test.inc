// (2026-11-23) Included by level_preload.c: the self test (--preload-test <folder>, ctest preload). A level of 40 000
// actors written to the folder; preloaded slowly (0.05) while frames run (each frame the main thread's share measured),
// then fast (1): the slow one must take longer and cost the frames almost nothing; taken into a world: every actor
// there, the camera the file's, the swap under 5 ms; a preload taken before it finished: done at full speed, the pump
// called; cancelled: nothing left.

static double run_until_ready(const char* path, float speed, double* frame_max, uint32_t* frames) {
    level_preload_cancel(NULL);
    P.st.frame_ms_max = 0.0;
    double t0 = re_time_seconds();
    if (!level_preload(path, speed)) return -1.0;
    *frames = 0;
    while (level_preload_state(path, NULL) != PRE_READY && re_time_seconds() - t0 < 60.0) {
        level_preload_update(); // (a frame: the main thread's part)
        (*frames)++;
        re_sleep_seconds(0.004); // (the rest of a frame)
        if (level_preload_state(path, NULL) == PRE_FAILED) return -1.0;
    }
    *frame_max = P.st.frame_ms_max;
    return (re_time_seconds() - t0) * 1000.0;
}

// (2026-11-24, the loading nodes' Loaded / Failed / Progress, bp_vm.c) What the VM reads each frame, checked here:
// the Progress of a Preload Level going 0 -> 1 without stepping back, then Ready (Loaded); a Load Stream Level of
// the same level: its Progress 0 -> 1 and loaded (level_stream_progress, level_stream_state by name), a missing one
// failed at once; Open Level of the preloaded level: the swap's time and the worst frame meanwhile under a limit
static bool loading_pins_test(const char* dir, const char* path, uint32_t alive) {
    bool ok = true;
    // Preload Level: Progress each frame
    level_preload_cancel(NULL);
    P.st.frame_ms_max = 0.0;
    level_preload(path, 0.2f);
    float last = -1.0f, first = -1.0f;
    int back = 0, frames = 0;
    double t0 = re_time_seconds(), worst = 0.0;
    for (;;) {
        double f0 = re_time_seconds();
        level_preload_update();
        double fms = (re_time_seconds() - f0) * 1000.0;
        worst = fms > worst ? fms : worst;
        float pr = 0.0f;
        PreloadState st = level_preload_state(path, &pr);
        if (first < 0.0f) first = pr;
        back += pr + 1e-6f < last, last = pr, frames++;
        if (st == PRE_READY || st == PRE_FAILED || re_time_seconds() - t0 > 60.0) {
            if (st != PRE_READY) printf("[RealEngine] FAIL: preload: Loaded never came (state %d)\n", (int)st), ok = false;
            break;
        }
        re_sleep_seconds(0.004);
    }
    printf("[RealEngine] preload: Preload Level's Progress %.2f -> %.2f over %d frames (%d steps back), Loaded\n", first, last, frames, back);
    if (back || first > 0.5f || last < 0.999f) printf("[RealEngine] FAIL: preload: the Progress didn't go 0 -> 1\n"), ok = false;
    // Open Level of it: the swap and the worst frame of the load (the main thread's)
    static World w2;
    ecs_init(&w2);
    LevelCamera cam;
    double swap = 0.0;
    bool took = level_preload_take(path, &w2, &cam, NULL, &swap);
    printf("[RealEngine] preload: Open Level of the preloaded level: swapped in %.3f ms (%u actors), the worst frame while it loaded %.3f ms\n", swap, w2.alive, worst);
    if (!took || w2.alive != alive || swap > 5.0 || worst > 8.0) printf("[RealEngine] FAIL: preload: Open Level hitched (swap %.3f ms, a frame %.3f ms)\n", swap, worst), ok = false;
    ecs_shutdown(&w2);
    // Load Stream Level: Progress and Loaded; a missing level: Failed
    level_stream_init(dir, "PersistentTest");
    static World w3;
    ecs_init(&w3);
    char name[160];
    const char* b = strrchr(path, '/');
    snprintf(name, sizeof name, "%s", b ? b + 1 : path);
    uint32_t req = level_stream_request(name, true, true);
    int res = 0, sframes = 0, sback = 0;
    float slast = -1.0f;
    t0 = re_time_seconds();
    while (!res && re_time_seconds() - t0 < 60.0) {
        level_stream_update(&w3, false);
        float pr = level_stream_progress(req, &res);
        sback += pr + 1e-6f < slast, slast = pr, sframes++;
        if (!res) re_sleep_seconds(0.004);
    }
    int by_name = 0;
    float pn = level_stream_state(name, &by_name);
    printf("[RealEngine] preload: Load Stream Level: Progress -> %.2f over %d frames, %s (by name: %.2f, %s), %u actors in\n", slast, sframes,
           res == 1 ? "Loaded" : res == 2 ? "Failed" : "going", pn, by_name == 1 ? "Loaded" : "not", w3.alive);
    if (res != 1 || sback || slast < 0.999f || by_name != 1 || w3.alive != alive) printf("[RealEngine] FAIL: preload: Load Stream Level's Loaded / Progress\n"), ok = false;
    uint32_t bad = level_stream_request("NoSuchLevel_123", true, true);
    int bres = 0;
    level_stream_progress(bad, &bres);
    int bname = 0;
    level_stream_state("NoSuchLevel_123", &bname);
    printf("[RealEngine] preload: Load Stream Level of a missing level: %s (by name: %s)\n", bres == 2 ? "Failed" : "not failed", bname == 2 ? "Failed" : "not failed");
    if (bres != 2 || bname != 2) printf("[RealEngine] FAIL: preload: a missing level didn't fail\n"), ok = false;
    ecs_shutdown(&w3);
    return ok;
}

static uint32_t g_pump_calls;
static void test_pump(float progress) { (void)progress, g_pump_calls++; }

bool level_preload_selftest(const char* dir) {
    bool ok = true;
    enum { N = 40000 };
    char path[800];
    snprintf(path, sizeof path, "%s/PreloadBig.rlevel", dir && dir[0] ? dir : ".");
    re_make_dirs(dir && dir[0] ? dir : ".");
    static World w;
    ecs_init(&w);
    for (uint32_t i = 0; i < N; i++) {
        Entity e = ecs_create(&w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_NAME));
        if (e == ENTITY_NONE) break;
        CTransform* t = ecs_get(&w, e, COMP_TRANSFORM);
        t->location[0] = (float)(i % 200) * 300.0f, t->location[1] = (float)(i / 200) * 300.0f;
        snprintf(((CName*)ecs_get(&w, e, COMP_NAME))->text, sizeof(((CName*)0)->text), "Rock_%u", i);
    }
    LevelCamera cam = { { 12.0f, 34.0f, 56.0f }, 7.0f, -8.0f, 90.0f };
    size_t bytes = 0;
    ok &= level_save(&w, &cam, path, &bytes);
    uint32_t alive = w.alive;
    ecs_shutdown(&w);
    if (!ok) {
        printf("[RealEngine] FAIL: preload: could not write %s\n", path);
        return false;
    }
    double fslow = 0, ffast = 0;
    uint32_t nslow = 0, nfast = 0;
    double slow = run_until_ready(path, 0.05f, &fslow, &nslow);
    double fast = run_until_ready(path, 1.0f, &ffast, &nfast);
    printf("[RealEngine] preload: %u actors, %.1f MB: at speed 0.05 ready in %.0f ms (%u frames, the main thread at most %.3f ms a frame); "
           "at 1 in %.0f ms (%u frames, at most %.3f ms)\n", alive, (double)bytes / 1048576.0, slow, nslow, fslow, fast, nfast, ffast);
    if (slow < 0.0 || fast < 0.0) printf("[RealEngine] FAIL: preload: it never got ready\n"), ok = false;
    else if (slow < fast * 1.5) printf("[RealEngine] FAIL: preload: the slow speed didn't pace it (%.0f vs %.0f ms)\n", slow, fast), ok = false;
    if (fslow > 1.0) printf("[RealEngine] FAIL: preload: at 0.05 a frame spent %.3f ms on it (over 1 ms)\n", fslow), ok = false;
    // the swap
    static World live;
    ecs_init(&live);
    for (int i = 0; i < 100; i++) ecs_create(&live, COMP_BIT(COMP_TRANSFORM)); // (the level being left)
    LevelCamera got;
    memset(&got, 0, sizeof got);
    double swap = 0.0;
    bool took = level_preload_take(path, &live, &got, NULL, &swap);
    printf("[RealEngine] preload: taken (%d): %u actors in the world, swapped in %.3f ms, camera (%.0f, %.0f, %.0f)\n", took, live.alive, swap, got.pos[0], got.pos[1], got.pos[2]);
    if (!took || live.alive != alive) printf("[RealEngine] FAIL: preload: the world taken has %u actors, wanted %u\n", live.alive, alive), ok = false;
    if (swap > 5.0) printf("[RealEngine] FAIL: preload: the swap took %.3f ms (over 5)\n", swap), ok = false;
    if (fabsf(got.pos[2] - 56.0f) > 0.01f) printf("[RealEngine] FAIL: preload: the file's camera wasn't kept\n"), ok = false;
    if (level_preload_state(path, NULL) != PRE_NONE) printf("[RealEngine] FAIL: preload: still listed after the take\n"), ok = false;
    // taken before it finished: finished at full speed, the loading screen's pump called
    level_preload(path, 0.01f);
    g_pump_calls = 0;
    double t0 = re_time_seconds();
    took = level_preload_take(path, &live, &got, test_pump, &swap);
    double early = (re_time_seconds() - t0) * 1000.0;
    printf("[RealEngine] preload: taken early (%d): finished in %.0f ms at full speed, %u loading screen frames\n", took, early, g_pump_calls);
    if (!took || live.alive != alive || !g_pump_calls) printf("[RealEngine] FAIL: preload: an early take didn't finish it\n"), ok = false;
    // cancelled
    level_preload(path, 0.05f);
    level_preload_cancel(path);
    if (level_preload_state(path, NULL) != PRE_NONE) printf("[RealEngine] FAIL: preload: cancel left it\n"), ok = false;
    // an unknown level
    if (level_preload("NoSuchLevel_123", 1.0f)) printf("[RealEngine] FAIL: preload: a missing level was accepted\n"), ok = false;
    if (level_preload_state("NoSuchLevel_123", NULL) != PRE_NONE) printf("[RealEngine] FAIL: preload: a missing level is listed (Failed wouldn't fire)\n"), ok = false;
    ok &= loading_pins_test(dir && dir[0] ? dir : ".", path, alive);
    ecs_shutdown(&live);
    printf("[RealEngine] preload: %s\n", ok ? "ok" : "FAILED");
    return ok;
}
