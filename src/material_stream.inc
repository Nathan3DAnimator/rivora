// Texture streaming (sugestão #51, notas-dev/materiais.md "Streaming de texturas"). Included by material_gpu.inc.
// Each material's (and Material Instance's) baked textures live at the size the screen needs, under a memory budget:
//   - the renderer sweeps the static meshes a slice a frame (mat_stream_note: the size in pixels a material is seen at,
//     its tiling included; off-screen meshes count too, so turning the camera never waits for a texture) and each
//     pass's end sets the wants: the next power of two of that size, between ST_MIN and the material's resolution,
//     all halved together while they don't fit the budget;
//   - one transition a frame at most. Up: new textures baked in tiles of ST_TILE texels a frame into the frame's own
//     command buffer (no stall), their mips, then swapped into the slots once the frame that baked them is done. Down:
//     copied from the old textures' smaller mips (no bake), swapped the same way; only under memory pressure and after
//     ST_DOWN_DELAY s of lower need (a camera passing by doesn't make it flicker);
//   - the old textures are freed when no frame in flight can read them; a variant always has its textures (at least
//     ST_MIN), so nothing is ever drawn without one.
// Animated materials (their flipbook) and the actors' dynamic instances are not streamed. With the budget to spare
// (the usual project) nothing ever goes down: everything is baked at its full resolution as before.

#define ST_MIN 256u                // the smallest resident size
#define ST_START 512u              // a variant baked while the budget is short starts here and climbs as it comes near
#define ST_TILE (64u * 1024u)      // texels baked a frame while a variant climbs, at first (then by its measured cost)
#define ST_TILE_MS 0.4             // the GPU time a frame's tile aims at (timestamps around it)
#define ST_DOWN_DELAY 2.0          // s of lower need before a variant goes down
#define ST_GRAVE 64                // old textures waiting for the frames in flight
#define ST_HASH 4096u              // material hash -> asset (open addressing, > 2x MAT_MAX_ASSETS)

static Variant g_inst_var[MAT_MAX_ASSETS]; // a Material Instance's own variant (update_instance)
static Compiled* compiled_of(const Material* m);

typedef struct StJob {
    int asset;         // -1: none
    Variant* v;
    const Compiled* c;
    uint32_t tex[3], res, row;
    bool down;
    uint64_t swap_at;  // the frame number from which the frame that wrote them is done (0: still writing)
} StJob;

static struct {
    bool on;
    uint64_t budget;
    float acc[MAT_MAX_ASSETS], need[MAT_MAX_ASSETS]; // px: this pass's maximum, the last pass's (0: not seen)
    uint32_t want[MAT_MAX_ASSETS];
    double low_since[MAT_MAX_ASSETS];
    bool pin[MAT_MAX_ASSETS];                        // always at full size (a crowd's: not in the sweep)
    int16_t base[MAT_MAX_ASSETS];                    // the material whose program an asset bakes with (st_hash_build)
    uint32_t hkey[ST_HASH];
    uint16_t hval[ST_HASH];
    uint32_t hcount;                                 // the assets the table was built for
    bool hstale;
    StJob job;
    struct { uint32_t tex[3]; uint64_t at; } grave[ST_GRAVE];
    uint32_t ngrave;
    struct { GpuBuf b; uint64_t at; } bgrave[ST_GRAVE]; // (the BC7 blocks a frame's compression copied from: tex_to_bc7)
    uint32_t nbgrave;
    uint32_t bias;                                   // halvings so the wants fit
    uint32_t tile;                                   // texels a frame (ST_TILE_MS of GPU)
    VkQueryPool qp;                                  // two timestamps per frame in flight around the tile
    uint32_t q_texels[RE_FRAMES_IN_FLIGHT];          // the tile each slot measured (0: none)
    bool ud_turn;                                    // (Q18) the UDIM tiles' turn to pick first
} S;

static uint64_t g_tex_budget_set; // (material_set_texture_budget: a phone's share of its shared memory)
void material_set_texture_budget(uint64_t bytes) {
    g_tex_budget_set = bytes;
    if (bytes && S.tile) S.budget = bytes, M.stats.budget = bytes; // (already streaming: from now on)
}

