// (2026-11-24, item 6 of the user's list: "uma API que guie e acelere a IA no controle") Included by mcp_tools.c (its
// helpers are static there). What it adds to the 22 tools (notas-dev/mcp.md "Guia"):
//   * the arguments checked against each tool's schema before anything runs: an unknown key answers the right one
//     (position -> location), a wrong type or value says what it takes, and tool_help has examples;
//   * batch: several calls as ONE undo step (a transaction): atomic rolls back on a failed step, dry_run rolls back always
//     (the world as it was: editor_undo_revert; the pushes inside held by ed->undo_hold);
//   * recipe: whole tasks in one call (test_level, lighting, playable_character, pbr_material, hud), each a batch (dry_run
//     answers the plan) plus the files it makes (a class, a material);
//   * inspect_level: what is missing or wrong, each with its fix, and the next steps;
//   * tool_help: the schema, examples that work as they are, the common errors, the related tools;
//   * search: types, components, recipes, the level's actors, assets, API functions and docs in one call;
//   * "next" in every answer: the likely next call;
//   * resources rivora://recipes and rivora://state, the prompt make_playable.

// --- the checks ---
static const ToolDef* tool_def(const char* name) {
    for (int i = 0; i < NTOOLS; i++)
        if (!strcmp(k_tools[i].name, name)) return &k_tools[i];
    return NULL;
}

static const JsonValue* tool_schema(const char* name) { // (parsed once, the main thread's)
    static JsonDoc docs[64];
    static bool parsed[64];
    for (int i = 0; i < NTOOLS && i < 64; i++) {
        if (strcmp(k_tools[i].name, name)) continue;
        if (!parsed[i]) {
            char err[120];
            parsed[i] = true;
            if (!json_parse(k_tools[i].schema, strlen(k_tools[i].schema), &docs[i], err, sizeof err)) memset(&docs[i], 0, sizeof docs[i]);
        }
        return docs[i].root.type == JSON_OBJECT ? &docs[i].root : NULL;
    }
    return NULL;
}

static const char* json_type_name(int t) {
    switch (t) {
    case JSON_NULL: return "null";
    case JSON_TRUE:
    case JSON_FALSE: return "a boolean";
    case JSON_NUMBER: return "a number";
    case JSON_STRING: return "a string";
    case JSON_ARRAY: return "a list";
    default: return "an object";
    }
}
static bool json_is(const JsonValue* v, const char* type) {
    if (!type) return true;
    if (!strcmp(type, "array")) return v->type == JSON_ARRAY;
    if (!strcmp(type, "object")) return v->type == JSON_OBJECT;
    if (!strcmp(type, "string")) return v->type == JSON_STRING;
    if (!strcmp(type, "number") || !strcmp(type, "integer")) return v->type == JSON_NUMBER;
    if (!strcmp(type, "boolean")) return v->type == JSON_TRUE || v->type == JSON_FALSE;
    return true;
}

// the key an AI meant (the usual other names, then a key that starts the same)
static const char* meant_key(const char* key, const JsonValue* props) {
    static const struct { const char* wrong; const char* right; } syn[] = {
        { "position", "location" }, { "pos", "location" }, { "loc", "location" }, { "translation", "location" }, { "origin", "location" },
        { "rot", "rotation" }, { "orientation", "rotation" }, { "rotator", "rotation" }, { "size", "scale" }, { "scale3d", "scale" },
        { "class", "type" }, { "actor_type", "type" }, { "kind", "type" }, { "mesh", "asset" }, { "blueprint", "asset" },
        { "props", "properties" }, { "property", "properties" }, { "id", "actor" }, { "target", "actor" }, { "actor_id", "actor" },
        { "script", "code" }, { "source", "code" }, { "lang", "language" }, { "text", "query" }, { "q", "query" }, { "search", "query" },
        { "commands", "steps" }, { "actions", "steps" }, { "operations", "steps" }, { "calls", "steps" }, { "name", "tool" },
        { "arguments", "args" }, { "parameters", "params" }, { "options", "params" }, { "preview", "dry_run" }, { "dryrun", "dry_run" },
        { "simulate", "dry_run" }, { "component", "components" }, { "add_components", "components" }, { "mat", "material" },
        { "target_location", "look_at" }, { "lookat", "look_at" }, { "focus", "focus_actor" }, { "duration", "seconds" },
    };
    for (size_t i = 0; i < sizeof syn / sizeof syn[0]; i++)
        if (!_stricmp(key, syn[i].wrong) && json_get(props, syn[i].right)) return syn[i].right;
    for (uint32_t i = 0; props && props->type == JSON_OBJECT && i < props->count; i++) {
        const char* k = props->members[i].key;
        if (ieq_loose(k, key) || (strlen(key) >= 3 && !_strnicmp(k, key, 3))) return k;
    }
    return NULL;
}

static void keys_of(const JsonValue* props, char* out, size_t cap) {
    out[0] = 0;
    for (uint32_t i = 0; props && props->type == JSON_OBJECT && i < props->count; i++) {
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "%s%s", i ? ", " : "", props->members[i].key);
    }
}

// obj against schema (its properties, their types and enums, required; one level into lists of objects)
static bool check_obj(const JsonValue* obj, const JsonValue* schema, const char* where, char* err, size_t cap) {
    const JsonValue* props = json_get(schema, "properties");
    char pre[96];
    snprintf(pre, sizeof pre, "%s%s", where, where[0] ? ": " : "");
    if (!obj || obj->type != JSON_OBJECT) {
        snprintf(err, cap, "%sexpected an object {...}, got %s", pre, obj ? json_type_name(obj->type) : "nothing");
        return false;
    }
    for (uint32_t i = 0; props && i < obj->count; i++) {
        const char* key = obj->members[i].key;
        const JsonValue* v = &obj->members[i].value;
        const JsonValue* ps = json_get(props, key);
        if (!ps) {
            char list[600];
            keys_of(props, list, sizeof list);
            const char* m = meant_key(key, props);
            snprintf(err, cap, "%sunknown argument '%s'%s%s%s. It takes: %s", pre, key, m ? "; did you mean '" : "", m ? m : "", m ? "'?" : "", list);
            return false;
        }
        const JsonValue* ty = json_get(ps, "type");
        if (ty && ty->type == JSON_STRING && !json_is(v, ty->str)) {
            const JsonValue* desc = json_get(ps, "description");
            snprintf(err, cap, "%s'%s' should be %s%s, got %s%s%s", pre, key, !strcmp(ty->str, "array") ? "a list" : !strcmp(ty->str, "object") ? "an object" : "a ",
                     !strcmp(ty->str, "array") || !strcmp(ty->str, "object") ? "" : ty->str, json_type_name(v->type), desc ? " (" : "",
                     desc ? json_str(desc, "") : "");
            if (desc) {
                size_t l = strlen(err);
                snprintf(err + l, cap - l, ")");
            }
            return false;
        }
        const JsonValue* en = json_get(ps, "enum");
        if (en && v->type == JSON_STRING) {
            bool ok = false;
            char list[300] = "";
            for (size_t k = 0; k < json_len(en); k++) {
                const char* s = json_str(json_at(en, k), "");
                ok |= !strcmp(s, v->str);
                size_t l = strlen(list);
                snprintf(list + l, sizeof list - l, "%s%s", k ? ", " : "", s);
            }
            if (!ok) {
                snprintf(err, cap, "%s'%s' is '%s'; it takes one of: %s", pre, key, v->str, list);
                return false;
            }
        }
        const JsonValue* items = json_get(ps, "items");
        if (v->type == JSON_ARRAY && items && json_get(items, "properties")) {
            for (size_t k = 0; k < json_len(v); k++) {
                char w2[96];
                snprintf(w2, sizeof w2, "%s[%zu]", key, k);
                if (!check_obj(json_at(v, k), items, w2, err, cap)) return false;
            }
        }
    }
    const JsonValue* req = json_get(schema, "required");
    for (size_t k = 0; k < json_len(req); k++) {
        const char* r = json_str(json_at(req, k), "");
        if (!json_get(obj, r)) {
            snprintf(err, cap, "%smissing '%s'", pre, r);
            return false;
        }
    }
    return true;
}

