// Blueprints (notas-dev/blueprints.md): Unreal's visual scripting. Assets in the Content folder, edited as graphs of
// nodes (bp_editor.c), compiled to a compact register bytecode run by a VM that allocates nothing while it runs.
//   .rbp        Blueprint Class: a parent (Actor, Pawn, Character, Game Mode or another Blueprint), components,
//               variables, interfaces it implements, the Event Graph and functions
//   .rstruct    Structure: named fields (a variable of it takes one slot per field)
//   .renum      Enumeration: named entries (a variable of it is an integer; Switch on <Enum> branches on it)
//   .rinterface Blueprint Interface: functions a class may implement (called on any actor: "Call <Function>")
//   .rdata      Data Asset: named values read by Blueprints ("Get Data Asset Value")
// The actors of a Blueprint carry CBlueprint (the class) and, for Characters, CCharacter (components.h).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

// --- Types of pins and variables ---
typedef enum BpType {
    BPT_EXEC = 0, // the white execution wire
    BPT_BOOL, BPT_INT, BPT_FLOAT, BPT_VECTOR, BPT_ROTATOR, BPT_STRING, BPT_ACTOR,
    BPT_STRUCT, // ref: the structure's asset
    BPT_ENUM,   // ref: the enumeration's asset
    // (files store the numbers: new types go at the end)
    BPT_BYTE, BPT_INT64, BPT_NAME, BPT_TEXT,
    BPT_COLOR,     // Linear Color: r g b a
    BPT_TRANSFORM, // three registers: location, rotation, scale
    BPT_CLASS,     // a Blueprint Class (ref: the base class; the value: its hash)
    BPT_OBJECT,    // an asset or a widget (ref: its kind or class)
    BPT_WILDCARD,  // a node's pin that takes the type of what is wired to it (the array nodes, Select, ...)
    BPT_COUNT
} BpType;
extern const char* const g_bp_type_names[BPT_COUNT];
// Unreal's containers: a variable or a pin holds one value, an Array, a Set (unique) or a Map (key -> value)
typedef enum BpContainer { BPC_SINGLE = 0, BPC_ARRAY, BPC_SET, BPC_MAP, BPC_COUNT } BpContainer;
static inline bool bp_type_stringish(uint8_t t) { return t == 6 /*BPT_STRING*/ || t == BPT_NAME || t == BPT_TEXT; }
static inline bool bp_type_numeric(uint8_t t) { return t == 1 || t == 2 || t == 3 || t == 9 /*enum*/ || t == BPT_BYTE || t == BPT_INT64; }
// A pin of this type can be split into its parts (Unreal's Split Struct Pin): vector, rotator, color, transform, struct
static inline bool bp_type_splittable(uint8_t t) { return t == 4 || t == 5 || t == 8 || t == BPT_COLOR || t == BPT_TRANSFORM; }

typedef enum BpAssetKind { BPA_CLASS = 0, BPA_STRUCT, BPA_ENUM, BPA_INTERFACE, BPA_DATA, BPA_MACROLIB, BPA_COUNT } BpAssetKind;
extern const char* const g_bp_asset_ext[BPA_COUNT];   // "rbp", "rstruct", ...
extern const char* const g_bp_asset_names[BPA_COUNT]; // "Blueprint Class", ...

// The native classes a Blueprint derives from (Unreal's)
// (files store the parent's name: new ones go at the end)
typedef enum BpNative {
    BPN_ACTOR = 0, BPN_PAWN, BPN_CHARACTER, BPN_GAME_MODE,
    BPN_PLAYER_CONTROLLER, BPN_AI_CONTROLLER, BPN_GAME_STATE, BPN_PLAYER_STATE, BPN_HUD, BPN_GAME_INSTANCE,
    BPN_ACTOR_COMPONENT, BPN_SCENE_COMPONENT, BPN_SAVE_GAME, BPN_ANIM_INSTANCE,
    BPN_LEVEL_SCRIPT, // a level's Level Blueprint (one per level, made by the editor)
    BPN_USER_WIDGET,  // a Widget Blueprint (UMG: its designer tree in BpAsset.ui)
    BPN_GAMEPLAY_ABILITY, // a Gameplay Ability (ability.h): its class defaults (cost, cooldown, tags...) and Event On Activate Ability
    BPN_COUNT
} BpNative;
// Whether the class's actors have a place in the world (a mesh to start with): Actor, Pawn, Character
static inline bool bp_native_spatial(BpNative n) { return n <= BPN_CHARACTER; }
extern const char* const g_bp_native_names[BPN_COUNT];

