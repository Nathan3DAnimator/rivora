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

// (2026-11-24) --mat-param-test (ctest mat_live_param, notas-dev/materiais.md "Parâmetros vivos"): the affine fold. A material
// whose base color is a noise times a Tint parameter (Mix Multiply), its roughness the noise's Fac times a Rough parameter,
// its emission the noise's color times a Glow parameter (and the Emission Intensity), its noise's Scale a parameter the bake
// reads. A Material Instance with its own values and an actor's dynamic instance set 1000 times by Set ... Parameter Value
// must not bake (bake_variant's count), and the slot's coefficients over the baked roots must give what a full bake of the
// same values gives (pixel by pixel, the fold off for that bake); the Scale must still bake; an edit of a folding value in
// the Material Editor writes the slots only; 300 rebakes keep the parameters' and the programs' room (before: they only grew)
static int lp_param(const Compiled* c, const char* name) {
    for (int i = 0; i < c->nparams; i++)
        if (!strcmp(c->pnames[i], name)) return i;
    return -1;
}
static void lp_graph(MatGraph* g, const float tint[3], float rough, float glow) {
    mat_graph_default(g);
    int bsdf = -1;
    for (int i = 0; i < g->node_count; i++)
        if (g->nodes[i].type == NODE_BSDF_PRINCIPLED) bsdf = i;
    int noise = mat_graph_add(g, NODE_TEX_NOISE, -700, 0);
    int tn = mat_graph_add(g, NODE_RGB, -700, 250);
    snprintf(g->nodes[tn].param, sizeof g->nodes[tn].param, "Tint");
    g->nodes[tn].value[0] = tint[0], g->nodes[tn].value[1] = tint[1], g->nodes[tn].value[2] = tint[2], g->nodes[tn].value[3] = 1.0f;
    int mix = mat_graph_add(g, NODE_MIX, -400, 0);
    g->nodes[mix].prop[1] = BLEND_MUL, g->nodes[mix].in[0][0] = 1.0f;
    mat_graph_link(g, noise, 1, mix, 5), mat_graph_link(g, tn, 0, mix, 6), mat_graph_link(g, mix, 2, bsdf, 0);
    int rv = mat_graph_add(g, NODE_VALUE, -700, 450);
    snprintf(g->nodes[rv].param, sizeof g->nodes[rv].param, "Rough");
    g->nodes[rv].value[0] = rough;
    int mul = mat_graph_add(g, NODE_MATH, -400, 300);
    g->nodes[mul].prop[0] = MATH_MULTIPLY;
    mat_graph_link(g, noise, 0, mul, 0), mat_graph_link(g, rv, 0, mul, 1), mat_graph_link(g, mul, 0, bsdf, 2);
    int gv = mat_graph_add(g, NODE_VALUE, -700, 600);
    snprintf(g->nodes[gv].param, sizeof g->nodes[gv].param, "Glow");
    g->nodes[gv].value[0] = glow;
    mat_graph_link(g, gv, 0, bsdf, 27), mat_graph_link(g, noise, 1, bsdf, 26);
    int sv = mat_graph_add(g, NODE_VALUE, -1000, 0);
    snprintf(g->nodes[sv].param, sizeof g->nodes[sv].param, "Scale");
    g->nodes[sv].value[0] = 5.0f;
    mat_graph_link(g, sv, 0, noise, 2);
}
static bool lp_read(uint32_t tex, void* out, size_t bytes) {
    const MatTex* t = &M.tex[tex];
    GpuBuf b = { 0 };
    if (!t->image || !gpu_buf_create(M.vk, &b, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    VkCommandBuffer cmd = gpu_begin_once(M.vk);
    VkBufferImageCopy r = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { t->w, t->h, 1 } };
    vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, b.buffer, 1, &r);
    gpu_end_once(M.vk, cmd);
    memcpy(out, b.mapped, bytes);
    gpu_buf_destroy(M.vk, &b);
    return true;
}
static float lp_lin(uint8_t s8) {
    float s = s8 / 255.0f;
    return s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
}

