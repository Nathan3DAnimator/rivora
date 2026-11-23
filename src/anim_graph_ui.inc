// --- The Anim Graph tab (included by anim_editor.c): Unreal's AnimGraph. The pose graph on a canvas (nodes with pose
// pins, links dragged from an output to an input, the right click's menu by category, Delete, double click into a State
// Machine), each State Machine's states and transitions (Any State, rules, curve, priority), the Details on the right ---

static const char* const g_state_kinds[ANIM_STATE_KIND_COUNT] = { "Sequence Player", "Blend Space 1D", "Blend Poses by Bool", "Blend Space 2D",
                                                                  "Motion Matching" };
static const char* const g_ops[ANIM_OP_COUNT] = { "is true", "is false", ">", "<", "Time Remaining (ratio) <= blend", "int ==",
                                                  "Time in State >" };
static const char* const g_curves[ANIM_CURVE_COUNT] = { "Linear", "Cubic", "Ease Out" };

static struct {
    int machine;            // -1: the AnimGraph; else the State Machine shown
    int node, drag_node;    // selected / dragged node
    int link_from;          // dragging a link from this node's output
    int link_to, link_pin;  // ... or from this node's input (its link pulled off)
    bool any_sel;           // the Any State box selected
    GraphView views[1 + ANIM_MAX_MACHINES];
    ImVec2 menu_at;
    char search[64];
    bool fit; // frame the graph on the next draw (a graph shown first, Home)
    // Unreal's selection: several nodes (or states) by Ctrl / Shift + click or a box dragged on the empty canvas; they
    // move and delete together. A state's edge dragged to another state makes a transition
    uint64_t sel;           // the pose graph's nodes selected (bit per node)
    uint64_t sel_states[2]; // the states selected (bit per state)
    bool box;
    ImVec2 box_from;
    int link_state;         // dragging a new transition from this state (ANIM_ANY_STATE: from Any State; -1 none)
} G = { -1, -1, -1, -1, -1, -1, false, .link_state = -1 };

static bool sel_has(const uint64_t* m, int i) { return i >= 0 && i < 128 && (m[i >> 6] >> (i & 63) & 1ull); }
static void sel_set(uint64_t* m, int i, bool on) {
    if (i < 0 || i >= 128) return;
    if (on) m[i >> 6] |= 1ull << (i & 63);
    else m[i >> 6] &= ~(1ull << (i & 63));
}
// The box being dragged on the canvas: drawn, and on release true with its corners
static bool box_select(ImDrawList* dl, ImVec2* b0, ImVec2* b1) {
    if (!G.box) return false;
    ImVec2 mp = igGetIO_Nil()->MousePos;
    *b0 = (ImVec2){ fminf(G.box_from.x, mp.x), fminf(G.box_from.y, mp.y) };
    *b1 = (ImVec2){ fmaxf(G.box_from.x, mp.x), fmaxf(G.box_from.y, mp.y) };
    ImDrawList_AddRectFilled(dl, *b0, *b1, 0x1E1EAAFFu, 0.0f, 0);
    ImDrawList_AddRect(dl, *b0, *b1, 0xC81EAAFFu, 0.0f, 1.0f, 0);
    if (igIsMouseDown_Nil(ImGuiMouseButton_Left)) return false;
    G.box = false;
    return true;
}

static float node_h(const AnimNode* n, float zm);

// The view framing every node (or the machine's states), zoom at most 1 (Unreal's Home)
static void fit_view(const AnimGraph* g, int machine, GraphView* v, ImVec2 sz) {
    float mn[2] = { 1e30f, 1e30f }, mx[2] = { -1e30f, -1e30f };
    float fs = igGetFontSize();
    if (machine < 0)
        for (uint32_t i = 0; i < g->nnodes; i++) {
            const AnimNode* n = &g->nodes[i];
            mn[0] = fminf(mn[0], n->x), mn[1] = fminf(mn[1], n->y);
            mx[0] = fmaxf(mx[0], n->x + fs * 13.0f), mx[1] = fmaxf(mx[1], n->y + node_h(n, 1.0f));
        }
    else {
        mn[0] = -320.0f, mn[1] = -140.0f, mx[0] = -320.0f + fs * 9.0f, mx[1] = -140.0f + fs * 2.6f; // (Any State)
        for (uint32_t i = 0; i < g->nstates; i++) {
            if (g->states[i].machine != machine) continue;
            mn[0] = fminf(mn[0], g->states[i].x), mn[1] = fminf(mn[1], g->states[i].y);
            mx[0] = fmaxf(mx[0], g->states[i].x + fs * 9.0f), mx[1] = fmaxf(mx[1], g->states[i].y + fs * 2.6f);
        }
    }
    if (mn[0] > mx[0]) return;
    float w = mx[0] - mn[0] + 120.0f, h = mx[1] - mn[1] + 120.0f;
    v->zoom = fminf(fminf(sz.x / w, sz.y / h), 1.0f);
    if (v->zoom < 0.25f) v->zoom = 0.25f;
    v->pan[0] = sz.x * 0.5f / v->zoom - (mn[0] + mx[0]) * 0.5f;
    v->pan[1] = sz.y * 0.5f / v->zoom - (mn[1] + mx[1]) * 0.5f;
}

// A 2D blend space's samples on a square (X across, Y up), the weights at the dot under the mouse while hovering
static void blend2d_plot(uint32_t nsamples, const float* sx, const float* sy, char (*names)[ANIM_NAME]) {
    float size = igGetFontSize() * 12.0f;
    ImVec2 o = igGetCursorScreenPos();
    igInvisibleButton("##bs2d", (ImVec2){ size, size }, 0);
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_AddRectFilled(dl, o, (ImVec2){ o.x + size, o.y + size }, 0xFF1A1A1Au, 4.0f, 0);
    for (int k = 1; k < 4; k++) {
        float f = size * k / 4.0f;
        ImDrawList_AddLine(dl, (ImVec2){ o.x + f, o.y }, (ImVec2){ o.x + f, o.y + size }, 0xFF2A2A2Au, 1.0f);
        ImDrawList_AddLine(dl, (ImVec2){ o.x, o.y + f }, (ImVec2){ o.x + size, o.y + f }, 0xFF2A2A2Au, 1.0f);
    }
    if (!nsamples) return;
    float mnx = sx[0], mxx = mnx, mny = sy[0], mxy = mny;
    for (uint32_t i = 1; i < nsamples; i++) mnx = fminf(mnx, sx[i]), mxx = fmaxf(mxx, sx[i]), mny = fminf(mny, sy[i]), mxy = fmaxf(mxy, sy[i]);
    float spx = mxx - mnx > 1e-3f ? mxx - mnx : 1.0f, spy = mxy - mny > 1e-3f ? mxy - mny : 1.0f, pad = size * 0.1f, in = size - 2 * pad;
    float wgt[ANIM_BLEND_SAMPLES] = { 0 };
    bool hov = igIsItemHovered(0);
    if (hov) {
        ImVec2 mp = igGetIO_Nil()->MousePos;
        float x = mnx + (mp.x - o.x - pad) / in * spx, y = mny + (o.y + size - pad - mp.y) / in * spy;
        anim_blend2d_weights(sx, sy, nsamples, x, y, wgt);
        ImDrawList_AddCircleFilled(dl, mp, 4.0f, 0xFF3FB7F2u, 12);
    }
    for (uint32_t i = 0; i < nsamples; i++) {
        float px = o.x + pad + (sx[i] - mnx) / spx * in, py = o.y + size - pad - (sy[i] - mny) / spy * in;
        ImDrawList_AddCircleFilled(dl, (ImVec2){ px, py }, 4.0f + (hov ? wgt[i] * 10.0f : 0.0f), 0xFFE0A040u, 16);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ px + 6, py - 7 }, 0xFFB0B0B0u, names[i][0] ? names[i] : "-", NULL);
    }
    igSetItemTooltip("%s", TR("The samples at their X, Y; hover: their weights there"));
}

static bool clip_combo(const char* label, char* clip, const char* skeleton) {
    bool ch = false;
    if (igBeginCombo(label, clip[0] ? clip : "-", 0)) {
        if (igSelectable_Bool("-", !clip[0], 0, (ImVec2){ 0, 0 })) clip[0] = 0, ch = true;
        for (uint32_t i = 0; i < anim_seq_count(); i++) {
            AnimSeq* q = anim_seq_at(i);
            if (skeleton[0] && strcmp(q->skeleton, skeleton)) continue;
            if (asset_selectable(q->name, !strcmp(clip, q->name), q->path, ASSET_ANIMATION)) snprintf(clip, ANIM_NAME, "%s", q->name), ch = true;
        }
        igEndCombo();
    }
    return ch;
}

static bool var_combo(const char* label, char* var, const BpAsset* a, int type) {
    bool ch = false;
    if (igBeginCombo(label, var[0] ? var : "-", 0)) {
        if (igSelectable_Bool("-", !var[0], 0, (ImVec2){ 0, 0 })) var[0] = 0, ch = true;
        for (uint32_t i = 0; i < a->nvars; i++) {
            const BpVar* v = &a->vars[i];
            if (type >= 0 && v->type != type && !(type == BPT_INT && v->type == BPT_FLOAT)) continue;
            if (igSelectable_Bool(v->name, !strcmp(var, v->name), 0, (ImVec2){ 0, 0 })) snprintf(var, ANIM_NAME, "%s", v->name), ch = true;
        }
        igEndCombo();
    }
    return ch;
}

static bool bone_combo(const char* label, char* bone, const SkelMesh* sk) {
    bool ch = false;
    if (!sk) {
        igSetNextItemWidth(-igGetFontSize() * 6.5f);
        return igInputText(label, bone, ANIM_NAME, 0, NULL, NULL);
    }
    if (igBeginCombo(label, bone[0] ? bone : "-", 0)) {
        if (igSelectable_Bool("-", !bone[0], 0, (ImVec2){ 0, 0 })) bone[0] = 0, ch = true;
        for (uint32_t b = 0; b < sk->nbones; b++)
            if (igSelectable_Bool(sk->bones[b].name, !strcmp(bone, sk->bones[b].name), 0, (ImVec2){ 0, 0 }))
                snprintf(bone, ANIM_NAME, "%s", sk->bones[b].name), ch = true;
        igEndCombo();
    }
    return ch;
}

// A blend space's samples (clip, X and, for 2D, Y; Motion Matching: clips only)
static bool samples_edit(uint8_t* nsamples, char (*samples)[ANIM_NAME], float* at, float* ys, const char* skeleton, int dims) {
    bool ch = false;
    for (uint32_t i = 0; i < *nsamples; i++) {
        igPushID_Int((int)i);
        igSetNextItemWidth(igGetFontSize() * (dims ? 8.0f : 12.0f));
        ch |= clip_combo("##c", samples[i], skeleton);
        if (dims) {
            igSameLine(0, 4);
            igSetNextItemWidth(igGetFontSize() * 4);
            ch |= igDragFloat("##at", &at[i], 1.0f, -1e4f, 1e4f, dims == 2 ? "X %.0f" : "%.0f", 0);
        }
        if (dims == 2) {
            igSameLine(0, 4);
            igSetNextItemWidth(igGetFontSize() * 4);
            ch |= igDragFloat("##y", &ys[i], 1.0f, -1e4f, 1e4f, "Y %.0f", 0);
        }
        igSameLine(0, 4);
        if (igSmallButton("x")) { // (removed)
            uint32_t rest = *nsamples - i - 1;
            memmove(&samples[i], &samples[i + 1], sizeof samples[0] * rest);
            memmove(&at[i], &at[i + 1], sizeof(float) * rest);
            memmove(&ys[i], &ys[i + 1], sizeof(float) * rest);
            (*nsamples)--, ch = true;
            igPopID();
            break;
        }
        igPopID();
    }
    if (*nsamples < ANIM_BLEND_SAMPLES && igButton(dims ? TR("Add Sample") : TR("Add Animation"), (ImVec2){ 0, 0 })) {
        uint32_t k = *nsamples;
        samples[k][0] = 0;
        at[k] = k && dims == 1 ? at[k - 1] + 100.0f : 0.0f;
        ys[k] = 0.0f;
        (*nsamples)++, ch = true;
    }
    return ch;
}

// The skeleton's looping clips into a Motion Matching database
static void mm_fill(const AnimGraph* g, uint8_t* n, char (*samples)[ANIM_NAME]) {
    for (uint32_t i = 0; i < anim_seq_count() && *n < ANIM_BLEND_SAMPLES; i++) {
        const AnimSeq* q = anim_seq_at(i);
        if (q->loop && (!g->skeleton[0] || !strcmp(q->skeleton, g->skeleton))) snprintf(samples[(*n)++], ANIM_NAME, "%s", q->name);
    }
}

// --- Unreal's Blend Space editor: a window of its own for a blend space node or state. Asset Details on the left (the
// axes: their variable, range, grid divisions; the samples), the grid in the middle (samples dragged and snapped to it,
// right click removes, animations dropped from the Asset Browser on the right), the preview point (the mouse; Shift
// holds it) with each sample's weight ---

// a name containing text, any case
static bool str_has_ci(const char* hay, const char* needle) {
    for (; *hay; hay++) {
        size_t k = 0;
        while (needle[k] && hay[k] && tolower((unsigned char)hay[k]) == tolower((unsigned char)needle[k])) k++;
        if (!needle[k]) return true;
    }
    return !needle[0];
}

typedef struct BsRef {
    uint8_t* n;
    char (*names)[ANIM_NAME];
    float *x, *y, *axis;
    char *px, *py;
    int dims;
    const char* title;
} BsRef;

static struct {
    bool open, focus;
    uint32_t hash; // the Anim Blueprint's
    int node, state, drag;
    float preview[2];
    char search[64];
} BS = { .node = -1, .state = -1, .drag = -1 };