static void stream_init(VkCtx* vk, bool update_after_bind) {
    memset(&S, 0, sizeof S);
    S.job.asset = -1;
    const char* e = getenv("REALENGINE_TEX_STREAMING");
    S.on = update_after_bind && !(e && e[0] == '0');
    // the budget: a share of the GPU's own memory (the rest: meshes, the GI, the frame's targets); an override for tests
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk->gpu, &mp);
    uint64_t vram = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[i].size > vram) vram = mp.memoryHeaps[i].size;
    S.budget = vram / 100 * 35;
    if (S.budget < (256ull << 20)) S.budget = 256ull << 20;
    if (g_tex_budget_set) S.budget = g_tex_budget_set;
    const char* b = getenv("REALENGINE_TEX_BUDGET_MB");
    if (b && atoi(b) > 0) S.budget = (uint64_t)atoi(b) << 20;
    M.stats.budget = S.budget;
    M.stats.streaming = S.on;
    S.tile = ST_TILE;
    M.stats.stream_tile = S.tile;
    if (S.on && vk->timestamp_ms > 0.0) {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * RE_FRAMES_IN_FLIGHT;
        if (vkCreateQueryPool(vk->device, &qi, NULL, &S.qp) != VK_SUCCESS) S.qp = VK_NULL_HANDLE;
    }
}

// A variant's memory at a size (its mips: 4/3)
static uint64_t st_bytes(uint32_t res, const Compiled* c) {
    uint64_t px = (uint64_t)res * res, per = (bc7_on() ? 2u : 8u) + ((c->flags & MATF_EMISSIVE) ? 8u : 0u); // (BC7: a byte a texel)
    return px * per / 3u * 4u;
}
// A climb's peak: baked in RGBA8 (and RGBA16F), then compressed, both resident for a few frames (with BC7 the climb to
// 4096 held 180 MB of RGBA8 over a 512 MB budget's planning of the final 45)
static uint64_t st_bytes_peak(uint32_t res, const Compiled* c) {
    uint64_t px = (uint64_t)res * res, raw = px * (8u + ((c->flags & MATF_EMISSIVE) ? 8u : 0u)) / 3u * 4u;
    return bc7_on() ? raw + st_bytes(res, c) : raw;
}

// The variant of an asset that streams (the material's own or the instance's own), NULL when it doesn't
static Variant* st_variant(uint32_t i, const Compiled** pc) {
    Material* m = M.mats[i];
    int b = S.base[i]; // (compiled_of without its search: st_refresh keeps it)
    const Compiled* c = b >= 0 && M.comp[b].valid ? &M.comp[b] : NULL;
    if (!c || c->animated || (c->flags & MATF_UDIM)) return NULL; // (Q18: a UDIM material's tiles stream on their own)
    Variant* v = m->instance ? &g_inst_var[i] : &M.comp[i].v;
    if (!v->own || !v->tex[0] || !M.tex[v->tex[0]].image) return NULL;
    *pc = c;
    return v;
}

static bool st_streamable(const Compiled* c, const Variant* v) {
    return S.on && !c->animated && (v == &c->v || (v >= g_inst_var && v < g_inst_var + MAT_MAX_ASSETS));
}

// The size a bake makes a variant at: its full resolution while that fits half the budget (the usual project: as
// before); else what it has (an edit keeps its streamed size) or ST_START, to climb when it's seen near
static uint32_t ud_bake_res(const Compiled* c, const Variant* v); // (Q18) material_udim.inc
static int ud_lone(const Compiled* c);                               // (one tile's .rudim sources)
static void ud_src_need(VkCommandBuffer cmd, const Compiled* c, int udim, uint32_t res);
static void ud_src_trim(VkCommandBuffer cmd, const Compiled* c, int udim);
static bool ud_src_prefetched(const Compiled* c, int udim, uint32_t res);
static uint64_t ud_src_bytes(const Compiled* c, int udim, uint32_t res);
static uint32_t stream_res(const Compiled* c, const Variant* v) {
    if (c->flags & MATF_UDIM) return ud_bake_res(c, v); // (Q18) a UDIM material's tiles
    if (!st_streamable(c, v)) return c->res;
    uint32_t cur = v->tex[0] && M.tex[v->tex[0]].image ? M.tex[v->tex[0]].w : 0;
    uint64_t others = M.stats.tex_bytes - (cur ? st_bytes(cur, c) : 0);
    if (others + st_bytes(c->res, c) <= S.budget / 2) return c->res;
    if (cur && cur <= c->res) return cur;
    uint32_t start = c->res < ST_START ? c->res : ST_START, lo = c->res < ST_MIN ? c->res : ST_MIN;
    return M.stats.tex_bytes + st_bytes(start, c) <= S.budget / 10 * 9 ? start : lo; // (no room: the least, to climb)
}

