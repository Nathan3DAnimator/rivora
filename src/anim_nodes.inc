// The AnimGraph's pose nodes (included by anim.c): Unreal's AnimGraph. Poses flow from the sources (State Machines,
// Sequence Players, Blend Spaces, Motion Matching, cached poses) through the blends (by bool / int / alpha, layered per
// bone, additive, the montages' slots, mirror, inertialization) and the skeletal controls (Two Bone IK, Leg IK, Aim
// Offset, Lean, Transform (Modify) Bone, Look At) to the Output Pose.
//   * logic (serial, step_logic): the machines' transitions (up to 3 rules together, priority, Any State, blend curve),
//     the active nodes' times, notifies, the bool / int blends' weights, the Motion Matching search;
//   * pose (parallel, pose_one): the graph evaluated from the Output Pose (recursion, thread-local buffers per depth;
//     each Save Cached Pose evaluated once per instance and frame).
// The old graphs (a state machine, the montages, Lean, Aim Offset, Foot IK as the graph's settings) become nodes.

static const AnimNodeInfo g_node_info[AN_KIND_COUNT] = {
    [AN_OUTPUT] = { "Output Pose", "", 0xFF3A3A3Au, 1, { "Result" } },
    [AN_STATE_MACHINE] = { "State Machine", "State Machines", 0xFF6E5A3Cu, 0, { 0 } },
    [AN_SEQUENCE] = { "Sequence Player", "Animation", 0xFF6E6A2Au, 0, { 0 } },
    [AN_BLEND_SPACE_1D] = { "Blend Space 1D Player", "Animation", 0xFF6E6A2Au, 0, { 0 } },
    [AN_BLEND_SPACE_2D] = { "Blend Space Player", "Animation", 0xFF6E6A2Au, 0, { 0 } },
    [AN_BLEND_BOOL] = { "Blend Poses by bool", "Blends", 0xFF7A5A2Au, 2, { "True Pose", "False Pose" } },
    [AN_BLEND_INT] = { "Blend Poses by int", "Blends", 0xFF7A5A2Au, 4, { "Blend Pose 0", "Blend Pose 1", "Blend Pose 2", "Blend Pose 3" } },
    [AN_BLEND] = { "Blend", "Blends", 0xFF7A5A2Au, 2, { "A", "B" } },
    [AN_LAYERED_BLEND] = { "Layered blend per bone", "Blends", 0xFF7A5A2Au, 2, { "Base Pose", "Blend Pose" } },
    [AN_APPLY_ADDITIVE] = { "Apply Additive", "Blends", 0xFF7A5A2Au, 1, { "Base" } },
    [AN_SLOT] = { "Slot", "Montage", 0xFF4A6A3Au, 1, { "Source" } },
    [AN_TWO_BONE_IK] = { "Two Bone IK", "Skeletal Controls", 0xFF8A5A3Au, 1, { "Component Pose" } },
    [AN_LEG_IK] = { "Leg IK", "Skeletal Controls", 0xFF8A5A3Au, 1, { "Component Pose" } },
    [AN_AIM_OFFSET] = { "Aim Offset", "Skeletal Controls", 0xFF8A5A3Au, 1, { "Base Pose" } },
    [AN_LEAN] = { "Lean", "Skeletal Controls", 0xFF8A5A3Au, 1, { "Component Pose" } },
    [AN_MODIFY_BONE] = { "Transform (Modify) Bone", "Skeletal Controls", 0xFF8A5A3Au, 1, { "Component Pose" } },
    [AN_LOOK_AT] = { "Look At", "Skeletal Controls", 0xFF8A5A3Au, 1, { "Component Pose" } },
    [AN_SAVE_CACHE] = { "Save Cached Pose", "Cached Poses", 0xFF3A5A7Au, 1, { "Pose" } },
    [AN_USE_CACHE] = { "Use Cached Pose", "Cached Poses", 0xFF3A5A7Au, 0, { 0 } },
    [AN_MOTION_MATCH] = { "Motion Matching", "Animation", 0xFF6A3A7Au, 0, { 0 } },
    [AN_INERTIALIZATION] = { "Inertialization", "Blends", 0xFF7A5A2Au, 1, { "Source" } },
    [AN_MIRROR] = { "Mirror", "Blends", 0xFF7A5A2Au, 1, { "Source" } },
    [AN_REROUTE] = { "Reroute", "Utilities", 0xFF505050u, 1, { "" } },
    [AN_ORIENT_WARP] = { "Orientation Warping", "Skeletal Controls", 0xFF8A5A3Au, 1, { "Component Pose" } },
    [AN_STRIDE_WARP] = { "Stride Warping", "Skeletal Controls", 0xFF8A5A3Au, 1, { "Component Pose" } },
};

const AnimNodeInfo* anim_node_info(int kind) { return &g_node_info[kind >= 0 && kind < AN_KIND_COUNT ? kind : 0]; }

void anim_node_init(AnimNode* n, int kind) {
    memset(n, 0, sizeof *n);
    n->kind = (uint8_t)kind;
    for (int k = 0; k < ANIM_NODE_IN; k++) n->in[k] = -1;
    switch (kind) {
    case AN_SEQUENCE: n->f[0] = 1.0f, n->flags = 1; break;
    case AN_BLEND_SPACE_1D: case AN_BLEND_SPACE_2D: n->f[0] = 1.0f; break;
    case AN_BLEND_BOOL: case AN_BLEND_INT: n->f[0] = 0.2f; break;
    case AN_BLEND: n->f[0] = 0.5f; break;
    case AN_LAYERED_BLEND: n->f[0] = 1.0f, snprintf(n->bone[0], ANIM_NAME, "spine_01"); break;
    case AN_APPLY_ADDITIVE: n->f[0] = 1.0f; break;
    case AN_SLOT: snprintf(n->name, ANIM_NAME, "DefaultSlot"); break;
    case AN_TWO_BONE_IK: n->f[3] = 1.0f, snprintf(n->bone[0], ANIM_NAME, "hand_r"); break;
    case AN_LEG_IK: n->f[0] = 45.0f, snprintf(n->bone[0], ANIM_NAME, "foot_l"), snprintf(n->bone[1], ANIM_NAME, "foot_r"); break;
    case AN_AIM_OFFSET: {
        static const char* const chain[4] = { "spine_01", "spine_02", "spine_03", "head" };
        static const float share[4] = { 0.2f, 0.25f, 0.25f, 0.3f };
        for (int k = 0; k < 4; k++) snprintf(n->bone[k], ANIM_NAME, "%s", chain[k]), n->f[k] = share[k];
        break;
    }
    case AN_LEAN:
        n->f[0] = 0.5f;
        for (int k = 0; k < 3; k++) snprintf(n->bone[k], ANIM_NAME, "spine_0%d", k + 1);
        break;
    case AN_MODIFY_BONE: n->f[6] = 1.0f, snprintf(n->bone[0], ANIM_NAME, "head"); break;
    case AN_LOOK_AT: n->f[0] = 60.0f, n->f[1] = 1.0f, snprintf(n->bone[0], ANIM_NAME, "head"); break;
    case AN_SAVE_CACHE: case AN_USE_CACHE: snprintf(n->name, ANIM_NAME, "Locomotion"); break;
    case AN_INERTIALIZATION: n->f[0] = 0.2f; break;
    case AN_MIRROR: n->flags = 1; break;
    case AN_ORIENT_WARP: { // (2026-11-23)
        static const char* const ob[4] = { "pelvis", "spine_01", "spine_02", "spine_03" };
        for (int k = 0; k < 4; k++) snprintf(n->bone[k], ANIM_NAME, "%s", ob[k]);
        n->f[0] = 90.0f, n->f[1] = 1.0f, n->f[2] = 10.0f;
        break;
    }
    case AN_STRIDE_WARP:
        snprintf(n->bone[0], ANIM_NAME, "foot_l"), snprintf(n->bone[1], ANIM_NAME, "foot_r"), snprintf(n->bone[2], ANIM_NAME, "pelvis");
        n->f[0] = 0.5f, n->f[1] = 2.0f, n->f[2] = 1.0f, n->f[3] = 6.0f;
        break;
    default: break;
    }
}