static void bs_open(const BpAsset* a, int node, int state) {
    BS.open = BS.focus = true, BS.hash = a->hash, BS.node = node, BS.state = state, BS.drag = -1;
}

static void tw_open(const BpAsset* a, int trans); // (a transition's rule window, below)

static bool bs_ref(AnimGraph* g, BsRef* r) {
    if (BS.node >= 0 && BS.node < (int)g->nnodes) {
        AnimNode* n = &g->nodes[BS.node];
        if (n->kind != AN_BLEND_SPACE_1D && n->kind != AN_BLEND_SPACE_2D) return false;
        *r = (BsRef){ &n->nsamples, n->samples, n->sample_at, n->sample_y, n->axis, n->var[0], n->var[1], n->kind == AN_BLEND_SPACE_2D ? 2 : 1,
                      TR(anim_node_info(n->kind)->name) };
        return true;
    }
    if (BS.state >= 0 && BS.state < (int)g->nstates) {
        AnimState* s = &g->states[BS.state];
        if (s->kind != ANIM_STATE_BLEND1D && s->kind != ANIM_STATE_BLEND2D) return false;
        *r = (BsRef){ &s->nsamples, s->samples, s->sample_at, s->sample_y, s->axis, s->param, s->param2, s->kind == ANIM_STATE_BLEND2D ? 2 : 1, s->name };
        return true;
    }
    return false;
}

// Axes never set: from the samples (0 to the farthest, rounded up to tens), 4 divisions
static void bs_axes_default(BsRef* r) {
    for (int k = 0; k < r->dims; k++) {
        float* ax = &r->axis[k * 2];
        if (r->axis[4 + k] < 1.0f) r->axis[4 + k] = 4.0f;
        if (ax[1] > ax[0]) continue;
        const float* v = k ? r->y : r->x;
        float mn = 0.0f, mx = 0.0f;
        for (uint32_t i = 0; i < *r->n; i++) mn = fminf(mn, v[i]), mx = fmaxf(mx, v[i]);
        ax[0] = mn < 0.0f ? -ceilf(-mn / 10.0f) * 10.0f : 0.0f;
        ax[1] = mx > ax[0] + 1.0f ? ceilf(mx / 10.0f) * 10.0f : ax[0] + 100.0f;
    }
}

// A 1D blend space's weights: the two samples around v, linear between them
static void bs_weights1d(const float* x, uint32_t n, float v, float* w) {
    int lo = -1, hi = -1;
    for (uint32_t i = 0; i < n; i++) {
        w[i] = 0.0f;
        if (x[i] <= v && (lo < 0 || x[i] > x[lo])) lo = (int)i;
        if (x[i] >= v && (hi < 0 || x[i] < x[hi])) hi = (int)i;
    }
    if (lo < 0) lo = hi;
    if (hi < 0) hi = lo;
    if (lo < 0) return;
    float span = x[hi] - x[lo], f = span > 1e-4f ? (v - x[lo]) / span : 0.0f;
    w[lo] = 1.0f - f;
    w[hi] += f;
}

static bool bs_grid(BsRef* r) {
    bool ch = false;
    ImGuiIO* io = igGetIO_Nil();
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 o = igGetCursorScreenPos(), sz = igGetContentRegionAvail();
    float fs = igGetFontSize();
    igInvisibleButton("##bs_canvas", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hov = igIsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem), clicked = igIsItemClicked(ImGuiMouseButton_Left);
    ImVec2 g0 = { o.x + fs * 4.5f, o.y + fs * 1.5f }, g1 = { o.x + sz.x - fs * 1.5f, o.y + sz.y - fs * 3.2f };
    if (r->dims == 1) { // a band across the middle
        float mid = (g0.y + g1.y) * 0.5f;
        g0.y = mid - fs * 2.5f, g1.y = mid + fs * 2.5f;
    }
    float ax0 = r->axis[0], ax1 = r->axis[1], ay0 = r->axis[2], ay1 = r->axis[3];
    float gw = g1.x - g0.x, gh = g1.y - g0.y;
#define BS_SX(v) (g0.x + ((v) - ax0) / (ax1 - ax0) * gw)
#define BS_SY(v) (r->dims == 2 ? g1.y - ((v) - ay0) / (ay1 - ay0) * gh : (g0.y + g1.y) * 0.5f)
    float mvx = ax0 + (io->MousePos.x - g0.x) / gw * (ax1 - ax0), mvy = ay0 + (g1.y - io->MousePos.y) / gh * (ay1 - ay0);
    int divx = (int)r->axis[4], divy = (int)r->axis[5];
    float stepx = (ax1 - ax0) / (float)divx, stepy = (ay1 - ay0) / (float)divy;
    // the grid, its values and the axes' names (Unreal's)
    ImDrawList_AddRectFilled(dl, g0, g1, 0xFF1C1C1Cu, 0.0f, 0);
    char t[48];
    for (int k = 0; k <= divx; k++) {
        float x = g0.x + gw * (float)k / (float)divx;
        ImDrawList_AddLine(dl, (ImVec2){ x, g0.y }, (ImVec2){ x, g1.y }, k == 0 || k == divx ? 0xFF505050u : 0xFF333333u, 1.0f);
        snprintf(t, sizeof t, "%g", ax0 + stepx * (float)k);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ x - igCalcTextSize(t, NULL, false, -1).x * 0.5f, g1.y + 4 }, 0xFF9A9A9Au, t, NULL);
    }
    for (int k = 0; r->dims == 2 && k <= divy; k++) {
        float y = g1.y - gh * (float)k / (float)divy;
        ImDrawList_AddLine(dl, (ImVec2){ g0.x, y }, (ImVec2){ g1.x, y }, k == 0 || k == divy ? 0xFF505050u : 0xFF333333u, 1.0f);
        snprintf(t, sizeof t, "%g", ay0 + stepy * (float)k);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ g0.x - igCalcTextSize(t, NULL, false, -1).x - 6, y - fs * 0.5f }, 0xFF9A9A9Au, t, NULL);
    }
    if (r->dims == 1) ImDrawList_AddRect(dl, g0, g1, 0xFF505050u, 0.0f, 1.0f, 0);
    const char* xn = r->px[0] ? r->px : TR("(no variable)");
    ImDrawList_AddText_Vec2(dl, (ImVec2){ (g0.x + g1.x - igCalcTextSize(xn, NULL, false, -1).x) * 0.5f, g1.y + fs * 1.5f }, 0xFFD8D8D8u, xn, NULL);
    if (r->dims == 2) ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 4, g0.y - fs * 1.3f }, 0xFFD8D8D8u, r->py[0] ? r->py : TR("(no variable)"), NULL);
    // a sample under the mouse: dragged (snapped to the grid, Alt: free), right click: its menu
    int hs = -1;
    for (uint32_t i = 0; i < *r->n; i++)
        if (fabsf(io->MousePos.x - BS_SX(r->x[i])) < fs * 0.6f && fabsf(io->MousePos.y - BS_SY(r->y[i])) < fs * 0.6f) hs = (int)i;
    if (clicked) BS.drag = hs;
    if (BS.drag >= 0 && BS.drag < (int)*r->n && igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
        float nx = fminf(fmaxf(mvx, ax0), ax1), ny = fminf(fmaxf(mvy, ay0), ay1);
        if (!io->KeyAlt) nx = ax0 + roundf((nx - ax0) / stepx) * stepx, ny = ay0 + roundf((ny - ay0) / stepy) * stepy;
        if (nx != r->x[BS.drag] || (r->dims == 2 && ny != r->y[BS.drag])) {
            r->x[BS.drag] = nx;
            if (r->dims == 2) r->y[BS.drag] = ny;
            ch = true;
        }
    } else BS.drag = -1;
    static int menu_sample = -1;
    if (hov && hs >= 0 && igIsMouseClicked_Bool(ImGuiMouseButton_Right, false)) menu_sample = hs, igOpenPopup_Str("bs_sample_menu", 0);
    if (igBeginPopup("bs_sample_menu", 0)) {
        if (menu_sample >= 0 && menu_sample < (int)*r->n) {
            igTextDisabled("%s", r->names[menu_sample][0] ? r->names[menu_sample] : "-");
            igSeparator();
            if (ui_menu_item(ICON_DELETE, TR("Delete"), "Del", false, true)) {
                uint32_t i = (uint32_t)menu_sample, rest = *r->n - i - 1;
                memmove(&r->names[i], &r->names[i + 1], sizeof r->names[0] * rest);
                memmove(&r->x[i], &r->x[i + 1], sizeof(float) * rest);
                memmove(&r->y[i], &r->y[i + 1], sizeof(float) * rest);
                (*r->n)--, menu_sample = -1, ch = true;
            }
        }
        igEndPopup();
    }
    // an animation dropped from the Asset Browser: a sample there
    if (igBeginDragDropTarget()) {
        const ImGuiPayload* p = igAcceptDragDropPayload("RE_BS_ANIM", 0);
        if (p && *r->n < ANIM_BLEND_SAMPLES) {
            uint32_t k = (*r->n)++;
            snprintf(r->names[k], ANIM_NAME, "%s", (const char*)p->Data);
            r->x[k] = ax0 + roundf((fminf(fmaxf(mvx, ax0), ax1) - ax0) / stepx) * stepx;
            r->y[k] = r->dims == 2 ? ay0 + roundf((fminf(fmaxf(mvy, ay0), ay1) - ay0) / stepy) * stepy : 0.0f;
            ch = true;
        }
        igEndDragDropTarget();
    }
    // the preview point: where the mouse is (Shift holds it), each sample's weight there
    if (hov && !io->KeyShift && BS.drag < 0 && io->MousePos.x >= g0.x && io->MousePos.x <= g1.x && io->MousePos.y >= g0.y - fs && io->MousePos.y <= g1.y + fs)
        BS.preview[0] = fminf(fmaxf(mvx, ax0), ax1), BS.preview[1] = r->dims == 2 ? fminf(fmaxf(mvy, ay0), ay1) : 0.0f;
    float wgt[ANIM_BLEND_SAMPLES] = { 0 };
    if (r->dims == 2) anim_blend2d_weights(r->x, r->y, *r->n, BS.preview[0], BS.preview[1], wgt);
    else bs_weights1d(r->x, *r->n, BS.preview[0], wgt);
    for (uint32_t i = 0; i < *r->n; i++) { // the samples: Unreal's diamonds, bigger with their weight
        ImVec2 c = { BS_SX(r->x[i]), BS_SY(r->y[i]) };
        float s = fs * (0.4f + wgt[i] * 0.45f);
        uint32_t col = (int)i == BS.drag ? 0xFF28A0FFu : (int)i == hs ? 0xFFFFFFFFu : wgt[i] > 0.001f ? 0xFF40C8F0u : 0xFFB0B0B0u;
        ImDrawList_AddQuadFilled(dl, (ImVec2){ c.x, c.y - s }, (ImVec2){ c.x + s, c.y }, (ImVec2){ c.x, c.y + s }, (ImVec2){ c.x - s, c.y }, col);
        ImDrawList_AddQuad(dl, (ImVec2){ c.x, c.y - s }, (ImVec2){ c.x + s, c.y }, (ImVec2){ c.x, c.y + s }, (ImVec2){ c.x - s, c.y }, 0xFF000000u, 1.0f);
        char lab[ANIM_NAME + 16];
        snprintf(lab, sizeof lab, wgt[i] > 0.001f ? "%s  %.0f%%" : "%s", r->names[i][0] ? r->names[i] : "-", wgt[i] * 100.0f);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ c.x + s + 4, c.y - fs * 0.5f }, 0xFFD0D0D0u, lab, NULL);
    }
    ImVec2 pc = { BS_SX(BS.preview[0]), BS_SY(BS.preview[1]) };
    ImDrawList_AddCircleFilled(dl, pc, fs * 0.28f, 0xFF50D050u, 12);
    ImDrawList_AddCircle(dl, pc, fs * 0.5f, 0xFF50D050u, 16, 1.5f);
    snprintf(t, sizeof t, r->dims == 2 ? "%.1f, %.1f" : "%.1f", BS.preview[0], BS.preview[1]);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ pc.x + fs * 0.7f, pc.y + fs * 0.2f }, 0xFF80E080u, t, NULL);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 6, o.y + sz.y - fs * 1.3f }, 0xFF7A7A7Au,
                            TR("Drag a sample to move it (snaps to the grid; Alt: free) | right click: delete | drop animations from the Asset Browser | Shift: hold the preview"), NULL);
#undef BS_SX
#undef BS_SY
    return ch;
}

// The character at the preview point: the samples played in step (one phase, a share of each one's length: Unreal's
// sync of a blend space) and blended by their weights there
static void bs_preview(AnimGraph* g, const BsRef* r) {
    static AssetViewport vp; // (Q7)
    static float phase;
    static AnimXf poses[ANIM_BLEND_SAMPLES][ANIM_MAX_BONES], out[ANIM_MAX_BONES];
    SkelMesh* m = g->skeleton[0] ? anim_mesh_name(g->skeleton) : NULL;
    ImVec2 p = igGetCursorScreenPos(), sz = igGetContentRegionAvail();
    if (!m || !*r->n) {
        igTextDisabled("%s", !m ? TR("Pick the Skeleton in the Anim Graph's settings to preview it here.") : TR("Add samples to see them blend here."));
        return;
    }
    float wgt[ANIM_BLEND_SAMPLES] = { 0 }, ww[ANIM_BLEND_SAMPLES], len = 0.0f, sum = 0.0f;
    if (r->dims == 2) anim_blend2d_weights(r->x, r->y, *r->n, BS.preview[0], BS.preview[1], wgt);
    else bs_weights1d(r->x, *r->n, BS.preview[0], wgt);
    AnimSeq* qs[ANIM_BLEND_SAMPLES];
    for (uint32_t i = 0; i < *r->n; i++) {
        qs[i] = wgt[i] > 1e-4f ? anim_seq_name(r->names[i]) : NULL;
        if (qs[i]) len += wgt[i] * qs[i]->length, sum += wgt[i];
    }
    if (sum > 0.0f) phase = fmodf(phase + igGetIO_Nil()->DeltaTime * sum / fmaxf(len, 0.05f), 1.0f);
    const AnimXf* pp[ANIM_BLEND_SAMPLES];
    uint32_t k = 0;
    for (uint32_t i = 0; i < *r->n; i++)
        if (qs[i]) {
            anim_sample(qs[i], m, phase * qs[i]->length, poses[k]);
            pp[k] = poses[k], ww[k] = wgt[i] / sum, k++;
        }
    if (k) anim_blend_n(pp, ww, k, m->nbones, out);
    else
        for (uint32_t b = 0; b < m->nbones; b++) out[b] = m->bones[b].bind;
    preview_cam(&vp, p, sz, m, out, -1, -1, BP_PV_KEY_BLEND_SPACE, 0, 0, NULL, NULL);
}

