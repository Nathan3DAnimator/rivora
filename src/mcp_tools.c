// The MCP server's tools (mcp.h, notas-dev/mcp.md), run on the main thread between frames. Made for an AI driving the
// editor: few round trips (every change takes a list: 100 actors in one call), compact answers (rounded numbers, only
// what was asked), errors that say what to do next (the close names, the valid values), a guide at the start
// (get_started, the server's instructions) and the scripts' whole API one call away (run_script, api_reference).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "assistant.h"
#include "blueprint.h"
#include "bp_internal.h"
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <GLFW/glfw3.h>
#include "content.h"
#include "cvars.h"
#include "editor.h"
#include "i18n.h"
#include "material.h"
#include "mcp.h"
#include "mcp_internal.h"
#include "net_http.h"
#include "particles.h"
#include "platform.h"
#include "platform_files.h"
#include "project.h"
#include "props.h"
#include "render.h"
#include "script.h"
#include "script_ide.h"
#include "vehicle.h"
#include "vkctx.h"

// --- helpers ---
char* mcp_jw_take(JsonWriter* w) {
    if (w->fail) return NULL;
    char* s = malloc(w->len + 1);
    if (!s) return NULL;
    memcpy(s, w->buf, w->len);
    s[w->len] = 0;
    return s;
}

void mcp_write_id(JsonWriter* w, const JsonValue* id) {
    if (id && id->type == JSON_NUMBER) jw_float(w, id->num);
    else if (id && id->type == JSON_STRING) jw_str(w, id->str);
    else jw_null(w);
}

static float r1(float v) { return roundf(v * 10.0f) / 10.0f; } // (the answers: a tenth of a cm / degree is enough)
// a scalar for jw_kfloat (a double: rounded in double, so 0.2 isn't written 0.200000003)
static double dq(double v, double q) { return round(v * q) / q; }
static void kvec(JsonWriter* w, const char* key, const float* v) {
    float r[3] = { r1(v[0]), r1(v[1]), r1(v[2]) };
    jw_kfloats(w, key, r, 3);
}
static bool get_vec3(const JsonValue* o, const char* key, float out[3]) {
    const JsonValue* a = json_get(o, key);
    if (!a || a->type != JSON_ARRAY || json_len(a) < 3) return false;
    for (int k = 0; k < 3; k++) out[k] = (float)json_num(json_at(a, (size_t)k), 0.0);
    return true;
}
static bool ieq_loose(const char* a, const char* b) { // (case, spaces, "_" and "-" don't matter: "point light" == "PointLight")
    for (;;) {
        while (*a == ' ' || *a == '_' || *a == '-') a++;
        while (*b == ' ' || *b == '_' || *b == '-') b++;
        if (!*a || !*b) return !*a && !*b;
        char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return false;
        a++, b++;
    }
}
static bool icontains(const char* hay, const char* needle) {
    if (!needle || !*needle) return true;
    size_t n = strlen(needle);
    for (; hay && *hay; hay++)
        if (!_strnicmp(hay, needle, n)) return true;
    return false;
}
static void fail(McpCall* c, const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    jw_free(&c->body);
    jw_init(&c->body);
    jw_obj_begin(&c->body);
    jw_kstr(&c->body, "error", msg);
    jw_obj_end(&c->body);
    c->is_error = true;
}
static const char* actor_name(World* w, Entity e) {
    const CName* n = ecs_get(w, e, COMP_NAME);
    return n ? n->text : "";
}
static const char* actor_class(World* w, Entity e) {
    const CBlueprint* b = ecs_get(w, e, COMP_BLUEPRINT);
    if (b) {
        BpAsset* a = bp_find_hash(b->asset_hash);
        if (a) return a->name;
    }
    return editor_actor_type(w, e);
}
// An actor named by the AI: its id (a number, or "#123"), or its name (exact first, then any case / spacing)
static Entity find_actor(Editor* ed, const JsonValue* v) {
    World* w = &ed->world;
    if (!v) return ENTITY_NONE;
    if (v->type == JSON_NUMBER) return ecs_alive(w, (Entity)v->num) ? (Entity)v->num : ENTITY_NONE;
    if (v->type != JSON_STRING) return ENTITY_NONE;
    if (v->str[0] == '#') {
        Entity e = (Entity)strtoul(v->str + 1, NULL, 10);
        return ecs_alive(w, e) ? e : ENTITY_NONE;
    }
    Entity loose = ENTITY_NONE;
    for (uint32_t i = 1; i < w->slot_count; i++) {
        Entity e = ecs_entity_at(w, i);
        if (!e) continue;
        const char* n = actor_name(w, e);
        if (!strcmp(n, v->str)) return e;
        if (!loose && ieq_loose(n, v->str)) loose = e;
    }
    return loose;
}
static void actor_ref_text(const JsonValue* v, char* out, size_t cap) {
    if (v && v->type == JSON_NUMBER) snprintf(out, cap, "%.0f", v->num);
    else snprintf(out, cap, "%s", v && v->type == JSON_STRING ? v->str : "?");
}
// A short line on an actor (the lists)
static void actor_brief(JsonWriter* jw, World* w, Entity e) {
    jw_obj_begin(jw);
    jw_kint(jw, "id", e);
    jw_kstr(jw, "name", actor_name(w, e));
    jw_kstr(jw, "class", actor_class(w, e));
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (t) {
        kvec(jw, "location", t->location);
        if (t->rotation[0] || t->rotation[1] || t->rotation[2]) kvec(jw, "rotation", t->rotation);
        if (t->scale[0] != 1.0f || t->scale[1] != 1.0f || t->scale[2] != 1.0f) kvec(jw, "scale", t->scale);
        if (t->flags & TRANSFORM_HIDDEN) jw_kbool(jw, "hidden", true);
    }
    jw_obj_end(jw);
}

// --- the guide ---
static const char k_guide[] =
    "# Rivora Engine editor (MCP)\n"
    "A game engine with an editor like Unreal Engine 5 (Actors and Components, Details, Content Browser, Blueprints, Play in Editor).\n\n"
    "## Conventions\n"
    "- Units: centimeters. Z is up, X forward. Rotation = [pitch, yaw, roll] in degrees. A person is ~180 cm; basic shapes are 100 cm.\n"
    "- Actors are named by `id` (number, from the answers) or by name. Asset paths start with `/Content/`.\n"
    "- Properties have the Details panel's names (`Intensity`, `Light Color`, `Attenuation Radius`, `Base Color`, `Roughness`...); "
    "`get_actor` lists every property an actor has with its value. Colors are [r, g, b] 0..1.\n"
    "- Every change is one undo step (`undo`). Nothing is saved until `level` with action save.\n\n"
    "## Fast workflow\n"
    "1. `get_editor_state` and `find_actors` to see the level (cheap; prefer them to screenshots).\n"
    "2. `spawn_actors` / `modify_actors` / `delete_actors` take LISTS: build a whole scene in one call.\n"
    "3. `set_camera` (look_at) then `screenshot` to check what it looks like.\n"
    "4. Logic, loops, procedural placement or anything the tools don't cover: `run_script` (Python by default; the engine's whole API, "
    "found with `api_reference`). Gameplay classes (doors, pickups, enemies): `write_script` a class in /Content, then spawn it with "
    "`spawn_actors` (asset = its path).\n"
    "5. `play` with seconds to test the game, then read what printed; `get_log` for messages and errors.\n"
    "6. `search_docs` / `read_doc` answer how-tos (lighting, materials, AI, animation...).\n\n"
    "## Actor types for spawn_actors (type)\n"
    "Cube, Sphere, Cylinder, Cone, Plane, Empty Actor, Player Start, Directional Light, Point Light, Spot Light, Rect Light, Sky Light, "
    "Exponential Height Fog, Volumetric Cloud, Post Process Volume, Trigger Box, Nav Mesh Bounds Volume, Camera Actor, Landscape, "
    "Ambient Sound, Decal, Fire, Rain, Ocean, Water Body, Sky Preset names... (`list_actor_types` has them all). Or `asset` = a "
    "/Content path of a Static Mesh (.rmesh), Skeletal Mesh (.rskm), Blueprint (.rbp), script class, Particle System, Vehicle.\n\n"
    "## Scripts (run_script, write_script)\n"
    "Four languages compile to the same bytecode: python (.py), real (.real), csharp (.cs), c (.c). In run_script give only the body: "
    "it runs once on the editor's level (not while playing), print() comes back to you.\n"
    "Python example (snake_case names):\n"
    "```\nfor i in range(get_selected_actor_count()):\n    a = get_selected_actor(i)\n    a.add_actor_world_offset(Vector(0, 0, 50))\n    print(a.get_display_name())\n```\n"
    "The API is the Blueprint nodes': `api_reference` with a word (\"spawn\", \"light\", \"material\") gives exact signatures.\n";

const char* mcp_instructions(void) {
    return "You are driving the Rivora Engine editor (a game engine like Unreal Engine 5). Call get_started first: it has the "
           "conventions (cm, Z up, degrees), the fast workflow and the current level. Prefer batch calls (spawn_actors, "
           "modify_actors take lists), find_actors/get_actor over screenshots, run_script for logic or bulk work (Python, the "
           "engine's whole API via api_reference), and search_docs for how-tos. Every change can be undone.";
}

