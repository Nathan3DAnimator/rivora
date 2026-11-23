// --- The Motion Matching editor (included by anim_editor.c): Unreal's Pose Search Database editor, for a Motion
// Matching state's (or node's) database. Database: its clips and the groups made from them (time-warped, mirrored), the
// made animations (directions in between, starts, stops, speed changes: anim_mm_gen.inc) with their settings, each
// one played in the viewport with its features (and walking over the ground: the feet must not slide). Query Debugger:
// the game's play of the database (anim_mm_sim_step) from a speed, a direction and an acceleration you set, or from a
// target speed the body reaches at its own limits (the Character's movement), the chosen frames blended in as in the
// game, the trajectory and the feet drawn from above, every group's speed against the query's, the costs and the switches ---

#define MMED_GROUPS 2048
static struct {
    bool open, playing, travel;
    BpAsset* a;
    int state, node;        // the database's owner: a state (index) or a pose node (index)
    AssetViewport vp; // (Q7) the asset viewport
    int mode;               // 0 Database, 1 Query Debugger
    int group;              // Database: the group shown
    float t, play_rate;
    // the query and its play (the game's rules: a search every 0.1 s, a switch when 15% better, 0.2 s blend); move: the
    // speed follows q_target at the body's limits (the acceleration is then the speed's change)
    float q_speed, q_dir, q_accel, q_target, q_turn; // (q_turn: the facing's turn asked, degrees)
    bool move;
    AnimMmPlay play;
    AnimMmMatch last;
    float cost_hist[120], walk[2];
    int hist_at;
    float gen[ANIM_MMGEN];  // the settings being edited (applied when a control is let go: the database is made again)
    bool gen_live;
    char saved[ANIM_NAME]; // (the last made animation saved as a file)
} M = { .play_rate = 1.0f, .play = { .cur = -1, .prev = -1 }, .q_speed = 200.0f, .q_target = 200.0f, .move = true, .travel = true };

static void mm_editor_open(BpAsset* a, int state, int node) {
    bool same = M.open && M.a == a && M.state == state && M.node == node;
    M.open = true, M.a = a, M.state = state, M.node = node;
    if (!same) {
        M.vp.cam.framed = false;
        M.group = 0, M.t = 0.0f, M.playing = true, M.hist_at = 0, M.gen_live = false;
        memset(&M.play, 0, sizeof M.play), M.play.cur = M.play.prev = -1;
        memset(M.cost_hist, 0, sizeof M.cost_hist);
    }
}

// The database's owner as a state (a node's stand-in)
static bool mm_owner(AnimGraph* g, AnimState* tmp, AnimState** s, const char** title) {
    if (M.state >= 0 && M.state < (int)g->nstates && g->states[M.state].kind == ANIM_STATE_MOTION_MATCH) {
        *s = &g->states[M.state], *title = g->states[M.state].name;
        return true;
    }
    if (M.node >= 0 && M.node < (int)g->nnodes && g->nodes[M.node].kind == AN_MOTION_MATCH) {
        anim_node_state(&g->nodes[M.node], tmp);
        snprintf(tmp->name, ANIM_NAME, "Motion Matching");
        *s = tmp, *title = tmp->name;
        return true;
    }
    return false;
}

static const SkelMesh* mm_mesh(const AnimGraph* g, const AnimState* s) {
    const SkelMesh* m = g->skeleton[0] ? anim_mesh_name(g->skeleton) : NULL;
    for (uint32_t k = 0; !m && k < s->nsamples; k++) { // (no skeleton set: its first clip's)
        AnimSeq* q = anim_seq_name(s->samples[k]);
        if (q) m = anim_mesh_name(q->skeleton);
    }
    return m;
}

static void mm_restart(void) {
    memset(&M.play, 0, sizeof M.play), M.play.cur = M.play.prev = -1;
    M.walk[0] = M.walk[1] = 0.0f;
}