static bool bs_window(BpAsset* a, AnimGraph* g) {
    if (!BS.open || BS.hash != a->hash) return false;
    BsRef r;
    if (!bs_ref(g, &r)) {
        BS.open = false;
        return false;
    }
    bs_axes_default(&r);
    bool ch = false, open = true;
    char title[200];
    snprintf(title, sizeof title, "%s - %s (%s)###blend_space_editor", a->name, r.title, TR("Blend Space"));
    const ImGuiViewport* mv = igGetMainViewport();
    igSetNextWindowSize((ImVec2){ fminf(1200.0f, mv->Size.x * 0.75f), fminf(680.0f, mv->Size.y * 0.7f) }, ImGuiCond_FirstUseEver);
    if (BS.focus) igSetNextWindowFocus(), BS.focus = false;
    if (!igBegin(title, &open, ImGuiWindowFlags_NoCollapse)) {
        igEnd();
        BS.open = open;
        return false;
    }
    float fs = igGetFontSize();
    ImVec2 av = igGetContentRegionAvail();
    float lw = fs * 20.0f, rw = fs * 14.0f;
    // Asset Details: the axes, the samples
    igBeginChild_Str("bs_details", (ImVec2){ lw, av.y }, ImGuiChildFlags_Borders, 0);
    igSeparatorText(TR("Axis Settings"));
    for (int k = 0; k < r.dims; k++) {
        igPushID_Int(k);
        igTextDisabled("%s", k ? TR("Vertical Axis") : TR("Horizontal Axis"));
        ch |= var_combo(PL(TR("Name")), k ? r.py : r.px, a, BPT_FLOAT);
        ch |= igDragFloat(PL(TR("Minimum Axis Value")), &r.axis[k * 2], 1.0f, -1e5f, 1e5f, "%.1f", 0);
        ch |= igDragFloat(PL(TR("Maximum Axis Value")), &r.axis[k * 2 + 1], 1.0f, -1e5f, 1e5f, "%.1f", 0);
        if (r.axis[k * 2 + 1] <= r.axis[k * 2]) r.axis[k * 2 + 1] = r.axis[k * 2] + 1.0f;
        int div = (int)r.axis[4 + k];
        if (igDragInt(PL(TR("Grid Divisions")), &div, 0.1f, 1, 32, "%d", ImGuiSliderFlags_AlwaysClamp)) r.axis[4 + k] = (float)div, ch = true;
        igPopID();
    }
    igSeparatorText(TR("Blend Samples"));
    ch |= samples_edit(r.n, r.names, r.x, r.y, g->skeleton, r.dims);
    igEndChild();
    igSameLine(0, 4);
    // the grid above, the character blending at the preview point below (Unreal's Blend Space editor's viewport)
    float mw = av.x - lw - rw - 8.0f, gh = floorf(av.y * 0.58f);
    igBeginGroup();
    igBeginChild_Str("bs_grid", (ImVec2){ mw, gh }, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ch |= bs_grid(&r);
    igEndChild();
    igBeginChild_Str("bs_view", (ImVec2){ mw, av.y - gh - igGetStyle()->ItemSpacing.y }, ImGuiChildFlags_Borders,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    bs_preview(g, &r);
    igEndChild();
    igEndGroup();
    igSameLine(0, 4);
    // the Asset Browser: the skeleton's animations, dragged onto the grid (double click: at the preview point)
    igBeginChild_Str("bs_assets", (ImVec2){ rw, av.y }, ImGuiChildFlags_Borders, 0);
    igSeparatorText(TR("Asset Browser"));
    igSetNextItemWidth(-FLT_MIN);
    igInputTextWithHint("##bs_search", TR("Search"), BS.search, sizeof BS.search, 0, NULL, NULL);
    for (uint32_t i = 0; i < anim_seq_count(); i++) {
        const AnimSeq* q = anim_seq_at(i);
        if (g->skeleton[0] && strcmp(q->skeleton, g->skeleton)) continue;
        if (BS.search[0] && !str_has_ci(q->name, BS.search)) continue;
        igPushID_Int((int)i);
        if (igSelectable_Bool(q->name, false, ImGuiSelectableFlags_AllowDoubleClick, (ImVec2){ 0, 0 }) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left) &&
            *r.n < ANIM_BLEND_SAMPLES) {
            uint32_t k = (*r.n)++;
            snprintf(r.names[k], ANIM_NAME, "%s", q->name);
            r.x[k] = BS.preview[0], r.y[k] = BS.preview[1], ch = true;
        }
        if (igBeginDragDropSource(0)) {
            igSetDragDropPayload("RE_BS_ANIM", q->name, strlen(q->name) + 1, 0);
            igTextUnformatted(q->name, NULL);
            igEndDragDropSource();
        }
        igPopID();
    }
    igEndChild();
    igEnd();
    BS.open = open;
    return ch;
}

// --- The pose graph's nodes on the canvas ---

// A text at the canvas's zoom (the nodes' titles and pins shrink and grow with it)
static void ztext(ImDrawList* dl, ImVec2 at, uint32_t col, const char* t, float zm) {
    ImDrawList_AddText_FontPtr(dl, igGetFont(), igGetFontSize() * zm, at, col, t, NULL, 0.0f, NULL);
}
// A hint in the Details: dimmed and wrapped
static void hint(const char* t) {
    igPushStyleColor_Vec4(ImGuiCol_Text, igGetStyle()->Colors[ImGuiCol_TextDisabled]);
    igTextWrapped("%s", t);
    igPopStyleColor(1);
}

static float node_w(float zm) { return igGetFontSize() * 13.0f * zm; }
// a Reroute is a small knot: its input and output on one line
static float node_wn(const AnimNode* n, float zm) { return n->kind == AN_REROUTE ? igGetFontSize() * 2.2f * zm : node_w(zm); }
static float node_h(const AnimNode* n, float zm) {
    if (n->kind == AN_REROUTE) return igGetFontSize() * 1.1f * zm;
    int rows = anim_node_info(n->kind)->inputs;
    return (igGetFontSize() * 2.9f + igGetFontSize() * 1.35f * (float)(rows > 1 ? rows : 1)) * zm;
}
static ImVec2 pin_in(const GraphView* v, ImVec2 o, const AnimNode* n, int k) {
    ImVec2 p = gv_screen(v, o, n->x, n->y);
    if (n->kind == AN_REROUTE) return (ImVec2){ p.x, p.y + node_h(n, v->zoom) * 0.5f };
    return (ImVec2){ p.x, p.y + (igGetFontSize() * 2.9f + igGetFontSize() * 1.35f * ((float)k + 0.5f)) * v->zoom };
}
static ImVec2 pin_out(const GraphView* v, ImVec2 o, const AnimNode* n) {
    ImVec2 p = gv_screen(v, o, n->x, n->y);
    if (n->kind == AN_REROUTE) return (ImVec2){ p.x + node_wn(n, v->zoom), p.y + node_h(n, v->zoom) * 0.5f };
    return (ImVec2){ p.x + node_w(v->zoom), p.y + igGetFontSize() * 3.55f * v->zoom };
}

// The line under a node's title: what it plays / reads
static void node_subtitle(const AnimGraph* g, const AnimNode* n, char* out, size_t cap) {
    out[0] = 0;
    switch (n->kind) {
    case AN_STATE_MACHINE: snprintf(out, cap, "%s", n->machine < g->nmachines ? g->machines[n->machine] : "-"); break;
    case AN_SEQUENCE: case AN_APPLY_ADDITIVE: snprintf(out, cap, "%s", n->clip[0] ? n->clip : "(none)"); break;
    case AN_BLEND_SPACE_1D: case AN_BLEND_SPACE_2D: snprintf(out, cap, "%u samples  %s %s", n->nsamples, n->var[0], n->var[1]); break;
    case AN_MOTION_MATCH: snprintf(out, cap, "%u clips  %s", n->nsamples, n->var[0][0] ? n->var[0] : "Speed"); break;
    case AN_BLEND_BOOL: case AN_BLEND_INT: case AN_BLEND: snprintf(out, cap, "%s", n->var[0][0] ? n->var[0] : ""); break;
    case AN_SLOT: snprintf(out, cap, "'%s'", n->name); break;
    case AN_SAVE_CACHE: case AN_USE_CACHE: snprintf(out, cap, "%s", n->name); break;
    case AN_TWO_BONE_IK: case AN_MODIFY_BONE: case AN_LOOK_AT: case AN_LAYERED_BLEND: snprintf(out, cap, "%s", n->bone[0]); break;
    case AN_AIM_OFFSET: snprintf(out, cap, "%s / %s", n->var[0], n->var[1]); break;
    case AN_LEG_IK: snprintf(out, cap, "%s, %s", n->bone[0], n->bone[1]); break;
    default: break;
    }
}

static int node_at(const AnimGraph* g, const GraphView* v, ImVec2 o, ImVec2 mp) {
    for (int i = (int)g->nnodes - 1; i >= 0; i--) {
        ImVec2 p = gv_screen(v, o, g->nodes[i].x, g->nodes[i].y);
        if (mp.x >= p.x && mp.x < p.x + node_wn(&g->nodes[i], v->zoom) && mp.y >= p.y && mp.y < p.y + node_h(&g->nodes[i], v->zoom)) return i;
    }
    return -1;
}

static void delete_node(AnimGraph* g, int d) {
    if (d < 0 || d >= (int)g->nnodes || g->nodes[d].kind == AN_OUTPUT) return;
    memmove(&g->nodes[d], &g->nodes[d + 1], sizeof(AnimNode) * (g->nnodes - (uint32_t)d - 1));
    g->nnodes--;
    for (uint32_t i = 0; i < g->nnodes; i++)
        for (int k = 0; k < ANIM_NODE_IN; k++) {
            int16_t* in = &g->nodes[i].in[k];
            if (*in == d) *in = -1;
            else if (*in > d) (*in)--;
        }
}

// A new State Machine: its own first state ("Idle", the entry)
static int new_machine(AnimGraph* g) {
    if (g->nmachines >= ANIM_MAX_MACHINES || g->nstates >= ANIM_MAX_STATES) return -1;
    int m = (int)g->nmachines++;
    snprintf(g->machines[m], ANIM_NAME, "New State Machine %d", m);
    AnimState* s = &g->states[g->nstates];
    memset(s, 0, sizeof *s);
    snprintf(s->name, ANIM_NAME, "Idle");
    s->machine = (uint8_t)m, s->kind = ANIM_STATE_SEQUENCE, s->loop = true, s->rate = 1.0f;
    g->machine_entry[m] = (uint8_t)g->nstates++;
    return m;
}

// A node of kind k placed where the menu opened (fed into connect_to_input_of's pin when >= 0); noted for the menus'
// recents and recommendations (graph_menu.h) after the node it connects to
static int ag_kind_named(const char* name) {
    for (int k = 1; k < AN_KIND_COUNT; k++)
        if (!strcmp(anim_node_info(k)->name, name)) return k;
    return -1;
}

static bool ag_accept(const char* item, void* user) {
    const char** shown = user;
    return ag_kind_named(item) > 0 && strcmp(item, "Output Pose") && (!shown[0] || strcmp(shown[0], item)) && (!shown[1] || strcmp(shown[1], item));
}

static bool add_menu_nodes(AnimGraph* g, ImVec2 o, int connect_to_input_of, int pin) {
    bool ch = false;
    igSetNextItemWidth(igGetFontSize() * 14);
    if (igIsWindowAppearing()) igSetKeyboardFocusHere(0), G.search[0] = 0;
    igInputTextWithHint("##find", TR("Search"), G.search, sizeof G.search, 0, NULL, NULL);
    // what is being done: the node the wire comes from, else the selected one
    int ctx_node = connect_to_input_of >= 0 ? connect_to_input_of : G.node;
    const char* ctx = ctx_node >= 0 && ctx_node < (int)g->nnodes ? anim_node_info(g->nodes[ctx_node].kind)->name : "";
    int pick = -1;
    if (!G.search[0]) { // the last 2 used in AnimGraphs, then 2 recommended after what is being done
        const char* items[8];
        const char* shown[2] = { NULL, NULL };
        int n = gm_recent(GM_ANIM_GRAPH, items, 8), ns = 0;
        for (int i = 0; i < n && ns < GM_RECENT_SHOWN; i++) {
            int k = ag_kind_named(items[i]);
            if (k <= 0 || !strcmp(items[i], "Output Pose")) continue;
            if (!ns) igSeparatorText(gm_title_recent());
            igPushID_Str("agr"), igPushID_Int(ns);
            if (igMenuItem_Bool(TR(items[i]), NULL, false, g->nnodes < ANIM_MAX_NODES)) pick = k;
            igPopID(), igPopID();
            shown[ns++] = items[i];
        }
        n = gm_recommend(GM_ANIM_GRAPH, ctx, ag_accept, (void*)shown, items, GM_RECOMMEND_SHOWN);
        for (int i = 0; i < n; i++) {
            if (!i) igSeparatorText(gm_title_recommended());
            igPushID_Str("agm"), igPushID_Int(i);
            if (igMenuItem_Bool(TR(items[i]), NULL, false, g->nnodes < ANIM_MAX_NODES)) pick = ag_kind_named(items[i]);
            igPopID(), igPopID();
        }
        igSeparatorText(TR("All Actions for this Graph"));
    }
    static const char* const cats[] = { "State Machines", "Animation", "Blends", "Montage", "Skeletal Controls", "Cached Poses", "Utilities" };
    for (int c = 0; c < 7 || pick > 0; c++) {
        bool open = pick > 0 || G.search[0] ? true : igBeginMenu(TR(cats[c]), true);
        if (!open) continue;
        for (int k = 1; k < AN_KIND_COUNT; k++) {
            const AnimNodeInfo* ni = anim_node_info(k);
            if (pick > 0 && k != pick) continue;
            if (pick <= 0 && strcmp(ni->category, cats[c])) continue;
            if (G.search[0] && pick <= 0) { // (by name, either language)
                char a[96], b[96];
                snprintf(a, sizeof a, "%s %s", ni->name, TR(ni->name));
                snprintf(b, sizeof b, "%s", G.search);
                for (char* p = a; *p; p++) *p = (char)tolower((unsigned char)*p);
                for (char* p = b; *p; p++) *p = (char)tolower((unsigned char)*p);
                if (!strstr(a, b)) continue;
            }
            if (pick != k && !igMenuItem_Bool(TR(ni->name), NULL, false, g->nnodes < ANIM_MAX_NODES)) continue;
            if (g->nnodes >= ANIM_MAX_NODES) break;
            gm_note(GM_ANIM_GRAPH, ctx, ni->name);
            int at = (int)g->nnodes;
            AnimNode* n = &g->nodes[g->nnodes++];
            anim_node_init(n, k);
            gv_graph(&G.views[0], o, G.menu_at, &n->x, &n->y);
            if (k == AN_STATE_MACHINE) {
                int m = new_machine(g);
                n->machine = (uint8_t)(m >= 0 ? m : 0);
            }
            if (k == AN_MOTION_MATCH) mm_fill(g, &n->nsamples, n->samples), snprintf(n->var[0], ANIM_NAME, "Speed");
            if (k == AN_USE_CACHE) // (the first saved pose's name)
                for (uint32_t i = 0; i < g->nnodes; i++)
                    if (g->nodes[i].kind == AN_SAVE_CACHE) {
                        snprintf(n->name, ANIM_NAME, "%s", g->nodes[i].name);
                        break;
                    }
            if (connect_to_input_of >= 0 && connect_to_input_of < at) g->nodes[connect_to_input_of].in[pin] = (int16_t)at;
            G.node = at, ch = true;
            if (pick > 0) igCloseCurrentPopup();
        }
        if (pick > 0) break; // (placed from the recents or the recommendations)
        if (!G.search[0]) igEndMenu();
    }
    return ch;
}

// (2026-11-23) Organize Nodes (Q, the canvas's menu): the selected nodes (two or more), else all, in columns toward the
// Output Pose (graph_layout.c)
static bool organize_pose(AnimGraph* g) {
    static GlNode nodes[256];
    static GlEdge edges[256 * ANIM_NODE_IN];
    static bool only[256];
    if (g->nnodes < 2 || g->nnodes > 256) return false;
    int nsel = 0, ne = 0;
    for (uint32_t i = 0; i < g->nnodes; i++) nsel += sel_has(&G.sel, (int)i);
    for (uint32_t i = 0; i < g->nnodes; i++) {
        nodes[i] = (GlNode){ g->nodes[i].x, g->nodes[i].y, node_wn(&g->nodes[i], 1.0f), node_h(&g->nodes[i], 1.0f) };
        only[i] = nsel < 2 || sel_has(&G.sel, (int)i);
        for (int k = 0; k < ANIM_NODE_IN; k++)
            if (g->nodes[i].in[k] >= 0 && g->nodes[i].in[k] < (int)g->nnodes) edges[ne++] = (GlEdge){ g->nodes[i].in[k], (int)i };
    }
    if (!graph_layout(nodes, (int)g->nnodes, edges, ne, only, GL_FLOW_RIGHT, igGetFontSize() * 4.0f, igGetFontSize() * 1.6f)) return false;
    for (uint32_t i = 0; i < g->nnodes; i++) g->nodes[i].x = roundf(nodes[i].x), g->nodes[i].y = roundf(nodes[i].y);
    return true;
}

static bool pose_canvas(BpAsset* a, AnimGraph* g, ImVec2 o, ImVec2 sz) {
    bool ch = false;
    GraphView* v = &G.views[0];
    ImDrawList* dl = igGetWindowDrawList();
    ImGuiIO* io = igGetIO_Nil();
    float zm = v->zoom, fs = igGetFontSize() * zm;
    ImVec2 mp = io->MousePos;
    bool hovered = igIsItemHovered(0);
    if (G.drag_node >= 0 && G.drag_node < (int)g->nnodes && igIsMouseDown_Nil(ImGuiMouseButton_Left) && (io->MouseDelta.x != 0.0f || io->MouseDelta.y != 0.0f)) {
        for (uint32_t i = 0; i < g->nnodes; i++) // (the selection moves together)
            if ((int)i == G.drag_node || (sel_has(&G.sel, G.drag_node) && sel_has(&G.sel, (int)i)))
                g->nodes[i].x += io->MouseDelta.x / zm, g->nodes[i].y += io->MouseDelta.y / zm;
        ch = true;
    }
    { // (2026-11-23) Blender's edge pan: a node or a link dragged near the edge scrolls the view (the node stays under the mouse)
        bool node = G.drag_node >= 0 && G.drag_node < (int)g->nnodes && igIsMouseDown_Nil(ImGuiMouseButton_Left);
        bool wire = (G.link_from >= 0 || G.link_to >= 0) && igIsMouseDown_Nil(ImGuiMouseButton_Left);
        float es[2];
        if ((node || wire) && gv_edge_scroll(o, sz, es)) {
            v->pan[0] -= es[0] / zm, v->pan[1] -= es[1] / zm;
            for (uint32_t i = 0; node && i < g->nnodes; i++)
                if ((int)i == G.drag_node || (sel_has(&G.sel, G.drag_node) && sel_has(&G.sel, (int)i))) g->nodes[i].x += es[0] / zm, g->nodes[i].y += es[1] / zm;
        }
    }
    // the links: a curve from each input to the node feeding it
    for (uint32_t i = 0; i < g->nnodes; i++)
        for (int k = 0; k < anim_node_info(g->nodes[i].kind)->inputs; k++) {
            int s = g->nodes[i].in[k];
            if (s < 0 || s >= (int)g->nnodes || (G.link_to == (int)i && G.link_pin == k)) continue;
            ImVec2 p0 = pin_out(v, o, &g->nodes[s]), p1 = pin_in(v, o, &g->nodes[i], k);
            graph_wire(dl, p0, p1, 0xFFE8E8E8u, fmaxf(zm, 0.6f), false, -1); // (Editor Preferences > Graph Style)
        }
    // the nodes
    int hover = hovered ? node_at(g, v, o, mp) : -1, hover_out = -1, hover_in = -1, hover_pin = -1;
    for (uint32_t i = 0; i < g->nnodes; i++) {
        const AnimNode* n = &g->nodes[i];
        const AnimNodeInfo* ni = anim_node_info(n->kind);
        ImVec2 p = gv_screen(v, o, n->x, n->y);
        float w = node_wn(n, zm), h = node_h(n, zm);
        bool sel = (int)i == G.node || sel_has(&G.sel, (int)i);
        if (n->kind == AN_REROUTE) { // a knot: a small pill, its two pins
            ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + h }, 0xF0303030u, h * 0.5f, 0);
            ImDrawList_AddRect(dl, p, (ImVec2){ p.x + w, p.y + h }, sel ? 0xFF28A0FFu : (int)i == hover ? 0xFF909090u : 0xFF5A5A5Au, h * 0.5f, sel ? 2.0f : 1.0f, 0);
            ImVec2 qi = pin_in(v, o, n, 0), qo = pin_out(v, o, n);
            bool hi = fabsf(mp.x - qi.x) < 8 * zm + 2 && fabsf(mp.y - qi.y) < 8 * zm + 2, ho = fabsf(mp.x - qo.x) < 8 * zm + 2 && fabsf(mp.y - qo.y) < 8 * zm + 2;
            if (hi && hovered) hover_in = (int)i, hover_pin = 0;
            if (ho && hovered && !hi) hover_out = (int)i;
            ImDrawList_AddCircleFilled(dl, qi, 4.5f * zm, n->in[0] >= 0 ? 0xFFE8E8E8u : 0xFF262626u, 12);
            ImDrawList_AddCircle(dl, qi, 4.5f * zm, hi ? 0xFF28A0FFu : 0xFFE8E8E8u, 12, 1.5f);
            ImDrawList_AddCircleFilled(dl, qo, 4.5f * zm, 0xFFE8E8E8u, 12);
            if (ho) ImDrawList_AddCircle(dl, qo, 7.0f * zm, 0xFF28A0FFu, 12, 1.5f);
            continue;
        }
        ImDrawList_AddRectFilled(dl, (ImVec2){ p.x + 3, p.y + 4 }, (ImVec2){ p.x + w + 3, p.y + h + 4 }, 0x60000000u, 7.0f * zm, 0);
        ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + h }, 0xF0262626u, 7.0f * zm, 0);
        ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + fs * 1.7f }, ni->color, 7.0f * zm, ImDrawFlags_RoundCornersTop);
        ImDrawList_AddRect(dl, p, (ImVec2){ p.x + w, p.y + h }, sel ? 0xFF28A0FFu : (int)i == hover ? 0xFF909090u : 0xFF4A4A4Au, 7.0f * zm,
                           sel ? 2.5f : 1.2f, 0);
        ztext(dl, (ImVec2){ p.x + 8 * zm, p.y + fs * 0.35f }, 0xFFFFFFFFu, TR(ni->name), zm);
        char sub[128];
        node_subtitle(g, n, sub, sizeof sub);
        if (sub[0]) ztext(dl, (ImVec2){ p.x + 8 * zm, p.y + fs * 1.85f }, 0xFFA8C8E8u, sub, zm);
        for (int k = 0; k < ni->inputs; k++) { // pose inputs: white pins, their label
            ImVec2 q = pin_in(v, o, n, k);
            bool hp = fabsf(mp.x - q.x) < 9 * zm + 2 && fabsf(mp.y - q.y) < 9 * zm + 2;
            if (hp && hovered) hover_in = (int)i, hover_pin = k;
            ImDrawList_AddCircleFilled(dl, q, 5.5f * zm, n->in[k] >= 0 ? 0xFFE8E8E8u : 0xFF262626u, 12);
            ImDrawList_AddCircle(dl, q, 5.5f * zm, hp ? 0xFF28A0FFu : 0xFFE8E8E8u, 12, 1.5f);
            ztext(dl, (ImVec2){ q.x + 10 * zm, q.y - fs * 0.5f }, 0xFFD0D0D0u, TR(ni->pins[k]), zm);
        }
        if (n->kind != AN_OUTPUT) { // the output pin
            ImVec2 q = pin_out(v, o, n);
            bool hp = fabsf(mp.x - q.x) < 9 * zm + 2 && fabsf(mp.y - q.y) < 9 * zm + 2;
            if (hp && hovered) hover_out = (int)i;
            ImDrawList_AddCircleFilled(dl, q, 5.5f * zm, 0xFFE8E8E8u, 12);
            if (hp) ImDrawList_AddCircle(dl, q, 8.0f * zm, 0xFF28A0FFu, 12, 1.5f);
        }
    }
    // dragging a link: from an output, or pulled off an input
    int from = G.link_from >= 0 ? G.link_from : G.link_to >= 0 ? g->nodes[G.link_to].in[G.link_pin] : -1;
    if (G.link_from >= 0 || G.link_to >= 0) {
        ImVec2 p0 = G.link_from >= 0 ? pin_out(v, o, &g->nodes[G.link_from]) : (from >= 0 ? pin_out(v, o, &g->nodes[from]) : mp);
        ImDrawList_AddBezierCubic(dl, p0, (ImVec2){ p0.x + 60 * zm, p0.y }, (ImVec2){ mp.x - 60 * zm, mp.y }, mp, 0xFF28A0FFu, 2.5f, 0);
        if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
            if (G.link_to >= 0) g->nodes[G.link_to].in[G.link_pin] = -1, ch = true; // (pulled off)
            if (from >= 0 && hover_in >= 0 && hover_in != from) g->nodes[hover_in].in[hover_pin] = (int16_t)from, ch = true;
            else if (from >= 0 && hover_in < 0 && hover < 0) { // dropped on the canvas: what to plug it into (a new node's input)
                G.menu_at = mp;
                igOpenPopup_Str("ag_menu_link", 0);
                G.link_pin = from; // (reused: the source for the new node)
            }
            G.link_from = G.link_to = -1;
        }
    }
    if (igBeginPopup("ag_menu_link", 0)) { // a new node fed by the dragged output
        int before = (int)g->nnodes;
        ch |= add_menu_nodes(g, o, -1, 0);
        if ((int)g->nnodes > before && anim_node_info(g->nodes[before].kind)->inputs > 0) g->nodes[before].in[0] = (int16_t)G.link_pin;
        igEndPopup();
    }
    // clicks: a pin starts a link; a node selects (and drags); a double click opens a State Machine
    if (hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
        if (hover_out >= 0) G.link_from = hover_out;
        else if (hover_in >= 0 && io->KeyAlt) g->nodes[hover_in].in[hover_pin] = -1, ch = true; // (Alt+click breaks it, Unreal's)
        else if (hover_in >= 0 && g->nodes[hover_in].in[hover_pin] >= 0) G.link_to = hover_in, G.link_pin = hover_pin;
        else if (hover_in >= 0) G.link_to = -1;
        else if (hover >= 0) { // a node: Ctrl toggles it, Shift adds it, a click on an unselected one selects it alone
            if (io->KeyCtrl) sel_set(&G.sel, hover, !sel_has(&G.sel, hover));
            else if (io->KeyShift || sel_has(&G.sel, hover)) sel_set(&G.sel, hover, true);
            else G.sel = 0, sel_set(&G.sel, hover, true);
            G.node = hover, G.drag_node = hover, E.state = E.trans = -1, G.any_sel = false;
        } else { // the empty canvas: a selection box
            if (!io->KeyCtrl && !io->KeyShift) G.sel = 0;
            G.node = -1, G.box = true, G.box_from = mp, E.state = E.trans = -1, G.any_sel = false;
        }
    }
    if (hovered && hover >= 0 && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left) && g->nodes[hover].kind == AN_STATE_MACHINE)
        G.machine = g->nodes[hover].machine < g->nmachines ? g->nodes[hover].machine : -1, G.node = -1, G.drag_node = -1, G.fit = true;
    if (hovered && hover >= 0 && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left) &&
        (g->nodes[hover].kind == AN_BLEND_SPACE_1D || g->nodes[hover].kind == AN_BLEND_SPACE_2D))
        bs_open(a, hover, -1); // Unreal's: a Blend Space opens in its own editor
    if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) G.drag_node = -1;
    ImVec2 b0, b1;
    if (box_select(dl, &b0, &b1)) // the nodes the box touches join the selection
        for (uint32_t i = 0; i < g->nnodes; i++) {
            ImVec2 p = gv_screen(v, o, g->nodes[i].x, g->nodes[i].y);
            if (p.x < b1.x && p.x + node_wn(&g->nodes[i], zm) > b0.x && p.y < b1.y && p.y + node_h(&g->nodes[i], zm) > b0.y) {
                sel_set(&G.sel, (int)i, true);
                if (G.node < 0) G.node = (int)i;
            }
        }
    if (igIsWindowFocused(0) && !io->WantTextInput && !io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_Q, false)) ch |= organize_pose(g); // (2026-11-23)
    if (igIsWindowFocused(0) && !io->WantTextInput && (igIsKeyPressed_Bool(ImGuiKey_Delete, false) || igIsKeyPressed_Bool(ImGuiKey_X, false))) {
        if (G.node >= 0) sel_set(&G.sel, G.node, true);
        for (int i = (int)g->nnodes - 1; i >= 0; i--) // (from the last: the indices below stay)
            if (sel_has(&G.sel, i) && g->nodes[i].kind != AN_OUTPUT) delete_node(g, i), ch = true;
        G.sel = 0, G.node = -1;
    }
    // the right click: on a node its menu, on the canvas the nodes by category
    if (hovered && gv_right_click()) {
        G.menu_at = mp;
        if (hover >= 0) G.node = hover, igOpenPopup_Str("ag_node_menu", 0);
        else igOpenPopup_Str("ag_menu", 0);
    }
    if (igBeginPopup("ag_menu", 0)) {
        if (ui_menu_item(ICON_SORT, TR("Organize Nodes"), "Q", false, g->nnodes > 1)) ch |= organize_pose(g);
        igSeparator();
        igTextDisabled("%s", TR("All Actions for this Graph"));
        ch |= add_menu_nodes(g, o, -1, 0);
        igEndPopup();
    }
    if (igBeginPopup("ag_node_menu", 0)) {
        if (G.node >= 0 && G.node < (int)g->nnodes) {
            AnimNode* n = &g->nodes[G.node];
            if (n->kind == AN_STATE_MACHINE && ui_menu_item(ICON_OPEN, TR("Open State Machine"), NULL, false, true))
                G.machine = n->machine < g->nmachines ? n->machine : -1, G.node = -1, G.fit = true;
            else if (n->kind != AN_OUTPUT && ui_menu_item(ICON_DUPLICATE, TR("Duplicate"), "Ctrl+D", false, g->nnodes < ANIM_MAX_NODES)) {
                g->nodes[g->nnodes] = *n;
                g->nodes[g->nnodes].x += 40.0f, g->nodes[g->nnodes].y += 40.0f;
                G.node = (int)g->nnodes++, ch = true;
            }
            if (G.node >= 0 && g->nodes[G.node].kind != AN_OUTPUT && ui_menu_item(ICON_DELETE, TR("Delete"), "Del", false, true))
                delete_node(g, G.node), G.node = -1, ch = true;
            if (G.node >= 0 && ui_menu_item(ICON_DELETE, TR("Break All Pin Links"), NULL, false, true)) {
                for (int k = 0; k < ANIM_NODE_IN; k++) g->nodes[G.node].in[k] = -1;
                for (uint32_t i = 0; i < g->nnodes; i++)
                    for (int k = 0; k < ANIM_NODE_IN; k++)
                        if (g->nodes[i].in[k] == G.node) g->nodes[i].in[k] = -1;
                ch = true;
            }
        }
        igEndPopup();
    }
    if (G.node >= 0 && igIsWindowFocused(0) && io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_D, false) && G.node < (int)g->nnodes &&
        g->nodes[G.node].kind != AN_OUTPUT && g->nnodes < ANIM_MAX_NODES) {
        g->nodes[g->nnodes] = g->nodes[G.node];
        g->nodes[g->nnodes].x += 40.0f, g->nodes[g->nnodes].y += 40.0f;
        G.node = (int)g->nnodes++, ch = true;
    }
    (void)a;
    (void)sz;
    return ch;
}