// --- the tools' list ---
typedef struct ToolDef { const char* name; const char* title; const char* desc; const char* schema; bool read_only, destructive; } ToolDef;
#define ACTOR_REF "{\"description\":\"An actor: its id (number) or its name\",\"type\":[\"integer\",\"string\"]}"
#define VEC3(d) "{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3,\"description\":\"" d "\"}"
static const ToolDef k_tools[] = {
    { "get_started", "Get started", "The guide to this editor (conventions, the fastest workflow, actor types, script examples) and the level's current state. Call it first.",
      "{\"type\":\"object\",\"properties\":{}}", true, false },
    { "get_editor_state", "Editor state", "The project, the level, Play, the selection, the camera, the actor count, the frame time, unsaved changes.",
      "{\"type\":\"object\",\"properties\":{}}", true, false },
    { "find_actors", "Find actors", "Lists the level's actors (id, name, class, location) filtered by name, class or component, the selection or a distance. Paged.",
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Part of the name or class (any case)\"},"
      "\"component\":{\"type\":\"string\",\"description\":\"Only actors with this component (e.g. Light, Static Mesh, Blueprint)\"},"
      "\"selected_only\":{\"type\":\"boolean\"},\"near\":" VEC3("A point [x,y,z]: sorted by distance") ",\"radius\":{\"type\":\"number\",\"description\":\"With near: cm\"},"
      "\"limit\":{\"type\":\"integer\",\"default\":50},\"offset\":{\"type\":\"integer\",\"default\":0}}}", true, false },
    { "get_actor", "Get actor", "One actor in full: transform, components and every property it has (the Details panel's names) with values.",
      "{\"type\":\"object\",\"properties\":{\"actor\":" ACTOR_REF ",\"filter\":{\"type\":\"string\",\"description\":\"Only properties whose name contains this\"},"
      "\"properties\":{\"type\":\"boolean\",\"default\":true}},\"required\":[\"actor\"]}", true, false },
    { "spawn_actors", "Spawn actors", "Places one or many actors in one call (one undo step each). Each: type (a class: Cube, Point Light...; see list_actor_types) or asset "
      "(a /Content path), and optional name, location, rotation, scale, properties, material. Returns their ids.",
      "{\"type\":\"object\",\"properties\":{\"actors\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
      "\"type\":{\"type\":\"string\"},\"asset\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"location\":" VEC3("cm") ",\"rotation\":" VEC3("pitch, yaw, roll in degrees") ","
      "\"scale\":" VEC3("1 = 100%") ",\"properties\":{\"type\":\"object\",\"description\":\"Details names to values: numbers, booleans, [r,g,b] / [x,y,z]\"},"
      "\"material\":{\"type\":\"string\",\"description\":\"A /Content/...rmat or .rmi for a mesh\"}}}},"
      "\"select\":{\"type\":\"boolean\",\"default\":true}},\"required\":[\"actors\"]}", false, false },
    { "modify_actors", "Modify actors", "Changes one or many actors in one call: location / rotation / scale (absolute), offset (added to the location), name, hidden, "
      "properties (Details names), material.",
      "{\"type\":\"object\",\"properties\":{\"actors\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"actor\":" ACTOR_REF ","
      "\"name\":{\"type\":\"string\"},\"location\":" VEC3("cm") ",\"offset\":" VEC3("cm added") ",\"rotation\":" VEC3("degrees") ",\"scale\":" VEC3("1 = 100%") ","
      "\"hidden\":{\"type\":\"boolean\"},\"properties\":{\"type\":\"object\"},\"material\":{\"type\":\"string\"}},\"required\":[\"actor\"]}}},\"required\":[\"actors\"]}", false, false },
    { "delete_actors", "Delete actors", "Removes actors (one undo step).",
      "{\"type\":\"object\",\"properties\":{\"actors\":{\"type\":\"array\",\"items\":" ACTOR_REF "}},\"required\":[\"actors\"]}", false, true },
    { "select_actors", "Select actors", "Selects actors in the editor (the user sees them in the Details) and optionally frames them with the camera.",
      "{\"type\":\"object\",\"properties\":{\"actors\":{\"type\":\"array\",\"items\":" ACTOR_REF "},\"focus\":{\"type\":\"boolean\",\"default\":false}},\"required\":[\"actors\"]}", false, false },
    { "set_camera", "Set camera", "Moves the editor's camera: location + rotation, or location + look_at, or focus on an actor (from a distance). Use before screenshot.",
      "{\"type\":\"object\",\"properties\":{\"location\":" VEC3("cm") ",\"rotation\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"description\":\"[pitch, yaw] degrees\"},"
      "\"look_at\":" VEC3("a point to face") ",\"focus_actor\":" ACTOR_REF ",\"distance\":{\"type\":\"number\",\"description\":\"With focus_actor: cm (default: from its size)\"}}}", false, false },
    { "screenshot", "Screenshot", "A picture of the viewport (or the whole editor window) as you would see it. Costs more than find_actors: use it to judge looks.",
      "{\"type\":\"object\",\"properties\":{\"max_width\":{\"type\":\"integer\",\"default\":1024},\"full_window\":{\"type\":\"boolean\",\"default\":false}}}", true, false },
    { "run_script", "Run script", "Runs code once on the editor's level with the engine's whole API (like an Editor Utility; one undo step). Give the body only (or a "
      "whole class with a Run event). print() output and compile errors (with lines) come back. Not while playing.",
      "{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"},\"language\":{\"type\":\"string\",\"enum\":[\"python\",\"real\",\"csharp\",\"c\"],\"default\":\"python\"}},\"required\":[\"code\"]}", false, false },
    { "write_script", "Write script", "Creates or replaces a script class file in /Content (a gameplay class: parent Actor, Character, Pawn...), compiles it and returns the "
      "errors with lines. Spawn it with spawn_actors (asset = its path). The class's name must be the file's.",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"/Content/.../Name.py (.real, .cs, .c)\"},\"code\":{\"type\":\"string\"}},\"required\":[\"path\",\"code\"]}", false, false },
    { "api_reference", "API reference", "Looks up the engine's scripting API (the Blueprint nodes as functions): exact signatures and what they do. Python uses snake_case of the same names.",
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Words of the name, category or description\"},\"limit\":{\"type\":\"integer\",\"default\":20}},\"required\":[\"query\"]}", true, false },
    { "search_docs", "Search docs", "Searches the engine's knowledge base (how-tos, recipes, errors explained, every control's description) in English, Portuguese or Spanish.",
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\",\"default\":6}},\"required\":[\"query\"]}", true, false },
    { "read_doc", "Read doc", "An article of the knowledge base in full (its id from search_docs).",
      "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"language\":{\"type\":\"string\",\"enum\":[\"en\",\"pt\"],\"default\":\"en\"}},\"required\":[\"id\"]}", true, false },
    { "list_assets", "List assets", "The project's assets (paths under /Content) by name and kind (Material, Static Mesh, Blueprint, Texture...).",
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\",\"default\":100}}}", true, false },
    { "list_actor_types", "List actor types", "Every class spawn_actors can place (type), by category.",
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}}}", true, false },
    { "console", "Console", "Reads or sets a console variable (render, quality, physics, audio: r.ScreenPercentage 75, p.GravityZ...) or lists them by prefix.",
      "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"\\\"r.GI.Probes 1\\\" sets, \\\"r.GI.Probes\\\" reads\"},\"list\":{\"type\":\"string\",\"description\":\"A prefix (\\\"r.\\\") to list\"}}}", false, false },
    { "play", "Play", "Play in Editor: start, stop, simulate or status. With seconds: plays that long, stops (the level comes back as it was) and returns what printed.",
      "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"start\",\"stop\",\"simulate\",\"status\"]},\"seconds\":{\"type\":\"number\"}},\"required\":[\"action\"]}", false, false },
    { "level", "Level", "Saves the level (to its file or a new /Content/...rlevel path) or opens another one.",
      "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"save\",\"open\"]},\"path\":{\"type\":\"string\"}},\"required\":[\"action\"]}", false, false },
    { "undo", "Undo", "Undoes (or redoes) the last changes.",
      "{\"type\":\"object\",\"properties\":{\"redo\":{\"type\":\"boolean\",\"default\":false},\"steps\":{\"type\":\"integer\",\"default\":1}}}", false, false },
    { "get_log", "Get log", "The editor's recent messages and the scripts' / Blueprints' prints.",
      "{\"type\":\"object\",\"properties\":{\"lines\":{\"type\":\"integer\",\"default\":40}}}", true, false },
};
#define NTOOLS (int)(sizeof k_tools / sizeof k_tools[0])

