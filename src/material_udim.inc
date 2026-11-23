// (Q18) UDIM materials (notas-dev/udim.md). Included by material_gpu.inc after material_stream.inc.
//
// An Image Texture node may read a UDIM texture (a .rudim asset, or a "<base>.<UDIM>.png" pattern of the tiles' files):
//   - the sources: each tile its own texture in the bindless set (clamped: no bleed from its far edge), and a table per
//     source in the parameters buffer the bake reads (mat_vm.glsl mat_sample_udim: the tile from the uv, the uv inside it);
//     an asset's tiles are uploaded at the size a bake needs, read from the mapped file, and go back to UD_MIN after;
//   - the bake: a material whose graph reads one has a tile per UDIM tile its sources have. Each of its variants (the
//     material's own, an instance's, an actor's) "owns" its tiles: the first is the variant itself (its slot's textures:
//     what every other path reads), the others tile variants baked over their unit square (BakePush.uv_off), all of them
//     in a table in the parameters (mesh.frag: the tile under the uv, read with the whole uv's gradients: no seam);
//   - the streaming: each tile has its own size. The renderer's sweep notes each tile's size on screen from the mesh's
//     triangles in that tile (render.c udim_mesh_tiles: their bounds, in view or not). Seen tiles climb (baked in slices
//     like the materials' climbs, the same job); tiles out of view for UD_UNSEEN_DELAY go down to UD_MIN.

#define UD_MAX_SRC 64
#define UD_POOL 1024
#define UD_MIN 64u            // an unseen tile's size (it keeps textures: nothing is ever drawn without them)
#define UD_UNSEEN_DELAY 2.0   // s out of view before a tile goes down to UD_MIN
#define UD_TABLE_ROWS (1u + UDIM_MAX_TILES) // an owner's: a header, then a row per tile (base, normal, emission)
#define UD_SRC_ROWS (1u + UDIM_MAX_TILES / 4u) // a source's: a header (its flags), then the tiles' textures 4 a row
#define UD_JOB 0x40000000     // a tile's transition: StJob.asset = UD_JOB + its index in the pool
#define UD_TEX_BIT 0x80000000u // an Image Texture's texture word: a UDIM source's table row (mat_vm.glsl)

typedef struct UdSrc {
    char full[MAT_PATH * 2];
    bool asset;              // a .rudim (else the pattern's files, read whole like any image)
    UdimFile* file;
    uint32_t flags, format;
    int n;
    int udim[UDIM_MAX_TILES];
    uint32_t tex[UDIM_MAX_TILES]; // the tile's texture (0: none)
    uint32_t res[UDIM_MAX_TILES]; // its size now (an asset's: the mip uploaded)
    uint64_t prefetch[UDIM_MAX_TILES]; // the frame a tile's mips were asked from the disk (an asset's)
    uint32_t tab;            // its table's first row in the parameters
} UdSrc;
static UdSrc* g_uds[UD_MAX_SRC];
static uint32_t g_nuds;

typedef struct UdTile {
    bool used, master, pinned;
    Variant* owner;
    int asset;               // the compiled material (M.comp's index)
    int udim;
    Variant v;               // a tile variant's own (the master's is the owner itself)
    float acc, need;         // px: this pass's most, the last pass's
    double seen_at, low_since;
    uint32_t want;
} UdTile;
static UdTile g_udt[UD_POOL];

typedef struct UdMat { int n; int udim[UDIM_MAX_TILES]; } UdMat; // a compiled material's tiles (the master first)
static UdMat g_udm[MAT_MAX_ASSETS];

static bool bake_variant(const Compiled* c, Variant* v);
static void write_slot(const Variant* v, const Compiled* c);
static int ud_asset(const Compiled* c) { return (int)(c - M.comp); }
static bool ud_on(const Compiled* c) { return c && (c->flags & MATF_UDIM) && g_udm[ud_asset(c)].n > 1; }
// One tile (a .rudim of 1001 alone, as a Substance/Tlender export of one UDIM tile): no MATF_UDIM, baked as any material,
// but its .rudim sources still sit at UD_MIN until a bake asks for its size (2026-11-23: the Capsula's 4K came out 64²).
// Its tile, 0: none
static int ud_lone(const Compiled* c) {
    const UdMat* um = c ? &g_udm[ud_asset(c)] : NULL;
    return um && !(c->flags & MATF_UDIM) && um->n == 1 ? um->udim[0] : 0;
}
static Variant* ud_var(UdTile* t) { return t->master ? t->owner : &t->v; }
static uint32_t ud_cur(UdTile* t) {
    Variant* v = ud_var(t);
    return v->tex[0] && M.tex[v->tex[0]].image ? M.tex[v->tex[0]].w : 0u;
}