static const JsonValue k_no_args = { JSON_OBJECT, 0, { 0 } };

// a call's arguments: false with what is wrong and how to fix it in err
static bool check_call_args(const char* tool, const JsonValue* args, char* err, size_t cap) {
    const JsonValue* schema = tool_schema(tool);
    if (!schema) return true; // (an unknown tool: its own error)
    if (!check_obj(args ? args : &k_no_args, schema, "", err, cap)) {
        size_t l = strlen(err);
        snprintf(err + l, cap - l, ". tool_help {\"tool\": \"%s\"} has examples that work as they are.", tool);
        return false;
    }
    return true;
}

// --- "next": the likely next call, at the end of every answer ---
static char g_recipe_next[480]; // (2026-11-24) a recipe's own next (playable_character: the Default Pawn an undo leaves); t_recipe clears it
static const char* next_hint(McpCall* c, Editor* ed) {
    const char* n = c->name;
    if (c->is_error) return "Read the error: it names the fix. tool_help gives this tool's examples; search finds the right names.";
    if (!strcmp(n, "recipe") && g_recipe_next[0]) return g_recipe_next;
    if (!strcmp(n, "get_started")) return ed->world.alive < 3 ? "The level is nearly empty: recipe test_level builds ground, sky, a Player Start and obstacles in one call."
                                                             : "inspect_level says what is missing; recipe {\"list\": true} has whole tasks in one call.";
    if (!strcmp(n, "spawn_actors") || !strcmp(n, "modify_actors") || !strcmp(n, "recipe") || !strcmp(n, "batch"))
        return "set_camera (look_at) + screenshot to judge the look, or inspect_level; undo reverts the last call.";
    if (!strcmp(n, "delete_actors")) return "undo brings them back if that was a mistake.";
    if (!strcmp(n, "run_script")) return "find_actors / get_actor to check what changed; undo reverts the whole run.";
    if (!strcmp(n, "write_script")) return "spawn_actors with asset = this path, then play {\"action\": \"start\", \"seconds\": 3} to test it.";
    if (!strcmp(n, "play")) return "get_log for errors; fix with write_script / modify_actors and play again.";
    if (!strcmp(n, "screenshot")) return "modify_actors to fix what looks wrong; level {\"action\": \"save\"} when it is right.";
    if (!strcmp(n, "inspect_level")) return NULL; // (its own next_steps)
    if (!strcmp(n, "level")) return NULL;
    if (!strcmp(n, "search") || !strcmp(n, "search_docs")) return "read_doc for an article in full; api_reference for a function's exact signature.";
    return NULL;
}

static void add_next(McpCall* c, Editor* ed) {
    if (strcmp(c->method, "tools/call")) return;
    const char* hint = next_hint(c, ed);
    if (!hint) return;
    char* t = mcp_jw_take(&c->body);
    size_t n = t ? strlen(t) : 0;
    if (n < 2 || t[n - 1] != '}') {
        free(t);
        return;
    }
    JsonWriter h;
    jw_init(&h);
    jw_str(&h, hint);
    char* hs = mcp_jw_take(&h);
    jw_free(&h);
    size_t cap = n + (hs ? strlen(hs) : 0) + 16;
    char* out = malloc(cap);
    if (out && hs) {
        t[n - 1] = 0; // (its closing brace)
        snprintf(out, cap, "%s%s\"next\":%s}", t, n > 2 ? "," : "", hs);
        jw_free(&c->body);
        jw_init(&c->body);
        jw_raw(&c->body, out);
    }
    free(out), free(hs), free(t);
}

// --- batch: a transaction ---
static const char* const k_batchable[] = { "spawn_actors", "modify_actors", "delete_actors", "select_actors", "set_camera", "run_script",
                                           "find_actors",  "get_actor",     "get_editor_state", "inspect_level" };
static bool batchable(const char* t) {
    for (size_t i = 0; i < sizeof k_batchable / sizeof k_batchable[0]; i++)
        if (!strcmp(k_batchable[i], t)) return true;
    return false;
}

// a step run as its own call, its answer written into out; false: it failed
static bool run_step(Editor* ed, double now, const char* tool, const JsonValue* args, JsonWriter* out) {
    McpCall s;
    memset(&s, 0, sizeof s);
    snprintf(s.method, sizeof s.method, "tools/call");
    snprintf(s.name, sizeof s.name, "%s", tool);
    s.args = args ? args : &k_no_args;
    jw_init(&s.body);
    run_tool(&s, ed, now);
    char* t = mcp_jw_take(&s.body);
    jw_raw(out, t && t[0] ? t : "{}");
    free(t);
    jw_free(&s.body);
    return !s.is_error;
}

// the steps checked before any runs (a malformed one: nothing done): false with the call failed
static bool batch_check(McpCall* c, const JsonValue* steps) {
    if (!steps || steps->type != JSON_ARRAY || !json_len(steps)) {
        fail(c, "steps: a list of {\"tool\": \"spawn_actors\", \"args\": {...}}");
        return false;
    }
    for (size_t i = 0; i < json_len(steps); i++) {
        const JsonValue* st = json_at(steps, i);
        const char* tool = json_get_str(st, "tool", "");
        if (!batchable(tool)) {
            char list[400] = "";
            for (size_t k = 0; k < sizeof k_batchable / sizeof k_batchable[0]; k++) {
                size_t l = strlen(list);
                snprintf(list + l, sizeof list - l, "%s%s", k ? ", " : "", k_batchable[k]);
            }
            fail(c, "steps[%zu]: '%s' %s. A batch takes: %s", i, tool, tool_def(tool) ? "can't be in a batch (it saves files, plays or waits for frames): call it on its own"
                                                                                       : "is no tool", list);
            return false;
        }
        char err[900];
        if (!check_call_args(tool, json_get(st, "args"), err, sizeof err)) {
            fail(c, "steps[%zu] (%s): %s", i, tool, err);
            return false;
        }
    }
    return true;
}

typedef struct BatchResult { int done, failed_at; bool rolled_back; double ms; } BatchResult;

// runs the steps (checked) as one undo step, writing "results" into the open object of c->body
static void batch_exec(McpCall* c, Editor* ed, double now, const JsonValue* steps, bool dry, bool atomic, BatchResult* r) {
    JsonWriter* jw = &c->body;
    memset(r, 0, sizeof *r);
    r->failed_at = -1;
    bool was_dirty = ed->dirty;
    EdCamera cam_was = ed->cam;
    uint32_t before = ed->world.alive;
    editor_undo_push(ed); // the batch's one step: the world as it was
    bool can_revert = ed->undo_count > 0;
    ed->undo_hold = true;
    g_mcp_dry = dry; // (2026-11-24) a dry run makes no file (the HUD Layout's widget)
    double t0 = re_time_seconds();
    jw_key(jw, "results");
    jw_arr_begin(jw);
    for (size_t i = 0; i < json_len(steps); i++) {
        const JsonValue* st = json_at(steps, i);
        const char* tool = json_get_str(st, "tool", "");
        jw_obj_begin(jw);
        jw_kstr(jw, "tool", tool);
        jw_key(jw, "result");
        bool ok = run_step(ed, now, tool, json_get(st, "args"), jw);
        if (!ok) jw_kbool(jw, "failed", true);
        jw_obj_end(jw);
        r->done++;
        if (!ok && r->failed_at < 0) r->failed_at = (int)i;
        if (!ok && atomic) break;
    }
    jw_arr_end(jw);
    g_mcp_dry = false;
    // (2026-11-24, the tester's item 6) the skeletal meshes' parts made now, as the editor's next frame would: the count
    // below is what the level has (a Character's Mannequin is 29 actors; the answer said 9, find_actors then 38)
    if (!ed->playing) anim_update(&ed->world, 0.0f, false);
    uint32_t would = ed->world.alive;
    ed->undo_hold = false;
    r->ms = (re_time_seconds() - t0) * 1000.0;
    if (dry) ed->cam = cam_was; // (2026-11-24) a dry run's set_camera: the view as it was too (the camera isn't in the undo step)
    if ((dry || (atomic && r->failed_at >= 0)) && can_revert) {
        editor_undo_revert(ed);
        ecs_touch_all(&ed->world);
        ed->dirty = was_dirty;
        r->rolled_back = true;
    }
    jw_kint(jw, "actors_before", before);
    jw_kint(jw, "actors_after", ed->world.alive);
    if (dry) jw_kint(jw, "actors_would_be", would); // (what the call without dry_run leaves)
    jw_kfloat(jw, "ms", dq(r->ms, 100.0));
    if (dry) jw_kstr(jw, "dry_run", "nothing was kept: call again without dry_run to do it");
    else if (r->rolled_back) {
        char m[200];
        snprintf(m, sizeof m, "step %d failed, so every step was rolled back (the level is as before); fix it and call again, or atomic: false", r->failed_at);
        jw_kstr(jw, "rolled_back", m);
    } else jw_kstr(jw, "undo", "one undo step for the whole batch");
    if (r->failed_at >= 0) c->is_error = true;
}