static void st_bury(const uint32_t tex[3]) {
    if (!tex[0] && !tex[1] && !tex[2]) return;
    uint64_t at = M.vk->frame_number + RE_FRAMES_IN_FLIGHT + 1;
    if (S.ngrave >= ST_GRAVE) { // (full: the GPU waited for once)
        vkDeviceWaitIdle(M.vk->device);
        for (uint32_t i = 0; i < S.ngrave; i++)
            for (int k = 0; k < 3; k++) tex_free(S.grave[i].tex[k]);
        S.ngrave = 0;
    }
    memcpy(S.grave[S.ngrave].tex, tex, sizeof S.grave[0].tex);
    S.grave[S.ngrave++].at = at;
}

static void st_bury1(uint32_t t) {
    uint32_t tex[3] = { t, 0, 0 };
    st_bury(tex);
}

// The old textures no frame in flight reads any more (all: the GPU is idle)
static void st_drain(bool all) {
    uint64_t fn = M.vk->frame_number;
    for (uint32_t i = 0; i < S.ngrave;) {
        if (!all && S.grave[i].at > fn) {
            i++;
            continue;
        }
        for (int k = 0; k < 3; k++) tex_free(S.grave[i].tex[k]);
        S.grave[i] = S.grave[--S.ngrave];
    }
    for (uint32_t i = 0; i < S.nbgrave;) {
        if (!all && S.bgrave[i].at > fn) {
            i++;
            continue;
        }
        gpu_buf_destroy(M.vk, &S.bgrave[i].b);
        S.bgrave[i] = S.bgrave[--S.nbgrave];
    }
}

// The transition of a variant dropped (it is baked again, freed, or its material recompiled)
static void stream_cancel(const Variant* v) {
    if (S.job.asset < 0 || (v && S.job.v != v)) return;
    st_bury(S.job.tex);
    S.job.asset = -1;
}

static void st_hash_build(void) {
    memset(S.hkey, 0, sizeof S.hkey);
    for (uint32_t i = 0; i < M.n; i++) {
        Material* m = M.mats[i];
        S.base[i] = (int16_t)(m->instance ? (m->parent && !m->parent->instance ? mat_index(m->parent) : -1) : (int)i);
        if (!m->hash) continue;
        // an instance without its own textures draws with its parent's: its need is the parent's
        uint32_t t = i;
        if (m->instance && !g_inst_var[i].own && m->parent) {
            int pi = mat_index(m->parent);
            if (pi >= 0) t = (uint32_t)pi;
        }
        uint32_t h = (m->hash * 2654435761u) >> 20;
        while (S.hkey[h] && S.hkey[h] != m->hash) h = (h + 1) & (ST_HASH - 1);
        S.hkey[h] = m->hash, S.hval[h] = (uint16_t)t;
    }
    S.hcount = M.n;
    S.hstale = false;
}

static void st_refresh(void) {
    if (S.hstale || S.hcount != M.n) st_hash_build();
}

bool mat_stream_on(void) { return M.ready && S.on; }

bool mat_stream_note(uint32_t hash, float px) {
    if (!hash) return false;
    st_refresh();
    uint32_t h = (hash * 2654435761u) >> 20;
    while (S.hkey[h]) {
        if (S.hkey[h] == hash) {
            uint32_t t = S.hval[h];
            if (px > S.acc[t]) S.acc[t] = px;
            int b = S.base[t];
            return b >= 0 && (M.comp[b].flags & MATF_UDIM); // (Q18: the caller notes its tiles too)
        }
        h = (h + 1) & (ST_HASH - 1);
    }
    return false;
}

