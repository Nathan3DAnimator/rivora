// Scripts (script.h): the registry of the Content folder's scripts, the compile pipeline (lex, parse, lower into a
// Blueprint graph, the Blueprints' compiler), the engine API as functions, the templates.
#include "script.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "plugins.h"
#include "platform.h"
#include "platform_files.h"
#include "script_internal.h"

const char* const g_script_lang_names[SL_COUNT] = { "Rivora Script", "C#", "Python", "C", "C++" }; // (2026-11-25: "Real" until item 13)
const char* const g_script_lang_ext[SL_COUNT] = { "rvs", "cs", "py", "c", "cpp" }; // (a new Rivora Script is .rvs; .real still opens)

#define MAX_SCRIPTS 512
static struct {
    char root[512];
    ScriptFile* f[MAX_SCRIPTS];
    uint32_t n;
    bool scanned;
    double next_check;
    ScriptApi* api;
    uint32_t napi;
    uint32_t api_plugins; // (B14c) the plugins' functions it was built with (more loaded since: built again)
} S;

// --- The engine API: every node of the Blueprints but the graph's own (events, flow, variables, math operators) ---
static bool api_excluded(uint32_t kind, const BpNodeDesc* d) {
    if (!d || !d->name || !d->name[0]) return true;
    switch (kind) {
    case BPK_EVENT_BEGIN_PLAY: case BPK_EVENT_TICK: case BPK_EVENT_KEY: case BPK_EVENT_CUSTOM: case BPK_EVENT_INTERFACE: case BPK_EVENT_TOUCH:
    case BPK_CALL_CUSTOM: case BPK_BRANCH: case BPK_SEQUENCE: case BPK_FOR_LOOP: case BPK_VAR_GET: case BPK_VAR_SET:
    case BPK_FUNC_ENTRY: case BPK_FUNC_RETURN: case BPK_CALL_FUNCTION: case BPK_MACRO: case BPK_TUNNEL_IN: case BPK_TUNNEL_OUT:
    case BPK_REROUTE: case BPK_WHILE: case BPK_FOR_LOOP_BREAK: case BPK_FOREACH: case BPK_FOREACH_BREAK: case BPK_SWITCH_INT:
    case BPK_SWITCH_ENUM: case BPK_FLIPFLOP: case BPK_GATE: case BPK_DO_N: case BPK_DO_ONCE: case BPK_EVENT_END_PLAY:
    case BPK_EVENT_BEGIN_OVERLAP: case BPK_EVENT_END_OVERLAP: case BPK_EVENT_OVERLAPPING: case BPK_EVENT_UPDATE_ANIM:
    case BPK_EVENT_INPUT_ACTION: case BPK_MAKE_STRUCT: case BPK_BREAK_STRUCT: case BPK_ENUM_LITERAL: case BPK_DISPATCH_CALL:
    case BPK_DISPATCH_BIND: case BPK_DISPATCH_UNBIND: case BPK_DISPATCH_UNBIND_ALL: case BPK_EVENT_DISPATCH:
        return true;
    default: break;
    }
    if (!strncmp(d->name, "Event ", 6)) return true;
    return false;
}

static void sanitize(const char* in, char* out, size_t cap) { // "Play Sound 2D" -> "PlaySound2D", "Play (Audio)" -> "PlayAudio"
    size_t o = 0;
    bool up = true;
    for (const char* s = in; *s && o + 1 < cap; s++) {
        if (isalnum((unsigned char)*s)) {
            out[o++] = up ? (char)toupper((unsigned char)*s) : *s;
            up = false;
        } else up = true;
    }
    out[o] = 0;
}
static const char* pin_type_name(const BpPin* p) {
    if (p->cont == BPC_ARRAY) return "array";
    switch (p->type) {
    case BPT_BOOL: return "bool";
    case BPT_INT: case BPT_BYTE: case BPT_ENUM: return "int";
    case BPT_INT64: return "int64";
    case BPT_FLOAT: return "float";
    case BPT_VECTOR: return "Vector";
    case BPT_ROTATOR: return "Rotator";
    case BPT_STRING: case BPT_NAME: case BPT_TEXT: return "string";
    case BPT_ACTOR: return "Actor";
    case BPT_COLOR: return "Color";
    case BPT_TRANSFORM: return "Transform";
    case BPT_CLASS: return "Class";
    case BPT_OBJECT: return "Object";
    case BPT_WILDCARD: return "any";
    default: return "?";
    }
}