bool mat_param_selftest(void) {
    bool ok = true;
    VkPipeline bc7 = M.bc7;
    M.bc7 = VK_NULL_HANDLE; // (RGBA8: the textures read back as they were baked)
    char path[MAT_PATH * 2], vp[MAT_PATH + 16];
    snprintf(path, sizeof path, "%s\\M_LiveParam.rmat", mat_root());
    Material* m = mat_create(path, false, NULL);
    if (!m || !m->graph) {
        printf("[RealEngine] FAIL: mat param: could not create %s\n", path);
        return M.bc7 = bc7, false;
    }
    m->resolution = 256;
    lp_graph(m->graph, (const float[3]){ 1.0f, 1.0f, 1.0f }, 1.0f, 1.0f);
    mat_save(m);
    mat_mark_dirty(m);
    mat_update(M.vk, VK_NULL_HANDLE);
    uint32_t idx = 0;
    while (idx < M.n && M.mats[idx] != m) idx++;
    Compiled* c = idx < M.n ? &M.comp[idx] : NULL;
    if (!c || !c->valid) {
        printf("[RealEngine] FAIL: mat param: the material didn't compile (%s)\n", m->error);
        return M.bc7 = bc7, false;
    }
    int pt = lp_param(c, "Tint"), pr = lp_param(c, "Rough"), pg = lp_param(c, "Glow"), ps = lp_param(c, "Scale"), pe = lp_param(c, MAT_EMISSION_INTENSITY);
    printf("[RealEngine] mat param: %d parameters (Tint %d, Rough %d, Glow %d, Scale %d, Emission Intensity %d), bake mask 0x%x, fold %d, %u CPU instructions, %u terms\n",
           c->nparams, pt, pr, pg, ps, pe, c->aff.bake_mask, c->aff.on, c->aff.nins, c->aff.nterms);
    if (pt < 0 || pr < 0 || pg < 0 || ps < 0 || pe < 0) printf("[RealEngine] FAIL: mat param: a parameter is missing\n"), ok = false;
    else {
        if (c->aff.bake_mask & (1u << pt | 1u << pr | 1u << pg | 1u << pe)) printf("[RealEngine] FAIL: mat param: Tint, Rough, Glow or the Emission Intensity is baked\n"), ok = false;
        if (!(c->aff.bake_mask & (1u << ps))) printf("[RealEngine] FAIL: mat param: the noise's Scale isn't baked\n"), ok = false;
    }
    // a Material Instance with its own values: no bake (it shares its parent's textures)
    mat_vpath(m, vp, sizeof vp);
    char ipath[MAT_PATH * 2];
    snprintf(ipath, sizeof ipath, "%s\\MI_LiveParam.rmi", mat_root());
    Material* mi = mat_create(ipath, true, vp);
    uint32_t b0 = M.bake_calls;
    if (mi) {
        mi->nparams = 3;
        snprintf(mi->params[0].name, MAT_NAME, "Tint"), mi->params[0].kind = 1;
        memcpy(mi->params[0].value, (const float[4]){ 0.2f, 0.8f, 0.3f, 1.0f }, 16);
        snprintf(mi->params[1].name, MAT_NAME, "Rough"), mi->params[1].kind = 0;
        memcpy(mi->params[1].value, (const float[4]){ 0.5f, 0.5f, 0.5f, 0.5f }, 16);
        snprintf(mi->params[2].name, MAT_NAME, "Glow"), mi->params[2].kind = 0;
        memcpy(mi->params[2].value, (const float[4]){ 3.0f, 3.0f, 3.0f, 3.0f }, 16);
        mat_mark_dirty(mi);
        mat_update(M.vk, VK_NULL_HANDLE);
        int mii = mat_index(mi);
        bool shares = mii >= 0 && !g_inst_var[mii].own && mi->slot;
        printf("[RealEngine] mat param: Material Instance: %u bakes, shares its parent's textures: %d\n", M.bake_calls - b0, shares);
        if (M.bake_calls != b0 || !shares) printf("[RealEngine] FAIL: mat param: the Material Instance's values baked\n"), ok = false;
    } else printf("[RealEngine] FAIL: mat param: no Material Instance\n"), ok = false;
    // an actor's dynamic instance set by a Blueprint 1000 times
    uint32_t slot = mat_dynamic_for((Entity)777u, m);
    mat_update(M.vk, VK_NULL_HANDLE);
    b0 = M.bake_calls;
    double t0 = re_time_seconds();
    for (int k = 0; k < 1000; k++) {
        float f = (float)k / 1000.0f;
        mat_set_param(slot, "Tint", (const float[4]){ f, 1.0f - f, 0.5f, 1.0f });
        mat_set_param(slot, "Glow", (const float[4]){ f * 8.0f, 0, 0, 0 });
        mat_update(M.vk, VK_NULL_HANDLE);
    }
    double us = (re_time_seconds() - t0) * 1e6 / 2000.0;
    mat_set_param(slot, "Tint", (const float[4]){ 0.9f, 0.4f, 0.2f, 1.0f });
    mat_set_param(slot, "Rough", (const float[4]){ 0.6f, 0.6f, 0.6f, 0.6f });
    mat_set_param(slot, "Glow", (const float[4]){ 4.0f, 4.0f, 4.0f, 4.0f });
    mat_set_param(slot, MAT_EMISSION_INTENSITY, (const float[4]){ 2.0f, 2.0f, 2.0f, 2.0f });
    mat_update(M.vk, VK_NULL_HANDLE);
    bool dyn_shared = slot != m->slot && !M.dyn[M.ndyn - 1].v.own;
    printf("[RealEngine] mat param: Blueprint: 2000 Set Parameter Value + 1000 updates, %u bakes, %.2f us a value (with its update)\n", M.bake_calls - b0, us);
    if (M.bake_calls != b0 || !dyn_shared) printf("[RealEngine] FAIL: mat param: Set Parameter Value baked\n"), ok = false;
    // the image: the roots through the dynamic slot's coefficients against a full bake of the same values (the fold off)
    snprintf(path, sizeof path, "%s\\M_LiveParamRef.rmat", mat_root());
    Material* r = mat_create(path, false, NULL);
    Compiled* rc = NULL;
    if (r && r->graph) {
        r->resolution = 256;
        lp_graph(r->graph, (const float[3]){ 0.9f, 0.4f, 0.2f }, 0.6f, 4.0f);
        r->emission_intensity = 2.0f;
        g_aff_off = true;
        mat_mark_dirty(r);
        mat_update(M.vk, VK_NULL_HANDLE);
        g_aff_off = false;
        int ri = mat_index(r);
        rc = ri >= 0 && M.comp[ri].valid ? &M.comp[ri] : NULL;
        idx = (uint32_t)mat_index(m), c = &M.comp[idx]; // (M.comp may have moved)
    }
    if (!rc || !rc->v.tex[0] || !c->v.tex[0] || !c->v.tex[2] || !rc->v.tex[2]) printf("[RealEngine] FAIL: mat param: no reference bake\n"), ok = false;
    else {
        uint32_t n = 256u * 256u;
        uint8_t* fb = malloc(n * 4), *fn = malloc(n * 4), *rb = malloc(n * 4), *rn = malloc(n * 4);
        uint16_t* fe = malloc(n * 8), *re = malloc(n * 8);
        bool rd = fb && fn && rb && rn && fe && re && M.tex[c->v.tex[0]].w == 256 && M.tex[rc->v.tex[0]].w == 256 &&
                  lp_read(c->v.tex[0], fb, n * 4) && lp_read(c->v.tex[1], fn, n * 4) && lp_read(c->v.tex[2], fe, n * 8) &&
                  lp_read(rc->v.tex[0], rb, n * 4) && lp_read(rc->v.tex[1], rn, n * 4) && lp_read(rc->v.tex[2], re, n * 8);
        if (!rd) printf("[RealEngine] FAIL: mat param: the textures couldn't be read back\n"), ok = false;
        else {
            const GpuMatAff* A = aff_block(slot), *R = aff_block(r->slot);
            bool fa = ((GpuMaterial*)M.materials.mapped)[slot].flags & MATF_AFFINE, ra = ((GpuMaterial*)M.materials.mapped)[r->slot].flags & MATF_AFFINE;
            double eb = 0.0, er = 0.0, ee = 0.0;
            for (uint32_t i = 0; i < n; i++) {
                for (int k = 0; k < 3; k++) {
                    float b = lp_lin(fb[i * 4 + k]), rb0 = lp_lin(rb[i * 4 + k]);
                    if (fa) b = aff_clamp01(b * A->a0[k] + A->b0[k]);
                    if (ra) rb0 = aff_clamp01(rb0 * R->a0[k] + R->b0[k]);
                    eb = fmax(eb, fabs(b - rb0));
                    float e = fmaxf(mg_half(fe[i * 4 + k]), 0.0f), e0 = fmaxf(mg_half(re[i * 4 + k]), 0.0f);
                    if (fa) e = fmaxf(e * A->a1[k] + A->b1[k], 0.0f);
                    if (ra) e0 = fmaxf(e0 * R->a1[k] + R->b1[k], 0.0f);
                    ee = fmax(ee, fabs(e - e0) / (fabs(e0) + 0.05));
                }
                float ro = fn[i * 4 + 2] / 255.0f, r0 = rn[i * 4 + 2] / 255.0f;
                if (fa) ro = aff_clamp01(ro * A->a1[3] + A->b1[3]);
                if (ra) r0 = aff_clamp01(r0 * R->a1[3] + R->b1[3]);
                er = fmax(er, fabs(ro - r0));
            }
            printf("[RealEngine] mat param: folded against a full bake, 256x256: base %.4f, roughness %.4f, emission %.4f (relative) at most\n", eb, er, ee);
            if (eb > 0.02 || er > 0.012 || ee > 0.03) printf("[RealEngine] FAIL: mat param: the folded image isn't the full bake's\n"), ok = false;
        }
        free(fb), free(fn), free(rb), free(rn), free(fe), free(re);
    }
    // a parameter the bake reads still bakes
    b0 = M.bake_calls;
    mat_set_param(slot, "Scale", (const float[4]){ 9.0f, 9.0f, 9.0f, 9.0f });
    mat_update(M.vk, VK_NULL_HANDLE);
    printf("[RealEngine] mat param: the noise's Scale set: %u bakes\n", M.bake_calls - b0);
    if (M.bake_calls != b0 + 1) printf("[RealEngine] FAIL: mat param: the Scale didn't bake once\n"), ok = false;
    // the Material Editor: a folding value dragged (live) writes the slots only
    b0 = M.bake_calls;
    uint32_t lw = M.stats.live_writes;
    t0 = re_time_seconds();
    for (int k = 0; k < 60; k++) {
        m->live_until = re_time_seconds() + 2.5;
        for (int i = 0; i < m->graph->node_count; i++)
            if (m->graph->nodes[i].type == NODE_RGB) m->graph->nodes[i].value[0] = (float)k / 60.0f;
        mat_mark_dirty(m);
        mat_update(M.vk, VK_NULL_HANDLE);
    }
    double ems = (re_time_seconds() - t0) * 1000.0 / 60.0;
    printf("[RealEngine] mat param: Material Editor: 60 edits of the Tint, %u bakes, %u slot writes, %.3f ms an edit\n", M.bake_calls - b0, M.stats.live_writes - lw, ems);
    if (M.bake_calls != b0 || M.stats.live_writes - lw < 60) printf("[RealEngine] FAIL: mat param: the editor's value baked\n"), ok = false;
    m->live_until = 0.0;
    // 300 rebakes: the room stays (before, each took new parameters until 65536 vec4 and the values overlapped)
    mat_update(M.vk, VK_NULL_HANDLE);
    vkDeviceWaitIdle(M.vk->device), st_drain(true); // (the programs a bake in flight could read: given back now)
    uint32_t pf0 = rh_free_total(&M.pheap), gf0 = rh_free_total(&M.gheap);
    for (int k = 0; k < 300; k++) {
        mat_mark_dirty(m);
        mat_update(M.vk, VK_NULL_HANDLE);
    }
    vkDeviceWaitIdle(M.vk->device), st_drain(true);
    uint32_t pf1 = rh_free_total(&M.pheap), gf1 = rh_free_total(&M.gheap);
    printf("[RealEngine] mat param: 300 rebakes: parameters' room %u -> %u vec4, programs' %u -> %u words\n", pf0, pf1, gf0, gf1);
    if (pf1 != pf0 || gf1 != gf0) printf("[RealEngine] FAIL: mat param: the rebakes took room they didn't give back\n"), ok = false;
    mat_end_play();
    M.bc7 = bc7;
    printf("[RealEngine] mat param: %s\n", ok ? "ok" : "FAILED");
    return ok;
}