void mat_stream_pin(uint32_t hash) {
    Material* m = hash ? mat_find_hash(hash) : NULL;
    if (m && m->instance && !g_inst_var[mat_index(m)].own && m->parent) m = m->parent;
    int i = m ? mat_index(m) : -1;
    if (i >= 0) S.pin[i] = true;
}

// A pass over the world ended: the wants from what it saw
static void ud_pass_end(double now);
void mat_stream_pass_end(void) {
    if (!S.on) return;
    st_refresh();
    double now = re_time_seconds();
    ud_pass_end(now); // (Q18) the UDIM tiles' wants
    static uint32_t full[MAT_MAX_ASSETS], floor_res[MAT_MAX_ASSETS];
    static const Compiled* cs[MAT_MAX_ASSETS];
    uint64_t total = 0, unseen = 0;
    uint32_t streamed = 0;
    for (uint32_t i = 0; i < M.n; i++) {
        S.need[i] = S.acc[i];
        S.acc[i] = 0.0f;
        const Compiled* c = NULL;
        Variant* v = st_variant(i, &c);
        cs[i] = v ? c : NULL;
        if (!v) {
            S.want[i] = 0;
            continue;
        }
        streamed++;
        uint32_t cur = M.tex[v->tex[0]].w, lo = c->res < ST_MIN ? c->res : ST_MIN, w = cur; // unseen: keeps its size
        if (S.pin[i]) w = c->res;
        else if (S.need[i] > 0.0f) {
            w = lo;
            while ((float)w < S.need[i] && w < c->res) w <<= 1;
            if (w > c->res) w = c->res;
        }
        full[i] = w, floor_res[i] = lo;
        total += st_bytes(w, c);
        if (S.need[i] <= 0.0f && !S.pin[i]) unseen += st_bytes(w, c) - st_bytes(lo, c);
    }
    // over the budget (90%: room for a climb in flight): the unseen to their least, then everything seen halved
    uint64_t room = S.budget / 10 * 9, others = 0;
    for (uint32_t i = 1; i < MAT_TEX_MAX; i++) others += M.tex[i].bytes; // (the sources and what isn't streamed count)
    for (uint32_t i = 0; i < M.n; i++) {
        if (!cs[i]) continue;
        const Variant* v = M.mats[i]->instance ? &g_inst_var[i] : &M.comp[i].v;
        for (int k = 0; k < 3; k++)
            if (v->tex[k]) others -= M.tex[v->tex[k]].bytes;
    }
    if (S.job.asset >= 0)
        for (int k = 0; k < 3; k++)
            if (S.job.tex[k]) others -= M.tex[S.job.tex[k]].bytes;
    room = room > others ? room - others : 0;
    bool drop_unseen = total > room && unseen > 0;
    if (drop_unseen) total -= unseen;
    S.bias = 0;
    while (total > room && S.bias < 6) {
        S.bias++;
        total = 0;
        for (uint32_t i = 0; i < M.n; i++) {
            if (!cs[i]) continue;
            bool seen = S.need[i] > 0.0f || S.pin[i];
            uint32_t w = seen ? full[i] >> S.bias : (drop_unseen ? floor_res[i] : full[i]);
            total += st_bytes(w < floor_res[i] ? floor_res[i] : w, cs[i]);
        }
    }
    for (uint32_t i = 0; i < M.n; i++) {
        if (!cs[i]) continue;
        bool seen = S.need[i] > 0.0f || S.pin[i];
        uint32_t w = seen ? full[i] >> S.bias : (drop_unseen ? floor_res[i] : full[i]);
        if (w < floor_res[i]) w = floor_res[i];
        const Variant* v = M.mats[i]->instance ? &g_inst_var[i] : &M.comp[i].v;
        uint32_t cur = M.tex[v->tex[0]].w;
        if (w < cur) {
            if (!S.low_since[i]) S.low_since[i] = now;
        } else S.low_since[i] = 0.0;
        S.want[i] = w;
    }
    M.stats.streamed = streamed;
}

static VkFormat st_format(int k) { return k == 2 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM; }

