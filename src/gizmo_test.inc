// (2026-11-23) The gizmo's own test (REALENGINE_GIZMO_TEST=1, test gizmo_ui; included by editor_viewport.c): mouse and keys
// injected into ImGui a frame at a time over the viewport, as a user would, on two cubes:
//   1. the X arrow dragged: the actor moves along X alone, one undo step;  2. a click on it that never moves: no step;
//   3. dragged, then Y pressed: the constraint goes to Y (the X part dropped);  4. dragged, "250" typed: exactly 250 cm;
//   5. dragged, the right button: back where it began, no step;  6. both selected, Rotate about the Median Point, "90"
//      typed on the Z ring: both turned a quarter about their middle;  7. Scale, "2" typed on the X box: twice as wide;
//   8. a top view whose target is behind the actor: the gizmo still there (the orthographic fix).
// It prints "gizmo-test: all passed" (or FAIL lines) when it ends.
typedef struct GtEv { uint64_t frame; int kind; float x, y; int key; bool down; } GtEv; // 0 pos, 1 left, 2 right, 3 key
static struct {
    bool on, done, failed;
    int step;
    uint64_t frame, step_at;
    GtEv ev[64];
    int nev;
    Entity a, b;
    float before[3], before_scale[3];
    int undo_before;
    bool ortho_seen;
    bool saw_hover_cursor, saw_drag_cursor; // (the Tlender's cursors: over a handle, hidden and drawn while dragging)
    // the rings' direction check: the grabbed point, the drag's end and its mirror on screen, the axes before
    Entity c;
    float q[3], pivot[3], m0[3][3];
    ImVec2 target, mirror;
    const char* ring;
} GT;

