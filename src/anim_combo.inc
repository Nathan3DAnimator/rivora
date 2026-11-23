// (2026-11-23, notas-dev/animacao.md "Janelas de combo e de acerto") Included by anim.c after anim_traversal.inc.
// A montage's windows (Unreal's Anim Notify States, the ones combat is made of):
//   - State: Event On Montage Window at its begin and its end (a parry, an invulnerability, a trail);
//   - Combo: the player's input inside it (Montage Combo Input, or 0.15 s before it) chains: at its end the montage
//     jumps to its Next Section; without it the montage ends with the section it is in (the attack is over);
//   - Hit: a sphere of its Radius swept along its Bone each frame: Event On Montage Hit once per actor it touches.
// Is In Montage Window says whether one of a name is open.

#define COMBO_BUFFER 0.15f // (an input this early still counts: the button pressed just before the window)
#define HIT_SEEN 16

void bp_montage_window_event(World* w, Entity actor, const char* name, bool begin); // (bp_vm.c)
void bp_montage_hit_event(World* w, Entity actor, Entity other, const float loc[3], const char* window);

static struct ComboState {
    Entity actor;
    const AnimMontage* mo;
    uint32_t open;           // the windows open (bit per window)
    float input_t;           // the montage time of the last combo input (-1: none)
    float stop_at;           // the section's end it stops at (-1: none)
    Entity seen[ANIM_MAX_WINDOWS][HIT_SEEN];
    uint8_t nseen[ANIM_MAX_WINDOWS];
    float prev[ANIM_MAX_WINDOWS][3];
    bool has_prev[ANIM_MAX_WINDOWS];
    uint32_t hits;           // (the test's count)
} g_combo[64];
static uint32_t g_ncombo;

static struct ComboState* combo_of(Entity actor, bool make) {
    for (uint32_t i = 0; i < g_ncombo; i++)
        if (g_combo[i].actor == actor) return &g_combo[i];
    if (!make || g_ncombo >= 64) return NULL;
    struct ComboState* c = &g_combo[g_ncombo++];
    memset(c, 0, sizeof *c);
    c->actor = actor, c->input_t = -1.0f, c->stop_at = -1.0f;
    return c;
}

// the section a time is in: its start and the next one's (the clip's end for the last)
static void combo_section(const AnimMontage* mo, float len, float t, float* s0, float* s1) {
    *s0 = 0.0f, *s1 = len;
    for (uint32_t i = 0; i < mo->nsections; i++) {
        float st = mo->sections[i].start;
        if (st <= t + 1e-4f && st >= *s0) *s0 = st;
    }
    for (uint32_t i = 0; i < mo->nsections; i++) {
        float st = mo->sections[i].start;
        if (st > *s0 + 1e-4f && st < *s1) *s1 = st;
    }
}