#define BP_NAME 48
#define BP_MAX_PINS 20 // (2026-11-19: 16 -> 20, the traces' Trace Complex; the pins are saved one by one)
#define BP_MAX_FIELDS 16
#define BP_MAX_ADEF 16 // an array variable's default elements

typedef struct BpValue { float v[4]; } BpValue; // a pin's literal / a variable's default (strings: s)

typedef struct BpVar {
    char name[BP_NAME];
    uint8_t type;
    char ref[BP_NAME];      // BPT_STRUCT / BPT_ENUM: the asset's name; BPT_ACTOR / BPT_CLASS: the class
    BpValue def;            // (a transform: the location; def3 the rotation and the scale)
    char sdef[64];          // BPT_STRING default
    bool editable;          // Instance Editable (shown in the actor's Details)
    bool save_game;         // Unreal's SaveGame flag: a Saveable actor's progress saves keep it (save_progress.h)
    char category[24];
    uint8_t cont, vtype;    // BpContainer; a Map's value type
    BpValue def3[2];
    uint8_t nadef;          // an array's / set's default elements
    BpValue adef[BP_MAX_ADEF];
    char tooltip[64];
    uint8_t rep;            // (Unreal's Replication) BP_REP_*: set on the server, sent to every client; RepNotify runs OnRep_<Name>
    bool expose_spawn;      // (2026-11-10) Unreal's Expose on Spawn: a pin on Spawn Actor from Class, set before its Construction Script and BeginPlay
} BpVar;
enum { BP_REP_NONE = 0, BP_REP_REPLICATED, BP_REP_NOTIFY };
// A Custom Event's Replicates (Unreal's): the node's net, low 2 bits the mode, bit 2 Reliable; calling it sends it
enum { BP_NET_NONE = 0, BP_NET_MULTICAST, BP_NET_SERVER, BP_NET_OWNER, BP_NET_RELIABLE = 4 };

typedef struct BpPin {
    char name[BP_NAME];
    uint8_t type, out;      // out: 1 output, 0 input
    char ref[BP_NAME];
    BpValue lit;            // an unconnected input's value
    char slit[64];
    uint8_t cont, vtype;    // BpContainer; a Map's value type
    uint8_t sub;            // a part of a split pin (1 + its index), 0: a whole pin
    uint8_t ptype;          // the split pin's type
    char pname[BP_NAME];    // the split pin's name
    char pref[BP_NAME];     // the split pin's ref (a structure)
    uint8_t wslot;          // a wildcard pin (resolved or not): 1 the element / key, 2 a Map's value; 0 none
    // (2026-11-10) Unreal's delegate (the red square): an exec-typed pin never followed as flow. A Custom Event's
    // OutputDelegate (last of its pins) wires into Bind Event to / Unbind / Set Timer by Event's Event input (last too):
    // that event is the one bound / timed
    uint8_t delegate;
    // (2026-11-10, "tudo nos blueprints tem que ser modular") the node's asset / name as an input (bp_asset_pin): its
    // literal is the node's text (Details), a wire gives it at run time (a Name / String variable, a Select, ...)
    uint8_t asset;
    // (2026-11-10) Unreal's Expose on Spawn: Spawn Actor from Class's pin for a variable of the chosen class (its name)
    uint8_t spawnarg;
} BpPin;

