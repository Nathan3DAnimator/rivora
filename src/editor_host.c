// The Editors window: the asset editors' tabs, of any kind, side by side (editor_host.h; 2026-11-25, item 9)
#include "editor_host.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "content.h"
#include "editor.h"
#include "editor_dock.h"
#include "i18n.h"
#include "platform_files.h"
#include "theme.h"

static struct {
    ImGuiWindowClass cls;
    bool cls_ready;
    int want;       // frames it shows though empty (a document on its way in)
    int docs;       // the documents in it (the last frame)
    int drawn;      // the frame it was submitted (a document docks into it only after: its dock space exists)
    bool placed;    // it took its first place (over the level viewport, as a class's window did: Q26)
    bool shown;
} H = { .drawn = -1 };

static const ImGuiWindowClass* doc_class(void) {
    if (!H.cls_ready) {
        memset(&H.cls, 0, sizeof H.cls);
        H.cls.ClassId = igImHashStr("RivoraAssetEditor", 0, 0);
        H.cls.ParentViewportId = (ImGuiID)-1; // (the default: the main window, io.ConfigViewportsNoDefaultParent false)
        H.cls.DockingAllowUnclassed = true;   // (they dock beside the level editor's panels too, and those beside them)
        H.cls_ready = true;
    }
    return &H.cls;
}

unsigned edhost_dockspace(void) { return igImHashStr("rivora_editors_dockspace", 0, 0); }
unsigned edhost_class_id(void) { return doc_class()->ClassId; }
int edhost_docs(void) { return H.docs; }
bool edhost_shown(void) { return H.shown; }

void edhost_reset(void) { H.placed = false, H.want = 0; }

static int count(const ImGuiDockNode* n) {
    if (!n) return 0;
    return n->Windows.Size + count(n->ChildNodes[0]) + count(n->ChildNodes[1]);
}

void edhost_frame(Editor* ed) {
    (void)ed;
    H.shown = false;
    if (H.want <= 0 && H.docs <= 0) return; // (nothing in it: no window)
    if (H.want > 0) H.want--;
    if (!H.placed) { // its first place: over the level viewport (the panels around it), else most of the screen
        H.placed = true;
        float vr[4];
        const ImGuiViewport* mv = igGetMainViewport();
        if (dock_panel_rect(DP_VIEWPORT, vr) && vr[2] > 600.0f && vr[3] > 400.0f) {
            igSetNextWindowPos((ImVec2){ vr[0], vr[1] }, ImGuiCond_Always, (ImVec2){ 0, 0 });
            igSetNextWindowSize((ImVec2){ vr[2], vr[3] }, ImGuiCond_Always);
        } else {
            ImVec2 ws = { mv->Size.x * 0.8f, mv->Size.y * 0.8f };
            igSetNextWindowPos((ImVec2){ mv->Pos.x + (mv->Size.x - ws.x) * 0.5f, mv->Pos.y + (mv->Size.y - ws.y) * 0.5f }, ImGuiCond_Always,
                               (ImVec2){ 0, 0 });
            igSetNextWindowSize(ws, ImGuiCond_Always);
        }
    }
    char title[96];
    snprintf(title, sizeof title, "%s###rivora_editors", TR("Editors"));
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){ 0, 0 });
    bool open = igBegin(title, NULL, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    igPopStyleVar(1);
    // (hidden behind another tab or minimized: the dock space kept alive, its documents with it)
    igDockSpace(edhost_dockspace(), (ImVec2){ 0, 0 }, open ? ImGuiDockNodeFlags_None : ImGuiDockNodeFlags_KeepAliveOnly, NULL);
    igEnd();
    H.drawn = igGetFrameCount();
    H.shown = true;
    H.docs = count(igDockBuilderGetNode(edhost_dockspace()));
}

void edhost_doc(bool* to_host) {
    igSetNextWindowClass(doc_class());
    if (!to_host || !*to_host) return;
    H.want = 3; // (the window comes up for it; it docks in on the next frame if the window was not drawn on this one)
    if (H.drawn != igGetFrameCount()) return;
    igSetNextWindowDockID(edhost_dockspace(), ImGuiCond_Always);
    *to_host = false;
}

// --- The layout's documents (Saved/Layout.rlayout: the editors open when the project closed, opened again) ----------
void bp_editor_doc_paths(void (*cb)(const char* path, void* user), void* user);
void mat_editor_doc_paths(void (*cb)(const char* path, void* user), void* user);
void asset_viewer_doc_paths(void (*cb)(const char* path, void* user), void* user);

typedef struct DocOut { char* out; size_t cap, len; } DocOut;
static void doc_cb(const char* path, void* user) {
    DocOut* o = user;
    char vp[700];
    if (!strncmp(path, "/Content", 8)) snprintf(vp, sizeof vp, "%s", path);
    else { // a file on disk under the project's Content: its /Content path
        const char* root = content_root();
        size_t rl = strlen(root);
        if (rl == 0 || strlen(path) <= rl) return;
        for (size_t i = 0; i < rl; i++) {
            char a = root[i], b = path[i];
            if (a == '\\') a = '/';
            if (b == '\\') b = '/';
            if (tolower((unsigned char)a) != tolower((unsigned char)b)) return; // (not under Content: not kept)
        }
        snprintf(vp, sizeof vp, "/Content%s", path + rl);
        for (char* c = vp; *c; c++)
            if (*c == '\\') *c = '/';
    }
    if (strchr(vp, '\n')) return;
    int w = snprintf(o->out + o->len, o->cap - o->len, "open=%s\n", vp);
    if (w > 0 && o->len + (size_t)w < o->cap) o->len += (size_t)w;
}

size_t edhost_layout_write(char* out, size_t cap) {
    if (!out || cap < 64) return 0;
    DocOut o = { out, cap, 0 };
    o.len = (size_t)snprintf(out, cap, "[RealEngine][Documents]\n");
    bp_editor_doc_paths(doc_cb, &o);
    mat_editor_doc_paths(doc_cb, &o);
    asset_viewer_doc_paths(doc_cb, &o);
    if (o.len + 2 < cap) out[o.len++] = '\n', out[o.len] = 0;
    return o.len;
}

int edhost_layout_read(const char* text) {
    const char* s = text ? strstr(text, "[RealEngine][Documents]") : NULL;
    if (!s) return 0;
    s = strchr(s, '\n');
    int n = 0;
    while (s && s[1] == 'o' && !strncmp(s + 1, "open=", 5)) {
        const char* a = s + 6;
        const char* e = strchr(a, '\n');
        size_t l = e ? (size_t)(e - a) : strlen(a);
        char vp[700];
        if (l && l < sizeof vp && !strncmp(a, "/Content", 8)) {
            memcpy(vp, a, l), vp[l] = 0;
            if (vp[l - 1] == '\r') vp[l - 1] = 0;
            char disk[1100];
            snprintf(disk, sizeof disk, "%s%s", content_root(), vp + 8);
            if (re_file_stat(disk, NULL, NULL, NULL)) content_open_asset(vp), n++; // (gone since: skipped)
        }
        s = e;
    }
    return n;
}
