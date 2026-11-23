// The Material Editor's own test (REALENGINE_MATED_TEST=1, test mated_ui; included by mat_editor.c): the mouse and the
// keys injected into ImGui as a user would, a frame at a time (2026-11-23):
//   1. a node dragged by its title over 6 frames: drawn where the mouse is in the same frame (no frame behind);
//   2. a link pulled from the Principled BSDF's output and dropped on the empty canvas: the add menu opens with what
//      takes a shader; "mix shader" + Enter: a Mix Shader, the BSDF in its first shader input;
//   3. a link pulled backwards from the free Base Color and dropped on nothing: "image texture" + Enter: an Image
//      Texture whose Color feeds the Base Color;
//   4. (2026-11-23) the BSDF dragged to the canvas's right edge and held there: the view scrolls (Blender's edge pan)
//      and the node stays under the mouse.
// It prints "mat-editor-test: all passed" (or FAIL lines) when it ends.
typedef struct MtEv { uint64_t frame; int kind; float x, y; int key; bool down; const char* text; } MtEv; // 0 pos, 1 button, 2 key, 3 text
static struct {
    bool on, setup, done, failed;
    int step;
    uint64_t frame, step_at;
    MtEv ev[64];
    int nev;
    // the drag's check
    ImVec2 press, press_p0;
    int synced, drag_frames;
    float start_x, start_y;
    int bsdf, base_color;
    float view0;   // (the edge pan's) the view's pan before
    ImVec2 edge;   // where the mouse held the node
} MT;

static void mt_fail(const char* what) {
    printf("[RealEngine] FAIL: mat-editor-test: %s\n", what);
    MT.failed = true;
}
static void mt_ev(int kind, float x, float y, int key, bool down, const char* text, int after) {
    if (MT.nev < 64) MT.ev[MT.nev++] = (MtEv){ MT.frame + (uint64_t)after, kind, x, y, key, down, text };
}
static void mt_type(const char* text, int after) {
    mt_ev(3, 0, 0, 0, false, text, after);
    mt_ev(2, 0, 0, ImGuiKey_Enter, true, NULL, after + 3);
    mt_ev(2, 0, 0, ImGuiKey_Enter, false, NULL, after + 4);
}
// a point of the canvas with no node under it (60 px clear), searched from a corner of the visible canvas
static bool mt_empty(const MatGraph* g, const Geo* geo, ImVec2 origin, ImVec2 size, bool left, ImVec2* out) {
    for (int ty = 0; ty < 8; ty++)
        for (int tx = 0; tx < 6; tx++) {
            float fx = left ? 0.06f + 0.07f * (float)tx : 0.94f - 0.07f * (float)tx, fy = 0.9f - 0.1f * (float)ty;
            ImVec2 p = { origin.x + size.x * fx, origin.y + size.y * fy };
            bool clear = true;
            for (int i = 0; i < g->node_count && clear; i++)
                clear = !(p.x > geo[i].p0.x - 60 && p.x < geo[i].p1.x + 60 && p.y > geo[i].p0.y - 60 && p.y < geo[i].p1.y + 60);
            if (clear) return *out = p, true;
        }
    return false;
}
static int mt_count(const MatGraph* g, int type, int* last) {
    int n = 0;
    for (int i = 0; i < g->node_count; i++)
        if (g->nodes[i].type == type) n++, *last = i;
    return n;
}

// Before the editor draws: the material made and opened, the window placed in front
static void mated_test_setup(void) {
    if (!MT.on) MT.on = getenv("REALENGINE_MATED_TEST") != NULL;
    if (!MT.on || MT.setup) return;
    MT.setup = true;
    char dir[700], path[800];
    snprintf(dir, sizeof dir, "%s\\MatEdTest", content_root());
    re_make_dirs(dir);
    snprintf(path, sizeof path, "%s\\M_MatEdTest.rmat", dir);
    Material* m = mat_load(path);
    if (!m) m = mat_create(path, false, NULL);
    if (!m || !m->graph) {
        mt_fail("the material could not be made");
        MT.done = true;
        return;
    }
    mat_graph_default(m->graph); // (the Principled BSDF into the Material Output, as new)
    m->graph->view[0] = m->graph->view[1] = 0.0f, m->graph->view[2] = 1.0f;
    E.m = m, E.open = true, E.sel = -1, E.unsaved = false, E.status[0] = 0;
    MT.bsdf = MT.base_color = -1;
}
static void mated_test_window(void) {
    if (!MT.on || MT.done) return;
    const ImGuiViewport* vp = igGetMainViewport();
    igSetNextWindowPos((ImVec2){ vp->WorkPos.x + 30.0f, vp->WorkPos.y + 30.0f }, ImGuiCond_Always, (ImVec2){ 0, 0 });
    igSetNextWindowSize((ImVec2){ fminf(vp->WorkSize.x - 60.0f, 1500.0f), fminf(vp->WorkSize.y - 60.0f, 950.0f) }, ImGuiCond_Always);
    if (MT.frame < 2) igSetNextWindowFocus(); // (once: focused every frame it closed the menu the test opens)
}

