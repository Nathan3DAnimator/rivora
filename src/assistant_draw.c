// The Assistant's pictures (assistant_draw.h): what isn't code is shown, not told. Drawn by the editor itself (no image
// files: light, sharp at any UI scale, in the article's language):
//   blueprint  the node graph of a recipe ("Event Tick -> Branch ( ... ) True -> Jump", one chain a line, indented
//              lines continue from the line above, a word before "->" labels the wire)
//   steps      boxes in a row with arrows ("A | B | C")
//   keys       key caps with what they do ("W: Move | E: Rotate")
//   axes       the 3D axes (lines "X: forward", "Y: right", "Z: up")
//   layout     a map of the editor's panels (lines "Name: x y w h" in percent; a click shows the panel)
//   compare    a table (lines "a | b | c", the first the header; yes / no colored)
//   bars       a bar chart (lines "label: value"; the kind's second word the unit)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "assistant_draw.h"

#include "theme.h"
static uint32_t rgba(float r, float g, float b, float a) { return igGetColorU32_Vec4((ImVec4){ r, g, b, a }); }
static float tw(const char* s) { return igCalcTextSize(s, NULL, false, -1).x; }
static void trim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\t')) s[--n] = 0;
    size_t k = 0;
    while (s[k] == ' ' || s[k] == '\t') k++;
    if (k) memmove(s, s + k, n - k + 1);
}
// the picture's area: a child with horizontal scroll (a wide graph never cut), its content's size reserved
static ImVec2 area_begin(int id, float w, float h) {
    char cid[32];
    snprintf(cid, sizeof cid, "##diag%d", id);
    igPushStyleColor_Vec4(ImGuiCol_ChildBg, THV(TK_N3));
    // the child's padding on both sides (the picture whole, no vertical scrollbar); the horizontal one when it is wider
    ImVec2 pad = igGetStyle()->WindowPadding;
    float avail = igGetContentRegionAvail().x;
    bool wide = w + pad.x * 2.0f > avail;
    igBeginChild_Str(cid, (ImVec2){ avail, h + pad.y * 2.0f + (wide ? igGetStyle()->ScrollbarSize : 0.0f) + 2.0f }, ImGuiChildFlags_Borders,
                     ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 o = igGetCursorScreenPos();
    igDummy((ImVec2){ w, h });
    return o;
}
static void area_end(void) {
    igEndChild();
    igPopStyleColor(1);
}

// --- blueprint ---
typedef struct BNode {
    char title[64], sub[3][72];
    int nsub, kind; // 0 event, 1 flow, 2 set, 3 function, 4 function entry
    int parent, col, row;
    char label[24]; // the wire from the parent
    float w, h;
} BNode;
#define BMAX 48

static int node_kind(const char* t) {
    if (!strncmp(t, "Event", 5) || !strncmp(t, "Evento", 6) || !strncmp(t, "On ", 3)) return 0;
    if (!strncmp(t, "Função", 7) || !strncmp(t, "Function", 8)) return 4;
    static const char* const flow[] = { "Branch", "Delay", "Sequence", "Do Once", "For Loop", "For Each", "Gate", "Retriggerable",
                                        "While", "Switch", "Select" };
    for (size_t i = 0; i < sizeof flow / sizeof flow[0]; i++)
        if (!strncmp(t, flow[i], strlen(flow[i]))) return 1;
    if (!strncmp(t, "Set ", 4) || strchr(t, '=')) return 2;
    return 3;
}
// a token "Name ( args ) Label": the node's title, its args (wrapped), the label of the wire after it
static void parse_node(BNode* n, const char* tok, char* label_after, size_t lcap) {
    char t[256];
    snprintf(t, sizeof t, "%s", tok);
    trim(t);
    label_after[0] = 0;
    char* close = strrchr(t, ')');
    if (close && close[1]) { // "Branch ( x ) True": True labels the next wire
        char tail[64];
        snprintf(tail, sizeof tail, "%s", close + 1);
        trim(tail);
        if (tail[0] && strlen(tail) < lcap && !strchr(tail, ' ')) snprintf(label_after, lcap, "%s", tail), close[1] = 0;
    }
    char args[200] = "";
    char* open = strchr(t, '(');
    if (open) {
        char* end = strrchr(open, ')');
        snprintf(args, sizeof args, "%.*s", (int)(end ? end - open - 1 : (long)strlen(open + 1)), open + 1);
        *open = 0;
    } else if (strchr(t, '=') && (!strncmp(t, "Set ", 4) || t[0] != 0)) { // "Set HP = Max HP": the value under the title
        char* eq = strchr(t, '=');
        snprintf(args, sizeof args, "%s", eq);
        *eq = 0;
    }
    trim(t), trim(args);
    snprintf(n->title, sizeof n->title, "%s", t);
    n->kind = node_kind(t);
    // the args wrapped at ~34 characters, 3 lines at most
    n->nsub = 0;
    const char* a = args;
    while (*a && n->nsub < 3) {
        size_t len = strlen(a), cut = len;
        if (len > 34) {
            cut = 34;
            while (cut > 12 && a[cut] != ' ' && a[cut] != ',') cut--;
            if (cut <= 12) cut = 34;
            if (n->nsub == 2) cut = len > 34 ? 31 : len;
        }
        snprintf(n->sub[n->nsub], sizeof n->sub[0], "%.*s%s", (int)cut, a, n->nsub == 2 && cut < len ? "..." : "");
        n->nsub++;
        a += cut;
        while (*a == ' ' || *a == ',') a++;
    }
}

static void draw_blueprint(const char* body, int id) {
    BNode nodes[BMAX];
    int nn = 0, nrow = 0;
    struct { int indent, last; } lines[64];
    int nl = 0;
    char note[512] = "";
    const char* p = body;
    char line[512];
    while (*p && nn < BMAX) {
        const char* e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), p);
        p = e ? e + 1 : p + n;
        int indent = 0;
        while (line[indent] == ' ') indent++;
        char* s = line + indent;
        if (!*s) continue;
        if (*s == '(') { // a note: under the picture
            size_t k = strlen(note);
            snprintf(note + k, sizeof note - k, "%s%s", k ? "  " : "", s);
            continue;
        }
        char* arrow = strstr(s, "->");
        if (indent > 0 && !arrow && nn > 0) { // an argument line under a node: its text under the title
            BNode* b = &nodes[nn - 1];
            if (b->nsub < 3) snprintf(b->sub[b->nsub++], sizeof b->sub[0], "%.70s", s);
            continue;
        }
        // the wire's label before the first "->" ("True -> ..."), the parent: a labeled line hangs from the line above
        // it (less indented); a plain "-> X" continues the last line at its own indentation or above
        char label[24] = "";
        int parent = -1;
        if (indent > 0 && arrow) {
            char pre[64];
            snprintf(pre, sizeof pre, "%.*s", (int)(arrow - s), s);
            trim(pre);
            if (pre[0] && !strchr(pre, '(') && strlen(pre) < sizeof label) snprintf(label, sizeof label, "%s", pre);
            s = arrow + 2;
            for (int i = nl - 1; i >= 0 && parent < 0; i--)
                if (label[0] ? lines[i].indent < indent : lines[i].indent <= indent) parent = lines[i].last;
        }
        // the chain of this line: its nodes one after another ("Branch ( x ) True -> Set": True labels that wire)
        int prev = parent, row = nrow++;
        char wire[24];
        snprintf(wire, sizeof wire, "%s", label);
        char* tok = s;
        while (tok && nn < BMAX) {
            char* next = strstr(tok, "->");
            char part[256];
            snprintf(part, sizeof part, "%.*s", (int)(next ? next - tok : (long)strlen(tok)), tok);
            trim(part);
            if (part[0]) {
                BNode* b = &nodes[nn];
                memset(b, 0, sizeof *b);
                char after[24];
                parse_node(b, part, after, sizeof after);
                b->parent = prev;
                snprintf(b->label, sizeof b->label, "%s", wire);
                b->col = prev >= 0 ? nodes[prev].col + 1 : 0;
                b->row = row;
                prev = nn++;
                snprintf(wire, sizeof wire, "%s", after);
            }
            tok = next ? next + 2 : NULL;
        }
        if (nl < 64) lines[nl].indent = indent, lines[nl].last = prev, nl++;
    }
    if (!nn) return;
    // sizes: a column as wide as its widest node; rows as tall as the tallest
    float fs = igGetFontSize(), lh = igGetTextLineHeight();
    float colw[BMAX] = { 0 };
    int ncol = 0;
    float rowh = 0.0f;
    for (int i = 0; i < nn; i++) {
        BNode* b = &nodes[i];
        float w = tw(b->title) + fs * 2.2f;
        for (int k = 0; k < b->nsub; k++) w = fmaxf(w, tw(b->sub[k]) + fs * 1.4f);
        b->w = fminf(fmaxf(w, fs * 6.5f), fs * 22.0f);
        b->h = lh * 1.5f + (b->nsub ? lh * (float)b->nsub + fs * 0.6f : fs * 0.5f);
        if (b->col < BMAX && b->w > colw[b->col]) colw[b->col] = b->w;
        if (b->col + 1 > ncol) ncol = b->col + 1;
        if (b->h > rowh) rowh = b->h;
    }
    float gapx = fs * 3.0f, gapy = fs * 1.4f, pad = fs * 0.8f;
    float colx[BMAX];
    float x = pad;
    for (int c = 0; c < ncol && c < BMAX; c++) colx[c] = x, x += colw[c] + gapx;
    float W = x - gapx + pad, H = pad * 2.0f + (float)nrow * rowh + (float)(nrow - 1) * gapy;
    float noteh = note[0] ? lh * 1.6f : 0.0f;
    ImVec2 o = area_begin(id, W, H + noteh);
    ImDrawList* dl = igGetWindowDrawList();
    static const float hc[5][3] = { { 0.62f, 0.13f, 0.13f }, { 0.36f, 0.37f, 0.40f }, { 0.12f, 0.45f, 0.42f }, { 0.14f, 0.34f, 0.62f }, { 0.45f, 0.22f, 0.58f } };
    // the wires first (under the nodes): from the parent's exec out to the node's exec in
    for (int i = 0; i < nn; i++) {
        BNode* b = &nodes[i];
        if (b->parent < 0) continue;
        BNode* a = &nodes[b->parent];
        float ax = o.x + colx[a->col] + a->w, ay = o.y + pad + (float)a->row * (rowh + gapy) + lh * 0.75f;
        float bx = o.x + colx[b->col], by = o.y + pad + (float)b->row * (rowh + gapy) + lh * 0.75f;
        float d = fmaxf((bx - ax) * 0.5f, fs * 1.5f);
        ImDrawList_AddBezierCubic(dl, (ImVec2){ ax, ay }, (ImVec2){ ax + d, ay }, (ImVec2){ bx - d, by }, (ImVec2){ bx, by }, rgba(0.92f, 0.92f, 0.92f, 0.9f), 2.2f, 0);
        if (b->label[0]) { // at the wire's end (a Branch's True and False leave the same pin: apart at their nodes)
            float tw = igCalcTextSize(b->label, NULL, false, -1).x;
            ImDrawList_AddText_Vec2(dl, (ImVec2){ bx - tw - fs * 0.35f, by - lh - 1.0f }, rgba(0.95f, 0.85f, 0.5f, 1.0f), b->label, NULL);
        }
    }
    for (int i = 0; i < nn; i++) {
        BNode* b = &nodes[i];
        float x0 = o.x + colx[b->col], y0 = o.y + pad + (float)b->row * (rowh + gapy);
        const float* c = hc[b->kind];
        ImDrawList_AddRectFilled(dl, (ImVec2){ x0 + 3, y0 + 4 }, (ImVec2){ x0 + b->w + 3, y0 + b->h + 4 }, rgba(0, 0, 0, 0.35f), 6.0f, 0);
        ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x0 + b->w, y0 + b->h }, rgba(0.13f, 0.135f, 0.15f, 0.97f), 6.0f, 0);
        ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x0 + b->w, y0 + lh * 1.5f }, rgba(c[0], c[1], c[2], 1.0f), 6.0f,
                                 ImDrawFlags_RoundCornersTop);
        ImDrawList_AddRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x0 + b->w, y0 + b->h }, rgba(c[0] * 1.4f, c[1] * 1.4f, c[2] * 1.4f, 0.8f), 6.0f, 1.0f, 0);
        // the exec pins: a triangle out (and in, but on events)
        float py = y0 + lh * 0.75f;
        uint32_t white = rgba(1, 1, 1, 0.95f);
        if (b->kind != 0 && b->kind != 4)
            ImDrawList_AddTriangleFilled(dl, (ImVec2){ x0 + 4, py - 4 }, (ImVec2){ x0 + 4, py + 4 }, (ImVec2){ x0 + 10, py }, white);
        ImDrawList_AddTriangleFilled(dl, (ImVec2){ x0 + b->w - 10, py - 4 }, (ImVec2){ x0 + b->w - 10, py + 4 }, (ImVec2){ x0 + b->w - 4, py }, white);
        float tx = x0 + (b->kind != 0 && b->kind != 4 ? 14.0f : 8.0f);
        ImDrawList_PushClipRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x0 + b->w - 12, y0 + b->h }, true);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ tx, y0 + lh * 0.25f }, white, b->title, NULL);
        for (int k = 0; k < b->nsub; k++)
            ImDrawList_AddText_Vec2(dl, (ImVec2){ x0 + fs * 0.7f, y0 + lh * 1.5f + fs * 0.3f + (float)k * lh }, rgba(0.78f, 0.80f, 0.84f, 1.0f), b->sub[k], NULL);
        ImDrawList_PopClipRect(dl);
    }
    if (note[0]) ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + pad, o.y + H }, rgba(0.6f, 0.62f, 0.66f, 1.0f), note, NULL);
    area_end();
}