static void api_build(void) {
    if (S.api && S.api_plugins == plugin_fn_count()) return;
    free(S.api), S.api = NULL, S.napi = 0;
    S.api_plugins = plugin_fn_count();
    uint32_t nk = bp_node_kind_count();
    uint32_t np = plugin_fn_count(); // (B14c) the plugins' functions after the engine's: SmoothStep(...) for "Smooth Step"
    S.api = calloc(nk + np, sizeof *S.api); // (once: the engine's functions)
    if (!S.api) return;
    BpAsset* tmp = bp_new_class("/Content/__api__.real", "", "__api__", "Actor"); // (nodes made once each to read their pins)
    for (uint32_t kk = 0; kk < nk + np && tmp; kk++) {
        const PluginFn* pf = kk >= nk ? plugin_fn_at(kk - nk) : NULL;
        uint32_t k = pf ? (pf->pure ? BPK_PLUGIN_PURE : BPK_PLUGIN_CALL) : kk;
        BpNodeDesc pd;
        if (pf) pd = (BpNodeDesc){ pf->name, pf->category, pf->tooltip, pf->pure };
        const BpNodeDesc* d = pf ? &pd : bp_node_desc(k);
        if (!pf && (api_excluded(k, d) || k == BPK_PLUGIN_PURE || k == BPK_PLUGIN_CALL)) continue;
        ScriptApi* a = &S.api[S.napi];
        sanitize(d->name, a->name, sizeof a->name);
        if (!a->name[0]) continue;
        bool dup = false; // (two nodes of one name: the first)
        for (uint32_t j = 0; j < S.napi; j++) dup |= !strcmp(S.api[j].name, a->name);
        if (dup) {
            memset(a, 0, sizeof *a);
            continue;
        }
        a->kind = (uint16_t)k, a->pure = d->pure, a->category = d->category, a->doc = d->tooltip;
        a->text_arg = !pf && d->tooltip && (strstr(d->tooltip, "in Details") || strstr(d->tooltip, "(Details)"));
        if (pf) snprintf(a->fixed, sizeof a->fixed, "%s", pf->name), a->category = pf->category, a->doc = pf->tooltip;
        BpGraph* g = &tmp->graphs[0];
        BpNode* n = bp_add_node(tmp, g, k, 0, 0, pf ? pf->name : NULL, NULL);
        const char* ret = "void";
        char args[128] = "";
        size_t o = 0;
        if (a->text_arg) o += (size_t)snprintf(args + o, sizeof args - o, "string name");
        for (int i = 0; n && i < n->npins; i++) {
            const BpPin* p = &n->pins[i];
            if (p->type == BPT_EXEC) continue;
            if (p->out) {
                if (!strcmp(ret, "void")) ret = pin_type_name(p);
                continue;
            }
            if (!strcmp(p->name, "Target") && p->type == BPT_ACTOR) continue; // (self, or obj.F(); a vector Target is an argument)
            char pn[48];
            sanitize(p->name[0] ? p->name : "Value", pn, sizeof pn);
            if (o < sizeof args - 2) o += (size_t)snprintf(args + o, sizeof args - o, "%s%s %s", o ? ", " : "", pin_type_name(p), pn);
        }
        snprintf(a->sig, sizeof a->sig, "%s %s(%s)", ret, a->name, args);
        if (n) bp_remove_node(g, n->id);
        S.napi++;
    }
    bp_free_asset(tmp);
}