// The query's step: with "move", the speed reaches the target at the body's limits (as the Character does), the
// acceleration its change; then the game's step
static void mm_simulate(const AnimGraph* g, const SkelMesh* m, const AnimState* s, float dt) {
    if (dt <= 0.0f) return;
    if (M.move) {
        float lim = anim_mm_limit(g, m, s, M.q_speed, M.q_target);
        if (lim <= 0.0f) lim = 4096.0f; // (no made starts: the movement's own)
        float dv = M.q_target - M.q_speed;
        M.q_speed += fabsf(dv) < lim * dt ? dv : (dv > 0.0f ? lim * dt : -lim * dt);
        // the game's trajectory: the speed in 0.3 s, toward the target at the body's limits (as an acceleration)
        dv = M.q_target - M.q_speed;
        lim = anim_mm_limit(g, m, s, M.q_speed, M.q_target);
        if (lim <= 0.0f) lim = 4096.0f;
        M.q_accel = (fabsf(dv) < lim * 0.3f ? dv : (dv > 0.0f ? lim : -lim) * 0.3f) / 0.3f;
    }
    AnimMmMatch last = { 0 };
    uint32_t searches = M.play.searches;
    anim_mm_sim_step(g, m, s, &M.play, M.q_speed, M.q_dir, M.q_accel, M.q_turn, dt, &last);
    if (M.play.searches != searches) M.last = last, M.cost_hist[M.hist_at++ % 120] = last.cost;
    float d = M.q_dir * 0.0174533f;
    M.walk[0] += cosf(d) * M.q_speed * dt, M.walk[1] += sinf(d) * M.q_speed * dt;
}

// From above: the query's velocity now and in 0.3 s (white, gray), the playing frame's (orange), its feet (blue / green)
// and their velocities; forward is up
static void mm_trajectory(ImVec2 at, float size, const float* f, bool have) {
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_AddRectFilled(dl, at, (ImVec2){ at.x + size, at.y + size }, 0xFF1C1C1Cu, 4.0f, 0);
    ImVec2 c = { at.x + size * 0.5f, at.y + size * 0.62f };
    float k = size * 0.42f / 600.0f; // (600 cm/s to the edge)
    for (int r = 1; r <= 3; r++) ImDrawList_AddCircle(dl, c, size * 0.14f * r, 0xFF2C2C2Cu, 48, 1.0f);
    ImDrawList_AddLine(dl, (ImVec2){ c.x, at.y + 4 }, (ImVec2){ c.x, at.y + size - 4 }, 0xFF2C2C2Cu, 1.0f);
#define MM_PT(fx, fy, sc) ((ImVec2){ c.x + (fy) * (sc), c.y - (fx) * (sc) }) // (x forward: up; y right... root space y is left: shown right)
    float d = M.q_dir * 0.0174533f, fut = fminf(fmaxf(M.q_speed + M.q_accel * 0.3f, 0.0f), M.q_speed * 2.0f + 600.0f);
    ImDrawList_AddLine(dl, c, MM_PT(cosf(d) * fut, -sinf(d) * fut, k), 0xFF808080u, 2.0f);
    ImDrawList_AddLine(dl, c, MM_PT(cosf(d) * M.q_speed, -sinf(d) * M.q_speed, k), 0xFFFFFFFFu, 3.0f);
    if (fabsf(M.q_turn) > 0.5f) { // (the turn asked, in 0.5 s at most half a turn: thin yellow)
        float tu = fminf(fmaxf(M.q_turn, -180.0f), 180.0f) * 0.0174533f, fr = size * 0.3f;
        ImDrawList_AddLine(dl, c, MM_PT(cosf(tu) * fr, -sinf(tu) * fr, 1.0f), 0xFF60A0B0u, 1.0f);
    }
    if (have) {
        ImDrawList_AddLine(dl, c, MM_PT(f[0], -f[1], k), 0xFF2E9BFFu, 3.0f);
        ImDrawList_AddLine(dl, c, MM_PT(f[2], -f[3], k), 0xFF1E6BBFu, 1.5f);
        float fr = size * 0.3f; // (where the frame faces in 0.5 s: yellow)
        ImDrawList_AddLine(dl, c, MM_PT(f[6] * fr, -f[7] * fr, 1.0f), 0xFF30D0E0u, 2.0f);
        float fk = size * 0.42f / 120.0f; // (the feet: 120 cm to the edge)
        for (int s = 0; s < 2; s++) {
            ImVec2 p = MM_PT(f[10 + s * 3], -f[11 + s * 3], fk);
            uint32_t col = s ? 0xFF60D060u : 0xFFF0A040u;
            ImDrawList_AddCircleFilled(dl, p, 5.0f, col, 12);
            ImDrawList_AddLine(dl, p, (ImVec2){ p.x - f[17 + s * 2] * fk * 0.25f, p.y - f[16 + s * 2] * fk * 0.25f }, col, 1.5f);
        }
    }
#undef MM_PT
    ImDrawList_AddText_Vec2(dl, (ImVec2){ at.x + 6, at.y + 4 }, 0xFFB0B0B0u, TR("Trajectory (from above)"), NULL);
    if (have && (fabsf(f[8]) > 1.0f || fabsf(f[9]) > 1.0f)) { // (a jump's frame: its vertical root motion)
        char vz[96];
        snprintf(vz, sizeof vz, TR("Jump: %.0f cm/s up now, %.0f in 0.3 s"), f[8], f[9]);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ at.x + 6, at.y + 22 }, 0xFFB0B0B0u, vz, NULL);
    }
    igDummy((ImVec2){ size, size });
    igSetItemTooltip("%s", TR("White: the query's velocity now; gray: in 0.3 s. Orange: the matched frame's velocities. Dots: its feet (left blue, right green) and where they move."));
}

