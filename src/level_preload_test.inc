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
    ecs_shutdown(&live);
    printf("[RealEngine] preload: %s\n", ok ? "ok" : "FAILED");
    return ok;
}