uint32_t script_api_count(void) {
    api_build();
    return S.napi;
}
const ScriptApi* script_api_at(uint32_t i) {
    api_build();
    return i < S.napi ? &S.api[i] : NULL;
}
static void norm(const char* s, char* out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 1 < cap; s++)
        if (*s != '_') out[o++] = (char)tolower((unsigned char)*s);
    out[o] = 0;
}
const ScriptApi* script_api_find(const char* name) {
    api_build();
    if (!name || !name[0]) return NULL;
    for (uint32_t i = 0; i < S.napi; i++)
        if (!strcmp(S.api[i].name, name)) return &S.api[i];
    char n[80], m[80];
    norm(name, n, sizeof n);
    for (uint32_t i = 0; i < S.napi; i++) {
        norm(S.api[i].name, m, sizeof m);
        if (!strcmp(m, n)) return &S.api[i];
    }
    // the usual other spellings: GetLocation -> GetActorLocation, Destroy -> DestroyActor...
    static const char* const alias[][2] = { { "getlocation", "GetActorLocation" }, { "setlocation", "SetActorLocation" },
                                            { "getrotation", "GetActorRotation" }, { "setrotation", "SetActorRotation" },
                                            { "destroy", "DestroyActor" }, { "spawn", "SpawnActorFromClass" },
                                            { "deltatime", "GetWorldDeltaSeconds" }, { "time", "GetGameTimeInSeconds" },
                                            { "player", "GetPlayerCharacter" }, { "isvalid", "IsValid" } };
    for (size_t k = 0; k < sizeof alias / sizeof alias[0]; k++)
        if (!strcmp(n, alias[k][0])) return script_api_find(alias[k][1]);
    return NULL;
}
const ScriptApi* s_api_find(const char* name) { return script_api_find(name); }

// --- Keywords (the IDE's colors and completion) ---
const char* const* script_keywords(ScriptLang lang, uint32_t* n) {
    static const char* const real[] = { "class", "extends", "var", "let", "const", "func", "fn", "event", "if", "elif", "else", "while", "for",
                                        "in", "step", "match", "case", "default", "break", "continue", "return", "pass", "and", "or", "not", "true", "false", "null", "self" };
    static const char* const cs[] = { "using", "namespace", "class", "public", "private", "protected", "static", "readonly", "const",
                                      "override", "virtual", "void", "var", "new", "if", "else", "while", "for", "foreach", "in", "break",
                                      "continue", "return", "switch", "case", "default", "true", "false", "null", "this" };
    static const char* const py[] = { "import", "from", "class", "def", "self", "if", "elif", "else", "while", "for", "in", "range", "break",
                                      "continue", "return", "match", "case", "pass", "and", "or", "not", "is", "True", "False", "None" };
    static const char* const c[] = { "void", "static", "const", "if", "else", "while", "for", "break", "continue", "return", "struct",
                                     "typedef", "switch", "case", "default", "true", "false", "NULL" };
    static const char* const cpp[] = { "class", "struct", "public", "private", "protected", "virtual", "override", "final", "const", "static",
                                       "auto", "void", "namespace", "using", "if", "else", "while", "for", "do", "break", "continue", "return",
                                       "switch", "case", "default", "true", "false", "nullptr", "this", "static_cast", "enum" };
    switch (lang) {
    case SL_REAL: *n = sizeof real / sizeof real[0]; return real;
    case SL_CSHARP: *n = sizeof cs / sizeof cs[0]; return cs;
    case SL_PYTHON: *n = sizeof py / sizeof py[0]; return py;
    case SL_CPP: *n = sizeof cpp / sizeof cpp[0]; return cpp;
    default: *n = sizeof c / sizeof c[0]; return c;
    }
}
const char* const* script_builtin_types(ScriptLang lang, uint32_t* n) {
    static const char* const real[] = { "bool", "int", "int64", "float", "string", "Vector", "Rotator", "Color", "Transform", "Actor", "Class", "Object" };
    static const char* const cs[] = { "bool", "int", "long", "float", "double", "string", "Vector3", "Vector", "Rotator", "Color", "Transform",
                                      "Actor", "List", "Character", "Pawn" };
    static const char* const py[] = { "bool", "int", "float", "str", "list", "Vector", "Rotator", "Color", "Transform", "Actor" };
    static const char* const c[] = { "bool", "int", "int64_t", "float", "double", "char", "Vector", "Rotator", "Color", "Actor" };
    static const char* const cpp[] = { "bool", "int", "int32", "int64_t", "float", "double", "string", "vector", "FVector", "FRotator",
                                       "FString", "FLinearColor", "TArray", "Vector", "Rotator", "Color", "Actor", "Character", "Pawn" };
    switch (lang) {
    case SL_REAL: *n = sizeof real / sizeof real[0]; return real;
    case SL_CSHARP: *n = sizeof cs / sizeof cs[0]; return cs;
    case SL_PYTHON: *n = sizeof py / sizeof py[0]; return py;
    case SL_CPP: *n = sizeof cpp / sizeof cpp[0]; return cpp;
    default: *n = sizeof c / sizeof c[0]; return c;
    }
}

