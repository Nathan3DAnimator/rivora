// (2026-11-25, bug-fixer) --place-hitch-test "<class>" <ms> ("Play": the Play button): the UI sweep saw a frame of 1096 ms when Add > Visual Effects >
// Heterogeneous Volume was clicked. The class placed from the catalog (as the Add menu does) once the editor settled,
// then its longest frame until its background work is done (at least 2 s): over <ms> fails
#include <stdio.h>
#include <string.h>

#include "async.h"
#include "assistant_ui.h"
#include "content.h"
#include "editor.h"

static struct {
    int frames, phase; // 0 settling, 1 placed
    double worst, t_placed;
    bool placed;
} H;

// true: finished (ok in *ok)
bool place_hitch_test_frame(Editor* ed, const char* name, double max_ms, double dt, double now, bool* ok) {
    H.frames++;
    if (H.phase == 0) {
        if (H.frames < 240 || !async_idle()) return false; // (the start's pipelines and materials first)
        if (!_stricmp(name, "Play")) { // (the Play button: the UI sweep's frame of 1284 ms, the upscalers asked in it)
            editor_play(ed, now);
            H.placed = ed->playing;
        } else if (!_stricmp(name, "OpenAsset")) { // (File > Open Asset: the Content walked in the frame, 1.0 s in the sweep)
            ed->show_open_asset = true;
            H.placed = true;
        } else if (!_stricmp(name, "Assistant")) { // (Help > Assistant: its base built in the frame, 0.4 s in the sweep)
            assistant_ui_open(NULL);
            H.placed = true;
        } else if (!strncmp(name, "/Content/", 9)) { // (an asset opened as the Content Browser opens it: its editor)
            content_open_asset(name);
            H.placed = true;
        } else {
            int found = -1;
            for (int i = 0; i < editor_place_count() && found < 0; i++)
                if (!_stricmp(editor_place_name(i, NULL), name)) found = i;
            if (found < 0) {
                printf("[RealEngine] FAIL: place hitch test: no class \"%s\" in the catalog\n", name);
                *ok = false;
                return true;
            }
            H.placed = editor_place_spawn(ed, found, now) != ENTITY_NONE;
        }
        printf("[RealEngine] Place hitch test: \"%s\" placed: %s\n", name, H.placed ? "yes" : "no");
        H.phase = 1, H.frames = 0, H.t_placed = now, H.worst = 0.0;
        return false;
    }
    if (dt * 1000.0 > H.worst) H.worst = dt * 1000.0; // (frame 1's dt: the frame that placed it)
    if (now - H.t_placed < 2.0 || !async_idle()) {
        if (now - H.t_placed < 60.0) return false;
    }
    *ok = H.placed && H.worst <= max_ms;
    printf("[RealEngine] Place hitch test: \"%s\": the longest frame after it %.0f ms (at most %.0f)\n", name, H.worst, max_ms);
    if (!*ok) printf("[RealEngine] FAIL: place hitch test: a frame of %.0f ms after placing \"%s\"\n", H.worst, name);
    else printf("[RealEngine] Place hitch test OK\n");
    return true;
}