typedef struct BpNode {
    uint32_t id;
    uint16_t kind;          // BpNodeKind (blueprint.c's registry)
    float x, y;             // in the graph
    char text[BP_NAME];     // the variable / function / event / key / class / interface / asset it names
    char text2[BP_NAME];    // a second name (Data Asset value, interface function)
    uint8_t npins;
    BpPin pins[BP_MAX_PINS];
    char comment[96];
    // a wildcard node's resolved types (the first wire decides: element / key, map value), its pins split, its pins added
    uint8_t wtype, wcont, wtype2;
    char wref[BP_NAME];
    uint16_t split;         // bit i: the node's i-th pin (as its spec makes them) is split
    uint8_t extra;          // pins added (Sequence's outputs, Make Array's inputs)
    uint8_t net;            // a Custom Event's Replicates (BP_NET_*)
} BpNode;

typedef struct BpLink { uint32_t from_node, to_node; uint8_t from_pin, to_pin; } BpLink;

typedef struct BpGraph {
    char name[BP_NAME];     // "EventGraph" or the function's
    BpNode* nodes;
    uint32_t nnodes, cap_nodes;
    BpLink* links;
    uint32_t nlinks, cap_links;
    uint32_t next_id;
    float pan[2], zoom;     // the editor's view of it
    // a function's signature (its entry node's outputs, its return node's inputs)
    uint8_t nparams, nresults;
    BpVar params[6], results[4];
    uint8_t gkind;          // BpGraphKind
    char iface[BP_NAME];    // an interface function implemented here: the interface
} BpGraph;
typedef enum BpGraphKind { BPG_FUNCTION = 0, BPG_MACRO, BPG_DISPATCHER, BPG_EVENT } BpGraphKind; // (an event: a Custom Event's inputs, its signature only) // (a dispatcher: its signature only) // (graph 0 is the Event Graph)

// A compiled op (the VM's): op, three operands (registers, constants, jump targets) and a fourth for natives
typedef struct BpOp { uint16_t op, a, b, c, d; } BpOp;
// A register holding a container: each instance makes its own (elements of w registers; a Map's values of w2)
typedef struct BpArrSlot { uint16_t slot; uint8_t cont, w, w2, type; const BpVar* def; } BpArrSlot;

typedef struct BpEntry { char name[BP_NAME]; uint32_t pc; uint16_t kind; } BpEntry; // an event's start in the code

// The class's component tree (Unreal's Components panel, its Simple Construction Script): each node becomes a child
// entity of the actor (CAttach) holding the node's component, at the node's transform relative to its parent
typedef enum BpScsKind {
    SCS_SCENE = 0, SCS_STATIC_MESH, SCS_POINT_LIGHT, SCS_SPOT_LIGHT, SCS_RECT_LIGHT, SCS_CAMERA, SCS_SPRING_ARM,
    SCS_BOX, SCS_SPHERE, SCS_CAPSULE, SCS_ARROW,
    SCS_BLUEPRINT, // an Actor / Scene Component Blueprint: its graph runs on the actor (data: the class's hash)
    SCS_SKELETAL_MESH, // data: CSkeletalMesh (anim.h)
    SCS_AUDIO,         // data: CAudio (audio_world.c): Unreal's Audio Component
    SCS_KIND_COUNT
} BpScsKind;
extern const char* const g_scs_names[SCS_KIND_COUNT]; // Unreal's: "Static Mesh", "Spring Arm", ...
#define BP_MAX_SCS 32
#define BP_SCS_ROOT (-1) // a node's parent: the actor itself (its root)
// (2026-11-10) room for the biggest component a node holds (CSkeletalMesh: 340 bytes since its morph weights; it was 96,
// so a Character's Mesh never reached its actor: no mannequin in the level nor in the Blueprint's Viewport).
// bp_components.c checks each kind fits; the .rbp stores it as hex, older files (96 bytes) still open
#define BP_SCS_DATA 352
typedef struct BpScsNode {
    char name[BP_NAME];      // "CameraBoom"
    uint16_t kind;           // BpScsKind
    int16_t parent;          // an earlier node (parents come first) or BP_SCS_ROOT
    float location[3], rotation[3], scale[3];
    uint8_t data[BP_SCS_DATA]; // the node's component (bp_scs_component: CStaticMesh, CLight, CCamera, CSkeletalMesh, ...)
} BpScsNode;

