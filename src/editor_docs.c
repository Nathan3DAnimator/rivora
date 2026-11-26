// The asset editors' open documents, all of them (editor_docs.h; 2026-11-25, bug-fixer: switching projects didn't
// close the editors, and the Blueprints' windows pointed at what bp_shutdown had freed)
#include "editor_docs.h"

#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "editor.h"
#include "i18n.h"
#include "project.h"
#include "theme.h"

typedef int (*DocsFn)(int op, struct Editor* ed, char* names, size_t cap);
static const DocsFn g_docs[] = { bp_editor_docs, mat_editor_docs, asset_viewer_docs, anim_editor_docs, ikrig_editor_docs,
                                 ai_editor_docs, input_editor_docs, seq_editor_docs, audio_editor_docs, script_ide_docs,
                                 particle_editor_docs, vehicle_editor_docs, dlg_editor_docs, quest_editor_docs,
                                 physmat_editor_docs, camshake_editor_docs, pcg_editor_docs, ui_designer_docs };
#define NDOCS ((int)(sizeof g_docs / sizeof g_docs[0]))

static struct {
    char project[600];
    int tpl;
    bool ask, up, discard;
    int test_answer;
    char names[4096];
    char error[200];
} S;

void editor_docs_add_name(char* names, size_t cap, const char* name) {
    if (!names || !cap) return;
    size_t l = strlen(names);
    if (l + 2 < cap) snprintf(names + l, cap - l, "%s\n", name && name[0] ? name : "?");
}

static int each(int op, Editor* ed, char* names, size_t cap) {
    if (names && cap) names[0] = 0;
    int n = 0;
    for (int i = 0; i < NDOCS; i++) n += g_docs[i](op, ed, names, cap);
    return n;
}

int editor_docs_open(void) { return each(DOCS_OPEN, NULL, NULL, 0); }
int editor_docs_unsaved(char* names, size_t cap) { return each(DOCS_UNSAVED, NULL, names, cap); }
void editor_docs_discard(void) { S.discard = true; }
int editor_docs_save(Editor* ed) { return each(DOCS_SAVE, ed, NULL, 0); }

void editor_docs_finish_quit(Editor* ed) {
    if (!S.discard) bp_editor_docs(DOCS_SAVE, ed, NULL, 0); // (editor_shutdown's bp_editor_save_all, while ImGui lives)
    S.discard = true;
    editor_docs_finish(ed);
}

void editor_docs_finish(Editor* ed) {
    if (!S.discard) {
        int failed = each(DOCS_SAVE, ed, NULL, 0);
        if (failed) printf("[RealEngine] %d asset(s) could not be saved before their editors closed\n", failed);
    }
    each(DOCS_CLOSE, ed, NULL, 0);
    S.discard = false;
}

bool editor_docs_switch_ask(Editor* ed, const char* rproject, int tpl) {
    // (no project open yet: the start's own, nothing of a project to lose; the prompt already up: the new choice waits)
    if (!project_file()[0]) return false;
    int n = editor_docs_unsaved(S.names, sizeof S.names);
    if (!n && !ed->dirty) return false;
    snprintf(S.project, sizeof S.project, "%s", rproject);
    S.tpl = tpl, S.ask = true, S.error[0] = 0;
    return true;
}

bool editor_docs_prompt_up(void) { return S.ask || S.up; }
void editor_docs_test_answer(int answer) { S.test_answer = answer; }

static void go(Editor* ed) { // the switch, between frames (main.c): editor_open_project finishes the documents
    snprintf(ed->pending_project, sizeof ed->pending_project, "%s", S.project);
    ed->pending_tpl = S.tpl;
    S.up = false;
}

void editor_docs_popup(Editor* ed, double now) {
    char title[96];
    snprintf(title, sizeof title, "%s###docs_switch", TR("Save changes?"));
    if (S.ask) {
        igOpenPopup_Str(title, 0);
        S.ask = false, S.up = true;
    }
    if (!S.up) return;
    if (!igBeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        S.up = false; // (closed from outside)
        return;
    }
    igText("%s", TR("Before opening the other project, these have unsaved changes:"));
    if (ed->dirty) igBulletText("%s", TR("The level"));
    S.names[0] = 0;
    editor_docs_unsaved(S.names, sizeof S.names);
    for (char *l = S.names, *e; *l; l = e + 1) {
        e = strchr(l, '\n');
        if (!e) break;
        igBulletText("%.*s", (int)(e - l), l);
    }
    if (S.error[0]) igTextColored(THV(TK_ERROR), "%s", S.error);
    int answer = S.test_answer;
    S.test_answer = 0;
    float bw = igGetFontSize() * 8.0f;
    if (igButton(TR("Save and Open"), (ImVec2){ bw, 0 }) || answer == 1) {
        bool ok = !ed->dirty || editor_save_level(ed, ed->level_path, now);
        ok = editor_docs_save(ed) == 0 && ok;
        if (ok) go(ed), igCloseCurrentPopup();
        else snprintf(S.error, sizeof S.error, "%s", TR("Some could not be saved: the project was not switched."));
    }
    igSameLine(0, -1);
    if (igButton(TR("Don't Save"), (ImVec2){ bw, 0 }) || answer == 2) {
        S.discard = true, ed->dirty = false; // (the level's changes go with it, as the editors')
        go(ed), igCloseCurrentPopup();
    }
    igSameLine(0, -1);
    if (igButton(TR("Cancel"), (ImVec2){ bw, 0 }) || answer == 3 || igIsKeyPressed_Bool(ImGuiKey_Escape, false))
        S.up = false, igCloseCurrentPopup();
    igEndPopup();
}