const char* mcp_tools_list_json(void) {
    static char* json;
    if (json) return json;
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_key(&w, "tools");
    jw_arr_begin(&w);
    for (int i = 0; i < NTOOLS; i++) {
        jw_obj_begin(&w);
        jw_kstr(&w, "name", k_tools[i].name);
        jw_kstr(&w, "title", k_tools[i].title);
        jw_kstr(&w, "description", k_tools[i].desc);
        jw_key(&w, "inputSchema");
        jw_raw(&w, k_tools[i].schema);
        jw_key(&w, "annotations");
        jw_obj_begin(&w);
        jw_kstr(&w, "title", k_tools[i].title);
        jw_kbool(&w, "readOnlyHint", k_tools[i].read_only);
        if (!k_tools[i].read_only) jw_kbool(&w, "destructiveHint", k_tools[i].destructive);
        jw_kbool(&w, "openWorldHint", false);
        jw_obj_end(&w);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    json = mcp_jw_take(&w);
    jw_free(&w);
    return json ? json : "{\"tools\":[]}";
}

// --- prompts: workflows the user can pick in the client ---
typedef struct PromptDef { const char* name; const char* title; const char* desc; const char* arg; const char* arg_desc; const char* text; } PromptDef;
static const PromptDef k_prompts[] = {
    { "build_scene", "Build a scene", "Builds a level from a description.", "description", "What the scene should be",
      "Build this scene in the Rivora editor: %s\n\nSteps: get_started; plan the layout in cm (Z up); place the ground, the big shapes, "
      "the lighting (a Directional Light, a Sky Light, an Exponential Height Fog or a Sky Preset) and the details with as few "
      "spawn_actors calls as possible (lists); use run_script for repeated or procedural placement; set_camera + screenshot to "
      "check; fix what looks wrong; save the level at the end." },
    { "gameplay_feature", "Add a gameplay feature", "Implements a mechanic with a script class.", "feature", "The mechanic (a door that opens, a pickup, an enemy...)",
      "Implement this gameplay feature in the Rivora editor: %s\n\nSteps: search_docs for the engine's ready-made components "
      "first (Interactable, Health, Inventory, Pickup, Quest...); look up the functions with api_reference; write_script the "
      "class (Python, parent Actor or Character), fix the compile errors it returns; spawn_actors it; play with seconds to "
      "test and read the prints; iterate; save the level." },
    { "optimize_performance", "Optimize performance", "Finds what costs the most and fixes it.", NULL, NULL,
      "Make this level run faster in the Rivora editor. Steps: get_editor_state (frame time); search_docs \"performance\"; "
      "find_actors for heavy things (lights with shadows, particles, high-poly meshes); console r.* variables for quality; "
      "change one thing at a time and compare the frame time; report what helped." },
    { "fix_problem", "Fix a problem", "Diagnoses something that is wrong.", "problem", "What is wrong",
      "Something is wrong in the Rivora editor: %s\n\nSteps: get_log for errors; search_docs with the message; find_actors / "
      "get_actor on what is involved; screenshot to see it; fix it with the smallest change; confirm." },
};
#define NPROMPTS (int)(sizeof k_prompts / sizeof k_prompts[0])

const char* mcp_prompts_list_json(void) {
    static char* json;
    if (json) return json;
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_key(&w, "prompts");
    jw_arr_begin(&w);
    for (int i = 0; i < NPROMPTS; i++) {
        jw_obj_begin(&w);
        jw_kstr(&w, "name", k_prompts[i].name);
        jw_kstr(&w, "title", k_prompts[i].title);
        jw_kstr(&w, "description", k_prompts[i].desc);
        jw_key(&w, "arguments");
        jw_arr_begin(&w);
        if (k_prompts[i].arg) {
            jw_obj_begin(&w);
            jw_kstr(&w, "name", k_prompts[i].arg);
            jw_kstr(&w, "description", k_prompts[i].arg_desc);
            jw_kbool(&w, "required", true);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    json = mcp_jw_take(&w);
    jw_free(&w);
    return json ? json : "{\"prompts\":[]}";
}

char* mcp_prompt_get(const JsonValue* params) {
    const char* name = json_get_str(params, "name", "");
    for (int i = 0; i < NPROMPTS; i++) {
        if (strcmp(name, k_prompts[i].name)) continue;
        const char* arg = k_prompts[i].arg ? json_get_str(json_get(params, "arguments"), k_prompts[i].arg, "") : "";
        size_t n = strlen(k_prompts[i].text) + strlen(arg) + 16;
        char* text = malloc(n);
        if (!text) return NULL;
        snprintf(text, n, k_prompts[i].text, arg);
        JsonWriter w;
        jw_init(&w);
        jw_obj_begin(&w);
        jw_kstr(&w, "description", k_prompts[i].desc);
        jw_key(&w, "messages");
        jw_arr_begin(&w);
        jw_obj_begin(&w);
        jw_kstr(&w, "role", "user");
        jw_key(&w, "content");
        jw_obj_begin(&w);
        jw_kstr(&w, "type", "text");
        jw_kstr(&w, "text", text);
        jw_obj_end(&w);
        jw_obj_end(&w);
        jw_arr_end(&w);
        jw_obj_end(&w);
        free(text);
        char* out = mcp_jw_take(&w);
        jw_free(&w);
        return out;
    }
    return NULL;
}

const char* mcp_resources_list_json(void) {
    return "{\"resources\":[{\"uri\":\"rivora://guide\",\"name\":\"guide\",\"title\":\"Rivora Engine: guide for AI\",\"description\":\"Conventions, the fastest "
           "workflow, actor types, script examples\",\"mimeType\":\"text/markdown\"}]}";
}
const char* mcp_resource_templates_json(void) {
    return "{\"resourceTemplates\":[{\"uriTemplate\":\"rivora://docs/{id}\",\"name\":\"doc\",\"title\":\"Knowledge base article\",\"description\":\"An article "
           "of the engine's knowledge base (ids from search_docs)\",\"mimeType\":\"text/markdown\"}]}";
}

// --- the answer ---
void mcp_compose_answer(McpCall* c) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kstr(&w, "jsonrpc", "2.0");
    jw_key(&w, "id");
    mcp_write_id(&w, c->id);
    if (!strcmp(c->method, "resources/read")) {
        if (!c->res_text) {
            jw_key(&w, "error");
            jw_obj_begin(&w);
            jw_kint(&w, "code", -32002);
            jw_kstr(&w, "message", "Resource not found");
            jw_obj_end(&w);
        } else {
            jw_key(&w, "result");
            jw_obj_begin(&w);
            jw_key(&w, "contents");
            jw_arr_begin(&w);
            jw_obj_begin(&w);
            jw_kstr(&w, "uri", c->name);
            jw_kstr(&w, "mimeType", "text/markdown");
            jw_kstr(&w, "text", c->res_text);
            jw_obj_end(&w);
            jw_arr_end(&w);
            jw_obj_end(&w);
        }
    } else {
        char* text = mcp_jw_take(&c->body);
        jw_key(&w, "result");
        jw_obj_begin(&w);
        jw_key(&w, "content");
        jw_arr_begin(&w);
        jw_obj_begin(&w);
        jw_kstr(&w, "type", "text");
        jw_kstr(&w, "text", text && text[0] ? text : "{}");
        jw_obj_end(&w);
        if (c->image_b64) {
            jw_obj_begin(&w);
            jw_kstr(&w, "type", "image");
            jw_kstr(&w, "data", c->image_b64);
            jw_kstr(&w, "mimeType", "image/png");
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_kbool(&w, "isError", c->is_error);
        jw_obj_end(&w);
        free(text);
    }
    jw_obj_end(&w);
    c->answer = mcp_jw_take(&w);
    jw_free(&w);
}

// --- the state ---
static void write_state(JsonWriter* jw, Editor* ed) {
    World* w = &ed->world;
    jw_kstr(jw, "project", project()->name);
    jw_kstr(jw, "content_root", "/Content");
    const char* lp = ed->level_path;
    const char* cr = content_root();
    size_t cn = cr ? strlen(cr) : 0;
    char lv[1100];
    if (lp[0] && cn && !_strnicmp(lp, cr, cn)) {
        snprintf(lv, sizeof lv, "/Content%s", lp + cn);
        for (char* p = lv; *p; p++)
            if (*p == '\\') *p = '/';
    } else snprintf(lv, sizeof lv, "%s", lp[0] ? lp : "(unsaved new level)");
    jw_kstr(jw, "level", lv);
    jw_kbool(jw, "unsaved_changes", ed->dirty);
    jw_kstr(jw, "play", ed->playing ? (ed->simulating ? "simulating" : ed->paused ? "paused" : "playing") : "stopped");
    jw_kint(jw, "actors", w->alive);
    jw_key(jw, "selection");
    jw_arr_begin(jw);
    for (uint32_t i = 0; i < ed->nsel && i < 20; i++)
        if (ecs_alive(w, ed->sel[i])) actor_brief(jw, w, ed->sel[i]);
    jw_arr_end(jw);
    if (ed->nsel > 20) jw_kint(jw, "selection_more", ed->nsel - 20);
    jw_key(jw, "camera");
    jw_obj_begin(jw);
    kvec(jw, "location", ed->cam.pos);
    float rot[2] = { r1(ed->cam.pitch), r1(ed->cam.yaw) };
    jw_kfloats(jw, "rotation", rot, 2);
    jw_kfloat(jw, "fov", dq(ed->cam.fov_deg, 10.0));
    jw_obj_end(jw);
    jw_kfloat(jw, "frame_ms", dq(ed->vk ? ed->vk->gpu_ms : 0.0, 10.0));
    jw_kstr(jw, "editor_language", i18n_language_code(i18n_language()));
}

// the classes of the Add menus (editor.c's catalog), "Light" counted in the class names too
static int find_type(const char* name) {
    int n = editor_place_count();
    for (int i = 0; i < n; i++)
        if (ieq_loose(editor_place_name(i, NULL), name)) return i;
    static const struct { const char* alias; const char* name; } al[] = { { "Box", "Cube" }, { "Light", "Point Light" }, { "Sun", "Directional Light" },
                                                                         { "Camera", "Camera Actor" }, { "Fog", "Exponential Height Fog" }, { "Empty", "Empty Actor" },
                                                                         { "Actor", "Empty Actor" }, { "Trigger", "Trigger Box" }, { "Sound", "Ambient Sound" } };
    for (size_t k = 0; k < sizeof al / sizeof al[0]; k++)
        if (ieq_loose(al[k].alias, name))
            for (int i = 0; i < n; i++)
                if (ieq_loose(editor_place_name(i, NULL), al[k].name)) return i;
    return -1;
}
static void close_types(const char* name, char* out, size_t cap) {
    out[0] = 0;
    int n = editor_place_count(), got = 0;
    char key[8] = "";
    snprintf(key, sizeof key, "%.4s", name);
    for (int i = 0; i < n && got < 8; i++) {
        const char* t = editor_place_name(i, NULL);
        if (icontains(t, key) || icontains(name, t)) {
            size_t l = strlen(out);
            snprintf(out + l, cap - l, "%s%s", got ? ", " : "", t), got++;
        }
    }
    if (!got) snprintf(out, cap, "Cube, Sphere, Point Light, Directional Light, Player Start, Camera Actor (list_actor_types has them all)");
}

// An asset placed (what the viewport's drop does with each kind)
static Entity spawn_asset(Editor* ed, const char* vpath, const float at[3], char* err, size_t ecap) {
    World* w = &ed->world;
    AssetKind k = content_kind_of(vpath);
    Entity e = ENTITY_NONE;
    if (k == ASSET_STATIC_MESH || k == ASSET_SKELETAL_MESH) {
        bool skel = k == ASSET_SKELETAL_MESH;
        e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(skel ? COMP_SKELETAL_MESH : COMP_STATIC_MESH));
        if (!e) return ENTITY_NONE;
        const char* base = strrchr(vpath, '/');
        char name[RE_NAME_MAX];
        snprintf(name, sizeof name, "%s", base ? base + 1 : vpath);
        char* dot = strrchr(name, '.');
        if (dot) *dot = 0;
        char uname[RE_NAME_MAX];
        editor_unique_name(ed, name, uname, sizeof uname);
        snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", uname);
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        memcpy(t->location, at, 12);
        t->flags = MOBILITY_MOVABLE;
        if (skel) {
            SkelMesh* m = anim_mesh_load(vpath);
            if (!m) snprintf(err, ecap, "%s couldn't be read", vpath);
            ((CSkeletalMesh*)ecs_get(w, e, COMP_SKELETAL_MESH))->mesh = m ? m->hash : 0;
        } else {
            CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
            sm->mesh = render_mesh_id_of(vpath);
            sm->base_color[0] = sm->base_color[1] = sm->base_color[2] = 0.7f, sm->roughness = 0.5f, sm->cast_shadow = 1;
            sm->material = content_mesh_material(vpath);
        }
    } else if (k == ASSET_BLUEPRINT || k == ASSET_SCRIPT) {
        ScriptFile* sf = k == ASSET_SCRIPT ? script_load(vpath) : NULL;
        BpAsset* cls = k == ASSET_SCRIPT ? (sf ? sf->cls : NULL) : bp_load(vpath);
        if (!cls) {
            snprintf(err, ecap, "%s: %s", vpath, sf && sf->ndiags ? sf->diags[0].text : "not a class that compiles");
            return ENTITY_NONE;
        }
        e = bp_spawn(w, cls, at, (float[3]){ 0, 0, 0 });
        if (e) {
            char name[RE_NAME_MAX];
            editor_unique_name(ed, cls->name, name, sizeof name);
            CName* nm = ecs_get(w, e, COMP_NAME);
            if (nm) snprintf(nm->text, sizeof nm->text, "%s", name);
        }
    } else if (k == ASSET_PARTICLE_SYSTEM) {
        ParticleSystemAsset* a = ps_load(vpath);
        e = a ? particles_spawn(w, a->hash, at, NULL, false) : ENTITY_NONE;
        if (e) {
            char uname[RE_NAME_MAX];
            editor_unique_name(ed, a->name, uname, sizeof uname);
            snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", uname);
        }
    } else if (k == ASSET_VEHICLE) {
        VehicleAsset* a = veh_load(vpath);
        if (a) {
            float loc[3] = { at[0], at[1], at[2] + veh_rest_height(a) };
            char uname[RE_NAME_MAX];
            editor_unique_name(ed, a->name, uname, sizeof uname);
            e = vehicle_spawn(w, a->hash, loc, ed->cam.yaw, uname);
        }
    } else {
        snprintf(err, ecap, "%s is a %s: only Static Mesh, Skeletal Mesh, Blueprint, script, Particle System and Vehicle assets can be placed", vpath,
                 asset_kind_name(k));
        return ENTITY_NONE;
    }
    if (!e && !err[0]) snprintf(err, ecap, "%s couldn't be placed (missing or unreadable?)", vpath);
    return e;
}

static bool set_material(Editor* ed, Entity e, const char* vpath, char* err, size_t ecap) {
    CStaticMesh* sm = ecs_get(&ed->world, e, COMP_STATIC_MESH);
    if (!sm) return snprintf(err, ecap, "material: %s has no Static Mesh", actor_name(&ed->world, e)), false;
    if (!vpath[0] || !_stricmp(vpath, "none")) return sm->material = 0, true;
    if (_strnicmp(vpath, "/Content/", 9)) return snprintf(err, ecap, "material: '%s' isn't a /Content/ path", vpath), false;
    char path[1200];
    snprintf(path, sizeof path, "%s%s", content_root(), vpath + 8);
    Material* m = mat_load(path);
    if (!m) return snprintf(err, ecap, "material: %s couldn't be read", vpath), false;
    sm->material = m->hash;
    return true;
}

// properties: { "Intensity": 5000, "Light Color": [1, 0.8, 0.6], "Cast Shadows": false }
static void set_props(Editor* ed, Entity e, const JsonValue* props, JsonWriter* errs, int* nerr) {
    for (uint32_t i = 0; props && props->type == JSON_OBJECT && i < props->count; i++) {
        const JsonMember* m = &props->members[i];
        float v = 0.0f, vec[3] = { 0, 0, 0 };
        const JsonValue* val = &m->value;
        if (val->type == JSON_ARRAY) {
            for (int k = 0; k < 3 && k < (int)json_len(val); k++) vec[k] = (float)json_num(json_at(val, (size_t)k), 0.0);
            v = vec[0];
        } else if (val->type == JSON_TRUE || val->type == JSON_FALSE) v = val->type == JSON_TRUE ? 1.0f : 0.0f;
        else v = (float)json_num(val, 0.0), vec[0] = vec[1] = vec[2] = v;
        if (val->type == JSON_STRING) {
            jw_str(errs, "a text value isn't supported by properties (use material for a material, run_script for the rest)");
            (*nerr)++;
            continue;
        }
        if (!prop_set(&ed->world, e, m->key, v, vec)) {
            char msg[600], near_[300] = "";
            int got = 0;
            uint64_t mask = ecs_mask(&ed->world, e);
            for (int p = 0; p < prop_count() && got < 6; p++) {
                const PropInfo* pi = prop_at(p);
                if (!(mask & COMP_BIT(pi->comp))) continue;
                char k4[6];
                snprintf(k4, sizeof k4, "%.4s", m->key);
                if (icontains(pi->label, k4)) {
                    size_t l = strlen(near_);
                    snprintf(near_ + l, sizeof near_ - l, "%s%s", got ? ", " : "", pi->label), got++;
                }
            }
            snprintf(msg, sizeof msg, "%s: no property '%s'%s%s (get_actor lists them)", actor_name(&ed->world, e), m->key, got ? "; close: " : "", near_);
            jw_str(errs, msg);
            (*nerr)++;
        }
    }
}

// --- the tools ---
static void t_get_started(McpCall* c, Editor* ed) {
    JsonWriter* jw = &c->body;
    jw_obj_begin(jw);
    jw_kstr(jw, "guide", k_guide);
    jw_key(jw, "state");
    jw_obj_begin(jw);
    write_state(jw, ed);
    jw_obj_end(jw);
    // the level at a glance: how many of each class
    World* w = &ed->world;
    const char* cls[32];
    int cnt[32], nc = 0;
    for (uint32_t i = 1; i < w->slot_count; i++) {
        Entity e = ecs_entity_at(w, i);
        if (!e) continue;
        const char* k = actor_class(w, e);
        int j = 0;
        while (j < nc && strcmp(cls[j], k)) j++;
        if (j == nc && nc < 32) cls[nc] = k, cnt[nc++] = 0;
        if (j < 32) cnt[j]++;
    }
    jw_key(jw, "actor_classes");
    jw_obj_begin(jw);
    for (int j = 0; j < nc; j++) jw_kint(jw, cls[j], cnt[j]);
    jw_obj_end(jw);
    jw_obj_end(jw);
}

static void t_state(McpCall* c, Editor* ed) {
    jw_obj_begin(&c->body);
    write_state(&c->body, ed);
    jw_obj_end(&c->body);
}

static void t_find(McpCall* c, Editor* ed) {
    World* w = &ed->world;
    const char* q = json_get_str(c->args, "query", "");
    const char* comp = json_get_str(c->args, "component", "");
    bool sel_only = json_get_bool(c->args, "selected_only", false);
    int limit = json_get_int(c->args, "limit", 50), offset = json_get_int(c->args, "offset", 0);
    if (limit < 1) limit = 1;
    if (limit > 1000) limit = 1000;
    float nearp[3], radius = (float)json_get_num(c->args, "radius", 0.0);
    bool has_near = get_vec3(c->args, "near", nearp);
    int comp_id = -1;
    if (comp[0]) {
        for (int k = 0; k < COMP_COUNT && comp_id < 0; k++)
            if (ieq_loose(g_components[k].name, comp)) comp_id = k;
        for (int k = 0; k < COMP_COUNT && comp_id < 0; k++)
            if (icontains(g_components[k].name, comp)) comp_id = k;
        if (comp_id < 0) {
            fail(c, "No component '%s'. Some: Static Mesh, Light, Skeletal Mesh, Blueprint, Camera, Collision, Audio, Particle System", comp);
            return;
        }
    }
    // matches (their distance when sorted by it)
    uint32_t cap = w->alive + 1, n = 0;
    Entity* hits = malloc(sizeof(Entity) * cap);
    float* dist = malloc(sizeof(float) * cap);
    if (!hits || !dist) {
        free(hits), free(dist);
        fail(c, "out of memory");
        return;
    }
    for (uint32_t i = 1; i < w->slot_count && n < cap; i++) {
        Entity e = ecs_entity_at(w, i);
        if (!e) continue;
        const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        if (sel_only && !editor_is_selected(ed, e)) continue;
        if (comp_id >= 0 && !(ecs_mask(w, e) & COMP_BIT(comp_id))) continue;
        if (q[0] && !icontains(actor_name(w, e), q) && !icontains(actor_class(w, e), q)) continue;
        float d = 0.0f;
        if (has_near && t) {
            float dx = t->location[0] - nearp[0], dy = t->location[1] - nearp[1], dz = t->location[2] - nearp[2];
            d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (radius > 0.0f && d > radius) continue;
        }
        hits[n] = e, dist[n++] = d;
    }
    if (has_near) // (by distance: an insertion sort is fine for the few thousand of a level)
        for (uint32_t i = 1; i < n; i++)
            for (uint32_t j = i; j > 0 && dist[j] < dist[j - 1]; j--) {
                float td = dist[j];
                dist[j] = dist[j - 1], dist[j - 1] = td;
                Entity te = hits[j];
                hits[j] = hits[j - 1], hits[j - 1] = te;
            }
    JsonWriter* jw = &c->body;
    jw_obj_begin(jw);
    jw_kint(jw, "total", n);
    jw_key(jw, "actors");
    jw_arr_begin(jw);
    for (uint32_t i = (uint32_t)(offset > 0 ? offset : 0); i < n && (int)i < offset + limit; i++) actor_brief(jw, w, hits[i]);
    jw_arr_end(jw);
    if ((uint32_t)(offset + limit) < n) jw_kint(jw, "next_offset", offset + limit);
    jw_obj_end(jw);
    free(hits), free(dist);
}

static void t_get_actor(McpCall* c, Editor* ed) {
    World* w = &ed->world;
    const JsonValue* ref = json_get(c->args, "actor");
    Entity e = find_actor(ed, ref);
    if (!e) {
        char r[160];
        actor_ref_text(ref, r, sizeof r);
        fail(c, "No actor '%s' (find_actors lists them)", r);
        return;
    }
    const char* filter = json_get_str(c->args, "filter", "");
    JsonWriter* jw = &c->body;
    jw_obj_begin(jw);
    jw_kint(jw, "id", e);
    jw_kstr(jw, "name", actor_name(w, e));
    jw_kstr(jw, "class", actor_class(w, e));
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (t) {
        kvec(jw, "location", t->location);
        kvec(jw, "rotation", t->rotation);
        kvec(jw, "scale", t->scale);
        jw_kbool(jw, "hidden", (t->flags & TRANSFORM_HIDDEN) != 0);
    }
    uint64_t mask = ecs_mask(w, e);
    jw_key(jw, "components");
    jw_arr_begin(jw);
    for (int k = 0; k < COMP_COUNT; k++)
        if ((mask & COMP_BIT(k)) && k != COMP_NAME && k != COMP_TRANSFORM) jw_str(jw, g_components[k].name);
    jw_arr_end(jw);
    const CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    if (sm && sm->material) {
        const Material* m = mat_find_hash(sm->material);
        if (m) jw_kstr(jw, "material", m->path);
    }
    if (json_get_bool(c->args, "properties", true)) {
        jw_key(jw, "properties");
        jw_obj_begin(jw);
        for (int p = 0; p < prop_count(); p++) {
            const PropInfo* pi = prop_at(p);
            if (!(mask & COMP_BIT(pi->comp)) || pi->kind == PK_STR) continue;
            char key[160];
            snprintf(key, sizeof key, "%s%s%s", pi->section && pi->section[0] ? pi->section : "", pi->section && pi->section[0] ? "." : "", pi->label);
            if (filter[0] && !icontains(key, filter)) continue;
            float v[4] = { 0, 0, 0, 0 };
            if (!prop_get(w, e, key, v) && !prop_get(w, e, pi->label, v)) continue;
            if (pi->count >= 3) {
                float r[3] = { roundf(v[0] * 1000.0f) / 1000.0f, roundf(v[1] * 1000.0f) / 1000.0f, roundf(v[2] * 1000.0f) / 1000.0f };
                jw_kfloats(jw, key, r, 3);
            } else if (pi->kind == PK_BOOL || pi->kind == PK_FLAG || pi->kind == PK_BIT) jw_kbool(jw, key, v[0] != 0.0f);
            else jw_kfloat(jw, key, dq(v[0], 1000.0));
        }
        jw_obj_end(jw);
    }
    jw_obj_end(jw);
}

static void t_spawn(McpCall* c, Editor* ed, double now) {
    const JsonValue* list = json_get(c->args, "actors");
    if (!list || list->type != JSON_ARRAY || !json_len(list)) {
        fail(c, "actors: a list of {type or asset, location...}");
        return;
    }
    if (ed->playing) {
        fail(c, "Stop playing first (play action stop): actors placed while playing vanish when it stops");
        return;
    }
    World* w = &ed->world;
    JsonWriter* jw = &c->body, errs;
    jw_init(&errs);
    jw_arr_begin(&errs);
    int nerr = 0;
    Entity made[1024];
    uint32_t nmade = 0;
    bool any_preset = false;
    jw_obj_begin(jw);
    jw_key(jw, "spawned");
    jw_arr_begin(jw);
    for (size_t i = 0; i < json_len(list); i++) {
        const JsonValue* a = json_at(list, i);
        const char* type = json_get_str(a, "type", "");
        const char* asset = json_get_str(a, "asset", "");
        float loc[3], rot[3], scl[3];
        bool has_loc = get_vec3(a, "location", loc);
        char err[400] = "";
        Entity e = ENTITY_NONE;
        if (asset[0]) {
            if (!has_loc) // (in front of the camera, 5 m)
                for (int k = 0; k < 3; k++) loc[k] = ed->cam.pos[k];
            if (!has_loc) {
                float cy = cosf(ed->cam.yaw * 0.017453293f), sy = sinf(ed->cam.yaw * 0.017453293f);
                loc[0] += cy * 500.0f, loc[1] += sy * 500.0f, loc[2] = 0.0f;
            }
            editor_undo_push(ed);
            e = spawn_asset(ed, asset, loc, err, sizeof err);
        } else if (type[0]) {
            int ti = find_type(type);
            if (ti < 0) {
                char near_[400];
                close_types(type, near_, sizeof near_);
                snprintf(err, sizeof err, "actors[%zu]: no type '%s'; close: %s", i, type, near_);
            } else {
                e = editor_place_spawn(ed, ti, now);
                if (!e && editor_place_is_preset(ti)) { // (a Sky Preset: the sun, the sky light, the fog and the clouds set)
                    jw_obj_begin(jw);
                    jw_kstr(jw, "preset", editor_place_name(ti, NULL));
                    jw_kbool(jw, "applied", true);
                    jw_obj_end(jw);
                    any_preset = true;
                    continue;
                }
                if (!e) snprintf(err, sizeof err, "actors[%zu]: '%s' couldn't be placed here", i, type);
            }
        } else snprintf(err, sizeof err, "actors[%zu]: give a type (Cube, Point Light...) or an asset (/Content/...)", i);
        if (!e) {
            jw_str(&errs, err), nerr++;
            continue;
        }
        if (err[0]) jw_str(&errs, err), nerr++;
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        if (t) {
            if (has_loc) memcpy(t->location, loc, 12);
            if (get_vec3(a, "rotation", rot)) memcpy(t->rotation, rot, 12);
            if (get_vec3(a, "scale", scl)) memcpy(t->scale, scl, 12);
        }
        const char* nm = json_get_str(a, "name", "");
        if (nm[0]) {
            char un[RE_NAME_MAX];
            editor_unique_name(ed, nm, un, sizeof un);
            CName* cn = ecs_get(w, e, COMP_NAME);
            if (cn) snprintf(cn->text, sizeof cn->text, "%s", un);
        }
        set_props(ed, e, json_get(a, "properties"), &errs, &nerr);
        const char* mat = json_get_str(a, "material", "");
        char merr[300] = "";
        if (mat[0] && !set_material(ed, e, mat, merr, sizeof merr)) jw_str(&errs, merr), nerr++;
        ecs_touch(w, e);
        if (nmade < 1024) made[nmade++] = e;
        jw_obj_begin(jw);
        jw_kint(jw, "id", e);
        jw_kstr(jw, "name", actor_name(w, e));
        jw_kstr(jw, "class", actor_class(w, e));
        if (t) kvec(jw, "location", t->location);
        jw_obj_end(jw);
    }
    jw_arr_end(jw);
    if (json_get_bool(c->args, "select", true) && nmade) {
        editor_select_none(ed);
        for (uint32_t i = 0; i < nmade; i++) editor_select(ed, made[i], SEL_ADD);
    }
    ed->dirty = true;
    jw_arr_end(&errs);
    if (nerr) {
        char* et = mcp_jw_take(&errs);
        jw_key(jw, "errors");
        jw_raw(jw, et ? et : "[]");
        free(et);
    }
    jw_free(&errs);
    jw_obj_end(jw);
    if (!nmade && !any_preset) c->is_error = true;
}

static void t_modify(McpCall* c, Editor* ed) {
    const JsonValue* list = json_get(c->args, "actors");
    if (!list || list->type != JSON_ARRAY || !json_len(list)) {
        fail(c, "actors: a list of {actor, location / rotation / scale / properties...}");
        return;
    }
    World* w = &ed->world;
    if (!ed->playing) editor_undo_push(ed);
    JsonWriter* jw = &c->body, errs;
    jw_init(&errs);
    jw_arr_begin(&errs);
    int nerr = 0, nok = 0;
    for (size_t i = 0; i < json_len(list); i++) {
        const JsonValue* a = json_at(list, i);
        Entity e = find_actor(ed, json_get(a, "actor"));
        if (!e) {
            char r[160], m[260];
            actor_ref_text(json_get(a, "actor"), r, sizeof r);
            snprintf(m, sizeof m, "actors[%zu]: no actor '%s'", i, r);
            jw_str(&errs, m), nerr++;
            continue;
        }
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        float v[3];
        if (t) {
            if (get_vec3(a, "location", v)) memcpy(t->location, v, 12);
            if (get_vec3(a, "offset", v))
                for (int k = 0; k < 3; k++) t->location[k] += v[k];
            if (get_vec3(a, "rotation", v)) memcpy(t->rotation, v, 12);
            if (get_vec3(a, "scale", v)) memcpy(t->scale, v, 12);
            const JsonValue* h = json_get(a, "hidden");
            if (h && (h->type == JSON_TRUE || h->type == JSON_FALSE)) t->flags = h->type == JSON_TRUE ? t->flags | TRANSFORM_HIDDEN : t->flags & ~TRANSFORM_HIDDEN;
        }
        const char* nm = json_get_str(a, "name", "");
        if (nm[0] && strcmp(nm, actor_name(w, e))) {
            char un[RE_NAME_MAX];
            editor_unique_name(ed, nm, un, sizeof un);
            CName* cn = ecs_get(w, e, COMP_NAME);
            if (cn) snprintf(cn->text, sizeof cn->text, "%s", un);
        }
        set_props(ed, e, json_get(a, "properties"), &errs, &nerr);
        const char* mat = json_get_str(a, "material", "");
        char merr[300] = "";
        if (mat[0] && !set_material(ed, e, mat, merr, sizeof merr)) jw_str(&errs, merr), nerr++;
        ecs_touch(w, e);
        nok++;
    }
    ed->dirty = true;
    jw_arr_end(&errs);
    jw_obj_begin(jw);
    jw_kint(jw, "modified", nok);
    if (nerr) {
        char* et = mcp_jw_take(&errs);
        jw_key(jw, "errors");
        jw_raw(jw, et ? et : "[]");
        free(et);
    }
    jw_obj_end(jw);
    jw_free(&errs);
    if (!nok) c->is_error = true;
}

static void t_delete(McpCall* c, Editor* ed) {
    const JsonValue* list = json_get(c->args, "actors");
    World* w = &ed->world;
    if (!list || list->type != JSON_ARRAY) {
        fail(c, "actors: a list of ids or names");
        return;
    }
    if (!ed->playing) editor_undo_push(ed);
    int n = 0, missing = 0;
    for (size_t i = 0; i < json_len(list); i++) {
        Entity e = find_actor(ed, json_at(list, i));
        if (!e) {
            missing++;
            continue;
        }
        ecs_destroy(w, e), n++;
    }
    editor_sync_selection(ed);
    ed->dirty = true;
    jw_obj_begin(&c->body);
    jw_kint(&c->body, "deleted", n);
    if (missing) jw_kint(&c->body, "not_found", missing);
    jw_obj_end(&c->body);
}

// The camera looks from 'from' to 'to'
static void look(Editor* ed, const float from[3], const float to[3]) {
    float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
    float h = sqrtf(d[0] * d[0] + d[1] * d[1]);
    memcpy(ed->cam.pos, from, 12);
    ed->cam.yaw = atan2f(d[1], d[0]) * 57.29578f;
    ed->cam.pitch = atan2f(d[2], h > 1e-4f ? h : 1e-4f) * 57.29578f;
}
// An actor's size (its scale times 100 cm: the basic shapes'; at least 2 m)
static float actor_size(World* w, Entity e) {
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    float s = t ? fmaxf(fmaxf(fabsf(t->scale[0]), fabsf(t->scale[1])), fabsf(t->scale[2])) * 100.0f : 100.0f;
    return fmaxf(s, 200.0f);
}
static void frame_actors(Editor* ed, const Entity* es, uint32_t n, float dist) {
    World* w = &ed->world;
    float c[3] = { 0, 0, 0 }, ext = 0.0f;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        const CTransform* t = ecs_get(w, es[i], COMP_TRANSFORM);
        if (!t) continue;
        for (int j = 0; j < 3; j++) c[j] += t->location[j];
        k++;
    }
    if (!k) return;
    for (int j = 0; j < 3; j++) c[j] /= (float)k;
    for (uint32_t i = 0; i < n; i++) {
        const CTransform* t = ecs_get(w, es[i], COMP_TRANSFORM);
        if (!t) continue;
        float dx = t->location[0] - c[0], dy = t->location[1] - c[1], dz = t->location[2] - c[2];
        ext = fmaxf(ext, sqrtf(dx * dx + dy * dy + dz * dz) + actor_size(w, es[i]) * 0.5f);
    }
    if (dist <= 0.0f) dist = fmaxf(ext, 100.0f) * 2.2f;
    // from the camera's side, a little above
    float yaw = ed->cam.yaw * 0.017453293f;
    float from[3] = { c[0] - cosf(yaw) * dist * 0.9f, c[1] - sinf(yaw) * dist * 0.9f, c[2] + dist * 0.45f };
    look(ed, from, c);
}

static void t_select(McpCall* c, Editor* ed) {
    const JsonValue* list = json_get(c->args, "actors");
    Entity es[512];
    uint32_t n = 0;
    editor_select_none(ed);
    for (size_t i = 0; list && i < json_len(list) && n < 512; i++) {
        Entity e = find_actor(ed, json_at(list, i));
        if (e) editor_select(ed, e, SEL_ADD), es[n++] = e;
    }
    if (json_get_bool(c->args, "focus", false)) frame_actors(ed, es, n, 0.0f);
    jw_obj_begin(&c->body);
    jw_kint(&c->body, "selected", n);
    jw_obj_end(&c->body);
}

static void t_camera(McpCall* c, Editor* ed) {
    float loc[3], at[3];
    const JsonValue* fa = json_get(c->args, "focus_actor");
    if (fa) {
        Entity e = find_actor(ed, fa);
        if (!e) {
            char r[160];
            actor_ref_text(fa, r, sizeof r);
            fail(c, "No actor '%s'", r);
            return;
        }
        frame_actors(ed, &e, 1, (float)json_get_num(c->args, "distance", 0.0));
    } else {
        bool has_loc = get_vec3(c->args, "location", loc);
        if (!has_loc) memcpy(loc, ed->cam.pos, 12);
        if (get_vec3(c->args, "look_at", at)) look(ed, loc, at);
        else {
            memcpy(ed->cam.pos, loc, 12);
            const JsonValue* r = json_get(c->args, "rotation");
            if (r && json_len(r) >= 2) ed->cam.pitch = (float)json_num(json_at(r, 0), 0), ed->cam.yaw = (float)json_num(json_at(r, 1), 0);
        }
    }
    if (ed->cam.pitch > 89.0f) ed->cam.pitch = 89.0f;
    if (ed->cam.pitch < -89.0f) ed->cam.pitch = -89.0f;
    jw_obj_begin(&c->body);
    kvec(&c->body, "location", ed->cam.pos);
    float rot[2] = { r1(ed->cam.pitch), r1(ed->cam.yaw) };
    jw_kfloats(&c->body, "rotation", rot, 2);
    jw_obj_end(&c->body);
}

// screenshot: stage 0 asks the frame's pixels (a few frames later: the camera's move drawn), 1 waits for them, scales, encodes
static bool t_screenshot(McpCall* c, Editor* ed) {
    VkCtx* vk = ed->vk;
    if (!vk) {
        fail(c, "No renderer");
        return true;
    }
    if (c->stage == 0) { // (2 frames for a camera just moved: its view and the TAA settle)
        if (++c->frames < 3) return false;
        float r[4];
        if (json_get_bool(c->args, "full_window", false)) {
            int fw = 0, fh = 0;
            glfwGetFramebufferSize(ed->window, &fw, &fh);
            r[0] = 0, r[1] = 0, r[2] = (float)fw, r[3] = (float)fh;
        } else {
            memcpy(r, ed->vp_rect, sizeof r);
            const ImGuiViewport* mv = igGetMainViewport();
            if (mv) r[0] -= mv->Pos.x, r[1] -= mv->Pos.y;
        }
        vk->grab_ready = false;
        vkctx_request_grab(vk, r);
        c->stage = 1, c->frames = 0;
        return false;
    }
    if (!vk->grab_ready) {
        if (++c->frames > 120) {
            fail(c, "The picture didn't come (the window minimized?)");
            return true;
        }
        return false;
    }
    vk->grab_ready = false;
    uint32_t sw = vk->grab_w, sh = vk->grab_h;
    int maxw = json_get_int(c->args, "max_width", 1024);
    if (maxw < 64) maxw = 64;
    if (maxw > 2560) maxw = 2560;
    uint32_t dw = sw > (uint32_t)maxw ? (uint32_t)maxw : sw, dh = sw ? (uint32_t)((uint64_t)sh * dw / sw) : 0;
    uint8_t* px = dw && dh ? malloc((size_t)dw * dh * 3) : NULL;
    if (!px) {
        fail(c, "The picture couldn't be made");
        return true;
    }
    // a box filter down to dw x dh
    for (uint32_t y = 0; y < dh; y++)
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t x0 = x * sw / dw, x1 = (x + 1) * sw / dw, y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            uint32_t acc[3] = { 0, 0, 0 }, n = 0;
            for (uint32_t yy = y0; yy < y1 && yy < sh; yy++)
                for (uint32_t xx = x0; xx < x1 && xx < sw; xx++) {
                    const uint8_t* s = &vk->grab_px[((size_t)yy * sw + xx) * 3];
                    acc[0] += s[0], acc[1] += s[1], acc[2] += s[2], n++;
                }
            for (int k = 0; k < 3; k++) px[((size_t)y * dw + x) * 3 + k] = (uint8_t)(n ? acc[k] / n : 0);
        }
    // PNG through a file in the temp folder (the engine's writer), read back and put in base64
    wchar_t tw[MAX_PATH];
    char tmp[MAX_PATH * 3];
    GetTempPathW(MAX_PATH, tw);
    WideCharToMultiByte(CP_UTF8, 0, tw, -1, tmp, (int)sizeof tmp, NULL, NULL);
    size_t tl = strlen(tmp);
    snprintf(tmp + tl, sizeof tmp - tl, "rivora_mcp_%lu.png", (unsigned long)GetCurrentProcessId());
    bool ok = vkctx_png_write_rgb(tmp, px, dw, dh);
    free(px);
    size_t n = 0;
    uint8_t* png = NULL;
    FILE* fp = ok ? re_fopen(tmp, "rb") : NULL;
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        png = sz > 0 ? malloc((size_t)sz) : NULL;
        if (png) n = fread(png, 1, (size_t)sz, fp);
        fclose(fp);
    }
    if (!png) {
        fail(c, "The picture couldn't be written");
        return true;
    }
    c->image_b64 = malloc(4 * ((n + 2) / 3) + 1);
    if (c->image_b64) http_base64(png, n, c->image_b64);
    free(png);
    jw_obj_begin(&c->body);
    jw_kint(&c->body, "width", dw);
    jw_kint(&c->body, "height", dh);
    jw_kstr(&c->body, "of", json_get_bool(c->args, "full_window", false) ? "the editor window" : "the viewport");
    jw_obj_end(&c->body);
    return true;
}