// --- steps / keys (rows of boxes, wrapped to the width) ---
static int split_bar(const char* body, char out[][160], int cap) {
    int n = 0;
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", body);
    for (char* c = buf; *c; c++)
        if (*c == '\n') *c = '|';
    for (char* t = strtok(buf, "|"); t && n < cap; t = strtok(NULL, "|")) {
        snprintf(out[n], 160, "%s", t);
        trim(out[n]);
        if (out[n][0]) n++;
    }
    return n;
}

static void draw_steps(const char* body, int id) {
    char it[16][160];
    int n = split_bar(body, it, 16);
    if (!n) return;
    float fs = igGetFontSize(), lh = igGetTextLineHeight(), avail = igGetContentRegionAvail().x - fs;
    float bw = fs * 9.5f, bh = lh * 3.4f, gap = fs * 2.2f;
    int per = (int)fmaxf(1.0f, floorf((avail + gap) / (bw + gap)));
    int rows = (n + per - 1) / per;
    float W = (float)(n < per ? n : per) * (bw + gap) - gap + fs, H = (float)rows * (bh + fs * 1.2f) + fs * 0.4f;
    ImVec2 o = area_begin(id, W, H);
    ImDrawList* dl = igGetWindowDrawList();
    for (int i = 0; i < n; i++) {
        int r = i / per, c = i % per;
        float x0 = o.x + fs * 0.5f + (float)c * (bw + gap), y0 = o.y + fs * 0.6f + (float)r * (bh + fs * 1.2f);
        ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x0 + bw, y0 + bh }, rgba(0.14f, 0.17f, 0.22f, 1.0f), 8.0f, 0);
        ImDrawList_AddRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x0 + bw, y0 + bh }, rgba(0.30f, 0.52f, 0.85f, 0.9f), 8.0f, 1.2f, 0);
        ImDrawList_AddCircleFilled(dl, (ImVec2){ x0 + fs * 0.9f, y0 + fs * 0.9f }, fs * 0.62f, rgba(0.30f, 0.52f, 0.85f, 1.0f), 16);
        char num[8];
        snprintf(num, sizeof num, "%d", i + 1);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ x0 + fs * 0.9f - tw(num) * 0.5f, y0 + fs * 0.9f - lh * 0.5f }, rgba(1, 1, 1, 1), num, NULL);
        // the text wrapped in the box
        ImDrawList_AddText_FontPtr(dl, igGetFont(), fs, (ImVec2){ x0 + fs * 0.6f, y0 + fs * 1.7f }, rgba(0.92f, 0.93f, 0.95f, 1.0f), it[i], NULL,
                                   bw - fs * 1.1f, NULL);
        if (c < per - 1 && i < n - 1) { // the arrow to the next
            float ax = x0 + bw + fs * 0.35f, ay = y0 + bh * 0.5f;
            ImDrawList_AddLine(dl, (ImVec2){ ax, ay }, (ImVec2){ ax + gap - fs * 0.7f, ay }, rgba(0.6f, 0.7f, 0.9f, 1.0f), 2.0f);
            ImDrawList_AddTriangleFilled(dl, (ImVec2){ ax + gap - fs * 0.7f, ay - 5 }, (ImVec2){ ax + gap - fs * 0.7f, ay + 5 },
                                         (ImVec2){ ax + gap - fs * 0.25f, ay }, rgba(0.6f, 0.7f, 0.9f, 1.0f));
        }
    }
    area_end();
}