// --- The sources ---

static void tex_set_clamp(uint32_t idx) {
    if (!idx || !M.tex[idx].image || M.tex[idx].clamp) return;
    M.tex[idx].clamp = true;
    write_descriptors(idx);
}

static void ud_src_table(UdSrc* s) {
    uint32_t(*row)[4] = (uint32_t(*)[4])param_vals();
    memset(row[s->tab], 0, 16u * UD_SRC_ROWS);
    row[s->tab][0] = (s->flags & UDIM_F_NORMAL) ? 1u : 0u; // (BC5: the bake rebuilds B)
    row[s->tab][1] = (uint32_t)s->n;
    for (int k = 0; k < s->n; k++) {
        int x, y;
        if (!udim_tile_xy(s->udim[k], &x, &y)) continue;
        uint32_t i = (uint32_t)(x + y * UDIM_COLS);
        row[s->tab + 1u + i / 4u][i % 4u] = s->tex[k];
    }
}

static VkFormat ud_vk_format(uint32_t f) {
    return f == UDIM_FMT_BC5 ? VK_FORMAT_BC5_UNORM_BLOCK : f == UDIM_FMT_BC7 ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_R8G8B8A8_UNORM;
}

// An asset's tile uploaded at a size (the smallest mip at least res; cmd NULL: now, waiting for it); its old texture buried
static bool ud_src_upload(VkCommandBuffer cmd, UdSrc* s, int k, uint32_t res) {
    int ti = udim_find(s->file, s->udim[k]);
    if (ti < 0) return false;
    const RUdimTile* t = &s->file->tiles[ti];
    uint32_t L = 0;
    while (L + 1 < t->mips && (t->width >> (L + 1)) >= res && (t->height >> (L + 1)) >= (res < 4 ? res : 4)) L++;
    uint32_t w = t->width >> L ? t->width >> L : 1, h = t->height >> L ? t->height >> L : 1, mips = t->mips - L;
    bool bc = s->format != UDIM_FMT_RGBA8 && M.vk->bc_textures;
    VkDeviceSize total = 0;
    for (uint32_t m = 0; m < mips; m++) {
        uint32_t mw = w >> m ? w >> m : 1, mh = h >> m ? h >> m : 1;
        total += bc ? udim_mip_bytes(s->format, mw, mh) : (VkDeviceSize)mw * mh * 4;
    }
    GpuBuf st = { 0 };
    if (!gpu_buf_create(M.vk, &st, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    VkBufferImageCopy r[16];
    VkDeviceSize off = 0;
    uint32_t n = 0;
    for (uint32_t m = 0; m < mips && n < 16; m++, n++) {
        uint32_t mw, mh;
        size_t mb;
        const uint8_t* src = udim_mip(s->file, ti, L + m, &mw, &mh, &mb);
        if (!src) break;
        if (bc) memcpy((uint8_t*)st.mapped + off, src, mb);
        else if (s->format == UDIM_FMT_RGBA8) memcpy((uint8_t*)st.mapped + off, src, mb);
        else { // (a GPU without BC: decoded here)
            uint8_t blk[64];
            uint8_t* o = (uint8_t*)st.mapped + off;
            uint32_t bw = (mw + 3) / 4;
            for (uint32_t by = 0; by < (mh + 3) / 4; by++)
                for (uint32_t bx = 0; bx < bw; bx++) {
                    const uint8_t* b = src + ((size_t)by * bw + bx) * 16;
                    if (s->format == UDIM_FMT_BC5) udim_bc5_decode_block(b, blk);
                    else udim_bc7_decode_block(b, blk);
                    for (int i = 0; i < 16; i++) {
                        uint32_t x = bx * 4 + (uint32_t)(i & 3), y = by * 4 + (uint32_t)(i >> 2);
                        if (x < mw && y < mh) memcpy(o + ((size_t)y * mw + x) * 4, blk + i * 4, 4);
                    }
                }
            mb = (size_t)mw * mh * 4;
        }
        r[n] = (VkBufferImageCopy){ off, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 }, { 0, 0, 0 }, { mw, mh, 1 } };
        off += mb;
    }
    bool once = !cmd;
    if (once) cmd = gpu_begin_once(M.vk);
    uint32_t idx = tex_create_in(cmd, w, h, bc ? ud_vk_format(s->format) : VK_FORMAT_R8G8B8A8_UNORM);
    if (idx) {
        vkCmdCopyBufferToImage(cmd, st.buffer, M.tex[idx].image, VK_IMAGE_LAYOUT_GENERAL, n, r);
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT);
        snprintf(M.tex[idx].source, sizeof M.tex[idx].source, "udim:%s", s->full);
        tex_set_clamp(idx);
    }
    if (once) {
        gpu_end_once(M.vk, cmd);
        gpu_buf_destroy(M.vk, &st);
    } else { // (read by this frame's copy: freed after it)
        if (S.nbgrave >= ST_GRAVE) vkDeviceWaitIdle(M.vk->device), st_drain(true);
        S.bgrave[S.nbgrave].b = st, S.bgrave[S.nbgrave++].at = M.vk->frame_number + RE_FRAMES_IN_FLIGHT + 1;
    }
    if (!idx) return false;
    if (s->tex[k]) st_bury1(s->tex[k]);
    s->tex[k] = idx, s->res[k] = w;
    M.stats.udim_src_uploads++;
    return true;
}