// --- scripts ---
static int lang_of(const char* s) {
    if (!s || !s[0] || !_stricmp(s, "python") || !_stricmp(s, "py")) return SL_PYTHON;
    if (!_stricmp(s, "real")) return SL_REAL;
    if (!_stricmp(s, "csharp") || !_stricmp(s, "c#") || !_stricmp(s, "cs")) return SL_CSHARP;
    if (!_stricmp(s, "c")) return SL_C;
    return -1;
}
// The prints of a run (the VM's messages: bp_set_message_sink)
static JsonWriter* g_prints;
static int g_nprints;
static void print_sink(const char* text, void* user) {
    (void)user;
    if (g_prints && g_nprints < 400) jw_str(g_prints, text), g_nprints++;
}

// The code as a class: a whole class (its Run event), or a body wrapped in one (header lines before it: out_offset)
static char* wrap_code(int lang, const char* code, int* line_offset) {
    bool whole = (lang == SL_C) ? strstr(code, "void Run") != NULL : strstr(code, "class ") != NULL;
    size_t n = strlen(code), cap = n * 2 + 512;
    char* out = malloc(cap);
    if (!out) return NULL;
    if (whole) {
        *line_offset = 0;
        // the class's name has to be MCP_Run (the file's)
        snprintf(out, cap, "%s", code);
        return out;
    }
    const char* head = lang == SL_PYTHON ? "from realengine import *\n\nclass MCP_Run(EditorUtility):\n    def Run(self):\n"
                       : lang == SL_REAL ? "class MCP_Run extends EditorUtility\n    event Run()\n"
                       : lang == SL_CSHARP ? "using RealEngine;\n\npublic class MCP_Run : EditorUtility\n{\n    void Run()\n    {\n"
                                           : "// parent: EditorUtility\n#include \"realengine.h\"\n\nvoid Run(void)\n{\n";
    const int indent = 8;
    *line_offset = 0;
    for (const char* p = head; *p; p++) *line_offset += *p == '\n';
    size_t o = (size_t)snprintf(out, cap, "%s", head);
    bool any = false;
    const char* p = code;
    while (*p) {
        const char* e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l && p[l - 1] == '\r') l--;
        if (o + l + indent + 16 >= cap) break;
        bool blank = true;
        for (size_t k = 0; k < l; k++) blank &= p[k] == ' ' || p[k] == '\t';
        if (!blank) {
            memset(out + o, ' ', (size_t)indent), o += (size_t)indent;
            memcpy(out + o, p, l), o += l;
            any = true;
        }
        out[o++] = '\n';
        p = e ? e + 1 : p + l;
    }
    if (!any && (lang == SL_PYTHON || lang == SL_REAL)) memcpy(out + o, "        pass\n", 13), o += 13;
    const char* tail = lang == SL_CSHARP ? "    }\n}\n" : lang == SL_C ? "}\n" : "";
    o += (size_t)snprintf(out + o, cap - o, "%s", tail);
    out[o] = 0;
    return out;
}

