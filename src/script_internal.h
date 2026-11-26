// The scripts' compiler inside (script.h): the tokens, the tree every language parses into, the arena it lives in.
#pragma once
#include "script.h"

// --- Types (what the checker infers, what the Blueprints' pins carry) ---
typedef enum STy {
    ST_UNKNOWN = 0, ST_VOID, ST_BOOL, ST_INT, ST_FLOAT, ST_STRING, ST_VECTOR, ST_ROTATOR, ST_ACTOR, ST_COLOR,
    ST_TRANSFORM, ST_INT64, ST_OBJECT, ST_CLASS, ST_COUNT
} STy;
#define ST_ARRAY 0x80 // | an element type: an array of it
// A structure (one of the script's own, or a Blueprint structure of the project): ST_STRUCT + its index in the compile's
// table (SCtx.snames); its name by s_ty_name
#define ST_STRUCT 0x40
#define S_MAX_STRUCTS 32
#define ST_IS_STRUCT(t) (((t) & 0x7F) >= ST_STRUCT && ((t) & 0x7F) < ST_STRUCT + S_MAX_STRUCTS)
extern const char* const g_sty_names[ST_COUNT];

// --- Tokens ---
typedef enum TokKind {
    TK_EOF = 0, TK_NEWLINE, TK_INDENT, TK_DEDENT, TK_IDENT, TK_INT, TK_FLOAT, TK_STRING, TK_FSTRING, TK_OP, TK_ERROR
} TokKind;
typedef struct Tok {
    uint8_t kind;
    char op[4];          // TK_OP: "+", "==", "+=", "->", "..", "{"...
    int line, col;
    const char* s;       // TK_IDENT / TK_STRING / TK_FSTRING: its text (the arena's), TK_ERROR: the message
    double num;
} Tok;

// --- The tree ---
typedef enum ExprKind {
    EX_INT = 0, EX_FLOAT, EX_BOOL, EX_STRING, EX_NONE, EX_NAME, EX_SELF, EX_MEMBER, EX_CALL, EX_UNARY, EX_BINARY,
    EX_INDEX, EX_TERNARY, EX_INTERP, EX_LIST, EX_NEW
} ExprKind;
typedef struct Expr {
    uint8_t k;
    char op[4];              // EX_UNARY / EX_BINARY: "+", "and", "==" (normalized: "&&" -> "and", "!" -> "not")
    int line, col;
    double num;
    const char* s;           // EX_NAME / EX_MEMBER: the name; EX_STRING: the text; EX_NEW: the type
    struct Expr *a, *b, *c;  // operands; EX_MEMBER / EX_CALL / EX_INDEX: a is the object (NULL: a plain call)
    struct Expr** args;      // EX_CALL / EX_INTERP / EX_LIST / EX_NEW
    int nargs;
} Expr;

typedef enum StmtKind {
    SS_EXPR = 0, SS_VAR, SS_ASSIGN, SS_IF, SS_WHILE, SS_FOR_RANGE, SS_FOR_EACH, SS_FOR_C, SS_BREAK, SS_CONTINUE,
    SS_RETURN, SS_BLOCK, SS_PASS
} StmtKind;
typedef struct Stmt {
    uint8_t k;
    char op[4];              // SS_ASSIGN: "=", "+=", "-=", "*=", "/=", "%="
    int line, col;
    const char* name;        // SS_VAR / SS_FOR_*: the variable
    uint8_t type;            // SS_VAR: declared (ST_UNKNOWN: inferred)
    Expr* e;                 // SS_EXPR, SS_VAR's value, SS_ASSIGN's value, SS_IF / SS_WHILE's condition, SS_RETURN's
    Expr* target;            // SS_ASSIGN
    Expr *lo, *hi, *step;    // SS_FOR_RANGE: lo .. hi (exclusive unless inclusive), step; SS_FOR_EACH: lo the array
    bool inclusive;
    struct Stmt** body;
    int nbody;
    struct Stmt** els;       // SS_IF's else (an elif: one SS_IF in it)
    int nels;
    struct Stmt *init, *post; // SS_FOR_C
} Stmt;

