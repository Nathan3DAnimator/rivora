// The graph menus (notas-dev/editor.md "Menu do botão direito"): the right button in every graph editor offers what
// that graph supports, and on top of it the last 2 actions used in that kind of graph and 2 recommended ones that
// connect with what is being done there (the wire being dragged, the selected node, or the last one placed).
// The recommendations: pairs learned as the user works (after A, B was placed) weighed with a table of the usual
// pairs (Unreal's habits: BeginPlay -> Print String, Get Actor Location -> Set Actor Location...). Kept per user in
// %APPDATA%\RealEngine\graph_menu.txt. The editors filter both by what their graph has and what fits the wire.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum GraphMenuKind {
    GM_BP_EVENT = 0,   // a Blueprint's Event Graph (Level Blueprint, Anim / Widget Blueprints' too)
    GM_BP_FUNCTION,    // a function (no events, nothing latent)
    GM_BP_MACRO,       // a macro (no events)
    GM_BP_CONSTRUCTION,// the Construction Script (no events, nothing latent, the actor's own)
    GM_ANIM_GRAPH,     // an Animation Blueprint's AnimGraph
    GM_MATERIAL,       // the Material Editor's nodes
    GM_BEHAVIOR_TREE,  // a Behavior Tree's composites, tasks, decorators, services
    GM_SOUND_CUE,      // a Sound Cue's nodes
    GM_PARTICLES,      // a Particle System's modules (particles.h)
    GM_COUNT
} GraphMenuKind;

#define GM_RECENT_SHOWN 2
#define GM_RECOMMEND_SHOWN 2

// An action placed in a graph of kind g: item (its menu name) after context (the node it connects to or follows; ""
// none). It becomes the first recent and the pair is learned (saved a moment later)
void gm_note(GraphMenuKind g, const char* context, const char* item);
// The last used, newest first (out: pointers into the module's table, valid until the next gm_note)
int gm_recent(GraphMenuKind g, const char** out, int max);
// The ones recommended after context, best first: learned pairs (their counts) and the table's, then (none) the most
// used of the graph; accept (may be NULL) keeps only what the graph has and what connects
typedef bool (*GmAccept)(const char* item, void* user);
int gm_recommend(GraphMenuKind g, const char* context, GmAccept accept, void* user, const char** out, int max);
// Section titles (translated): "Recent", "Recommended"
const char* gm_title_recent(void);
const char* gm_title_recommended(void);
// The self-test (--graph-menu-test): learning, ranking, the table, save and load. 0 = ok
int gm_selftest(void);