static int add_node(AnimGraph* g, int kind, float x, float y) {
    if (g->nnodes >= ANIM_MAX_NODES) return -1;
    AnimNode* n = &g->nodes[g->nnodes];
    anim_node_init(n, kind);
    n->x = x, n->y = y;
    return (int)g->nnodes++;
}

// The old settings as a chain: State Machine -> Slot DefaultSlot -> Slot UpperBody -> Lean -> Aim Offset -> Leg IK -> Output
void anim_graph_ensure_nodes(struct AnimGraph* g) {
    if (g->nmachines == 0) {
        g->nmachines = 1;
        snprintf(g->machines[0], ANIM_NAME, "Locomotion");
        g->machine_entry[0] = (uint8_t)g->entry;
    }
    if (g->nnodes) return;
    float x = -1200.0f;
    int prev = add_node(g, AN_STATE_MACHINE, x, 0.0f);
    snprintf(g->nodes[prev].name, ANIM_NAME, "%s", g->machines[0]);
    int at;
    const char* slots[2] = { "DefaultSlot", "UpperBody" };
    for (int k = 0; k < 2; k++) {
        at = add_node(g, AN_SLOT, x += 260.0f, 0.0f);
        snprintf(g->nodes[at].name, ANIM_NAME, "%s", slots[k]);
        g->nodes[at].in[0] = (int16_t)prev, prev = at;
    }
    if (g->lean > 0.0f) {
        at = add_node(g, AN_LEAN, x += 260.0f, 0.0f);
        g->nodes[at].f[0] = g->lean;
        for (int k = 0; k < 3; k++) snprintf(g->nodes[at].bone[k], ANIM_NAME, "%s", g->aim_bones[k]);
        g->nodes[at].in[0] = (int16_t)prev, prev = at;
    }
    if (g->aim_yaw[0] || g->aim_pitch[0]) {
        at = add_node(g, AN_AIM_OFFSET, x += 260.0f, 0.0f);
        snprintf(g->nodes[at].var[0], ANIM_NAME, "%s", g->aim_yaw);
        snprintf(g->nodes[at].var[1], ANIM_NAME, "%s", g->aim_pitch);
        for (int k = 0; k < 4; k++) snprintf(g->nodes[at].bone[k], ANIM_NAME, "%s", g->aim_bones[k]), g->nodes[at].f[k] = g->aim_share[k];
        g->nodes[at].in[0] = (int16_t)prev, prev = at;
    }
    if (g->foot_ik) {
        at = add_node(g, AN_LEG_IK, x += 260.0f, 0.0f);
        snprintf(g->nodes[at].bone[0], ANIM_NAME, "%s", g->foot_l);
        snprintf(g->nodes[at].bone[1], ANIM_NAME, "%s", g->foot_r);
        g->nodes[at].f[0] = g->foot_reach > 1.0f ? g->foot_reach : 45.0f;
        g->nodes[at].in[0] = (int16_t)prev, prev = at;
    }
    at = add_node(g, AN_OUTPUT, x += 300.0f, 0.0f);
    g->nodes[at].in[0] = (int16_t)prev;
}

static int output_node(const AnimGraph* g) {
    for (uint32_t i = 0; i < g->nnodes; i++)
        if (g->nodes[i].kind == AN_OUTPUT) return (int)i;
    return -1;
}

// --- The graph's file: the machines, the nodes, a transition's more rules (anim_graph_write / read) ---