static void t_batch(McpCall* c, Editor* ed, double now) {
    if (ed->playing) {
        fail(c, "Stop playing first (play action stop): a batch changes the editor's level");
        return;
    }
    const JsonValue* steps = json_get(c->args, "steps");
    if (!batch_check(c, steps)) return;
    jw_obj_begin(&c->body);
    BatchResult r;
    batch_exec(c, ed, now, steps, json_get_bool(c->args, "dry_run", false), json_get_bool(c->args, "atomic", true), &r);
    jw_obj_end(&c->body);
}

// --- recipes ---
typedef struct RecipeDef { const char* name; const char* desc; const char* params; const char* example; } RecipeDef;
static const RecipeDef k_recipes[] = {
    { "test_level", "A level to test gameplay on: a ground, a Player Start, a sky preset (sun, sky light, fog, clouds), obstacles in a ring, the camera framing it.",
      "size_m (40: the ground's side in meters), obstacles (6), lighting (day | morning | sunset | cloudy | overcast | foggy | night), player_start (true)",
      "{\"recipe\": \"test_level\", \"params\": {\"size_m\": 60, \"obstacles\": 8, \"lighting\": \"sunset\"}}" },
    { "lighting", "The sky and the light: a sky preset sets (or makes) the sun, the sky light, the fog and the clouds.",
      "style (day | morning | sunset | cloudy | overcast | foggy | night)", "{\"recipe\": \"lighting\", \"params\": {\"style\": \"night\"}}" },
    { "playable_character", "A Character class you can play (moves, jumps, camera, input) made in /Content, the project's Default Pawn, placed in the level (and a Player Start when none).",
      "name (BP_Player), view (third | first | top_down), location ([0,0,100])", "{\"recipe\": \"playable_character\", \"params\": {\"name\": \"BP_Hero\", \"view\": \"first\"}}" },
    { "pbr_material", "A PBR material from texture maps (base color, roughness, metallic, normal, ambient occlusion), found by their names in a folder or given one by one; applied to actors.",
      "name (the material: M_<name>), folder (where the textures are: /Content/...), textures ({base_color, roughness, metallic, normal, ao}: paths), "
      "output (the material's folder: the textures' folder), apply_to ([actors]), tile_cm (0: the mesh's UVs; > 0: world aligned)",
      "{\"recipe\": \"pbr_material\", \"params\": {\"name\": \"Brick\", \"folder\": \"/Content/Textures/Brick\", \"apply_to\": [\"Wall\"]}}" },
    { "hud", "The game's HUD (health bars, compass, messages, crosshair, markers): a HUD Layout on an actor (a new GameHUD actor, or the one given); Content/UI/WBP_GameHUD made when missing.",
      "actor (optional: the actor that carries it)", "{\"recipe\": \"hud\"}" },
};
#define NRECIPES (int)(sizeof k_recipes / sizeof k_recipes[0])

static char* recipes_markdown(void) {
    size_t cap = 8192, n = 0;
    char* s = malloc(cap);
    if (!s) return NULL;
    n += (size_t)snprintf(s + n, cap - n, "# Recipes: a whole task in one call (tool `recipe`)\n\n`dry_run: true` answers the plan and changes nothing; "
                                          "each recipe is one undo step.\n\n");
    for (int i = 0; i < NRECIPES && n < cap; i++)
        n += (size_t)snprintf(s + n, cap - n, "## %s\n%s\n- Params: %s\n- Example: `%s`\n\n", k_recipes[i].name, k_recipes[i].desc, k_recipes[i].params, k_recipes[i].example);
    return s;
}

static const char* sky_name(const char* style) {
    static const struct { const char* k; const char* name; } m[] = { { "day", "Clear Day" },       { "clear", "Clear Day" }, { "morning", "Morning" },
                                                                     { "sunset", "Sunset" },       { "cloudy", "Cloudy" },   { "overcast", "Overcast" },
                                                                     { "foggy", "Foggy" },         { "fog", "Foggy" },       { "night", "Night (Moonlight)" },
                                                                     { "moonlight", "Night (Moonlight)" } };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++)
        if (!_stricmp(style, m[i].k) || !_stricmp(style, m[i].name)) return m[i].name;
    return NULL;
}

static void wvec(JsonWriter* w, const char* key, float x, float y, float z) {
    float v[3] = { x, y, z };
    jw_kfloats(w, key, v, 3);
}

// a socket of a material node by its name (-1: none)
static int mcp_sock(const MatGraph* g, int node, bool out, const char* name) {
    if (node < 0) return -1;
    const MatNodeDef* d = mat_node_def(g->nodes[node].type);
    if (!d) return -1;
    int n = out ? d->nout : d->nin;
    for (int i = 0; i < n; i++)
        if (!strcmp(out ? d->out[i].name : d->in[i].name, name)) return i;
    return -1;
}

// the pictures in a folder (its own, not its subfolders'): their /Content paths
typedef struct PicList { char folder[400]; char vp[128][512]; int n; } PicList;
static void pic_found(void* user, const char* name) {
    PicList* pl = user;
    if (pl->n >= 128 || name[0] == '.') return;
    char vp[512];
    snprintf(vp, sizeof vp, "%s/%s", pl->folder, name);
    if (content_kind_of(vp) == ASSET_TEXTURE_SOURCE) snprintf(pl->vp[pl->n++], 512, "%s", vp);
}

// a step {"tool": ..., "args": {...}} opened: the caller writes the args' members, then step_end
static void step_begin(JsonWriter* w, const char* tool) {
    jw_obj_begin(w);
    jw_kstr(w, "tool", tool);
    jw_key(w, "args");
    jw_obj_begin(w);
}
static void step_end(JsonWriter* w) {
    jw_obj_end(w);
    jw_obj_end(w);
}

// playable_character's view: the Camera Boom's length and angle (the templates')
static void recipe_pawn_view(BpAsset* cls, bool first, bool top) {
    for (uint32_t i = 0; i < cls->nscs; i++) {
        if (strcmp(cls->scs[i].name, "CameraBoom")) continue;
        CSpringArm* arm = (CSpringArm*)cls->scs[i].data;
        if (first) arm->target_arm_length = 0.0f, arm->do_collision_test = 0, cls->scs[i].location[2] = 64.0f;
        else if (top) arm->target_arm_length = 1400.0f, arm->use_pawn_control_rotation = 0, arm->do_collision_test = 0, cls->scs[i].rotation[0] = -55.0f;
        else arm->enable_camera_lag = 1, arm->camera_lag_speed = 12.0f;
    }
    if (first) ((CCharacter*)cls->comp_data[COMP_CHARACTER])->use_controller_yaw = 1;
}
// a dry run's class in memory (recipe_plan), freed after its trial
static void recipe_dry_free(void) {
    if (g_dry_cls) bp_free_asset(g_dry_cls);
    g_dry_cls = NULL, g_dry_vpath[0] = 0;
}