// --- A State Machine's states and transitions ---

static ImVec2 any_box(const GraphView* v, ImVec2 o) { return gv_screen(v, o, -320.0f, -140.0f); }

static bool machine_canvas(BpAsset* a, AnimGraph* g, int mi, ImVec2 o) {
    bool ch = false;
    GraphView* v = &G.views[1 + mi];
    ImDrawList* dl = igGetWindowDrawList();
    ImGuiIO* io = igGetIO_Nil();
    float zm = v->zoom, bw = igGetFontSize() * 9.0f * zm, bh = igGetFontSize() * 2.6f * zm;
    if (igIsItemActive() && igIsMouseDown_Nil(ImGuiMouseButton_Left) && E.drag_state >= 0 && E.drag_state < (int)g->nstates &&
        (io->MouseDelta.x != 0.0f || io->MouseDelta.y != 0.0f)) {
        for (uint32_t i = 0; i < g->nstates; i++) // (the selected states move together)
            if ((int)i == E.drag_state || (sel_has(G.sel_states, E.drag_state) && sel_has(G.sel_states, (int)i)))
                g->states[i].x += io->MouseDelta.x / zm, g->states[i].y += io->MouseDelta.y / zm;
        ch = true;
    }
    int hover = -1;
    for (uint32_t i = 0; i < g->nstates; i++) {
        if (g->states[i].machine != mi) continue;
        ImVec2 p = gv_screen(v, o, g->states[i].x, g->states[i].y);
        if (io->MousePos.x >= p.x && io->MousePos.x < p.x + bw && io->MousePos.y >= p.y && io->MousePos.y < p.y + bh) hover = (int)i;
    }
    ImVec2 ab = any_box(v, o);
    bool hover_any = io->MousePos.x >= ab.x && io->MousePos.x < ab.x + bw && io->MousePos.y >= ab.y && io->MousePos.y < ab.y + bh;
    // the transitions: arrows between the boxes (two ways: offset to each side); Any State's from its box
    int hover_t = -1;
    for (uint32_t i = 0; i < g->ntrans; i++) {
        const AnimTransition* t = &g->trans[i];
        if (t->to >= g->nstates || g->states[t->to].machine != mi) continue;
        ImVec2 p0 = t->from == ANIM_ANY_STATE ? ab : gv_screen(v, o, g->states[t->from].x, g->states[t->from].y);
        ImVec2 p1 = gv_screen(v, o, g->states[t->to].x, g->states[t->to].y);
        float x0 = p0.x + bw * 0.5f, y0 = p0.y + bh * 0.5f, x1 = p1.x + bw * 0.5f, y1 = p1.y + bh * 0.5f;
        float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
        if (l < 1.0f) continue;
        float nx = -dy / l * 7.0f, ny = dx / l * 7.0f;
        x0 += nx, y0 += ny, x1 += nx, y1 += ny;
        float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
        bool sel = (int)i == E.trans;
        uint32_t c = sel ? 0xFF28A0FFu : t->from == ANIM_ANY_STATE ? 0xFF7AC8E8u : 0xFFD0D0D0u;
        ImDrawList_AddLine(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, c, sel ? 3.0f : 2.0f);
        float ux = dx / l, uy = dy / l;
        ImDrawList_AddTriangleFilled(dl, (ImVec2){ mx + ux * 8, my + uy * 8 }, (ImVec2){ mx - uy * 6, my + ux * 6 }, (ImVec2){ mx + uy * 6, my - ux * 6 }, c);
        if (t->nmore || t->priority) ImDrawList_AddCircleFilled(dl, (ImVec2){ mx - uy * 11, my + ux * 11 }, 3.0f, c, 8); // (more rules)
        if (fabsf(io->MousePos.x - mx) < 10 && fabsf(io->MousePos.y - my) < 10) hover_t = (int)i;
    }
    // Any State (Unreal's State Alias "any"), the entry arrow, the boxes
    ImDrawList_AddRectFilled(dl, ab, (ImVec2){ ab.x + bw, ab.y + bh }, 0xFF2A3A44u, 6.0f, 0);
    ImDrawList_AddRect(dl, ab, (ImVec2){ ab.x + bw, ab.y + bh }, G.any_sel ? 0xFF28A0FFu : hover_any ? 0xFFA0A0A0u : 0xFF5A6A74u, 6.0f, 1.5f, 0);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ ab.x + 10, ab.y + 5 }, 0xFFFFFFFFu, TR("Any State"), NULL);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ ab.x + 10, ab.y + 5 + igGetFontSize() }, 0xFF9AD0FFu, TR("(state alias)"), NULL);
    for (uint32_t i = 0; i < g->nstates; i++) {
        const AnimState* s = &g->states[i];
        if (s->machine != mi) continue;
        ImVec2 p = gv_screen(v, o, s->x, s->y);
        bool sel = (int)i == E.state || sel_has(G.sel_states, (int)i);
        ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + bw, p.y + bh }, 0xFF3A3A3Au, 6.0f, 0);
        ImDrawList_AddRect(dl, p, (ImVec2){ p.x + bw, p.y + bh }, sel ? 0xFF28A0FFu : (int)i == hover ? 0xFFA0A0A0u : 0xFF5A5A5Au, 6.0f, sel ? 2.5f : 1.5f, 0);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + 10, p.y + 5 }, 0xFFFFFFFFu, s->name, NULL);
        const char* sub = s->kind == ANIM_STATE_SEQUENCE ? s->clip : s->param;
        char line[96];
        snprintf(line, sizeof line, "%s%s", s->kind == ANIM_STATE_BLEND1D ? "Blend: " : s->kind == ANIM_STATE_BLEND2D ? "Blend 2D: " : s->kind == ANIM_STATE_BLEND_BOOL ? "Bool: "
                                            : s->kind == ANIM_STATE_MOTION_MATCH ? "Motion Matching: " : "", sub);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + 10, p.y + 5 + igGetFontSize() }, 0xFF9AD0FFu, line, NULL);
        if (i == g->machine_entry[mi]) {
            ImDrawList_AddLine(dl, (ImVec2){ p.x - 40, p.y + bh * 0.5f }, (ImVec2){ p.x - 4, p.y + bh * 0.5f }, 0xFF60D060u, 2.0f);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x - 44, p.y - 4 }, 0xFF60D060u, "Entry", NULL);
        }
    }
    // a state's edge (Unreal's): pressed there, a new transition follows the mouse to the state it is dropped on
    float edge = 7.0f * fmaxf(zm, 0.6f);
    int hover_edge = -1;
    if (hover >= 0) {
        ImVec2 p = gv_screen(v, o, g->states[hover].x, g->states[hover].y);
        ImVec2 mp = io->MousePos;
        if (mp.x < p.x + edge || mp.x > p.x + bw - edge || mp.y < p.y + edge || mp.y > p.y + bh - edge) hover_edge = hover;
    } else if (hover_any) {
        ImVec2 mp = io->MousePos;
        if (mp.x < ab.x + edge || mp.x > ab.x + bw - edge || mp.y < ab.y + edge || mp.y > ab.y + bh - edge) hover_edge = ANIM_ANY_STATE;
    }
    if (hover_edge >= 0 && G.link_state < 0) { // the edge lit: where a drag starts a transition
        ImVec2 p = hover_edge == ANIM_ANY_STATE ? ab : gv_screen(v, o, g->states[hover_edge].x, g->states[hover_edge].y);
        ImDrawList_AddRect(dl, p, (ImVec2){ p.x + bw, p.y + bh }, 0xFF28A0FFu, 6.0f, 3.0f, 0);
    }
    if (igIsItemClicked(ImGuiMouseButton_Left)) {
        if (hover_edge >= 0 && !io->KeyCtrl && !io->KeyShift) G.link_state = hover_edge, E.drag_state = -1;
        else if (hover >= 0) { // Ctrl toggles, Shift adds, a click on an unselected state selects it alone
            if (io->KeyCtrl) sel_set(G.sel_states, hover, !sel_has(G.sel_states, hover));
            else if (io->KeyShift || sel_has(G.sel_states, hover)) sel_set(G.sel_states, hover, true);
            else G.sel_states[0] = G.sel_states[1] = 0, sel_set(G.sel_states, hover, true);
            E.state = hover, E.trans = -1, E.drag_state = hover, G.any_sel = false, G.node = -1;
        } else {
            E.state = -1, E.trans = !hover_any ? hover_t : -1, E.drag_state = -1, G.any_sel = hover_any, G.node = -1;
            if (!hover_any && hover_t < 0) { // the empty canvas: a selection box
                if (!io->KeyCtrl && !io->KeyShift) G.sel_states[0] = G.sel_states[1] = 0;
                G.box = true, G.box_from = io->MousePos;
            }
        }
    }
    if (G.link_state >= 0) { // the new transition's arrow to the mouse
        ImVec2 p = G.link_state == ANIM_ANY_STATE ? ab : gv_screen(v, o, g->states[G.link_state].x, g->states[G.link_state].y);
        ImVec2 c = { p.x + bw * 0.5f, p.y + bh * 0.5f }, mp = io->MousePos;
        ImDrawList_AddLine(dl, c, mp, 0xFF28A0FFu, 2.5f);
        float dx = mp.x - c.x, dy = mp.y - c.y, l = sqrtf(dx * dx + dy * dy);
        if (l > 1.0f) {
            float ux = dx / l, uy = dy / l;
            ImDrawList_AddTriangleFilled(dl, mp, (ImVec2){ mp.x - ux * 12 - uy * 6, mp.y - uy * 12 + ux * 6 }, (ImVec2){ mp.x - ux * 12 + uy * 6, mp.y - uy * 12 - ux * 6 }, 0xFF28A0FFu);
        }
        if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
            if (hover >= 0 && hover != G.link_state && g->ntrans < ANIM_MAX_TRANS) {
                AnimTransition* t = &g->trans[g->ntrans];
                memset(t, 0, sizeof *t);
                t->from = (uint8_t)G.link_state, t->to = (uint8_t)hover, t->op = ANIM_OP_TRUE, t->blend = 0.2f;
                E.trans = (int)g->ntrans++, E.state = -1, G.any_sel = false, ch = true;
            }
            G.link_state = -1;
        }
    }
    if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) E.drag_state = -1;
    if (igIsItemHovered(0) && hover < 0 && hover_t >= 0 && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) tw_open(a, hover_t);
    if (igIsItemHovered(0) && hover >= 0 && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left) &&
        (g->states[hover].kind == ANIM_STATE_BLEND1D || g->states[hover].kind == ANIM_STATE_BLEND2D))
        bs_open(a, -1, hover); // a blend state's Blend Space in its own editor
    ImVec2 b0, b1;
    if (box_select(dl, &b0, &b1))
        for (uint32_t i = 0; i < g->nstates; i++) {
            if (g->states[i].machine != mi) continue;
            ImVec2 p = gv_screen(v, o, g->states[i].x, g->states[i].y);
            if (p.x < b1.x && p.x + bw > b0.x && p.y < b1.y && p.y + bh > b0.y) {
                sel_set(G.sel_states, (int)i, true);
                if (E.state < 0) E.state = (int)i;
            }
        }
    if (igIsItemHovered(0) && gv_right_click()) G.menu_at = io->MousePos, igOpenPopup_Str("ag_state_menu", 0);
    if (igBeginPopup("ag_state_menu", 0)) {
        bool room = g->nstates < ANIM_MAX_STATES;
        int kind = -1;
        if (ui_menu_item(ICON_PLUS, TR("Add State"), NULL, false, room)) kind = ANIM_STATE_SEQUENCE;
        // Motion Matching ready to play: the Speed variable, the skeleton's looping clips as its database
        if (ui_menu_item(ICON_PLUS, TR("Add Motion Matching State"), NULL, false, room)) kind = ANIM_STATE_MOTION_MATCH;
        if (kind >= 0) {
            AnimState* s = &g->states[g->nstates];
            memset(s, 0, sizeof *s);
            snprintf(s->name, ANIM_NAME, kind == ANIM_STATE_MOTION_MATCH ? "MotionMatching%u" : "State%u", g->nstates);
            s->kind = (uint8_t)kind, s->loop = true, s->rate = 1.0f, s->machine = (uint8_t)mi;
            if (kind == ANIM_STATE_MOTION_MATCH) {
                for (uint32_t i = 0; i < a->nvars; i++)
                    if (a->vars[i].type == BPT_FLOAT && !strcmp(a->vars[i].name, "Speed")) snprintf(s->param, ANIM_NAME, "Speed");
                mm_fill(g, &s->nsamples, s->samples);
            }
            gv_graph(v, o, G.menu_at, &s->x, &s->y);
            E.state = (int)g->nstates++, E.trans = -1, ch = true;
        }
        igEndPopup();
    }
    return ch;
}

