// (2026-11-23, the user: "a system to reorganize the nodes of the shader, of any Blueprint, of every node graph")
// Organize Nodes: a layered layout (Sugiyama's, kept small) every node editor shares. Columns by the longest way to the
// graph's ends (a flow graph: each node one column left of the furthest node it feeds - the outputs, the events' chains
// on the right; a tree: by depth from its root, top down), each column's order by its neighbours' places (barycenters),
// then stacked without overlaps; the result anchored where the graph was. Only the selection when there is one.
#include "graph_layout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct GlWork {
    int* rank;
    int* order;   // the nodes sorted by column, then by their desired place
    float* want;  // a node's desired place along its column
    int* stack;   // the DFS's state: 0 new, 1 on the way, 2 done
} GlWork;

// the longest way to a sink (flow) or from a source (tree), cycles cut where they close
static int gl_rank(const GlNode* n, int count, const GlEdge* e, int ne, const bool* only, bool from_sources, int v, GlWork* w, int depth) {
    if (w->stack[v] == 2) return w->rank[v];
    if (w->stack[v] == 1 || depth > 4096) return 0; // (a loop: cut here)
    w->stack[v] = 1;
    int r = 0;
    for (int k = 0; k < ne; k++) {
        int a = from_sources ? e[k].to : e[k].from, b = from_sources ? e[k].from : e[k].to; // (a's rank above b's)
        if (a != v || b < 0 || b >= count || (only && !only[b]) || b == v) continue;
        int rb = gl_rank(n, count, e, ne, only, from_sources, b, w, depth + 1) + 1;
        if (rb > r) r = rb;
    }
    w->stack[v] = 2;
    w->rank[v] = r;
    return r;
}

static const float* g_sort_key;
static const float* g_sort_orig; // (a tie - siblings under one parent - keeps their old order: a Behavior Tree runs its
                                 // children left to right)
static const int* g_sort_rank;
static int gl_cmp(const void* pa, const void* pb) {
    int a = *(const int*)pa, b = *(const int*)pb;
    if (g_sort_rank[a] != g_sort_rank[b]) return g_sort_rank[a] - g_sort_rank[b];
    if (g_sort_key[a] != g_sort_key[b]) return g_sort_key[a] < g_sort_key[b] ? -1 : 1;
    if (g_sort_orig[a] != g_sort_orig[b]) return g_sort_orig[a] < g_sort_orig[b] ? -1 : 1;
    return a - b;
}