// Every group's speed on one axis, the query's speed, the playing group
static void mm_speeds(const AnimMmGroup* groups, uint32_t ng, int cur, float qspeed) {
    ImDrawList* dl = igGetWindowDrawList();
    float w = igGetContentRegionAvail().x, h = igGetFontSize() * 3.0f, mx = 100.0f;
    for (uint32_t i = 0; i < ng; i++) mx = fmaxf(mx, groups[i].speed);
    mx = fmaxf(mx, qspeed) * 1.1f;
    ImVec2 at = igGetCursorScreenPos();
    ImDrawList_AddRectFilled(dl, at, (ImVec2){ at.x + w, at.y + h }, 0xFF1C1C1Cu, 4.0f, 0);
    for (uint32_t i = 0; i < ng; i++) {
        float x = at.x + 6 + (w - 12) * groups[i].speed / mx, y0 = at.y + (groups[i].mirror ? h * 0.5f : 6.0f), y1 = y0 + h * 0.5f - 6.0f;
        ImDrawList_AddLine(dl, (ImVec2){ x, y0 }, (ImVec2){ x, y1 }, (int)i == cur ? 0xFF2E9BFFu : groups[i].mirror ? 0xFF8060A0u : 0xFF909090u,
                           (int)i == cur ? 3.0f : 1.5f);
    }
    float qx = at.x + 6 + (w - 12) * qspeed / mx;
    ImDrawList_AddLine(dl, (ImVec2){ qx, at.y + 2 }, (ImVec2){ qx, at.y + h - 2 }, 0xFFFFFFFFu, 2.0f);
    char lab[64];
    snprintf(lab, sizeof lab, "0 .. %.0f cm/s", mx);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ at.x + w - igCalcTextSize(lab, NULL, false, -1).x - 6, at.y + h - igGetFontSize() - 2 }, 0xFF808080u, lab, NULL);
    igDummy((ImVec2){ w, h });
    igSetItemTooltip("%s", TR("Each group's speed (top: as made, bottom: mirrored); white: the query's; blue: the one playing."));
}

static const char* mm_kind_name(int kind) {
    switch (kind) {
    case ANIM_MMG_DIRECTION: return TR("Directions in Between");
    case ANIM_MMG_START: return TR("Starts");
    case ANIM_MMG_STOP: return TR("Stops");
    case ANIM_MMG_SPEED: return TR("Speed Changes");
    default: return TR("Animations");
    }
}