static void write_diags(JsonWriter* jw, const ScriptFile* f, int line_offset) {
    jw_key(jw, "errors");
    jw_arr_begin(jw);
    for (uint32_t i = 0; i < f->ndiags; i++) {
        jw_obj_begin(jw);
        jw_kint(jw, "line", f->diags[i].line - line_offset > 0 ? f->diags[i].line - line_offset : f->diags[i].line);
        bool body = line_offset > 0 && f->diags[i].line > line_offset; // (a wrapped body: its 8 spaces of indentation off too)
        jw_kint(jw, "column", body && f->diags[i].col > 8 ? f->diags[i].col - 8 : f->diags[i].col);
        jw_kstr(jw, "message", f->diags[i].text);
        if (!f->diags[i].error) jw_kbool(jw, "warning", true);
        int doc = f->diags[i].error ? assistant_explain_error(f->diags[i].text) : -1;
        const AsDoc* d = doc >= 0 ? assistant_doc(doc) : NULL;
        if (d && d->src == AS_SRC_ARTICLE) jw_kstr(jw, "help_doc", d->id);
        jw_obj_end(jw);
    }
    jw_arr_end(jw);
}

static void t_run_script(McpCall* c, Editor* ed) {
    const char* code = json_get_str(c->args, "code", "");
    int lang = lang_of(json_get_str(c->args, "language", "python"));
    if (lang < 0) {
        fail(c, "language: python, real, csharp or c");
        return;
    }
    if (!code[0]) {
        fail(c, "code is empty");
        return;
    }
    if (ed->playing) {
        fail(c, "Scripts run on the editor's level: stop playing first (play action stop)");
        return;
    }
    int off = 0;
    char* text = wrap_code(lang, code, &off);
    if (!text) {
        fail(c, "out of memory");
        return;
    }
    static ScriptFile f; // (one class, MCP_Run, replaced each run; never written to the disk)
    free(f.syms);
    memset(&f, 0, sizeof f);
    f.lang = (ScriptLang)lang;
    snprintf(f.name, sizeof f.name, "MCP_Run");
    snprintf(f.vpath, sizeof f.vpath, "/Content/__MCP__/MCP_Run.%s", g_script_lang_ext[lang]);
    snprintf(f.path, sizeof f.path, "%s\\__MCP__\\MCP_Run.%s", content_root(), g_script_lang_ext[lang]);
    f.hash = bp_vpath_hash(f.vpath);
    bool ok = script_compile_text(&f, text, strlen(text), true);
    free(text);
    JsonWriter* jw = &c->body;
    jw_obj_begin(jw);
    if (!ok || !f.cls) {
        jw_kbool(jw, "ran", false);
        write_diags(jw, &f, off);
        jw_obj_end(jw);
        c->is_error = true;
        return;
    }
    // the Run event (any case)
    const char* ev = NULL;
    for (uint32_t k = 0; k < f.cls->nentry && !ev; k++)
        if (f.cls->entries_code[k].kind == BPK_EVENT_CUSTOM && !_stricmp(f.cls->entries_code[k].name, "Run")) ev = f.cls->entries_code[k].name;
    if (!ev) {
        jw_kbool(jw, "ran", false);
        jw_kstr(jw, "error", "The class has no Run event (a method without parameters named Run)");
        jw_obj_end(jw);
        c->is_error = true;
        return;
    }
    editor_undo_push(ed);
    editor_utilities_bind(ed);
    JsonWriter prints;
    jw_init(&prints);
    jw_arr_begin(&prints);
    g_prints = &prints, g_nprints = 0;
    bp_set_message_sink(print_sink, NULL);
    uint32_t before = ed->world.alive;
    double t0 = re_time_seconds();
    bool ran = bp_run_editor_event(&ed->world, f.cls, ev);
    double ms = (re_time_seconds() - t0) * 1000.0;
    bp_set_message_sink(NULL, NULL);
    g_prints = NULL;
    jw_arr_end(&prints);
    ed->dirty = true;
    ecs_touch_all(&ed->world);
    jw_kbool(jw, "ran", ran);
    jw_kfloat(jw, "ms", dq(ms, 100.0));
    char* pt = mcp_jw_take(&prints);
    jw_key(jw, "output");
    jw_raw(jw, pt ? pt : "[]");
    free(pt);
    jw_free(&prints);
    int delta = (int)ed->world.alive - (int)before;
    if (delta) jw_kint(jw, "actors_added", delta);
    if (f.ndiags) write_diags(jw, &f, off);
    jw_obj_end(jw);
    if (!ran) c->is_error = true;
}