static void draw_keys(const char* body, int id) {
    char it[24][160];
    int n = split_bar(body, it, 24);
    if (!n) return;
    float fs = igGetFontSize(), lh = igGetTextLineHeight(), avail = igGetContentRegionAvail().x - fs;
    float cw = fs * 8.0f, ch = lh * 3.6f;
    int per = (int)fmaxf(1.0f, floorf(avail / cw));
    int rows = (n + per - 1) / per;
    ImVec2 o = area_begin(id, (float)(n < per ? n : per) * cw + fs * 0.5f, (float)rows * ch + fs * 0.4f);
    ImDrawList* dl = igGetWindowDrawList();
    for (int i = 0; i < n; i++) {
        char key[48] = "", what[120] = "";
        char* colon = strchr(it[i], ':');
        if (colon) snprintf(key, sizeof key, "%.*s", (int)(colon - it[i]), it[i]), snprintf(what, sizeof what, "%s", colon + 1);
        else snprintf(key, sizeof key, "%s", it[i]);
        trim(key), trim(what);
        int r = i / per, c = i % per;
        float x0 = o.x + fs * 0.5f + (float)c * cw, y0 = o.y + fs * 0.5f + (float)r * ch;
        float kw = fmaxf(tw(key) + fs * 1.2f, fs * 2.4f), kh = lh * 1.6f;
        ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 + 3 }, (ImVec2){ x0 + kw, y0 + kh + 3 }, rgba(0.05f, 0.05f, 0.06f, 1.0f), 5.0f, 0);
        ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x0 + kw, y0 + kh }, rgba(0.88f, 0.89f, 0.92f, 1.0f), 5.0f, 0);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ x0 + (kw - tw(key)) * 0.5f, y0 + (kh - lh) * 0.5f }, rgba(0.08f, 0.08f, 0.1f, 1.0f), key, NULL);
        ImDrawList_AddText_FontPtr(dl, igGetFont(), fs, (ImVec2){ x0, y0 + kh + fs * 0.3f }, rgba(0.85f, 0.86f, 0.9f, 1.0f), what, NULL, cw - fs * 0.6f, NULL);
    }
    area_end();
}