typedef struct SParam { const char* name; uint8_t type; int line, col; } SParam;
#define S_MAX_PARAMS 6
typedef struct SMethod {
    const char* name;        // as written (the engine's events by their PascalCase: Tick, BeginPlay...)
    int line, col, end_line;
    uint8_t ret;
    SParam params[S_MAX_PARAMS];
    int nparams;
    Stmt** body;
    int nbody;
} SMethod;
typedef struct SField { const char* name; uint8_t type; Expr* init; int line, col; bool editable, save_game; const char* asset_ref; } SField; // (asset_ref: 2026-11-24, a Level / Sound... typed reference)
// A structure declared in the script (Real's struct, C#'s / C's struct, Python's @dataclass): its fields, in order
#define S_MAX_SFIELDS 24
typedef struct SStruct { const char* name; int line, col; SField fields[S_MAX_SFIELDS]; int nfields; } SStruct;
#define S_MAX_SSTRUCTS 8
#define S_MAX_FIELDS 48
#define S_MAX_METHODS 14
typedef struct SClass {
    const char* name;
    const char* parent;      // "Actor" when not said
    int line;
    SField fields[S_MAX_FIELDS];
    int nfields;
    SMethod methods[S_MAX_METHODS];
    int nmethods;
    SStruct structs[S_MAX_SSTRUCTS];
    int nstructs;
} SClass;

// --- The arena (a compile's memory: freed at once) and the diagnostics ---
typedef struct SArena { char* blocks[256]; size_t used, cap; int nblocks; } SArena;
void* sa_alloc(SArena* a, size_t n);
const char* sa_str(SArena* a, const char* s, size_t n);
void sa_free(SArena* a);

typedef struct SCtx {
    ScriptLang lang;
    SArena arena;
    ScriptDiag* diags;
    uint32_t* ndiags;
    ScriptSym* syms;
    uint32_t nsyms, cap_syms;
    const char* file_name;   // the class's name when the source doesn't say (C, a class without a name)
    const char* snames[S_MAX_STRUCTS]; // the structures' types (ST_STRUCT + i): the script's first, then the project's used
    int nsnames, ndeclared;  // (ndeclared: the script's own, at the table's start)
} SCtx;
void s_diag(SCtx* c, int line, int col, bool error, const char* fmt, ...);
void s_sym(SCtx* c, ScriptSymKind kind, const char* name, const char* type, int line, int col, int from, int to);

// The lexer (every language; indent: Real's and Python's INDENT / DEDENT) and the parsers
Tok* s_lex(SCtx* c, const char* src, size_t len, int* ntok);
SClass* s_parse(SCtx* c, Tok* t, int n); // by c->lang
// Types by name in any language ("float", "Vector3", "List<int>", "str"...): ST_UNKNOWN if none
uint8_t s_type_named(const char* name);
// (2026-11-24) an asset reference's type (Level, LevelSequence, Sound...): its class as the Blueprint variables name it; NULL: none
const char* s_asset_ref_named(const char* name);
// A structure's type by name: the script's (declared) or a Blueprint structure of the project (ST_UNKNOWN: none)
uint8_t s_struct_type(SCtx* c, const char* name);
const char* s_ty_name(SCtx* c, uint8_t ty); // a type's name (structures by theirs; arrays without the [])
// The class lowered into a Blueprint graph and compiled (script_lower.c)
struct BpAsset;
bool s_lower(SCtx* c, SClass* cls, struct BpAsset* a);
// The script's structures made Blueprint structures (registered: the class's Make / Break nodes read them)
bool s_make_structs(SCtx* c, SClass* cls, const char* vpath, const char* disk);
// The engine API's lookup (script.c)
const ScriptApi* s_api_find(const char* name);