static void t_write_script(McpCall* c, Editor* ed) {
    (void)ed;
    const char* vpath = json_get_str(c->args, "path", "");
    const char* code = json_get_str(c->args, "code", "");
    if (_strnicmp(vpath, "/Content/", 9) || !script_is_path(vpath) || strstr(vpath, "..")) {
        fail(c, "path: /Content/<folder>/<Name>.py (or .real, .cs, .c)");
        return;
    }
    char disk[1200];
    snprintf(disk, sizeof disk, "%s%s", content_root(), vpath + 8);
    for (char* p = disk; *p; p++)
        if (*p == '/') *p = '\\';
    char dir[1200];
    snprintf(dir, sizeof dir, "%s", disk);
    char* sl = strrchr(dir, '\\');
    if (sl) *sl = 0, re_make_dirs(dir);
    FILE* fp = re_fopen(disk, "wb");
    if (!fp) {
        fail(c, "Couldn't write %s", vpath);
        return;
    }
    fwrite(code, 1, strlen(code), fp);
    fclose(fp);
    content_mark_dirty();
    content_created(vpath);
    ScriptFile* f = script_load(vpath);
    JsonWriter* jw = &c->body;
    jw_obj_begin(jw);
    jw_kstr(jw, "path", vpath);
    jw_kbool(jw, "compiled", f && f->ok);
    if (f) {
        jw_kstr(jw, "class", f->name);
        if (f->cls) jw_kstr(jw, "parent", f->cls->parent);
        write_diags(jw, f, 0);
    }
    jw_obj_end(jw);
    if (!f || !f->ok) c->is_error = true;
}

