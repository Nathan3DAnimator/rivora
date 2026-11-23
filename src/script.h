// Scripting (notas-dev/scripts.md): game code written as text in four languages, compiled inside the engine.
//   * Real (.real): the engine's own language — indentation like Python, static types when wanted, events and
//     functions by name, string interpolation ("HP: {hp}"), ranges (for i in 0..10)
//   * C# (.cs): a class deriving from Actor (Character, Pawn...), fields, methods, the usual statements
//   * Python (.py): a class deriving from Actor; snake_case names reach the engine's PascalCase ones
//   * C (.c): the file is the class (its parent in a "// parent: Character" line); globals are its fields
// Every script is one class. Its source is parsed into one tree (whatever its language), checked, then lowered into
// a Blueprint graph that the Blueprints' compiler turns into the VM's register bytecode: a script runs wherever a
// Blueprint runs (the editor's Play, the packaged game, online), with the whole engine API (every node of the
// Blueprints is a function: PlaySound2D("Boom"), GetActorLocation(), SpawnActorFromClass(...), ...), its events
// (BeginPlay, Tick, EndPlay, overlaps, Custom Events: widgets', notifies', sequences', OnAudioFinished_...), its
// timers and Delay. A script's class is placed, spawned and parented like a Blueprint Class.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum ScriptLang { SL_REAL = 0, SL_CSHARP, SL_PYTHON, SL_C, SL_COUNT } ScriptLang;
extern const char* const g_script_lang_names[SL_COUNT]; // "Real", "C#", "Python", "C"
extern const char* const g_script_lang_ext[SL_COUNT];   // "real", "cs", "py", "c"

// A message of the compiler (an error stops the class; a warning doesn't)
typedef struct ScriptDiag {
    int line, col;          // 1-based
    bool error;
    char text[160];
} ScriptDiag;
#define SCRIPT_MAX_DIAGS 64

// What the IDE knows of a script after its last compile (completion, go to definition, outline)
typedef enum { SYM_FIELD = 0, SYM_METHOD, SYM_EVENT, SYM_PARAM, SYM_LOCAL, SYM_CLASS, SYM_API, SYM_KEYWORD } ScriptSymKind;
typedef struct ScriptSym {
    char name[48];
    char type[32];          // "float", "Vector", "void (float dt)"...
    uint8_t kind;           // ScriptSymKind
    int line, col;          // where it is declared (0: the engine's)
    int scope_from, scope_to; // a local's / parameter's lines (0: the whole file)
} ScriptSym;

typedef struct ScriptFile {
    char vpath[512], path[600];
    char name[64];          // the class's (the file's name)
    ScriptLang lang;
    int64_t mtime;          // of the file last compiled
    uint32_t hash;          // of its vpath: the class's (actors hold it like a Blueprint's)
    struct BpAsset* cls;    // the class made (NULL: it never compiled)
    bool ok;                // the last compile worked
    ScriptDiag diags[SCRIPT_MAX_DIAGS];
    uint32_t ndiags;
    ScriptSym* syms;        // the declarations seen (fields, methods, parameters, locals)
    uint32_t nsyms;
    double compile_ms;
    uint32_t nodes, ops;    // the graph's nodes made, the bytecode's ops
} ScriptFile;

// --- The registry: the scripts of the Content folder, compiled on demand, again when their file changes ---
void script_init(const char* content_root);
void script_shutdown(void);
ScriptLang script_lang_of(const char* path);     // by its extension (SL_COUNT: not a script)
bool script_is_path(const char* path);
ScriptFile* script_load(const char* vpath);      // compiled (or the last result); NULL: not a script
ScriptFile* script_find_hash(uint32_t hash);
uint32_t script_count(void);
ScriptFile* script_at(uint32_t i);
void script_rescan(void);
// Compiles text as the file's new source (the IDE's live check: nothing written; adopt: the class replaced too)
bool script_compile_text(ScriptFile* f, const char* text, size_t len, bool adopt);
// Every frame (the editor, not while playing: a running class isn't swapped): the files changed on disk compiled again
void script_hot_reload(bool playing);
// A new script of a language in a Content folder (a class deriving from parent), written; its vpath in out
bool script_create(const char* folder_vpath, const char* name, ScriptLang lang, const char* parent, char* out_vpath, size_t cap);
// The template a new script starts from (BeginPlay prints, Tick is there)
const char* script_template(ScriptLang lang);

// --- The engine API as the scripts see it (the IDE's completion, the reference window) ---
typedef struct ScriptApi {
    char name[64];          // PascalCase: "PlaySound2D"
    char sig[160];          // "void PlaySound2D(string sound, float VolumeMultiplier = 1, ...)"
    const char* category;   // the node's ("Audio", "Transformation"...)
    const char* doc;        // its description
    uint16_t kind;          // the node it calls
    bool pure, text_arg;    // pure: no side effect (a value); text_arg: its first argument is the node's asset / name
    char fixed[48];         // (B14c) a plugin's function: the node's text (its name), not an argument
} ScriptApi;
uint32_t script_api_count(void);
const ScriptApi* script_api_at(uint32_t i);
const ScriptApi* script_api_find(const char* name); // PascalCase, snake_case or any case
// The language's keywords and built-in names (colors, completion)
const char* const* script_keywords(ScriptLang lang, uint32_t* n);
const char* const* script_builtin_types(ScriptLang lang, uint32_t* n);