static void nodes_write(const AnimGraph* g, JsonWriter* w) {
    jw_key(w, "machines");
    jw_arr_begin(w);
    for (uint32_t m = 0; m < g->nmachines; m++) {
        jw_obj_begin(w);
        jw_kstr(w, "name", g->machines[m]);
        jw_kint(w, "entry", g->machine_entry[m]);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_key(w, "nodes");
    jw_arr_begin(w);
    for (uint32_t i = 0; i < g->nnodes; i++) {
        const AnimNode* n = &g->nodes[i];
        jw_obj_begin(w);
        jw_kint(w, "kind", n->kind);
        jw_kfloats(w, "pos", (float[]){ n->x, n->y }, 2);
        jw_kfloats(w, "in", (float[]){ n->in[0], n->in[1], n->in[2], n->in[3] }, 4);
        if (n->kind == AN_STATE_MACHINE) jw_kint(w, "machine", n->machine);
        if (n->flags) jw_kint(w, "flags", n->flags);
        if (n->name[0]) jw_kstr(w, "name", n->name);
        if (n->var[0][0]) jw_kstr(w, "var", n->var[0]);
        if (n->var[1][0]) jw_kstr(w, "var2", n->var[1]);
        if (n->clip[0]) jw_kstr(w, "clip", n->clip);
        jw_key(w, "bones");
        jw_arr_begin(w);
        for (int k = 0; k < 4; k++) jw_str(w, n->bone[k]);
        jw_arr_end(w);
        jw_kfloats(w, "f", n->f, 8);
        if (n->kind == AN_MOTION_MATCH && n->mmgen[ANIM_MMGEN_SET] == 1.0f) jw_kfloats(w, "mmgen", n->mmgen, ANIM_MMGEN);
        if (n->axis[1] != n->axis[0] || n->axis[4] != 0.0f) jw_kfloats(w, "axis", n->axis, 6);
        if (n->nsamples) {
            jw_key(w, "samples");
            jw_arr_begin(w);
            for (uint32_t k = 0; k < n->nsamples; k++) {
                jw_obj_begin(w);
                jw_kstr(w, "clip", n->samples[k]);
                jw_kfloat(w, "at", n->sample_at[k]);
                jw_kfloat(w, "y", n->sample_y[k]);
                jw_obj_end(w);
            }
            jw_arr_end(w);
        }
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

static void nodes_read(AnimGraph* g, const JsonValue* v) {
    const JsonValue* ms = json_get(v, "machines");
    for (size_t m = 0; m < json_len(ms) && g->nmachines < ANIM_MAX_MACHINES; m++) {
        snprintf(g->machines[g->nmachines], ANIM_NAME, "%s", json_get_str(json_at(ms, m), "name", "State Machine"));
        int e = json_get_int(json_at(ms, m), "entry", 0);
        g->machine_entry[g->nmachines++] = (uint8_t)(e >= 0 && e < (int)g->nstates ? e : 0);
    }
    const JsonValue* ns = json_get(v, "nodes");
    for (size_t i = 0; i < json_len(ns) && g->nnodes < ANIM_MAX_NODES; i++) {
        const JsonValue* o = json_at(ns, i);
        int kind = json_get_int(o, "kind", 0);
        if (kind < 0 || kind >= AN_KIND_COUNT) kind = AN_OUTPUT;
        AnimNode* n = &g->nodes[g->nnodes++];
        anim_node_init(n, kind);
        float pos[2] = { 0, 0 }, in[4] = { -1, -1, -1, -1 };
        json_get_floats(o, "pos", pos, 2);
        json_get_floats(o, "in", in, 4);
        n->x = pos[0], n->y = pos[1];
        for (int k = 0; k < ANIM_NODE_IN; k++) n->in[k] = (int16_t)(in[k] >= 0.0f && in[k] < ANIM_MAX_NODES ? in[k] : -1);
        n->machine = (uint8_t)json_get_int(o, "machine", 0);
        n->flags = (uint8_t)json_get_int(o, "flags", n->flags);
        snprintf(n->name, ANIM_NAME, "%s", json_get_str(o, "name", n->name));
        snprintf(n->var[0], ANIM_NAME, "%s", json_get_str(o, "var", ""));
        snprintf(n->var[1], ANIM_NAME, "%s", json_get_str(o, "var2", ""));
        snprintf(n->clip, ANIM_NAME, "%s", json_get_str(o, "clip", ""));
        const JsonValue* bs = json_get(o, "bones");
        for (size_t k = 0; k < json_len(bs) && k < 4; k++) snprintf(n->bone[k], ANIM_NAME, "%s", json_str(json_at(bs, k), ""));
        json_get_floats(o, "f", n->f, 8);
        json_get_floats(o, "mmgen", n->mmgen, ANIM_MMGEN);
        json_get_floats(o, "axis", n->axis, 6);
        const JsonValue* sm = json_get(o, "samples");
        for (size_t k = 0; k < json_len(sm) && n->nsamples < ANIM_BLEND_SAMPLES; k++) {
            snprintf(n->samples[n->nsamples], ANIM_NAME, "%s", json_get_str(json_at(sm, k), "clip", ""));
            n->sample_y[n->nsamples] = (float)json_get_num(json_at(sm, k), "y", 0.0);
            n->sample_at[n->nsamples++] = (float)json_get_num(json_at(sm, k), "at", 0.0);
        }
    }
    for (uint32_t i = 0; i < g->nnodes; i++) // (a link past the nodes read: none)
        for (int k = 0; k < ANIM_NODE_IN; k++)
            if (g->nodes[i].in[k] >= (int16_t)g->nnodes) g->nodes[i].in[k] = -1;
}

// --- Run time: the machines (each AnimInst.sm[m]) ---

static float curve_of(uint8_t curve, float t) {
    t = fminf(fmaxf(t, 0.0f), 1.0f);
    if (curve == ANIM_CURVE_CUBIC) return t * t * (3.0f - 2.0f * t);
    if (curve == ANIM_CURVE_EASE_OUT) return 1.0f - (1.0f - t) * (1.0f - t);
    return t;
}

static bool rule_ready(World* w, AnimInst* in, uint32_t mi, uint8_t op, const char* var, float value, float blend) {
    const AnimGraph* g = in->graph;
    if (op == ANIM_OP_ANIM_END) return in->sm[mi].state_t >= state_length(in, &g->states[in->sm[mi].state]) - blend;
    if (op == ANIM_OP_STATE_TIME) return in->sm[mi].state_t > value;
    float v = var_of(w, in, var);
    switch (op) {
    case ANIM_OP_TRUE: return v != 0.0f;
    case ANIM_OP_FALSE: return v == 0.0f;
    case ANIM_OP_GREATER: return v > value;
    case ANIM_OP_LESS: return v < value;
    case ANIM_OP_INT_EQUAL: return (int)lroundf(v) == (int)lroundf(value);
    default: return false;
    }
}

static bool trans_ready(World* w, AnimInst* in, uint32_t mi, const AnimTransition* t) {
    if (!rule_ready(w, in, mi, t->op, t->var, t->value, t->blend)) return false;
    for (uint32_t k = 0; k < t->nmore && k < 2; k++)
        if (!rule_ready(w, in, mi, t->more[k].op, t->more[k].var, t->more[k].value, t->blend)) return false;
    return true;
}

// A machine's frame: its transitions (by priority; Any State's too), its state's time and notifies
static void machine_step(World* w, AnimInst* in, uint32_t mi, float dt) {
    const AnimGraph* g = in->graph;
    MachineRun* r = &in->sm[mi];
    if (r->state >= g->nstates || g->states[r->state].machine != mi) { // (edited: back to its entry)
        r->state = r->prev = g->machine_entry[mi] < g->nstates ? g->machine_entry[mi] : 0;
        r->state_t = r->prev_t = 0.0f, r->blend_t = r->blend_len = 0.0f;
    }
    const AnimTransition* best = NULL;
    for (uint32_t i = 0; i < g->ntrans; i++) {
        const AnimTransition* t = &g->trans[i];
        if (t->to >= g->nstates || g->states[t->to].machine != mi || t->to == r->state) continue;
        if (t->from != ANIM_ANY_STATE && t->from != r->state) continue;
        if (best && t->priority >= best->priority) continue;
        if (trans_ready(w, in, mi, t)) best = t;
    }
    if (best) {
        r->prev = r->state, r->prev_t = r->state_t;
        r->state = best->to, r->state_t = 0.0f;
        r->blend_len = fmaxf(best->blend, 0.0f), r->blend_t = 0.0f, r->curve = best->curve;
    }
    const AnimState* s = &g->states[r->state];
    float rate = s->rate > 0.0f ? s->rate : 1.0f;
    if (s->kind == ANIM_STATE_SEQUENCE) fire_notifies(w, in, anim_seq_name(s->clip), r->state_t, r->state_t + dt * rate);
    else if (s->kind == ANIM_STATE_MOTION_MATCH) mm_step(w, in, s, dt); // (its own times and notifies)
    else if (s->kind == ANIM_STATE_BLEND1D && s->nsamples) { // the dominant sample's notifies
        float x = var_of(w, in, s->param);
        uint32_t k = 0;
        for (uint32_t j = 1; j < s->nsamples; j++)
            if (fabsf(x - s->sample_at[j]) < fabsf(x - s->sample_at[k])) k = j;
        fire_notifies(w, in, anim_seq_name(s->samples[k]), r->state_t, r->state_t + dt * rate);
    }
    r->state_t += dt * rate, r->prev_t += dt;
    if (r->blend_t < r->blend_len) r->blend_t += dt;
    if (mi == 0 || !in->state_name) in->state_name = s->name;
    state_warm(in, s);
    if (r->prev < g->nstates) state_warm(in, &g->states[r->prev]);
}

static void machine_pose(World* w, const AnimInst* in, uint32_t mi, AnimXf* out, AnimXf* tmp) {
    const AnimGraph* g = in->graph;
    const MachineRun* r = &in->sm[mi];
    const SkelMesh* m = in->mesh;
    if (!g->nstates || r->state >= g->nstates) {
        for (uint32_t i = 0; i < m->nbones; i++) out[i] = m->bones[i].bind;
        return;
    }
    state_pose(w, in, &g->states[r->state], r->state_t, out);
    if (r->blend_t < r->blend_len && r->prev != r->state && r->prev < g->nstates) {
        state_pose(w, in, &g->states[r->prev], r->prev_t, tmp);
        anim_blend(tmp, out, curve_of(r->curve, r->blend_t / r->blend_len), m->nbones, out);
    }
}

// A node's stand-in state (the code the states share: Blend Spaces, Motion Matching)
void anim_node_state(const AnimNode* n, AnimState* s) {
    memset(s, 0, sizeof *s);
    s->kind = n->kind == AN_BLEND_SPACE_1D ? ANIM_STATE_BLEND1D : n->kind == AN_BLEND_SPACE_2D ? ANIM_STATE_BLEND2D
              : n->kind == AN_MOTION_MATCH ? ANIM_STATE_MOTION_MATCH : ANIM_STATE_SEQUENCE;
    s->loop = n->kind != AN_SEQUENCE || (n->flags & 1);
    s->rate = n->f[0] > 0.0f ? n->f[0] : 1.0f;
    snprintf(s->clip, ANIM_NAME, "%s", n->clip);
    snprintf(s->param, ANIM_NAME, "%s", n->var[0]);
    snprintf(s->param2, ANIM_NAME, "%s", n->var[1]);
    s->nsamples = n->nsamples;
    memcpy(s->samples, n->samples, sizeof s->samples);
    memcpy(s->sample_at, n->sample_at, sizeof s->sample_at);
    memcpy(s->sample_y, n->sample_y, sizeof s->sample_y);
    memcpy(s->mmgen, n->mmgen, sizeof s->mmgen);
    memcpy(s->axis, n->axis, sizeof s->axis);
}

// --- The logic: the nodes the Output Pose reaches (with this frame's choices), their times and weights ---

static void nodes_visit(World* w, AnimInst* in, int ni, float dt, int depth, uint64_t* seen_machines) {
    const AnimGraph* g = in->graph;
    if (ni < 0 || ni >= (int)g->nnodes || depth > 24) return;
    const AnimNode* n = &g->nodes[ni];
    switch (n->kind) {
    case AN_STATE_MACHINE:
        if (n->machine < g->nmachines && !(*seen_machines & (1ull << n->machine))) {
            *seen_machines |= 1ull << n->machine;
            machine_step(w, in, n->machine, dt);
        }
        return;
    case AN_SEQUENCE: {
        AnimSeq* q = anim_seq_name(n->clip);
        float rate = n->f[0] > 0.0f ? n->f[0] : 1.0f;
        if (in->node_t[ni] == 0.0f && n->f[1] > 0.0f) in->node_t[ni] = n->f[1];
        if (q) fire_notifies(w, in, q, in->node_t[ni], in->node_t[ni] + dt * rate), seq_warm(q, in->mesh);
        in->node_t[ni] += dt * rate;
        return;
    }
    case AN_BLEND_SPACE_1D: case AN_BLEND_SPACE_2D: {
        AnimState s;
        anim_node_state(n, &s);
        state_warm(in, &s);
        in->node_t[ni] += dt * s.rate;
        return;
    }
    case AN_MOTION_MATCH: {
        AnimState s;
        anim_node_state(n, &s);
        mm_step(w, in, &s, dt);
        state_warm(in, &s);
        return;
    }
    case AN_APPLY_ADDITIVE: {
        AnimSeq* q = anim_seq_name(n->clip);
        if (q) seq_warm(q, in->mesh);
        in->node_t[ni] += dt;
        break;
    }
    case AN_BLEND_BOOL: case AN_BLEND_INT: { // the weights ease toward the chosen pose over the blend time
        int pick = n->kind == AN_BLEND_BOOL ? (var_of(w, in, n->var[0]) != 0.0f ? 0 : 1)
                                            : (int)fminf(fmaxf(lroundf(var_of(w, in, n->var[0])), 0.0f), 3.0f);
        float k = n->f[0] > 1e-3f ? fminf(dt / n->f[0], 1.0f) : 1.0f;
        for (int p = 0; p < ANIM_NODE_IN; p++) {
            float target = p == pick ? 1.0f : 0.0f;
            float* wv = &in->node_w[ni][p];
            *wv += fminf(fmaxf(target - *wv, -k), k);
            if (*wv > 0.001f) nodes_visit(w, in, n->in[p], dt, depth + 1, seen_machines);
        }
        return;
    }
    case AN_USE_CACHE:
        for (uint32_t j = 0; j < g->nnodes; j++)
            if (g->nodes[j].kind == AN_SAVE_CACHE && !strcmp(g->nodes[j].name, n->name) && !(in->cache_visited & (1ull << j))) {
                in->cache_visited |= 1ull << j;
                nodes_visit(w, in, g->nodes[j].in[0], dt, depth + 1, seen_machines);
            }
        return;
    default: break;
    }
    for (int p = 0; p < ANIM_NODE_IN; p++) nodes_visit(w, in, n->in[p], dt, depth + 1, seen_machines);
}

static void nodes_logic(World* w, AnimInst* in, float dt) {
    uint64_t seen = 0;
    in->cache_visited = 0;
    in->state_name = NULL;
    nodes_visit(w, in, output_node(in->graph), dt, 0, &seen);
    if (!in->state_name) in->state_name = "-";
}

// --- The pose: the graph evaluated from the Output Pose ---

#define EVAL_DEPTH 16
typedef struct EvalCtx {
    World* w;
    AnimInst* in;
    const float (*rows)[4];
    float (*bones)[3][4];
    const CCharacter* chr;
    uint64_t cached; // the Save Cached Pose nodes done this frame
} EvalCtx;
static _Thread_local AnimXf t_eval[EVAL_DEPTH][2][ANIM_MAX_BONES];
static _Thread_local AnimXf t_cache[4][ANIM_MAX_BONES];
static _Thread_local int t_cache_node[4];

static void bind_pose(const SkelMesh* m, AnimXf* out) {
    for (uint32_t i = 0; i < m->nbones; i++) out[i] = m->bones[i].bind;
}

// The skeletal controls' math (shared with the old graphs' settings)
static void turn_bones(const SkelMesh* m, AnimXf* pose, const float rows[3][4], float bones[][3][4], const int16_t* bs, const float* share,
                       int n, const float axis1[3], float a1, const float axis2[3], float a2) {
    float total = 0.0f;
    for (int k = 0; k < n; k++) total += bs[k] >= 0 ? fmaxf(share ? share[k] : 1.0f, 0.0f) : 0.0f;
    if (total < 1e-4f || (fabsf(a1) + fabsf(a2)) < 1e-5f) return;
    for (int k = 0; k < n; k++) {
        int16_t b = bs[k];
        if (b < 0) continue;
        float f = fmaxf(share ? share[k] : 1.0f, 0.0f) / total, q1[4], q2[4], q[4], Rq[3][3], D[3][3], W[3][3];
        q_axis(axis1, a1 * f, q1), q_axis(axis2, a2 * f, q2);
        q_mul(q1, q2, q);
        q_axes(q, Rq);
        for (int a = 0; a < 3; a++)
            for (int r = 0; r < 3; r++) D[r][a] = Rq[a][r];
        anim_world(m, pose, rows, bones);
        rows_rot(bones[b], W);
        m_mul(D, W, W);
        set_world_rot(m, pose, (uint32_t)b, W, bones, rows);
    }
}

// the bone (end) moved to target by turning its parent and grandparent (two bone IK), its own turn kept
static void two_bone_to(const SkelMesh* m, AnimXf* pose, const float rows[3][4], float bones[][3][4], int16_t th, int16_t ca, int16_t fb,
                        const float T[3], float alpha) {
    anim_world(m, pose, rows, bones);
    float keepW[3][3];
    rows_rot(bones[fb], keepW);
    float A[3] = { bones[th][0][3], bones[th][1][3], bones[th][2][3] }, B[3] = { bones[ca][0][3], bones[ca][1][3], bones[ca][2][3] };
    float C[3] = { bones[fb][0][3], bones[fb][1][3], bones[fb][2][3] }, Tt[3];
    for (int k = 0; k < 3; k++) Tt[k] = C[k] + (T[k] - C[k]) * alpha;
    float nB[3], nC[3], u[3], v[3], D[3][3], W[3][3];
    anim_two_bone_ik(A, B, C, Tt, nB, nC);
    float lu = 0.0f, lv = 0.0f;
    for (int k = 0; k < 3; k++) u[k] = B[k] - A[k], v[k] = nB[k] - A[k], lu += u[k] * u[k], lv += v[k] * v[k];
    lu = sqrtf(lu), lv = sqrtf(lv);
    if (lu < 1e-4f || lv < 1e-4f) return;
    for (int k = 0; k < 3; k++) u[k] /= lu, v[k] /= lv;
    m_between(u, v, D);
    rows_rot(bones[th], W);
    m_mul(D, W, W);
    set_world_rot(m, pose, (uint32_t)th, W, bones, rows);
    anim_world(m, pose, rows, bones);
    float B2[3] = { bones[ca][0][3], bones[ca][1][3], bones[ca][2][3] }, C2[3] = { bones[fb][0][3], bones[fb][1][3], bones[fb][2][3] };
    lu = lv = 0.0f;
    for (int k = 0; k < 3; k++) u[k] = C2[k] - B2[k], v[k] = nC[k] - B2[k], lu += u[k] * u[k], lv += v[k] * v[k];
    lu = sqrtf(lu), lv = sqrtf(lv);
    if (lu < 1e-4f || lv < 1e-4f) return;
    for (int k = 0; k < 3; k++) u[k] /= lu, v[k] /= lv;
    m_between(u, v, D);
    rows_rot(bones[ca], W);
    m_mul(D, W, W);
    set_world_rot(m, pose, (uint32_t)ca, W, bones, rows);
    anim_world(m, pose, rows, bones);
    set_world_rot(m, pose, (uint32_t)fb, keepW, bones, rows);
}

static void eval_node(EvalCtx* c, int ni, AnimXf* out, int depth);

static void eval_input(EvalCtx* c, const AnimNode* n, int pin, AnimXf* out, int depth) {
    if (n->in[pin] < 0) bind_pose(c->in->mesh, out);
    else eval_node(c, n->in[pin], out, depth + 1);
}

static void eval_node(EvalCtx* c, int ni, AnimXf* out, int depth) {
    AnimInst* in = c->in;
    const AnimGraph* g = in->graph;
    const SkelMesh* m = in->mesh;
    if (ni < 0 || ni >= (int)g->nnodes || depth >= EVAL_DEPTH) {
        bind_pose(m, out);
        return;
    }
    const AnimNode* n = &g->nodes[ni];
    const int16_t* nb = in->node_bone[ni];
    AnimXf* tmp = t_eval[depth][0];
    switch (n->kind) {
    case AN_OUTPUT: eval_input(c, n, 0, out, depth); return;
    case AN_STATE_MACHINE:
        if (n->machine < g->nmachines) machine_pose(c->w, in, n->machine, out, tmp);
        else bind_pose(m, out);
        return;
    case AN_SEQUENCE: case AN_BLEND_SPACE_1D: case AN_BLEND_SPACE_2D: case AN_MOTION_MATCH: {
        AnimState s;
        anim_node_state(n, &s);
        state_pose(c->w, in, &s, in->node_t[ni], out);
        return;
    }
    case AN_BLEND_BOOL: case AN_BLEND_INT: {
        bool any = false;
        float sum = 0.0f;
        for (int p = 0; p < ANIM_NODE_IN; p++) sum += in->node_w[ni][p] > 0.001f ? in->node_w[ni][p] : 0.0f;
        for (int p = 0; p < ANIM_NODE_IN; p++) {
            float wv = in->node_w[ni][p];
            if (wv <= 0.001f) continue;
            if (!any) {
                eval_input(c, n, p, out, depth), any = true;
                sum = wv;
            } else {
                eval_input(c, n, p, tmp, depth);
                sum += wv;
                anim_blend(out, tmp, wv / sum, m->nbones, out);
            }
        }
        if (!any) eval_input(c, n, n->kind == AN_BLEND_BOOL ? 1 : 0, out, depth);
        return;
    }
    case AN_BLEND: {
        float a = n->var[0][0] ? var_of(c->w, in, n->var[0]) : n->f[0];
        a = fminf(fmaxf(a, 0.0f), 1.0f);
        eval_input(c, n, 0, out, depth);
        if (a > 0.0f) {
            eval_input(c, n, 1, tmp, depth);
            anim_blend(out, tmp, a, m->nbones, out);
        }
        return;
    }
    case AN_LAYERED_BLEND: {
        eval_input(c, n, 0, out, depth);
        float a = fminf(fmaxf(n->f[0], 0.0f), 1.0f);
        if (a <= 0.0f) return;
        eval_input(c, n, 1, tmp, depth);
        const uint64_t* mask = in->node_mask[ni];
        for (uint32_t b = 0; b < m->nbones; b++)
            if (MASK_HAS(mask, b)) anim_blend(&out[b], &tmp[b], a, 1, &out[b]);
        return;
    }
    case AN_APPLY_ADDITIVE: {
        eval_input(c, n, 0, out, depth);
        AnimSeq* q = anim_seq_name(n->clip);
        float a = fminf(fmaxf(n->var[0][0] ? var_of(c->w, in, n->var[0]) : n->f[0], 0.0f), 1.0f);
        if (!q || a <= 0.0f) return;
        int16_t map[ANIM_MAX_BONES];
        const int16_t* mp = q->map_for == m ? q->map : (track_map(q, m, map), map);
        AnimXf* ref = t_eval[depth][1];
        sample_mapped(q, m, mp, 0.0f, ref);
        sample_mapped(q, m, mp, in->node_t[ni], tmp);
        for (uint32_t b = 0; b < m->nbones; b++) { // the clip's change from its first frame, on the base
            float inv[4] = { -ref[b].q[0], -ref[b].q[1], -ref[b].q[2], ref[b].q[3] }, d[4], id[4] = { 0, 0, 0, 1 }, dq[4], r[4];
            q_mul(tmp[b].q, inv, d);
            q_nlerp(id, d, a, dq);
            q_mul(dq, out[b].q, r);
            memcpy(out[b].q, r, sizeof r);
            for (int k = 0; k < 3; k++) out[b].t[k] += (tmp[b].t[k] - ref[b].t[k]) * a;
        }
        return;
    }
    case AN_SLOT: {
        eval_input(c, n, 0, out, depth);
        const char* slot = n->name[0] ? n->name : "DefaultSlot";
        if (!in->montage || !in->mclip.seq || in->mweight <= 0.0f || strcmp(in->montage->slot, slot)) return;
        sample_mapped(in->mclip.seq, m, in->mclip.map, in->mtime, tmp);
        if (strcmp(slot, "UpperBody")) rm_inplace(in, m, tmp); // (2026-11-23) its root path off (a whole-body slot): the actor carries it
        bool upper = !strcmp(slot, "UpperBody") && in->has_upper;
        float wgt = in->mweight * in->mweight * (3.0f - 2.0f * in->mweight);
        for (uint32_t b = 0; b < m->nbones; b++)
            if (!upper || MASK_HAS(in->upper_mask, b)) anim_blend(&out[b], &tmp[b], wgt, 1, &out[b]);
        return;
    }
    case AN_SAVE_CACHE: eval_input(c, n, 0, out, depth); return;
    case AN_REROUTE: eval_input(c, n, 0, out, depth); return;
    case AN_USE_CACHE:
        for (uint32_t j = 0; j < g->nnodes; j++) {
            if (g->nodes[j].kind != AN_SAVE_CACHE || strcmp(g->nodes[j].name, n->name)) continue;
            int slot = -1;
            for (int k = 0; k < 4; k++)
                if ((c->cached >> j & 1ull) && t_cache_node[k] == (int)j) slot = k;
            if (slot < 0) { // evaluated once this frame, then copied
                for (int k = 0; k < 4 && slot < 0; k++)
                    if (t_cache_node[k] < 0 || !(c->cached >> t_cache_node[k] & 1ull)) slot = k;
                if (slot < 0) slot = 0;
                eval_node(c, g->nodes[j].in[0], t_cache[slot], depth + 1);
                t_cache_node[slot] = (int)j, c->cached |= 1ull << j;
            }
            memcpy(out, t_cache[slot], sizeof(AnimXf) * m->nbones);
            return;
        }
        bind_pose(m, out);
        return;
    case AN_MIRROR: {
        eval_input(c, n, 0, out, depth);
        if (!(n->var[0][0] ? var_of(c->w, in, n->var[0]) != 0.0f : (n->flags & 1))) return;
        memcpy(tmp, out, sizeof(AnimXf) * m->nbones);
        for (uint32_t b = 0; b < m->nbones; b++) mm_mirror_xf(&tmp[in->mirror_of[b] >= 0 ? in->mirror_of[b] : (int16_t)b], &out[b]);
        return;
    }
    case AN_INERTIALIZATION: { // a jump in the input (a switch upstream) eases out: its offset from the last output decays
        eval_input(c, n, 0, out, depth);
        if (!in->inert || in->inert_node != ni) return;
        float tau = n->f[0] > 0.02f ? n->f[0] : 0.2f, k = expf(-in->pose_dt * 4.0f / tau);
        AnimXf* last_in = in->inert, *last_out = in->inert + ANIM_MAX_BONES, *off = in->inert + 2 * ANIM_MAX_BONES;
        bool jump = false;
        for (uint32_t b = 0; b < m->nbones && in->inert_ok && !jump; b++) {
            float dot = fabsf(out[b].q[0] * last_in[b].q[0] + out[b].q[1] * last_in[b].q[1] + out[b].q[2] * last_in[b].q[2] + out[b].q[3] * last_in[b].q[3]);
            float dx = out[b].t[0] - last_in[b].t[0], dy = out[b].t[1] - last_in[b].t[1], dz = out[b].t[2] - last_in[b].t[2];
            jump = dot < 0.985f || dx * dx + dy * dy + dz * dz > 100.0f; // (over ~10 degrees or 10 cm in a frame)
        }
        for (uint32_t b = 0; b < m->nbones; b++) {
            last_in[b] = out[b];
            if (jump) { // the offset from the new input to where the pose was
                float inv[4] = { -out[b].q[0], -out[b].q[1], -out[b].q[2], out[b].q[3] };
                q_mul(last_out[b].q, inv, off[b].q);
                for (int a = 0; a < 3; a++) off[b].t[a] = last_out[b].t[a] - out[b].t[a];
            } else if (in->inert_ok) {
                float id[4] = { 0, 0, 0, 1 };
                q_nlerp(id, off[b].q, k, off[b].q);
                for (int a = 0; a < 3; a++) off[b].t[a] *= k;
            } else
                off[b].q[0] = off[b].q[1] = off[b].q[2] = 0.0f, off[b].q[3] = 1.0f, off[b].t[0] = off[b].t[1] = off[b].t[2] = 0.0f;
            float r[4];
            q_mul(off[b].q, out[b].q, r);
            memcpy(out[b].q, r, sizeof r);
            for (int a = 0; a < 3; a++) out[b].t[a] += off[b].t[a];
            last_out[b] = out[b];
        }
        in->inert_ok = true;
        return;
    }
    default: break;
    }
    // the skeletal controls: on their input, in the actor's space
    eval_input(c, n, 0, out, depth);
    const float (*rows)[4] = c->rows;
    float(*bones)[3][4] = c->bones;
    float A[3][3];
    rows_rot(rows, A);
    float fwd[3] = { A[0][0], A[1][0], A[2][0] }, right[3] = { A[0][1], A[1][1], A[2][1] }, up[3] = { A[0][2], A[1][2], A[2][2] };
    switch (n->kind) {
    case AN_AIM_OFFSET: {
        float yaw = var_of(c->w, in, n->var[0]) * 0.0174533f, pitch = var_of(c->w, in, n->var[1]) * 0.0174533f;
        turn_bones(m, out, rows, bones, nb, n->f, 4, up, yaw, right, -pitch); // (+ pitch: X toward up)
        return;
    }
    case AN_ORIENT_WARP: { // (2026-11-23) the pelvis turned toward the movement, the spine turned back: the legs walk that way, the chest faces on
        if (nb[0] < 0) return;
        float want = n->var[0][0] ? var_of(c->w, in, n->var[0]) : in->mv_speed > 10.0f ? in->mv_angle : 0.0f;
        if (n->flags & 1) want = remainderf(want, 90.0f); // (four directions: the rest past the nearest one's clip)
        else if (fabsf(want) > 100.0f) want = remainderf(want + 180.0f, 360.0f); // (backward: a backward clip's, turned the short way)
        float lim = fminf(fmaxf(n->f[0], 0.0f), 180.0f);
        want = fminf(fmaxf(want, -lim), lim) * fminf(fmaxf(n->f[1], 0.0f), 1.0f);
        float k = fminf(in->pose_dt * fmaxf(n->f[2], 0.5f), 1.0f); // (eased: no snap when the direction jumps)
        in->ow_angle += (want - in->ow_angle) * k;
        float a = -in->ow_angle * 0.0174533f; // (+ angle: the move to the right, a turn about up the other way: right-handed, z up)
        turn_bones(m, out, rows, bones, nb, NULL, 1, up, a, right, 0.0f);
        turn_bones(m, out, rows, bones, nb + 1, NULL, 3, up, -a, right, 0.0f);
        return;
    }
    case AN_STRIDE_WARP: { // (2026-11-23) the feet along the move by speed / the animation's speed, the legs reaching them
        int16_t fl = nb[0], fr = nb[1], pv = nb[2];
        if (fl < 0 || fr < 0) return;
        anim_world(m, out, rows, bones);
        // the animation's own speed: its planted foot (the lower one) sliding back under the body in the actor's space
        float ft[2] = { 0, 0 }, fz[2] = { bones[fl][2][3], bones[fr][2][3] };
        for (int s = 0; s < 2; s++) {
            const float(*F)[4] = bones[s ? fr : fl];
            float d[3] = { F[0][3] - rows[0][3], F[1][3] - rows[1][3], F[2][3] - rows[2][3] };
            ft[s] = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        }
        float dt = in->pose_dt;
        if (in->sw_has && dt > 1e-4f) {
            int st = fz[0] < fz[1] ? 0 : 1;
            float v = -(ft[st] - in->sw_prev[st]) / dt;
            if (v > 0.0f && v < 2000.0f) in->sw_anim_speed += (v - in->sw_anim_speed) * fminf(dt * 6.0f, 1.0f);
        }
        in->sw_prev[0] = ft[0], in->sw_prev[1] = ft[1], in->sw_has = true;
        float scale = n->var[0][0] ? var_of(c->w, in, n->var[0]) : in->sw_anim_speed > 15.0f && in->mv_speed > 5.0f ? in->mv_speed / in->sw_anim_speed : 1.0f;
        scale = fminf(fmaxf(scale, fmaxf(n->f[0], 0.1f)), fmaxf(n->f[1], n->f[0]));
        in->sw_scale += (scale - in->sw_scale) * fminf(dt * 8.0f, 1.0f);
        if (in->sw_scale <= 0.0f) in->sw_scale = 1.0f;
        float alpha = fminf(fmaxf(n->f[2], 0.0f), 1.0f), sc = 1.0f + (in->sw_scale - 1.0f) * alpha;
        if (fabsf(sc - 1.0f) < 1e-3f) return;
        // the pelvis lowered a little for a longer stride (its legs reach further)
        if (pv >= 0 && sc > 1.0f && n->f[3] > 0.0f) {
            int p = m->bones[pv].parent;
            const float(*P)[4] = p >= 0 ? bones[p] : rows;
            float drop = -fminf((sc - 1.0f) * n->f[3], 20.0f), d[3] = { up[0] * drop, up[1] * drop, up[2] * drop };
            for (int ax = 0; ax < 3; ax++) {
                float l2 = P[0][ax] * P[0][ax] + P[1][ax] * P[1][ax] + P[2][ax] * P[2][ax];
                out[pv].t[ax] += l2 > 1e-12f ? (P[0][ax] * d[0] + P[1][ax] * d[1] + P[2][ax] * d[2]) / l2 : 0.0f;
            }
            anim_world(m, out, rows, bones);
        }
        // each foot: its place along the move from under the hips scaled, reached by the leg (thigh, calf)
        float hip[3] = { 0, 0, 0 };
        if (pv >= 0) hip[0] = bones[pv][0][3], hip[1] = bones[pv][1][3], hip[2] = bones[pv][2][3];
        for (int s = 0; s < 2; s++) {
            int16_t fb = s ? fr : fl, ca = m->bones[fb].parent, th = ca >= 0 ? m->bones[ca].parent : -1;
            if (th < 0) continue;
            float F[3] = { bones[fb][0][3], bones[fb][1][3], bones[fb][2][3] };
            float along = (F[0] - hip[0]) * fwd[0] + (F[1] - hip[1]) * fwd[1] + (F[2] - hip[2]) * fwd[2];
            float T[3];
            for (int k = 0; k < 3; k++) T[k] = F[k] + fwd[k] * along * (sc - 1.0f);
            two_bone_to(m, out, rows, bones, th, ca, fb, T, 1.0f);
        }
        return;
    }
    case AN_LEAN: {
        float roll = fminf(fmaxf(-in->lean_rate * 0.06f, -16.0f), 16.0f) * n->f[0] * 0.0174533f;
        float pitch = fminf(fmaxf(in->lean_accel * 0.006f, -8.0f), 10.0f) * n->f[0] * 0.0174533f;
        turn_bones(m, out, rows, bones, nb, NULL, 3, fwd, roll, right, pitch);
        return;
    }
    case AN_MODIFY_BONE: {
        if (nb[0] < 0) return;
        float a = fminf(fmaxf(n->var[0][0] ? var_of(c->w, in, n->var[0]) : n->f[6], 0.0f), 1.0f);
        if (a <= 0.0f) return;
        // the turn about the actor's axes (roll X, pitch Y, yaw Z), then the move (actor space, cm)
        float qx[4], qy[4], qz[4], q[4], t1[4];
        q_axis(fwd, n->f[0] * 0.0174533f * a, qx), q_axis(right, n->f[1] * 0.0174533f * a, qy), q_axis(up, n->f[2] * 0.0174533f * a, qz);
        q_mul(qz, qy, t1), q_mul(t1, qx, q);
        float Rq[3][3], D[3][3], W[3][3];
        q_axes(q, Rq);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) D[j][i] = Rq[i][j];
        anim_world(m, out, rows, bones);
        rows_rot(bones[nb[0]], W);
        m_mul(D, W, W);
        set_world_rot(m, out, (uint32_t)nb[0], W, bones, rows);
        if (n->f[3] != 0.0f || n->f[4] != 0.0f || n->f[5] != 0.0f) {
            int p = m->bones[nb[0]].parent;
            anim_world(m, out, rows, bones);
            const float(*P)[4] = p >= 0 ? bones[p] : rows;
            float d[3];
            for (int k = 0; k < 3; k++) d[k] = (fwd[k] * n->f[3] + right[k] * n->f[4] + up[k] * n->f[5]) * a;
            for (int ax = 0; ax < 3; ax++) {
                float l2 = P[0][ax] * P[0][ax] + P[1][ax] * P[1][ax] + P[2][ax] * P[2][ax];
                out[nb[0]].t[ax] += l2 > 1e-12f ? (P[0][ax] * d[0] + P[1][ax] * d[1] + P[2][ax] * d[2]) / l2 : 0.0f;
            }
        }
        return;
    }
    case AN_LOOK_AT: { // the bone turned by the angle between the body's facing and the view point (clamped)
        if (nb[0] < 0 || !A_view_ok) return;
        anim_world(m, out, rows, bones);
        float d[3] = { A_view[0] - bones[nb[0]][0][3], A_view[1] - bones[nb[0]][1][3], A_view[2] - bones[nb[0]][2][3] };
        float yaw = atan2f(d[0] * right[0] + d[1] * right[1] + d[2] * right[2], d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]);
        float hz = sqrtf(powf(d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2], 2.0f) + powf(d[0] * right[0] + d[1] * right[1] + d[2] * right[2], 2.0f));
        float pitch = atan2f(d[0] * up[0] + d[1] * up[1] + d[2] * up[2], hz);
        float lim = fminf(fmaxf(n->f[0], 0.0f), 180.0f) * 0.0174533f, a = fminf(fmaxf(n->f[1], 0.0f), 1.0f);
        if (fabsf(yaw) > 2.6f) return; // (behind: the head doesn't turn around)
        yaw = fminf(fmaxf(yaw, -lim), lim) * a, pitch = fminf(fmaxf(pitch, -lim * 0.6f), lim * 0.6f) * a;
        turn_bones(m, out, rows, bones, nb, NULL, 1, up, -yaw, right, -pitch);
        return;
    }
    case AN_TWO_BONE_IK: {
        int16_t fb = nb[0];
        if (fb < 0) return;
        int16_t ca = m->bones[fb].parent, th = ca >= 0 ? m->bones[ca].parent : -1;
        if (th < 0) return;
        anim_world(m, out, rows, bones);
        float T[3];
        const float(*src)[4] = nb[1] >= 0 ? bones[nb[1]] : bones[fb];
        for (int k = 0; k < 3; k++) T[k] = src[k][3] + fwd[k] * n->f[0] + right[k] * n->f[1] + up[k] * n->f[2];
        two_bone_to(m, out, rows, bones, th, ca, fb, T, fminf(fmaxf(n->f[3], 0.0f), 1.0f));
        return;
    }
    case AN_LEG_IK: { // each foot on the ground under it, the pelvis down for the lower one (the old Foot IK)
        int16_t lf = nb[0], rf = nb[1];
        if (lf < 0 || rf < 0) return;
        int16_t leg[2][3];
        for (int f = 0; f < 2; f++) {
            int16_t foot = f ? rf : lf, calf = m->bones[foot].parent, thigh = calf >= 0 ? m->bones[calf].parent : -1;
            leg[f][0] = thigh, leg[f][1] = calf, leg[f][2] = foot;
            if (thigh < 0) return;
        }
        int16_t pv = m->bones[leg[0][0]].parent;
        float reach = n->f[0] > 1.0f ? n->f[0] : 45.0f, ground = rows[2][3], target_off[2] = { 0.0f, 0.0f };
        bool grounded = !c->chr || c->chr->on_ground;
        anim_world(m, out, rows, bones);
        float foot0[2][3];
        for (int f = 0; f < 2; f++) {
            for (int k = 0; k < 3; k++) foot0[f][k] = bones[leg[f][2]][k][3];
            if (!grounded) continue;
            float a[3] = { foot0[f][0], foot0[f][1], ground + reach }, b[3] = { foot0[f][0], foot0[f][1], ground - reach };
            float t = bp_trace_boxes(a, b, 0.0f);
            if (t < 1.0f) target_off[f] = (a[2] + (b[2] - a[2]) * t) - ground;
        }
        for (int f = 0; f < 2; f++) in->foot_off[f] += (target_off[f] - in->foot_off[f]) * 0.35f;
        in->pelvis_off += (fminf(fminf(in->foot_off[0], in->foot_off[1]), 0.0f) - in->pelvis_off) * 0.35f;
        if (fabsf(in->foot_off[0]) < 0.05f && fabsf(in->foot_off[1]) < 0.05f && fabsf(in->pelvis_off) < 0.05f) return;
        if (pv >= 0 && in->pelvis_off < 0.0f) {
            int p = m->bones[pv].parent;
            const float(*P)[4] = p >= 0 ? bones[p] : rows;
            float d[3] = { 0.0f, 0.0f, in->pelvis_off };
            for (int ax = 0; ax < 3; ax++) {
                float l2 = P[0][ax] * P[0][ax] + P[1][ax] * P[1][ax] + P[2][ax] * P[2][ax];
                out[pv].t[ax] += l2 > 1e-12f ? (P[0][ax] * d[0] + P[1][ax] * d[1] + P[2][ax] * d[2]) / l2 : 0.0f;
            }
        }
        for (int f = 0; f < 2; f++) {
            float T[3] = { foot0[f][0], foot0[f][1], foot0[f][2] + in->foot_off[f] };
            two_bone_to(m, out, rows, bones, leg[f][0], leg[f][1], leg[f][2], T, 1.0f);
        }
        return;
    }
    default: return;
    }
}