// --- axes ---
static void draw_axes(const char* body, int id) {
    char lab[3][120] = { "X", "Y", "Z" };
    char it[8][160];
    int n = split_bar(body, it, 8);
    for (int i = 0; i < n; i++)
        for (int a = 0; a < 3; a++)
            if (it[i][0] == "XYZ"[a] && it[i][1] == ':') snprintf(lab[a], sizeof lab[a], "%s", it[i]);
    float fs = igGetFontSize();
    float W = fs * 30.0f, H = fs * 12.0f;
    ImVec2 o = area_begin(id, W, H);
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 c = { o.x + fs * 11.0f, o.y + H * 0.62f }; // (room on the left for the X label, it points there)
    // a grid on the ground (perspective-ish)
    for (int k = -3; k <= 3; k++) {
        float t = (float)k * fs * 0.9f;
        ImDrawList_AddLine(dl, (ImVec2){ c.x - fs * 5 + t * 0.6f, c.y + fs * 1.6f + t * 0.3f }, (ImVec2){ c.x + fs * 5 + t * 0.6f, c.y - fs * 0.6f + t * 0.3f }, rgba(1, 1, 1, 0.07f), 1.0f);
    }
    struct { ImVec2 d; float r, g, b; } ax[3] = { { { -0.75f, 0.45f }, 0.95f, 0.30f, 0.30f }, { { 1.0f, -0.18f }, 0.40f, 0.85f, 0.35f }, { { 0.0f, -1.0f }, 0.35f, 0.55f, 1.0f } };
    float L = fs * 5.0f;
    for (int a = 0; a < 3; a++) {
        ImVec2 e = { c.x + ax[a].d.x * L, c.y + ax[a].d.y * L };
        uint32_t col = rgba(ax[a].r, ax[a].g, ax[a].b, 1.0f);
        ImDrawList_AddLine(dl, c, e, col, 3.0f);
        float nx = ax[a].d.x, ny = ax[a].d.y, len = sqrtf(nx * nx + ny * ny);
        nx /= len, ny /= len;
        ImDrawList_AddTriangleFilled(dl, (ImVec2){ e.x + nx * 10, e.y + ny * 10 }, (ImVec2){ e.x - ny * 6, e.y + nx * 6 }, (ImVec2){ e.x + ny * 6, e.y - nx * 6 }, col);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ e.x + nx * 16 + (nx < 0 ? -tw(lab[a]) : 0), e.y + ny * 16 - fs * 0.5f }, col, lab[a], NULL);
    }
    ImDrawList_AddCircleFilled(dl, c, 4.0f, rgba(1, 1, 1, 0.9f), 12);
    area_end();
}