static bool st_start(VkCommandBuffer cmd, uint32_t i, Variant* v, const Compiled* c, uint32_t res, bool down) {
    StJob* j = &S.job;
    memset(j, 0, sizeof *j);
    for (int k = 0; k < 3; k++) {
        if (!v->tex[k]) continue;
        // (down: the old one's format, copied as it is; up: baked in RGBA8, then compressed at the end, st_work)
        j->tex[k] = tex_create_in(cmd, res, res, down ? M.tex[v->tex[k]].format : st_format(k));
        if (!j->tex[k]) { // (out of memory or of bindless slots: stays as it is; the ones made wait for this frame)
            st_bury(j->tex);
            if (i < MAT_MAX_ASSETS) S.want[i] = M.tex[v->tex[0]].w; // (a UDIM tile's job: its want stays, picked again)
            j->asset = -1;
            return false;
        }
    }
    j->asset = (int)i, j->v = v, j->c = c, j->res = res, j->down = down;
    return true;
}

// This frame's share of the transition: a tile of the bake (up) or the copies (down)
static void st_work(VkCommandBuffer cmd) {
    StJob* j = &S.job;
    const Compiled* c = j->c;
    if (j->down) {
        const MatTex* o = &M.tex[j->v->tex[0]];
        uint32_t skip = 0;
        while ((o->w >> skip) > j->res) skip++;
        for (int k = 0; k < 3; k++) {
            if (!j->tex[k]) continue;
            const MatTex* src = &M.tex[j->v->tex[k]];
            const MatTex* dst = &M.tex[j->tex[k]];
            VkImageCopy r[16];
            uint32_t n = 0;
            for (uint32_t l = 0; l < dst->mips && l + skip < src->mips && n < 16; l++, n++) {
                uint32_t s = dst->w >> l ? dst->w >> l : 1;
                r[n] = (VkImageCopy){ { VK_IMAGE_ASPECT_COLOR_BIT, l + skip, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 },
                                      { 0, 0, 0 }, { s, s, 1 } };
            }
            vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, n, r);
        }
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT);
        j->swap_at = M.vk->frame_number + RE_FRAMES_IN_FLIGHT;
        return;
    }
    uint32_t rows = S.tile / j->res;
    rows = rows < 8 ? 8 : rows & ~7u;
    uint32_t r1 = j->row + rows < j->res ? j->row + rows : j->res;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, M.bake);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, M.layout, 0, 1, &M.set, 0, NULL);
    BakePush p = { M.programs.addr, M.params.addr + (VkDeviceAddress)j->v->param_off * 16u, c->bake_off, c->bake_len, j->res,
                   j->res, j->tex[0], j->tex[1], j->tex[2], c->flags, { 0, 0, 0, 0 }, { 0, j->row } };
    p.udim = M.params.addr, p.uv_off[0] = j->v->uv_off[0], p.uv_off[1] = j->v->uv_off[1]; // (Q18: a UDIM tile's square)
    uint32_t qs = M.vk->frame * 2u;
    if (S.qp) { // (the tile's GPU time: the next tiles' size)
        vkCmdResetQueryPool(cmd, S.qp, qs, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, S.qp, qs);
    }
    vkCmdPushConstants(cmd, M.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(cmd, (j->res + 7) / 8, (r1 - j->row + 7) / 8, 1);
    if (S.qp) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, S.qp, qs + 1);
        S.q_texels[M.vk->frame] = j->res * (r1 - j->row);
    }
    M.stats.stream_texels += j->res * (r1 - j->row);
    j->row = r1;
    if (j->row < j->res) return;
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    for (int k = 0; k < 3; k++) // (the blits: inside a frame the Kaiser mips cost the texture_streaming test 200 -> 97 fps)
        if (j->tex[k]) tex_mips(cmd, j->tex[k]);
    for (int k = 0; k < 2 && bc7_on(); k++) { // the color and the normal compressed in this frame too (the RGBA8 ones buried)
        GpuBuf blk = { 0 };
        uint32_t n = j->tex[k] ? tex_to_bc7(cmd, j->tex[k], &blk) : 0u;
        if (!n) continue;
        st_bury1(j->tex[k]);
        j->tex[k] = n;
        if (S.nbgrave >= ST_GRAVE) vkDeviceWaitIdle(M.vk->device), st_drain(true);
        S.bgrave[S.nbgrave].b = blk, S.bgrave[S.nbgrave++].at = M.vk->frame_number + RE_FRAMES_IN_FLIGHT + 1;
    }
    j->swap_at = M.vk->frame_number + RE_FRAMES_IN_FLIGHT;
}