static void gt_fail(const char* what) {
    printf("[RealEngine] FAIL: gizmo-test: %s\n", what);
    GT.failed = true;
}
static void gt_ev(int kind, float x, float y, int key, bool down, int after) {
    if (GT.nev < 64) GT.ev[GT.nev++] = (GtEv){ GT.frame + (uint64_t)after, kind, x, y, key, down };
}
static void gt_key(int key, int after) { gt_ev(3, 0, 0, key, true, after), gt_ev(3, 0, 0, key, false, after + 1); }
// a drag: to p, down, moved by (dx, dy) in 4 steps, keys between, up
static void gt_drag(ImVec2 p, float dx, float dy, const int* keys, int nkeys, bool cancel) {
    gt_ev(0, p.x, p.y, 0, false, 1);
    gt_ev(1, 0, 0, 0, true, 3);
    for (int k = 1; k <= 4; k++) gt_ev(0, p.x + dx * (float)k / 4.0f, p.y + dy * (float)k / 4.0f, 0, false, 3 + k);
    for (int k = 0; k < nkeys; k++) gt_key(keys[k], 9 + k * 2);
    if (cancel) gt_ev(2, 0, 0, 0, true, 10 + nkeys * 2), gt_ev(2, 0, 0, 0, false, 11 + nkeys * 2);
    if (nkeys) gt_ev(0, p.x + dx + 4.0f, p.y + dy + 2.0f, 0, false, 12 + nkeys * 2); // (one more motion: the typed value holds)
    gt_ev(1, 0, 0, 0, false, 14 + nkeys * 2);
}
static Entity gt_cube(Editor* ed, const char* name, float x, float y) {
    World* w = &ed->world;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = 50.0f, t->flags = MOBILITY_MOVABLE;
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    CStaticMesh* m = ecs_get(w, e, COMP_STATIC_MESH);
    m->mesh = SHAPE_CUBE, m->base_color[0] = m->base_color[1] = m->base_color[2] = 0.6f, m->roughness = 0.5f, m->cast_shadow = 1;
    return e;
}
static const float* gt_loc(Editor* ed, Entity e) {
    const CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    static const float zero[3] = { 0, 0, 0 };
    return t ? t->location : zero;
}
static void gt_remember(Editor* ed, Entity e) {
    const CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    if (t) memcpy(GT.before, t->location, 12), memcpy(GT.before_scale, t->scale, 12);
    GT.undo_before = ed->undo_count;
}
// A ring grabbed at angle a0 (from the axis after it toward the next: ring_angle's) and dragged along it by da: where the
// grabbed point has to end on screen (and its mirror, a turn the wrong way)
static void gt_ring_drag(const Gizmo* g, const View* v, int axis, float a0, float da) {
    const float *u = g->ax[(axis + 1) % 3], *w = g->ax[(axis + 2) % 3];
    float r = GZ_RING * g->unit, t[3], mi[3];
    for (int k = 0; k < 3; k++) {
        GT.q[k] = g->c[k] + (u[k] * cosf(a0) + w[k] * sinf(a0)) * r;
        t[k] = g->c[k] + (u[k] * cosf(a0 + da) + w[k] * sinf(a0 + da)) * r;
        mi[k] = g->c[k] + (u[k] * cosf(a0 - da) + w[k] * sinf(a0 - da)) * r;
    }
    memcpy(GT.pivot, g->c, sizeof GT.pivot);
    ImVec2 sq = g->pc;
    project_point(v, GT.q, &sq), project_point(v, t, &GT.target), project_point(v, mi, &GT.mirror);
    gt_drag(sq, GT.target.x - sq.x, GT.target.y - sq.y, NULL, 0, false);
}
// After it: the grabbed point turned as the actor turned, on screen: near the drag's end (not its mirror)
static void gt_ring_check(Editor* ed, const View* v) {
    const CTransform* t = ecs_get(&ed->world, GT.c, COMP_TRANSFORM);
    if (!t) return;
    float m1[3][3], rel[3] = { GT.q[0] - GT.pivot[0], GT.q[1] - GT.pivot[1], GT.q[2] - GT.pivot[2] }, q2[3];
    rotator_axes(t->rotation, m1);
    for (int j = 0; j < 3; j++) { // R = M1 M0^T (the rows: the actor's axes in the world)
        q2[j] = GT.pivot[j];
        for (int k = 0; k < 3; k++) q2[j] += m1[k][j] * dot3(GT.m0[k], rel);
    }
    ImVec2 s = { 0, 0 };
    project_point(v, q2, &s);
    float dt = hypotf(s.x - GT.target.x, s.y - GT.target.y), dm = hypotf(s.x - GT.mirror.x, s.y - GT.mirror.y);
    printf("[RealEngine] gizmo-test: %s dragged along: the grabbed point %.1f px from the mouse, %.1f px from the other way\n", GT.ring, dt, dm);
    if (!(dt < dm) || dt > 20.0f) {
        char why[160];
        snprintf(why, sizeof why, "the %s turned the other way (the grabbed point doesn't follow the mouse)", GT.ring);
        gt_fail(why);
    }
}
static void gt_ring_reset(Editor* ed) { // (C unturned; its axes kept for the check)
    CTransform* t = ecs_get(&ed->world, GT.c, COMP_TRANSFORM);
    if (!t) return;
    t->rotation[0] = t->rotation[1] = t->rotation[2] = 0.0f;
    ecs_touch(&ed->world, GT.c);
    rotator_axes(t->rotation, GT.m0);
}

static ImVec2 gt_axis_at(const Gizmo* g, const View* v, int axis, float t) {
    ImVec2 p = g->pc;
    gz_axis_point(g, v, axis, t, &p);
    return p;
}