// --- The Details: a node's, a state's, a transition's, the graph's ---

static bool node_details(BpAsset* a, AnimGraph* g, AnimNode* n) {
    bool ch = false;
    const SkelMesh* sk = anim_mesh_name(g->skeleton);
    igSeparatorText(TR(anim_node_info(n->kind)->name));
    switch (n->kind) {
    case AN_OUTPUT: hint(TR("The final pose: what the character shows.")); break;
    case AN_STATE_MACHINE:
        if (n->machine < g->nmachines) {
            ch |= igInputText(PL(TR("Name")), g->machines[n->machine], ANIM_NAME, 0, NULL, NULL);
            if (igButton(TR("Open State Machine"), (ImVec2){ -1, 0 })) G.machine = n->machine, G.node = -1, G.fit = true;
        }
        if (igBeginCombo(PL(TR("State Machine")), n->machine < g->nmachines ? g->machines[n->machine] : "-", 0)) {
            for (uint32_t m = 0; m < g->nmachines; m++)
                if (igSelectable_Bool(g->machines[m], n->machine == m, 0, (ImVec2){ 0, 0 })) n->machine = (uint8_t)m, ch = true;
            igEndCombo();
        }
        break;
    case AN_SEQUENCE: {
        ch |= clip_combo(PL(TR("Sequence")), n->clip, g->skeleton);
        ch |= igDragFloat(PL(TR("Play Rate")), &n->f[0], 0.01f, 0.05f, 5.0f, "%.2f", 0);
        ch |= igDragFloat(PL(TR("Start Position")), &n->f[1], 0.01f, 0.0f, 600.0f, "%.2f s", 0);
        bool loop = n->flags & 1;
        if (igCheckbox(PL(TR("Loop Animation")), &loop)) n->flags = (uint8_t)((n->flags & ~1u) | (loop ? 1u : 0u)), ch = true;
        break;
    }
    case AN_BLEND_SPACE_1D: case AN_BLEND_SPACE_2D: {
        bool two = n->kind == AN_BLEND_SPACE_2D;
        ch |= var_combo(PL(two ? TR("X (Horizontal Axis)") : TR("Parameter")), n->var[0], a, BPT_FLOAT);
        if (two) ch |= var_combo(PL(TR("Y (Vertical Axis)")), n->var[1], a, BPT_FLOAT);
        if (igButton(TR("Open Blend Space Editor"), (ImVec2){ -FLT_MIN, 0 })) bs_open(a, G.node, -1);
        ch |= samples_edit(&n->nsamples, n->samples, n->sample_at, n->sample_y, g->skeleton, two ? 2 : 1);
        ch |= igDragFloat(PL(TR("Play Rate")), &n->f[0], 0.01f, 0.05f, 5.0f, "%.2f", 0);
        if (two) blend2d_plot(n->nsamples, n->sample_at, n->sample_y, n->samples);
        break;
    }
    case AN_MOTION_MATCH:
        ch |= var_combo(PL(TR("Speed")), n->var[0], a, BPT_FLOAT);
        ch |= var_combo(PL(TR("Direction (optional)")), n->var[1], a, BPT_FLOAT);
        igSeparatorText(TR("Database"));
        ch |= samples_edit(&n->nsamples, n->samples, n->sample_at, n->sample_y, g->skeleton, 0);
        if (igButton(TR("Open Motion Matching Editor"), (ImVec2){ -1, 0 })) mm_editor_open(a, -1, (int)(n - g->nodes));
        break;
    case AN_BLEND_BOOL:
        ch |= var_combo(PL(TR("Active Value")), n->var[0], a, BPT_BOOL);
        ch |= igDragFloat(PL(TR("Blend Time")), &n->f[0], 0.01f, 0.0f, 3.0f, "%.2f s", 0);
        break;
    case AN_BLEND_INT:
        ch |= var_combo(PL(TR("Active Child Index")), n->var[0], a, BPT_INT);
        ch |= igDragFloat(PL(TR("Blend Time")), &n->f[0], 0.01f, 0.0f, 3.0f, "%.2f s", 0);
        break;
    case AN_BLEND:
        ch |= var_combo(PL(TR("Alpha")), n->var[0], a, BPT_FLOAT);
        if (!n->var[0][0]) ch |= igSliderFloat(PL(TR("Alpha (constant)")), &n->f[0], 0.0f, 1.0f, "%.2f", 0);
        break;
    case AN_LAYERED_BLEND:
        ch |= bone_combo(PL(TR("Branch Filter (bone)")), n->bone[0], sk);
        ch |= igSliderFloat(PL(TR("Blend Weight")), &n->f[0], 0.0f, 1.0f, "%.2f", 0);
        hint(TR("The Blend Pose from this bone down its branch, over the Base Pose."));
        break;
    case AN_APPLY_ADDITIVE:
        ch |= clip_combo(PL(TR("Additive Animation")), n->clip, g->skeleton);
        ch |= var_combo(PL(TR("Alpha")), n->var[0], a, BPT_FLOAT);
        if (!n->var[0][0]) ch |= igSliderFloat(PL(TR("Alpha (constant)")), &n->f[0], 0.0f, 1.0f, "%.2f", 0);
        hint(TR("The animation's motion from its first frame, added on the base (breathing, recoil)."));
        break;
    case AN_SLOT:
        if (igBeginCombo(PL(TR("Slot Name")), n->name, 0)) {
            static const char* const slots[2] = { "DefaultSlot", "UpperBody" };
            for (int k = 0; k < 2; k++)
                if (igSelectable_Bool(slots[k], !strcmp(n->name, slots[k]), 0, (ImVec2){ 0, 0 })) snprintf(n->name, ANIM_NAME, "%s", slots[k]), ch = true;
            igEndCombo();
        }
        hint(TR("A montage of this slot plays over the Source (UpperBody: from the graph's upper bone up)."));
        break;
    case AN_TWO_BONE_IK:
        ch |= bone_combo(PL(TR("IK Bone")), n->bone[0], sk);
        ch |= bone_combo(PL(TR("Effector Target (bone)")), n->bone[1], sk);
        ch |= igDragFloat3(PL(TR("Effector Offset")), n->f, 0.5f, -500.0f, 500.0f, "%.1f", 0);
        ch |= igSliderFloat(PL(TR("Alpha")), &n->f[3], 0.0f, 1.0f, "%.2f", 0);
        hint(TR("The IK Bone reaches the target bone's place plus the offset (cm, the actor's forward / right / up)."));
        break;
    case AN_LEG_IK:
        ch |= bone_combo(PL(TR("Left Foot")), n->bone[0], sk);
        ch |= bone_combo(PL(TR("Right Foot")), n->bone[1], sk);
        ch |= igDragFloat(PL(TR("Reach")), &n->f[0], 0.5f, 5.0f, 150.0f, "%.0f cm", 0);
        hint(TR("While playing: each foot on the ground under it, the pelvis down for the lower one."));
        break;
    case AN_AIM_OFFSET:
        ch |= var_combo(PL(TR("Yaw (deg)")), n->var[0], a, BPT_FLOAT);
        ch |= var_combo(PL(TR("Pitch (deg)")), n->var[1], a, BPT_FLOAT);
        for (int k = 0; k < 4; k++) {
            igPushID_Int(100 + k);
            igSetNextItemWidth(igGetFontSize() * 8);
            ch |= bone_combo("##aimb", n->bone[k], sk);
            igSameLine(0, 4);
            igSetNextItemWidth(igGetFontSize() * 5);
            ch |= igSliderFloat("##share", &n->f[k], 0.0f, 1.0f, TR("share %.2f"), 0);
            igPopID();
        }
        break;
    case AN_ORIENT_WARP: { // (2026-11-23)
        ch |= bone_combo(PL(TR("Pelvis")), n->bone[0], sk);
        for (int k = 1; k < 4; k++) {
            igPushID_Int(300 + k);
            ch |= bone_combo(PL(TR("Spine Bone")), n->bone[k], sk);
            igPopID();
        }
        ch |= igSliderFloat(PL(TR("Max Angle")), &n->f[0], 0.0f, 180.0f, "%.0f deg", 0);
        ch |= igSliderFloat(PL(TR("Alpha")), &n->f[1], 0.0f, 1.0f, "%.2f", 0);
        ch |= igSliderFloat(PL(TR("Turn Speed")), &n->f[2], 1.0f, 30.0f, "%.1f", 0);
        bool four = n->flags & 1;
        if (igCheckbox(PL(TR("Four Directions")), &four)) n->flags = (uint8_t)((n->flags & ~1) | (four ? 1 : 0)), ch = true;
        ch |= var_combo(PL(TR("Angle Variable")), n->var[0], a, BPT_FLOAT);
        hint(TR("Turns the legs to where the Character moves and the spine back, so the chest keeps facing ahead (strafing with one forward walk). Four Directions: only the rest past the nearest forward, back, left or right clip."));
        break;
    }
    case AN_STRIDE_WARP:
        ch |= bone_combo(PL(TR("Left Foot")), n->bone[0], sk);
        ch |= bone_combo(PL(TR("Right Foot")), n->bone[1], sk);
        ch |= bone_combo(PL(TR("Pelvis")), n->bone[2], sk);
        ch |= igDragFloatRange2(PL(TR("Stride Scale")), &n->f[0], &n->f[1], 0.01f, 0.1f, 4.0f, "%.2f", "%.2f", 0);
        ch |= igSliderFloat(PL(TR("Alpha")), &n->f[2], 0.0f, 1.0f, "%.2f", 0);
        ch |= igSliderFloat(PL(TR("Pelvis Drop")), &n->f[3], 0.0f, 20.0f, "%.1f cm", 0);
        ch |= var_combo(PL(TR("Scale Variable")), n->var[0], a, BPT_FLOAT);
        hint(TR("Stretches or shortens the steps to the Character's speed against the animation's own (read from its planted foot), so the feet don't slide when it walks faster or slower than the clip."));
        break;
    case AN_LEAN:
        ch |= igSliderFloat(PL(TR("Lean Amount")), &n->f[0], 0.0f, 1.0f, "%.2f", 0);
        for (int k = 0; k < 3; k++) {
            igPushID_Int(200 + k);
            ch |= bone_combo(PL(TR("Bone")), n->bone[k], sk);
            igPopID();
        }
        hint(TR("Tilts into turns and with speeding up (along these spine bones)."));
        break;
    case AN_MODIFY_BONE:
        ch |= bone_combo(PL(TR("Bone to Modify")), n->bone[0], sk);
        ch |= igDragFloat3(PL(TR("Rotation (roll, pitch, yaw)")), n->f, 0.5f, -180.0f, 180.0f, "%.1f", 0);
        ch |= igDragFloat3(PL(TR("Translation")), n->f + 3, 0.5f, -500.0f, 500.0f, "%.1f", 0);
        ch |= var_combo(PL(TR("Alpha")), n->var[0], a, BPT_FLOAT);
        if (!n->var[0][0]) ch |= igSliderFloat(PL(TR("Alpha (constant)")), &n->f[6], 0.0f, 1.0f, "%.2f", 0);
        break;
    case AN_LOOK_AT:
        ch |= bone_combo(PL(TR("Bone to Modify")), n->bone[0], sk);
        ch |= igSliderFloat(PL(TR("Look At Clamp")), &n->f[0], 0.0f, 120.0f, "%.0f deg", 0);
        ch |= igSliderFloat(PL(TR("Alpha")), &n->f[1], 0.0f, 1.0f, "%.2f", 0);
        hint(TR("Turns toward the camera (the player's view)."));
        break;
    case AN_SAVE_CACHE: ch |= igInputText(PL(TR("Cache Name")), n->name, ANIM_NAME, 0, NULL, NULL); break;
    case AN_USE_CACHE:
        if (igBeginCombo(PL(TR("Cached Pose")), n->name, 0)) {
            for (uint32_t i = 0; i < g->nnodes; i++)
                if (g->nodes[i].kind == AN_SAVE_CACHE && igSelectable_Bool(g->nodes[i].name, !strcmp(n->name, g->nodes[i].name), 0, (ImVec2){ 0, 0 }))
                    snprintf(n->name, ANIM_NAME, "%s", g->nodes[i].name), ch = true;
            igEndCombo();
        }
        break;
    case AN_INERTIALIZATION:
        ch |= igDragFloat(PL(TR("Blend Time")), &n->f[0], 0.01f, 0.02f, 2.0f, "%.2f s", 0);
        hint(TR("A sudden change in the Source (a switch upstream) eases out instead of popping."));
        break;
    case AN_MIRROR: {
        ch |= var_combo(PL(TR("Mirror (bool)")), n->var[0], a, BPT_BOOL);
        bool on = n->flags & 1;
        if (!n->var[0][0] && igCheckbox(PL(TR("Mirror")), &on)) n->flags = (uint8_t)((n->flags & ~1u) | (on ? 1u : 0u)), ch = true;
        break;
    }
    default: break;
    }
    if (n->kind != AN_OUTPUT && igButton(TR("Delete Node"), (ImVec2){ 0, 0 })) delete_node(g, (int)(n - g->nodes)), G.node = -1, ch = true;
    return ch;
}