// The nodes' bones by name and their masks, once per graph revision (ik_bind calls it)
static void nodes_bind(AnimInst* in, const SkelMesh* m) {
    const AnimGraph* g = in->graph;
    in->inert_node = -1;
    for (uint32_t i = 0; i < g->nnodes && i < ANIM_MAX_NODES; i++) {
        const AnimNode* n = &g->nodes[i];
        for (int k = 0; k < 4; k++) in->node_bone[i][k] = bone_named(m, n->bone[k]);
        memset(in->node_mask[i], 0, sizeof in->node_mask[i]);
        if (n->kind == AN_LAYERED_BLEND) bone_mask(m, n->bone[0], in->node_mask[i]);
        if (n->kind == AN_INERTIALIZATION && in->inert_node < 0) in->inert_node = (int)i;
    }
    for (uint32_t b = 0; b < m->nbones; b++) in->mirror_of[b] = mm_pair(m, b);
}

static void nodes_pose(World* w, AnimInst* in, AnimXf* pose, const float rows[3][4], float bones[][3][4], const CCharacter* chr) {
    EvalCtx c = { w, in, rows, bones, chr, 0 };
    for (int k = 0; k < 4; k++) t_cache_node[k] = -1;
    eval_node(&c, output_node(in->graph), pose, 0);
}