// A UDIM image's table row (its tiles loaded: an asset's at UD_MIN, the pattern's files whole); UD_TEX_BIT | row, 0: none
static uint32_t ud_source(const char* full) {
    for (uint32_t i = 0; i < g_nuds; i++)
        if (g_uds[i] && !_stricmp(g_uds[i]->full, full)) return UD_TEX_BIT | g_uds[i]->tab;
    if (g_nuds >= UD_MAX_SRC) return 0;
    UdSrc* s = calloc(1, sizeof *s);
    if (!s) return 0;
    snprintf(s->full, sizeof s->full, "%s", full);
    size_t fl = strlen(full);
    s->asset = fl > 6 && !_stricmp(full + fl - 6, ".rudim");
    if (s->asset) {
        s->file = calloc(1, sizeof *s->file);
        char err[120];
        if (!s->file || !udim_open(full, s->file, err, sizeof err)) {
            printf("[RealEngine] UDIM: %s: %s\n", full, err);
            free(s->file), free(s);
            return 0;
        }
        s->flags = s->file->flags, s->format = s->file->format, s->n = (int)s->file->count;
        for (int k = 0; k < s->n; k++) s->udim[k] = (int)s->file->tiles[k].udim;
    } else {
        s->n = udim_find_tiles(full, s->udim, UDIM_MAX_TILES);
        s->flags = udim_name_is_normal(full) ? 0u : UDIM_F_SRGB; // (read as RGBA: the node's Non-Color does the rest)
        if (!s->n) {
            printf("[RealEngine] UDIM: %s: no tile found\n", full);
            free(s);
            return 0;
        }
    }
    s->tab = params_alloc(UD_SRC_ROWS);
    for (int k = 0; k < s->n; k++) {
        if (s->asset) ud_src_upload(VK_NULL_HANDLE, s, k, UD_MIN);
        else {
            char tp[MAT_PATH * 2];
            udim_tile_path(full, s->udim[k], tp, sizeof tp);
            s->tex[k] = mat_source_texture(tp);
            tex_set_clamp(s->tex[k]);
            s->res[k] = s->tex[k] ? M.tex[s->tex[k]].w : 0u;
        }
    }
    ud_src_table(s);
    g_uds[g_nuds++] = s;
    return UD_TEX_BIT | s->tab;
}

// The sources a material reads (its Image Texture nodes with a UDIM image); their count
static int ud_sources_of(const Material* m, UdSrc** out, int cap) {
    const Material* base = m && m->instance ? m->parent : m;
    int n = 0;
    if (!base || !base->graph) return 0;
    for (int i = 0; i < base->graph->node_count && n < cap; i++) {
        const MatNode* nd = &base->graph->nodes[i];
        if (nd->type != NODE_TEX_IMAGE || !udim_is_path(nd->image)) continue;
        char full[MAT_PATH * 2];
        source_full(nd->image, full, sizeof full);
        for (uint32_t k = 0; k < g_nuds; k++)
            if (g_uds[k] && !_stricmp(g_uds[k]->full, full)) {
                bool dup = false;
                for (int j = 0; j < n; j++) dup |= out[j] == g_uds[k];
                if (!dup) out[n++] = g_uds[k];
            }
    }
    return n;
}