// the recipe's steps (world changes only) into w (an array open); its files made unless dry; false with err
static bool recipe_plan(Editor* ed, const char* name, const JsonValue* p, bool dry, JsonWriter* w, JsonWriter* made, char* err, size_t ecap) {
    if (!strcmp(name, "test_level") || !strcmp(name, "lighting")) {
        bool level = !strcmp(name, "test_level");
        const char* style = json_get_str(p, level ? "lighting" : "style", "day");
        const char* sky = sky_name(style);
        if (!sky) return snprintf(err, ecap, "%s '%s': day, morning, sunset, cloudy, overcast, foggy or night", level ? "lighting" : "style", style), false;
        step_begin(w, "spawn_actors");
        jw_key(w, "actors");
        jw_arr_begin(w);
        float size = (float)json_get_num(p, "size_m", 40.0);
        size = size < 5.0f ? 5.0f : size > 2000.0f ? 2000.0f : size;
        if (level) {
            jw_obj_begin(w); // the ground: a plane of size_m (a basic shape is 1 m)
            jw_kstr(w, "type", "Plane"), jw_kstr(w, "name", "Ground");
            wvec(w, "location", 0, 0, 0), wvec(w, "scale", size, size, 1);
            jw_key(w, "properties"), jw_obj_begin(w), wvec(w, "Base Color", 0.42f, 0.43f, 0.45f), jw_obj_end(w);
            jw_obj_end(w);
            if (json_get_bool(p, "player_start", true)) {
                jw_obj_begin(w);
                jw_kstr(w, "type", "Player Start"), wvec(w, "location", 0, 0, 100);
                jw_obj_end(w);
            }
            int n = json_get_int(p, "obstacles", 6);
            n = n < 0 ? 0 : n > 64 ? 64 : n;
            float r = size * 100.0f * 0.3f;
            for (int k = 0; k < n; k++) { // a ring of boxes of three heights (to jump on, to walk around)
                float a = 6.2831853f * (float)k / (float)(n ? n : 1), h = 0.5f + 0.5f * (float)(k % 3);
                char nm[32];
                snprintf(nm, sizeof nm, "Obstacle%d", k + 1);
                jw_obj_begin(w);
                jw_kstr(w, "type", "Cube"), jw_kstr(w, "name", nm);
                wvec(w, "location", roundf(cosf(a) * r), roundf(sinf(a) * r), 50.0f * h);
                wvec(w, "scale", 1.5f, 1.5f, h);
                wvec(w, "rotation", 0, roundf(a * 57.29578f), 0);
                jw_obj_end(w);
            }
        }
        jw_obj_begin(w); // the sky preset: the sun, the sky light, the fog and the clouds (made when missing)
        jw_kstr(w, "type", sky);
        jw_obj_end(w);
        jw_arr_end(w);
        jw_kbool(w, "select", false);
        step_end(w);
        if (level) { // the camera framing it
            step_begin(w, "set_camera");
            wvec(w, "location", -size * 55.0f, -size * 55.0f, size * 35.0f);
            wvec(w, "look_at", 0, 0, 0);
            step_end(w);
        }
        return true;
    }
    if (!strcmp(name, "hud")) {
        const JsonValue* actor = json_get(p, "actor");
        if (actor) {
            if (!find_actor(ed, actor)) {
                char r[160];
                actor_ref_text(actor, r, sizeof r);
                return snprintf(err, ecap, "actor '%s' isn't in the level (find_actors lists them)", r), false;
            }
            step_begin(w, "modify_actors");
            jw_key(w, "actors"), jw_arr_begin(w), jw_obj_begin(w);
            jw_key(w, "actor"), actor->type == JSON_NUMBER ? jw_float(w, actor->num) : jw_str(w, json_str(actor, ""));
            jw_key(w, "components"), jw_arr_begin(w), jw_str(w, "HUD Layout"), jw_arr_end(w);
            jw_obj_end(w), jw_arr_end(w);
            step_end(w);
        } else {
            step_begin(w, "spawn_actors");
            jw_key(w, "actors"), jw_arr_begin(w), jw_obj_begin(w);
            jw_kstr(w, "type", "Empty Actor"), jw_kstr(w, "name", "GameHUD"), wvec(w, "location", 0, 0, 0);
            jw_key(w, "components"), jw_arr_begin(w), jw_str(w, "HUD Layout"), jw_arr_end(w);
            jw_obj_end(w), jw_arr_end(w);
            jw_kbool(w, "select", false);
            step_end(w);
        }
        jw_str(made, "/Content/UI/WBP_GameHUD.rbp (made when missing: the bars, compass, messages, crosshair; edit it in the Widget editor)");
        return true;
    }
    if (!strcmp(name, "playable_character")) {
        const char* nm = json_get_str(p, "name", "BP_Player");
        const char* view = json_get_str(p, "view", "third");
        bool first = !_stricmp(view, "first") || !_stricmp(view, "first_person"), top = !_stricmp(view, "top_down") || !_stricmp(view, "topdown");
        if (!first && !top && _stricmp(view, "third") && _stricmp(view, "third_person"))
            return snprintf(err, ecap, "view '%s': third, first or top_down", view), false;
        for (const char* q = nm; *q; q++)
            if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_'))
                return snprintf(err, ecap, "name '%s': letters, digits and _ only (a class name: BP_Player)", nm), false;
        char folder[160];
        snprintf(folder, sizeof folder, "/Content/%s", first ? "FirstPerson" : top ? "TopDown" : "ThirdPerson");
        char vpath[300];
        snprintf(vpath, sizeof vpath, "%s/%s.rbp", folder, nm);
        float loc[3] = { 0, 0, 100 };
        get_vec3(p, "location", loc);
        BpAsset* cls = bp_find_name(BPA_CLASS, nm);
        const char* pawn_was = project()->default_pawn[0] ? project()->default_pawn : "none";
        if (!dry) {
            if (!cls) {
                char dir[1200];
                snprintf(dir, sizeof dir, "%s%s", content_root(), folder + 8);
                re_make_dirs(dir);
                cls = bp_create(BPA_CLASS, folder, nm, "Character");
                if (!cls) return snprintf(err, ecap, "the class %s couldn't be made", vpath), false;
                recipe_pawn_view(cls, first, top);
                bp_save(cls);
                content_mark_dirty();
                content_created(vpath);
                jw_str(made, vpath);
            }
            // (2026-11-24, the tester's item 6) the Default Pawn is a project setting, not the level: the recipe's undo
            // step (the level) leaves it; said here and in "next"
            char m[400];
            snprintf(m, sizeof m, "Default Pawn: %s (was %s; an undo takes the actors back, not this: Project Settings > Maps & Modes)", cls->name, pawn_was);
            snprintf(g_recipe_next, sizeof g_recipe_next, "The Default Pawn is now %s (was %s): undo takes back the actors, not the Default Pawn "
                     "(Project Settings > Maps & Modes). play {\"action\": \"start\", \"seconds\": 3} to try the character.", cls->name, pawn_was);
            jw_str(made, m);
            snprintf(project()->default_pawn, sizeof project()->default_pawn, "%s", cls->name);
            project_save(NULL);
        } else {
            char m[400];
            if (!cls) { // (2026-11-24, the tester's item 6) the class in memory for the trial spawn: the plan whole, nothing on disk
                g_dry_cls = bp_new_class(vpath, "", nm, "Character");
                if (!g_dry_cls) return snprintf(err, ecap, "the class %s couldn't be made", vpath), false;
                recipe_pawn_view(g_dry_cls, first, top);
                snprintf(g_dry_vpath, sizeof g_dry_vpath, "%s", vpath);
                snprintf(m, sizeof m, "%s would be made (a Character, %s view)", vpath, first ? "first person" : top ? "top-down" : "third person");
                jw_str(made, m);
            }
            snprintf(m, sizeof m, "Default Pawn: %s would be set (now %s; an undo would leave it)", nm, pawn_was);
            jw_str(made, m);
        }
        bool start = false;
        for (uint32_t i = 1; i < ed->world.slot_count && !start; i++) {
            Entity e = ecs_entity_at(&ed->world, i);
            start = e && ecs_get(&ed->world, e, COMP_PLAYER_START);
        }
        step_begin(w, "spawn_actors");
        jw_key(w, "actors"), jw_arr_begin(w);
        jw_obj_begin(w); // (the class: made above, or in memory for a dry run's trial)
        jw_kstr(w, "asset", vpath), wvec(w, "location", loc[0], loc[1], loc[2]);
        jw_obj_end(w);
        if (!start) {
            jw_obj_begin(w);
            jw_kstr(w, "type", "Player Start"), wvec(w, "location", loc[0], loc[1], loc[2]);
            jw_obj_end(w);
        }
        jw_arr_end(w);
        step_end(w);
        return true;
    }
    if (!strcmp(name, "pbr_material")) {
        const char* nm = json_get_str(p, "name", "");
        const char* folder = json_get_str(p, "folder", "");
        const JsonValue* tex = json_get(p, "textures");
        if (!nm[0]) return snprintf(err, ecap, "name: the material's name (M_<name>.rmat)"), false;
        if (!folder[0] && !tex) return snprintf(err, ecap, "give folder (/Content/... where the maps are) or textures {base_color, roughness, metallic, normal, ao}"), false;
        // the maps: given, or found by their names in the folder (Unreal's and the stores' suffixes)
        enum { MP_BASE, MP_ROUGH, MP_METAL, MP_NORMAL, MP_AO, MP_COUNT };
        static const char* const keys[MP_COUNT] = { "base_color", "roughness", "metallic", "normal", "ao" };
        static const char* const tags[MP_COUNT][8] = { { "basecolor", "base_color", "albedo", "diffuse", "color", "col", "_d.", NULL },
                                                        { "roughness", "rough", "_r.", NULL },
                                                        { "metallic", "metalness", "metal", "_m.", NULL },
                                                        { "normalgl", "normal", "nrm", "_n.", NULL },
                                                        { "ambientocclusion", "occlusion", "_ao", "ao.", NULL } };
        char maps[MP_COUNT][400];
        memset(maps, 0, sizeof maps);
        for (int k = 0; k < MP_COUNT && tex; k++) snprintf(maps[k], sizeof maps[k], "%s", json_get_str(tex, keys[k], ""));
        if (folder[0]) {
            if (_strnicmp(folder, "/Content", 8) || strstr(folder, "..")) return snprintf(err, ecap, "folder: a /Content/... path"), false;
            // (the folder on the disk: content_search leaves the .png / .jpg out, the thumbnails' kinds)
            PicList pl;
            memset(&pl, 0, sizeof pl);
            snprintf(pl.folder, sizeof pl.folder, "%s", folder);
            size_t fl = strlen(pl.folder);
            while (fl && pl.folder[fl - 1] == '/') pl.folder[--fl] = 0;
            char dir[1300];
            snprintf(dir, sizeof dir, "%s%s", content_root(), pl.folder + 8);
            re_list_dir(dir, pic_found, &pl);
            int seen = pl.n;
            for (int i = 0; i < pl.n; i++) {
                const char* vp_i = pl.vp[i];
                char low[512];
                snprintf(low, sizeof low, "%s", strrchr(vp_i, '/') + 1);
                for (char* q = low; *q; q++) *q = (char)(*q >= 'A' && *q <= 'Z' ? *q + 32 : *q);
                for (int k = MP_COUNT - 1; k >= 0; k--) { // (ao before normal before ... base color last: "color" is in many names)
                    if (maps[k][0]) continue;
                    bool hit = false;
                    for (int t = 0; tags[k][t] && !hit; t++) hit = strstr(low, tags[k][t]) != NULL;
                    if (k == MP_BASE && (strstr(low, "normal") || strstr(low, "rough") || strstr(low, "metal") || strstr(low, "occlusion"))) hit = false;
                    if (hit) {
                        snprintf(maps[k], sizeof maps[k], "%s", vp_i);
                        break;
                    }
                }
            }
            if (!seen) return snprintf(err, ecap, "no textures in %s (list_assets kind Texture shows where they are)", folder), false;
        }
        if (!maps[MP_BASE][0]) return snprintf(err, ecap, "no base color map found (a name with BaseColor, Albedo, Diffuse or Color; or textures.base_color)"), false;
        const char* outdir = json_get_str(p, "output", "");
        char outf[400];
        if (outdir[0]) snprintf(outf, sizeof outf, "%s", outdir);
        else {
            snprintf(outf, sizeof outf, "%s", maps[MP_BASE]);
            char* sl = strrchr(outf, '/');
            if (sl) *sl = 0;
        }
        char mvp[600], disk[1300];
        snprintf(mvp, sizeof mvp, "%s/%s%s.rmat", outf, _strnicmp(nm, "M_", 2) ? "M_" : "", nm);
        snprintf(disk, sizeof disk, "%s%s", content_root(), mvp + 8);
        for (int k = 0; k < MP_COUNT; k++) {
            if (!maps[k][0]) continue;
            char m[520];
            snprintf(m, sizeof m, "%s: %s", keys[k], maps[k]);
            jw_str(made, m);
        }
        if (!dry) {
            char dir[1300];
            snprintf(dir, sizeof dir, "%s", disk);
            char* sl = strrchr(dir, '/');
            if (sl) *sl = 0, re_make_dirs(dir);
            Material* m = re_file_stat(disk, NULL, NULL, NULL) ? mat_load(disk) : mat_create(disk, false, NULL);
            if (!m || !m->graph) return snprintf(err, ecap, "%s couldn't be made", mvp), false;
            MatGraph* g = m->graph;
            int bsdf = -1;
            for (int i = 0; i < g->node_count; i++)
                if (g->nodes[i].type == NODE_BSDF_PRINCIPLED) bsdf = i;
            if (bsdf < 0) return snprintf(err, ecap, "%s has no Principled BSDF", mvp), false;
            float bx = g->nodes[bsdf].x, by = g->nodes[bsdf].y;
            #define SOCK(node, out, nm_) mcp_sock(g, node, out, nm_)
            int img[MP_COUNT];
            for (int k = 0; k < MP_COUNT; k++) {
                img[k] = -1;
                if (!maps[k][0]) continue;
                img[k] = mat_graph_add(g, NODE_TEX_IMAGE, bx - 700.0f, by + 180.0f * (float)k);
                if (img[k] < 0) continue;
                snprintf(g->nodes[img[k]].image, sizeof g->nodes[img[k]].image, "%s", maps[k]);
                if (k != MP_BASE) g->nodes[img[k]].prop[2] = IMAGE_COLORSPACE_NONCOLOR;
            }
            int base = img[MP_BASE];
            const char* base_out = "Color";
            if (img[MP_AO] >= 0) { // the ambient occlusion darkens the color (its crevices)
                int mix = mat_graph_add(g, NODE_MIX, bx - 380.0f, by);
                if (mix >= 0) {
                    g->nodes[mix].prop[1] = BLEND_MUL;
                    int fi = SOCK(mix, false, "Factor");
                    if (fi >= 0) g->nodes[mix].in[fi][0] = 1.0f;
                    mat_graph_link(g, base, SOCK(base, true, "Color"), mix, SOCK(mix, false, "A_Color"));
                    mat_graph_link(g, img[MP_AO], SOCK(img[MP_AO], true, "Color"), mix, SOCK(mix, false, "B_Color"));
                    base = mix, base_out = "Result_Color";
                }
            }
            mat_graph_link(g, base, SOCK(base, true, base_out), bsdf, SOCK(bsdf, false, "Base Color"));
            if (img[MP_ROUGH] >= 0) mat_graph_link(g, img[MP_ROUGH], SOCK(img[MP_ROUGH], true, "Color"), bsdf, SOCK(bsdf, false, "Roughness"));
            if (img[MP_METAL] >= 0) mat_graph_link(g, img[MP_METAL], SOCK(img[MP_METAL], true, "Color"), bsdf, SOCK(bsdf, false, "Metallic"));
            if (img[MP_NORMAL] >= 0) {
                int nmn = mat_graph_add(g, NODE_NORMAL_MAP, bx - 380.0f, by + 540.0f);
                if (nmn >= 0) {
                    mat_graph_link(g, img[MP_NORMAL], SOCK(img[MP_NORMAL], true, "Color"), nmn, SOCK(nmn, false, "Color"));
                    mat_graph_link(g, nmn, SOCK(nmn, true, "Normal"), bsdf, SOCK(bsdf, false, "Normal"));
                }
            }
            #undef SOCK
            float tile = (float)json_get_num(p, "tile_cm", 0.0);
            m->world_aligned = tile > 0.0f;
            if (tile > 0.0f) m->world_tile = tile;
            mat_save(m);
            mat_mark_dirty(m);
            content_mark_dirty();
            content_created(mvp);
        }
        char m2[700];
        snprintf(m2, sizeof m2, "material: %s%s", mvp, dry ? " (would be made)" : "");
        jw_str(made, m2);
        const JsonValue* to = json_get(p, "apply_to");
        if (json_len(to)) {
            step_begin(w, "modify_actors");
            jw_key(w, "actors"), jw_arr_begin(w);
            for (size_t i = 0; i < json_len(to); i++) {
                const JsonValue* a = json_at(to, i);
                jw_obj_begin(w);
                jw_key(w, "actor"), a->type == JSON_NUMBER ? jw_float(w, a->num) : jw_str(w, json_str(a, ""));
                if (!dry) jw_kstr(w, "material", mvp);
                jw_obj_end(w);
            }
            jw_arr_end(w);
            step_end(w);
        }
        return true;
    }
    snprintf(err, ecap, "no recipe '%s': test_level, lighting, playable_character, pbr_material, hud ({\"list\": true} has them)", name);
    return false;
}