static bool rule_edit(const BpAsset* a, uint8_t* op, char* var, float* value) {
    bool ch = false;
    int o = *op;
    if (igCombo_Str_arr(PL(TR("Rule")), &o, g_ops, ANIM_OP_COUNT, -1)) *op = (uint8_t)o, ch = true;
    if (*op != ANIM_OP_ANIM_END && *op != ANIM_OP_STATE_TIME)
        ch |= var_combo(PL(TR("Variable")), var, a, *op <= ANIM_OP_FALSE ? BPT_BOOL : *op == ANIM_OP_INT_EQUAL ? BPT_INT : BPT_FLOAT);
    if (*op == ANIM_OP_GREATER || *op == ANIM_OP_LESS || *op == ANIM_OP_INT_EQUAL || *op == ANIM_OP_STATE_TIME)
        ch |= igDragFloat(PL(TR("Value")), value, *op == ANIM_OP_INT_EQUAL ? 1.0f : 0.05f, -1e5f, 1e5f, *op == ANIM_OP_INT_EQUAL ? "%.0f" : "%.2f", 0);
    return ch;
}


// --- A transition's rule in a window of its own (Unreal's transition rule graph, opened by a double click on its arrow):
// the rules on the left, their graph in the middle (each rule a node into an AND into Result: Can Enter Transition), the
// blend settings ---
static struct { bool open, focus; uint32_t hash; int trans; } TW = { .trans = -1 };