// The asset sources' tile at least res (for a bake of that tile; cmd NULL: now). The pattern's are whole already
static void ud_src_need(VkCommandBuffer cmd, const Compiled* c, int udim, uint32_t res) {
    UdSrc* src[8];
    int n = ud_sources_of(M.mats[ud_asset(c)], src, 8);
    for (int i = 0; i < n; i++) {
        UdSrc* s = src[i];
        if (!s->asset) continue;
        for (int k = 0; k < s->n; k++) {
            if (s->udim[k] != udim) continue;
            int ti = udim_find(s->file, udim);
            uint32_t full = ti >= 0 ? s->file->tiles[ti].width : 0u, want = res < full ? res : full;
            if (s->res[k] < want && ud_src_upload(cmd, s, k, want)) ud_src_table(s);
        }
    }
}
// ... and back to the least once that bake is done (only the bakes read the sources: the tiles are what is drawn)
static void ud_src_trim(VkCommandBuffer cmd, const Compiled* c, int udim) {
    UdSrc* src[8];
    int n = ud_sources_of(M.mats[ud_asset(c)], src, 8);
    for (int i = 0; i < n; i++) {
        UdSrc* s = src[i];
        if (!s->asset || !S.on) continue;
        for (int k = 0; k < s->n; k++)
            if (s->udim[k] == udim && s->res[k] > UD_MIN && ud_src_upload(cmd, s, k, UD_MIN)) ud_src_table(s);
    }
}
// The least size the material's .rudim sources have of a tile now (0: none)
static uint32_t ud_src_res(const Compiled* c, int udim) {
    UdSrc* src[8];
    int n = ud_sources_of(M.mats[ud_asset(c)], src, 8);
    uint32_t least = 0;
    for (int i = 0; i < n; i++)
        for (int k = 0; src[i]->asset && k < src[i]->n; k++)
            if (src[i]->udim[k] == udim && (!least || src[i]->res[k] < least)) least = src[i]->res[k];
    return least;
}
// The disk read of a climb's mips started ahead (the mapped file's pages), so the upload frames later doesn't wait for it
static bool ud_src_prefetched(const Compiled* c, int udim, uint32_t res) {
    UdSrc* src[8];
    int n = ud_sources_of(M.mats[ud_asset(c)], src, 8);
    bool ready = true;
    for (int i = 0; i < n; i++) {
        UdSrc* s = src[i];
        if (!s->asset) continue;
        for (int k = 0; k < s->n; k++) {
            if (s->udim[k] != udim) continue;
            int ti = udim_find(s->file, udim);
            if (ti < 0 || s->res[k] >= res) continue;
            if (!s->prefetch[k] || M.vk->frame_number - s->prefetch[k] > 120) {
                uint32_t L = 0;
                const RUdimTile* t = &s->file->tiles[ti];
                while (L + 1 < t->mips && (t->width >> (L + 1)) >= res) L++;
                udim_prefetch(s->file, ti, L);
                s->prefetch[k] = M.vk->frame_number;
            }
            if (M.vk->frame_number - s->prefetch[k] < 2) ready = false;
        }
    }
    return ready;
}

// --- A material's tiles ---

// After its compile: the tiles its UDIM images have (the union; 1001 first when there), MATF_UDIM when more than one
static void ud_compile(Material* m, Compiled* c) {
    UdMat* um = &g_udm[ud_asset(c)];
    um->n = 0;
    c->flags &= ~MATF_UDIM;
    bool timed = false;
    for (int i = 0; m->graph && i < m->graph->node_count; i++) timed |= m->graph->nodes[i].type == NODE_RE_TIME;
    UdSrc* src[8];
    int n = timed || getenv("REALENGINE_NO_UDIM") ? 0 : ud_sources_of(m, src, 8); // (an animated material's flipbook stays one square: notas-dev/udim.md; the env: the measure's "before", tile 1001 alone)
    bool have[UDIM_MAX_TILES] = { false };
    for (int i = 0; i < n; i++)
        for (int k = 0; k < src[i]->n; k++) {
            int x, y;
            if (udim_tile_xy(src[i]->udim[k], &x, &y)) have[x + y * UDIM_COLS] = true;
        }
    for (int k = 0; k < UDIM_MAX_TILES; k++)
        if (have[k]) um->udim[um->n++] = UDIM_FIRST + k;
    if (um->n > 1) c->flags |= MATF_UDIM;
}

