// (Q19, notas-dev/importar-tlender.md) The conversion report of an import: what came in the same, what was converted
// into an Engine equivalent, what was baked (on the GPU, into a texture or a curve) and what was lost, each with its
// reason. The importer (a worker; one import at a time) fills it; the editor's main thread publishes it when the import
// is done (Window > Import Report) and it is written to the project's Saved/ImportReports/<source>.txt (never into Content: 2026-11-24).
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum ImpRepKind { IMPREP_SAME = 0, IMPREP_CONVERTED, IMPREP_BAKED, IMPREP_LOST, IMPREP_KIND_COUNT } ImpRepKind;
typedef enum ImpRepCat {
    IMPREP_CAT_MESH = 0, IMPREP_CAT_MODIFIER, IMPREP_CAT_NODE, IMPREP_CAT_TEXTURE, IMPREP_CAT_DRIVER, IMPREP_CAT_RIG,
    IMPREP_CAT_SCENE, IMPREP_CAT_COUNT
} ImpRepCat;

typedef struct ImpRepItem {
    unsigned char kind, cat;
    unsigned short count;      // the same item and reason seen this many times (other owners: "owner (+n)")
    char owner[64];            // the object, material or bone it belongs to
    char item[64];             // the modifier, node, image, driver
    char reason[200];          // why (translated when written)
} ImpRepItem;

// The importer (its thread): a new report for the file src (its name shown), an item (merged with an equal one: same kind,
// category, item and reason), the counts per kind
void imprep_begin(const char* src);
void imprep_add(int kind, int cat, const char* owner, const char* item, const char* reason_fmt, ...);
void imprep_counts(unsigned out[IMPREP_KIND_COUNT]);
// Written to <project>/Saved/ImportReports/<source name>.txt, the project being the folder that holds the Content folder
// folder_disk is in (UTF-8: a summary line, then a line per item by kind). False when it could
// not write the file. imprep_summary: the counts at the end of the importer's line ("; report: 3 same, ...")
bool imprep_finish(const char* folder_disk, char* text, size_t text_cap);
void imprep_summary(char* text, size_t text_cap);
// The milliseconds of the import's steps (the test's measure: --import prints them)
void imprep_time(const char* step, double ms);

// The main thread: the last finished report becomes the one shown (after the importer is done with it)
void imprep_publish(void);
const ImpRepItem* imprep_items(int* count, const char** source, const char** file);
const char* imprep_kind_name(int kind); // "Same", "Converted", "Baked", "Lost" (msgids)
const char* imprep_cat_name(int cat);
bool* imprep_show_flag(void);
void imprep_window(void); // Window > Import Report (ImGui; nothing while closed)