static void tw_open(const BpAsset* a, int trans) { TW.open = TW.focus = true, TW.hash = a->hash, TW.trans = trans; }

static void tw_rule_text(const AnimGraph* g, uint8_t op, const char* var, float value, char* out, size_t cap) {
    (void)g;
    switch (op) {
    case ANIM_OP_TRUE: snprintf(out, cap, "%s", var[0] ? var : "?"); break;
    case ANIM_OP_FALSE: snprintf(out, cap, "NOT %s", var[0] ? var : "?"); break;
    case ANIM_OP_GREATER: snprintf(out, cap, "%s > %.2f", var[0] ? var : "?", value); break;
    case ANIM_OP_LESS: snprintf(out, cap, "%s < %.2f", var[0] ? var : "?", value); break;
    case ANIM_OP_INT_EQUAL: snprintf(out, cap, "%s == %.0f", var[0] ? var : "?", value); break;
    case ANIM_OP_ANIM_END: snprintf(out, cap, "Time Remaining (ratio) <= blend"); break;
    case ANIM_OP_STATE_TIME: snprintf(out, cap, "Time in State > %.2f s", value); break;
    default: snprintf(out, cap, "?"); break;
    }
}

static void tr_draw(void) {
    if (!TW.open) return;
    BpAsset* a = bp_find_hash(TW.hash);
    AnimGraph* g = a ? anim_graph_of(a, false) : NULL;
    if (!g || TW.trans < 0 || TW.trans >= (int)g->ntrans) {
        TW.open = false;
        return;
    }
    AnimTransition* t = &g->trans[TW.trans];
    char title[200];
    snprintf(title, sizeof title, "%s (%s -> %s)###transition_rule", TR("Transition Rule"),
             t->from == ANIM_ANY_STATE ? TR("Any State") : g->states[t->from].name, g->states[t->to].name);
    igSetNextWindowSize((ImVec2){ 980, 520 }, ImGuiCond_FirstUseEver);
    if (TW.focus) igSetNextWindowFocus(), TW.focus = false;
    bool open = true, ch = false;
    if (!igBegin(title, &open, ImGuiWindowFlags_NoCollapse)) {
        igEnd();
        TW.open = open;
        return;
    }
    float fs = igGetFontSize();
    ImVec2 av = igGetContentRegionAvail();
    float lw = fs * 22.0f;
    igBeginChild_Str("tw_rules", (ImVec2){ lw, av.y }, ImGuiChildFlags_Borders, 0);
    igSeparatorText(TR("Transition Rule (all true)"));
    igPushID_Int(0);
    ch |= rule_edit(a, &t->op, t->var, &t->value);
    igPopID();
    for (uint32_t k = 0; k < t->nmore && k < 2; k++) {
        igPushID_Int(1 + (int)k);
        igSeparator();
        ch |= rule_edit(a, &t->more[k].op, t->more[k].var, &t->more[k].value);
        if (igSmallButton(TR("Remove Rule"))) {
            if (k == 0) t->more[0] = t->more[1];
            t->nmore--, ch = true;
        }
        igPopID();
    }
    if (t->nmore < 2 && igButton(TR("+ Add Rule (AND)"), (ImVec2){ 0, 0 })) memset(&t->more[t->nmore], 0, sizeof t->more[0]), t->nmore++, ch = true;
    igSeparatorText(TR("Blend Settings"));
    ch |= igDragFloat(PL(TR("Duration")), &t->blend, 0.01f, 0.0f, 3.0f, "%.2f s", 0);
    int cv = t->curve;
    if (igCombo_Str_arr(PL(TR("Blend Logic")), &cv, g_curves, ANIM_CURVE_COUNT, -1)) t->curve = (uint8_t)cv, ch = true;
    int pr = t->priority;
    if (igSliderInt(PL(TR("Priority Order")), &pr, 0, 9, "%d", 0)) t->priority = (uint8_t)pr, ch = true;
    igEndChild();
    igSameLine(0, 4);
    // the rule's graph: each condition a node, into AND, into Result
    igBeginChild_Str("tw_graph", (ImVec2){ av.x - lw - 4.0f, av.y }, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 o = igGetCursorScreenPos(), sz = igGetContentRegionAvail();
    ImDrawList_AddRectFilled(dl, o, (ImVec2){ o.x + sz.x, o.y + sz.y }, 0xFF1E1E1Eu, 0.0f, 0);
    for (float x = 0; x < sz.x; x += 32.0f) ImDrawList_AddLine(dl, (ImVec2){ o.x + x, o.y }, (ImVec2){ o.x + x, o.y + sz.y }, 0xFF262626u, 1.0f);
    for (float y = 0; y < sz.y; y += 32.0f) ImDrawList_AddLine(dl, (ImVec2){ o.x, o.y + y }, (ImVec2){ o.x + sz.x, o.y + y }, 0xFF262626u, 1.0f);
    int nr = 1 + (int)t->nmore;
    float nw = fs * 15.0f, nh = fs * 3.0f, gap = fs * 1.2f, top = o.y + (sz.y - (float)nr * (nh + gap)) * 0.5f;
    ImVec2 andp = { o.x + sz.x * 0.55f, o.y + sz.y * 0.5f - nh * 0.5f }, res = { o.x + sz.x * 0.78f, andp.y };
    for (int k = 0; k < nr; k++) {
        ImVec2 p = { o.x + fs * 2.0f, top + (float)k * (nh + gap) };
        char txt[96];
        if (k == 0) tw_rule_text(g, t->op, t->var, t->value, txt, sizeof txt);
        else tw_rule_text(g, t->more[k - 1].op, t->more[k - 1].var, t->more[k - 1].value, txt, sizeof txt);
        ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + nw, p.y + nh }, 0xF0262626u, 6.0f, 0);
        ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + nw, p.y + fs * 1.4f }, 0xFF3C6E3Cu, 6.0f, ImDrawFlags_RoundCornersTop);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + 8, p.y + 2 }, 0xFFFFFFFFu, TR("Condition"), NULL);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + 8, p.y + fs * 1.6f }, 0xFFD8D8D8u, txt, NULL);
        ImVec2 a0 = { p.x + nw, p.y + nh * 0.6f }, a1 = { andp.x, andp.y + nh * 0.5f };
        ImDrawList_AddBezierCubic(dl, a0, (ImVec2){ a0.x + 60, a0.y }, (ImVec2){ a1.x - 60, a1.y }, a1, 0xFFC03030u, 2.5f, 0);
    }
    ImDrawList_AddRectFilled(dl, andp, (ImVec2){ andp.x + fs * 5.0f, andp.y + nh }, 0xF0262626u, 6.0f, 0);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ andp.x + fs * 1.6f, andp.y + nh * 0.5f - fs * 0.5f }, 0xFFFFFFFFu, "AND", NULL);
    ImVec2 b0 = { andp.x + fs * 5.0f, andp.y + nh * 0.5f }, b1 = { res.x, res.y + nh * 0.5f };
    ImDrawList_AddBezierCubic(dl, b0, (ImVec2){ b0.x + 50, b0.y }, (ImVec2){ b1.x - 50, b1.y }, b1, 0xFFC03030u, 2.5f, 0);
    ImDrawList_AddRectFilled(dl, res, (ImVec2){ res.x + fs * 11.0f, res.y + nh }, 0xF0262626u, 6.0f, 0);
    ImDrawList_AddRectFilled(dl, res, (ImVec2){ res.x + fs * 11.0f, res.y + fs * 1.4f }, 0xFF6E3C3Cu, 6.0f, ImDrawFlags_RoundCornersTop);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ res.x + 8, res.y + 2 }, 0xFFFFFFFFu, TR("Result"), NULL);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ res.x + 8, res.y + fs * 1.6f }, 0xFFD8D8D8u, TR("Can Enter Transition"), NULL);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + sz.x - fs * 9.0f, o.y + sz.y - fs * 2.0f }, 0x40FFFFFFu, "TRANSITION", NULL);
    igEndChild();
    igEnd();
    TW.open = open;
    if (ch) g->rev++, a->dirty = true;
}