// --- layout: the editor's panels (a click: show_where) ---
static void draw_layout(const char* body, int id, AsShowFn show) {
    float fs = igGetFontSize();
    float W = fminf(igGetContentRegionAvail().x - fs, fs * 34.0f), H = W * 0.56f;
    ImVec2 o = area_begin(id, W, H);
    ImDrawList* dl = igGetWindowDrawList();
    const char* p = body;
    char line[200];
    int k = 0;
    while (*p) {
        const char* e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), p);
        p = e ? e + 1 : p + n;
        char* colon = strrchr(line, ':');
        float x, y, w, h;
        if (!colon || sscanf(colon + 1, "%f %f %f %f", &x, &y, &w, &h) != 4) continue;
        *colon = 0;
        trim(line);
        float x0 = o.x + W * x / 100.0f + 2, y0 = o.y + H * y / 100.0f + 2, x1 = x0 + W * w / 100.0f - 4, y1 = y0 + H * h / 100.0f - 4;
        bool hov = igIsMouseHoveringRect((ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, true) && igIsWindowHovered(0);
        static const float pal[6][3] = { { 0.20f, 0.32f, 0.50f }, { 0.22f, 0.40f, 0.30f }, { 0.42f, 0.30f, 0.18f }, { 0.36f, 0.22f, 0.42f }, { 0.22f, 0.36f, 0.40f }, { 0.30f, 0.30f, 0.32f } };
        const float* c = pal[k++ % 6];
        float boost = hov ? 1.35f : 1.0f;
        ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, rgba(c[0] * boost, c[1] * boost, c[2] * boost, 1.0f), 4.0f, 0);
        ImDrawList_AddRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, rgba(1, 1, 1, hov ? 0.8f : 0.25f), 4.0f, hov ? 2.0f : 1.0f, 0);
        ImVec2 ts = igCalcTextSize(line, NULL, false, -1);
        ImDrawList_PushClipRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, true);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ (x0 + x1 - ts.x) * 0.5f, (y0 + y1 - ts.y) * 0.5f }, rgba(1, 1, 1, 0.95f), line, NULL);
        ImDrawList_PopClipRect(dl);
        if (hov && show) {
            igSetMouseCursor(ImGuiMouseCursor_Hand);
            if (igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) show(line);
        }
    }
    area_end();
}