// The master tile's origin of a variant (the first tile: 1001 when the images have it)
static void ud_master_origin(const Compiled* c, Variant* v) {
    v->uv_off[0] = v->uv_off[1] = 0.0f;
    if (ud_on(c)) udim_origin(g_udm[ud_asset(c)].udim[0], &v->uv_off[0], &v->uv_off[1]);
}

static void ud_table_write(Variant* owner) {
    if (!owner->ud_table) return;
    uint32_t(*row)[4] = (uint32_t(*)[4])param_vals();
    memset(row[owner->ud_table], 0, 16u * UD_TABLE_ROWS);
    row[owner->ud_table][0] = owner->ud_n;
    for (uint32_t i = 0; i < owner->ud_n; i++) {
        UdTile* t = &g_udt[owner->ud_first + i];
        int x, y;
        if (!t->used || !udim_tile_xy(t->udim, &x, &y)) continue;
        Variant* v = ud_var(t);
        uint32_t* r = row[owner->ud_table + 1u + (uint32_t)(x + y * UDIM_COLS)];
        r[0] = v->tex[0], r[1] = v->tex[1], r[2] = v->tex[2], r[3] = 0;
    }
}

static void ud_owner_free(Variant* owner) {
    for (uint32_t i = 0; i < owner->ud_n; i++) {
        UdTile* t = &g_udt[owner->ud_first + i];
        if (!t->used) continue;
        if (!t->master) variant_free(&t->v);
        memset(t, 0, sizeof *t);
    }
    owner->ud_n = 0, owner->ud_first = 0;
}

// A tile's size for a bake now: the full resolution while all the material's tiles fit half the budget at it (the
// usual project); else what it has (an edit keeps it) or UD_MIN: only the tiles the camera sees climb, from the sweep
static uint32_t ud_bake_res(const Compiled* c, const Variant* v) {
    if (!S.on) return c->res;
    uint32_t cur = v->tex[0] && M.tex[v->tex[0]].image ? M.tex[v->tex[0]].w : 0;
    uint64_t all = st_bytes(c->res, c) * (uint64_t)g_udm[ud_asset(c)].n;
    if (M.stats.tex_bytes + all <= S.budget / 2) return c->res;
    if (cur && cur <= c->res) return cur;
    return c->res < UD_MIN ? c->res : UD_MIN;
}

// The sources' memory a climb of a tile to res uploads first (the budget's check counts it with the bake's peak)
static uint64_t ud_src_bytes(const Compiled* c, int udim, uint32_t res) {
    UdSrc* src[8];
    int n = ud_sources_of(M.mats[ud_asset(c)], src, 8);
    uint64_t b = 0;
    for (int i = 0; i < n; i++) {
        if (!src[i]->asset) continue;
        int ti = udim_find(src[i]->file, udim);
        if (ti < 0) continue;
        uint64_t s = res < src[i]->file->tiles[ti].width ? res : src[i]->file->tiles[ti].width;
        b += s * s * (src[i]->format == UDIM_FMT_RGBA8 || !M.vk->bc_textures ? 4u : 1u) / 3u * 4u;
    }
    return b;
}

static void ud_variant_clamp(Variant* v) {
    for (int k = 0; k < 3; k++) tex_set_clamp(v->tex[k]);
}