static void t_recipe(McpCall* c, Editor* ed, double now) {
    JsonWriter* jw = &c->body;
    const char* name = json_get_str(c->args, "recipe", "");
    g_recipe_next[0] = 0;
    if (json_get_bool(c->args, "list", false) || !name[0]) {
        jw_obj_begin(jw);
        jw_key(jw, "recipes");
        jw_arr_begin(jw);
        for (int i = 0; i < NRECIPES; i++) {
            jw_obj_begin(jw);
            jw_kstr(jw, "recipe", k_recipes[i].name);
            jw_kstr(jw, "does", k_recipes[i].desc);
            jw_kstr(jw, "params", k_recipes[i].params);
            jw_key(jw, "example");
            jw_raw(jw, k_recipes[i].example);
            jw_obj_end(jw);
        }
        jw_arr_end(jw);
        jw_kstr(jw, "note", "Each recipe is one call and one undo step; dry_run: true answers the plan and changes nothing.");
        jw_obj_end(jw);
        return;
    }
    if (ed->playing) {
        fail(c, "Stop playing first (play action stop): a recipe changes the editor's level");
        return;
    }
    bool dry = json_get_bool(c->args, "dry_run", false);
    const JsonValue* params = json_get(c->args, "params");
    JsonWriter plan, made;
    jw_init(&plan), jw_init(&made);
    jw_obj_begin(&plan);
    jw_key(&plan, "steps");
    jw_arr_begin(&plan);
    jw_arr_begin(&made);
    char err[600] = "";
    double t0 = re_time_seconds();
    bool ok = recipe_plan(ed, name, params ? params : &k_no_args, dry, &plan, &made, err, sizeof err);
    jw_arr_end(&plan);
    jw_obj_end(&plan);
    jw_arr_end(&made);
    char* pt = mcp_jw_take(&plan);
    char* mt = mcp_jw_take(&made);
    jw_free(&plan), jw_free(&made);
    JsonDoc d;
    memset(&d, 0, sizeof d);
    char perr[160];
    if (!ok || !pt || !json_parse(pt, strlen(pt), &d, perr, sizeof perr)) {
        fail(c, "recipe %s: %s", name, ok ? "its plan didn't parse (a bug: tell the user)" : err);
        free(pt), free(mt), recipe_dry_free();
        return;
    }
    const JsonValue* steps = json_get(&d.root, "steps");
    if (!batch_check(c, steps)) {
        json_free(&d), free(pt), free(mt), recipe_dry_free();
        return;
    }
    jw_obj_begin(jw);
    jw_kstr(jw, "recipe", name);
    jw_key(jw, "files");
    jw_raw(jw, mt ? mt : "[]");
    BatchResult r;
    batch_exec(c, ed, now, steps, dry, true, &r);
    recipe_dry_free(); // (its trial actors went with the roll back)
    jw_kfloat(jw, "total_ms", dq((re_time_seconds() - t0) * 1000.0, 100.0));
    if (dry) {
        jw_key(jw, "plan");
        jw_raw(jw, pt);
    }
    jw_obj_end(jw);
    json_free(&d);
    free(pt), free(mt);
}