// --- compare: a table, yes / no colored ---
static bool is_yes(const char* s) { return !strcmp(s, "sim") || !strcmp(s, "yes") || !strcmp(s, "Sim") || !strcmp(s, "Yes"); }
static bool is_no(const char* s) { return !strcmp(s, "não") || !strcmp(s, "no") || !strcmp(s, "Não") || !strcmp(s, "No") || !strcmp(s, "nao"); }
static void draw_compare(const char* body, int id) {
    char cells[12][6][96];
    int nr = 0, nc = 0;
    const char* p = body;
    char line[600];
    while (*p && nr < 12) {
        const char* e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), p);
        p = e ? e + 1 : p + n;
        if (!strchr(line, '|')) continue;
        int c = 0;
        char* s = line;
        for (;;) {
            char* bar = strchr(s, '|');
            if (bar) *bar = 0;
            if (c < 6) snprintf(cells[nr][c], sizeof cells[0][0], "%s", s), trim(cells[nr][c]), c++;
            if (!bar) break;
            s = bar + 1;
        }
        if (c > nc) nc = c;
        nr++;
    }
    if (!nr || !nc) return;
    float fs = igGetFontSize(), lh = igGetTextLineHeight();
    float cw[6] = { 0 };
    for (int r = 0; r < nr; r++)
        for (int c = 0; c < nc; c++) cw[c] = fmaxf(cw[c], tw(cells[r][c]) + fs * 1.4f);
    float W = fs * 0.5f, rh = lh * 1.7f;
    for (int c = 0; c < nc; c++) W += cw[c];
    ImVec2 o = area_begin(id, W + fs * 0.5f, (float)nr * rh + fs * 0.5f);
    ImDrawList* dl = igGetWindowDrawList();
    for (int r = 0; r < nr; r++) {
        float x = o.x + fs * 0.25f, y = o.y + fs * 0.25f + (float)r * rh;
        if (r == 0) ImDrawList_AddRectFilled(dl, (ImVec2){ x, y }, (ImVec2){ x + W, y + rh }, rgba(0.20f, 0.30f, 0.48f, 1.0f), 4.0f, 0);
        else if (r % 2 == 0) ImDrawList_AddRectFilled(dl, (ImVec2){ x, y }, (ImVec2){ x + W, y + rh }, rgba(1, 1, 1, 0.035f), 0.0f, 0);
        for (int c = 0; c < nc; c++) {
            const char* t = cells[r][c];
            uint32_t col = r == 0 ? rgba(1, 1, 1, 1) : c == 0 ? rgba(0.75f, 0.77f, 0.8f, 1) : is_yes(t) ? rgba(0.45f, 0.85f, 0.5f, 1)
                         : is_no(t) ? rgba(0.95f, 0.45f, 0.42f, 1) : rgba(0.92f, 0.92f, 0.94f, 1);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ x + fs * 0.6f, y + (rh - lh) * 0.5f }, col, t, NULL);
            x += cw[c];
        }
    }
    area_end();
}