typedef struct BpAsset {
    BpAssetKind kind;
    char name[BP_NAME];
    char path[512];         // on disk
    uint32_t hash;          // of its virtual path ("/Content/..."): what levels store
    char parent[BP_NAME];   // a native class name or another Blueprint's name
    // components the class's actors start with (a mask and each component's bytes, components.h)
    uint64_t comp_mask;
    uint8_t comp_data[COMP_COUNT][128];
    BpScsNode scs[BP_MAX_SCS];
    uint32_t nscs;
    BpVar vars[64];
    uint32_t nvars;
    char interfaces[8][BP_NAME];
    uint32_t ninterfaces;
    BpGraph graphs[16];     // [0]: the Event Graph; then the functions
    uint32_t ngraphs;
    // Structure fields / Enumeration entries / Interface functions / Data Asset values (vars)
    BpVar fields[BP_MAX_FIELDS];
    uint32_t nfields;
    char entries[32][BP_NAME];
    uint32_t nentries;
    char description[128];  // Unreal's Description of a Structure / an Enumeration (its editor)
    char entry_desc[32][64]; // an Enumeration's entries' descriptions
    bool dirty;
    bool relink;            // (read during the folder scan with a wire its pins couldn't take yet: read again after it)
    // compiled
    BpOp* code;
    uint32_t ncode, cap_code;
    BpValue* consts;
    uint32_t nconsts, cap_consts;
    char strings[128][96];
    uint32_t nstrings;
    BpArrSlot* arr;         // the registers holding containers (made per instance)
    uint32_t narr, cap_arr;
    BpEntry entries_code[64];
    uint32_t nentry;
    uint32_t nslots;        // registers per instance: self, the variables (a struct takes one per field), the nodes' outputs
    BpValue* init;          // nslots: an instance's registers when it starts (the variables' defaults)
    uint16_t var_slot[64];  // each variable's first slot
    uint32_t nlatent;       // Delay nodes: their resume slots per instance
    // (2026-11-16) Timelines (bp_timeline.h): the class's own (NULL: none), and each Timeline node's code (bp_vm.c)
    struct BpTimelines* tl;
    struct BpTlCode {
        const struct BpTimeline* tl;
        int32_t pc_update, pc_finished, pc_event[6];
        uint16_t reg_dir, reg_track[6];
        uint16_t node; uint8_t graph;
    } tlc[8];
    uint8_t ntlc;
    bool compiled;
    char error[256];        // the last compile's error ("" when it compiled)
    uint32_t error_node;
    struct AnimGraph* anim;  // an Animation Blueprint's Anim Graph (anim.h; NULL for the others)
    struct UiTree* ui;       // a Widget Blueprint's designer tree (ui_widget.h; NULL for the others)
    bool generated;          // made from a script (script.h): its graph is the compiler's, never saved as a .rbp
    // its native code (bp_jit.c: built the first time it runs, dropped by a compile): the memory, the entry
    void* jit;
    uint32_t (*jit_entry)(BpValue* r, const BpValue* k, void* ctx, uint32_t pc);
    bool jit_ok, jit_failed;
    // the debugger (bp_debug.h): each op's node (an index + 1 of dbg_nodes; 0: none), where the executable nodes start, the
    // variables' registers; the ops OP_BREAK replaces now (its native code waits meanwhile: jit_stale, made again later)
    uint16_t* op_src;
    struct BpDbgNode* dbg_nodes;
    struct BpDbgSite* dbg_sites;
    struct BpDbgVar* dbg_vars;
    uint32_t cap_src, ndbg_nodes, cap_dbg_nodes, ndbg_sites, cap_dbg_sites, ndbg_vars;
    uint32_t dbg_patched;
    bool jit_stale;
} BpAsset;