// A slot's textures replaced in place (the frames in flight read the old or the new ids, both whole)
#define UD_JOB 0x40000000 // (Q18) a UDIM tile's transition (material_udim.inc: UD_JOB + its pool index)
static bool ud_job_valid(const StJob* j);
static bool ud_pick(VkCommandBuffer cmd);
static void ud_after_swap(VkCommandBuffer cmd, int i, bool down);
static void st_slot_tex(uint32_t slot, const uint32_t tex[3]) {
    if (!slot) return;
    GpuMaterial* g = &((GpuMaterial*)M.materials.mapped)[slot];
    g->tex_base = tex[0], g->tex_nrm = tex[1], g->tex_emis = tex[2];
}

// A variant's textures into its slot, and a material's own into the slots that share them: its instances without their
// own, the actors' (the streaming's swaps, a bake's BC7 compression)
static void st_share(const Compiled* c, const Variant* v) {
    st_slot_tex(v->slot, v->tex);
    if (v != &c->v) return;
    for (uint32_t i = 0; i < M.n; i++)
        if (M.mats[i]->instance && !g_inst_var[i].own && compiled_of(M.mats[i]) == c) st_slot_tex(g_inst_var[i].slot, v->tex);
    for (uint32_t i = 0; i < M.ndyn; i++)
        if (!M.dyn[i].v.own && compiled_of(M.dyn[i].m) == c) st_slot_tex(M.dyn[i].v.slot, v->tex);
}

static void st_swap(void) {
    StJob* j = &S.job;
    Variant* v = j->v;
    uint32_t old[3];
    memcpy(old, v->tex, sizeof old);
    memcpy(v->tex, j->tex, sizeof v->tex);
    st_share(j->c, v);
    st_bury(old);
    if (j->down) M.stats.downs++;
    else M.stats.ups++;
    j->asset = -1;
}

static void st_pick(VkCommandBuffer cmd) {
    double now = re_time_seconds();
    int up = -1, down = -1;
    float up_score = 1.0f;
    uint64_t down_bytes = 0;
    for (uint32_t i = 0; i < M.n; i++) {
        if (!S.want[i]) continue;
        const Compiled* c;
        Variant* v = st_variant(i, &c);
        if (!v) continue;
        uint32_t cur = M.tex[v->tex[0]].w;
        if (S.want[i] > cur) { // the blurriest first (the nearest of them)
            float s = (float)S.want[i] / (float)cur + S.need[i] * 1e-6f;
            if (s > up_score) up_score = s, up = (int)i;
        } else if (S.want[i] < cur && S.low_since[i] > 0.0 && now - S.low_since[i] >= ST_DOWN_DELAY) {
            uint64_t b = st_bytes(cur, c);
            if (b > down_bytes) down_bytes = b, down = (int)i;
        }
    }
    M.stats.pending = (up >= 0) + (down >= 0);
    const Compiled* c;
    if (up >= 0) {
        Variant* v = st_variant((uint32_t)up, &c);
        // in steps of 4x (512 -> 2048 -> 4096): the detail shows sooner than with one long bake to the top
        uint32_t cur = M.tex[v->tex[0]].w, to = S.want[up] < cur * 4u ? S.want[up] : cur * 4u;
        int lone = ud_lone(c); // (a one-tile .rudim's sources climb with it: material_udim.inc)
        if (M.stats.tex_bytes + st_bytes_peak(to, c) + (lone ? ud_src_bytes(c, lone, to) : 0u) <= S.budget) {
            if (lone && !ud_src_prefetched(c, lone, to)) return; // (its mips on their way from the disk: this frame waits)
            if (lone) ud_src_need(cmd, c, lone, to);
            if (st_start(cmd, (uint32_t)up, v, c, to, false)) st_work(cmd);
            return;
        }
    }
    // down only when the memory is needed (with room to spare, what is resident stays: no bake to climb back)
    bool pressure = M.stats.tex_bytes > S.budget / 100 * 85 || up >= 0;
    if (down >= 0 && pressure) {
        Variant* v = st_variant((uint32_t)down, &c);
        if (st_start(cmd, (uint32_t)down, v, c, S.want[down], true)) st_work(cmd);
    }
}