// --- The nodes' self test (--anim-selftest): a made-up skeleton (root, spine, head, hand_l, hand_r) and a graph of the
// nodes that need no clip: Save / Use Cached Pose, Transform (Modify) Bone, Blend, Blend Poses by bool, Mirror ---
static int anim_nodes_selftest(void) {
    int fails = 0;
#define NCHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: AnimGraph nodes: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
    static SkelMesh m;
    static AnimGraph g;
    static AnimInst in;
    memset(&m, 0, sizeof m), memset(&g, 0, sizeof g), memset(&in, 0, sizeof in);
    static const struct { const char* name; int parent; float t[3]; } bones[5] = {
        { "root", -1, { 0, 0, 0 } }, { "spine_01", 0, { 0, 0, 100 } }, { "head", 1, { 0, 0, 60 } }, { "hand_l", 1, { 0, 40, 40 } }, { "hand_r", 1, { 0, -40, 40 } },
    };
    m.nbones = 5;
    for (int b = 0; b < 5; b++) {
        snprintf(m.bones[b].name, ANIM_NAME, "%s", bones[b].name);
        m.bones[b].parent = (int16_t)bones[b].parent;
        memcpy(m.bones[b].bind.t, bones[b].t, 12);
        m.bones[b].bind.q[3] = 1.0f, m.bones[b].bind.s[0] = m.bones[b].bind.s[1] = m.bones[b].bind.s[2] = 1.0f;
    }
    // Output <- Blend(0.5: A = Use Base, B = Modify head yaw 40 <- Use Base); Save Base <- the bind pose
    int save = add_node(&g, AN_SAVE_CACHE, 0, 0), use1 = add_node(&g, AN_USE_CACHE, 0, 0), use2 = add_node(&g, AN_USE_CACHE, 0, 0);
    snprintf(g.nodes[save].name, ANIM_NAME, "Base"), snprintf(g.nodes[use1].name, ANIM_NAME, "Base"), snprintf(g.nodes[use2].name, ANIM_NAME, "Base");
    int mod = add_node(&g, AN_MODIFY_BONE, 0, 0), blend = add_node(&g, AN_BLEND, 0, 0), out = add_node(&g, AN_OUTPUT, 0, 0);
    snprintf(g.nodes[mod].bone[0], ANIM_NAME, "head"), g.nodes[mod].f[2] = 40.0f, g.nodes[mod].f[6] = 1.0f, g.nodes[mod].in[0] = (int16_t)use2;
    g.nodes[blend].in[0] = (int16_t)use1, g.nodes[blend].in[1] = (int16_t)mod, g.nodes[blend].f[0] = 0.5f;
    g.nodes[out].in[0] = (int16_t)blend;
    // Blend Poses by bool (no variable: false, pin 1, over 0.2 s); a Mirror of hand_l turned 30 degrees
    int bsel = add_node(&g, AN_BLEND_BOOL, 0, 0), hmod = add_node(&g, AN_MODIFY_BONE, 0, 0), mir = add_node(&g, AN_MIRROR, 0, 0);
    snprintf(g.nodes[hmod].bone[0], ANIM_NAME, "hand_l"), g.nodes[hmod].f[0] = 30.0f, g.nodes[hmod].f[6] = 1.0f;
    g.nodes[mir].in[0] = (int16_t)hmod;
    g.nodes[bsel].in[0] = (int16_t)use1, g.nodes[bsel].in[1] = (int16_t)mod, g.nodes[bsel].f[0] = 0.2f;
    g.nmachines = 1;
    in.mesh = &m, in.graph = &g, in.inert_node = -1;
    ik_bind(&in, &m);
    nodes_logic(NULL, &in, 1.0f / 60.0f);
    static const float rows[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    static float wb[ANIM_MAX_BONES][3][4];
    static AnimXf pose[ANIM_MAX_BONES], lp[ANIM_MAX_BONES];
    nodes_pose(NULL, &in, pose, rows, wb, NULL);
    // the head turned half of 40 degrees about up (the blend's alpha); hand_l untouched
    float ang = 2.0f * atan2f(fabsf(pose[2].q[2]), fabsf(pose[2].q[3])) * 57.29578f;
    NCHECK(fabsf(ang - 20.0f) < 0.6f, "Blend 0.5 of a 40 degree Modify Bone turned the head %.2f degrees", ang);
    NCHECK(fabsf(pose[3].q[3] - 1.0f) < 1e-5f, "a bone the graph didn't touch moved");
    // Blend Poses by bool: false eases in over its 0.2 s: 0.25 after 0.05 s, all of it after 0.2 s
    in.node_w[bsel][0] = 1.0f, in.node_w[bsel][1] = 0.0f;
    g.nodes[out].in[0] = (int16_t)bsel;
    nodes_logic(NULL, &in, 0.05f);
    NCHECK(fabsf(in.node_w[bsel][1] - 0.25f) < 1e-4f && fabsf(in.node_w[bsel][0] - 0.75f) < 1e-4f, "bool blend weights %.3f / %.3f after 0.05 s",
           in.node_w[bsel][0], in.node_w[bsel][1]);
    for (int k = 0; k < 4; k++) nodes_logic(NULL, &in, 0.05f);
    NCHECK(in.node_w[bsel][1] > 0.999f && in.node_w[bsel][0] < 0.001f, "bool blend not done after its time");
    // Mirror: hand_l's turn shows on hand_r, mirrored (y -> -y); hand_l gets hand_r's (none)
    EvalCtx c = { NULL, &in, rows, wb, NULL, 0 };
    for (int k = 0; k < 4; k++) t_cache_node[k] = -1;
    eval_node(&c, hmod, lp, 0);
    eval_node(&c, mir, pose, 0);
    AnimXf want;
    mm_mirror_xf(&lp[3], &want);
    NCHECK(fabsf(pose[4].q[0] - want.q[0]) < 1e-4f && fabsf(pose[4].q[2] - want.q[2]) < 1e-4f && fabsf(pose[4].q[3] - want.q[3]) < 1e-4f,
           "Mirror: hand_r is not hand_l's turn mirrored");
    NCHECK(fabsf(lp[3].q[3] - 1.0f) > 1e-3f && fabsf(pose[3].q[3] - 1.0f) < 1e-4f, "Mirror: hand_l kept its turn");
#undef NCHECK
    return fails;
}