// --- Assets ---
void bp_init(const char* content_root); // the Content folder (scanned for assets on demand)
void bp_shutdown(void);
BpAssetKind bp_kind_of_path(const char* path, bool* is_bp);
// Creates a new asset file in the Content folder vpath (a folder) and returns it (NULL: failed); parent for classes
BpAsset* bp_create(BpAssetKind kind, const char* folder_vpath, const char* name, const char* parent);
BpAsset* bp_load(const char* vpath);   // cached; NULL when missing or invalid
BpAsset* bp_find_hash(uint32_t hash);  // a loaded asset, or the Content folder scanned for it
BpAsset* bp_find_name(BpAssetKind kind, const char* name);
bool bp_save(BpAsset* a);
// A class made elsewhere (a script compiled into a graph, script.c) put in the registry by its hash: an older one of
// the same hash takes its contents (the same pointer: the actors and nodes holding it follow); returns the one kept
BpAsset* bp_adopt(BpAsset* a);
// A class in memory (no file: a script's), its Event Graph and its parent's components; freed by bp_free_asset unless
// adopted. A variable added to it (its index; -1: 64 already)
BpAsset* bp_new_class(const char* vpath, const char* disk_path, const char* name, const char* parent);
void bp_free_asset(BpAsset* a);
int bp_add_var(BpAsset* a, const char* name, uint8_t type);
uint32_t bp_vpath_hash(const char* vpath);
BpNative bp_native_of(const BpAsset* a); // the native class at the root of its parents
bool bp_is_child_of(const BpAsset* a, const char* class_name);
uint32_t bp_asset_count(void); // every asset loaded (bp_find_* scans the Content folder once)
BpAsset* bp_asset_at(uint32_t i);
// The Content Browser renamed or moved an asset's file: its name, path and hash follow (old hashes still find it)
void bp_renamed(const char* old_vpath, const char* new_vpath);
// A structure, enumeration, interface or data asset changed: every class's pins made again, compiled again on play
void bp_refresh_all(void);
// The class's components (its defaults) copied to the level's actors of it (Details' values except the transform)
void bp_apply_defaults(World* w, const BpAsset* cls);

// --- The component tree (bp_components.c) ---
ComponentId bp_scs_component(uint32_t kind); // the component a node holds (COMP_COUNT: a Scene Component, none)
void bp_scs_defaults(BpScsNode* n, uint32_t kind);
int bp_scs_add(BpAsset* a, uint32_t kind, int parent, const char* name); // its index (-1: full); a unique name
void bp_scs_remove(BpAsset* a, int i);   // its children go to its parent
// A node's transform in the actor's space: axes (rows: local X, Y, Z scaled) and origin. socket: where its children
// hang (a Spring Arm's end); i BP_SCS_ROOT: the actor's root
void bp_scs_actor_space(const BpAsset* a, int i, bool socket, float axes[3][3], float origin[3]);
// The actor's components made again from its class (its children destroyed and created); cls NULL: its own class
void bp_components_build(World* w, Entity actor, const BpAsset* cls);
void bp_components_rebuild(World* w, const BpAsset* cls); // every actor of the class (NULL: every Blueprint actor)
// Each frame (editor and game): the components follow their parents, the Spring Arms reach out (with lag and their
// collision test while playing), the components of destroyed actors go
void bp_components_update(World* w, float dt, bool playing);
Entity bp_component_owner(const World* w, Entity e); // the actor a component belongs to (itself when it is not one)
bool bp_is_component(const World* w, Entity e);
// The actor's first Camera component: its world transform (false: none)
bool bp_component_camera(World* w, Entity actor, float pos[3], float* yaw, float* pitch, float* fov);
// (Q12) ... its roll too (a First Person Camera's lean, a shake) and the camera (NULL: not wanted)
struct CCamera;
bool bp_component_camera_ex(World* w, Entity actor, float pos[3], float* yaw, float* pitch, float* roll, const struct CCamera** cam);