// --- inspect_level ---
static void issue(JsonWriter* jw, const char* severity, const char* what, const char* fix) {
    jw_obj_begin(jw);
    jw_kstr(jw, "severity", severity);
    jw_kstr(jw, "issue", what);
    jw_kstr(jw, "fix", fix);
    jw_obj_end(jw);
}

typedef struct LevelFacts { int lights, suns, skies, ground, starts, characters, fallen, scripts_bad, log_errors; char bad_script[160], fallen_name[64], log_line[200]; } LevelFacts;

static void level_facts(Editor* ed, LevelFacts* f) {
    memset(f, 0, sizeof *f);
    World* w = &ed->world;
    for (uint32_t i = 1; i < w->slot_count; i++) {
        Entity e = ecs_entity_at(w, i);
        if (!e) continue;
        const CLight* l = ecs_get(w, e, COMP_LIGHT);
        if (l) f->lights++, f->suns += l->type == LIGHT_DIRECTIONAL, f->skies += l->type == LIGHT_SKY;
        const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        if (ecs_get(w, e, COMP_LANDSCAPE)) f->ground++;
        else if (t && ecs_get(w, e, COMP_STATIC_MESH) && t->scale[0] * t->scale[1] >= 9.0f && t->location[2] < 150.0f) f->ground++; // (a wide floor near the origin's height)
        if (ecs_get(w, e, COMP_PLAYER_START)) f->starts++;
        if (ecs_get(w, e, COMP_CHARACTER)) f->characters++;
        if (t && t->location[2] < -10000.0f) {
            if (!f->fallen) snprintf(f->fallen_name, sizeof f->fallen_name, "%s", actor_name(w, e));
            f->fallen++;
        }
    }
    for (uint32_t i = 0; i < script_count(); i++) {
        const ScriptFile* s = script_at(i);
        if (!s || s->ok || strstr(s->vpath, "/__MCP__/")) continue;
        if (!f->scripts_bad) snprintf(f->bad_script, sizeof f->bad_script, "%s", s->vpath);
        f->scripts_bad++;
    }
    for (int i = ed->log_count > 40 ? ed->log_count - 40 : 0; i < ed->log_count; i++)
        if (icontains(ed->log[i], "error") || icontains(ed->log[i], "failed") || icontains(ed->log[i], "couldn't")) {
            snprintf(f->log_line, sizeof f->log_line, "%s", ed->log[i]);
            f->log_errors++;
        }
}