bool graph_layout(GlNode* n, int count, const GlEdge* e, int ne, const bool* only, int mode, float gap_col, float gap_row) {
    if (count <= 0) return false;
    bool tree = mode == GL_TREE_DOWN || mode == GL_TREE_RIGHT, down = mode == GL_TREE_DOWN;
    // a tree going down: laid out sideways (x along its depth) and turned back
    if (down)
        for (int i = 0; i < count; i++) {
            float t = n[i].x;
            n[i].x = n[i].y, n[i].y = t, t = n[i].w, n[i].w = n[i].h, n[i].h = t;
        }
    GlWork w;
    w.rank = calloc((size_t)count, sizeof(int)), w.order = malloc(sizeof(int) * (size_t)count);
    w.want = malloc(sizeof(float) * (size_t)count), w.stack = calloc((size_t)count, sizeof(int));
    float* orig = malloc(sizeof(float) * (size_t)count);
    int m = 0;
    bool ok = w.rank && w.order && w.want && w.stack && orig;
    for (int i = 0; ok && i < count; i++) orig[i] = n[i].y;
    float right = -1e30f, left = 1e30f, top = 1e30f;
    for (int i = 0; ok && i < count; i++) {
        if (only && !only[i]) continue;
        gl_rank(n, count, e, ne, only, tree, i, &w, 0);
        w.order[m++] = i, w.want[i] = n[i].y;
        if (n[i].x + n[i].w > right) right = n[i].x + n[i].w;
        if (n[i].x < left) left = n[i].x;
        if (n[i].y < top) top = n[i].y;
    }
    if (ok && m > 1) {
        int cols = 0;
        for (int k = 0; k < m; k++)
            if (w.rank[w.order[k]] + 1 > cols) cols = w.rank[w.order[k]] + 1;
        // the columns' places: a flow from the right (rank 0 the ends), a tree from the left (rank 0 its root)
        float* colx = calloc((size_t)cols, sizeof(float));
        float* colw = calloc((size_t)cols, sizeof(float));
        if (colx && colw) {
            for (int k = 0; k < m; k++) {
                int v = w.order[k];
                if (n[v].w > colw[w.rank[v]]) colw[w.rank[v]] = n[v].w;
            }
            if (tree) {
                float x = left;
                for (int c = 0; c < cols; c++) colx[c] = x, x += colw[c] + gap_col;
            } else {
                float x = right;
                for (int c = 0; c < cols; c++) x -= colw[c], colx[c] = x, x -= gap_col;
            }
            // column by column from rank 0: each node wants the middle of the ones it links to in the columns placed
            // before; the column sorted by that and stacked down from it (no overlaps)
            g_sort_key = w.want, g_sort_rank = w.rank, g_sort_orig = orig;
            qsort(w.order, (size_t)m, sizeof(int), gl_cmp); // (rank 0 first, by its old place)
            float* place = malloc(sizeof(float) * (size_t)count);
            if (place) {
                int k = 0;
                for (int c = 0; c < cols; c++) {
                    int k0 = k;
                    while (k < m && w.rank[w.order[k]] == c) k++;
                    for (int q = k0; q < k && c > 0; q++) { // the desired place: the mean of the neighbours already placed
                        int v = w.order[q];
                        float sum = 0.0f;
                        int cnt = 0;
                        for (int l = 0; l < ne; l++) {
                            int a = tree ? e[l].to : e[l].from, b = tree ? e[l].from : e[l].to;
                            if (a != v || b < 0 || b >= count || (only && !only[b]) || w.rank[b] >= c) continue;
                            sum += place[b] + n[b].h * 0.5f, cnt++;
                        }
                        w.want[v] = cnt ? sum / (float)cnt - n[v].h * 0.5f : n[v].y;
                    }
                    qsort(w.order + k0, (size_t)(k - k0), sizeof(int), gl_cmp);
                    float y = -1e30f;
                    for (int q = k0; q < k; q++) {
                        int v = w.order[q];
                        float py = c == 0 ? (q == k0 ? top : y) : fmaxf(w.want[v], y);
                        place[v] = py, y = py + n[v].h + gap_row;
                    }
                }
                for (int q = 0; q < m; q++) {
                    int v = w.order[q];
                    n[v].x = tree ? colx[w.rank[v]] : colx[w.rank[v]] + colw[w.rank[v]] - n[v].w; // (a flow's column: right-aligned)
                    n[v].y = place[v];
                }
                // anchored: the laid out block's top where the graph's was
                float ty = 1e30f;
                for (int q = 0; q < m; q++) ty = fminf(ty, n[w.order[q]].y);
                for (int q = 0; q < m; q++) n[w.order[q]].y += top - ty;
                free(place);
            } else
                ok = false;
        } else
            ok = false;
        free(colx), free(colw);
    }
    if (down)
        for (int i = 0; i < count; i++) {
            float t = n[i].x;
            n[i].x = n[i].y, n[i].y = t, t = n[i].w, n[i].w = n[i].h, n[i].h = t;
        }
    free(w.rank), free(w.order), free(w.want), free(w.stack), free(orig);
    return ok && m > 1;
}