// --- Graph editing (bp_editor.c) ---
typedef struct BpNodeDesc {
    const char* name;      // "Branch", "Print String"
    const char* category;  // "Flow Control", "Utilities", ...
    const char* tooltip;
    bool pure;             // no exec pins: evaluated where its outputs are used
} BpNodeDesc;
uint32_t bp_node_kind_count(void);
// (2026-11-10) a node of the Gameplay Framework (plugins.h PLUGIN_GAMEPLAY: Health, Inventory, Interaction, Pause Menu,
// Save Progress, Dialogue, Quests, Game Rules, Abilities, HUD): hidden from the menus while the plugin is off
bool bp_kind_gameplay(uint32_t kind);
// ... and its components (COMP_HEALTH .. COMP_HUD_LAYOUT)
#define BP_COMP_GAMEPLAY(c) ((c) >= COMP_HEALTH && (c) <= COMP_HUD_LAYOUT)
const BpNodeDesc* bp_node_desc(uint32_t kind);
// (2026-11-10) the name of the input pin that names a node's asset (Play Sequence's Sequence, Play Sound's Sound, a
// widget's name, a Blackboard key...): NULL when the node has none (its text is a function, an event, a variable)
const char* bp_asset_pin(uint32_t kind);
// A node of a kind at (x, y), its pins made for its text (a variable's type, a struct's fields, an enum's entries)
BpNode* bp_add_node(BpAsset* a, BpGraph* g, uint32_t kind, float x, float y, const char* text, const char* text2);
void bp_remove_node(BpGraph* g, uint32_t id);
BpNode* bp_node(BpGraph* g, uint32_t id);
bool bp_link(BpGraph* g, uint32_t from_node, uint8_t from_pin, uint32_t to_node, uint8_t to_pin); // false: incompatible
void bp_unlink_pin(BpGraph* g, uint32_t node, uint8_t pin);
bool bp_pin_linked(const BpGraph* g, uint32_t node, uint8_t pin);
bool bp_types_compatible(const BpPin* from, const BpPin* to);
void bp_refresh_pins(BpAsset* a, BpGraph* g, BpNode* n); // after a variable / struct / enum changed (links follow by name)
// Unreal's Split Struct Pin (a vector, rotator, color, transform, structure in its parts) and Recombine
bool bp_split_pin(BpAsset* a, BpGraph* g, BpNode* n, uint8_t pin, bool split);
// Macros (graphs of kind BPG_MACRO in a class or a Macro Library), interface functions (an interface's graphs)
const BpGraph* bp_find_macro(const BpAsset* a, const char* name, const char* lib);
const BpGraph* bp_interface_function(const char* iface, const char* func);
const char* bp_interface_of(const BpAsset* cls, const char* func); // the class's interface that has func (NULL: none)
BpGraph* bp_add_graph(BpAsset* a, const char* name, int gkind);
// An Event Dispatcher by name: of the class named cls ("": a) or its parents; NULL when none
const BpGraph* bp_dispatcher(const BpAsset* a, const char* name, const char* cls);
// A Custom Event's inputs (its signature graph, BPG_EVENT) in a class (cls: another class by name; NULL / "": a) or its parents
const BpGraph* bp_event_signature(const BpAsset* a, const char* name, const char* cls);    // a function, a macro (NULL: 16 graphs already)
// A pin's registers (a container: one, its handle; a transform: three; a structure: its fields')
uint32_t bp_pin_width(const BpPin* p);
uint32_t bp_var_width(const BpVar* v);
uint32_t bp_type_width(uint8_t type, const char* ref);
uint32_t bp_kind_by_name(const char* name);
void bp_add_function(BpAsset* a, const char* name);
int bp_var_index(const BpAsset* a, const char* name);
// Instance Editable values per actor (notas-dev/blueprints.md "Instance Editable por ator"): the name's hash (FNV-1a,
// never 0), whether a variable can have one (one register or text: not a container, struct, transform, actor, object or
// class), the actor's value of it (NULL: the class default), set (text or v) / cleared. Applied when the instance is
// made (Play, the game, the editor's Construction Script), before the Construction Script and BeginPlay
uint32_t bp_name_hash(const char* name);
bool bp_var_overridable(const BpVar* v);
struct CBlueprint;
struct CBpOverride* bp_override_find(struct CBlueprint* b, const char* var);
bool bp_override_set(struct CBlueprint* b, const char* var, const float v[4], const char* text);
void bp_override_clear(struct CBlueprint* b, const char* var);
// Unreal's ready-made graphs: a Character gets its movement, camera and jump
void bp_template_graph(BpAsset* a);