// An owner baked (bake_variant, its master tile done): its other tiles baked with its parameters, its table, the GI's
// mean over all of them
static void ud_owner_sync(const Compiled* c, Variant* owner) {
    if (!ud_on(c)) {
        if (owner->ud_n) ud_owner_free(owner);
        return;
    }
    UdMat* um = &g_udm[ud_asset(c)];
    bool same = owner->ud_n == (uint32_t)um->n;
    for (int i = 0; same && i < um->n; i++) same = g_udt[owner->ud_first + i].used && g_udt[owner->ud_first + i].udim == um->udim[i];
    if (!same) {
        ud_owner_free(owner);
        int first = -1;
        for (int i = 0; i + um->n <= UD_POOL && first < 0; i++) {
            bool free_run = true;
            for (int k = 0; k < um->n && free_run; k++) free_run = !g_udt[i + k].used;
            if (free_run) first = i;
        }
        if (first < 0) {
            printf("[RealEngine] UDIM: the tile pool is full (%d tiles); %s keeps one square\n", UD_POOL, M.mats[ud_asset(c)]->name);
            return;
        }
        owner->ud_first = (uint16_t)first, owner->ud_n = (uint16_t)um->n;
        bool pinned = (const uint8_t*)owner >= (const uint8_t*)M.dyn && (const uint8_t*)owner < (const uint8_t*)(M.dyn + MAT_MAX_DYNAMIC);
        for (int i = 0; i < um->n; i++) {
            UdTile* t = &g_udt[first + i];
            memset(t, 0, sizeof *t);
            t->used = true, t->master = i == 0, t->owner = owner, t->asset = ud_asset(c), t->udim = um->udim[i];
            t->pinned = pinned; // (an actor's dynamic instance: at full size, like the materials' streaming leaves them)
            t->seen_at = re_time_seconds();
            if (!t->master) {
                t->v.ud_tile = true;
                udim_origin(t->udim, &t->v.uv_off[0], &t->v.uv_off[1]);
            }
        }
    }
    float avg[3] = { 0, 0, 0 };
    for (int i = 0; i < um->n; i++) {
        UdTile* t = &g_udt[owner->ud_first + i];
        if (!t->master) {
            t->v.param_off = owner->param_off;
            bake_variant(c, &t->v);
        }
        Variant* v = ud_var(t);
        ud_variant_clamp(v);
        for (int k = 0; k < 3; k++) avg[k] += v->avg[k] / (float)um->n;
    }
    memcpy(owner->avg, avg, sizeof avg);
    if (!owner->ud_table) owner->ud_table = params_alloc(UD_TABLE_ROWS);
    ud_table_write(owner);
}

// --- The streaming of the tiles ---

// The renderer's note for one tile of a mesh's UDIM material (mat_stream_note said it has tiles)
void mat_stream_note_tile(uint32_t hash, int udim, float px) {
    if (!S.on || !hash) return;
    st_refresh();
    uint32_t h = (hash * 2654435761u) >> 20;
    while (S.hkey[h] && S.hkey[h] != hash) h = (h + 1) & (ST_HASH - 1);
    if (!S.hkey[h]) return;
    uint32_t t = S.hval[h];
    Variant* owner = M.mats[t]->instance ? &g_inst_var[t] : &M.comp[t].v;
    for (uint32_t i = 0; i < owner->ud_n; i++) {
        UdTile* u = &g_udt[owner->ud_first + i];
        if (u->used && u->udim == udim && px > u->acc) u->acc = px;
    }
}

static void ud_pass_end(double now) {
    uint64_t total = 0, cur_bytes = 0;
    uint32_t tiles = 0, resident = 0;
    static uint32_t full[UD_POOL];
    for (int i = 0; i < UD_POOL; i++) {
        UdTile* t = &g_udt[i];
        if (!t->used) continue;
        const Compiled* c = &M.comp[t->asset];
        t->need = t->acc, t->acc = 0.0f;
        if (t->need > 0.0f) t->seen_at = now;
        uint32_t cur = ud_cur(t), lo = c->res < UD_MIN ? c->res : UD_MIN, w = cur ? cur : lo;
        if (t->pinned) w = c->res;
        else if (t->need > 0.0f) {
            w = lo;
            while ((float)w < t->need && w < c->res) w <<= 1;
            if (w > c->res) w = c->res;
        } else if (now - t->seen_at >= UD_UNSEEN_DELAY) w = lo; // (only the tiles seen stay resident)
        full[i] = w;
        total += st_bytes(w, c);
        if (cur) cur_bytes += st_bytes(cur, c);
        tiles++, resident += cur > lo;
    }
    // over the budget (90%, beside everything that isn't a tile): the seen ones halved together
    uint64_t room = S.budget / 10 * 9, others = M.stats.tex_bytes > cur_bytes ? M.stats.tex_bytes - cur_bytes : 0;
    room = room > others ? room - others : 0;
    uint32_t bias = 0;
    while (total > room && bias < 6) {
        bias++;
        total = 0;
        for (int i = 0; i < UD_POOL; i++) {
            if (!g_udt[i].used) continue;
            const Compiled* c = &M.comp[g_udt[i].asset];
            uint32_t lo = c->res < UD_MIN ? c->res : UD_MIN, w = full[i] >> bias;
            total += st_bytes(w < lo ? lo : w, c);
        }
    }
    for (int i = 0; i < UD_POOL; i++) {
        UdTile* t = &g_udt[i];
        if (!t->used) continue;
        const Compiled* c = &M.comp[t->asset];
        uint32_t lo = c->res < UD_MIN ? c->res : UD_MIN, w = full[i] >> bias;
        if (w < lo) w = lo;
        uint32_t cur = ud_cur(t);
        if (w < cur) {
            if (!t->low_since) t->low_since = now;
        } else t->low_since = 0.0;
        t->want = w;
    }
    M.stats.udim_tiles = tiles, M.stats.udim_resident = resident, M.stats.udim_bytes = cur_bytes, M.stats.udim_bias = bias;
}