static void t_api(McpCall* c) {
    const char* q = json_get_str(c->args, "query", "");
    int limit = json_get_int(c->args, "limit", 20);
    if (limit < 1) limit = 1;
    if (limit > 100) limit = 100;
    // every word of the query in the name, the category or the description (the name's matches first)
    char words[8][48];
    int nw = 0;
    for (const char* p = q; *p && nw < 8;) {
        while (*p == ' ') p++;
        size_t l = strcspn(p, " ");
        if (l) snprintf(words[nw++], sizeof words[0], "%.*s", (int)(l < 47 ? l : 47), p);
        p += l;
    }
    JsonWriter* jw = &c->body;
    jw_obj_begin(jw);
    jw_key(jw, "functions");
    jw_arr_begin(jw);
    int got = 0;
    for (int pass = 0; pass < 2 && got < limit; pass++)
        for (uint32_t i = 0; i < script_api_count() && got < limit; i++) {
            const ScriptApi* a = script_api_at(i);
            bool in_name = true, all = true;
            for (int k = 0; k < nw; k++) {
                bool n = icontains(a->name, words[k]);
                in_name &= n;
                all &= n || icontains(a->category, words[k]) || icontains(a->doc, words[k]);
            }
            if (pass == 0 ? !in_name : (in_name || !all)) continue;
            jw_obj_begin(jw);
            jw_kstr(jw, "signature", a->sig);
            if (a->category) jw_kstr(jw, "category", a->category);
            if (a->doc) jw_kstr(jw, "doc", a->doc);
            if (a->pure) jw_kbool(jw, "pure", true);
            jw_obj_end(jw);
            got++;
        }
    jw_arr_end(jw);
    if (!got) jw_kstr(jw, "hint", "Nothing matched: try one simpler word (spawn, location, light, material, sound, timer, widget)");
    jw_kstr(jw, "note", "Python uses snake_case of these names (SetActorLocation -> set_actor_location); a function on an actor is called on it (a.set_actor_location(...)) or on self.");
    jw_obj_end(jw);
}

static void t_search_docs(McpCall* c) {
    const char* q = json_get_str(c->args, "query", "");
    int limit = json_get_int(c->args, "limit", 6);
    if (limit < 1) limit = 1;
    if (limit > 20) limit = 20;
    AsHit hits[20];
    bool weak = false;
    int n = assistant_search(q, hits, limit, &weak);
    JsonWriter* jw = &c->body;
    jw_obj_begin(jw);
    jw_key(jw, "results");
    jw_arr_begin(jw);
    for (int i = 0; i < n; i++) {
        const AsDoc* d = assistant_doc(hits[i].doc);
        if (!d) continue;
        jw_obj_begin(jw);
        jw_kstr(jw, "kind", d->src == AS_SRC_ARTICLE ? "article" : d->src == AS_SRC_API ? "function" : "control");
        jw_kstr(jw, "id", d->id);
        jw_kstr(jw, "title", d->title[0] ? d->title[0] : d->id);
        const char* b = d->body[0] ? d->body[0] : "";
        char snip[400];
        snprintf(snip, sizeof snip, "%s", b);
        if (strlen(b) >= sizeof snip - 1) memcpy(snip + sizeof snip - 4, "...", 4);
        jw_kstr(jw, d->src == AS_SRC_ARTICLE ? "start" : "description", snip);
        if (d->src == AS_SRC_API && d->extra) jw_kstr(jw, "signature", d->extra);
        jw_obj_end(jw);
    }
    jw_arr_end(jw);
    if (weak || !n) jw_kstr(jw, "hint", assistant_state() == AS_FETCHING ? "The knowledge base is still downloading: ask again in a few seconds" : "Weak matches: try other words or Portuguese");
    jw_obj_end(jw);
}

static const char* doc_text(const char* id, int lang) {
    int i = assistant_find_article(id);
    const AsDoc* d = i >= 0 ? assistant_doc(i) : NULL;
    if (!d) return NULL;
    return d->body[lang] && d->body[lang][0] ? d->body[lang] : d->body[0];
}

static void t_read_doc(McpCall* c) {
    const char* id = json_get_str(c->args, "id", "");
    int lang = !_stricmp(json_get_str(c->args, "language", "en"), "pt") ? 1 : 0;
    const char* t = doc_text(id, lang);
    if (!t) {
        fail(c, "No article '%s' (ids come from search_docs results of kind article)", id);
        return;
    }
    jw_obj_begin(&c->body);
    jw_kstr(&c->body, "id", id);
    jw_kstr(&c->body, "text", t);
    jw_obj_end(&c->body);
}