// --- Compile and run ---
bool bp_compile(BpAsset* a); // a->error / a->error_node when false
// Play (PIE or the game): every actor with CBlueprint gets its instance, BeginPlay runs; Tick each frame; Stop frees
void bp_begin_play(World* w);
void bp_tick(World* w, float dt);
void bp_end_play(World* w);
// The editor's Construction Script (sugestão #64): run on one actor of the editor's world, only that actor and its
// components change (no game runs); whether a class's does anything; every compile counted (they run again)
bool bp_construct_in_editor(World* w, Entity actor);
// Editor utilities (script_ide.h): a class's Custom Event run on the editor's level (no play), its self a hidden actor
// made for the run; the editor's selection through its hooks. False: no such event / class
bool bp_run_editor_event(World* w, BpAsset* cls, const char* event);
// The same, a variable of the run read at its end (the JIT's measure: the same work interpreted and native)
bool bp_run_event_read(World* w, BpAsset* cls, const char* event, const char* var, BpValue* out);
typedef struct BpEditorHooks {
    uint32_t (*selected_count)(void);
    Entity (*selected_at)(uint32_t i);
    void (*select)(Entity e, bool add);
} BpEditorHooks;
void bp_set_editor_hooks(const BpEditorHooks* h);
bool bp_has_construction(const BpAsset* cls);
extern uint32_t g_bp_compiles;
void bp_key_event(World* w, int key, bool pressed); // GLFW keys
int bp_debug_instance(Entity e, int* alive, int* inputs); // (the tests': its instance, -1 none)
bool bp_key_held(int key);                          // as the Blueprints see it (bp_key_event's last)
void bp_set_input(const float move[2], const float look[2]); // the axes this frame (W/S A/D, the mouse)
// The pointer and the fingers this frame (Get Mouse Position, Event Touch): the touch screen's points, else the mouse
// (pixels of the view; NULL: none) as a finger while its left button is down
void bp_touch_update(World* w, const float mouse[2], bool mouse_down);
void bp_get_input(float move[2], float look[2]);            // (the same, read back: a driven vehicle's)
Entity bp_player(World* w);               // the possessed pawn (a Character / Pawn), ENTITY_NONE: none
bool bp_player_camera(World* w, float pos[3], float* yaw, float* pitch); // the possessed pawn's view
// (Q12) ... with its roll and field of view (a pawn's First Person Camera: its own FOV; the Camera Shakes' FOV added;
// roll 0 and FOV 90 otherwise, as before)
bool bp_player_camera_ex(World* w, float pos[3], float* yaw, float* pitch, float* roll, float* fov);
// Print String's lines (the viewport shows them a few seconds, like Unreal)
uint32_t bp_messages(const char** lines, float* ages, uint32_t max);
// An actor of a class at a location (Spawn Actor, the viewport's drop)
Entity bp_spawn(World* w, BpAsset* cls, const float location[3], const float rotation[3]);
// Animation Blueprints (anim.c): a variable of an instance (the entity with its CBlueprint) and a custom event by name
bool bp_instance_var(World* w, Entity e, const char* name, BpValue* out);
bool bp_call_event(World* w, Entity e, const char* name);
bool bp_instance_var_text(World* w, Entity e, const char* name, char* out, size_t cap); // a variable as text (UMG's bindings)
// A Custom Event named so in the Event Graph (a free spot at its right), unless it is there already
void bp_add_custom_event(BpAsset* a, const char* name);
// An instance for an entity made while playing (an Animation Blueprint's): its BeginPlay now
void bp_start_instance(World* w, Entity e);
// The host's level streaming (level_stream.c, set by the editor's Play and the game): a request (its id, 0: refused),
// whether it is done, Open Level (between frames), the persistent level's name
typedef struct BpLevelHooks {
    uint32_t (*stream)(const char* level, bool load, bool visible);
    bool (*stream_done)(uint32_t request);
    void (*open)(const char* level);
    const char* (*current)(void);
} BpLevelHooks;
void bp_set_level_hooks(const BpLevelHooks* h);
// Statistics (the editor's stats line)
typedef struct BpStats { uint32_t instances, ops_last_frame; double ms_last_frame; uint32_t overrides; } BpStats; // overrides: Instance Editable values applied since the start
const BpStats* bp_stats(void);
// The Debug Viewer (debug_viewer.c): each instance's Tick and Delays timed while on (off: nothing measured)
typedef struct BpProfileItem { Entity e; const char* cls; float ms; uint32_t ops; } BpProfileItem;
void bp_profile(bool on);
// The live instances' last frame (up to cap); *movement_ms: the Characters' movement
uint32_t bp_profile_items(BpProfileItem* out, uint32_t cap, double* movement_ms);
// Online play and the saves (online.c): the possessed pawn, a plain variable read and written by name (no actors,
// objects or containers), every plain variable to bytes and back, events of a kind fired with their outputs' values
void bp_set_player(World* w, Entity e);
bool bp_get_instance_var(World* w, Entity e, const char* name, BpValue* out, uint32_t cap, uint32_t* width, char* text,
                         size_t tcap, bool* is_text);