// A tile's transition when the materials' streaming has none this frame; true: one started
static bool ud_pick(VkCommandBuffer cmd) {
    double now = re_time_seconds();
    int up = -1, down = -1;
    float up_score = 1.0f;
    uint64_t down_bytes = 0;
    bool pressure = M.stats.tex_bytes > S.budget / 100 * 85;
    for (int i = 0; i < UD_POOL; i++) {
        UdTile* t = &g_udt[i];
        if (!t->used || !t->want) continue;
        uint32_t cur = ud_cur(t);
        if (!cur) continue;
        if (t->want > cur) {
            float s = (float)t->want / (float)cur + t->need * 1e-6f;
            if (s > up_score) up_score = s, up = i;
        } else if (t->want < cur && t->low_since > 0.0 && now - t->low_since >= (t->need > 0.0f ? ST_DOWN_DELAY : 0.0)) {
            bool unseen = t->need <= 0.0f; // (out of view: down without waiting for pressure; a smaller need only under it)
            uint64_t b = st_bytes(cur, &M.comp[t->asset]);
            if ((unseen || pressure) && b > down_bytes) down_bytes = b, down = i;
        }
    }
    if (up >= 0) {
        UdTile* t = &g_udt[up];
        const Compiled* c = &M.comp[t->asset];
        Variant* v = ud_var(t);
        uint32_t cur = ud_cur(t), to = t->want < cur * 4u ? t->want : cur * 4u;
        if (M.stats.tex_bytes + st_bytes_peak(to, c) + ud_src_bytes(c, t->udim, to) <= S.budget) {
            if (!ud_src_prefetched(c, t->udim, to)) return true; // (its mips on their way from the disk: this frame waits)
            ud_src_need(cmd, c, t->udim, to);
            if (st_start(cmd, (uint32_t)(UD_JOB + up), v, c, to, false)) {
                st_work(cmd);
                return true;
            }
            return false;
        }
    }
    if (down >= 0) {
        UdTile* t = &g_udt[down];
        if (st_start(cmd, (uint32_t)(UD_JOB + down), ud_var(t), &M.comp[t->asset], t->want, true)) {
            st_work(cmd);
            return true;
        }
    }
    return false;
}

static bool ud_job_valid(const StJob* j) {
    int i = j->asset - UD_JOB;
    return i >= 0 && i < UD_POOL && g_udt[i].used && ud_var(&g_udt[i]) == j->v && &M.comp[g_udt[i].asset] == j->c;
}

// A tile's transition swapped in: its textures clamped, its owner's table, its source back to the least
static void ud_after_swap(VkCommandBuffer cmd, int i, bool down) {
    UdTile* t = &g_udt[i];
    ud_variant_clamp(ud_var(t));
    ud_table_write(t->owner);
    if (down) M.stats.udim_downs++;
    else M.stats.udim_ups++, ud_src_trim(cmd, &M.comp[t->asset], t->udim);
}

// The Debug Viewer's list: a tile's sizes (false: no tile i)
bool mat_udim_info(uint32_t i, const char** name, int* udim, uint32_t* cur, uint32_t* want, uint32_t* full, float* need_px) {
    if (i >= UD_POOL || !g_udt[i].used) return false;
    UdTile* t = &g_udt[i];
    *name = M.mats[t->asset]->name;
    *udim = t->udim;
    *cur = ud_cur(t);
    *want = t->want ? t->want : *cur;
    *full = M.comp[t->asset].res;
    *need_px = t->need;
    return true;
}
