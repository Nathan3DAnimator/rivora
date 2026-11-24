// (2026-11-24, report #180) Included by material.c after material_gpu.inc: --mat-live-test (ctest mat_live_edit). A
// 4096 material edited 60 times in a row as the Material Editor does while a value is dragged (live_until set, a new
// Base Color, a bake each time): every bake must reuse the same textures in RGBA8 (before: a new 4096 set and a BC7
// swap each edit, ~256 MB churned per edit until an allocation failed and the actor vanished) and stay valid; when the
// editing stops, the textures are compressed to BC7 once. Prints the edits' time.
bool mat_live_selftest(void) {
    char path[MAT_PATH * 2];
    snprintf(path, sizeof path, "%s\\M_LiveEdit.rmat", mat_root());
    Material* m = mat_create(path, false, NULL);
    if (!m || !m->graph) {
        printf("[RealEngine] FAIL: mat live: could not create %s\n", path);
        return false;
    }
    m->resolution = 4096;
    int bsdf = -1;
    for (int i = 0; i < m->graph->node_count; i++)
        if (m->graph->nodes[i].type == NODE_BSDF_PRINCIPLED) bsdf = i;
    mat_mark_dirty(m);
    mat_update(M.vk, VK_NULL_HANDLE); // (its first bake: BC7 when it isn't being edited)
    uint32_t idx = 0;
    while (idx < M.n && M.mats[idx] != m) idx++;
    if (idx >= M.n || bsdf < 0) {
        printf("[RealEngine] FAIL: mat live: the material isn't loaded\n");
        return false;
    }
    Compiled* c = &M.comp[idx];
    bool ok = true;
    uint32_t t0[2] = { 0, 0 }, swaps = 0;
    double start = re_time_seconds();
    for (int k = 0; k < 60; k++) {
        m->live_until = re_time_seconds() + 2.5; // (the editor's touch())
        m->graph->nodes[bsdf].in[0][0] = (float)k / 60.0f;
        mat_mark_dirty(m);
        mat_update(M.vk, VK_NULL_HANDLE);
        if (!c->valid || !c->v.tex[0]) {
            printf("[RealEngine] FAIL: mat live: edit %d left the material invalid\n", k);
            ok = false;
            break;
        }
        if (M.tex[c->v.tex[0]].format != VK_FORMAT_R8G8B8A8_UNORM) {
            printf("[RealEngine] FAIL: mat live: edit %d baked into %d, not RGBA8\n", k, (int)M.tex[c->v.tex[0]].format);
            ok = false;
        }
        if (k == 0) t0[0] = c->v.tex[0], t0[1] = c->v.tex[1];
        else if (c->v.tex[0] != t0[0] || c->v.tex[1] != t0[1]) swaps++;
    }
    double ms = (re_time_seconds() - start) * 1000.0 / 60.0;
    if (swaps) printf("[RealEngine] FAIL: mat live: %u edits made new textures (wanted the same ones reused)\n", swaps), ok = false;
    // the editing stops: compressed once
    m->live_until = 0.0;
    mat_update(M.vk, VK_NULL_HANDLE);
    bool bc7 = !bc7_on() || M.tex[c->v.tex[0]].format == VK_FORMAT_BC7_UNORM_BLOCK;
    if (!bc7 || m->bc7_pending) printf("[RealEngine] FAIL: mat live: not compressed when the editing stopped\n"), ok = false;
    printf("[RealEngine] mat live: 60 edits of a 4096 material, %.1f ms a bake, the same textures each time (%u swaps), BC7 after: %d\n", ms, swaps, bc7);
    printf("[RealEngine] mat live: %s\n", ok ? "ok" : "FAILED");
    return ok;
}