bool bp_set_instance_var(World* w, Entity e, const char* name, const BpValue* v, uint32_t width);
bool bp_set_instance_text(World* w, Entity e, const char* name, const char* text);
uint32_t bp_vars_dump(World* w, Entity e, uint8_t* out, uint32_t cap);
uint32_t bp_vars_dump_ex(World* w, Entity e, uint8_t* out, uint32_t cap, bool save_only); // save_only: the variables marked SaveGame
bool bp_vars_load(World* w, Entity e, const uint8_t* d, uint32_t len);
void bp_fire_event(World* w, uint16_t kind, const BpValue* args, uint32_t nargs);
void bp_make_string(BpValue* v, const char* s);
void bp_online_events(World* w); // the network's and Steam's events into the Blueprints (once a frame)
// The events of a kind on one actor's instances: its own and its Actor Components' (inventory.c: On Inventory Changed,
// On Item Used, On Item Equipped), with their outputs' values
void bp_fire_actor_event(World* w, Entity actor, uint16_t kind, const BpValue* args, uint32_t nargs);
// Destroys an actor as Destroy Actor does while playing (its End Play first; the player's pawn let go). save_progress.c
void bp_destroy_actor(World* w, Entity e, bool end_play); // end_play false: an actor whose BeginPlay never ran
// (B13) the network's prediction (online.c): one Character move with a given input outside the tick (the server applying
// a client's move, the client replaying its moves after a correction); Characters the tick leaves alone (a remote
// player's pawn on the server: its moves move it); the possessed pawn's move each tick (input, jump, dt), after it moved
void bp_character_step(World* w, Entity e, const float input[2], bool jump, float dt);
void bp_character_skip(Entity e, bool skip);
// Jump on e's Character, as the Jump node (a Motion Matching database with jumps: the animation's)
void bp_character_jump(World* w, Entity e);
typedef void (*BpMoveHook)(World* w, Entity e, const float input[2], bool jump, float dt);
void bp_set_move_hook(BpMoveHook hook);
// The actor's Blueprint class has a variable marked SaveGame (save_progress.h)
bool bp_has_save_vars(World* w, Entity e);
// A Custom Event on an actor with its inputs' values (interaction.c: a Lever's target, its bool On); false: none so named
bool bp_call_event_args(World* w, Entity e, const char* name, const BpValue* args, uint32_t nargs);
// A Custom Event on an actor (its class's) with its inputs as text, converted to their types (dialogue.c's actions):
// false when the actor has no such event or nothing plays
bool bp_call_event_text(World* w, Entity e, const char* name, const char* const* args, uint32_t nargs);
// The Characters' collision boxes (bp_vm.c): an actor made while playing joins them at the next tick (bp_spawn calls it;
// a Pickup dropped, a network spawn too), a destroyed one leaves them (bp_destroy_actor); the tests' counts: the boxes,
// those added for spawned actors, the frames a platform turned a Character, the last add / sweep / build's ms
void bp_box_spawned(Entity e);
void bp_box_stats(uint32_t* boxes, uint32_t* spawned, uint32_t* turns, double* ms);
void bp_boxes_changed(void); // the boxes made again at the next tick (procgen.c's facilities, ar_arcore.c's planes)