static bool state_details(BpAsset* a, AnimGraph* g, int mi) {
    bool ch = false;
    if (G.any_sel) {
        igSeparatorText(TR("Any State"));
        hint(TR("Its transitions are tried from whatever state this machine is in."));
        if (g->ntrans < ANIM_MAX_TRANS && igBeginCombo(PL(TR("Add Transition To")), "...", 0)) {
            for (uint32_t i = 0; i < g->nstates; i++)
                if (g->states[i].machine == mi && igSelectable_Bool(g->states[i].name, false, 0, (ImVec2){ 0, 0 })) {
                    AnimTransition* t = &g->trans[g->ntrans];
                    memset(t, 0, sizeof *t);
                    t->from = ANIM_ANY_STATE, t->to = (uint8_t)i, t->op = ANIM_OP_TRUE, t->blend = 0.2f;
                    E.trans = (int)g->ntrans++, G.any_sel = false, ch = true;
                }
            igEndCombo();
        }
        return ch;
    }
    if (E.state >= 0 && E.state < (int)g->nstates) {
        AnimState* s = &g->states[E.state];
        igSeparatorText(TR("State"));
        ch |= igInputText(PL(TR("Name")), s->name, sizeof s->name, 0, NULL, NULL);
        int k = s->kind;
        if (igCombo_Str_arr(PL(TR("Plays")), &k, g_state_kinds, ANIM_STATE_KIND_COUNT, -1)) {
            s->kind = (uint8_t)k, ch = true;
            if (k == ANIM_STATE_MOTION_MATCH && !s->nsamples) mm_fill(g, &s->nsamples, s->samples);
        }
        if (s->kind == ANIM_STATE_SEQUENCE) ch |= clip_combo(PL(TR("Sequence")), s->clip, g->skeleton);
        else if (s->kind == ANIM_STATE_MOTION_MATCH) { // the database's clips; the pawn's Speed (and Direction)
            ch |= var_combo(PL(TR("Speed")), s->param, a, BPT_FLOAT);
            ch |= var_combo(PL(TR("Direction (optional)")), s->param2, a, BPT_FLOAT);
            igSeparatorText(TR("Database"));
            ch |= samples_edit(&s->nsamples, s->samples, s->sample_at, s->sample_y, g->skeleton, 0);
            if (igButton(TR("Open Motion Matching Editor"), (ImVec2){ -1, 0 })) mm_editor_open(a, E.state, -1);
            const AnimStats* st = anim_stats();
            if (st->mm_frames) igTextDisabled(TR("Database: %u frames; %u searches, %u switches since Play"), st->mm_frames, st->mm_searches, st->mm_switches);
        } else if (s->kind == ANIM_STATE_BLEND_BOOL) {
            ch |= var_combo(PL(TR("Bool")), s->param, a, BPT_BOOL);
            ch |= clip_combo(PL(TR("True Pose")), s->clip, g->skeleton);
            ch |= clip_combo(PL(TR("False Pose")), s->clip2, g->skeleton);
        } else {
            bool two = s->kind == ANIM_STATE_BLEND2D;
            ch |= var_combo(PL(two ? TR("Parameter X") : TR("Parameter")), s->param, a, BPT_FLOAT);
            if (two) ch |= var_combo(PL(TR("Parameter Y")), s->param2, a, BPT_FLOAT);
            if (igButton(TR("Open Blend Space Editor"), (ImVec2){ -FLT_MIN, 0 })) bs_open(a, -1, E.state);
            ch |= samples_edit(&s->nsamples, s->samples, s->sample_at, s->sample_y, g->skeleton, two ? 2 : 1);
            if (two) blend2d_plot(s->nsamples, s->sample_at, s->sample_y, s->samples);
        }
        ch |= igCheckbox(PL(TR("Loop")), &s->loop);
        ch |= igDragFloat(PL(TR("Play Rate")), &s->rate, 0.01f, 0.05f, 5.0f, "%.2f", 0);
        if (igButton(TR("Set as Entry"), (ImVec2){ 0, 0 })) g->machine_entry[mi] = (uint8_t)E.state, g->entry = mi == 0 ? (uint32_t)E.state : g->entry, ch = true;
        igSameLine(0, 6);
        int in_machine = 0;
        for (uint32_t i = 0; i < g->nstates; i++) in_machine += g->states[i].machine == mi;
        if (in_machine > 1 && igButton(TR("Delete State"), (ImVec2){ 0, 0 })) {
            uint32_t d = (uint32_t)E.state, keep = 0;
            for (uint32_t i = 0; i < g->ntrans; i++) {
                AnimTransition t = g->trans[i];
                if (t.from == d || t.to == d) continue;
                if (t.from != ANIM_ANY_STATE && t.from > d) t.from--;
                if (t.to > d) t.to--;
                g->trans[keep++] = t;
            }
            g->ntrans = keep;
            memmove(&g->states[d], &g->states[d + 1], sizeof(AnimState) * (g->nstates - d - 1));
            g->nstates--;
            for (uint32_t m = 0; m < g->nmachines; m++)
                if (g->machine_entry[m] > d) g->machine_entry[m]--;
                else if (g->machine_entry[m] == d) g->machine_entry[m] = 0;
            g->entry = g->machine_entry[0];
            E.state = -1, ch = true;
        }
        if (E.state >= 0 && g->ntrans < ANIM_MAX_TRANS && igBeginCombo(PL(TR("Add Transition To")), "...", 0)) {
            for (uint32_t i = 0; i < g->nstates; i++)
                if ((int)i != E.state && g->states[i].machine == mi && igSelectable_Bool(g->states[i].name, false, 0, (ImVec2){ 0, 0 })) {
                    AnimTransition* t = &g->trans[g->ntrans];
                    memset(t, 0, sizeof *t);
                    t->from = (uint8_t)E.state, t->to = (uint8_t)i, t->op = ANIM_OP_TRUE, t->blend = 0.2f;
                    E.trans = (int)g->ntrans++, E.state = -1, ch = true;
                }
            igEndCombo();
        }
    } else if (E.trans >= 0 && E.trans < (int)g->ntrans) {
        AnimTransition* t = &g->trans[E.trans];
        igSeparatorText(TR("Transition"));
        igText("%s -> %s", t->from == ANIM_ANY_STATE ? TR("Any State") : g->states[t->from].name, g->states[t->to].name);
        if (igButton(TR("Open Transition Rule"), (ImVec2){ -FLT_MIN, 0 })) tw_open(a, E.trans);
        igSeparatorText(TR("Transition Rule (all true)"));
        igPushID_Int(0);
        ch |= rule_edit(a, &t->op, t->var, &t->value);
        igPopID();
        for (uint32_t k = 0; k < t->nmore && k < 2; k++) {
            igPushID_Int(1 + (int)k);
            igSeparator();
            ch |= rule_edit(a, &t->more[k].op, t->more[k].var, &t->more[k].value);
            if (igSmallButton(TR("Remove Rule"))) {
                if (k == 0) t->more[0] = t->more[1];
                t->nmore--, ch = true;
            }
            igPopID();
        }
        if (t->nmore < 2 && igButton(TR("+ Add Rule (AND)"), (ImVec2){ 0, 0 })) memset(&t->more[t->nmore], 0, sizeof t->more[0]), t->nmore++, ch = true;
        igSeparatorText(TR("Blend Settings"));
        ch |= igDragFloat(PL(TR("Duration")), &t->blend, 0.01f, 0.0f, 3.0f, "%.2f s", 0);
        int cv = t->curve;
        if (igCombo_Str_arr(PL(TR("Blend Logic")), &cv, g_curves, ANIM_CURVE_COUNT, -1)) t->curve = (uint8_t)cv, ch = true;
        int pr = t->priority;
        if (igSliderInt(PL(TR("Priority Order")), &pr, 0, 9, "%d", 0)) t->priority = (uint8_t)pr, ch = true;
        igSetItemTooltip("%s", TR("When several transitions are ready, the lowest priority order is taken."));
        if (igButton(TR("Delete Transition"), (ImVec2){ 0, 0 })) {
            memmove(t, t + 1, sizeof *t * (g->ntrans - (uint32_t)E.trans - 1));
            g->ntrans--, E.trans = -1, ch = true;
        }
    } else
        hint(TR("Click a state or a transition. Right click the canvas to add a state; Any State's transitions leave from any state."));
    return ch;
}

static bool graph_settings(AnimGraph* g) {
    bool ch = false;
    igSeparatorText(TR("Anim Graph"));
    if (igBeginCombo(PL(TR("Skeleton")), g->skeleton[0] ? g->skeleton : "-", 0)) {
        for (uint32_t i = 0; i < anim_mesh_count(); i++) {
            SkelMesh* m = anim_mesh_at(i);
            if (igSelectable_Bool(m->name, !strcmp(g->skeleton, m->name), 0, (ImVec2){ 0, 0 })) snprintf(g->skeleton, ANIM_NAME, "%s", m->name), ch = true;
        }
        igEndCombo();
    }
    const SkelMesh* sk = anim_mesh_name(g->skeleton);
    if (sk) ch |= bone_combo(PL(TR("Upper Body Bone")), g->upper_bone, sk);
    hint(TR("Right click the canvas to add nodes. Drag from a pose pin to another to link them; Alt+click a pin breaks its link. Double click a State Machine to open it."));
    return ch;
}

bool anim_graph_tab(BpAsset* a, float width, float height) {
    AnimGraph* g = anim_graph_of(a, true);
    if (!g) return false;
    if (!g->nnodes) anim_graph_ensure_nodes(g);
    if (E.graph_of != g) {
        E.graph_of = g, E.state = E.trans = -1;
        G.machine = -1, G.node = G.drag_node = G.link_from = G.link_to = -1, G.any_sel = false;
        G.sel = 0, G.sel_states[0] = G.sel_states[1] = 0, G.box = false, G.link_state = -1;
        for (int k = 0; k < 1 + ANIM_MAX_MACHINES; k++) G.views[k] = (GraphView){ { k ? 200.0f : 700.0f, 160.0f }, 1.0f, false };
        G.fit = true;
    }
    if (G.machine >= (int)g->nmachines) G.machine = -1;
    bool ch = false;
    float right = igGetFontSize() * 21.0f, cw = width - right - 6.0f;
    igBeginChild_Str("ag_canvas", (ImVec2){ cw, height }, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 o = igGetCursorScreenPos(), sz = igGetContentRegionAvail();
    GraphView* v = &G.views[G.machine < 0 ? 0 : 1 + G.machine];
    if (G.fit || (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && igIsKeyPressed_Bool(ImGuiKey_Home, false)))
        fit_view(g, G.machine, v, sz), G.fit = false;
    igSetNextItemAllowOverlap(); // (the breadcrumb drawn over it below gets its clicks)
    igInvisibleButton("ag_bg", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    gv_input(v, o, igIsItemHovered(0) || v->panning); // right / middle drag: pan; wheel: zoom
    gv_grid(v, dl, o, sz, 0xFF262626u); // (after the input: at this frame's zoom, like the nodes)
    if (G.machine < 0) ch |= pose_canvas(a, g, o, sz);
    else ch |= machine_canvas(a, g, G.machine, o);
    // the breadcrumb (Unreal's): AnimGraph > the State Machine; clicking AnimGraph goes back up
    // (it covered the canvas's button and never got the click: no way back from a State Machine). Up a level too: the
    // back arrow, Alt+Left or the mouse's back button
    igSetCursorScreenPos((ImVec2){ o.x + 8, o.y + 6 });
    bool up = false;
    if (G.machine >= 0) {
        up |= igSmallButton("<##ag_up");
        igSetItemTooltip("%s", TR("Up a level (Alt+Left)"));
        igSameLine(0, 6);
        ImGuiIO* io = igGetIO_Nil();
        if (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io->WantTextInput &&
            (igIsKeyChordPressed_Nil(ImGuiMod_Alt | ImGuiKey_LeftArrow) ||
             igIsMouseClicked_Bool(ImGuiMouseButton_Middle + 1, false)))
            up = true;
    }
    if (igSmallButton("AnimGraph") || up) G.machine = -1, E.state = E.trans = -1, G.any_sel = false;
    if (G.machine >= 0) {
        igSameLine(0, 6);
        igTextUnformatted(">", NULL);
        igSameLine(0, 6);
        igTextColored((ImVec4){ 0.85f, 0.75f, 0.5f, 1 }, "%s (%s)", g->machines[G.machine], TR("State Machine"));
    }
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + sz.x - igCalcTextSize("ANIMATION", NULL, false, -1).x * 1.6f - 12, o.y + sz.y - igGetFontSize() * 2.0f },
                            0x40FFFFFFu, "ANIMATION", NULL);
    igEndChild();
    igSameLine(0, 4);
    igBeginChild_Str("ag_details", (ImVec2){ right, height }, ImGuiChildFlags_Borders, 0);
    // the preview in the corner (Unreal's: the Anim Blueprint's viewport beside its graph), the variables in the Viewport tab
    char pvl[64];
    snprintf(pvl, sizeof pvl, "%s###ag_preview", TR("Preview"));
    if (igCollapsingHeader_TreeNodeFlags(pvl, ImGuiTreeNodeFlags_DefaultOpen)) {
        float pw = igGetContentRegionAvail().x, ph = fminf(pw * 0.95f, height * 0.5f);
        igBeginChild_Str("ag_pv", (ImVec2){ pw, ph }, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImVec2 p = igGetCursorScreenPos();
        anim_bp_preview(a, p.x, p.y, pw, ph, true);
        igEndChild();
    }
    igPushItemWidth(-igGetFontSize() * 7.5f);
    if (G.machine < 0 && G.node >= 0 && G.node < (int)g->nnodes) ch |= node_details(a, g, &g->nodes[G.node]);
    else if (G.machine >= 0) ch |= state_details(a, g, G.machine);
    else ch |= graph_settings(g);
    igPopItemWidth();
    igEndChild();

    if (ch) g->rev++;
    return ch;
}

// The Blend Space editor's window, every frame while open (its Anim Blueprint's tabs needn't be shown)
static void bs_draw(void) {
    if (!BS.open) return;
    BpAsset* a = bp_find_hash(BS.hash);
    AnimGraph* g = a ? anim_graph_of(a, false) : NULL;
    if (!g) {
        BS.open = false;
        return;
    }
    if (bs_window(a, g)) g->rev++, a->dirty = true;
}