// A montage's step (step_logic, after its root motion): the windows opened and closed, the combo chained, the hits
static void combo_step(World* w, AnimInst* in, float t0, float t1) {
    const AnimMontage* mo = in->montage;
    Entity actor = in->actor ? in->actor : in->e;
    if (!mo || (!mo->nwindows && !combo_of(actor, false))) return;
    struct ComboState* c = combo_of(actor, mo->nwindows != 0);
    if (!c) return;
    if (c->mo != mo || t1 < t0 - 1e-4f) { // (another montage, or it started again: nothing open, no input)
        c->mo = mo, c->open = 0, c->input_t = -1.0f, c->stop_at = -1.0f;
        memset(c->nseen, 0, sizeof c->nseen), memset(c->has_prev, 0, sizeof c->has_prev);
    }
    float len = in->mclip.seq ? in->mclip.seq->length : t1;
    for (uint32_t k = 0; k < mo->nwindows; k++) {
        const AnimMontageWindow* wn = &mo->windows[k];
        bool was = (c->open >> k) & 1u, now = t1 >= wn->start && t1 < wn->end;
        if (now && !was) {
            c->open |= 1u << k, c->nseen[k] = 0, c->has_prev[k] = false;
            bp_montage_window_event(w, actor, wn->name, true);
        }
        if (now && wn->kind == ANIM_WIN_HIT && wn->bone[0]) { // the sweep from where the bone was to where it is
            float rows[3][4];
            if (anim_socket_world(w, actor, wn->bone, rows)) {
                float p[3] = { rows[0][3], rows[1][3], rows[2][3] };
                if (c->has_prev[k]) {
                    SqQuery q;
                    SqHit hits[8];
                    memset(&q, 0, sizeof q);
                    memcpy(q.start, c->prev[k], 12), memcpy(q.end, p, 12);
                    q.shape = SQ_SPHERE, q.radius = wn->radius > 1.0f ? wn->radius : 20.0f, q.nignore = 1, q.ignore = &actor;
                    uint32_t n = sq_trace_multi(w, &q, hits, 8);
                    for (uint32_t h = 0; h < n; h++) {
                        Entity o = hits[h].actor;
                        bool seen = !o;
                        for (uint32_t s = 0; s < c->nseen[k] && !seen; s++) seen = c->seen[k][s] == o;
                        if (seen) continue;
                        if (c->nseen[k] < HIT_SEEN) c->seen[k][c->nseen[k]++] = o;
                        c->hits++;
                        bp_montage_hit_event(w, actor, o, hits[h].impact_point, wn->name);
                    }
                }
                memcpy(c->prev[k], p, 12), c->has_prev[k] = true;
            }
        }
        if (!now && was) {
            c->open &= ~(1u << k);
            bp_montage_window_event(w, actor, wn->name, false);
            if (wn->kind == ANIM_WIN_COMBO) {
                const float* sec = NULL;
                for (uint32_t i = 0; i < mo->nsections && wn->next[0]; i++)
                    if (!_stricmp(mo->sections[i].name, wn->next)) sec = &mo->sections[i].start;
                if (c->input_t >= wn->start - COMBO_BUFFER && c->input_t < wn->end && sec) { // chained: on to the next section
                    in->mtime = *sec, c->input_t = -1.0f, c->stop_at = -1.0f, c->open = 0;
                    memset(c->nseen, 0, sizeof c->nseen), memset(c->has_prev, 0, sizeof c->has_prev);
                    return;
                }
                if (sec) { // not chained: the attack ends with its section
                    float s0, s1;
                    combo_section(mo, len, wn->start, &s0, &s1);
                    c->stop_at = s1;
                }
            }
        }
    }
    if (c->stop_at >= 0.0f && t1 >= c->stop_at - (in->mblend_out > 0.0f ? in->mblend_out : mo->blend_out) && !in->mstopping) {
        in->mstopping = true, in->mcut = true; // (blended out from its section's end: the rest isn't played)
        in->mblend_out = fmaxf(mo->blend_out, 0.05f);
        c->stop_at = -1.0f;
    }
}

// The player's attack button while a combo montage plays (Montage Combo Input): kept for the window's end
bool anim_montage_combo_input(World* w, Entity actor) {
    AnimInst* in = inst_of_actor(w, actor);
    if (!in || !in->montage) return false;
    struct ComboState* c = combo_of(actor, true);
    if (!c) return false;
    const AnimMontage* mo = in->montage;
    for (uint32_t k = 0; k < mo->nwindows; k++)
        if (mo->windows[k].kind == ANIM_WIN_COMBO && in->mtime >= mo->windows[k].start - COMBO_BUFFER && in->mtime < mo->windows[k].end) {
            c->input_t = in->mtime;
            return true;
        }
    return false;
}
bool anim_montage_window_open(World* w, Entity actor, const char* name) {
    (void)w;
    struct ComboState* c = combo_of(actor, false);
    if (!c || !c->mo || !name) return false;
    for (uint32_t k = 0; k < c->mo->nwindows; k++)
        if (((c->open >> k) & 1u) && !_stricmp(c->mo->windows[k].name, name)) return true;
    return false;
}
uint32_t anim_montage_hit_count(Entity actor) {
    struct ComboState* c = combo_of(actor, false);
    return c ? c->hits : 0;
}
static void combo_end_play(void) { g_ncombo = 0; }