// --- bars ---
static void draw_bars(const char* body, const char* unit, int id) {
    char lab[16][96];
    float val[16], mx = 0.0f;
    int n = 0;
    const char* p = body;
    char line[200];
    while (*p && n < 16) {
        const char* e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), p);
        p = e ? e + 1 : p + len;
        char* colon = strrchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        snprintf(lab[n], sizeof lab[0], "%s", line);
        trim(lab[n]);
        val[n] = (float)atof(colon + 1);
        mx = fmaxf(mx, val[n]);
        n++;
    }
    if (!n) return;
    float fs = igGetFontSize(), lh = igGetTextLineHeight();
    float lw = 0.0f;
    for (int i = 0; i < n; i++) lw = fmaxf(lw, tw(lab[i]));
    float W = fminf(igGetContentRegionAvail().x - fs, fs * 34.0f), bw = W - lw - fs * 6.0f, rh = lh * 1.6f;
    ImVec2 o = area_begin(id, W, (float)n * rh + fs * 0.6f);
    ImDrawList* dl = igGetWindowDrawList();
    for (int i = 0; i < n; i++) {
        float y = o.y + fs * 0.3f + (float)i * rh, x = o.x + fs * 0.5f;
        ImDrawList_AddText_Vec2(dl, (ImVec2){ x, y + (rh - lh) * 0.5f }, rgba(0.85f, 0.86f, 0.9f, 1), lab[i], NULL);
        float bx = x + lw + fs, len = mx > 0 ? bw * val[i] / mx : 0.0f;
        float h = lh * 0.9f, by = y + (rh - h) * 0.5f;
        ImDrawList_AddRectFilled(dl, (ImVec2){ bx, by }, (ImVec2){ bx + bw, by + h }, rgba(1, 1, 1, 0.05f), 3.0f, 0);
        ImDrawList_AddRectFilledMultiColor(dl, (ImVec2){ bx, by }, (ImVec2){ bx + len, by + h }, rgba(0.25f, 0.55f, 0.95f, 1), rgba(0.45f, 0.75f, 1.0f, 1),
                                           rgba(0.45f, 0.75f, 1.0f, 1), rgba(0.25f, 0.55f, 0.95f, 1));
        char v[48];
        snprintf(v, sizeof v, "%g %s", val[i], unit);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ bx + len + fs * 0.4f, y + (rh - lh) * 0.5f }, rgba(0.95f, 0.95f, 0.97f, 1), v, NULL);
    }
    area_end();
}

bool assistant_draw_diagram(const char* kind, const char* body, int id, AsShowFn show) {
    char k[32], unit[16] = "";
    if (sscanf(kind, "%31s %15s", k, unit) < 1) return false;
    if (!strcmp(k, "blueprint")) draw_blueprint(body, id);
    else if (!strcmp(k, "steps")) draw_steps(body, id);
    else if (!strcmp(k, "keys")) draw_keys(body, id);
    else if (!strcmp(k, "axes")) draw_axes(body, id);
    else if (!strcmp(k, "layout")) draw_layout(body, id, show);
    else if (!strcmp(k, "compare")) draw_compare(body, id);
    else if (!strcmp(k, "bars")) draw_bars(body, unit, id);
    else return false;
    return true;
}