// Each viewport frame after the gizmo's (g: this frame's gizmo, NULL when it isn't shown)
static void gizmo_test_frame(Editor* ed, const Gizmo* g, const View* v) {
    if (!GT.on) GT.on = getenv("REALENGINE_GIZMO_TEST") != NULL;
    if (!GT.on || GT.done) return;
    ImGuiIO* io = igGetIO_Nil();
    GT.frame++;
    for (int i = 0; i < GT.nev; i++) {
        GtEv* e = &GT.ev[i];
        if (e->frame != GT.frame) continue;
        if (e->kind == 0) ImGuiIO_AddMousePosEvent(io, e->x, e->y);
        else if (e->kind == 1) ImGuiIO_AddMouseButtonEvent(io, 0, e->down);
        else if (e->kind == 2) ImGuiIO_AddMouseButtonEvent(io, 1, e->down);
        else ImGuiIO_AddKeyEvent(io, (ImGuiKey)e->key, e->down);
    }
    if (GT.step == 16 && g) GT.ortho_seen = true; // (the top view's frames)
    if (ed->gizmo_active && igGetMouseCursor() == ImGuiMouseCursor_None) GT.saw_drag_cursor = true;
    if (!ed->gizmo_active && ed->gizmo_hot && igGetMouseCursor() != ImGuiMouseCursor_Arrow) GT.saw_hover_cursor = true;
    if (GT.frame < 60 || GT.frame - GT.step_at < 40) return;
    GT.step_at = GT.frame, GT.nev = 0;
    const float* l;
    switch (GT.step++) {
    case 0: // the scene: two cubes, the Move tool, no snapping, World, the active actor's pivot
        ecs_shutdown(&ed->world);
        ecs_init(&ed->world);
        editor_undo_clear(ed);
        GT.a = gt_cube(ed, "GizmoA", 0.0f, 0.0f), GT.b = gt_cube(ed, "GizmoB", 0.0f, 300.0f);
        editor_select_none(ed);
        editor_select(ed, GT.a, SEL_REPLACE);
        ed->tool = TOOL_MOVE, ed->snap_move = ed->snap_rotate = ed->snap_scale = false;
        ed->gizmo_orient = GIZMO_ORIENT_WORLD, ed->gizmo_pivot = GIZMO_PIVOT_ACTIVE, ed->gizmo_only_loc = false, ed->gizmo_size = 100.0f;
        ed->vp_proj = VP_PROJ_PERSPECTIVE;
        ed->cam.pos[0] = -700.0f, ed->cam.pos[1] = -500.0f, ed->cam.pos[2] = 450.0f, ed->cam.yaw = 35.0f, ed->cam.pitch = -25.0f;
        break;
    case 1: // 1. the X arrow dragged
        if (!g) { gt_fail("no gizmo on the selected actor"), GT.done = true; break; }
        gt_remember(ed, GT.a);
        {
            ImVec2 p = gt_axis_at(g, v, 0, 0.7f), q = gt_axis_at(g, v, 0, 1.0f);
            float dx = q.x - p.x, dy = q.y - p.y, len = sqrtf(dx * dx + dy * dy);
            gt_drag(p, dx / fmaxf(len, 1.0f) * 120.0f, dy / fmaxf(len, 1.0f) * 120.0f, NULL, 0, false);
        }
        break;
    case 2:
        l = gt_loc(ed, GT.a);
        printf("[RealEngine] gizmo-test: the X arrow moved it %.1f, %.2f, %.2f cm, %d undo step(s)\n", l[0] - GT.before[0], l[1] - GT.before[1],
               l[2] - GT.before[2], ed->undo_count - GT.undo_before);
        if (fabsf(l[0] - GT.before[0]) < 1.0f || fabsf(l[1] - GT.before[1]) > 0.01f || fabsf(l[2] - GT.before[2]) > 0.01f) gt_fail("the X arrow didn't move it along X alone");
        if (ed->undo_count - GT.undo_before != 1) gt_fail("the drag didn't make exactly one undo step");
        // 2. a click that never moves
        gt_remember(ed, GT.a);
        if (g) gt_drag(gt_axis_at(g, v, 0, 0.7f), 0.0f, 0.0f, NULL, 0, false);
        break;
    case 3:
        l = gt_loc(ed, GT.a);
        printf("[RealEngine] gizmo-test: a click on the arrow: %d undo step(s)\n", ed->undo_count - GT.undo_before);
        if (ed->undo_count != GT.undo_before) gt_fail("a click that never moved left an undo step");
        if (fabsf(l[0] - GT.before[0]) + fabsf(l[1] - GT.before[1]) + fabsf(l[2] - GT.before[2]) > 0.001f) gt_fail("a click moved the actor");
        // 3. dragged, then Y: the constraint on Y
        gt_remember(ed, GT.a);
        if (g) {
            static const int ky[1] = { ImGuiKey_Y };
            ImVec2 p = gt_axis_at(g, v, 0, 0.7f);
            gt_drag(p, 80.0f, 0.0f, ky, 1, false);
        }
        break;
    case 4:
        l = gt_loc(ed, GT.a);
        printf("[RealEngine] gizmo-test: after Y: moved %.2f, %.1f, %.2f cm\n", l[0] - GT.before[0], l[1] - GT.before[1], l[2] - GT.before[2]);
        if (fabsf(l[0] - GT.before[0]) > 0.01f || fabsf(l[2] - GT.before[2]) > 0.01f || fabsf(l[1] - GT.before[1]) < 0.5f)
            gt_fail("Y didn't move the constraint to the Y axis");
        // 4. dragged, "250" typed
        gt_remember(ed, GT.a);
        if (g) {
            static const int kn[3] = { ImGuiKey_2, ImGuiKey_5, ImGuiKey_0 };
            gt_drag(gt_axis_at(g, v, 0, 0.7f), 30.0f, 0.0f, kn, 3, false);
        }
        break;
    case 5:
        l = gt_loc(ed, GT.a);
        printf("[RealEngine] gizmo-test: typed 250: moved %.3f, %.3f, %.3f cm\n", l[0] - GT.before[0], l[1] - GT.before[1], l[2] - GT.before[2]);
        if (fabsf(l[0] - GT.before[0] - 250.0f) > 0.01f || fabsf(l[1] - GT.before[1]) > 0.01f) gt_fail("the typed 250 didn't move it 250 cm along X");
        // 5. dragged, the right button
        gt_remember(ed, GT.a);
        if (g) gt_drag(gt_axis_at(g, v, 0, 0.7f), 90.0f, 10.0f, NULL, 0, true);
        break;
    case 6:
        l = gt_loc(ed, GT.a);
        printf("[RealEngine] gizmo-test: cancelled: off by %.3f cm, %d undo step(s)\n",
               fabsf(l[0] - GT.before[0]) + fabsf(l[1] - GT.before[1]) + fabsf(l[2] - GT.before[2]), ed->undo_count - GT.undo_before);
        if (fabsf(l[0] - GT.before[0]) + fabsf(l[1] - GT.before[1]) + fabsf(l[2] - GT.before[2]) > 0.001f) gt_fail("the right button didn't put it back");
        if (ed->undo_count != GT.undo_before) gt_fail("a cancelled drag left an undo step");
        // 6. both, Rotate about the Median Point, 90 typed on the Z ring
        editor_select(ed, GT.b, SEL_ADD);
        ed->tool = TOOL_ROTATE, ed->gizmo_pivot = GIZMO_PIVOT_MEDIAN;
        break;
    case 7: {
        if (!g) { gt_fail("no rotate gizmo"), GT.done = true; break; }
        gt_remember(ed, GT.a);
        // the Z ring (in the XY plane): its point along the gizmo's X, the front half
        ImVec2 p = gt_axis_at(g, v, 0, GZ_RING);
        float ex[3] = { g->ax[0][0], g->ax[0][1], g->ax[0][2] };
        if (dot3(ex, g->to_eye) < 0.0f) p = gt_axis_at(g, v, 0, -GZ_RING); // (the half facing the viewer)
        static const int kn[2] = { ImGuiKey_9, ImGuiKey_0 };
        gt_drag(p, 0.0f, 25.0f, kn, 2, false);
        break;
    }
    case 8: {
        const float* la = gt_loc(ed, GT.a);
        const CTransform* ta = ecs_get(&ed->world, GT.a, COMP_TRANSFORM);
        // A (where the steps before left it) turned a quarter about Z around the middle of A and B: (-y, x) or (y, -x) of
        // its offset from the median
        const float* lb = gt_loc(ed, GT.b);
        // (two actors turned about their middle keep it)
        float mx = (la[0] + lb[0]) * 0.5f, my = (la[1] + lb[1]) * 0.5f, ox = GT.before[0] - mx, oy = GT.before[1] - my;
        bool ccw = fabsf(la[0] - (mx - oy)) < 0.5f && fabsf(la[1] - (my + ox)) < 0.5f, cw = fabsf(la[0] - (mx + oy)) < 0.5f && fabsf(la[1] - (my - ox)) < 0.5f;
        printf("[RealEngine] gizmo-test: rotated 90 about the median (%.1f, %.1f): A from %.1f, %.1f to %.1f, %.1f (yaw %.1f)\n", mx, my, GT.before[0],
               GT.before[1], la[0], la[1], ta ? ta->rotation[1] : 0.0f);
        if (!ccw && !cw) gt_fail("Rotate about the Median Point didn't turn A a quarter about the middle");
        if (!ta || fabsf(fabsf(ta->rotation[1]) - 90.0f) > 0.5f) gt_fail("A didn't turn 90 degrees");
        // 7. Scale on A alone, "2" typed on the X box (Local axes)
        editor_select(ed, GT.a, SEL_REPLACE);
        ed->tool = TOOL_SCALE, ed->gizmo_pivot = GIZMO_PIVOT_ACTIVE;
        break;
    }
    case 9: {
        if (!g) { gt_fail("no scale gizmo"), GT.done = true; break; }
        gt_remember(ed, GT.a);
        static const int k2[1] = { ImGuiKey_2 };
        gt_drag(gt_axis_at(g, v, 0, GZ_BOX_AT), 20.0f, 0.0f, k2, 1, false);
        break;
    }
    case 10: {
        const CTransform* ta = ecs_get(&ed->world, GT.a, COMP_TRANSFORM);
        printf("[RealEngine] gizmo-test: typed 2 on the X box: scale %.3f, %.3f, %.3f\n", ta ? ta->scale[0] : 0, ta ? ta->scale[1] : 0, ta ? ta->scale[2] : 0);
        if (!ta || fabsf(ta->scale[0] - GT.before_scale[0] * 2.0f) > 0.001f || fabsf(ta->scale[1] - GT.before_scale[1]) > 0.001f)
            gt_fail("the X box with 2 typed didn't double the X scale alone");
        // 8. the rings turn the way they're dragged (the grabbed point follows the mouse): a new cube C alone, Rotate, World
        GT.c = gt_cube(ed, "GizmoC", 500.0f, -200.0f);
        editor_select(ed, GT.c, SEL_REPLACE);
        ed->tool = TOOL_ROTATE, ed->gizmo_orient = GIZMO_ORIENT_WORLD;
        gt_ring_reset(ed);
        break;
    }
    case 11: // the Z ring (flat), dragged 25 degrees
        if (!g) { gt_fail("no rotate gizmo on C"), GT.done = true; break; }
        GT.ring = "Z ring";
        { // (at 45 degrees on the side facing the camera: at an axis two rings cross and the press could take the other)
            float d[3] = { g->ax[0][0] + g->ax[1][0], g->ax[0][1] + g->ax[1][1], g->ax[0][2] + g->ax[1][2] };
            gt_ring_drag(g, v, 2, dot3(d, g->to_eye) >= 0.0f ? 0.785398f : 3.926991f, 25.0f * DEG2RAD);
        }
        break;
    case 12: // its check; the X ring
        gt_ring_check(ed, v);
        gt_ring_reset(ed);
        if (!g) break;
        GT.ring = "X ring";
        {
            float d[3] = { g->ax[1][0] + g->ax[2][0], g->ax[1][1] + g->ax[2][1], g->ax[1][2] + g->ax[2][2] };
            gt_ring_drag(g, v, 0, dot3(d, g->to_eye) >= 0.0f ? 0.785398f : 3.926991f, 25.0f * DEG2RAD);
        }
        break;
    case 13: // its check; the outer ring (about the view's axis) grabbed at its right, dragged up
        gt_ring_check(ed, v);
        gt_ring_reset(ed);
        if (!g) break;
        {
            GT.ring = "outer ring";
            float r = GZ_VIEW_RING * GIZMO_PX, a0 = 0.0f, da = -0.45f; // (screen angles, y down: -0.45 is up)
            ImVec2 sq = { g->pc.x + r * cosf(a0), g->pc.y + r * sinf(a0) };
            GT.target = (ImVec2){ g->pc.x + r * cosf(a0 + da), g->pc.y + r * sinf(a0 + da) };
            GT.mirror = (ImVec2){ g->pc.x + r * cosf(a0 - da), g->pc.y + r * sinf(a0 - da) };
            float dir[3];
            mouse_ray(v, sq, dir);
            memcpy(GT.pivot, g->c, sizeof GT.pivot);
            if (!ray_plane(v, dir, g->c, v->fwd, GT.q)) memcpy(GT.q, g->c, sizeof GT.q);
            gt_drag(sq, GT.target.x - sq.x, GT.target.y - sq.y, NULL, 0, false);
        }
        break;
    case 14: // its check; the trackball: grabbed inside the rings, dragged right and down
        gt_ring_check(ed, v);
        gt_ring_reset(ed);
        if (!g) break;
        memcpy(GT.pivot, g->c, sizeof GT.pivot);
        for (int k = 0; k < 3; k++) GT.q[k] = g->c[k] + g->to_eye[k] * g->unit * 0.5f; // (the face toward the camera)
        project_point(v, GT.q, &GT.target);
        gt_drag((ImVec2){ g->pc.x + 8.0f, g->pc.y + 6.0f }, 60.0f, 40.0f, NULL, 0, false);
        break;
    case 15: { // its check: that face went right and down with the mouse; 9. a top view looking at a point behind the actor
        const CTransform* t = ecs_get(&ed->world, GT.c, COMP_TRANSFORM);
        if (t) {
            float m1[3][3], rel[3] = { GT.q[0] - GT.pivot[0], GT.q[1] - GT.pivot[1], GT.q[2] - GT.pivot[2] }, q2[3];
            rotator_axes(t->rotation, m1);
            for (int j = 0; j < 3; j++) {
                q2[j] = GT.pivot[j];
                for (int k = 0; k < 3; k++) q2[j] += m1[k][j] * dot3(GT.m0[k], rel);
            }
            ImVec2 s2 = GT.target;
            project_point(v, q2, &s2);
            printf("[RealEngine] gizmo-test: trackball dragged right and down: the front moved %.1f, %.1f px\n", s2.x - GT.target.x, s2.y - GT.target.y);
            if (!(s2.x - GT.target.x > 5.0f) || !(s2.y - GT.target.y > 5.0f)) gt_fail("the trackball turned the other way (the front doesn't follow the mouse)");
        }
        ed->tool = TOOL_MOVE;
        ed->vp_proj = VP_PROJ_TOP, ed->vp_ortho = 1500.0f;
        ed->vp_target[0] = ed->vp_target[1] = 0.0f, ed->vp_target[2] = -500.0f;
        GT.ortho_seen = false;
        break;
    }
    case 16:
        printf("[RealEngine] gizmo-test: the top view %s the gizmo\n", GT.ortho_seen ? "shows" : "hides");
        if (!GT.ortho_seen) gt_fail("the orthographic view hid the gizmo of an actor in front of its target");
        printf("[RealEngine] gizmo-test: the cursor over a handle %s, while dragging %s\n", GT.saw_hover_cursor ? "changed" : "stayed the arrow",
               GT.saw_drag_cursor ? "hidden (the transform's drawn)" : "shown");
        if (!GT.saw_hover_cursor || !GT.saw_drag_cursor) gt_fail("the cursor didn't change over the gizmo as the Tlender's");
        ed->vp_proj = VP_PROJ_PERSPECTIVE;
        GT.done = true;
        printf("[RealEngine] gizmo-test: %s\n", GT.failed ? "FAILED" : "all passed");
        break;
    }
}