// Once a frame (mat_update with the frame's command buffer)
static void stream_step(VkCommandBuffer cmd) {
    st_drain(false); // (the textures a bake replaced too, streaming or not)
    if (!S.on) return;
    double t0 = re_time_seconds();
    M.stats.stream_texels = 0;
    // the tile this frame slot baked last time (its frame is done): texels a frame from its cost
    uint32_t fs = M.vk->frame;
    if (S.qp && S.q_texels[fs]) {
        uint64_t t[2];
        if (vkGetQueryPoolResults(M.vk->device, S.qp, fs * 2u, 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0]) {
            double ms = (double)(t[1] - t[0]) * M.vk->timestamp_ms;
            M.stats.stream_gpu_ms = ms;
            if (ms > M.stats.stream_gpu_max) M.stats.stream_gpu_max = ms;
            double want = (double)S.q_texels[fs] * ST_TILE_MS / (ms > 0.001 ? ms : 0.001);
            want = want < 16384.0 ? 16384.0 : want > 4194304.0 ? 4194304.0 : want;
            S.tile = (uint32_t)(S.tile * 0.5 + want * 0.5);
            M.stats.stream_tile = S.tile;
        }
        S.q_texels[fs] = 0;
    } else M.stats.stream_gpu_ms = 0.0;
    st_refresh();
    StJob* j = &S.job;
    if (j->asset >= UD_JOB) { // (Q18) a UDIM tile's: its tile or owner gone
        if (!ud_job_valid(j)) stream_cancel(NULL);
    } else if (j->asset >= 0) { // (its variant changed under it: dropped)
        const Compiled* c;
        if ((uint32_t)j->asset >= M.n || st_variant((uint32_t)j->asset, &c) != j->v || c != j->c) stream_cancel(NULL);
    }
    if (j->asset < 0) { // (Q18) the UDIM tiles and the materials take turns picking first when both have work
        S.ud_turn = !S.ud_turn;
        if (S.ud_turn) {
            if (!ud_pick(cmd)) st_pick(cmd);
        } else {
            st_pick(cmd);
            if (j->asset < 0) ud_pick(cmd);
        }
    } else if (!j->swap_at) st_work(cmd);
    else if (M.vk->frame_number >= j->swap_at) {
        int ui = j->asset - UD_JOB, lone = ui < 0 && !j->down ? ud_lone(j->c) : 0;
        const Compiled* jc = j->c;
        bool down = j->down;
        st_swap();
        if (ui >= 0) ud_after_swap(cmd, ui, down);
        else if (lone) ud_src_trim(cmd, jc, lone); // (a one-tile .rudim's sources back to their least after the climb)
    }
    M.stats.stream_cpu_ms = (re_time_seconds() - t0) * 1000.0;
    if (M.stats.stream_cpu_ms > M.stats.stream_cpu_max) M.stats.stream_cpu_max = M.stats.stream_cpu_ms;
}

// The Debug Viewer's list: an asset's streamed sizes (false: not streamed)
bool mat_stream_info(uint32_t i, const char** name, uint32_t* cur, uint32_t* want, uint32_t* full, float* need_px, uint64_t* bytes) {
    if (i >= M.n) return false;
    st_refresh();
    const Compiled* c;
    Variant* v = st_variant(i, &c);
    if (!v) return false;
    *name = M.mats[i]->name;
    *cur = M.tex[v->tex[0]].w;
    *want = S.want[i] ? S.want[i] : *cur;
    *full = c->res;
    *need_px = S.need[i];
    *bytes = 0;
    for (int k = 0; k < 3; k++)
        if (v->tex[k]) *bytes += M.tex[v->tex[k]].bytes;
    return true;
}