// --graph-layout-test: a material-like flow scrambled (no overlaps after, every wire left to right, the output where it
// was, only the selection moved), a tree (children under their parent in their old order), a loop (no hang). 0: passed
static bool gl_overlap(const GlNode* a, const GlNode* b) {
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}
int graph_layout_selftest(void) {
    int fails = 0;
    // 0 output <- 1 principled <- 2 color tex, 3 rough tex, 4 normal map <- 5 normal tex; 6 mapping -> 2, 3, 5; 7 loose
    GlNode n[8];
    for (int i = 0; i < 8; i++) n[i] = (GlNode){ (float)((i * 397) % 700), (float)((i * 211) % 500), 160.0f + 20.0f * (float)(i % 3), 90.0f + 30.0f * (float)(i % 4) };
    const GlEdge e[] = { { 1, 0 }, { 2, 1 }, { 3, 1 }, { 4, 1 }, { 5, 4 }, { 6, 2 }, { 6, 3 }, { 6, 5 } };
    float out_right = n[0].x + n[0].w;
    if (!graph_layout(n, 8, e, 8, NULL, GL_FLOW_RIGHT, 60.0f, 20.0f)) printf("[RealEngine] FAIL: graph layout: nothing done\n"), fails++;
    for (int i = 0; i < 8; i++)
        for (int j = i + 1; j < 8; j++)
            if (gl_overlap(&n[i], &n[j])) printf("[RealEngine] FAIL: graph layout: nodes %d and %d overlap\n", i, j), fails++;
    for (int k = 0; k < 8; k++)
        if (n[e[k].from].x + n[e[k].from].w > n[e[k].to].x) printf("[RealEngine] FAIL: graph layout: the wire %d -> %d goes back\n", e[k].from, e[k].to), fails++;
    if (fabsf(n[0].x + n[0].w - out_right) > 0.5f && n[7].x + n[7].w < out_right - 0.5f) printf("[RealEngine] FAIL: graph layout: the output moved off its place\n"), fails++;
    // only the selection
    GlNode m[3] = { { 0, 0, 100, 50 }, { 500, 300, 100, 50 }, { 900, 900, 100, 50 } };
    const GlEdge me[] = { { 0, 1 } };
    bool only[3] = { true, true, false };
    graph_layout(m, 3, me, 1, only, GL_FLOW_RIGHT, 60.0f, 20.0f);
    if (m[2].x != 900.0f || m[2].y != 900.0f) printf("[RealEngine] FAIL: graph layout: a node not selected moved\n"), fails++;
    // a tree down: root 0; children 1, 2, 3 (old order by x: 3, 1, 2); 4 under 1
    GlNode t[5] = { { 0, 0, 190, 60 }, { 50, 400, 190, 60 }, { 600, 380, 190, 60 }, { -400, 420, 190, 60 }, { 0, 900, 190, 60 } };
    const GlEdge te[] = { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 4 } };
    graph_layout(t, 5, te, 4, NULL, GL_TREE_DOWN, 50.0f, 30.0f);
    if (!(t[3].x < t[1].x && t[1].x < t[2].x)) printf("[RealEngine] FAIL: graph layout: the tree's children left their order\n"), fails++;
    for (int k = 0; k < 4; k++)
        if (t[te[k].to].y < t[te[k].from].y + t[te[k].from].h) printf("[RealEngine] FAIL: graph layout: a child above its parent\n"), fails++;
    // a loop (a state machine's, a Blueprint's): laid out, no hang
    GlNode l[3] = { { 0, 0, 100, 40 }, { 0, 0, 100, 40 }, { 0, 0, 100, 40 } };
    const GlEdge le[] = { { 0, 1 }, { 1, 2 }, { 2, 0 } };
    graph_layout(l, 3, le, 3, NULL, GL_FLOW_RIGHT, 60.0f, 20.0f);
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            if (gl_overlap(&l[i], &l[j])) printf("[RealEngine] FAIL: graph layout: a loop's nodes overlap\n"), fails++;
    printf("[RealEngine] graph layout: a flow of 8, a selection, a tree of 5, a loop: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}