static void t_list_assets(McpCall* c) {
    const char* q = json_get_str(c->args, "query", "");
    const char* kind = json_get_str(c->args, "kind", "");
    int limit = json_get_int(c->args, "limit", 100);
    if (limit < 1) limit = 1;
    if (limit > 2000) limit = 2000;
    static char found[4096][512];
    int n = content_search(q, found, 4096);
    JsonWriter* jw = &c->body;
    jw_obj_begin(jw);
    jw_key(jw, "assets");
    jw_arr_begin(jw);
    int got = 0, total = 0;
    for (int i = 0; i < n; i++) {
        AssetKind k = content_kind_of(found[i]);
        if (k == ASSET_FOLDER) continue;
        const char* kn = asset_kind_name(k);
        if (kind[0] && !icontains(kn, kind)) continue;
        if (strstr(found[i], "/__MCP__/")) continue;
        total++;
        if (got >= limit) continue;
        jw_obj_begin(jw);
        jw_kstr(jw, "path", found[i]);
        jw_kstr(jw, "kind", kn);
        jw_obj_end(jw);
        got++;
    }
    jw_arr_end(jw);
    jw_kint(jw, "total", total);
    jw_obj_end(jw);
}

static void t_types(McpCall* c) {
    const char* q = json_get_str(c->args, "query", "");
    JsonWriter* jw = &c->body;
    jw_obj_begin(jw);
    int n = editor_place_count();
    const char* last = NULL;
    for (int i = 0; i < n; i++) {
        const char* cat = NULL;
        const char* t = editor_place_name(i, &cat);
        if (q[0] && !icontains(t, q) && !icontains(cat, q)) continue;
        if (!last || strcmp(last, cat)) {
            if (last) jw_arr_end(jw);
            jw_key(jw, cat);
            jw_arr_begin(jw);
            last = cat;
        }
        jw_str(jw, t);
    }
    if (last) jw_arr_end(jw);
    jw_obj_end(jw);
}

static void t_console(McpCall* c) {
    const char* cmd = json_get_str(c->args, "command", "");
    const char* list = json_get_str(c->args, "list", NULL);
    JsonWriter* jw = &c->body;
    jw_obj_begin(jw);
    if (list) {
        jw_key(jw, "variables");
        jw_obj_begin(jw);
        for (int i = 0; i < cvar_count(); i++) {
            const Cvar* v = cvar_at(i);
            if (_strnicmp(v->name, list, strlen(list))) continue;
            char val[64] = "";
            float f = 0.0f;
            cvar_get(v->name, &f, val, sizeof val);
            char line[400];
            snprintf(line, sizeof line, "%s  (%s)", val, v->help ? v->help : "");
            jw_kstr(jw, v->name, line);
        }
        jw_obj_end(jw);
    }
    if (cmd[0]) {
        char out[256] = "";
        bool ok = cvar_exec(cmd, out, sizeof out);
        jw_kbool(jw, "ok", ok);
        if (out[0]) jw_kstr(jw, "result", out);
        if (!ok) jw_kstr(jw, "hint", "Unknown variable: list with {\"list\": \"r.\"} (r. render, p. physics, au. audio, t. time, sg. quality)"), c->is_error = true;
    }
    jw_obj_end(jw);
}

static bool t_play(McpCall* c, Editor* ed, double now) {
    const char* act = json_get_str(c->args, "action", "status");
    double secs = json_get_num(c->args, "seconds", 0.0);
    if (c->stage == 0) {
        if (!strcmp(act, "start") || !strcmp(act, "simulate")) {
            if (!ed->playing) {
                if (!strcmp(act, "simulate")) editor_simulate(ed, now);
                else editor_play(ed, now);
            }
            if (secs > 0.0) {
                c->stage = 1, c->t_until = now + (secs > 600.0 ? 600.0 : secs);
                g_prints = &c->body; // (the prints while it plays, collected below)
                jw_obj_begin(&c->body);
                jw_key(&c->body, "output");
                jw_arr_begin(&c->body);
                g_nprints = 0;
                bp_set_message_sink(print_sink, NULL);
                return false;
            }
        } else if (!strcmp(act, "stop")) {
            if (ed->playing) editor_stop(ed, now);
        } else if (strcmp(act, "status")) {
            fail(c, "action: start, stop, simulate or status");
            return true;
        }
        jw_obj_begin(&c->body);
        jw_kstr(&c->body, "play", ed->playing ? (ed->simulating ? "simulating" : "playing") : "stopped");
        jw_obj_end(&c->body);
        return true;
    }
    // playing for N seconds
    if (ed->playing && now < c->t_until) return false;
    bp_set_message_sink(NULL, NULL);
    g_prints = NULL;
    jw_arr_end(&c->body);
    jw_kfloat(&c->body, "played_seconds", dq(now - (c->t_until - secs), 10.0));
    jw_kfloat(&c->body, "frame_ms", dq(ed->vk ? ed->vk->gpu_ms : 0.0, 10.0));
    if (ed->playing) editor_stop(ed, now);
    else jw_kstr(&c->body, "note", "The play stopped by itself (Esc, a Quit node or an error)");
    jw_kstr(&c->body, "play", "stopped (the level is back as before playing)");
    jw_obj_end(&c->body);
    return true;
}

static void t_level(McpCall* c, Editor* ed, double now) {
    const char* act = json_get_str(c->args, "action", "");
    const char* vp = json_get_str(c->args, "path", "");
    char disk[1200] = "";
    if (vp[0]) {
        if (_strnicmp(vp, "/Content/", 9) || strstr(vp, "..")) {
            fail(c, "path: /Content/.../Name.rlevel");
            return;
        }
        snprintf(disk, sizeof disk, "%s%s", content_root(), vp + 8);
        for (char* p = disk; *p; p++)
            if (*p == '/') *p = '\\';
    }
    if (ed->playing) {
        fail(c, "Stop playing first");
        return;
    }
    bool ok;
    if (!strcmp(act, "save")) {
        if (!disk[0] && !ed->level_path[0]) {
            fail(c, "The level is new: give a path (/Content/Maps/Name.rlevel)");
            return;
        }
        if (disk[0]) {
            char dir[1200];
            snprintf(dir, sizeof dir, "%s", disk);
            char* sl = strrchr(dir, '\\');
            if (sl) *sl = 0, re_make_dirs(dir);
        }
        ok = editor_save_level(ed, disk[0] ? disk : ed->level_path, now);
    } else if (!strcmp(act, "open")) {
        if (!disk[0]) {
            fail(c, "path: the level to open (list_assets kind Level)");
            return;
        }
        ok = editor_open_level(ed, disk, now);
    } else {
        fail(c, "action: save or open");
        return;
    }
    jw_obj_begin(&c->body);
    jw_kbool(&c->body, "ok", ok);
    write_state(&c->body, ed);
    jw_obj_end(&c->body);
    if (!ok) c->is_error = true;
}

static void t_undo(McpCall* c, Editor* ed, double now) {
    int steps = json_get_int(c->args, "steps", 1);
    bool redo = json_get_bool(c->args, "redo", false);
    if (ed->playing) {
        fail(c, "Stop playing first");
        return;
    }
    for (int i = 0; i < steps && i < 100; i++) redo ? editor_redo(ed, now) : editor_undo(ed, now);
    jw_obj_begin(&c->body);
    jw_kint(&c->body, redo ? "redone" : "undone", steps);
    jw_kint(&c->body, "actors", ed->world.alive);
    jw_obj_end(&c->body);
}

static void t_log(McpCall* c, Editor* ed) {
    int lines = json_get_int(c->args, "lines", 40);
    if (lines < 1) lines = 1;
    JsonWriter* jw = &c->body;
    jw_obj_begin(jw);
    jw_key(jw, "editor");
    jw_arr_begin(jw);
    for (int i = ed->log_count > lines ? ed->log_count - lines : 0; i < ed->log_count; i++) jw_str(jw, ed->log[i]);
    jw_arr_end(jw);
    const char* msg[16];
    float ages[16];
    uint32_t n = bp_messages(msg, ages, 16);
    jw_key(jw, "prints");
    jw_arr_begin(jw);
    for (uint32_t i = n; i-- > 0;) jw_str(jw, msg[i]);
    jw_arr_end(jw);
    jw_obj_end(jw);
}

static void read_resource(McpCall* c) {
    const char* uri = c->name;
    if (!strcmp(uri, "rivora://guide")) c->res_text = _strdup(k_guide);
    else if (!strncmp(uri, "rivora://docs/", 14)) {
        const char* t = doc_text(uri + 14, 0);
        if (t) c->res_text = _strdup(t);
    }
}

bool mcp_tools_run(McpCall* c, Editor* ed, double now) {
    if (!strcmp(c->method, "resources/read")) {
        read_resource(c);
        return true;
    }
    const char* n = c->name;
    if (c->stage == 0 && c->frames == 0 && strcmp(n, "screenshot") && strcmp(n, "play")) jw_free(&c->body), jw_init(&c->body);
    if (!strcmp(n, "get_started")) t_get_started(c, ed);
    else if (!strcmp(n, "get_editor_state")) t_state(c, ed);
    else if (!strcmp(n, "find_actors")) t_find(c, ed);
    else if (!strcmp(n, "get_actor")) t_get_actor(c, ed);
    else if (!strcmp(n, "spawn_actors")) t_spawn(c, ed, now);
    else if (!strcmp(n, "modify_actors")) t_modify(c, ed);
    else if (!strcmp(n, "delete_actors")) t_delete(c, ed);
    else if (!strcmp(n, "select_actors")) t_select(c, ed);
    else if (!strcmp(n, "set_camera")) t_camera(c, ed);
    else if (!strcmp(n, "screenshot")) return t_screenshot(c, ed);
    else if (!strcmp(n, "run_script")) t_run_script(c, ed);
    else if (!strcmp(n, "write_script")) t_write_script(c, ed);
    else if (!strcmp(n, "api_reference")) t_api(c);
    else if (!strcmp(n, "search_docs")) t_search_docs(c);
    else if (!strcmp(n, "read_doc")) t_read_doc(c);
    else if (!strcmp(n, "list_assets")) t_list_assets(c);
    else if (!strcmp(n, "list_actor_types")) t_types(c);
    else if (!strcmp(n, "console")) t_console(c);
    else if (!strcmp(n, "play")) return t_play(c, ed, now);
    else if (!strcmp(n, "level")) t_level(c, ed, now);
    else if (!strcmp(n, "undo")) t_undo(c, ed, now);
    else if (!strcmp(n, "get_log")) t_log(c, ed);
    else fail(c, "Unknown tool '%s' (tools/list has them)", n);
    return true;
}