static int write_issues(JsonWriter* jw, Editor* ed, const LevelFacts* f) {
    int n = 0;
    char m[400];
    if (!f->lights) issue(jw, "high", "No light: the level renders dark", "recipe lighting {\"style\": \"day\"} (sun, sky light, fog, clouds)"), n++;
    else if (!f->skies) issue(jw, "medium", "No Sky Light: shadows are flat black", "recipe lighting (a sky preset adds it) or spawn_actors Sky Light"), n++;
    if (!f->ground) issue(jw, "high", "Nothing to stand on (no Landscape and no wide floor near Z 0): the player falls", "recipe test_level, or spawn_actors a Plane scaled [40,40,1]"), n++;
    if (!f->starts && !f->characters && !project()->default_pawn[0])
        issue(jw, "high", "No player: no Player Start, no Character and no Default Pawn", "recipe playable_character {\"view\": \"third\"}"), n++;
    else if (!f->starts && !f->characters) issue(jw, "low", "No Player Start: the player starts at the camera", "spawn_actors Player Start at [0,0,100]"), n++;
    if (f->fallen) {
        snprintf(m, sizeof m, "%d actor(s) far below the world (Z < -100 m), e.g. %s", f->fallen, f->fallen_name);
        issue(jw, "medium", m, "modify_actors their location, or delete_actors them"), n++;
    }
    if (f->scripts_bad) {
        snprintf(m, sizeof m, "%d script(s) don't compile, e.g. %s", f->scripts_bad, f->bad_script);
        issue(jw, "high", m, "write_script it again with the errors fixed (it answers the lines)"), n++;
    }
    if (f->log_errors) {
        snprintf(m, sizeof m, "%d error line(s) in the log, the last: %s", f->log_errors, f->log_line);
        issue(jw, "medium", m, "get_log for the rest; search with the message"), n++;
    }
    double fms = ed->vk ? ed->vk->gpu_ms : 0.0;
    if (fms > 16.7) {
        snprintf(m, sizeof m, "Slow frame: %.1f ms (under 60 fps)", fms);
        issue(jw, "low", m, "the prompt optimize_performance; console r.ScreenPercentage 75; fewer shadowed lights"), n++;
    }
    if (ed->dirty) issue(jw, "low", "Unsaved changes", ed->level_path[0] ? "level {\"action\": \"save\"}" : "level {\"action\": \"save\", \"path\": \"/Content/Maps/Name.rlevel\"}"), n++;
    return n;
}

static void t_inspect(McpCall* c, Editor* ed) {
    JsonWriter* jw = &c->body;
    LevelFacts f;
    level_facts(ed, &f);
    jw_obj_begin(jw);
    jw_key(jw, "state");
    jw_obj_begin(jw);
    write_state(jw, ed);
    jw_obj_end(jw);
    jw_key(jw, "has");
    jw_obj_begin(jw);
    jw_kint(jw, "lights", f.lights), jw_kint(jw, "suns", f.suns), jw_kint(jw, "sky_lights", f.skies), jw_kint(jw, "ground", f.ground);
    jw_kint(jw, "player_starts", f.starts), jw_kint(jw, "characters", f.characters);
    jw_kstr(jw, "default_pawn", project()->default_pawn[0] ? project()->default_pawn : "(none)");
    jw_obj_end(jw);
    jw_key(jw, "issues");
    jw_arr_begin(jw);
    int n = write_issues(jw, ed, &f);
    jw_arr_end(jw);
    jw_key(jw, "next_steps");
    jw_arr_begin(jw);
    if (!f.ground && !f.lights) jw_str(jw, "recipe test_level: ground, sky, a Player Start, obstacles (one call)");
    else if (!f.lights) jw_str(jw, "recipe lighting");
    if (!f.characters && !project()->default_pawn[0]) jw_str(jw, "recipe playable_character");
    jw_str(jw, n ? "fix the issues above (each says how), then inspect_level again" : "play {\"action\": \"start\", \"seconds\": 3} to test, then get_log");
    jw_arr_end(jw);
    jw_obj_end(jw);
}

static char* state_markdown(Editor* ed) {
    LevelFacts f;
    level_facts(ed, &f);
    JsonWriter jw;
    jw_init(&jw);
    jw_arr_begin(&jw);
    write_issues(&jw, ed, &f);
    jw_arr_end(&jw);
    char* issues = mcp_jw_take(&jw);
    jw_free(&jw);
    size_t cap = 4096 + (issues ? strlen(issues) : 0);
    char* s = malloc(cap);
    if (s)
        snprintf(s, cap,
                 "# The level now\n- Project: %s\n- Actors: %u (lights %d, ground %d, Player Starts %d, Characters %d)\n- Default Pawn: %s\n- Play: %s\n- Unsaved: %s\n\n"
                 "## Issues (each with its fix)\n```json\n%s\n```\n",
                 project()->name, ed->world.alive, f.lights, f.ground, f.starts, f.characters, project()->default_pawn[0] ? project()->default_pawn : "(none)",
                 ed->playing ? "playing" : "stopped", ed->dirty ? "yes" : "no", issues ? issues : "[]");
    free(issues);
    return s;
}

// --- tool_help ---
typedef struct ToolHelp { const char* tool; const char* examples; const char* errors; const char* related; } ToolHelp;
static const ToolHelp k_help[] = {
    { "spawn_actors", "[{\"actors\": [{\"type\": \"Cube\", \"name\": \"Box1\", \"location\": [0, 0, 50], \"scale\": [2, 2, 1]}, "
                      "{\"type\": \"Point Light\", \"location\": [0, 0, 300], \"properties\": {\"Intensity\": 5000, \"Light Color\": [1, 0.8, 0.6]}}]}, "
                      "{\"actors\": [{\"asset\": \"/Content/Meshes/Rock.rmesh\", \"location\": [500, 0, 0], \"material\": \"/Content/Materials/M_Rock.rmat\"}]}, "
                      "{\"actors\": [{\"type\": \"Empty Actor\", \"name\": \"Pickup\", \"components\": [\"Interactable\", \"Rotating Movement\"]}]}]",
      "a wrong type answers the close ones (list_actor_types has all); location is [x, y, z] in cm, Z up; a material needs a Static Mesh",
      "list_actor_types, search, modify_actors, batch, recipe" },
    { "modify_actors", "[{\"actors\": [{\"actor\": \"Box1\", \"offset\": [0, 0, 100]}, {\"actor\": 42, \"properties\": {\"Intensity\": 2000}}]}, "
                       "{\"actors\": [{\"actor\": \"Hero\", \"components\": [\"Health\"]}]}]",
      "actor is the id (number) or the name; properties use the Details names (get_actor lists them); offset adds, location sets",
      "get_actor, find_actors, batch" },
    { "batch", "[{\"steps\": [{\"tool\": \"spawn_actors\", \"args\": {\"actors\": [{\"type\": \"Cube\", \"name\": \"A\"}]}}, "
               "{\"tool\": \"modify_actors\", \"args\": {\"actors\": [{\"actor\": \"A\", \"offset\": [0, 0, 100]}]}}]}, "
               "{\"dry_run\": true, \"steps\": [{\"tool\": \"delete_actors\", \"args\": {\"actors\": [\"A\"]}}]}]",
      "only world tools (no write_script, level, play, screenshot, undo, console); a malformed step stops it before anything runs; atomic rolls back on a failed step",
      "recipe, undo" },
    { "recipe", "[{\"list\": true}, {\"recipe\": \"test_level\", \"params\": {\"size_m\": 40, \"lighting\": \"sunset\"}}, "
                "{\"recipe\": \"playable_character\", \"params\": {\"view\": \"first\"}}, {\"recipe\": \"pbr_material\", \"params\": {\"name\": \"Brick\", \"folder\": \"/Content/Textures/Brick\"}}, "
                "{\"recipe\": \"hud\"}, {\"recipe\": \"lighting\", \"params\": {\"style\": \"night\"}, \"dry_run\": true}]",
      "params are checked by each recipe (the error lists the valid values); the files it makes (a class, a material) stay after an undo",
      "batch, inspect_level, undo" },
    { "run_script", "[{\"code\": \"for i in range(10):\\n    spawn_actor_from_class(\\\"Cube\\\", Vector(i * 200, 0, 50), Rotator(0, 0, 0))\"}, "
                    "{\"language\": \"real\", \"code\": \"print(\\\"hi\\\")\"}]",
      "give the body only (it is wrapped in a class with a Run event); errors come back with the line in your code and a help_doc; not while playing",
      "api_reference, write_script" },
    { "find_actors", "[{\"query\": \"Light\"}, {\"component\": \"Static Mesh\", \"near\": [0, 0, 0], \"radius\": 1000}, {\"selected_only\": true}]",
      "paged by limit/offset; total says how many matched", "get_actor, search" },
    { "set_camera", "[{\"location\": [-800, 0, 400], \"look_at\": [0, 0, 100]}, {\"focus_actor\": \"Hero\", \"distance\": 600}]",
      "rotation is [pitch, yaw]", "screenshot" },
    { "play", "[{\"action\": \"start\", \"seconds\": 3}, {\"action\": \"status\"}, {\"action\": \"stop\"}]",
      "with seconds it stops by itself and the level comes back as it was", "get_log" },
    { "level", "[{\"action\": \"save\"}, {\"action\": \"save\", \"path\": \"/Content/Maps/Arena.rlevel\"}, {\"action\": \"open\", \"path\": \"/Content/Maps/Arena.rlevel\"}]",
      "a new level needs a path", "inspect_level" },
    { "write_script", "[{\"path\": \"/Content/Scripts/Door.py\", \"code\": \"from realengine import *\\n\\nclass Door(Actor):\\n    def BeginPlay(self):\\n        print(\\\"door ready\\\")\\n\"}]",
      "the class name must be the file's; errors come back with lines", "spawn_actors (asset = the path), api_reference" },
    { "search", "[{\"query\": \"point light\"}, {\"query\": \"health\"}, {\"query\": \"component\"}]", "one word works best", "search_docs, api_reference, list_assets" },
};