// --- Templates ---
const char* script_template(ScriptLang lang) {
    switch (lang) {
    case SL_CSHARP:
        return "using RealEngine;\n\npublic class NewScript : Actor\n{\n    [Editable] public float Speed = 100.0f;\n\n"
               "    void BeginPlay()\n    {\n        Print($\"Hello from {GetDisplayName()}!\");\n    }\n\n"
               "    void Tick(float dt)\n    {\n        AddActorWorldOffset(new Vector(0, 0, 0) * dt);\n    }\n}\n";
    case SL_PYTHON:
        return "from realengine import *\n\nclass NewScript(Actor):\n    speed: float = 100.0\n\n"
               "    def begin_play(self):\n        print(f\"Hello from {get_display_name()}!\")\n\n"
               "    def tick(self, dt: float):\n        pass\n";
    case SL_CPP:
        return "#include \"rivora.hpp\"\n\nclass NewScript : public Actor {\npublic:\n    float Speed = 100.0f; // (a field: the Details shows it)\n\n"
               "    void BeginPlay() override {\n        Print(\"Hello from \" + GetDisplayName() + \"!\");\n    }\n\n"
               "    void Tick(float dt) override {\n    }\n};\n";
    case SL_C:
        return "// parent: Actor\n#include \"realengine.h\"\n\nfloat speed = 100.0f;\n\n"
               "void BeginPlay(void)\n{\n    Print(\"Hello from C!\");\n}\n\n"
               "void Tick(float dt)\n{\n}\n";
    default:
        return "class NewScript extends Actor\n    @editable\n    var speed: float = 100\n\n"
               "    event BeginPlay()\n        print(\"Hello from {GetDisplayName()}!\")\n\n"
               "    event Tick(dt: float)\n        pass\n";
    }
}

// --- The registry ---
ScriptLang script_lang_of(const char* path) {
    const char* dot = path ? strrchr(path, '.') : NULL;
    for (int k = 0; dot && k < SL_COUNT; k++)
        if (!_stricmp(dot + 1, g_script_lang_ext[k])) return (ScriptLang)k;
    if (dot && !_stricmp(dot + 1, "real")) return SL_REAL; // (2026-11-25, item 13: Rivora Script's old extension)
    if (dot && !_stricmp(dot + 1, "rcpp")) return SL_CPP;  // (a C++ script kept apart from a plugin's native .cpp)
    return SL_COUNT;
}
bool script_is_path(const char* path) { return script_lang_of(path) != SL_COUNT; }

static void disk_of(const char* vpath, char* out, size_t cap) {
    if (!strncmp(vpath, "/Content", 8)) snprintf(out, cap, "%s%s", S.root, vpath + 8);
    else snprintf(out, cap, "%s", vpath);
}

static char* read_file(const char* path, size_t* len) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = malloc((size_t)(n > 0 ? n : 0) + 1); // (a compile, not a frame)
    if (t && n > 0 && fread(t, 1, (size_t)n, f) != (size_t)n) free(t), t = NULL;
    fclose(f);
    if (t) t[n > 0 ? n : 0] = 0, *len = (size_t)(n > 0 ? n : 0);
    return t;
}