// The made animations' settings (written to the state or the node when a control is let go: the database is made
// again with them) and the body's limits they give
static bool mm_gen_settings(AnimState* edit, AnimNode* node, const AnimMmInfo* info, bool ok) {
    float* dst = edit ? edit->mmgen : node->mmgen;
    if (!M.gen_live) {
        if (dst[ANIM_MMGEN_SET] == 1.0f) memcpy(M.gen, dst, sizeof M.gen);
        else anim_mm_gen_defaults(M.gen);
        M.gen_live = true;
    }
    bool commit = false;
    int off = (int)M.gen[ANIM_MMGEN_OFF];
    bool on[4] = { !(off & ANIM_MMGEN_NO_STARTS), !(off & ANIM_MMGEN_NO_SPEEDS), !(off & ANIM_MMGEN_NO_DIRECTIONS), !(off & ANIM_MMGEN_NO_MOVEMENT) };
    static const int bit[4] = { ANIM_MMGEN_NO_STARTS, ANIM_MMGEN_NO_SPEEDS, ANIM_MMGEN_NO_DIRECTIONS, ANIM_MMGEN_NO_MOVEMENT };
    const char* lab[4] = { TR("Starts and Stops"), TR("Speed Changes"), TR("Directions in Between"), TR("Movement Follows the Body") };
    const char* tip[4] = {
        TR("From the idle into each locomotion and back, from 8 places of its cycle: the speed ramps at the body's acceleration, the stride and the cadence grow with it, the planted feet stay put."),
        TR("Between neighboring speeds of the same direction (walk and run): both at the same phase, the cycle between theirs."),
        TR("Between two clips of about the same speed (forward and to the side), every few degrees: the stride warped back to the speed."),
        TR("The Character's acceleration and braking become the body's (the made starts' and stops'): it cannot start or stop faster than its legs, so the feet do not slide."),
    };
    for (int k = 0; k < 4; k++) {
        if (igCheckbox(lab[k], &on[k])) {
            off = on[k] ? off & ~bit[k] : off | bit[k];
            M.gen[ANIM_MMGEN_OFF] = (float)off, commit = true;
        }
        igSetItemTooltip("%s", tip[k]);
    }
    igPushItemWidth(-igGetFontSize() * 7.5f);
    char fmt[64];
    snprintf(fmt, sizeof fmt, M.gen[ANIM_MMGEN_ACCEL] > 0.0f ? "%%.0f cm/s2" : TR("Auto"));
    igDragFloat(PL(TR("Acceleration")), &M.gen[ANIM_MMGEN_ACCEL], 5.0f, 0.0f, 1200.0f, fmt, ImGuiSliderFlags_AlwaysClamp);
    commit |= igIsItemDeactivatedAfterEdit();
    igSetItemTooltip("%s", TR("The body's mean acceleration when it starts (0: from the clips' own timing: the steps to reach the speed, each as long as the clip's step)."));
    snprintf(fmt, sizeof fmt, M.gen[ANIM_MMGEN_DECEL] > 0.0f ? "%%.0f cm/s2" : TR("Auto"));
    igDragFloat(PL(TR("Deceleration")), &M.gen[ANIM_MMGEN_DECEL], 5.0f, 0.0f, 1500.0f, fmt, ImGuiSliderFlags_AlwaysClamp);
    commit |= igIsItemDeactivatedAfterEdit();
    igSetItemTooltip("%s", TR("The body's mean deceleration when it stops (0: from the clips' timing; stopping takes fewer steps than starting)."));
    igSliderFloat(PL(TR("Weight")), &M.gen[ANIM_MMGEN_WEIGHT], 0.25f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
    commit |= igIsItemDeactivatedAfterEdit();
    igSetItemTooltip("%s", TR("Heavier than the clips look (> 1): slower starts and stops, a slower sway. Lighter (< 1): snappier."));
    igSliderFloat(PL(TR("Lean")), &M.gen[ANIM_MMGEN_LEAN], 0.0f, 3.0f, "%.2f", 0);
    commit |= igIsItemDeactivatedAfterEdit();
    igSetItemTooltip("%s", TR("The lean into the acceleration (1: physical, the angle whose tangent is the acceleration over gravity; 0: none)."));
    igSliderFloat(PL(TR("Settle")), &M.gen[ANIM_MMGEN_SETTLE], 0.0f, 1.0f, "%.2f", 0);
    commit |= igIsItemDeactivatedAfterEdit();
    igSetItemTooltip("%s", TR("How much the body sways forward and back when it stops (a damped spring: 0 none, 1 a lot)."));
    igSliderFloat(PL(TR("Angle Step")), &M.gen[ANIM_MMGEN_STEP], 10.0f, 45.0f, "%.1f deg", 0);
    commit |= igIsItemDeactivatedAfterEdit();
    igSetItemTooltip("%s", TR("Degrees between the made directions (a clip forward and one to the side: one every step between them)."));
    igPopItemWidth();
    if (igButton(TR("Reset to Defaults"), (ImVec2){ -1, 0 })) anim_mm_gen_defaults(M.gen), commit = true;
    if (ok && info->nlim) {
        igTextDisabled("%s", TR("The body (from the clips):"));
        for (uint32_t i = 0; i < info->nlim; i++)
            igTextDisabled(TR("  %.0f cm/s: starts %.0f, stops %.0f cm/s2"), info->lim_speed[i], info->lim_acc[i], info->lim_dec[i]);
    }
    if (commit) {
        M.gen[ANIM_MMGEN_SET] = 1.0f;
        memcpy(dst, M.gen, sizeof M.gen);
    }
    return commit;
}

static void mm_editor_draw(void) {
    if (!M.open || !M.a) return;
    AnimGraph* g = anim_graph_of(M.a, false);
    AnimState tmp, *s = NULL;
    const char* owner = "";
    if (!g || !mm_owner(g, &tmp, &s, &owner)) {
        M.open = false;
        return;
    }
    const SkelMesh* m = mm_mesh(g, s);
    static AnimMmGroup groups[MMED_GROUPS];
    AnimMmInfo info;
    bool ok = m && anim_mm_info(g, m, s, &info, groups, MMED_GROUPS);
    uint32_t ng = ok ? (info.ngroups < MMED_GROUPS ? info.ngroups : MMED_GROUPS) : 0;
    char title[200];
    snprintf(title, sizeof title, "%s: %s - %s###mm_editor", M.a->name, owner, TR("Motion Matching Editor"));
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 76, igGetFontSize() * 44 }, ImGuiCond_FirstUseEver);
    bool open = true;
    if (!igBegin(title, &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        igEnd();
        if (!open) M.open = false;
        return;
    }
    bool ch = false, gen_ch = false;
    float dt = igGetIO_Nil()->DeltaTime * (M.playing ? M.play_rate : 0.0f);
    // the toolbar: the mode (Unreal's database editor / its debugger), play, the rate
    if (igRadioButton_Bool(TR("Database"), M.mode == 0)) M.mode = 0;
    igSameLine(0, 10);
    if (igRadioButton_Bool(TR("Query Debugger"), M.mode == 1)) M.mode = 1, mm_restart();
    igSameLine(0, 24);
    if (igButton(M.playing ? TR("Pause") : TR("Play"), (ImVec2){ igGetFontSize() * 5, 0 })) M.playing = !M.playing;
    igSameLine(0, 8);
    igSetNextItemWidth(igGetFontSize() * 8);
    igSliderFloat("##rate", &M.play_rate, 0.05f, 2.0f, TR("rate %.2fx"), 0);
    igSameLine(0, 12);
    igCheckbox(PL(TR("Walk Over the Ground")), &M.travel);
    igSetItemTooltip("%s", TR("The character moves by its speed in the viewport: a planted foot that slides shows against the grid."));
    igSameLine(0, 24);
    if (ok)
        igTextDisabled(TR("%u frames in %u groups (%s), built in %.1f ms (made ones %.1f ms)"), info.frames, info.ngroups,
                       info.can_mirror ? TR("mirrored") : TR("not mirrored"), info.build_ms, info.gen_ms);
    else
        igTextColored((ImVec4){ 1, 0.6f, 0.3f, 1 }, "%s", TR("No database: add moving clips of a skeleton with feet (foot_l / foot_r)."));
    ImVec2 av = igGetContentRegionAvail();
    float left = igGetFontSize() * 23.0f, right = igGetFontSize() * 21.0f, mid = av.x - left - right - 12.0f;
    // the left: the database's clips and their groups, the made ones and their settings
    igBeginChild_Str("mm_db", (ImVec2){ left, av.y }, ImGuiChildFlags_Borders, 0);
    igSeparatorText(TR("Animations"));
    AnimState* edit = M.state >= 0 ? s : NULL;
    AnimNode* node = M.node >= 0 ? &g->nodes[M.node] : NULL;
    uint8_t* nsamp = edit ? &edit->nsamples : &node->nsamples;
    char(*samples)[ANIM_NAME] = edit ? edit->samples : node->samples;
    for (uint32_t k = 0; k < *nsamp; k++) {
        igPushID_Int((int)k);
        AnimSeq* q = anim_seq_name(samples[k]);
        uint32_t gk = 0;
        for (uint32_t i = 0; i < ng; i++) gk += groups[i].kind == ANIM_MMG_CLIP && groups[i].clip_index == k;
        char head[160];
        snprintf(head, sizeof head, "%s  (%.0f cm/s, %u)###clip", samples[k][0] ? samples[k] : "-", ok ? info.clip_speed[k] : 0.0f, gk);
        bool tree = igTreeNodeEx_Str(head, ImGuiTreeNodeFlags_SpanAvailWidth);
        if (igBeginPopupContextItem("clip_menu", ImGuiPopupFlags_MouseButtonRight)) {
            if (ui_menu_item(ICON_DELETE, TR("Remove from the Database"), NULL, false, true)) {
                memmove(&samples[k], &samples[k + 1], sizeof samples[0] * (*nsamp - k - 1));
                (*nsamp)--, ch = true;
            }
            igEndPopup();
        }
        if (tree) {
            if (q) igTextDisabled(TR("%.2f s, %s"), q->length, q->loop ? TR("loop") : TR("once"));
            for (uint32_t i = 0; i < ng; i++) // (its root motion: taken out of the poses, the actor carries it)
                if (groups[i].kind == ANIM_MMG_CLIP && groups[i].clip_index == k && !groups[i].mirror) {
                    static const char* const rm[4] = { NULL, "root motion: the root bone's", "root motion: the pelvis's", "in place, its own velocity and turn" };
                    if (groups[i].root_motion && groups[i].root_motion < 4) igTextDisabled("%s", TR(rm[groups[i].root_motion]));
                    break;
                }
            for (uint32_t i = 0; i < ng; i++) {
                if (groups[i].kind != ANIM_MMG_CLIP || groups[i].clip_index != k) continue;
                char row[128];
                snprintf(row, sizeof row, "x%.2f %s  %.0f cm/s  %+.0f deg##g%u", groups[i].rate, groups[i].mirror ? TR("mirrored") : "", groups[i].speed,
                         groups[i].dir_deg, i);
                if (igSelectable_Bool(row, M.mode == 0 && M.group == (int)i, 0, (ImVec2){ 0, 0 })) M.group = (int)i, M.t = 0.0f, M.mode = 0;
            }
            igTreePop();
        }
        igPopID();
        if (ch) break;
    }
    if (*nsamp < ANIM_BLEND_SAMPLES) {
        static char add[ANIM_NAME];
        add[0] = 0;
        igSetNextItemWidth(-1);
        if (clip_combo("##add", add, g->skeleton) && add[0]) snprintf(samples[(*nsamp)++], ANIM_NAME, "%s", add), ch = true;
        igTextDisabled("%s", TR("+ Add an animation (above)"));
    }
    // the made ones, by kind
    igSeparatorText(TR("Made Animations"));
    hint(TR("Made from the clips above in their own style (no extra animations needed): an idle, a walk and a run give the starts, stops and speed changes; a clip forward and one to the side give the directions between them."));
    for (int kind = ANIM_MMG_DIRECTION; kind < ANIM_MMG_KINDS && ok; kind++) {
        char head[96];
        snprintf(head, sizeof head, "%s  (%u)###kind%d", mm_kind_name(kind), info.made[kind], kind);
        if (!igTreeNodeEx_Str(head, ImGuiTreeNodeFlags_SpanAvailWidth)) continue;
        for (uint32_t i = 0; i < ng; i++) {
            if (groups[i].kind != kind || (kind == ANIM_MMG_DIRECTION && groups[i].rate != 1.0f)) continue;
            char row[200];
            if (groups[i].loop)
                snprintf(row, sizeof row, "%s  %.0f cm/s##g%u", groups[i].label, groups[i].speed, i);
            else
                snprintf(row, sizeof row, "%s  %.2f s, %+.0f cm/s2##g%u", groups[i].label, groups[i].length, groups[i].accel, i);
            if (igSelectable_Bool(row, M.mode == 0 && M.group == (int)i, 0, (ImVec2){ 0, 0 })) M.group = (int)i, M.t = 0.0f, M.mode = 0;
        }
        igTreePop();
    }
    if (ok && M.mode == 0 && M.group < (int)ng && groups[M.group].kind != ANIM_MMG_CLIP) { // (the one shown: a file of its own)
        static bool with_root = true;
        igCheckbox(PL(TR("With Root Motion")), &with_root);
        igSetItemTooltip("%s", TR("Its path on the root bone, as a clip of yours would carry it (Motion Matching takes it out again and moves the actor)."));
        if (igButton(TR("Save as Animation"), (ImVec2){ -1, 0 })) {
            char name[ANIM_NAME];
            if (anim_mm_export(g, m, s, M.group, with_root, name, sizeof name)) snprintf(M.saved, sizeof M.saved, "%s", name);
            else snprintf(M.saved, sizeof M.saved, "%s", TR("(could not save)"));
        }
        igSetItemTooltip("%s", TR("Saves the made animation shown as an Animation Sequence next to the first clip: yours to edit, or to add to the database as your own."));
        if (M.saved[0]) igTextDisabled(TR("Saved: %s"), M.saved);
    }
    igSeparatorText(TR("Made Animations' Settings"));
    gen_ch = mm_gen_settings(edit, node, &info, ok);
    igSeparatorText(TR("How it searches"));
    hint(TR("30 frames a second per group. Each moving clip also plays at 0.8x-1.25x (other speeds in its own style) and mirrored (the other foot first), when the skeleton is symmetric."));
    hint(TR("Every 0.1 s (at once when the query jumps): the frame nearest to the velocity now and in 0.3 s, and to the pose playing (feet places and velocities). A frame 15% better starts there, blended in over 0.2 s. A start or a stop plays as fast as the body's real speed goes."));
    igEndChild();
    igSameLine(0, 6);
    // the middle: the viewport and, for a group, its timeline
    igBeginChild_Str("mm_view", (ImVec2){ mid, av.y }, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    static AnimXf pose[ANIM_MAX_BONES];
    float feat[ANIM_MM_FEAT], walk[2] = { 0, 0 }, gspeed = 0.0f;
    bool have_feat = false;
    const char* playing = "-";
    if (ok && M.mode == 0) {
        if (M.group >= (int)ng) M.group = 0;
        const AnimMmGroup* gr = &groups[M.group];
        M.t += dt * gr->rate;
        if (gr->loop && gr->length > 1e-3f) M.t = fmodf(M.t, gr->length);
        else if (M.t > gr->length + 0.6f) M.t = 0.0f; // (a one-shot: again, after a moment held at its end)
        anim_mm_pose(g, m, s, M.group, fminf(M.t, gr->length), pose);
        have_feat = anim_mm_features(g, m, s, M.group, fminf(M.t, gr->length), feat);
        anim_mm_travel(g, m, s, M.group, fminf(M.t, gr->length), walk, &gspeed);
        playing = gr->kind == ANIM_MMG_CLIP ? gr->clip : gr->label;
    } else if (ok) {
        mm_simulate(g, m, s, dt);
        if (anim_mm_sim_pose(g, m, s, &M.play, pose)) {
            have_feat = anim_mm_features(g, m, s, M.play.cur, M.play.t, feat);
            playing = groups[M.play.cur].kind == ANIM_MMG_CLIP ? groups[M.play.cur].clip : groups[M.play.cur].label;
            walk[0] = M.walk[0], walk[1] = M.walk[1];
        } else if (m)
            bind_pose(m, pose);
    } else if (m)
        bind_pose(m, pose);
    if (m && M.travel) { // (over the ground: from 1.2 m behind the middle to 1.2 m ahead, then back: in view)
        float l = hypotf(walk[0], walk[1]), k = l > 1e-3f ? (fmodf(l, 240.0f) - 120.0f) / l : 0.0f;
        pose[0].t[0] += walk[0] * k, pose[0].t[1] += walk[1] * k;
    }
    ImVec2 vav = igGetContentRegionAvail();
    float tl = M.mode == 0 && ok ? igGetFontSize() * 2.2f : 0.0f;
    if (m) preview_cam(&M.vp, igGetCursorScreenPos(), (ImVec2){ vav.x, vav.y - tl }, m, pose, -1, -1, BP_PV_KEY_MOTION_MATCH, 0, 0, NULL, NULL);
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 o = igGetWindowPos();
    char line[260];
    if (M.mode == 0 && ok) {
        const AnimMmGroup* gr = &groups[M.group];
        snprintf(line, sizeof line, "%s  x%.2f%s  %.2f / %.2f s  %.0f cm/s%s", playing, gr->rate, gr->mirror ? TR("  mirrored") : "", fminf(M.t, gr->length),
                 gr->length, gr->loop ? gr->speed : gspeed, gr->loop ? "" : (gr->next >= 0 ? TR("  (then on into its loop)") : ""));
    } else if (ok && M.play.cur >= 0)
        snprintf(line, sizeof line, "%s  %.2f s  %s %.2fx  %.0f cm/s%s", playing, M.play.t, TR("play rate"), M.play.rate, M.q_speed,
                 M.play.prev >= 0 ? TR("  (blending in)") : "");
    else
        snprintf(line, sizeof line, "%s", "-");
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 10, o.y + 8 }, 0xFFE8E8E8u, line, NULL);
    if (M.mode == 0 && ok) {
        igSetNextItemWidth(-1);
        float len = groups[M.group].length;
        if (igSliderFloat("##t", &M.t, 0.0f, len > 1e-3f ? len : 1.0f, "%.2f s", 0)) M.playing = false;
    }
    igEndChild();
    igSameLine(0, 6);
    // the right: the query (the Query Debugger's), the trajectory, the speeds, the costs
    igBeginChild_Str("mm_query", (ImVec2){ right, av.y }, ImGuiChildFlags_Borders, 0);
    igPushItemWidth(-igGetFontSize() * 6.0f);
    igSeparatorText(TR("Query"));
    if (M.mode == 0) hint(TR("Switch to the Query Debugger to run the game's search with these values."));
    igCheckbox(PL(TR("Move Like the Character")), &M.move);
    igSetItemTooltip("%s", TR("The speed goes to the target at the body's limits (the made starts' and stops'), as the Character's movement does; the acceleration is its change."));
    if (M.move) {
        igSliderFloat(PL(TR("Target Speed")), &M.q_target, 0.0f, 800.0f, "%.0f cm/s", 0);
        if (ok && info.nlim) {
            if (igButton(TR("Stop"), (ImVec2){ 0, 0 })) M.q_target = 0.0f;
            for (uint32_t i = 0; i < info.nlim && i < 4; i++) {
                igSameLine(0, 4);
                char b[32];
                snprintf(b, sizeof b, "%.0f##sp%u", info.lim_speed[i], i);
                if (igButton(b, (ImVec2){ 0, 0 })) M.q_target = info.lim_speed[i];
            }
        }
        igText(TR("Speed %.0f cm/s, acceleration %.0f cm/s2"), M.q_speed, M.q_accel);
    } else {
        igSliderFloat(PL(TR("Speed")), &M.q_speed, 0.0f, 800.0f, "%.0f cm/s", 0);
        igSliderFloat(PL(TR("Acceleration")), &M.q_accel, -1500.0f, 1500.0f, "%.0f cm/s2", 0);
    }
    igSliderFloat(PL(TR("Direction")), &M.q_dir, -180.0f, 180.0f, "%.0f deg", 0);
    igSliderFloat(PL(TR("Turn")), &M.q_turn, -180.0f, 180.0f, "%.0f deg", 0);
    igSetItemTooltip("%s", TR("Where it is asked to face (degrees from where it faces): a turn in place or a pivot, if the database has one."));
    igPopItemWidth();
    mm_trajectory(igGetCursorScreenPos(), fminf(igGetContentRegionAvail().x, igGetFontSize() * 15.0f), feat, have_feat);
    igSeparatorText(TR("Database Speeds"));
    mm_speeds(groups, ng, M.mode == 0 ? M.group : M.play.cur, M.q_speed);
    if (M.mode == 1) {
        igSeparatorText(TR("Search"));
        igText(TR("Searches %u, switches %u"), M.play.searches, M.play.switches);
        if (M.play.cur >= 0) igText(TR("Best cost %.3f, playing %.3f"), M.last.cost, M.last.cur_cost < 1e29f ? M.last.cur_cost : 0.0f);
        igPlotLines_FloatPtr("##cost", M.cost_hist, 120, M.hist_at % 120, TR("best cost (lower: closer)"), 0.0f, FLT_MAX,
                             (ImVec2){ -1, igGetFontSize() * 4.0f }, sizeof(float));
        if (igButton(TR("Restart"), (ImVec2){ -1, 0 })) mm_restart();
    }
    igEndChild();
    igEnd();
    if (!open) M.open = false;
    if (ch || gen_ch) {
        g->rev++;
        M.a->dirty = true;
        M.group = 0;
        mm_restart();
    }
    if (ch) M.gen_live = false;
}