static void t_tool_help(McpCall* c) {
    JsonWriter* jw = &c->body;
    const char* tool = json_get_str(c->args, "tool", "");
    jw_obj_begin(jw);
    if (!tool[0]) {
        jw_key(jw, "tools");
        jw_obj_begin(jw);
        for (int i = 0; i < NTOOLS; i++) {
            char line[200];
            const char* d = k_tools[i].desc;
            const char* dot = strstr(d, ". ");
            snprintf(line, sizeof line, "%.*s", dot ? (int)(dot - d) : 180, d);
            jw_kstr(jw, k_tools[i].name, line);
        }
        jw_obj_end(jw);
        jw_kstr(jw, "start", "get_started, then recipe or batch for whole tasks; inspect_level for what is missing.");
        jw_obj_end(jw);
        return;
    }
    const ToolDef* d = tool_def(tool);
    if (!d) {
        jw_obj_end(jw);
        fail(c, "No tool '%s'. tool_help without a tool lists them all.", tool);
        return;
    }
    jw_kstr(jw, "tool", d->name);
    jw_kstr(jw, "description", d->desc);
    jw_key(jw, "schema");
    jw_raw(jw, d->schema);
    for (size_t i = 0; i < sizeof k_help / sizeof k_help[0]; i++) {
        if (strcmp(k_help[i].tool, tool)) continue;
        jw_key(jw, "examples");
        jw_raw(jw, k_help[i].examples);
        jw_kstr(jw, "common_errors", k_help[i].errors);
        jw_kstr(jw, "related", k_help[i].related);
    }
    jw_kbool(jw, "read_only", d->read_only);
    if (batchable(tool)) jw_kbool(jw, "batchable", true);
    jw_obj_end(jw);
}

// --- search: everything in one call ---
static void t_search(McpCall* c, Editor* ed) {
    const char* q = json_get_str(c->args, "query", "");
    int lim = json_get_int(c->args, "limit", 5);
    lim = lim < 1 ? 1 : lim > 30 ? 30 : lim;
    JsonWriter* jw = &c->body;
    World* w = &ed->world;
    int total = 0, n;
    bool all_comps = !_stricmp(q, "component") || !_stricmp(q, "components");
    jw_obj_begin(jw);
    jw_kstr(jw, "query", q);
    // actor types (spawn_actors type)
    jw_key(jw, "actor_types"), jw_arr_begin(jw), n = 0;
    for (int i = 0; i < editor_place_count() && n < lim; i++) {
        const char* cat = NULL;
        const char* t = editor_place_name(i, &cat);
        if (icontains(t, q)) jw_str(jw, t), n++;
        else if (icontains(cat, q)) { // (2026-11-25) found by its group: named with it ("Procedural World: Facility"; spawn_actors takes it)
            char full[200];
            snprintf(full, sizeof full, "%s: %s", cat, t);
            jw_str(jw, full), n++;
        }
    }
    jw_arr_end(jw), total += n;
    // components (spawn_actors / modify_actors components)
    jw_key(jw, "components"), jw_arr_begin(jw), n = 0;
    for (int k = 0; k < COMP_COUNT && (n < lim || all_comps); k++)
        if (g_components[k].user_addable && (all_comps || icontains(g_components[k].name, q))) jw_str(jw, g_components[k].name), n++;
    jw_arr_end(jw), total += n;
    // recipes
    jw_key(jw, "recipes"), jw_arr_begin(jw), n = 0;
    for (int i = 0; i < NRECIPES; i++)
        if (icontains(k_recipes[i].name, q) || icontains(k_recipes[i].desc, q)) jw_str(jw, k_recipes[i].name), n++;
    jw_arr_end(jw), total += n;
    // the level's actors
    jw_key(jw, "actors"), jw_arr_begin(jw), n = 0;
    for (uint32_t i = 1; i < w->slot_count && n < lim; i++) {
        Entity e = ecs_entity_at(w, i);
        if (e && (icontains(actor_name(w, e), q) || icontains(actor_class(w, e), q))) actor_brief(jw, w, e), n++;
    }
    jw_arr_end(jw), total += n;
    // assets
    static char found[4096][512];
    int na = content_search(q, found, 4096);
    jw_key(jw, "assets"), jw_arr_begin(jw), n = 0;
    for (int i = 0; i < na && n < lim; i++) {
        AssetKind k = content_kind_of(found[i]);
        if (k == ASSET_FOLDER || strstr(found[i], "/__MCP__/")) continue;
        jw_obj_begin(jw), jw_kstr(jw, "path", found[i]), jw_kstr(jw, "kind", asset_kind_name(k)), jw_obj_end(jw), n++;
    }
    jw_arr_end(jw), total += n;
    // API functions (the name's matches)
    jw_key(jw, "functions"), jw_arr_begin(jw), n = 0;
    for (uint32_t i = 0; i < script_api_count() && n < lim; i++) {
        const ScriptApi* a = script_api_at(i);
        if (icontains(a->name, q)) jw_str(jw, a->sig), n++;
    }
    jw_arr_end(jw), total += n;
    // the knowledge base's articles
    AsHit hits[30];
    bool weak = false;
    int nh = assistant_search(q, hits, lim, &weak);
    jw_key(jw, "docs"), jw_arr_begin(jw), n = 0;
    for (int i = 0; i < nh; i++) {
        const AsDoc* d = assistant_doc(hits[i].doc);
        if (!d || d->src != AS_SRC_ARTICLE) continue;
        jw_obj_begin(jw), jw_kstr(jw, "id", d->id), jw_kstr(jw, "title", d->title[0] ? d->title[0] : d->id), jw_obj_end(jw), n++;
    }
    jw_arr_end(jw), total += n;
    if (!total) jw_kstr(jw, "hint", "Nothing matched: try one shorter word (light, door, health, material) or Portuguese");
    jw_obj_end(jw);
}