bool script_compile_text(ScriptFile* f, const char* text, size_t len, bool adopt) {
    double t0 = re_time_seconds();
    SCtx c;
    memset(&c, 0, sizeof c);
    c.lang = f->lang;
    f->ndiags = 0;
    c.diags = f->diags, c.ndiags = &f->ndiags;
    c.file_name = f->name;
    // (UTF-8's mark at the start: skipped)
    if (len >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) text += 3, len -= 3;
    int nt = 0;
    Tok* toks = s_lex(&c, text, len, &nt);
    SClass* cls = toks ? s_parse(&c, toks, nt) : NULL;
    bool parsed = cls != NULL;
    for (uint32_t i = 0; i < f->ndiags; i++) parsed &= !f->diags[i].error;
    bool ok = false;
    BpAsset* a = NULL;
    if (parsed) {
        if (strcmp(cls->name, f->name)) // (the class's name is the file's: what the Content Browser shows)
            s_diag(&c, cls->line, 1, false, "The class is named '%s' after its file, not '%s'", f->name, cls->name);
        api_build();
        bool structs_ok = s_make_structs(&c, cls, f->vpath, f->path); // (before the class: its nodes read them)
        a = structs_ok ? bp_new_class(f->vpath, f->path, f->name, cls->parent) : NULL;
        if (a && s_lower(&c, cls, a)) {
            ok = bp_compile(a);
            if (!ok) { // the Blueprints' compiler's error: back to the script's line (the node's comment "L12:C5")
                int line = 1, col = 1;
                for (uint32_t gi = 0; gi < a->ngraphs; gi++) {
                    BpNode* n = bp_node(&a->graphs[gi], a->error_node);
                    if (n && sscanf(n->comment, "L%d:C%d", &line, &col) == 2) break;
                }
                s_diag(&c, line, col, true, "%s", a->error[0] ? a->error : "The class didn't compile");
            }
        }
    }
    uint32_t nodes = 0;
    for (uint32_t gi = 0; a && gi < a->ngraphs; gi++) nodes += a->graphs[gi].nnodes;
    f->nodes = nodes, f->ops = a ? a->ncode : 0;
    f->ok = ok;
    if (ok && adopt) {
        f->cls = bp_adopt(a);
        a = NULL;
    }
    if (a) bp_free_asset(a);
    free(f->syms);
    f->syms = c.syms, f->nsyms = c.nsyms;
    sa_free(&c.arena);
    f->compile_ms = (re_time_seconds() - t0) * 1000.0;
    return ok;
}

static bool compile_file(ScriptFile* f) {
    size_t len = 0;
    char* text = read_file(f->path, &len);
    if (!text) {
        f->ndiags = 1;
        f->diags[0] = (ScriptDiag){ 1, 1, true, "The file can't be read" };
        f->ok = false;
        return false;
    }
    int64_t mt = 0;
    re_file_stat(f->path, NULL, &mt, NULL);
    f->mtime = mt;
    bool ok = script_compile_text(f, text, len, true);
    free(text);
    if (!ok) {
        for (uint32_t i = 0; i < f->ndiags; i++)
            if (f->diags[i].error) {
                printf("[RealEngine] Script %s(%d:%d): %s\n", f->vpath, f->diags[i].line, f->diags[i].col, f->diags[i].text);
                break;
            }
    }
    return ok;
}

ScriptFile* script_load(const char* vpath) {
    ScriptLang lang = script_lang_of(vpath);
    if (lang == SL_COUNT) return NULL;
    uint32_t h = bp_vpath_hash(vpath);
    for (uint32_t i = 0; i < S.n; i++)
        if (S.f[i]->hash == h) {
            int64_t mt = 0; // (its file changed since: compiled again)
            if (re_file_stat(S.f[i]->path, NULL, &mt, NULL) && mt != S.f[i]->mtime) compile_file(S.f[i]);
            return S.f[i];
        }
    if (S.n >= MAX_SCRIPTS) return NULL;
    ScriptFile* f = calloc(1, sizeof *f); // (kept: the class it made is the registry's)
    if (!f) return NULL;
    snprintf(f->vpath, sizeof f->vpath, "%s", vpath);
    disk_of(vpath, f->path, sizeof f->path);
    f->lang = lang, f->hash = h;
    const char* b = strrchr(vpath, '/');
    snprintf(f->name, sizeof f->name, "%s", b ? b + 1 : vpath);
    char* dot = strrchr(f->name, '.');
    if (dot) *dot = 0;
    S.f[S.n++] = f;
    compile_file(f);
    return f;
}