// Each frame of the canvas (its nodes' boxes and pins of this frame)
static void mated_test_frame(MatGraph* g, const Geo* geo, ImVec2 origin, ImVec2 size) {
    if (!MT.on || MT.done) return;
    ImGuiIO* io = igGetIO_Nil();
    MT.frame++;
    for (int i = 0; i < MT.nev; i++) { // (ImGui takes them at the next frame)
        MtEv* e = &MT.ev[i];
        if (e->frame != MT.frame) continue;
        if (e->kind == 0) ImGuiIO_AddMousePosEvent(io, e->x, e->y);
        else if (e->kind == 1) ImGuiIO_AddMouseButtonEvent(io, 0, e->down);
        else if (e->kind == 2) ImGuiIO_AddKeyEvent(io, (ImGuiKey)e->key, e->down);
        else ImGuiIO_AddInputCharactersUTF8(io, e->text);
    }
    // the drag in sync: while the node is held, its box where the mouse took it, this same frame
    if (MT.step == 2 && E.drag_node == MT.bsdf && MT.bsdf >= 0) {
        ImVec2 m = io->MousePos;
        if (m.x != MT.press.x || m.y != MT.press.y) {
            float ex = MT.press_p0.x + (m.x - MT.press.x), ey = MT.press_p0.y + (m.y - MT.press.y);
            MT.drag_frames++;
            if (fabsf(geo[MT.bsdf].p0.x - ex) < 0.51f && fabsf(geo[MT.bsdf].p0.y - ey) < 0.51f) MT.synced++;
        }
    }
    if (MT.frame < 40 || MT.frame - MT.step_at < 30) return; // (the window up first; each step's events, then its check)
    MT.step_at = MT.frame, MT.nev = 0;
    int last = -1;
    switch (MT.step++) {
    case 0: // the nodes: the BSDF and its Base Color
        mt_count(g, NODE_BSDF_PRINCIPLED, &MT.bsdf);
        if (MT.bsdf >= 0) {
            const MatNodeDef* d = mat_node_def(g->nodes[MT.bsdf].type);
            for (int k = 0; d && k < d->nin && MT.base_color < 0; k++)
                if (!strcmp(d->in[k].name, "Base Color")) MT.base_color = k;
        }
        if (MT.bsdf < 0 || MT.base_color < 0) mt_fail("no Principled BSDF with a Base Color"), MT.done = true;
        break;
    case 1: { // 1. the drag by the title: down, 6 moves of (20, 10), up
        ImVec2 p = { geo[MT.bsdf].p0.x + 40.0f, geo[MT.bsdf].p0.y + 9.0f };
        MT.press = p, MT.press_p0 = geo[MT.bsdf].p0, MT.start_x = g->nodes[MT.bsdf].x, MT.start_y = g->nodes[MT.bsdf].y;
        mt_ev(0, p.x, p.y, 0, false, NULL, 1);
        mt_ev(1, 0, 0, 0, true, NULL, 3);
        for (int k = 1; k <= 6; k++) mt_ev(0, p.x + 20.0f * (float)k, p.y + 10.0f * (float)k, 0, false, NULL, 4 + k);
        mt_ev(1, 0, 0, 0, false, NULL, 13);
        break;
    }
    case 2: { // its check; 2. a link from the BSDF's output to nothing
        float z = g->view[2] > 0.1f ? g->view[2] : 1.0f;
        float mx = g->nodes[MT.bsdf].x - MT.start_x, my = g->nodes[MT.bsdf].y - MT.start_y;
        printf("[RealEngine] mat-editor-test: the node dragged %.1f, %.1f (want %.1f, %.1f), drawn with the mouse in %d of %d frames\n", mx, my,
               120.0f / z, 60.0f / z, MT.synced, MT.drag_frames);
        if (fabsf(mx - 120.0f / z) > 0.6f || fabsf(my - 60.0f / z) > 0.6f) mt_fail("the node didn't follow the mouse");
        if (MT.drag_frames < 5 || MT.synced != MT.drag_frames) mt_fail("the dragged node was drawn behind the mouse");
        ImVec2 a = geo[MT.bsdf].out[0], t;
        if (!mt_empty(g, geo, origin, size, false, &t)) {
            mt_fail("no empty place on the canvas");
            MT.done = true;
            break;
        }
        mt_ev(0, a.x, a.y, 0, false, NULL, 1);
        mt_ev(1, 0, 0, 0, true, NULL, 3);
        mt_ev(0, (a.x + t.x) * 0.5f, (a.y + t.y) * 0.5f, 0, false, NULL, 5);
        mt_ev(0, t.x, t.y, 0, false, NULL, 7);
        mt_ev(1, 0, 0, 0, false, NULL, 9);
        mt_type("mix shader", 16);
        break;
    }
    case 3: { // its check; 3. a link backwards from the Base Color to nothing
        int n = mt_count(g, NODE_MIX_SHADER, &last), l = n == 1 ? mat_graph_input_link(g, last, 1) : -1;
        printf("[RealEngine] mat-editor-test: dropped from the BSDF: %d Mix Shader, its shader input from %s\n", n,
               l >= 0 && g->links[l].from == MT.bsdf && g->links[l].from_sock == 0 ? "the BSDF" : "nothing");
        if (n != 1) mt_fail("the link dropped on nothing didn't make a Mix Shader from the menu");
        else if (l < 0 || g->links[l].from != MT.bsdf || g->links[l].from_sock != 0) mt_fail("the Mix Shader isn't fed by the BSDF");
        if (mat_graph_input_link(g, MT.bsdf, MT.base_color) >= 0) {
            mt_fail("the Base Color is linked already");
            MT.done = true;
            break;
        }
        ImVec2 a = geo[MT.bsdf].in[MT.base_color], t;
        if (!mt_empty(g, geo, origin, size, true, &t)) {
            mt_fail("no empty place on the canvas");
            MT.done = true;
            break;
        }
        mt_ev(0, a.x, a.y, 0, false, NULL, 1);
        mt_ev(1, 0, 0, 0, true, NULL, 3);
        mt_ev(0, (a.x + t.x) * 0.5f, (a.y + t.y) * 0.5f, 0, false, NULL, 5);
        mt_ev(0, t.x, t.y, 0, false, NULL, 7);
        mt_ev(1, 0, 0, 0, false, NULL, 9);
        mt_type("image texture", 16);
        break;
    }
    case 4: { // its check: an Image Texture's Color into the Base Color
        int n = mt_count(g, NODE_TEX_IMAGE, &last), l = mat_graph_input_link(g, MT.bsdf, MT.base_color);
        printf("[RealEngine] mat-editor-test: dropped from the Base Color: %d Image Texture, the Base Color from %s\n", n,
               l >= 0 && n == 1 && g->links[l].from == last && g->links[l].from_sock == 0 ? "its Color" : "something else");
        if (n != 1) mt_fail("the link dropped on nothing didn't make an Image Texture from the menu");
        else if (l < 0 || g->links[l].from != last || g->links[l].from_sock != 0) mt_fail("the Image Texture's Color doesn't feed the Base Color");
        // 4. the BSDF by its title to 8 px from the canvas's right edge, held 18 frames, let go
        ImVec2 p = { geo[MT.bsdf].p0.x + 40.0f, geo[MT.bsdf].p0.y + 9.0f };
        MT.view0 = g->view[0], MT.edge = (ImVec2){ origin.x + size.x - 8.0f, p.y };
        mt_ev(0, p.x, p.y, 0, false, NULL, 1);
        mt_ev(1, 0, 0, 0, true, NULL, 3);
        for (int k = 1; k <= 4; k++) mt_ev(0, p.x + (MT.edge.x - p.x) * (float)k / 4.0f, p.y, 0, false, NULL, 4 + k);
        mt_ev(1, 0, 0, 0, false, NULL, 26);
        break;
    }
    case 5: { // its check: the view scrolled on to the right, the node under the mouse where it was let go
        float scrolled = MT.view0 - g->view[0], under = geo[MT.bsdf].p0.x + 40.0f - MT.edge.x;
        printf("[RealEngine] mat-editor-test: held at the right edge: the view scrolled %.0f px, the node %.1f px off the mouse\n", scrolled, under);
        if (scrolled < 5.0f) mt_fail("the view didn't scroll with a node held at its edge"); // (by time: ~550 px/s 8 px in; 18 frames offscreen are a few hundredths)
        if (fabsf(under) > 2.0f) mt_fail("the node didn't stay under the mouse while the view scrolled");
        MT.done = true;
        E.unsaved = false;
        // the end's screenshot (--shot): the BSDF's Node tab (its Base Color from the Image Texture), or with
        // REALENGINE_MATED_END=material nothing selected (the Material tab)
        memset(E.msel, 0, sizeof E.msel);
        E.sel = getenv("REALENGINE_MATED_END") && !strcmp(getenv("REALENGINE_MATED_END"), "material") ? -1 : MT.bsdf;
        printf("[RealEngine] mat-editor-test: %s\n", MT.failed ? "FAILED" : "all passed");
        break;
    }
    }
}