static void scan_dir(const char* disk, const char* vpath, int depth);
typedef struct ScanCtx { const char* disk; const char* vpath; int depth; } ScanCtx;
static void scan_entry(void* user, const ReDirEntry* e) {
    const ScanCtx* sc = user;
    char d[800], v[800];
    snprintf(d, sizeof d, "%s/%s", sc->disk, e->name);
    snprintf(v, sizeof v, "%s/%s", sc->vpath, e->name);
    if (e->is_dir) scan_dir(d, v, sc->depth + 1);
    else if (script_is_path(e->name)) script_load(v);
}
static void scan_dir(const char* disk, const char* vpath, int depth) {
    if (depth > 12) return;
    ScanCtx sc = { disk, vpath, depth };
    re_dir_walk(disk, scan_entry, &sc);
}

void script_init(const char* content_root) {
    snprintf(S.root, sizeof S.root, "%s", content_root ? content_root : "");
    for (size_t i = 0; S.root[i]; i++)
        if (S.root[i] == '\\') S.root[i] = '/';
    size_t l = strlen(S.root);
    if (l && S.root[l - 1] == '/') S.root[l - 1] = 0;
    for (uint32_t i = 0; i < S.n; i++) free(S.f[i]->syms), free(S.f[i]); // (another project: its classes went with bp_init)
    S.n = 0;
    double t0 = re_time_seconds();
    scan_dir(S.root, "/Content", 0); // (every script compiled now: the levels' actors find their classes)
    S.scanned = true;
    if (S.n) {
        uint32_t ok = 0;
        for (uint32_t i = 0; i < S.n; i++) ok += S.f[i]->ok;
        printf("[RealEngine] Scripts: %u of %u compiled in %.1f ms\n", ok, S.n, (re_time_seconds() - t0) * 1000.0);
    }
}
void script_shutdown(void) {
    for (uint32_t i = 0; i < S.n; i++) free(S.f[i]->syms), free(S.f[i]);
    S.n = 0;
    free(S.api), S.api = NULL, S.napi = 0;
}
void script_rescan(void) {
    if (S.root[0]) scan_dir(S.root, "/Content", 0);
}
ScriptFile* script_find_hash(uint32_t hash) {
    for (uint32_t i = 0; i < S.n; i++)
        if (S.f[i]->hash == hash) return S.f[i];
    return NULL;
}
uint32_t script_count(void) { return S.n; }
ScriptFile* script_at(uint32_t i) { return i < S.n ? S.f[i] : NULL; }

void script_hot_reload(bool playing) {
    double now = re_time_seconds();
    if (playing || now < S.next_check) return;
    S.next_check = now + 0.5;
    for (uint32_t i = 0; i < S.n; i++) {
        int64_t mt = 0;
        if (re_file_stat(S.f[i]->path, NULL, &mt, NULL) && mt != S.f[i]->mtime) {
            compile_file(S.f[i]);
            printf("[RealEngine] Script %s compiled again (%s, %.1f ms)\n", S.f[i]->vpath, S.f[i]->ok ? "ok" : "errors", S.f[i]->compile_ms);
        }
    }
}

bool script_create(const char* folder, const char* name, ScriptLang lang, const char* parent, char* out, size_t cap) {
    if (lang >= SL_COUNT) return false;
    char vp[600], disk[800];
    snprintf(vp, sizeof vp, "%s/%s.%s", folder, name, g_script_lang_ext[lang]);
    disk_of(vp, disk, sizeof disk);
    if (re_file_stat(disk, NULL, NULL, NULL)) return false;
    // the template with the file's name as the class's, the parent asked
    const char* t = script_template(lang);
    char body[4096];
    size_t o = 0;
    for (const char* s = t; *s && o + 64 < sizeof body;) {
        if (!strncmp(s, "NewScript", 9)) {
            o += (size_t)snprintf(body + o, sizeof body - o, "%s", name);
            s += 9;
        } else if (parent && parent[0] && strcmp(parent, "Actor") && !strncmp(s, "Actor", 5) && (s == t || !isalnum((unsigned char)s[-1])) &&
                   !isalnum((unsigned char)s[5])) {
            o += (size_t)snprintf(body + o, sizeof body - o, "%s", parent);
            s += 5;
        } else body[o++] = *s++;
    }
    body[o] = 0;
    FILE* f = re_fopen(disk, "wb");
    if (!f) return false;
    bool ok = fwrite(body, 1, o, f) == o;
    ok = fclose(f) == 0 && ok;
    if (ok) {
        script_load(vp);
        if (out) snprintf(out, cap, "%s", vp);
    }
    return ok;
}
