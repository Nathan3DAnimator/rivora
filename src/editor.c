// The editor's panels and shortcuts. One full-window host holds the toolbar, the viewport, the Outliner over the
// Details (resizable by splitters) and the status bar; the Content Browser is a drawer over the viewport (Ctrl+Space)
// or docked under it; the viewport's toolbar floats on it. Undo/Redo keeps whole-world blobs (cheap for the editor's
// worlds: a few KB per step), levels are the same blob in a file.
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <GLFW/glfw3.h>

#include "blueprint.h"
#include "crashlog.h"
#include "updater.h"
#include "endpoints.h"
#include "rig_detect.h" // (2026-11-23) --rig-fit-test
#include "ikrig.h"
#include "level_preload.h" // (2026-11-23)
#include "tess.h"
#include "theme.h"
#include "editor_dock.h"
#include "bp_debug.h"
#include "bp_internal.h"
#include "physics.h"
#include "phys_test.h"
#include "terror_test.h"
#include "pcg.h"
#include "pcg_test.h"
#include "udim_test.h" // (Q18)
#include "emissive_test.h" // (2026-11-23) emissive materials lighting
#include "farview_test.h" // (Q21)
#include "ai_senses_test.h" // (Q24)
#include "physics_shapes.h"
#include "post.h"
#include "terror.h"
#include "audio_internal.h" // (Q13: the Light Flicker's sound picker)
#include "anim.h"
#include "particles.h"
#include "decal.h"
#include "ikrig.h"
#include "vehicle.h"
#include "cloth.h"
#include "cloth_mesh.h"
#include "softbody.h"
#include "groom.h"
#include "fire.h"
#include "splat.h"
#include "delight.h"
#include "volume.h"
#include "world_partition.h"
#include "rpak.h"
#include "mesh_build.h"
#include "procgen.h"
#include "vehicle_editor.h"
#include "dialogue.h"
#include "quest.h"
#include "physmat.h"
#include "toon_actor.h" // (Q11) the actor's outline, the light's toon
#include "camshake.h"
#include "fpcam.h"
#include "game_rules.h"
#include "ability.h"
#include "ikrig_editor.h"
#include "particle_editor.h"
#include "ai.h"
#include "input.h"
#include "input_editor.h"
#include "project.h"
#include "upscale.h"
#include "bp_editor.h"
#include "gameplay_tags.h"
#include "content.h"
#include "debug_viewer.h"
#include "editor_look.h"
#include "mat_editor.h"
#include "traffic.h"
#include "crowd.h"
#include "ui_widget.h"
#include "landscape.h"
#include "navmesh.h"
#include "health.h"
#include "hud.h"
#include "save_progress.h"
#include "interaction.h"
#include "inventory.h"
#include "foliage.h"
#include "proto.h"
#include "asset_viewer.h"
#include "package_res.h"
#include "anim_editor.h"
#include "ai_editor.h"
#include "audio.h"
#include "audio_editor.h"
#include "water.h"
#include "weather_field.h" // (Q27.6)
#include "city.h"          // (Q27.10)
#include "audio_world.h"
#include "movie_render.h"
#include "import_report.h" // (Q19) Window > Import Report
#include "source_control.h"
#include "trace.h"
#include "plugins.h"
#include "graph_style.h"
#include "script.h"
#include "script_ide.h"
#include "sequencer.h"
#include "sequencer_ui.h"
#include "online.h"
#include "tlend_import.h"
#include "async.h"
#include "asset_picker.h"
#include "async_ui.h"
#include "game_settings.h"
#include "assistant_ui.h"
#include "material.h"
#include "editor.h"
#include "mcp.h"
#include "asset_viewport.h"
#include "sublevels.h"
#include "gi_bake.h"
#include "runtime_lighting.h" // (Q16)
#include "cvars.h"
#include "i18n.h"
#include "lights.h"
#include "heavy_test.h"
#include "platform.h"
#include "platform_files.h"
#include "services.h"
#include "systems.h"
#include "ui_icons.h"
#include "ui_tips.h"
#include "vkctx.h"
#include "spline.h" // (Q14) splines and cables

static bool g_prefs_dirty; // a preference changed: editor.ini is written at the end of the frame
#define ACCENT THVA(TK_ACCENT, 0.85f)
static bool s_drawer_shown; // the Content Drawer was shown last frame

void editor_status(Editor* ed, double now, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ed->status_msg, sizeof ed->status_msg, fmt, ap);
    va_end(ap);
    ed->status_until = now + 6.0;
    if (ed->log_count == RE_LOG_LINES) {
        memmove(ed->log[0], ed->log[1], sizeof ed->log[0] * (RE_LOG_LINES - 1));
        ed->log_count--;
    }
    snprintf(ed->log[ed->log_count++], sizeof ed->log[0], "%s", ed->status_msg);
    printf("[RealEngine] %s\n", ed->status_msg);
    fflush(stdout);
}

static void set_name(World* w, Entity e, const char* name) {
    CName* n = ecs_get(w, e, COMP_NAME);
    if (n) snprintf(n->text, sizeof n->text, "%s", name);
    ecs_touch(w, e); // the Outliner lists it again
}

void editor_unique_name(Editor* ed, const char* base, char* out, size_t cap) {
    World* w = &ed->world;
    size_t n = strlen(base);
    int best = 0;
    bool base_taken = false;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_NAME))) continue;
        const CName* names = ecs_arch_column(a, COMP_NAME);
        for (uint32_t i = 0; i < a->count; i++) {
            const char* s = names[i].text;
            if (strncmp(s, base, n) != 0) continue;
            if (s[n] == 0) base_taken = true;
            else if (isdigit((unsigned char)s[n])) {
                int k = atoi(s + n);
                if (k > best) best = k;
            }
        }
    }
    if (!base_taken) snprintf(out, cap, "%s", base);
    else snprintf(out, cap, "%.*s%d", (int)(cap > 12 ? cap - 12 : 1), base, best < 2 ? 2 : best + 1);
}

// --- Spawning (Unreal's Quick Add / Place Actors) ---

typedef enum SpawnKind { SPAWN_EMPTY, SPAWN_PLAYER_START, SPAWN_SHAPE, SPAWN_LIGHT, SPAWN_CAMERA, SPAWN_FOG, SPAWN_CLOUD, SPAWN_POST, SPAWN_CROWD, SPAWN_LANDSCAPE, SPAWN_PROTO, SPAWN_TRIGGER, SPAWN_FOLIAGE, SPAWN_AMBIENT_SOUND, SPAWN_AUDIO_VOLUME, SPAWN_NAV, SPAWN_PICKUP, SPAWN_CHECKPOINT, SPAWN_DEVICE, SPAWN_GAME_RULES, SPAWN_DECAL, SPAWN_FIRE, SPAWN_RIVER, SPAWN_FOG_VOLUME, SPAWN_SPLAT, SPAWN_VOLUME, SPAWN_WORLD_PARTITION } SpawnKind;

// The defaults of a new Exponential Height Fog (a light haze, volumetric) and Volumetric Cloud (scattered cumulus)
static const CFog g_fog_default = { 0.001f, 0.02f, { 0.92f, 0.94f, 1.0f }, 0.6f, 0.0f, 1.0f, { 0, 0, 0 }, 1, 6000.0f, 1.0f }; // (Q21: the sky atmosphere's ambient, 1)
static const CCloud g_cloud_default = { 0.35f, 0.04f, 150000.0f, 400000.0f, { 8.0f, 3.0f }, { 0.95f, 0.95f, 0.95f }, 0.5f, 0.85f, 0.35f };

static uint32_t content_material(const char* rel);
static Editor* g_pg_editor; // (the procedural world's origin moved: the cameras with it)
static void pg_cameras(const float d[3]) {
    if (!g_pg_editor) return;
    for (int k = 0; k < 3; k++) g_pg_editor->cam.pos[k] += d[k], g_pg_editor->pie_cam.pos[k] += d[k];
}
// Quick Add > Vehicles: the template's asset (Content/Vehicles, made once) as an actor on its wheels in front of the
// camera; a Water Body; a Rail (a loop)
static void add_vehicle(Editor* ed, int tmpl, double now) {
    char name[64], vp[200], dir[700];
    snprintf(name, sizeof name, "VH_%s", g_veh_template_names[tmpl]);
    for (char* c = name; *c; c++)
        if (*c == ' ') *c = '_';
    snprintf(vp, sizeof vp, "/Content/Vehicles/%s.rvehicle", name);
    snprintf(dir, sizeof dir, "%s/Vehicles", content_root());
    re_make_dirs(dir);
    VehicleAsset* a = veh_load(vp);
    uint32_t h = a ? a->hash : veh_create("/Content/Vehicles", name, tmpl);
    a = veh_find(h);
    if (!a) return;
    float cy = cosf(ed->cam.yaw * 0.017453293f), sy = sinf(ed->cam.yaw * 0.017453293f);
    float at[3] = { ed->cam.pos[0] + cy * 1200.0f, ed->cam.pos[1] + sy * 1200.0f, veh_rest_height(a) };
    editor_undo_push(ed);
    char un[RE_NAME_MAX];
    editor_unique_name(ed, name, un, sizeof un);
    Entity e = vehicle_spawn(&ed->world, h, at, ed->cam.yaw, un);
    if (e == ENTITY_NONE) return;
    editor_select(ed, e, SEL_REPLACE);
    ed->dirty = true;
    content_mark_dirty();
    editor_status(ed, now, TR("Vehicle placed: Auto Possess in its Details drives it on Play (W/S, A/D, Space; F opens the doors)."));
}
static void add_water_or_rail(Editor* ed, bool rail, double now) {
    float cy = cosf(ed->cam.yaw * 0.017453293f), sy = sinf(ed->cam.yaw * 0.017453293f);
    float at[3] = { ed->cam.pos[0] + cy * 2000.0f, ed->cam.pos[1] + sy * 2000.0f, 0.0f };
    editor_undo_push(ed);
    Entity e;
    if (rail) {
        const float pts[6][3] = { { -4000, -1500, 0 }, { 0, -2500, 0 }, { 4000, -1500, 0 }, { 4000, 1500, 0 }, { 0, 2500, 0 }, { -4000, 1500, 0 } };
        e = rail_spawn(&ed->world, pts, 6, true, "Rail");
        CTransform* t = e ? ecs_get(&ed->world, e, COMP_TRANSFORM) : NULL;
        if (t) memcpy(t->location, at, 12);
    } else e = water_spawn(&ed->world, at, 6000.0f, 6000.0f);
    if (e == ENTITY_NONE) return;
    char un[RE_NAME_MAX];
    editor_unique_name(ed, rail ? "Rail" : "WaterBody", un, sizeof un);
    set_name(&ed->world, e, un);
    editor_select(ed, e, SEL_REPLACE);
    ed->dirty = true;
    editor_status(ed, now, rail ? TR("Rail placed: its points in its Details; a train's Rail field names it.") : TR("Water Body placed: boats float on it (its size is its scale)."));
}
// Window > Cloth Simulation (cloth.h): every Cloth of the level (select one: its Details), the simulation's cost, start
// them over, cut the selected one in its middle (Tear Cloth's test), its presets
static bool g_show_cine; // (B9) Window > Cinematics
static bool g_show_submit; // (B14) File > Source Control > Submit Content
static bool g_show_trace;  // (B14b) Tools > Trace (Insights)
static bool g_show_plugins; // (B14c) Edit > Plugins
void editor_show_plugins(void) { g_show_plugins = true; }
static Editor* g_plugins_ed;
static void plugins_status_cb(const char* text) { // (a plugin's status text on the editor's bar)
    if (g_plugins_ed) editor_status(g_plugins_ed, re_time_seconds(), "%s", text);
}
void editor_show_trace(void) { g_show_trace = true; }
static void cloth_panel(Editor* ed) {
    if (!ed->show_cloth) return;
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 26, igGetFontSize() * 24 }, ImGuiCond_FirstUseEver);
    if (igBegin(TR("Cloth Simulation"), &ed->show_cloth, 0)) {
        const ClothStats* st = cloth_stats();
        igText(TR("%u cloths, %u vertices, %u colliders; %u GPU passes a frame"), st->cloths, st->vertices, st->colliders, st->dispatches);
        igTextDisabled("%s", TR("Vertex Block Descent on the GPU; contacts never closer than each cloth's Thickness."));
        World* w = &ed->world;
        if (igButton(TR("Restart All"), (ImVec2){ 0, 0 }))
            for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
                EcsArchetype* a = &w->archetypes[ai];
                if (!(a->mask & COMP_BIT(COMP_CLOTH))) continue;
                CCloth* c = ecs_arch_column(a, COMP_CLOTH);
                for (uint32_t i = 0; i < a->count; i++) c[i].reset = 1;
            }
        igSameLine(0, 6);
        const CTransform* st_t = ed->selected ? ecs_get(w, ed->selected, COMP_TRANSFORM) : NULL;
        const CCloth* sc = ed->selected ? ecs_get(w, ed->selected, COMP_CLOTH) : NULL;
        if (igButton(TR("Tear Selected"), (ImVec2){ 0, 0 }) && sc && st_t) { // (its middle: a hole a fifth of its width)
            float c[3] = { st_t->location[0], st_t->location[1], st_t->location[2] - sc->height * 0.5f };
            cloth_tear(c, sc->width * 0.2f);
        }
        igSeparatorText(TR("Cloths"));
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            EcsArchetype* a = &w->archetypes[ai];
            if (!(a->mask & COMP_BIT(COMP_CLOTH)) || !(a->mask & COMP_BIT(COMP_NAME))) continue;
            const Entity* ents = ecs_arch_entities(a);
            CCloth* c = ecs_arch_column(a, COMP_CLOTH);
            CName* nm = ecs_arch_column(a, COMP_NAME);
            for (uint32_t i = 0; i < a->count; i++) {
                char label[96];
                if (c[i].kind == CLOTH_KIND_SOFT) // (Q9) a soft body: its preset and its lattice's cells
                    snprintf(label, sizeof label, "%s  (%s: %s, %u)##cl%u", nm[i].text, TR("Soft Body"), TR(g_soft_preset_names[c[i].soft_preset < SOFT_PRESET_COUNT ? c[i].soft_preset : 0]), c[i].soft_res, entity_index(ents[i]));
                else
                    snprintf(label, sizeof label, "%s  (%s, %ux%u)##cl%u", nm[i].text, TR(g_cloth_preset_names[c[i].preset < CLOTH_PRESET_COUNT ? c[i].preset : 0]), c[i].res_x, c[i].res_y, entity_index(ents[i]));
                if (igSelectable_Bool(label, ed->selected == ents[i], 0, (ImVec2){ 0, 0 })) editor_select(ed, ents[i], SEL_REPLACE);
            }
        }
        if (sc) {
            igSeparatorText(TR("Selected"));
            igTextWrapped(TR("Preset, pins, stiffness, wind, tearing and the bone it follows: its Details (Cloth)."));
            // (Q7) its model in the asset viewport at rest: its own mesh, else the sheet (width x height, hanging)
            static AssetViewport vp;
            static Entity vp_of;
            if (vp_of != ed->selected) vp.cam.framed = false, vp_of = ed->selected;
            PreviewDraw d;
            avp_mesh_draw(&d, sc->mesh ? sc->mesh : SHAPE_PLANE, 0, sc->color, sc->roughness, 0.0f);
            if (sc->mesh) {
                for (int k = 0; k < 3; k++) d.rows[k][k] = sc->mesh_scale[k] != 0.0f ? sc->mesh_scale[k] : 1.0f;
            } else { // (the plane lies on XY, 100 cm: turned up, X across, Z down from its top)
                memset(d.rows, 0, sizeof d.rows);
                d.rows[0][0] = sc->width / 100.0f, d.rows[2][1] = sc->height / 100.0f, d.rows[1][2] = 1.0f;
                d.rows[2][3] = sc->height * 0.5f;
            }
            AvpScene scn;
            memset(&scn, 0, sizeof scn);
            scn.draws = &d, scn.ndraws = 1, scn.sel_bone = scn.sel_body = scn.sel_constraint = -1;
            scn.hint = TR("Its rest shape; it simulates in the level. Left drag: orbit, wheel: zoom.");
            ImVec2 o = igGetCursorScreenPos(), av = igGetContentRegionAvail();
            float vh = fmaxf(av.y - 4.0f, igGetFontSize() * 12.0f);
            avp_draw(&vp, BP_PV_KEY_CLOTH, o.x, o.y, av.x, vh, &scn, NULL);
            igSetCursorScreenPos((ImVec2){ o.x, o.y + vh });
            igDummy((ImVec2){ 0, 0 });
        }
    }
    igEnd();
}
// Quick Add > Cloth (cloth.h): a sheet of a preset hanging in front of the camera
static void add_cloth(Editor* ed, int preset, double now) {
    editor_undo_push(ed);
    Entity e = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CLOTH));
    if (e == ENTITY_NONE) return;
    CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    float cy = cosf(ed->cam.yaw * 0.017453293f), sy = sinf(ed->cam.yaw * 0.017453293f);
    t->location[0] = ed->cam.pos[0] + cy * 500.0f, t->location[1] = ed->cam.pos[1] + sy * 500.0f, t->location[2] = ed->cam.pos[2] + 120.0f;
    t->rotation[1] = ed->cam.yaw + 90.0f, t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE;
    cloth_apply_preset(ecs_get(&ed->world, e, COMP_CLOTH), preset);
    char un[RE_NAME_MAX];
    editor_unique_name(ed, g_cloth_preset_names[preset], un, sizeof un);
    set_name(&ed->world, e, un);
    editor_select(ed, e, SEL_REPLACE);
    ed->dirty = true;
    editor_status(ed, now, TR("Cloth placed: it simulates at once; its preset, pins, wind and tearing in its Details (Window > Cloth Simulation)."));
}
// Quick Add > Rain (water.h): one per level (only the first rains); and an Ocean: an endless Water Body at z 0
static void add_rain(Editor* ed, double now) {
    editor_undo_push(ed);
    Entity e = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_RAIN));
    if (e == ENTITY_NONE) return;
    CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    memcpy(t->location, ed->cam.pos, 12), t->location[2] += 300.0f, t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    char un[RE_NAME_MAX];
    editor_unique_name(ed, "Rain", un, sizeof un);
    set_name(&ed->world, e, un);
    editor_select(ed, e, SEL_REPLACE);
    ed->dirty = true;
    editor_status(ed, now, TR("Rain placed: it rains around the camera; the surfaces it reaches get wet, puddles form, water runs downhill (its Details)."));
}
static Entity spawn_actor(Editor* ed, SpawnKind kind, int sub, const float at[3], double now);
Entity editor_spawn_camera(Editor* ed, const float location[3], float pitch, float yaw, double now) {
    editor_undo_push(ed);
    Entity e = spawn_actor(ed, SPAWN_CAMERA, 0, location, now);
    CTransform* t = e ? ecs_get(&ed->world, e, COMP_TRANSFORM) : NULL;
    if (t) memcpy(t->location, location, 12), t->rotation[0] = pitch, t->rotation[1] = yaw, t->rotation[2] = 0.0f, ecs_touch(&ed->world, e);
    return e;
}
static void add_ocean(Editor* ed, double now) {
    editor_undo_push(ed);
    Entity e = water_spawn(&ed->world, (float[3]){ ed->cam.pos[0], ed->cam.pos[1], 0.0f }, 200000.0f, 200000.0f);
    if (e == ENTITY_NONE) return;
    ((CWater*)ecs_get(&ed->world, e, COMP_WATER))->ocean = 1;
    char un[RE_NAME_MAX];
    editor_unique_name(ed, "Ocean", un, sizeof un);
    set_name(&ed->world, e, un);
    editor_select(ed, e, SEL_REPLACE);
    ed->dirty = true;
    editor_status(ed, now, TR("Ocean placed: endless waves from its wind (its Details); boats float on its big waves."));
}
// (2026-11-10) Quick Add > World > Water Body Exclusion: a 20 x 20 m hole in the ocean where the camera looks (land, a dock)
static void add_water_exclusion(Editor* ed, double now) {
    editor_undo_push(ed);
    Entity e = water_spawn(&ed->world, (float[3]){ ed->cam.pos[0], ed->cam.pos[1], 0.0f }, 2000.0f, 2000.0f);
    if (e == ENTITY_NONE) return;
    ((CWater*)ecs_get(&ed->world, e, COMP_WATER))->exclusion = 1;
    char un[RE_NAME_MAX];
    editor_unique_name(ed, "WaterBodyExclusion", un, sizeof un);
    set_name(&ed->world, e, un);
    editor_select(ed, e, SEL_REPLACE);
    ed->dirty = true;
    editor_status(ed, now, TR("Water Body Exclusion placed: no ocean inside its rectangle (scale it; turn it by its yaw)."));
}
// Quick Add > Visual Effects > Particle System: the template's asset (Content/Particles, made once) played by an actor in
// front of the camera
static void add_particles(Editor* ed, int tmpl, double now) {
    char name[64], vp[200], dir[700];
    snprintf(name, sizeof name, "PS_%s", g_ps_template_names[tmpl]);
    for (char* c = name; *c; c++)
        if (*c == ' ' || *c == '(' || *c == ')') *c = '_';
    snprintf(vp, sizeof vp, "/Content/Particles/%s.rparticle", name);
    snprintf(dir, sizeof dir, "%s/Particles", content_root());
    re_make_dirs(dir);
    ParticleSystemAsset* a = ps_load(vp);
    uint32_t h = a ? a->hash : ps_create("/Content/Particles", name, tmpl);
    if (!h) return;
    float cy = cosf(ed->cam.yaw * 0.017453293f), sy = sinf(ed->cam.yaw * 0.017453293f);
    float at[3] = { ed->cam.pos[0] + cy * 800.0f, ed->cam.pos[1] + sy * 800.0f, 0.0f };
    editor_undo_push(ed);
    Entity e = particles_spawn(&ed->world, h, at, NULL, false);
    if (e == ENTITY_NONE) return;
    char un[RE_NAME_MAX];
    editor_unique_name(ed, name, un, sizeof un);
    set_name(&ed->world, e, un);
    editor_select(ed, e, SEL_REPLACE);
    ed->dirty = true;
    content_mark_dirty();
    editor_status(ed, now, TR("Particle System placed: double-click its asset to edit its emitters."));
}

// (A9.5e) Tools > De-light: the selected mesh actor's material's Base Color image (an Image Texture into the Principled's
// Base Color) with its mesh's normals: the light of the scan taken out into <image>_delit.png, the node pointed at it
static void delight_selected(Editor* ed, double now) {
    World* w = &ed->world;
    const CStaticMesh* sm = ecs_get(w, ed->selected, COMP_STATIC_MESH);
    Material* m = sm && sm->material ? mat_find_hash(sm->material) : NULL;
    if (m && m->instance) m = m->parent;
    if (!m || !m->graph) {
        editor_status(ed, now, "%s", TR("De-light: select a mesh actor whose material has an Image Texture in its Base Color."));
        return;
    }
    MatGraph* g = m->graph;
    int img = -1;
    for (int i = 0; i < g->node_count && img < 0; i++) {
        if (g->nodes[i].type != NODE_BSDF_PRINCIPLED) continue;
        const MatNodeDef* d = mat_node_def(NODE_BSDF_PRINCIPLED);
        for (int k = 0; d && k < d->nin; k++)
            if (!strcmp(d->in[k].name, "Base Color")) {
                int li = mat_graph_input_link(g, i, k);
                if (li >= 0 && g->nodes[g->links[li].from].type == NODE_TEX_IMAGE) img = g->links[li].from;
            }
    }
    for (int i = 0; i < g->node_count && img < 0; i++) // (else the first image)
        if (g->nodes[i].type == NODE_TEX_IMAGE && g->nodes[i].image[0]) img = i;
    if (img < 0 || !g->nodes[img].image[0]) {
        editor_status(ed, now, "%s", TR("De-light: select a mesh actor whose material has an Image Texture in its Base Color."));
        return;
    }
    char src[1200], mesh[1200], out[1200], vout[MAT_PATH];
    const char* ip = g->nodes[img].image;
    if (!strncmp(ip, "/Content", 8)) snprintf(src, sizeof src, "%s%s", content_root(), ip + 8);
    else snprintf(src, sizeof src, "%s", ip);
    const char* mv = render_mesh_vpath(render_mesh_slot(sm->mesh));
    if (mv[0] && !strncmp(mv, "/Content", 8)) snprintf(mesh, sizeof mesh, "%s%s", content_root(), mv + 8);
    else mesh[0] = 0;
    snprintf(out, sizeof out, "%s", src);
    char* dot = strrchr(out, '.');
    if (dot && !strchr(dot, '/') && !strchr(dot, '\\')) *dot = 0;
    if (strlen(out) > 6 && !strcmp(out + strlen(out) - 6, "_delit")) out[strlen(out) - 6] = 0; // (again: from the de-lit one)
    strncat(out, "_delit.png", sizeof out - strlen(out) - 1);
    DelightStats st = { 0 };
    if (!delight_file(src, mesh, out, 1.0f, &st)) {
        editor_status(ed, now, "%s", TR("De-light: the texture could not be read or written."));
        return;
    }
    snprintf(vout, sizeof vout, "%s", ip);
    dot = strrchr(vout, '.');
    if (dot) *dot = 0;
    if (strlen(vout) > 6 && !strcmp(vout + strlen(vout) - 6, "_delit")) vout[strlen(vout) - 6] = 0;
    strncat(vout, "_delit.png", sizeof vout - strlen(vout) - 1);
    snprintf(g->nodes[img].image, sizeof g->nodes[img].image, "%s", vout);
    m->dirty = true;
    mat_save(m);
    content_mark_dirty();
    editor_status(ed, now, TR("De-light: %s (the light's spread %.2f -> %.2f, %s the mesh's normals, %.0f ms)."), vout, st.before, st.after,
                  mesh[0] ? TR("with") : TR("without"), st.ms);
}

static Entity spawn_actor(Editor* ed, SpawnKind kind, int sub, const float at[3], double now) {
    uint64_t mask = COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM);
    const char* base = "Actor";
    switch (kind) {
    case SPAWN_EMPTY: break;
    case SPAWN_PLAYER_START: mask |= COMP_BIT(COMP_PLAYER_START); base = "PlayerStart"; break;
    case SPAWN_SHAPE: mask |= COMP_BIT(COMP_STATIC_MESH); base = g_shape_names[sub]; break;
    case SPAWN_LIGHT: mask |= COMP_BIT(COMP_LIGHT); base = NULL; break;
    case SPAWN_CAMERA: mask |= COMP_BIT(COMP_CAMERA); base = "CameraActor"; break;
    case SPAWN_FOG: mask |= COMP_BIT(COMP_FOG); base = "ExponentialHeightFog"; break;
    case SPAWN_CLOUD: mask |= COMP_BIT(COMP_CLOUD); base = "VolumetricCloud"; break;
    case SPAWN_POST: mask |= COMP_BIT(COMP_POST_PROCESS); base = "PostProcessVolume"; break;
    case SPAWN_CROWD: mask |= COMP_BIT(COMP_CROWD); base = "CrowdSpawner"; break;
    case SPAWN_LANDSCAPE: mask |= COMP_BIT(COMP_LANDSCAPE); base = "Landscape"; break;
    case SPAWN_FOLIAGE: mask |= COMP_BIT(COMP_FOLIAGE); base = "Foliage"; break;
    case SPAWN_AMBIENT_SOUND: mask |= COMP_BIT(COMP_AUDIO); base = "AmbientSound"; break;
    case SPAWN_AUDIO_VOLUME: mask |= COMP_BIT(COMP_AUDIO_VOLUME); base = "AudioVolume"; break;
    case SPAWN_DECAL: mask |= COMP_BIT(COMP_DECAL); base = "Decal"; break;
    case SPAWN_FIRE: mask |= COMP_BIT(COMP_FIRE) | COMP_BIT(COMP_LIGHT); base = "Fire"; break; // (A14: its light flickers with it)
    case SPAWN_RIVER: mask |= COMP_BIT(COMP_WATER) | COMP_BIT(COMP_RIVER); base = "WaterBodyRiver"; break; // (B12c: Unreal's Water Body River)
    case SPAWN_FOG_VOLUME: mask |= COMP_BIT(COMP_FOG_VOLUME); base = "LocalFogVolume"; break; // (A9.5b: Unreal's Local Fog Volume)
    case SPAWN_SPLAT: mask |= COMP_BIT(COMP_SPLAT); base = "GaussianSplat"; break; // (A9.5d)
    case SPAWN_VOLUME: mask |= COMP_BIT(COMP_VOLUME); base = "HeterogeneousVolume"; break; // (A9.5a)
    case SPAWN_WORLD_PARTITION: mask |= COMP_BIT(COMP_WORLD_PARTITION); base = "WorldPartition"; break; // (B1: one a level)
    case SPAWN_NAV: mask |= COMP_BIT(COMP_NAV); base = sub == NAV_LINK_PROXY ? "NavLinkProxy" : sub == NAV_MODIFIER_VOLUME ? "NavModifierVolume" : "NavMeshBoundsVolume"; break;
    case SPAWN_PROTO: mask |= COMP_BIT(COMP_PROTO); base = g_proto_kind_names[sub % PROTO_KIND_COUNT]; break;
    case SPAWN_TRIGGER: mask |= COMP_BIT(COMP_COLLISION); base = sub == 1 ? "TriggerSphere" : sub == 2 ? "TriggerCapsule" : "TriggerBox"; break;
    case SPAWN_PICKUP: mask |= COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_PICKUP); base = "Pickup"; break;
    case SPAWN_CHECKPOINT: mask |= COMP_BIT(COMP_CHECKPOINT); base = "Checkpoint"; break;
    case SPAWN_GAME_RULES: mask |= COMP_BIT(COMP_GAME_RULES); base = "GameRules"; break; // (game_rules.h: the match's rules)
    case SPAWN_DEVICE: mask |= COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_INTERACTABLE) | COMP_BIT(COMP_DEVICE); base = g_device_names[sub % DEV_KIND_COUNT]; break;
    }
    static const char* const light_base[LIGHT_TYPE_COUNT] = { "DirectionalLight", "PointLight", "SpotLight", "RectLight", "SkyLight" };
    if (kind == SPAWN_LIGHT) base = light_base[sub];
    Entity e = ecs_create(&ed->world, mask);
    if (e == ENTITY_NONE) {
        editor_status(ed, now, TR("Could not create the actor (the world is full)."));
        return e;
    }
    char name[RE_NAME_MAX];
    editor_unique_name(ed, base, name, sizeof name);
    set_name(&ed->world, e, name);
    CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    memcpy(t->location, at, sizeof t->location);
    if (kind == SPAWN_SHAPE) { // with the grid material (Unreal's prototyping look: its size reads at a glance), not plain white
        CStaticMesh* sm = ecs_get(&ed->world, e, COMP_STATIC_MESH);
        sm->mesh = (uint32_t)sub;
        sm->material = content_material("M_Grid.rmat");
    } else if (kind == SPAWN_LIGHT) {
        CLight* l = ecs_get(&ed->world, e, COMP_LIGHT);
        l->type = (uint32_t)sub;
        switch (sub) {
        // physical units: the sun's lux above the atmosphere (~100 000 reach the ground at noon); candela
        case LIGHT_DIRECTIONAL: l->intensity = 120000.0f; t->rotation[0] = -45.0f; t->rotation[1] = 30.0f; break;
        case LIGHT_SPOT: l->intensity = 3000.0f; t->rotation[0] = -90.0f; break; // pointing down
        case LIGHT_RECT: l->intensity = 1500.0f; t->rotation[0] = -90.0f; l->cast_shadows = 0; break;
        case LIGHT_SKY: l->intensity = 1.0f; break;
        default: break;
        }
        t->flags = MOBILITY_STATIONARY;
    } else if (kind == SPAWN_FOG) {
        *(CFog*)ecs_get(&ed->world, e, COMP_FOG) = g_fog_default;
        t->location[2] = 0.0f; // the fog's base height: the ground
    } else if (kind == SPAWN_CLOUD) {
        *(CCloud*)ecs_get(&ed->world, e, COMP_CLOUD) = g_cloud_default;
    } else if (kind == SPAWN_PROTO) { // on the floor, static; each kind's own proportions
        CProto* p = ecs_get(&ed->world, e, COMP_PROTO);
        p->kind = (uint32_t)sub;
        t->location[2] = 0.0f, t->flags = MOBILITY_STATIC;
        static const float sz[PROTO_KIND_COUNT][3] = { { 200, 200, 200 }, { 400, 200, 240 }, { 600, 200, 150 }, { 600, 20, 300 },
                                                       { 800, 600, 300 }, { 1200, 300, 300 }, { 400, 400, 240 } };
        memcpy(p->size, sz[sub % PROTO_KIND_COUNT], 12);
    } else if (kind == SPAWN_LANDSCAPE) { // on the ground's level, static (its lightmaps can be baked)
        t->location[2] = 0.0f, t->flags = MOBILITY_STATIC;
    } else if (kind == SPAWN_FOLIAGE) { // (its instances are where they were painted: the actor itself stays at the origin)
        memset(t->location, 0, sizeof t->location), t->flags = MOBILITY_STATIC;
    } else if (kind == SPAWN_TRIGGER) { // Unreal's Trigger Box / Sphere / Capsule: it only overlaps (Generate Overlap Events, no blocking)
        CCollision* c = ecs_get(&ed->world, e, COMP_COLLISION);
        c->shape = sub == 1 ? COLLISION_SPHERE : sub == 2 ? COLLISION_CAPSULE : COLLISION_BOX;
        c->extent[0] = sub == 2 ? 40.0f : 50.0f, c->extent[1] = sub == 2 ? 90.0f : 50.0f, c->extent[2] = 50.0f;
        c->generate_overlap_events = 1, c->block = 0;
        t->location[2] += sub == 2 ? 90.0f : 50.0f;
    } else if (kind == SPAWN_AUDIO_VOLUME) { // a 4 m box (a room's), its reverb on
        for (int k = 0; k < 3; k++) t->scale[k] = 2.0f;
    } else if (kind == SPAWN_WORLD_PARTITION) { // (B1) at the origin: only its settings matter
        memset(t->location, 0, sizeof t->location), t->flags = MOBILITY_STATIC;
    } else if (kind == SPAWN_VOLUME) { // (A9.5a) the example puff until a file is chosen
        if (volume_examples()) content_mark_dirty();
        CVolume* vo = ecs_get(&ed->world, e, COMP_VOLUME);
        snprintf(vo->path, sizeof vo->path, "Volumes/Example_Puff_0001.vdb");
        vo->frames = 24;
    } else if (kind == SPAWN_SPLAT) { // (A9.5d) the example bush until a file is chosen
        if (splat_examples()) content_mark_dirty();
        CSplat* sp = ecs_get(&ed->world, e, COMP_SPLAT);
        snprintf(sp->path, sizeof sp->path, "Splats/Example_Bush2.splat");
        sp->up_axis = 0;
    } else if (kind == SPAWN_FOG_VOLUME) { // (A9.5b) 10 m across, its bottom on the floor
        for (int k = 0; k < 3; k++) t->scale[k] = 10.0f;
        t->location[2] += 400.0f;
    } else if (kind == SPAWN_FIRE) { // (A14) a campfire: its light a point light in its flame, casting shadows; its box's
        // base (a fifth of its height below the actor) on the floor
        t->location[2] += 40.0f;
        CLight* l = ecs_get(&ed->world, e, COMP_LIGHT);
        l->type = LIGHT_POINT, l->source_radius = 20.0f, l->cast_shadows = 1, l->color[0] = l->color[1] = l->color[2] = 1.0f;
        l->intensity = 4000.0f, l->attenuation_radius = 1200.0f, l->use_temperature = 1, l->temperature = 1800.0f;
    } else if (kind == SPAWN_DECAL) { // Unreal's: pointing down at the floor (its X down), 1 m deep, 2 x 2 m, the grime example
        t->rotation[0] = -90.0f, t->scale[0] = 0.5f, t->scale[1] = t->scale[2] = 1.0f;
        CDecal* d = ecs_get(&ed->world, e, COMP_DECAL);
        d->material = content_material("Examples/M_DecalGrime.rmat");
    } else if (kind == SPAWN_NAV) { // Unreal's: a bounds volume over the floor around it (20 x 20 x 4 m), a 4 m link, a 2 m modifier
        CNav* nv = ecs_get(&ed->world, e, COMP_NAV);
        nv->kind = (uint32_t)(sub < NAV_KIND_COUNT ? sub : 0);
        if (sub == NAV_BOUNDS_VOLUME) t->scale[0] = t->scale[1] = 10.0f, t->scale[2] = 2.0f, t->location[2] += 100.0f;
        else if (sub == NAV_LINK_PROXY) nv->left[0] = -200.0f, nv->right[0] = 200.0f, t->location[2] += 10.0f;
        else t->location[2] += 100.0f;
        navmesh_set_wanted(true);
    } else if (kind == SPAWN_PICKUP) { // an item in the world (inventory.h): the project's first Item Definition, walked through
        CPickup* pk = ecs_get(&ed->world, e, COMP_PICKUP);
        CStaticMesh* sm = ecs_get(&ed->world, e, COMP_STATIC_MESH);
        inv_items_refresh();
        const ItemDef* d = inv_item_at(0);
        if (d) pk->item = d->hash, sm->mesh = d->mesh, memcpy(sm->base_color, d->color, 12);
        float s = d ? d->attach_scale : 0.25f;
        t->scale[0] = t->scale[1] = t->scale[2] = s;
        t->location[2] = 40.0f, t->flags = MOBILITY_MOVABLE | TRANSFORM_NO_COLLISION;
    } else if (kind == SPAWN_GAME_RULES) { // the match's rules (game_rules.h): its example widgets into Content/UI when missing
        t->location[2] = 120.0f;
        rules_hud_asset(), rules_end_asset();
    } else if (kind == SPAWN_CHECKPOINT) { // a volume on the floor (save_progress.h): its box's bottom on the ground, the player back at its feet
        CCheckpoint* ck = ecs_get(&ed->world, e, COMP_CHECKPOINT);
        t->location[2] += ck ? ck->extent[2] : 100.0f;
    } else if (kind == SPAWN_DEVICE) { // a Door, Chest, Lever or Button (interaction.h): its mesh, size, motion; a chest's lid
        interaction_setup_device(&ed->world, e, sub);
        interaction_prompt_asset(); // (the example widget into Content/UI when missing)
    } else if (kind == SPAWN_AMBIENT_SOUND) { // Unreal's Ambient Sound: an Audio Component that loops what it is given
        t->location[2] += 100.0f;
    } else if (kind == SPAWN_POST) { // Unreal's: a 4 m box, nothing overridden yet (Infinite Extent off: it acts inside)
        CPostProcess* p = ecs_get(&ed->world, e, COMP_POST_PROCESS);
        post_volume_defaults(p);
        p->flags = PPV_ENABLED;
        for (int k = 0; k < 3; k++) t->scale[k] = 2.0f;
    }
    ed->selected = e;
    return e;
}

Entity editor_spawn_shape(Editor* ed, int shape, const float location[3], double now) {
    if (shape < 0 || shape >= SHAPE_COUNT) return ENTITY_NONE;
    return spawn_actor(ed, SPAWN_SHAPE, shape, location, now);
}

// 4 m in front of the camera, on the ground (lights and cameras a little above it)
static void place_in_front(Editor* ed, SpawnKind kind, float at[3]) {
    float yaw = ed->cam.yaw * 0.017453292f;
    at[0] = roundf((ed->cam.pos[0] + cosf(yaw) * 400.0f) / 10.0f) * 10.0f;
    at[1] = roundf((ed->cam.pos[1] + sinf(yaw) * 400.0f) / 10.0f) * 10.0f;
    at[2] = kind == SPAWN_SHAPE ? 50.0f : kind == SPAWN_LIGHT || kind == SPAWN_CAMERA ? 250.0f : 100.0f;
}

static void add_actor(Editor* ed, SpawnKind kind, int sub, double now) {
    float at[3];
    place_in_front(ed, kind, at);
    if (kind == SPAWN_SHAPE && sub == SHAPE_PLANE) at[2] = 0.0f;
    editor_undo_push(ed);
    spawn_actor(ed, kind, sub, at, now);
}

// --- Sky presets: realistic skies in one click (the sun's angle and color come from the atmosphere itself) ---

const SkyPreset g_sky_presets[SKY_PRESET_COUNT] = {
    // name, sun pitch/yaw, lux, color; clouds: coverage, density (1/m), detail, shadow; fog: density (1/m), falloff
    // (1/m), albedo, anisotropy, emissive (nits)
    { "Clear Day", -55.0f, 30.0f, 120000.0f, { 1, 1, 1 }, 0.15f, 0.03f, 0.4f, 0.8f, 0.0004f, 0.01f, { 0.9f, 0.93f, 1.0f }, 0.6f, { 0, 0, 0 } },
    { "Morning", -10.0f, -70.0f, 120000.0f, { 1, 1, 1 }, 0.3f, 0.035f, 0.5f, 0.85f, 0.004f, 0.03f, { 1.0f, 0.95f, 0.9f }, 0.75f, { 0, 0, 0 } },
    { "Sunset", -3.0f, 70.0f, 120000.0f, { 1, 1, 1 }, 0.45f, 0.035f, 0.6f, 0.8f, 0.0015f, 0.02f, { 1.0f, 0.9f, 0.85f }, 0.8f, { 0, 0, 0 } },
    { "Cloudy", -45.0f, 40.0f, 120000.0f, { 1, 1, 1 }, 0.6f, 0.045f, 0.6f, 0.9f, 0.001f, 0.015f, { 0.92f, 0.94f, 1.0f }, 0.6f, { 0, 0, 0 } },
    { "Overcast", -60.0f, 30.0f, 120000.0f, { 1, 1, 1 }, 0.97f, 0.06f, 0.8f, 1.0f, 0.002f, 0.01f, { 0.9f, 0.9f, 0.9f }, 0.3f, { 0, 0, 0 } },
    { "Foggy", -35.0f, 20.0f, 120000.0f, { 1, 1, 1 }, 0.85f, 0.05f, 0.7f, 1.0f, 0.04f, 0.08f, { 0.95f, 0.95f, 0.95f }, 0.5f, { 0, 0, 0 } },
    { "Night (Moonlight)", -40.0f, 200.0f, 0.3f, { 0.6f, 0.7f, 1.0f }, 0.3f, 0.035f, 0.5f, 0.6f, 0.001f, 0.02f, { 0.9f, 0.92f, 1.0f }, 0.5f,
      { 0.0004f, 0.0003f, 0.0002f } },
};

// The first actor with component c (ENTITY_NONE: none); for COMP_LIGHT, the first of type light_type
static Entity find_actor(World* w, ComponentId c, int light_type) {
    for (uint32_t i = 1; i < w->slot_count; i++) {
        Entity e = ecs_entity_at(w, i);
        if (e == ENTITY_NONE) continue;
        void* p = ecs_get(w, e, c);
        if (!p) continue;
        if (c == COMP_LIGHT && (int)((CLight*)p)->type != light_type) continue;
        return e;
    }
    return ENTITY_NONE;
}

// (2026-11-17, Q3) with sublevels: only the current level's (a sky preset made with Night current goes into Night, never
// changes Day's sun); what is missing is made there
static Entity find_level_actor(Editor* ed, ComponentId c, int light_type) {
    World* w = &ed->world;
    for (uint32_t i = 1; i < w->slot_count; i++) {
        Entity e = ecs_entity_at(w, i);
        if (e == ENTITY_NONE) continue;
        void* p = ecs_get(w, e, c);
        if (!p) continue;
        if (c == COMP_LIGHT && (int)((CLight*)p)->type != light_type) continue;
        if (!levels_in_current(ed, e)) continue;
        return e;
    }
    return ENTITY_NONE;
}
static Entity spawn_in_level(Editor* ed, SpawnKind kind, int sub, const float at[3], double now) {
    Entity e = spawn_actor(ed, kind, sub, at, now);
    levels_assign_now(ed); // (into the current level now, not next frame: the next find sees it there)
    return e;
}

void editor_sky_preset(Editor* ed, int preset, double now) {
    if (preset < 0 || preset >= SKY_PRESET_COUNT) return;
    const SkyPreset* p = &g_sky_presets[preset];
    editor_undo_push(ed);
    World* w = &ed->world;
    Entity sun = find_level_actor(ed, COMP_LIGHT, LIGHT_DIRECTIONAL);
    if (sun == ENTITY_NONE) sun = spawn_in_level(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, -300, 600 }, now);
    if (find_level_actor(ed, COMP_LIGHT, LIGHT_SKY) == ENTITY_NONE) spawn_in_level(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, now);
    Entity fog = find_level_actor(ed, COMP_FOG, 0);
    if (fog == ENTITY_NONE) fog = spawn_in_level(ed, SPAWN_FOG, 0, (float[]){ 0, 0, 0 }, now);
    Entity cloud = find_level_actor(ed, COMP_CLOUD, 0);
    if (cloud == ENTITY_NONE) cloud = spawn_in_level(ed, SPAWN_CLOUD, 0, (float[]){ 0, 0, 800 }, now);
    CLight* l = ecs_get(w, sun, COMP_LIGHT);
    CTransform* t = ecs_get(w, sun, COMP_TRANSFORM);
    if (l && t) {
        l->intensity = p->sun_lux;
        memcpy(l->color, p->sun_color, sizeof l->color);
        t->rotation[0] = p->sun_pitch;
        t->rotation[1] = p->sun_yaw;
        t->rotation[2] = 0.0f;
    }
    CCloud* c = ecs_get(w, cloud, COMP_CLOUD);
    if (c) {
        c->coverage = p->coverage;
        c->density = p->cloud_density;
        c->detail = p->detail;
        c->shadow_strength = p->cloud_shadow;
        // the high cirrus: clear and sunset skies show it most; an overcast or foggy one hides it
        static const float cirrus[SKY_PRESET_COUNT] = { 0.45f, 0.5f, 0.6f, 0.3f, 0.0f, 0.0f, 0.3f };
        c->cirrus = cirrus[preset];
    }
    CFog* g = ecs_get(w, fog, COMP_FOG);
    if (g) {
        g->density = p->fog_density;
        g->height_falloff = p->fog_falloff;
        memcpy(g->albedo, p->fog_albedo, sizeof g->albedo);
        g->anisotropy = p->fog_anisotropy;
        memcpy(g->emissive, p->fog_emissive, sizeof g->emissive);
    }
    ed->selected = ENTITY_NONE;
    ed->dirty = true;
    editor_status(ed, now, TR("Sky preset: %s."), TR(p->name));
}

// --- The classes to place (2026-11-10: organized, searchable, dragged, in submenus) ---
// One catalog for the toolbar's Add (Quick Add), Actor > Add, the Outliner's "+" and the Place Actors panel: each class
// once, in one category (Unreal 5's), the templates (particle systems, vehicles, cloth, procedural worlds, sky presets)
// grouped under a submenu of their own. The menus open with a search field (typing filters every class by its name in
// English and in the language shown); any class drags from the menu or the panel into the viewport (dropped where the
// mouse ray meets the ground), or a click puts it 4 m in front of the camera.
enum { PX_PARTICLES = 100, PX_VEHICLE, PX_WATER_BODY, PX_RAIL, PX_OCEAN, PX_RAIN, PX_CLOTH, PX_PROCGEN, PX_SKY, PX_WATER_EXCLUSION, PX_SOFT, PX_SPLINE };
static void add_soft(Editor* ed, int preset, double now); // (Q9: editor_softbody.inc)
static void add_spline(Editor* ed, int what, double now); // (Q14: editor_spline.inc)
typedef struct PlaceItem { int cat, spawn, arg; const char* name; UiIcon icon; uint32_t color; const char* group; } PlaceItem; // (color: a theme token)
enum { PC_BASIC, PC_LIGHTS, PC_SHAPES, PC_VFX, PC_VOLUMES, PC_GAMEPLAY, PC_AUDIO, PC_NAV_AI, PC_VEHICLES, PC_WORLD, PC_PROTO, PC_CINEMATIC, PC_COUNT };
static const char* const g_place_cat_names[PC_COUNT] = { "Basic", "Lights", "Shapes", "Visual Effects", "Volumes", "Gameplay", "Audio",
                                                         "Navigation and AI", "Vehicles and Physics", "World", "Prototyping", "Cinematic" };
static const UiIcon g_place_cat_icons[PC_COUNT] = { ICON_ACTOR, ICON_BULB, ICON_CUBE, ICON_SKY, ICON_SELECT, ICON_GAMEPAD, ICON_SOUND,
                                                    ICON_MOVEMENT, ICON_ARROW, ICON_GLOBE, ICON_GRID, ICON_CAMERA };
static PlaceItem g_place[224];
static int g_nplace, g_place_recent[8], g_nplace_recent;

static void place_catalog(void) {
    if (g_nplace) return;
    int n = 0;
#define ADD(c_, s_, a_, name_, icon_, col_, grp_) g_place[n++] = (PlaceItem){ c_, s_, a_, name_, icon_, col_, grp_ }
    ADD(PC_BASIC, SPAWN_EMPTY, 0, "Empty Actor", ICON_CUBE, TK_N14, NULL);
    ADD(PC_BASIC, SPAWN_PLAYER_START, 0, "Player Start", ICON_CHARACTER, TK_AMBER_L, NULL);
    static const UiIcon li[LIGHT_TYPE_COUNT] = { ICON_SUN, ICON_BULB, ICON_SPOT, ICON_RECT, ICON_SKY };
    for (int i = 0; i < LIGHT_TYPE_COUNT; i++) ADD(PC_LIGHTS, SPAWN_LIGHT, i, g_light_names[i], li[i], TK_AMBER_L, NULL);
    for (int i = 0; i < SHAPE_COUNT; i++) ADD(PC_SHAPES, SPAWN_SHAPE, i, g_shape_names[i], (UiIcon)(ICON_CUBE + i), TK_N14, NULL);
    ADD(PC_VFX, SPAWN_FOG, 0, "Exponential Height Fog", ICON_GLOBE, TK_N14, NULL);
    ADD(PC_VFX, SPAWN_CLOUD, 0, "Volumetric Cloud", ICON_SKY, TK_N14, NULL);
    ADD(PC_VFX, SPAWN_FOG_VOLUME, 0, "Local Fog Volume", ICON_GLOBE, TK_N14, NULL);
    ADD(PC_VFX, SPAWN_VOLUME, 0, "Heterogeneous Volume", ICON_GLOBE, TK_N12, NULL);
    ADD(PC_VFX, SPAWN_DECAL, 0, "Decal", ICON_EXPOSURE, TK_N14, NULL);
    ADD(PC_VFX, SPAWN_FIRE, 0, "Fire", ICON_EXPOSURE, TK_ORANGE, NULL);
    ADD(PC_VFX, SPAWN_SPLAT, 0, "Gaussian Splat", ICON_GLOBE, TK_GREEN_L, NULL);
    ADD(PC_VFX, PX_RAIN, 0, "Rain", ICON_SKY, TK_N13, NULL);
    for (int t = 1; t < PST_COUNT; t++) ADD(PC_VFX, PX_PARTICLES, t, g_ps_template_names[t], ICON_BULB, TK_AMBER, "Particle System");
    ADD(PC_VOLUMES, SPAWN_POST, 0, "Post Process Volume", ICON_EXPOSURE, TK_N14, NULL);
    ADD(PC_VOLUMES, SPAWN_AUDIO_VOLUME, 0, "Audio Volume", ICON_SOUND, TK_GREEN, NULL);
    ADD(PC_VOLUMES, SPAWN_TRIGGER, 0, "Trigger Box", ICON_CUBE, TK_ORANGE, NULL);
    ADD(PC_VOLUMES, SPAWN_TRIGGER, 1, "Trigger Sphere", ICON_SPHERE, TK_ORANGE, NULL);
    ADD(PC_VOLUMES, SPAWN_TRIGGER, 2, "Trigger Capsule", ICON_CAPSULE, TK_ORANGE, NULL);
    ADD(PC_VOLUMES, SPAWN_NAV, NAV_BOUNDS_VOLUME, "Nav Mesh Bounds Volume", ICON_MOVEMENT, TK_GREEN, NULL);
    ADD(PC_VOLUMES, SPAWN_NAV, NAV_MODIFIER_VOLUME, "Nav Modifier Volume", ICON_MOVEMENT, TK_GREEN, NULL);
    ADD(PC_GAMEPLAY, SPAWN_PICKUP, 0, "Pickup", ICON_CUBE, TK_CYAN, NULL);
    ADD(PC_GAMEPLAY, SPAWN_CHECKPOINT, 0, "Checkpoint", ICON_CHARACTER, TK_AMBER, NULL);
    ADD(PC_GAMEPLAY, SPAWN_GAME_RULES, 0, "Game Rules", ICON_BLUEPRINT, TK_BLUE, NULL);
    for (int k = 0; k < DEV_KIND_COUNT; k++) ADD(PC_GAMEPLAY, SPAWN_DEVICE, k, g_device_names[k], ICON_CUBE, TK_AMBER, "Interactables");
    ADD(PC_AUDIO, SPAWN_AMBIENT_SOUND, 0, "Ambient Sound", ICON_SOUND, TK_CYAN, NULL);
    ADD(PC_NAV_AI, SPAWN_NAV, NAV_LINK_PROXY, "Nav Link Proxy", ICON_MOVEMENT, TK_GREEN, NULL);
    ADD(PC_NAV_AI, SPAWN_CROWD, 0, "Crowd Spawner", ICON_CHARACTER, TK_AMBER_L, NULL);
    for (int t = 0; t < VT_COUNT; t++) ADD(PC_VEHICLES, PX_VEHICLE, t, g_veh_template_names[t], ICON_MOVEMENT, TK_AMBER, "Vehicle");
    ADD(PC_VEHICLES, PX_RAIL, 0, "Rail", ICON_GRAPH, TK_N11, NULL);
    for (int pr = 1; pr < CLOTH_PRESET_COUNT; pr++) ADD(PC_VEHICLES, PX_CLOTH, pr, g_cloth_preset_names[pr], ICON_PLANE, TK_RED, "Cloth");
    for (int pr = 1; pr < SOFT_PRESET_COUNT; pr++) ADD(PC_VEHICLES, PX_SOFT, pr, g_soft_preset_names[pr], ICON_SPHERE, TK_RED, "Soft Body"); // (Q9)
    ADD(PC_WORLD, SPAWN_LANDSCAPE, 0, "Landscape", ICON_FLOOR, TK_LIME_L, NULL);
    ADD(PC_WORLD, SPAWN_FOLIAGE, 0, "Foliage", ICON_FLOOR, TK_GREEN, NULL);
    ADD(PC_WORLD, SPAWN_WORLD_PARTITION, 0, "World Partition", ICON_GLOBE, TK_LIME_L, NULL);
    ADD(PC_WORLD, SPAWN_RIVER, 0, "Water Body River", ICON_GLOBE, TK_BLUE_L, NULL);
    static const char* const spl[4] = { "Spline", "Spline Mesh", "Spline Instances", "Cable" }; // (Q14: editor_spline.inc's order)
    for (int k = 0; k < 4; k++) ADD(PC_WORLD, PX_SPLINE, k, spl[k], ICON_SPLINE, TK_AMBER, "Splines");
    ADD(PC_WORLD, PX_WATER_BODY, 0, "Water Body", ICON_PLANE, TK_BLUE, NULL);
    ADD(PC_WORLD, PX_OCEAN, 0, "Ocean", ICON_GLOBE, TK_BLUE, NULL);
    ADD(PC_WORLD, PX_WATER_EXCLUSION, 0, "Water Body Exclusion", ICON_PLANE, TK_ORANGE, NULL);
    for (int m = 0; m < PROCGEN_MODE_COUNT; m++) ADD(PC_WORLD, PX_PROCGEN, m, g_procgen_mode_names[m], ICON_GLOBE, TK_GREEN, "Procedural World");
    ADD(PC_WORLD, PX_PROCGEN, PROCGEN_PCG, "PCG Volume", ICON_FOLIAGE, TK_LIME, NULL); // (Q15: a PCG component in a box)
    for (int p = 0; p < SKY_PRESET_COUNT; p++) ADD(PC_WORLD, PX_SKY, p, g_sky_presets[p].name, ICON_SUN, TK_AMBER_L, "Sky Preset");
    for (int k = 0; k < PROTO_KIND_COUNT; k++) ADD(PC_PROTO, SPAWN_PROTO, k, g_proto_kind_names[k], ICON_CUBE, TK_N14, NULL);
    ADD(PC_CINEMATIC, SPAWN_CAMERA, 0, "Camera Actor", ICON_CAMERA, TK_N14, NULL);
#undef ADD
    g_nplace = n;
}

static void add_procgen(Editor* ed, int m, double now) {
    editor_undo_push(ed);
    if (m == (int)PROCGEN_PCG) { // (Q15) a PCG Volume in front of the camera, on the ground, with the forest example
        float cy = cosf(ed->cam.yaw * 0.017453293f), sy = sinf(ed->cam.yaw * 0.017453293f), at[3] = { ed->cam.pos[0] + cy * 6000.0f, ed->cam.pos[1] + sy * 6000.0f, 0.0f };
        landscape_height(at[0], at[1], &at[2]);
        Entity pe = pcg_spawn_volume(&ed->world, at, "/Content/PCG/Examples/PCG_Forest.rpcg");
        if (!pe) return;
        char pn[RE_NAME_MAX];
        editor_unique_name(ed, "PCGVolume", pn, sizeof pn);
        set_name(&ed->world, pe, pn);
        editor_select(ed, pe, SEL_REPLACE);
        ed->dirty = true;
        editor_status(ed, now, TR("PCG Volume placed: its graph, seed and box in Details; it generates inside the box."));
        return;
    }
    Entity pw = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_PROC_WORLD));
    if (!pw) return;
    char un[RE_NAME_MAX];
    editor_unique_name(ed, "ProceduralWorld", un, sizeof un);
    set_name(&ed->world, pw, un);
    CTransform* t = ecs_get(&ed->world, pw, COMP_TRANSFORM);
    t->flags = MOBILITY_STATIC;
    if (m == PROCGEN_FACILITY) {
        float cy = cosf(ed->cam.yaw * 0.017453293f), sy = sinf(ed->cam.yaw * 0.017453293f);
        t->location[0] = ed->cam.pos[0] + cy * 1500.0f, t->location[1] = ed->cam.pos[1] + sy * 1500.0f;
    }
    ((CProcWorld*)ecs_get(&ed->world, pw, COMP_PROC_WORLD))->mode = (uint32_t)m;
    editor_select(ed, pw, SEL_REPLACE);
    ed->dirty = true;
    editor_status(ed, now, TR("Procedural World placed: its seed and rules in Details; on Play it grows around the player."));
}

// A class of the catalog placed (in front of the camera); remembered in Recently Placed
static void place_item(Editor* ed, int i, double now) {
    if (i < 0 || i >= g_nplace) return;
    const PlaceItem* it = &g_place[i];
    switch (it->spawn) {
    case PX_PARTICLES: add_particles(ed, it->arg, now); break;
    case PX_VEHICLE: add_vehicle(ed, it->arg, now); break;
    case PX_WATER_BODY: add_water_or_rail(ed, false, now); break;
    case PX_RAIL: add_water_or_rail(ed, true, now); break;
    case PX_OCEAN: add_ocean(ed, now); break;
    case PX_WATER_EXCLUSION: add_water_exclusion(ed, now); break;
    case PX_RAIN: add_rain(ed, now); break;
    case PX_CLOTH: add_cloth(ed, it->arg, now); break;
    case PX_SOFT: add_soft(ed, it->arg, now); break;
    case PX_SPLINE: add_spline(ed, it->arg, now); break; // (Q14)
    case PX_PROCGEN: add_procgen(ed, it->arg, now); break;
    case PX_SKY: editor_sky_preset(ed, it->arg, now); break;
    case SPAWN_WORLD_PARTITION: { // (B1: one a level; there already: selected)
        Entity wpe = wp_settings(&ed->world);
        if (wpe != ENTITY_NONE) editor_select(ed, wpe, SEL_REPLACE);
        else add_actor(ed, SPAWN_WORLD_PARTITION, 0, now);
    } break;
    default: add_actor(ed, (SpawnKind)it->spawn, it->arg, now); break;
    }
    int r = 0; // (Recently Placed: this one first, no repeats, 8 at most)
    while (r < g_nplace_recent && g_place_recent[r] != i) r++;
    if (r == g_nplace_recent && g_nplace_recent < 8) g_nplace_recent++;
    if (r >= g_nplace_recent) r = g_nplace_recent - 1;
    memmove(&g_place_recent[1], &g_place_recent[0], sizeof(int) * (size_t)r);
    g_place_recent[0] = i;
}

void editor_place_drop(Editor* ed, int index, const float at[3]) {
    place_catalog();
    if (index < 0 || index >= g_nplace || ed->playing) return;
    const PlaceItem* it = &g_place[index];
    if (it->spawn == PX_SKY) { // (the sky has no place)
        place_item(ed, index, igGetTime());
        return;
    }
    Entity before = ed->selected;
    place_item(ed, index, igGetTime());
    Entity e = ed->selected;
    CTransform* t = e != before && e ? ecs_get(&ed->world, e, COMP_TRANSFORM) : NULL;
    if (!t) return;
    // where it was dropped (on a 10 cm grid), as high above that point as it was above the ground in front of the camera
    t->location[0] = roundf(at[0] / 10.0f) * 10.0f, t->location[1] = roundf(at[1] / 10.0f) * 10.0f;
    t->location[2] += at[2];
    ecs_touch(&ed->world, e);
    ed->drawer_open = false;
}

// The class matches the search (its English name, its translation, its group's: lower case, any part); the Gameplay
// Framework's (Pickup, Checkpoint, Game Rules, the Interactables) only while that plugin is on
static bool place_match(const PlaceItem* it, const char* low) {
    if (!plugins_gameplay_enabled() && (it->spawn == SPAWN_PICKUP || it->spawn == SPAWN_CHECKPOINT || it->spawn == SPAWN_GAME_RULES ||
                                        it->spawn == SPAWN_DEVICE))
        return false;
    if (!low[0]) return true;
    char a[256];
    snprintf(a, sizeof a, "%s %s %s %s", it->name, TR(it->name), it->group ? it->group : "", it->group ? TR(it->group) : "");
    for (char* p = a; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strstr(a, low) != NULL;
}

static void place_drag_source(int i) {
    if (!igBeginDragDropSource(0)) return;
    PlacePayload pl = { i };
    igSetDragDropPayload(RE_PAYLOAD_PLACE, &pl, sizeof pl, 0);
    ui_icon_item(g_place[i].icon, igGetFontSize(), TH(g_place[i].color));
    igSameLine(0, 6);
    igTextUnformatted(TR(g_place[i].name), NULL);
    igEndDragDropSource();
}

// A menu line of the catalog: its icon, its name; a click places it, a drag carries it to the viewport
static void place_menu_item(Editor* ed, int i, bool with_group, double now) {
    const PlaceItem* it = &g_place[i];
    if (!place_match(it, "")) return; // (a plugin's class while it is off)
    char label[160];
    if (with_group && it->group) snprintf(label, sizeof label, "%s: %s##pl%d", TR(it->group), TR(it->name), i);
    else snprintf(label, sizeof label, "%s##pl%d", TR(it->name), i);
    ui_icon_item(it->icon, igGetFontSize(), TH(it->color));
    igSameLine(0, 6);
    if (igMenuItem_Bool(label, NULL, false, !ed->playing)) place_item(ed, i, now);
    place_drag_source(i);
    if (it->spawn == SPAWN_WORLD_PARTITION)
        igSetItemTooltip("%s", TR("Streams a big level by a grid around the player, with Data Layers and HLOD (World Partition)."));
}

// The Quick Add items (toolbar "+", Actor > Add, the Outliner's "+"): the search first, then a submenu per category
// (the templates in a submenu of their own inside it); typing lists every match at once
static void quick_add_items(Editor* ed, double now) {
    place_catalog();
    static char find[64];
    if (igIsWindowAppearing()) find[0] = 0, igSetKeyboardFocusHere(0);
    igSetNextItemWidth(igGetFontSize() * 16.0f);
    igInputTextWithHint("##qa_find", TR("Search Classes"), find, sizeof find, 0, NULL, NULL);
    char low[64];
    snprintf(low, sizeof low, "%s", find);
    for (char* p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (low[0]) { // a flat list of the matches, by category
        int shown = 0;
        for (int c = 0; c < PC_COUNT; c++) {
            bool title = false;
            for (int i = 0; i < g_nplace; i++) {
                if (g_place[i].cat != c || !place_match(&g_place[i], low)) continue;
                if (!title) igSeparatorText(TR(g_place_cat_names[c])), title = true;
                place_menu_item(ed, i, true, now);
                shown++;
            }
        }
        if (!shown) igTextDisabled("%s", TR("No classes match the search."));
        return;
    }
    if (g_nplace_recent) {
        igSeparatorText(TR("Recently Placed"));
        for (int r = 0; r < g_nplace_recent; r++) place_menu_item(ed, g_place_recent[r], true, now);
    }
    igSeparatorText(TR("Classes"));
    for (int c = 0; c < PC_COUNT; c++) {
        bool any = false; // (a category whose classes are all a plugin's that is off: not shown)
        for (int i = 0; i < g_nplace && !any; i++) any = g_place[i].cat == c && place_match(&g_place[i], "");
        if (!any) continue;
        igPushID_Int(c);
        if (ui_begin_menu(g_place_cat_icons[c], TR(g_place_cat_names[c]), true)) {
            const char* groups[8];
            int ng = 0;
            for (int i = 0; i < g_nplace; i++) {
                const PlaceItem* it = &g_place[i];
                if (it->cat != c) continue;
                if (!it->group) {
                    place_menu_item(ed, i, false, now);
                    continue;
                }
                bool seen = false; // (a group's submenu once, where its first class is)
                for (int g = 0; g < ng; g++) seen |= !strcmp(groups[g], it->group);
                if (seen || ng == 8 || !place_match(it, "")) continue;
                groups[ng++] = it->group;
                if (ui_begin_menu(it->icon, TR(it->group), true)) {
                    for (int j = i; j < g_nplace; j++)
                        if (g_place[j].cat == c && g_place[j].group && !strcmp(g_place[j].group, it->group)) place_menu_item(ed, j, false, now);
                    igEndMenu();
                }
            }
            igEndMenu();
        }
        igPopID();
    }
    igSeparator();
    igTextDisabled("%s", TR("Drag a class into the viewport to place it where you drop it."));
}

static void panel_header(const char* title);

// Unreal's Visibility: H hides the selected in the editor (the game still shows them), Ctrl+H shows every actor
// Actor > Make Cloth from Selected (cloth_mesh.h): the selected imported mesh (a skirt, a cape, a sleeve: a part of an
// imported Skeletal Mesh too) becomes a Cloth on that mesh, in its place: its top band pinned (the top along the world's
// up as it stands now), following the part's bone when it is one; the mesh hidden (the cloth draws it)
static void make_cloth_from_selected(Editor* ed) {
    World* w = &ed->world;
    Entity src = ed->selected;
    CStaticMesh* sm = src ? ecs_get(w, src, COMP_STATIC_MESH) : NULL;
    CTransform* st = src ? ecs_get(w, src, COMP_TRANSFORM) : NULL;
    if (!sm || !st || !(sm->mesh & 0x80000000u)) {
        editor_status(ed, 0.0, "%s", TR("Select an imported mesh (a skirt, a cape, a sleeve) to make a cloth of it."));
        return;
    }
    editor_undo_push(ed);
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CLOTH));
    if (e == ENTITY_NONE) return;
    const CName* sn = ecs_get(w, src, COMP_NAME);
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%.40s_Cloth", sn ? sn->text : "Mesh");
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, st->location, 12), memcpy(t->rotation, st->rotation, 12);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE;
    CCloth* c = ecs_get(w, e, COMP_CLOTH);
    ecs_component_defaults(COMP_CLOTH, c);
    c->mesh = sm->mesh;
    memcpy(c->mesh_scale, st->scale, 12);
    memcpy(c->color, sm->base_color, 12);
    float ax[3][3], mn[3], mx[3];
    rotator_axes(st->rotation, ax); // (the world's up in the mesh's space: the pins are its top as it stands)
    for (int k = 0; k < 3; k++) c->pin_axis[k] = ax[k][2];
    render_mesh_bounds(sm->mesh, mn, mx);
    float tall = 0.0f;
    for (int k = 0; k < 3; k++) tall += fabsf((mx[k] - mn[k]) * st->scale[k] * c->pin_axis[k]);
    c->pin_band = fmaxf(tall * 0.05f, 2.0f);
    const CAttach* at = ecs_get(w, src, COMP_ATTACH);
    if (at && at->kind == ATTACH_SK_PART && sn) { // a part of a Skeletal Mesh: its bone carries the pins
        const CName* on = ecs_get(w, (Entity)at->owner, COMP_NAME);
        if (on) snprintf(c->follow_actor, sizeof c->follow_actor, "%s", on->text), snprintf(c->follow_bone, sizeof c->follow_bone, "%s", sn->text);
        c->follow_turn = 1; // (its mesh in the bone's space)
    }
    st->flags |= TRANSFORM_HIDDEN | TRANSFORM_HIDDEN_GAME;
    editor_select_none(ed);
    editor_select(ed, e, SEL_REPLACE);
    ed->dirty = true;
    editor_status(ed, 0.0, TR("Cloth made from %s: its top band pinned (Details > Cloth > Pin Band). Play (Alt+P) to see it fall."), sn ? sn->text : "");
}

// Actor > Make Vehicle from Selected (vehicle.h vehicle_from_actors): the selected Static Mesh actors (an imported
// model: its body, wheels, doors) become a vehicle of a kind, its asset in /Content/Vehicles
static int g_veh_front = -1; // the model's front: -1 its longest side, 0 +X, 1 -X, 2 +Y, 3 -Y
static void make_vehicle_menu(Editor* ed) {
    static const int kinds[] = { VT_CAR, VT_SPORTS, VT_TRUCK, VT_MOTORCYCLE, VT_TANK, VT_BOAT, VT_AIRPLANE, VT_HELICOPTER };
    igTextDisabled("%s", TR("Select the model's pieces (body, wheels, doors), then its kind:"));
    for (int i = 0; i < (int)(sizeof kinds / sizeof kinds[0]); i++) {
        if (!ui_menu_item(ICON_GAMEPAD, TR(g_veh_template_names[kinds[i]]), NULL, false, ed->nsel > 0 && !ed->playing)) continue;
        char dir[1100], err[200];
        snprintf(dir, sizeof dir, "%s/Vehicles", content_root());
        re_make_dirs(dir);
        editor_undo_push(ed);
        Entity v = vehicle_from_actors(&ed->world, ed->sel, ed->nsel, kinds[i], g_veh_front, "/Content/Vehicles", err, sizeof err);
        if (v == ENTITY_NONE) editor_status(ed, 0.0, TR("Could not make the vehicle: %s"), err);
        else {
            editor_select_none(ed);
            editor_select(ed, v, SEL_REPLACE);
            content_mark_dirty();
            CVehicle* cv = ecs_get(&ed->world, v, COMP_VEHICLE);
            cv->auto_possess = 1; // (Play drives it at once; Details > Vehicle > Auto Possess turns it off)
            const VehicleAsset* va = veh_find(cv->asset);
            uint32_t wheels = 0;
            for (uint32_t k = 0; va && k < va->nparts; k++) wheels += va->parts[k].kind == VP_WHEEL;
            editor_status(ed, 0.0, TR("Vehicle made: %s (%u wheels). Play (Alt+P) drives it (W/S, A/D, Space); its asset in Content/Vehicles adjusts it."),
                          va ? va->name : "", wheels);
        }
    }
    igSeparator();
    static const char* const fronts[5] = { "Front: Automatic (longest side)", "Front: +X", "Front: -X", "Front: +Y", "Front: -Y" };
    if (ui_begin_menu(ICON_ARROW, TR(fronts[g_veh_front + 1]), true)) {
        for (int f = -1; f < 4; f++)
            if (ui_menu_item(ICON_ARROW, TR(fronts[f + 1]), NULL, g_veh_front == f, true)) g_veh_front = f;
        igEndMenu();
    }
}

static void hide_selected(Editor* ed, bool hide) {
    editor_undo_push(ed);
    if (hide) {
        for (uint32_t i = 0; i < ed->nsel; i++) {
            CTransform* t = ecs_get(&ed->world, ed->sel[i], COMP_TRANSFORM);
            if (t) t->flags |= TRANSFORM_HIDDEN;
        }
        return;
    }
    for (uint32_t ai = 0; ai < ed->world.archetype_count; ai++) {
        EcsArchetype* ar = &ed->world.archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        CTransform* t = ecs_arch_column(ar, COMP_TRANSFORM);
        for (uint32_t i = 0; i < ar->count; i++) t[i].flags &= ~TRANSFORM_HIDDEN;
    }
}

// Unreal's Place Actors panel: the classes by category (a tab each, a search over all); a click places one in front of
// the camera, any class can be dragged into the viewport too (the same catalog as the Add menus)
// (Q26) A row of Place Actors: the class's icon on a dark tile and its name, on a card; click places it in front of the
// camera, a drag carries it into the viewport
static void place_row(Editor* ed, int i, int salt, double now) {
    const PlaceItem* it = &g_place[i];
    float fs = igGetFontSize(), s = fs * 1.95f;
    ImDrawList* dl = igGetWindowDrawList();
    igPushID_Int(salt * 1000 + i);
    ImVec2 p = igGetCursorScreenPos();
    float cw = igGetContentRegionAvail().x;
    bool click = igInvisibleButton("##it", (ImVec2){ cw, s }, 0); // tips: none (a canvas or a tile drawn with its own name: no description over the work)
    bool hov = igIsItemHovered(0), held = igIsItemActive();
    place_drag_source(i);
    igSetItemTooltip("%s", TR("Click to place it in front of the camera, or drag it into the viewport."));
    float r = TMV(TM_RADIUS_FRAME);
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + cw, p.y + s }, held ? TH(TK_BG_SELECTED_DIM) : hov ? TH(TK_BG_HOVER) : TH(TK_BG_CONTROL), r, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2){ p.x + 3, p.y + 3 }, (ImVec2){ p.x + s - 3, p.y + s - 3 }, TH(TK_BG_FIELD), r, 0);
    ui_icon(dl, it->icon, p.x + s * 0.2f, p.y + s * 0.2f, s * 0.6f, TH(it->color));
    ImDrawList_PushClipRect(dl, p, (ImVec2){ p.x + cw - 4, p.y + s }, true);
    const char* name = it->spawn == SPAWN_EMPTY ? "Actor" : it->name; // (Unreal's: the empty actor is "Actor")
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + s + fs * 0.6f, p.y + (s - fs) * 0.5f }, hov ? TH(TK_TEXT_STRONG) : TH(TK_TEXT), TR(name), NULL);
    ImDrawList_PopClipRect(dl);
    if (click && !ed->playing) place_item(ed, i, now);
    igPopID();
    igDummy((ImVec2){ 0, 1.0f });
}

// (Q26) A category's header: a chevron, the name; clicked, it opens or closes (the mockups' Basic, Geometry...)
static bool place_header(const char* name, int count, bool* open) {
    float fs = igGetFontSize(), h = fs * 1.75f, w = igGetContentRegionAvail().x;
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 p = igGetCursorScreenPos();
    igPushID_Str(name);
    if (igInvisibleButton("##cat", (ImVec2){ w, h }, 0)) *open = !*open; // tips: none (a header drawn with its own name)
    bool hov = igIsItemHovered(0);
    igPopID();
    if (hov) ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + h }, TH(TK_BG_HOVER), TMV(TM_RADIUS_FRAME), 0);
    float cs = fs * 0.55f, cx = p.x + fs * 0.55f, cy = p.y + h * 0.5f;
    uint32_t cc = TH(TK_TEXT_DIM);
    if (*open) ImDrawList_AddTriangleFilled(dl, (ImVec2){ cx - cs * 0.5f, cy - cs * 0.3f }, (ImVec2){ cx + cs * 0.5f, cy - cs * 0.3f }, (ImVec2){ cx, cy + cs * 0.4f }, cc);
    else ImDrawList_AddTriangleFilled(dl, (ImVec2){ cx - cs * 0.3f, cy - cs * 0.5f }, (ImVec2){ cx + cs * 0.4f, cy }, (ImVec2){ cx - cs * 0.3f, cy + cs * 0.5f }, cc);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + fs * 1.4f, p.y + (h - fs) * 0.5f }, *open ? TH(TK_TEXT_STRONG) : TH(TK_TEXT), name, NULL);
    char n[16];
    snprintf(n, sizeof n, "%d", count);
    ImVec2 ns = igCalcTextSize(n, NULL, false, -1);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + w - ns.x - fs * 0.5f, p.y + (h - fs) * 0.5f }, TH(TK_TEXT_DISABLED), n, NULL);
    return *open;
}

static void place_actors(Editor* ed, double now) {
    place_catalog();
    static char find[64];
    // (Q26, the mockups) the search on top; then the categories as headers that open and close: Basic (the classes a
    // level starts with: actor, player start, the lights, triggers, shapes, camera) open, the others closed;
    // Recently Placed first when there is one. Searching lists every match under its category's header
    panel_header(TR("Place Actors"));
    igSetNextItemWidth(-1);
    igInputTextWithHint("##place_find", TR("Search Classes..."), find, sizeof find, 0, NULL, NULL);
    igDummy((ImVec2){ 0, 2 });
    igBeginChild_Str("place_list", (ImVec2){ 0, 0 }, 0, 0);
    char low[64];
    snprintf(low, sizeof low, "%s", find);
    for (char* p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
    enum { CAT_RECENT = PC_COUNT, CAT_ALL };
    static bool open[PC_COUNT + 2] = { [PC_BASIC] = true };
    // Basic: Unreal 5's (its classes are also in their own categories)
    static int basic[16], nbasic = -1;
    if (nbasic < 0) {
        nbasic = 0;
        for (int i = 0; i < g_nplace && nbasic < 16; i++) {
            const PlaceItem* it = &g_place[i];
            bool b = it->cat == PC_BASIC || (it->spawn == SPAWN_LIGHT && it->arg != 0) || (it->spawn == SPAWN_TRIGGER && it->arg <= 1) ||
                     (it->spawn == SPAWN_SHAPE && it->arg <= 1) || it->spawn == SPAWN_CAMERA;
            if (b) basic[nbasic++] = i;
        }
    }
    int shown = 0;
    for (int k = -1; k <= PC_COUNT; k++) { // Recently Placed, the categories, All Classes (only while not searching)
        int cat = k < 0 ? CAT_RECENT : k == PC_COUNT ? CAT_ALL : k;
        if (find[0] && (cat == CAT_RECENT || cat == CAT_ALL)) continue;
        static int list[224];
        int nl = 0;
        if (cat == CAT_RECENT) {
            for (int r = 0; r < g_nplace_recent; r++)
                if (place_match(&g_place[g_place_recent[r]], "")) list[nl++] = g_place_recent[r];
        } else if (cat == PC_BASIC && !find[0]) {
            for (int b = 0; b < nbasic; b++)
                if (place_match(&g_place[basic[b]], "")) list[nl++] = basic[b];
        } else
            for (int i = 0; i < g_nplace && nl < 224; i++)
                if ((cat == CAT_ALL || g_place[i].cat == cat) && place_match(&g_place[i], low)) list[nl++] = i;
        if (!nl) continue;
        const char* cname = cat == CAT_RECENT ? "Recently Placed" : cat == CAT_ALL ? "All Classes" : g_place_cat_names[cat];
        bool op = find[0] ? true : open[cat];
        if (!find[0]) op = place_header(TR(cname), nl, &open[cat]);
        else { // (searching: the category's name over its matches)
            igTextDisabled("%s", TR(cname));
        }
        if (op) {
            igIndent(igGetFontSize() * 0.3f);
            for (int li = 0; li < nl; li++) place_row(ed, list[li], k + 1, now);
            igUnindent(igGetFontSize() * 0.3f);
            igDummy((ImVec2){ 0, 3 });
        }
        shown++;
    }
    if (find[0] && !shown) igTextDisabled("%s", TR("No classes match the search."));
    igEndChild();
}

static Entity mesh_actor(Editor* ed, int shape, float x, float y, float z, const float color[3], float metal, float rough) {
    float at[3] = { x, y, z };
    Entity e = spawn_actor(ed, SPAWN_SHAPE, shape, at, 0);
    CStaticMesh* m = ecs_get(&ed->world, e, COMP_STATIC_MESH);
    if (m) {
        memcpy(m->base_color, color, sizeof m->base_color);
        m->metallic = metal;
        m->roughness = rough;
    }
    return e;
}

// The scale tests (--stress-objects/--stress-lights, notas-dev/escala.md): a field of shapes on a grid around the
// default level (3 m apart, random shape, color, size, yaw; Static) and point lights above them (random warm/cool
// colors, 2-15 m of reach). Without names: generating unique ones is the editor's, not the scene's, cost.
// The texture streaming's stress (sugestÃ£o #51): n cubes 2.5 m apart in a row along +X, each with its own Material
// Instance of a 4096 material (n x 2 textures of 4096: ~180 MB a cube at full size, far past any budget). The camera
// flies along the row (main.c), so the cubes it nears climb and the ones it leaves go down.
uint32_t mat_make_stress(const char* content_root, uint32_t n, uint32_t* out);
void editor_stress_textures(Editor* ed, uint32_t n) {
    static uint32_t hashes[1000];
    if (n > 1000) n = 1000;
    double t0 = re_time_seconds();
    uint32_t made = mat_make_stress(content_root(), n, hashes);
    mat_bake_pending(NULL);
    mat_update(ed->vk, VK_NULL_HANDLE); // (the instances' own textures: at the start size, the budget short)
    for (uint32_t i = 0; i < made; i++) {
        Entity e = ecs_create(&ed->world, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
        if (e == ENTITY_NONE) break;
        CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        t->location[0] = 300.0f + (float)i * 250.0f;
        t->location[1] = (i & 1) ? 140.0f : -140.0f;
        t->location[2] = 75.0f;
        t->scale[0] = t->scale[1] = t->scale[2] = 1.5f;
        t->flags = MOBILITY_STATIC;
        CStaticMesh* m = ecs_get(&ed->world, e, COMP_STATIC_MESH);
        m->mesh = SHAPE_CUBE;
        m->material = hashes[i];
        m->base_color[0] = m->base_color[1] = m->base_color[2] = 0.8f;
        m->roughness = 0.5f;
    }
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = 0.0f, ed->cam.pos[2] = 160.0f;
    ed->cam.yaw = 0.0f, ed->cam.pitch = -5.0f;
    const MatStats* st = mat_stats();
    printf("[RealEngine] Stress textures: %u cubes with their own 4096 instance in %.2f s; %.0f MB of %.0f MB resident\n", made,
           re_time_seconds() - t0, (double)st->tex_bytes / 1048576.0, (double)st->budget / 1048576.0);
}

// The run's verdict: the budget kept, the cubes near the camera climbed, the far ones went down
bool editor_stress_textures_check(Editor* ed) {
    (void)ed;
    const MatStats* st = mat_stats();
    printf("[RealEngine] Texture streaming: %.0f of %.0f MB, %u variants streamed, %u up, %u down, %u textures; CPU %.3f ms at worst, "
           "bake tiles of %u texels (GPU %.3f ms at worst)\n",
           (double)st->tex_bytes / 1048576.0, (double)st->budget / 1048576.0, st->streamed, st->ups, st->downs, st->textures, st->stream_cpu_max,
           st->stream_tile, st->stream_gpu_max);
    if (!st->streaming) {
        printf("[RealEngine] Texture streaming off on this GPU: not checked\n");
        return true;
    }
    bool ok = true;
    if (st->tex_bytes > st->budget) printf("[RealEngine] FAIL: the textures (%.0f MB) passed the budget\n", (double)st->tex_bytes / 1048576.0), ok = false;
    if (!st->ups) printf("[RealEngine] FAIL: no variant climbed as the camera neared it\n"), ok = false;
    if (!st->downs) printf("[RealEngine] FAIL: no variant went down as the camera left it\n"), ok = false;
    return ok;
}

void editor_stress_scene(Editor* ed, uint32_t objects, uint32_t lights, float cast) {
    uint32_t seed = 12345u;
#define RND() ((seed = seed * 1664525u + 1013904223u) >> 8) * (1.0f / 16777216.0f)
    uint32_t side = (uint32_t)ceilf(sqrtf((float)(objects > 0 ? objects : 1)));
    float half = (float)side * 150.0f;
    for (uint32_t i = 0; i < objects; i++) {
        Entity e = ecs_create(&ed->world, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
        if (e == ENTITY_NONE) break;
        CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        float s = 0.4f + RND() * 1.2f;
        t->location[0] = (float)(i % side) * 300.0f - half + (RND() - 0.5f) * 120.0f;
        t->location[1] = (float)(i / side) * 300.0f - half + (RND() - 0.5f) * 120.0f;
        t->location[2] = 50.0f * s;
        t->rotation[1] = RND() * 360.0f;
        t->scale[0] = t->scale[1] = t->scale[2] = s;
        t->flags = MOBILITY_STATIC;
        CStaticMesh* m = ecs_get(&ed->world, e, COMP_STATIC_MESH);
        m->mesh = (uint32_t)(RND() * 4.0f) % 4u; // cube, sphere, cylinder, cone
        m->base_color[0] = 0.15f + RND() * 0.75f;
        m->base_color[1] = 0.15f + RND() * 0.75f;
        m->base_color[2] = 0.15f + RND() * 0.75f;
        m->metallic = RND() < 0.2f ? 1.0f : 0.0f;
        m->roughness = 0.2f + RND() * 0.7f;
    }
    // the lights over a field of their own size (one per 3 m x 3 m: ~25 reach each point, as many as on the objects)
    half = ceilf(sqrtf((float)(lights > 0 ? lights : 1))) * 150.0f;
    for (uint32_t i = 0; i < lights; i++) {
        Entity e = ecs_create(&ed->world, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_LIGHT));
        if (e == ENTITY_NONE) break;
        CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        t->location[0] = (RND() - 0.5f) * 2.0f * half;
        t->location[1] = (RND() - 0.5f) * 2.0f * half;
        t->location[2] = 150.0f + RND() * 300.0f;
        t->flags = MOBILITY_STATIONARY;
        CLight* l = ecs_get(&ed->world, e, COMP_LIGHT);
        l->type = LIGHT_POINT;
        bool warm = RND() < 0.5f;
        l->color[0] = warm ? 1.0f : 0.55f;
        l->color[1] = warm ? 0.6f : 0.75f;
        l->color[2] = warm ? 0.3f : 1.0f;
        l->intensity = 2000.0f + RND() * 18000.0f;
        l->attenuation_radius = 200.0f + RND() * 1300.0f;
        l->cast_shadows = RND() < cast;
    }
#undef RND
    ed->dirty = true;
}

// --shadow-lights N (the local shadow maps' measures, notas-dev/luz.md): N point lights casting shadows spread inside the
// meshes' bounds (shrunk 15%, between 35% and 85% of their height), 3-8 k cd, 3-7 m of range, a fixed seed
void editor_shadow_lights(Editor* ed, uint32_t n) {
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t s = 1; s < ed->world.slot_count; s++) {
        Entity e = ecs_entity_at(&ed->world, s);
        const CTransform* t = e && ecs_get(&ed->world, e, COMP_STATIC_MESH) ? ecs_get(&ed->world, e, COMP_TRANSFORM) : NULL;
        if (!t) continue;
        for (int k = 0; k < 3; k++)
            mn[k] = fminf(mn[k], t->location[k] - 50.0f * t->scale[k]), mx[k] = fmaxf(mx[k], t->location[k] + 50.0f * t->scale[k]);
    }
    if (!(mn[0] < mx[0])) mn[0] = mn[1] = -500.0f, mn[2] = 0.0f, mx[0] = mx[1] = 500.0f, mx[2] = 300.0f;
    uint32_t seed = 777u;
#define RND() ((seed = seed * 1664525u + 1013904223u) >> 8) * (1.0f / 16777216.0f)
    for (uint32_t i = 0; i < n; i++) {
        Entity e = ecs_create(&ed->world, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_LIGHT));
        if (e == ENTITY_NONE) break;
        CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        for (int k = 0; k < 2; k++) {
            float c = 0.5f * (mn[k] + mx[k]), h = 0.5f * (mx[k] - mn[k]) * 0.85f;
            t->location[k] = c + (RND() * 2.0f - 1.0f) * h;
        }
        t->location[2] = mn[2] + (0.35f + 0.5f * RND()) * (mx[2] - mn[2]);
        t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
        t->flags = MOBILITY_STATIONARY;
        CLight* l = ecs_get(&ed->world, e, COMP_LIGHT);
        l->type = LIGHT_POINT;
        l->color[0] = 1.0f, l->color[1] = 0.7f + 0.3f * RND(), l->color[2] = 0.45f + 0.5f * RND();
        l->intensity = 3000.0f + RND() * 5000.0f;
        l->attenuation_radius = 300.0f + RND() * 400.0f;
        l->cast_shadows = 1;
    }
#undef RND
    ed->dirty = true;
}

// The GI tests' scenes (--scene): "house", a closed house in the dark (no sun, no sky) with a spot light on a wall: the
// bounce must reach the floor and the ceiling, nothing may leak through the walls, nothing may flicker
// A material of Content/Materials by its file (rel: "Examples/M_Marble.rmat"): its hash (0: none)
static uint32_t content_material(const char* rel) {
    char path[700];
    snprintf(path, sizeof path, "%s/Materials/%s", content_root(), rel);
    Material* m = mat_load(path);
    return m ? m->hash : 0u;
}

// (A4c, --ssr-test) the default level's floor made a mirror (M_Grid, metallic, roughness 0.04): the screen's reflections
// show the cone, the cube, the sphere and the cylinder standing on it
bool editor_ssr_test_setup(Editor* ed) {
    char src[700], dst[700];
    snprintf(src, sizeof src, "%s/Materials/M_Grid.rmat", content_root());
    snprintf(dst, sizeof dst, "%s/Materials/SSRTest_Mirror.rmat", content_root());
    FILE* f = re_fopen(src, "rb");
    if (!f) return false;
    static char text[1 << 16];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    char* r = strstr(text, "\"Roughness\":[");
    char* m = strstr(text, "\"Metallic\":[");
    if (!r || !m) return false;
    memcpy(r + 13, "0.0400000000", 12); // ("0.649999976," -> "0.0400000000")
    m[12] = '1';
    f = re_fopen(dst, "wb");
    if (!f) return false;
    fwrite(text, 1, n, f);
    fclose(f);
    uint32_t mat = content_material("SSRTest_Mirror.rmat");
    bool set = false;
    for (uint32_t ai = 0; ai < ed->world.archetype_count; ai++) {
        EcsArchetype* a = &ed->world.archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_STATIC_MESH)) || !(a->mask & COMP_BIT(COMP_NAME))) continue;
        CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
        const CName* nm = ecs_arch_column(a, COMP_NAME);
        for (uint32_t i = 0; i < a->count; i++)
            if (!strcmp(nm[i].text, "Floor")) sm[i].material = mat, set = mat != 0;
    }
    ecs_touch_all(&ed->world);
    printf("[RealEngine] SSR test: the floor a mirror (%s)\n", set ? "set" : "FAIL: not set");
    return set;
}

// A rigid body (physics.c) of a shape at (x, y, z)
static Entity body_actor(Editor* ed, int shape, float x, float y, float z, const float color[3], float size) {
    Entity e = mesh_actor(ed, shape, x, y, z, color, 0, 0.55f);
    CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    t->scale[0] = t->scale[1] = t->scale[2] = size;
    t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
    ecs_add(&ed->world, e, COMP_RIGID_BODY);
    return e;
}

// The physics' scene (--scene physics): a pyramid of boxes, a tower, balls rolling down a ramp, a box that breaks on the
// floor. "physics_stress": 1000 boxes falling into a heap (the measure); "physics_stress_shapes": the same with cubes,
// spheres, cylinders and cones (the cylinders and cones by GJK)
static void physics_scene(Editor* ed, bool stress, bool mixed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    float g[3] = { 0.5f, 0.5f, 0.5f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, g, 0, 0.7f);
    CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 80.0f;
    CStaticMesh* fm = ecs_get(&ed->world, fl, COMP_STATIC_MESH);
    fm->material = content_material("M_Grid.rmat");
    if (stress) {
        for (int i = 0; i < 1000; i++) {
            float c[3] = { 0.3f + 0.6f * (float)(i % 7) / 6.0f, 0.4f, 0.9f - 0.6f * (float)(i % 5) / 4.0f };
            static const int mix[4] = { SHAPE_CUBE, SHAPE_CYLINDER, SHAPE_CONE, SHAPE_SPHERE };
            Entity e = body_actor(ed, mixed ? mix[i % 4] : i % 4 == 3 ? SHAPE_SPHERE : SHAPE_CUBE, -450.0f + 100.0f * (float)(i % 10) + (float)(i / 100) * 7.0f,
                                  -450.0f + 100.0f * (float)((i / 10) % 10), 100.0f + 110.0f * (float)(i / 100), c, 0.6f);
            CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
            t->rotation[1] = (float)(i * 37 % 90);
        }
    } else {
        float wood[3] = { 0.62f, 0.42f, 0.25f }, red[3] = { 0.75f, 0.12f, 0.08f }, blue[3] = { 0.15f, 0.3f, 0.8f };
        for (int row = 0; row < 5; row++) // the pyramid: 5 + 4 + 3 + 2 + 1
            for (int i = 0; i < 5 - row; i++) body_actor(ed, SHAPE_CUBE, -400.0f + (float)i * 51.0f + (float)row * 25.5f, 0, 25.0f + (float)row * 50.5f, wood, 0.5f);
        for (int k = 0; k < 6; k++) body_actor(ed, SHAPE_CUBE, 0, 300, 25.0f + (float)k * 50.5f, blue, 0.5f); // the tower
        // the ramp (a static box tilted 20 degrees) and the balls on it
        Entity ramp = mesh_actor(ed, SHAPE_CUBE, 350, -250, 80, g, 0, 0.6f);
        CTransform* rt = ecs_get(&ed->world, ramp, COMP_TRANSFORM);
        rt->scale[0] = 4.0f, rt->scale[1] = 2.0f, rt->scale[2] = 0.2f;
        rt->rotation[0] = -20.0f; // Unreal's pitch: the far end (+X) down
        rt->flags = MOBILITY_STATIC;
        for (int k = 0; k < 3; k++) {
            Entity b = body_actor(ed, SHAPE_SPHERE, 220.0f, -300.0f + (float)k * 50.0f, 200.0f + (float)k * 40.0f, red, 0.4f);
            ((CRigidBody*)ecs_get(&ed->world, b, COMP_RIGID_BODY))->angular_damping = 0.8f; // rolling resistance
        }
        // the box that breaks: dropped from 6 m
        Entity br = body_actor(ed, SHAPE_CUBE, 0, -300, 600, (float[]){ 0.85f, 0.8f, 0.7f }, 1.0f);
        CRigidBody* rb = ecs_get(&ed->world, br, COMP_RIGID_BODY);
        rb->fracture = 1, rb->fracture_speed = 600.0f, rb->fracture_pieces = 3;
        CTransform* bt = ecs_get(&ed->world, br, COMP_TRANSFORM);
        bt->rotation[0] = 20.0f, bt->rotation[2] = 15.0f;
    }
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* st = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    st->rotation[0] = -45.0f;
    st->rotation[1] = 35.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = -150.0f; ed->cam.pos[1] = -1150.0f; ed->cam.pos[2] = 420.0f;
    ed->cam.yaw = 80.0f; ed->cam.pitch = -18.0f;
    ed->dirty = true;
}

// --scene physics_shapes (the test physics_shapes; notas-dev/fisica.md "Formas de colisÃ£o"): the shapes beyond the box
// and the ball. A cylinder and a cone standing, a cone dropped on its side, a capsule falling over, a cylinder rolling
// down a ramp and on along the floor, three convex hulls of an "imported" prism stacked, and boxes and balls on an
// "imported" staircase (not convex) colliding with its triangles (Use Complex Collision). Its meshes go into the project.
enum { SH_CYL_UP = 0, SH_CONE_UP, SH_CONE_SIDE, SH_CAPSULE, SH_ROLLER, SH_GEM0, SH_STEP_BOX0 = SH_GEM0 + 3, SH_STEP_BALL0 = SH_STEP_BOX0 + 4,
       SH_COUNT = SH_STEP_BALL0 + 4 };
static Entity g_sh[SH_COUNT];
static float g_sh0[SH_COUNT][3];

static uint32_t shapes_oct(const float n[3]) { // (the octahedral normal, as the meshes keep it)
    float l1 = fabsf(n[0]) + fabsf(n[1]) + fabsf(n[2]);
    if (l1 < 1e-12f) return 0;
    float x = n[0] / l1, y = n[1] / l1;
    if (n[2] < 0.0f) {
        float ox = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f), oy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox, y = oy;
    }
    int16_t ix = (int16_t)lroundf(fminf(fmaxf(x, -1.0f), 1.0f) * 32767.0f), iy = (int16_t)lroundf(fminf(fmaxf(y, -1.0f), 1.0f) * 32767.0f);
    return (uint16_t)ix | ((uint32_t)(uint16_t)iy << 16);
}

// The materials scene's vertex color sphere (M5): a UV sphere whose vertices carry a rainbow around it and black at its
// bottom (Content/Meshes/Examples/SM_VertexColorSphere.rmesh, RMESH_COLORS); made when missing. True: written now
static bool vcol_sphere_mesh(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/Meshes/Examples/SM_VertexColorSphere.rmesh", content_root());
    if (re_file_stat(path, NULL, NULL, NULL)) return false;
    enum { SEG = 48, RING = 24 };
    uint32_t ntri = SEG * RING * 2;
    GpuVertex* c = calloc((size_t)ntri * 3, sizeof(GpuVertex));
    uint32_t* col = calloc((size_t)ntri * 3, sizeof(uint32_t));
    bool ok = c && col;
    uint32_t t = 0;
    for (int r = 0; ok && r < RING; r++)
        for (int s = 0; s < SEG; s++) {
            int q[4][2] = { { s, r }, { s + 1, r }, { s + 1, r + 1 }, { s, r + 1 } }, tris[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
            for (int h = 0; h < 2; h++, t++)
                for (int k = 0; k < 3; k++) {
                    int su = q[tris[h][k]][0], rv = q[tris[h][k]][1];
                    float u = (float)su / SEG, v = (float)rv / RING, th = u * 6.2831853f, ph = v * 3.1415927f;
                    float n[3] = { sinf(ph) * cosf(th), sinf(ph) * sinf(th), cosf(ph) };
                    GpuVertex* g = &c[t * 3 + k];
                    for (int a = 0; a < 3; a++) g->pos[a] = n[a] * 50.0f;
                    g->normal_oct = shapes_oct(n);
                    g->uv[0] = u * 4.0f, g->uv[1] = v * 2.0f, g->uv2[0] = g->uv2[1] = -1.0f;
                    // the hue around, darker toward the bottom (sRGB bytes, alpha the height)
                    float hue = u * 6.0f, x = 1.0f - fabsf(fmodf(hue, 2.0f) - 1.0f), rgb[3] = { 0, 0, 0 };
                    int sec = (int)hue % 6;
                    static const int pick[6][3] = { { 0, 1, 2 }, { 1, 0, 2 }, { 2, 0, 1 }, { 2, 1, 0 }, { 1, 2, 0 }, { 0, 2, 1 } };
                    rgb[pick[sec][0]] = 1.0f, rgb[pick[sec][1]] = x;
                    float lit = 1.0f - v * 0.85f;
                    uint32_t o = 0;
                    for (int a = 0; a < 3; a++) o |= (uint32_t)(rgb[a] * lit * 255.0f + 0.5f) << (8 * a);
                    col[t * 3 + k] = o | (uint32_t)((1.0f - v) * 255.0f + 0.5f) << 24;
                }
        }
    MeshBuilt mb;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/Meshes/Examples", content_root());
    re_make_dirs(dir);
    ok = ok && mesh_build_corners_col(c, col, ntri, false, &mb);
    if (ok) {
        ok = rmesh_write(path, &mb);
        mesh_built_free(&mb);
    }
    free(c), free(col);
    return ok;
}

// (A9.3c) SM_Eye: an eyeball (1 m across like the engine's sphere; scaled to 2.4 cm) looking along +X, its UV a planar
// projection from the front (the iris at the center: the Eye shading model's), denser rings toward the front
static bool eye_mesh(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/Meshes/Examples/SM_Eye.rmesh", content_root());
    if (re_file_stat(path, NULL, NULL, NULL)) return false;
    enum { SEG = 64, RING = 40 };
    uint32_t ntri = SEG * RING * 2;
    GpuVertex* c = calloc((size_t)ntri * 3, sizeof(GpuVertex));
    if (!c) return false;
    uint32_t t = 0;
    for (int r = 0; r < RING; r++)
        for (int s = 0; s < SEG; s++) {
            int q[4][2] = { { s, r }, { s, r + 1 }, { s + 1, r + 1 }, { s + 1, r } }, tris[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
            for (int h = 0; h < 2; h++, t++)
                for (int k = 0; k < 3; k++) {
                    int su = q[tris[h][k]][0], rv = q[tris[h][k]][1];
                    float v = (float)rv / RING, ph = v * v * 3.1415927f, th = (float)su / SEG * 6.2831853f; // (ph from +X)
                    float n[3] = { cosf(ph), sinf(ph) * cosf(th), sinf(ph) * sinf(th) };
                    GpuVertex* g = &c[t * 3 + k];
                    for (int a = 0; a < 3; a++) g->pos[a] = n[a] * 50.0f;
                    g->normal_oct = shapes_oct(n);
                    g->uv[0] = 0.5f + n[1] * 0.5f, g->uv[1] = 0.5f - n[2] * 0.5f, g->uv2[0] = g->uv2[1] = -1.0f;
                }
        }
    MeshBuilt mb;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/Meshes/Examples", content_root());
    re_make_dirs(dir);
    bool ok = mesh_build_corners(c, ntri, false, &mb);
    if (ok) {
        ok = rmesh_write(path, &mb);
        mesh_built_free(&mb);
    }
    free(c);
    return ok;
}

static void shapes_mesh(const char* name, const float (*tri)[3][3], uint32_t ntri) {
    GpuVertex* c = calloc((size_t)ntri * 3, sizeof(GpuVertex));
    if (!c) return;
    for (uint32_t t = 0; t < ntri; t++) {
        float e1[3], e2[3], n[3];
        for (int k = 0; k < 3; k++) e1[k] = tri[t][1][k] - tri[t][0][k], e2[k] = tri[t][2][k] - tri[t][0][k];
        n[0] = e1[1] * e2[2] - e1[2] * e2[1], n[1] = e1[2] * e2[0] - e1[0] * e2[2], n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int k = 0; k < 3; k++) n[k] = l > 0.0f ? n[k] / l : 0.0f;
        uint32_t o = shapes_oct(n);
        for (int k = 0; k < 3; k++) {
            GpuVertex* g = &c[t * 3 + k];
            memcpy(g->pos, tri[t][k], 12);
            g->normal_oct = o;
            g->uv[0] = (tri[t][k][0] + tri[t][k][1]) * 0.01f, g->uv[1] = tri[t][k][2] * 0.01f;
            g->uv2[0] = g->uv2[1] = -1.0f;
        }
    }
    char path[1100];
    snprintf(path, sizeof path, "%s/Meshes", content_root());
    re_make_dirs(path);
    snprintf(path, sizeof path, "%s/Meshes/%s.rmesh", content_root(), name);
    MeshBuilt mb;
    if (mesh_build_corners(c, ntri, false, &mb)) {
        if (!rmesh_write(path, &mb)) printf("[RealEngine] FAIL: shapes: %s not written\n", name);
        mesh_built_free(&mb);
    }
    free(c);
}

static uint32_t shapes_quad(float (*t)[3][3], uint32_t n, const float a[3], const float b[3], const float c[3], const float d[3]) {
    memcpy(t[n][0], a, 12), memcpy(t[n][1], b, 12), memcpy(t[n][2], c, 12);
    memcpy(t[n + 1][0], a, 12), memcpy(t[n + 1][1], c, 12), memcpy(t[n + 1][2], d, 12);
    return n + 2;
}

static uint32_t shapes_box(float (*t)[3][3], uint32_t n, const float mn[3], const float mx[3]) { // (outward, counterclockwise)
    float c[8][3];
    for (int i = 0; i < 8; i++) c[i][0] = i & 1 ? mx[0] : mn[0], c[i][1] = i & 2 ? mx[1] : mn[1], c[i][2] = i & 4 ? mx[2] : mn[2];
    static const int F[6][4] = { { 0, 4, 6, 2 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 2, 3, 1 }, { 4, 5, 7, 6 } };
    for (int f = 0; f < 6; f++) n = shapes_quad(t, n, c[F[f][0]], c[F[f][1]], c[F[f][2]], c[F[f][3]]);
    return n;
}

static Entity shapes_body(Editor* ed, int idx, int shape, float x, float y, float z, const float col[3], float size) {
    Entity e = body_actor(ed, shape, x, y, z, col, size);
    g_sh[idx] = e;
    g_sh0[idx][0] = x, g_sh0[idx][1] = y, g_sh0[idx][2] = z;
    return e;
}

static void shapes_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    memset(g_sh, 0, sizeof g_sh);
    if (!project_file()[0]) { // (never into the build's Content: the other tests package it)
        printf("[RealEngine] FAIL: shapes: no project open (--new-project): the test meshes are not written\n");
        return;
    }
    // the "imported" meshes: an octagonal prism (60 cm across, 30 high: convex, not a box) and a staircase of 4 steps
    // (25 cm high, 40 deep, 200 wide: four slabs, not convex)
    static float tri[256][3][3];
    uint32_t n = 0;
    for (int i = 0; i < 8; i++) {
        float a0 = 6.2831853f * (float)i / 8.0f, a1 = 6.2831853f * (float)(i + 1) / 8.0f;
        float b0[3] = { 30.0f * cosf(a0), 30.0f * sinf(a0), -15.0f }, b1[3] = { 30.0f * cosf(a1), 30.0f * sinf(a1), -15.0f };
        float t0[3] = { b0[0], b0[1], 15.0f }, t1[3] = { b1[0], b1[1], 15.0f }, tc[3] = { 0, 0, 15.0f }, bc[3] = { 0, 0, -15.0f };
        n = shapes_quad(tri, n, b0, b1, t1, t0);
        memcpy(tri[n][0], tc, 12), memcpy(tri[n][1], t0, 12), memcpy(tri[n][2], t1, 12), n++;
        memcpy(tri[n][0], bc, 12), memcpy(tri[n][1], b1, 12), memcpy(tri[n][2], b0, 12), n++;
    }
    shapes_mesh("SM_TestPrism", (const float(*)[3][3])tri, n);
    n = 0;
    for (int k = 0; k < 4; k++) {
        float mn[3] = { 40.0f * (float)k, -100.0f, 25.0f * (float)k }, mx[3] = { 160.0f, 100.0f, 25.0f * (float)(k + 1) };
        n = shapes_box(tri, n, mn, mx);
    }
    shapes_mesh("SM_TestStairs", (const float(*)[3][3])tri, n);
    render_meshes_reload(ed->vk);
    uint32_t prism = render_mesh_id_of("/Content/Meshes/SM_TestPrism.rmesh"), stairs = render_mesh_id_of("/Content/Meshes/SM_TestStairs.rmesh");
    if (render_mesh_slot(prism) < SHAPE_COUNT || render_mesh_slot(stairs) < SHAPE_COUNT) printf("[RealEngine] FAIL: shapes: the test meshes not loaded\n");
    float g[3] = { 0.5f, 0.5f, 0.5f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 300, 0, g, 0, 0.7f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 40.0f, ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(w, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    float red[3] = { 0.75f, 0.12f, 0.08f }, blue[3] = { 0.15f, 0.3f, 0.8f }, wood[3] = { 0.62f, 0.42f, 0.25f }, green[3] = { 0.2f, 0.6f, 0.25f };
    // standing and falling over (the cylinder and the cone 60 cm, the capsule 30 cm wide and 100 long)
    shapes_body(ed, SH_CYL_UP, SHAPE_CYLINDER, -900, -600, 45, blue, 0.6f);
    shapes_body(ed, SH_CONE_UP, SHAPE_CONE, -700, -600, 45, red, 0.6f);
    Entity cs = shapes_body(ed, SH_CONE_SIDE, SHAPE_CONE, -500, -600, 40, red, 0.6f);
    ((CTransform*)ecs_get(w, cs, COMP_TRANSFORM))->rotation[2] = 90.0f;
    Entity cap = shapes_body(ed, SH_CAPSULE, SHAPE_CYLINDER, -300, -600, 70, green, 1.0f);
    CTransform* ct = ecs_get(w, cap, COMP_TRANSFORM);
    ct->scale[0] = ct->scale[1] = 0.3f, ct->rotation[2] = 80.0f;
    ((CRigidBody*)ecs_get(w, cap, COMP_RIGID_BODY))->collision_shape = COLSHAPE_CAPSULE;
    // the ramp (a box tilted 20 degrees, the far end +X down) and a cylinder on its side rolling down it
    Entity ramp = mesh_actor(ed, SHAPE_CUBE, -600, 0, 80, g, 0, 0.6f);
    CTransform* rt = ecs_get(w, ramp, COMP_TRANSFORM);
    rt->scale[0] = 4.0f, rt->scale[1] = 2.0f, rt->scale[2] = 0.2f, rt->rotation[0] = -20.0f, rt->flags = MOBILITY_STATIC;
    Entity roll = shapes_body(ed, SH_ROLLER, SHAPE_CYLINDER, -720, 0, 162, blue, 0.4f);
    ((CTransform*)ecs_get(w, roll, COMP_TRANSFORM))->rotation[2] = 90.0f; // (its axis along Y: it rolls along X)
    ((CRigidBody*)ecs_get(w, roll, COMP_RIGID_BODY))->angular_damping = 0.8f; // (rolling resistance, as the physics scene's balls)
    // the prisms stacked (2 cm apart)
    for (int k = 0; k < 3; k++) {
        Entity e = shapes_body(ed, SH_GEM0 + k, SHAPE_CUBE, -700, 600, 16.0f + 32.0f * (float)k, wood, 1.0f);
        ((CStaticMesh*)ecs_get(w, e, COMP_STATIC_MESH))->mesh = prism;
    }
    // the staircase: static, its triangles (x -800 to -640; the steps' tops 25, 50, 75, 100) and a box and a ball on each
    Entity st = mesh_actor(ed, SHAPE_CUBE, -800, 1200, 0, g, 0, 0.6f);
    ((CStaticMesh*)ecs_get(w, st, COMP_STATIC_MESH))->mesh = stairs;
    ((CTransform*)ecs_get(w, st, COMP_TRANSFORM))->flags = MOBILITY_STATIC;
    if (ecs_add(w, st, COMP_RIGID_BODY)) {
        CRigidBody* rb = ecs_get(w, st, COMP_RIGID_BODY);
        rb->simulate = 0, rb->complex_collision = 1;
    }
    for (int k = 0; k < 4; k++) {
        float x = -800.0f + 40.0f * (float)k + 20.0f, top = 25.0f * (float)(k + 1);
        shapes_body(ed, SH_STEP_BOX0 + k, SHAPE_CUBE, x, 1160, top + 40.0f, wood, 0.2f);
        shapes_body(ed, SH_STEP_BALL0 + k, SHAPE_SPHERE, x, 1240, top + 60.0f + 30.0f * (float)k, red, 0.2f);
    }
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(w, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -45.0f, lt->rotation[1] = 35.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = -150.0f, ed->cam.pos[1] = -1400.0f, ed->cam.pos[2] = 650.0f;
    ed->cam.yaw = 112.0f, ed->cam.pitch = -24.0f;
    ed->dirty = true;
}

bool editor_shapes_check(Editor* ed) {
    World* w = &ed->world;
    const PhysicsStats* s = physics_stats();
    bool ok = true;
    uint32_t checked = 0;
    for (int i = 0; i < SH_COUNT; i++) {
        const CTransform* t = ecs_alive(w, g_sh[i]) ? ecs_get(w, g_sh[i], COMP_TRANSFORM) : NULL;
        const CRigidBody* rb = t ? ecs_get(w, g_sh[i], COMP_RIGID_BODY) : NULL;
        if (!t || !rb) {
            printf("[RealEngine] FAIL: shapes: body %d is gone\n", i), ok = false;
            continue;
        }
        float R[3][3];
        rotator_axes(t->rotation, R);
        const float* p = t->location;
        float up = R[2][2], drift = sqrtf((p[0] - g_sh0[i][0]) * (p[0] - g_sh0[i][0]) + (p[1] - g_sh0[i][1]) * (p[1] - g_sh0[i][1]));
        const char* what = "";
        float speed = sqrtf(rb->velocity[0] * rb->velocity[0] + rb->velocity[1] * rb->velocity[1] + rb->velocity[2] * rb->velocity[2]);
        bool good = rb->sleeping != 0 || (i == SH_ROLLER && speed < 30.0f); // (the roll's end depends on the simulated time: nearly still)
        float want = 0.0f;
        if (i == SH_CYL_UP || i == SH_CONE_UP) what = i ? "cone standing" : "cylinder standing", want = 30.0f, good &= up > 0.99f && fabsf(p[2] - want) < 2.0f;
        else if (i == SH_CONE_SIDE) what = "cone on its side", good &= fabsf(up + 0.447f) < 0.08f; // (the tip and the base's rim down)
        else if (i == SH_CAPSULE) what = "capsule lying", want = 15.0f, good &= fabsf(up) < 0.1f && fabsf(p[2] - want) < 2.0f;
        else if (i == SH_ROLLER) what = "cylinder rolled", want = 20.0f, good &= fabsf(up) < 0.1f && fabsf(p[2] - want) < 2.0f && p[0] - g_sh0[i][0] > 250.0f;
        else if (i < SH_STEP_BOX0) what = "prism stacked", want = 15.0f + 30.0f * (float)(i - SH_GEM0), good &= fabsf(p[2] - want) < 3.0f && drift < 5.0f;
        else {
            int k = (i - SH_STEP_BOX0) % 4;
            what = i < SH_STEP_BALL0 ? "box on a step" : "ball on a step", want = 25.0f * (float)(k + 1) + 10.0f;
            good &= fabsf(p[2] - want) < 3.0f && drift < 15.0f;
        }
        printf("[RealEngine]   %-17s z %6.1f (want %5.1f) up %6.3f moved %6.1f %s\n", what, p[2], want, up, drift, rb->sleeping ? "asleep" : "AWAKE");
        if (!good) printf("[RealEngine] FAIL: shapes: the %s (body %d)\n", what, i), ok = false;
        checked++;
    }
    printf("[RealEngine] Shapes: %u bodies checked; %u hulls and %u triangle meshes made (%.2f ms, the largest %.2f ms); GJK %u, EPA %u "
           "(fallback %u); peak %.3f ms, %.3f ms a step\n", checked, s->hulls, s->tri_meshes, s->shapes_build_ms, s->shapes_build_peak_ms, s->gjk,
           s->epa, s->epa_fallback, s->ms_peak, s->busy_steps ? s->busy_ms / s->busy_steps : 0.0);
    if (s->hulls < 2 || s->tri_meshes < 1) printf("[RealEngine] FAIL: shapes: the hulls / the staircase's triangles not made\n"), ok = false;
    { // the hull's cost for a big mesh: 100 thousand points on a lumpy ball (as a scanned rock)
        enum { NPT = 100000 };
        float(*pt)[3] = malloc(sizeof(float) * 3 * NPT);
        PhysHull* h = malloc(sizeof(PhysHull));
        if (pt && h) {
            for (int i = 0; i < NPT; i++) {
                float z = 1.0f - 2.0f * ((float)i + 0.5f) / NPT, r = sqrtf(fmaxf(1.0f - z * z, 0.0f)), ph = 2.39996323f * (float)i;
                float rr = 50.0f * (1.0f + 0.15f * sinf(5.0f * ph) * cosf(7.0f * z));
                pt[i][0] = rr * r * cosf(ph) * 1.4f, pt[i][1] = rr * r * sinf(ph), pt[i][2] = rr * z * 0.8f;
            }
            bool built = phys_hull_build(pt, 12, NPT, h);
            printf("[RealEngine] Shapes: a hull of %d points: %s, %u vertices, %u faces, %.2f ms\n", NPT, built ? "made" : "FAILED", h->nv, h->nf, h->build_ms);
            if (!built || h->nv > PHYS_HULL_MAX_V || h->nv < 32) printf("[RealEngine] FAIL: shapes: the big hull\n"), ok = false;
        }
        free(pt), free(h);
    }
    return ok;
}

// (B12c, B12f) after --play in the "river" scene: the crates float (at the surface, not sunk nor flown off) and drifted
// downstream; the Character swims (its head above the surface)
bool editor_river_check(Editor* ed) {
    World* w = &ed->world;
    uint32_t floating = 0, crates = 0, swimming = 0;
    float moved = 1e9f;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        if (a->mask & COMP_BIT(COMP_BUOYANCY))
            for (uint32_t i = 0; i < a->count; i++) {
                crates++;
                if (t[i].location[2] > -60.0f && t[i].location[2] < 60.0f) floating++;
                moved = fminf(moved, t[i].location[0] + 1650.0f - 80.0f * (float)(crates - 1));
            }
        if (a->mask & COMP_BIT(COMP_CHARACTER)) {
            const CCharacter* ch = ecs_arch_column(a, COMP_CHARACTER);
            for (uint32_t i = 0; i < a->count; i++) {
                printf("[RealEngine]   character: at %.0f %.0f %.0f, swimming %u (half height %.0f)\n", t[i].location[0], t[i].location[1],
                       t[i].location[2], ch[i].swimming, ch[i].half_height);
                if (ch[i].swimming && t[i].location[2] > -80.0f && t[i].location[2] < 20.0f) swimming++;
            }
        }
    }
    printf("[RealEngine] River: %u of %u crates floating, the least moved %.0f cm downstream, %u swimming\n", floating, crates, moved, swimming);
    bool ok = crates == 3 && floating == 3 && moved > 150.0f && swimming == 1;
    if (!ok) printf("[RealEngine] FAIL: river: the crates float and drift, the character swims\n");
    return ok;
}

bool editor_physics_check(Editor* ed, bool expect_fracture) {
    const PhysicsStats* s = physics_stats();
    World* w = &ed->world;
    uint32_t below = 0, total = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_RIGID_BODY))) continue;
        const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        const CRigidBody* rbs = ecs_arch_column(a, COMP_RIGID_BODY);
        if (getenv("REALENGINE_PHYSICS_DEBUG"))
            for (uint32_t i = 0; i < a->count; i++)
                if (!rbs[i].sleeping)
                    printf("[RealEngine]   awake: z %.0f v %.1f w %.2f\n", t[i].location[2],
                           sqrtf(rbs[i].velocity[0] * rbs[i].velocity[0] + rbs[i].velocity[1] * rbs[i].velocity[1] + rbs[i].velocity[2] * rbs[i].velocity[2]),
                           sqrtf(rbs[i].angular_velocity[0] * rbs[i].angular_velocity[0] + rbs[i].angular_velocity[1] * rbs[i].angular_velocity[1] +
                                 rbs[i].angular_velocity[2] * rbs[i].angular_velocity[2]));
        const CStaticMesh* sms = (a->mask & COMP_BIT(COMP_STATIC_MESH)) ? ecs_arch_column(a, COMP_STATIC_MESH) : NULL;
        for (uint32_t i = 0; i < a->count; i++, total++)
            if (t[i].location[2] < 0.0f) { // through the floor
                below++;
                uint32_t ms = sms ? render_mesh_slot(sms[i].mesh) : 0;
                printf("[RealEngine]   below: %.0f %.0f %.0f (%s)\n", t[i].location[0], t[i].location[1], t[i].location[2], ms < SHAPE_COUNT ? g_shape_names[ms] : "mesh");
            }
    }
    printf("[RealEngine] Physics: %u bodies (%u awake), %u contacts, %u pieces broken off, %.3f ms (peak %.3f ms, %.3f ms a step over %u busy steps, "
           "settled at %.1f s); GJK %u, EPA %u (fallback %u); %u below the floor\n", s->bodies, s->awake, s->contacts, s->pieces_spawned, s->ms, s->ms_peak,
           s->busy_steps ? s->busy_ms / s->busy_steps : 0.0, s->busy_steps, s->settle_time, s->gjk, s->epa, s->epa_fallback, below);
    bool ok = total > 0 && below == 0 && s->awake * 4 <= s->bodies && (!expect_fracture || s->pieces_spawned > 0);
    if (!ok) printf("[RealEngine] FAIL: the bodies didn't come to rest on the floor\n");
    return ok;
}

// The animation's scene (--scene anim N): N Mannequins walking, running and idling on a grid (each its own phase)
static void anim_scene(Editor* ed, uint32_t n) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    float g[3] = { 0.5f, 0.5f, 0.5f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, g, 0, 0.7f);
    CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 20.0f + sqrtf((float)n) * 2.5f;
    CStaticMesh* fm = ecs_get(&ed->world, fl, COMP_STATIC_MESH);
    fm->material = content_material("M_Grid.rmat");
    const SkelMesh* sk = anim_mesh_name("SK_Mannequin");
    static const char* const clips[3] = { "MM_Walk", "MM_Run", "MM_Idle" };
    uint32_t side = (uint32_t)ceilf(sqrtf((float)n));
    for (uint32_t i = 0; i < n && sk; i++) {
        Entity e = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SKELETAL_MESH));
        if (e == ENTITY_NONE) break;
        snprintf(((CName*)ecs_get(&ed->world, e, COMP_NAME))->text, RE_NAME_MAX, "SkeletalMeshActor%u", i);
        CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        t->location[0] = ((float)(i % side) - (float)side * 0.5f) * 120.0f;
        t->location[1] = ((float)(i / side) - (float)side * 0.5f) * 120.0f;
        t->rotation[1] = (float)(i * 47 % 360);
        t->flags = MOBILITY_MOVABLE;
        CSkeletalMesh* c = ecs_get(&ed->world, e, COMP_SKELETAL_MESH);
        c->mesh = sk->hash;
        const AnimSeq* s = anim_seq_name(clips[i % 3]);
        c->anim = s ? s->hash : 0;
    }
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* st = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    st->rotation[0] = -40.0f;
    st->rotation[1] = 35.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    float far = (float)side * 60.0f;
    ed->cam.pos[0] = -far * 0.2f; ed->cam.pos[1] = -far - 500.0f; ed->cam.pos[2] = 260.0f + far * 0.25f;
    ed->cam.yaw = 85.0f; ed->cam.pitch = -16.0f;
    ed->dirty = true;
}

// An AI Character: the Mannequin, its Anim Blueprint, a Behavior Tree or a Brain
static Entity ai_character(Editor* ed, const char* name, float x, float y, float yaw, uint32_t mode, uint32_t asset, uint32_t team) {
    World* w = &ed->world;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_SKELETAL_MESH) |
                                 COMP_BIT(COMP_AI_AGENT));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = 92.0f, t->rotation[1] = yaw;
    t->flags = MOBILITY_MOVABLE;
    CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    c->max_walk_speed = 450.0f, c->jump_z_velocity = 520.0f, c->gravity_scale = 1.0f, c->radius = 34.0f, c->half_height = 88.0f;
    CSkeletalMesh* sk = ecs_get(w, e, COMP_SKELETAL_MESH);
    const SkelMesh* m = anim_mesh_name("SK_Mannequin");
    const BpAsset* abp = bp_find_name(BPA_CLASS, "ABP_Mannequin");
    sk->mesh = m ? m->hash : 0, sk->anim_class = abp ? abp->hash : 0;
    CAIAgent* a = ecs_get(w, e, COMP_AI_AGENT);
    a->mode = mode, a->asset = asset, a->team = team;
    return e;
}

// The AI's scene (--scene ai): a house with two rooms; a Hunter (BT_Chase) and two Guards (Brain_Guard) of the other
// team: they patrol, see each other through the door, chase, lose sight, investigate
static void ai_scene(Editor* ed) {
    editor_test_scene(ed, "house");
    World* w = &ed->world;
    // the house's spot light is dim for a walk: the sun and the sky through an open roof
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        CTransform* t = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (t && ecs_get(w, e, COMP_STATIC_MESH) && t->location[2] > 300.0f) ecs_destroy(w, e); // the ceiling
    }
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 600 }, 0);
    CTransform* st = ecs_get(w, ed->selected, COMP_TRANSFORM);
    st->rotation[0] = -50.0f, st->rotation[1] = 30.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, 0, 500 }, 0);
    const BehaviorTree* bt = ai_tree_name("BT_Chase");
    const Brain* guard = NULL;
    for (uint32_t i = 0; i < ai_brain_count(); i++)
        if (!strcmp(ai_brain_at(i)->name, "Brain_Guard")) guard = ai_brain_at(i);
    ai_character(ed, "Hunter", -350.0f, -200.0f, 0.0f, AI_MODE_TREE, bt ? bt->hash : 0, 1);
    ai_character(ed, "Guard", 350.0f, 150.0f, 180.0f, AI_MODE_BRAIN, guard ? guard->hash : 0, 2);
    ai_character(ed, "Guard2", 300.0f, -250.0f, 90.0f, AI_MODE_BRAIN, guard ? guard->hash : 0, 2);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = 0.0f; ed->cam.pos[1] = -900.0f; ed->cam.pos[2] = 900.0f;
    ed->cam.yaw = 90.0f; ed->cam.pitch = -50.0f;
}

// --scene ai_squad: a target running across an open floor (team 2) and four hunters of a squad (Brain_Squad, team 1);
// one sees it, the team knows; they aim where it will be, part of them go round, they close a ring (editor_ai_squad_check);
// ai_squad_mm: the hunters animated by Motion Matching (ABP_Mannequin_MM)
static Entity g_squad_runner, g_squad_hunters[4];
static void ai_squad_scene(Editor* ed, bool motion_matching) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, (float[]){ 0.5f, 0.5f, 0.5f }, 0, 0.75f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 40.0f;
    ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(w, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 800 }, 0);
    CTransform* st = ecs_get(w, ed->selected, COMP_TRANSFORM);
    st->rotation[0] = -50.0f, st->rotation[1] = 30.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, 0, 600 }, 0);
    // the runner: a box with a Projectile Movement (straight on, 260 cm/s), an AI of team 2 that does nothing
    Entity r = mesh_actor(ed, SHAPE_CUBE, -1400.0f, -300.0f, 90.0f, (float[]){ 0.9f, 0.25f, 0.15f }, 0, 0.5f);
    set_name(w, r, "Runner");
    ecs_add(w, r, COMP_PROJECTILE);
    ecs_add(w, r, COMP_AI_AGENT);
    CTransform* rt = ecs_get(w, r, COMP_TRANSFORM);
    rt->flags = MOBILITY_MOVABLE;
    rt->scale[0] = rt->scale[1] = 0.6f, rt->scale[2] = 1.8f;
    CProjectile* pj = ecs_get(w, r, COMP_PROJECTILE);
    pj->initial_speed = 260.0f, pj->gravity_scale = 0.0f, pj->active = 1;
    pj->velocity[0] = 1.0f, pj->velocity[1] = pj->velocity[2] = 0.0f;
    CAIAgent* ra = ecs_get(w, r, COMP_AI_AGENT);
    ra->mode = AI_MODE_NONE, ra->team = 2;
    g_squad_runner = r;
    const Brain* squad = NULL;
    for (uint32_t i = 0; i < ai_brain_count(); i++)
        if (!strcmp(ai_brain_at(i)->name, "Brain_Squad")) squad = ai_brain_at(i);
    static const float at[4][3] = { { 900, 900, 220 }, { 1300, -200, 180 }, { 900, -1300, 140 }, { 200, 1500, 250 } };
    for (int i = 0; i < 4; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "Hunter%d", i + 1);
        Entity h = ai_character(ed, nm, at[i][0], at[i][1], at[i][2], AI_MODE_BRAIN, squad ? squad->hash : 0, 1);
        CAIAgent* a = ecs_get(w, h, COMP_AI_AGENT);
        a->sight_radius = i == 3 ? 100.0f : 6000.0f, a->sight_angle = 360.0f; // (the fourth sees nothing: it is told)
        const BpAsset* mm = motion_matching ? bp_find_name(BPA_CLASS, "ABP_Mannequin_MM") : NULL;
        if (mm) ((CSkeletalMesh*)ecs_get(w, h, COMP_SKELETAL_MESH))->anim_class = mm->hash;
        g_squad_hunters[i] = h;
    }
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = 0.0f; ed->cam.pos[1] = -2600.0f; ed->cam.pos[2] = 2400.0f;
    ed->cam.yaw = 90.0f; ed->cam.pitch = -45.0f;
}

bool editor_ai_squad_check(Editor* ed) {
    World* w = &ed->world;
    static AiDebug d[16];
    uint32_t n = ai_debug(d, 16);
    const CTransform* rt = ecs_get(w, g_squad_runner, COMP_TRANSFORM);
    if (!rt) return printf("[RealEngine] FAIL: AI squad: no runner\n"), false;
    uint32_t known = 0, flank = 0, ahead = 0;
    float mean = 0.0f, ang[4];
    uint32_t na = 0;
    for (int i = 0; i < 4; i++) {
        const CTransform* t = ecs_get(w, g_squad_hunters[i], COMP_TRANSFORM);
        if (!t) continue;
        float dx = t->location[0] - rt->location[0], dy = t->location[1] - rt->location[1];
        mean += sqrtf(dx * dx + dy * dy) * 0.25f;
        ang[na++] = atan2f(dy, dx);
        for (uint32_t k = 0; k < n; k++)
            if (d[k].pawn == g_squad_hunters[i]) {
                printf("[RealEngine]   Hunter%d: %s%s at %.0f %.0f\n", i + 1, d[k].what, d[k].sees ? " (sees)" : "", t->location[0], t->location[1]);
                known += d[k].has_target, flank += d[k].flanks;
                ahead += d[k].has_target && d[k].predicted[0] > rt->location[0] + 50.0f; // (aimed ahead: it runs along +x)
            }
    }
    // the largest gap between their angles around the runner: under 180 degrees, they are round it
    for (uint32_t i = 1; i < na; i++)
        for (uint32_t j = i; j > 0 && ang[j] < ang[j - 1]; j--) {
            float t = ang[j];
            ang[j] = ang[j - 1], ang[j - 1] = t;
        }
    float gap = 0.0f;
    for (uint32_t i = 0; i < na; i++) {
        float next = i + 1 < na ? ang[i + 1] : ang[0] + 6.2831853f;
        gap = fmaxf(gap, next - ang[i]);
    }
    printf("[RealEngine] AI squad: %u of 4 know the target, %u flanked, %u aim ahead, mean distance %.0f cm, largest gap %.0f deg, %.3f ms\n",
           known, flank, ahead, mean, gap * 57.29578f, ai_stats()->ms);
    const AnimStats* as = anim_stats();
    printf("[RealEngine] AI squad animation: feet slide %.1f%% of the travel (%.0f cm); Motion Matching %u frames, %u searches, %u switches, %.3f ms in all\n",
           as->foot_slide * 100.0f, as->travel_cm, as->mm_frames, as->mm_searches, as->mm_switches, as->mm_ms);
    bool ok = known == 4 && ahead >= 2 && mean < 700.0f && gap < 3.1415927f;
    if (!ok) printf("[RealEngine] FAIL: AI squad: the hunters should know, aim ahead and close round the runner\n");
    // ai_squad_mm: Motion Matching played (its database, its searches) and the feet kept their place (under 60%)
    const CSkeletalMesh* sk = ecs_get(w, g_squad_hunters[0], COMP_SKELETAL_MESH);
    const BpAsset* mm = bp_find_name(BPA_CLASS, "ABP_Mannequin_MM");
    if (sk && mm && sk->anim_class == mm->hash && (!as->mm_frames || !as->mm_searches || as->foot_slide > 0.6f)) {
        printf("[RealEngine] FAIL: Motion Matching: no database, no search or the feet slide\n");
        ok = false;
    }
    return ok;
}

// The Tlender's .tlend (or an OBJ, a glTF, an FBX) into the project (tlend_import.c): the assets in <Content>/<its name>, its objects placed in the
// level (one undo step), the renderer's meshes and the animation assets read again
static Editor* g_import_ed;

static bool run_importer(const char* src, const char* folder_vpath, World* w, TlendReport* r) {
    const char* ext = strrchr(src, '.');
    bool ok = ext && !_stricmp(ext, ".obj")                                ? obj_import(src, folder_vpath, content_root(), w, r)
              : ext && (!_stricmp(ext, ".gltf") || !_stricmp(ext, ".glb")) ? gltf_import(src, folder_vpath, content_root(), w, r)
              : ext && !_stricmp(ext, ".fbx")                                ? fbx_import(src, folder_vpath, content_root(), w, r)
              : ext && !_stricmp(ext, ".rasset")                             ? rasset_import(src, folder_vpath, content_root(), w, r)
                                                                           : tlend_import(src, folder_vpath, content_root(), w, r);
    imprep_summary(r->text, sizeof r->text); // (Q19) the conversion report's counts at the end of the line
    return ok;
}

// The assets an import wrote, known to the editor: the materials read (again), the meshes and the animations (the
// Skeletal Meshes already loaded from that folder read again: a reimport)
// (Q19) the import's conversion report into Window > Import Report, which opens by itself when something was lost
static void import_report_shown(void) {
    imprep_publish();
    int n = 0;
    const ImpRepItem* it = imprep_items(&n, NULL, NULL);
    for (int i = 0; i < n; i++)
        if (it[i].kind == IMPREP_LOST) *imprep_show_flag() = true;
}

static void import_assets_ready(Editor* ed, const TlendReport* r, bool background, const char* folder) {
    for (unsigned i = 0; i < r->nmat_paths; i++)
        if (!mat_reload_file(r->mat_paths[i])) mat_load(r->mat_paths[i]);
    if (background) render_meshes_reload_async(ed->vk);
    else render_meshes_reload(ed->vk);
    anim_rescan();
    anim_reload_folder(folder);
    content_mark_dirty();
}

bool editor_import_tlend(Editor* ed, const char* src, const char* folder_vpath, bool place) {
    if (place) editor_undo_push(ed);
    World* w = place ? &ed->world : NULL; // (no world: only the assets)
    TlendReport* r = calloc(1, sizeof *r);
    if (!r) return false;
    bool ok = run_importer(src, folder_vpath, w, r);
    import_report_shown(); // (Q19)
    if (ok) {
        import_assets_ready(ed, r, false, folder_vpath);
        if (place) ed->dirty = true;
    }
    printf("[RealEngine] Import %s -> %s: %s\n", src, folder_vpath, r->text);
    editor_status(ed, re_time_seconds(), ok ? TR("Imported: %s") : TR("Could not import: %s"), r->text);
    free(r);
    return ok;
}

// The same in the background (async.h): the importer on a worker into a world of its own (the level untouched
// meanwhile); when it is done its actors are copied into the level (one undo step) and the assets read. One import at
// a time (the importers keep big buffers of their own), the others queued.
typedef struct ImportJob {
    Editor* ed;
    char src[1024], folder[600], name[160];
    char mirror_src[1024], mirror_dst[600]; // (2026-11-23) a dropped folder's (tlend_import_set_mirror), taken when queued
    bool place, ok, world_ok;
    World w;
    TlendReport r;
    struct ImportJob* next;
} ImportJob;
static struct { ImportJob *head, *tail; bool running; } g_imports;

static void import_next(void);

static void import_work(AsyncTask* t, void* arg) {
    ImportJob* j = arg;
    async_status(t, TR("Reading %s and writing its assets..."), j->name);
    if (j->place) j->world_ok = ecs_init(&j->w);
    tlend_import_set_mirror(j->mirror_src, j->mirror_dst);
    j->ok = run_importer(j->src, j->folder, j->world_ok ? &j->w : NULL, &j->r);
    tlend_import_set_mirror(NULL, NULL);
    if (!j->ok) async_fail(t, "%s", j->r.text[0] ? j->r.text : TR("Could not import the file."));
    else async_status(t, "%s", j->r.text);
}

// the import's actors into the level: copies (their components as they are), named apart, selected
static int import_place(Editor* ed, World* src) {
    World* dst = &ed->world;
    int n = 0;
    for (uint32_t i = 1; i < src->slot_count; i++) {
        Entity e = ecs_entity_at(src, i);
        uint64_t m = e ? ecs_mask(src, e) : 0;
        if (!m) continue;
        Entity c = ecs_create(dst, m);
        if (!c) break;
        for (int k = 0; k < COMP_COUNT; k++)
            if (m & COMP_BIT(k)) memcpy(ecs_get(dst, c, (ComponentId)k), ecs_get(src, e, (ComponentId)k), g_components[k].size);
        CName* nm = (m & COMP_BIT(COMP_NAME)) ? ecs_get(dst, c, COMP_NAME) : NULL;
        if (nm) {
            char base[RE_NAME_MAX], uname[RE_NAME_MAX];
            snprintf(base, sizeof base, "%s", nm->text);
            editor_unique_name(ed, base, uname, sizeof uname);
            snprintf(nm->text, sizeof nm->text, "%s", uname);
        }
        editor_select(ed, c, n ? SEL_ADD : SEL_REPLACE);
        n++;
    }
    return n;
}

static void import_done(AsyncTask* t, void* arg, bool ok) {
    ImportJob* j = arg;
    Editor* ed = j->ed;
    import_report_shown(); // (Q19)
    if (ok && j->ok) {
        import_assets_ready(ed, &j->r, true, j->folder);
        if (j->world_ok && j->w.alive) {
            if (ed->playing) { // (the level is the game's now: placed later by dragging the assets in)
                async_status(t, "%s", TR("Assets made; not placed: the game is playing."));
            } else {
                editor_undo_push(ed);
                int n = import_place(ed, &j->w);
                if (n) ed->dirty = true;
            }
        }
        editor_status(ed, re_time_seconds(), TR("Imported: %s"), j->r.text);
    } else if (!async_cancelled(t))
        editor_status(ed, re_time_seconds(), TR("Could not import: %s"), j->r.text);
    printf("[RealEngine] Import (background) %s -> %s: %s\n", j->src, j->folder, j->r.text);
    if (j->world_ok) ecs_shutdown(&j->w);
    free(j);
    g_imports.running = false;
    import_next();
}

static void import_next(void) {
    if (g_imports.running || !g_imports.head) return;
    ImportJob* j = g_imports.head;
    g_imports.head = j->next;
    if (!g_imports.head) g_imports.tail = NULL;
    g_imports.running = true;
    char label[200];
    snprintf(label, sizeof label, TR("Importing %s"), j->name);
    async_run(label, import_work, import_done, j, 0);
}

void editor_import_async(Editor* ed, const char* src, const char* folder_vpath, bool place) {
    ImportJob* j = calloc(1, sizeof *j);
    if (!j) return;
    j->ed = ed, j->place = place;
    snprintf(j->src, sizeof j->src, "%s", src);
    snprintf(j->folder, sizeof j->folder, "%s", folder_vpath);
    tlend_import_get_mirror(j->mirror_src, sizeof j->mirror_src, j->mirror_dst, sizeof j->mirror_dst);
    const char* base = src + strlen(src);
    while (base > src && base[-1] != '/' && base[-1] != '\\') base--;
    snprintf(j->name, sizeof j->name, "%s", base);
    if (g_imports.tail) g_imports.tail->next = j;
    else g_imports.head = j;
    g_imports.tail = j;
    import_next();
}

// (Q6) the Content Browser's Create > Cloth / Vehicle on a Static Mesh: the mesh placed 4 m in front of the camera on the
// ground, selected, then made a cloth (Make Cloth from Selected) or a car (Make Vehicle from Selected); (Q9) a soft body
static void make_soft_from_selected(Editor* ed);
static bool content_actor_cb(const char* mesh_vpath, int make, char* msg, size_t cap) {
    Editor* ed = g_import_ed;
    if (!ed || ed->playing) return snprintf(msg, cap, "%s", TR("Stop playing first.")), false;
    uint32_t mesh = render_mesh_id_of(mesh_vpath);
    if (!mesh) return snprintf(msg, cap, TR("Could not read %s."), mesh_vpath), false;
    World* w = &ed->world;
    editor_undo_push(ed);
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    if (e == ENTITY_NONE) return false;
    const char* base = strrchr(mesh_vpath, '/');
    char name[RE_NAME_MAX], uname[RE_NAME_MAX];
    snprintf(name, sizeof name, "%s", base ? base + 1 : mesh_vpath);
    char* dot = strrchr(name, '.');
    if (dot) *dot = 0;
    editor_unique_name(ed, name, uname, sizeof uname);
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", uname);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    float yaw = ed->cam.yaw * 3.14159265f / 180.0f;
    t->location[0] = ed->cam.pos[0] + cosf(yaw) * 400.0f, t->location[1] = ed->cam.pos[1] + sinf(yaw) * 400.0f, t->location[2] = 0.0f;
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE;
    CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    sm->mesh = mesh;
    sm->base_color[0] = sm->base_color[1] = sm->base_color[2] = 0.7f, sm->roughness = 0.5f, sm->cast_shadow = 1;
    sm->material = content_mesh_material(mesh_vpath);
    float mn[3], mx[3];
    render_mesh_bounds(mesh, mn, mx);
    t->location[2] = -mn[2]; // (standing on the ground)
    editor_select_none(ed);
    editor_select(ed, e, SEL_REPLACE);
    ed->dirty = true;
    if (make == CM_CLOTH) {
        make_cloth_from_selected(ed);
        snprintf(msg, cap, TR("Cloth made from %s: Play (Alt+P) to see it fall."), uname);
        return true;
    }
    if (make == CM_SOFT_BODY) {
        make_soft_from_selected(ed);
        snprintf(msg, cap, TR("Soft body made from %s: Play (Alt+P) to see it; its stiffness and hold in Details > Soft Body."), uname);
        return true;
    }
    char dir[1100], err[200];
    snprintf(dir, sizeof dir, "%s/Vehicles", content_root());
    re_make_dirs(dir);
    Entity v = vehicle_from_actors(w, ed->sel, ed->nsel, VT_CAR, -1, "/Content/Vehicles", err, sizeof err);
    if (v == ENTITY_NONE) return snprintf(msg, cap, TR("Could not make the vehicle: %s"), err), false;
    editor_select_none(ed);
    editor_select(ed, v, SEL_REPLACE);
    snprintf(msg, cap, TR("Vehicle made from %s: Play (Alt+P) drives it; its asset is in /Content/Vehicles."), uname);
    return true;
}
static void content_meshes_cb(void) {
    if (g_import_ed) render_meshes_reload_async(g_import_ed->vk);
}

static bool level_open_cb(const char* path) { return g_import_ed && editor_open_level(g_import_ed, path, re_time_seconds()); }
static void save_all_cb(void) {
    if (g_import_ed) editor_save_all(g_import_ed, re_time_seconds());
}
static void template_ground(Editor* ed, float size);
// Content Browser > Add > Level: an empty level written to path (the open one set aside meanwhile, untouched)
static bool level_create_cb(const char* path) {
    Editor* ed = g_import_ed;
    if (!ed) return false;
    World keep = ed->world;
    Entity sel = ed->selected;
    memset(&ed->world, 0, sizeof ed->world);
    ecs_init(&ed->world);
    template_ground(ed, 50.0f);
    LevelCamera cam = { { -600, 0, 250 }, 0, -12, 90, 600 };
    procgen_purge(&ed->world); // (the procedural world: only its actor saved; made again next frame)
    pcg_purge(&ed->world); // (Q15: what the PCG components made; made again next frame)
    bool ok = level_save(&ed->world, &cam, path, NULL);
    ecs_shutdown(&ed->world);
    ed->world = keep;
    ed->selected = sel;
    return ok;
}

// (2026-11-23) a Static Mesh's material saved in its viewer: the level's actors with that mesh that had its old material
// (or none) take the new one, undoable, the level marked changed
static Editor* g_mesh_mat_ed;
static void mesh_material_saved(const char* mesh_vpath, uint32_t old_mat, uint32_t new_mat) {
    Editor* ed = g_mesh_mat_ed;
    if (!ed || ed->playing) return;
    uint32_t id = render_mesh_id_of(mesh_vpath);
    World* w = &ed->world;
    int n = 0;
    for (uint32_t i = 1; id && i < w->slot_count; i++) {
        Entity e = ecs_entity_at(w, i);
        CStaticMesh* sm = e ? ecs_get(w, e, COMP_STATIC_MESH) : NULL;
        if (!sm || sm->mesh != id || (sm->material != old_mat && sm->material)) continue;
        if (!n) editor_undo_push(ed);
        sm->material = new_mat, ecs_touch(w, e), n++;
    }
    if (n) ed->dirty = true;
    printf("[RealEngine] Mesh material of %s saved: %d actor(s) of the level updated\n", mesh_vpath, n);
}


// (2026-11-23) --mesh-socket-test (after --import of tests/data/import/near): a Static Mesh's sockets saved and read
// back, found on an actor turned and scaled (anim_socket_world), and its material saved from the viewer taking over the
// level's actors that had the old one. True: passed
// (2026-11-23) --rig-fit-test (after --import tests/data/import/humanoid.glb: Mixamo's names, a tail, two ears, each
// bone's skin a tube of a known radius): the rig found (Mixamo, a humanoid, the Unreal names resolved to its bones, the
// tail and the ears as chains), then Generate Bodies: the thigh as thick as its tube (8 cm), the shin (6 cm), nothing
// overlapping at the bind, the tail and the ears as dynamic bones with the Tail and Ear presets
bool editor_rig_fit_test(Editor* ed) {
    (void)ed;
    SkelMesh* m = NULL;
    for (uint32_t i = 0; i < anim_mesh_count(); i++)
        if (anim_mesh_at(i) && anim_mesh_at(i)->nbones >= 30) m = anim_mesh_at(i);
    if (!m) {
        printf("[RealEngine] FAIL: rig fit: no Skeletal Mesh imported\n");
        return false;
    }
    bool ok = true;
    const RigInfo* ri = rig_info(m);
    if (!ri) ok = false, printf("[RealEngine] FAIL: rig fit: no rig found\n");
    static const char* const want[][2] = { { "foot_l", "mixamorig:LeftFoot" }, { "calf_r", "mixamorig:RightLeg" }, { "thigh_l", "mixamorig:LeftUpLeg" },
                                           { "ball_l", "mixamorig:LeftToeBase" }, { "pelvis", "mixamorig:Hips" }, { "hand_r", "mixamorig:RightHand" },
                                           { "spine_02", "mixamorig:Spine1" }, { "head", "mixamorig:Head" } };
    for (size_t k = 0; k < sizeof want / sizeof want[0]; k++) {
        int16_t b = rig_bone_ue(m, want[k][0]);
        if (b < 0 || strcmp(m->bones[b].name, want[k][1])) printf("[RealEngine] FAIL: rig fit: %s is %s, wanted %s\n", want[k][0], b >= 0 ? m->bones[b].name : "(none)", want[k][1]), ok = false;
    }
    int tails = 0, ears = 0;
    for (uint32_t c = 0; ri && c < ri->nchains; c++) tails += ri->chains[c].kind == RIGC_TAIL, ears += ri->chains[c].kind == RIGC_EAR;
    printf("[RealEngine] rig fit: %s: '%s' (%s), %s, %d of %d main roles, %d tail, %d ears, forward (%.2f, %.2f, %.2f), %.2f ms\n", m->name,
           ri ? ri->convention : "-", ri ? rig_system_label(ri->system) : "-", ri ? rig_class_label(ri->rig_class) : "-", ri ? ri->main_mapped : 0,
           ri ? ri->main_total : 0, tails, ears, ri ? ri->forward[0] : 0.0f, ri ? ri->forward[1] : 0.0f, ri ? ri->forward[2] : 0.0f, ri ? ri->ms : 0.0);
    for (uint32_t c = 0; ri && c < ri->nchains; c++)
        printf("[RealEngine] rig fit: chain %s (%s): %s .. %s\n", ri->chains[c].name, rig_chain_kind_label(ri->chains[c].kind), m->bones[ri->chains[c].first].name,
               m->bones[ri->chains[c].last].name);
    if (tails != 1 || ears != 2) printf("[RealEngine] FAIL: rig fit: %d tails and %d ears (wanted 1 and 2)\n", tails, ears), ok = false;
    PhysicsAsset* p = calloc(1, sizeof *p); // (a test's asset, not saved)
    if (!p) return false;
    p->skeleton = m->hash, p->density = 1.0f;
    phys_autogen(p, m);
    const PhysFitStats* st = phys_fit_stats();
    float thigh = -1.0f, shin = -1.0f, arm = -1.0f;
    for (uint32_t i = 0; i < p->nbodies; i++) {
        if (!strcmp(p->bodies[i].bone, "mixamorig:LeftUpLeg")) thigh = p->bodies[i].radius;
        if (!strcmp(p->bodies[i].bone, "mixamorig:LeftLeg")) shin = p->bodies[i].radius;
        if (!strcmp(p->bodies[i].bone, "mixamorig:LeftArm")) arm = p->bodies[i].radius;
    }
    int ctail = 0, cear = 0;
    for (uint32_t c = 0; c < p->nchains; c++) ctail += p->chains[c].preset == DYN_TAIL, cear += p->chains[c].preset == DYN_EAR;
    printf("[RealEngine] rig fit: Generate Bodies: %u bodies (%u fitted to %u skin vertices, %u shrunk apart, %u overlapping), thigh %.1f cm (tube 8), "
           "shin %.1f cm (6), upper arm %.1f cm (5); %u chains (%d Tail, %d Ear); %.2f ms\n", st->bodies, st->fitted, st->vertices, st->shrunk, st->overlaps,
           thigh, shin, arm, p->nchains, ctail, cear, st->ms);
    if (!st->fitted || st->overlaps) printf("[RealEngine] FAIL: rig fit: bodies not fitted to the skin or still overlapping\n"), ok = false;
    if (fabsf(thigh - 8.0f) > 1.5f || fabsf(shin - 6.0f) > 1.5f) printf("[RealEngine] FAIL: rig fit: the leg's bodies aren't as thick as the mesh\n"), ok = false;
    if (ctail != 1 || cear != 2) printf("[RealEngine] FAIL: rig fit: the tail and the ears aren't dynamic bones\n"), ok = false;
    for (uint32_t i = 0; i < p->nbodies; i++)
        if (strstr(p->bodies[i].bone, "Tail") || strstr(p->bodies[i].bone, "Ear") || strstr(p->bodies[i].bone, "Index"))
            printf("[RealEngine] FAIL: rig fit: %s got a ragdoll body (a chain or a finger)\n", p->bodies[i].bone), ok = false;
    free(p);
    printf("[RealEngine] rig fit: %s\n", ok ? "ok" : "FAILED");
    return ok;
}

bool editor_mesh_socket_test(Editor* ed) {
    const char* vp = "/Content/Imported/Meshes/SM_Crate.rmesh";
    uint32_t id = render_mesh_id_of(vp);
    bool ok = id != 0;
    if (!ok) printf("[RealEngine] FAIL: mesh sockets: %s isn't loaded\n", vp);
    AnimSocket s[2];
    memset(s, 0, sizeof s);
    snprintf(s[0].name, sizeof s[0].name, "%s", "Handle");
    s[0].local.t[0] = 50.0f, s[0].local.t[2] = 100.0f, s[0].local.q[3] = 1.0f, s[0].local.s[0] = s[0].local.s[1] = s[0].local.s[2] = 1.0f;
    s[1] = s[0];
    snprintf(s[1].name, sizeof s[1].name, "%s", "Top");
    s[1].local.t[0] = 0.0f;
    ok &= anim_mesh_sockets_save(vp, s, 2);
    AnimSocket back[4];
    uint32_t n = anim_mesh_sockets(vp, true, back, 4);
    if (n != 2 || strcmp(back[0].name, "Handle") || fabsf(back[0].local.t[2] - 100.0f) > 1e-3f) printf("[RealEngine] FAIL: mesh sockets: saved and read back differ (%u)\n", n), ok = false;
    // an actor with it: at (100, 200, 0), turned 90 about z, scaled 2
    World* w = &ed->world;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = 100.0f, t->location[1] = 200.0f, t->scale[0] = t->scale[1] = t->scale[2] = 2.0f, t->rotation[1] = 90.0f;
    CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    sm->mesh = id, sm->material = content_mesh_material(vp);
    uint32_t old = sm->material;
    float rows[3][4];
    bool found = anim_socket_world(w, e, "handle", rows); // (by name, any case)
    // (50, 0, 100) scaled 2 and turned 90 (x to y): (0, 100, 200) + (100, 200, 0)
    float ex = 100.0f, ey = 300.0f, ez = 200.0f;
    if (!found || fabsf(rows[0][3] - ex) > 0.5f || fabsf(rows[1][3] - ey) > 0.5f || fabsf(rows[2][3] - ez) > 0.5f)
        printf("[RealEngine] FAIL: mesh sockets: Handle at (%.1f, %.1f, %.1f), wanted (%.0f, %.0f, %.0f)\n", found ? rows[0][3] : 0.0f, found ? rows[1][3] : 0.0f,
               found ? rows[2][3] : 0.0f, ex, ey, ez), ok = false;
    if (anim_socket_world(w, e, "Nope", rows)) printf("[RealEngine] FAIL: mesh sockets: a socket that isn't there was found\n"), ok = false;
    // an actor hung on "Top" (0, 0, 100): at (100, 200, 200), 10 cm above it by its offset
    Entity c = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
    const float off[3] = { 0, 0, 10 };
    CTransform* ct = c ? ecs_get(w, c, COMP_TRANSFORM) : NULL;
    if (ct) ct->scale[0] = ct->scale[1] = ct->scale[2] = 1.0f;
    bool hung = c && anim_attach_actor_to_socket(w, c, e, "Top", off, NULL);
    ct = c ? ecs_get(w, c, COMP_TRANSFORM) : NULL;
    if (!hung || !ct || fabsf(ct->location[0] - 100.0f) > 0.5f || fabsf(ct->location[1] - 200.0f) > 0.5f || fabsf(ct->location[2] - 220.0f) > 0.5f)
        printf("[RealEngine] FAIL: mesh sockets: the actor hung on Top is at (%.1f, %.1f, %.1f), wanted (100, 200, 220)\n", ct ? ct->location[0] : 0.0f,
               ct ? ct->location[1] : 0.0f, ct ? ct->location[2] : 0.0f), ok = false;
    if (c) anim_detach_socket(w, c);
    // the mesh's material saved: the actor with the old one takes it
    uint32_t nm = mat_vpath_hash("/Content/Imported/Materials/M_M_Barrel.rmat");
    mesh_material_saved(vp, old, nm);
    if (sm->material != nm || content_mesh_material(vp) == nm) printf("[RealEngine] FAIL: mesh material: the actor has %08x (wanted %08x)\n", sm->material, nm), ok = false;
    anim_mesh_sockets_save(vp, NULL, 0); // (the test's file removed)
    printf("[RealEngine] mesh sockets: 2 saved and read back, Handle on a turned and scaled actor, the material over the level: %s\n", ok ? "ok" : "FAILED");
    return ok;
}

static bool import_tlend_cb(const char* src, const char* folder_vpath, bool place, char* msg, size_t cap) {
    if (!g_import_ed) return false;
    editor_import_async(g_import_ed, src, folder_vpath, place); // (the card at the bottom right shows it; the editor keeps going)
    snprintf(msg, cap, "%s", TR("Importing in the background (the card at the bottom right)."));
    return true;
}

// --- The Assistant's actions (assistant_ui.c): an article's buttons ---
static Editor* g_assist_ed;
static void build_lighting(Editor* ed, BakeQuality q, double now);
static void build_hlods(Editor* ed, double now);
static void pie_multi_play(Editor* ed, double now); // (B13: below)
static void pie_multi_stop(void);
static void editor_landscape_mode(Editor* ed, double now);
static void editor_foliage_mode(Editor* ed, double now);
static bool name_is(const char* a, const char* b) { // (any case, spaces ignored: "PointLight" is "Point Light")
    for (;;) {
        while (*a == ' ') a++;
        while (*b == ' ') b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++, b++;
    }
}
static bool assistant_action(const char* act) {
    Editor* ed = g_assist_ed;
    if (!ed) return false;
    double now = re_time_seconds();
    if (!strncmp(act, "open:", 5)) {
        const char* w = act + 5;
        if (!strcmp(w, "package")) ed->show_package = true;
        else if (!strcmp(w, "prefs")) ed->show_prefs = true;
        else if (!strcmp(w, "project")) ed->show_project_settings = true;
        else if (!strcmp(w, "outliner")) ed->show_outliner = true;
        else if (!strcmp(w, "details")) ed->show_details = true;
        else if (!strcmp(w, "place")) ed->show_place = true;
        else if (!strcmp(w, "content")) ed->content_docked ? (void)0 : (void)(ed->drawer_open = true);
        else if (!strcmp(w, "log")) ed->show_log = true;
        else if (!strcmp(w, "stats")) ed->show_stats = true;
        else if (!strcmp(w, "debug")) dv_set_open(true);
        else if (!strcmp(w, "api")) script_api_reference_open(NULL);
        else if (!strcmp(w, "utilities")) editor_utilities_open();
        else if (!strcmp(w, "browser")) ed->show_browser = !ed->playing;
        else if (!strcmp(w, "platforms")) *look_platforms_flag() = true; // (Q26) Platform Support
        else if (!strcmp(w, "modes")) *look_modes_flag() = true;
        else return false;
        return true;
    }
    if (!strncmp(act, "run:", 4)) {
        const char* w = act + 4;
        if (!strcmp(w, "play")) { if (!ed->playing) editor_play(ed, now); }
        else if (!strcmp(w, "build-lighting")) { if (!ed->playing && gi_available() && !gi_baking()) build_lighting(ed, BAKE_PREVIEW, now); }
        else if (!strcmp(w, "landscape-mode")) editor_landscape_mode(ed, now);
        else if (!strcmp(w, "foliage-mode")) editor_foliage_mode(ed, now);
        else if (!strcmp(w, "measure")) ed->bench_request = 3.0;
        else return false;
        return true;
    }
    if (!strncmp(act, "add:", 4)) { // an actor in front of the camera, like Place Actors
        const char* w = act + 4;
        if (ed->playing) return false;
        for (int s = 0; s < SHAPE_COUNT; s++)
            if (name_is(w, g_shape_names[s])) return add_actor(ed, SPAWN_SHAPE, s, now), true;
        for (int l = 0; l < LIGHT_TYPE_COUNT; l++)
            if (name_is(w, g_light_names[l])) return add_actor(ed, SPAWN_LIGHT, l, now), true;
        for (int k = 0; k < PROTO_KIND_COUNT; k++)
            if (name_is(w, g_proto_kind_names[k])) return add_actor(ed, SPAWN_PROTO, k, now), true;
        static const struct { const char* n; SpawnKind k; } fixed[] = {
            { "Empty", SPAWN_EMPTY }, { "PlayerStart", SPAWN_PLAYER_START }, { "Camera", SPAWN_CAMERA }, { "Fog", SPAWN_FOG },
            { "Cloud", SPAWN_CLOUD }, { "PostProcess", SPAWN_POST }, { "Crowd", SPAWN_CROWD }, { "Landscape", SPAWN_LANDSCAPE },
            { "Trigger", SPAWN_TRIGGER }, { "Foliage", SPAWN_FOLIAGE }, { "AmbientSound", SPAWN_AMBIENT_SOUND }, { "AudioVolume", SPAWN_AUDIO_VOLUME }, { "Decal", SPAWN_DECAL }, { "Fire", SPAWN_FIRE }, { "River", SPAWN_RIVER }, { "LocalFogVolume", SPAWN_FOG_VOLUME }, { "GaussianSplat", SPAWN_SPLAT }, { "HeterogeneousVolume", SPAWN_VOLUME }, { "WorldPartition", SPAWN_WORLD_PARTITION },
            { "NavMeshBoundsVolume", SPAWN_NAV }, { "Pickup", SPAWN_PICKUP }, { "Checkpoint", SPAWN_CHECKPOINT },
            { "GameRules", SPAWN_GAME_RULES },
        };
        if (name_is(w, "NavLinkProxy")) return add_actor(ed, SPAWN_NAV, NAV_LINK_PROXY, now), true;
        for (int k = 0; k < DEV_KIND_COUNT; k++) // (interaction.h: add:Door, add:Chest, add:Lever, add:Button)
            if (name_is(w, g_device_names[k])) return add_actor(ed, SPAWN_DEVICE, k, now), true;
        if (name_is(w, "NavModifierVolume")) return add_actor(ed, SPAWN_NAV, NAV_MODIFIER_VOLUME, now), true;
        for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
            if (name_is(w, fixed[i].n)) return add_actor(ed, fixed[i].k, 0, now), true;
        return false;
    }
    if (!strncmp(act, "suggest:", 8)) { // the question it couldn't answer, into Send a Suggestion (the user sends it)
        char body[600];
        snprintf(body, sizeof body, "%s\n\n%s", TR("A question the Assistant couldn't answer:"), act + 8);
        services_suggest_prefill(TR("Assistant: an article missing"), body);
        return true;
    }
    return false;
}

// --- Projects (project.h, project_ui.c): opening one, and the templates a new one starts from ---

static void default_level(Editor* ed);
static void undo_clear(Editor* ed);

// The common ground of a template: a big grid floor, the sun and the sky, a Player Start
static void template_ground(Editor* ed, float size) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, (float[]){ 0.5f, 0.5f, 0.5f }, 0, 0.75f);
    CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = size;
    ft->flags = MOBILITY_STATIC;
    set_name(&ed->world, fl, "Floor");
    ((CStaticMesh*)ecs_get(&ed->world, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, -300, 800 }, 0);
    CLight* sun = ecs_get(&ed->world, ed->selected, COMP_LIGHT);
    sun->color[0] = sun->color[1] = sun->color[2] = 1.0f;
    CTransform* st = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    st->rotation[0] = -42.0f, st->rotation[1] = 30.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 600 }, 0);
    spawn_actor(ed, SPAWN_PLAYER_START, 0, (float[]){ 0, 0, 100 }, 0);
}

// A static block of the course (ramps, steps, walls), grey
static Entity block(Editor* ed, float x, float y, float z, float sx, float sy, float sz, float pitch, const float color[3]) {
    Entity e = mesh_actor(ed, SHAPE_CUBE, x, y, z, color, 0, 0.6f);
    CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    t->scale[0] = sx, t->scale[1] = sy, t->scale[2] = sz, t->rotation[0] = pitch;
    t->flags = MOBILITY_STATIC;
    return e;
}

// The Third Person template's playground: ramps, steps, a platform, boxes (Unreal's grey course)
static void course(Editor* ed) {
    const float g[3] = { 0.62f, 0.62f, 0.64f }, b[3] = { 0.25f, 0.42f, 0.75f };
    block(ed, 700, 0, 100, 4, 6, 2, 0, g);           // a platform
    block(ed, 450, 0, 45, 3, 6, 0.3f, 18.0f, g);     // its ramp
    for (int i = 0; i < 6; i++) block(ed, -400 - (float)i * 40, 500, 10 + (float)i * 20, 0.4f, 3, 0.2f + 0.4f * (float)i, 0, g); // steps
    block(ed, 0, -900, 150, 20, 0.4f, 3, 0, g);      // a wall
    for (int i = 0; i < 5; i++) {
        Entity c = mesh_actor(ed, SHAPE_CUBE, -300 + (float)i * 130, -300, 50, b, 0, 0.4f);
        ecs_add(&ed->world, c, COMP_RIGID_BODY); // boxes to push around (Simulate Physics)
        ((CTransform*)ecs_get(&ed->world, c, COMP_TRANSFORM))->flags = MOBILITY_MOVABLE;
    }
}

static BpAsset* template_class(const char* folder, const char* name, const char* parent) {
    BpAsset* a = bp_find_name(BPA_CLASS, name);
    if (a) return a;
    char dir[700];
    snprintf(dir, sizeof dir, "%s%s", content_root(), folder + 8); // "/Content/X" -> <root>/X
    re_make_dirs(dir);
    return bp_create(BPA_CLASS, folder, name, parent);
}

static int scs_index(BpAsset* a, const char* name) {
    for (uint32_t i = 0; i < a->nscs; i++)
        if (!strcmp(a->scs[i].name, name)) return (int)i;
    return -1;
}

// The Camera Boom's lag on (Enable Camera Lag, Unreal's): a smooth follow, steps and turns eased (the templates' and
// the demos' players; a class made by hand keeps Unreal's default, off)
static void camera_lag_on(BpAsset* c) {
    int boom = c ? scs_index(c, "CameraBoom") : -1;
    if (boom < 0) return;
    CSpringArm* arm = (CSpringArm*)c->scs[boom].data;
    arm->enable_camera_lag = 1, arm->camera_lag_speed = 12.0f;
}

#include "demo_kit.inc" // the demos' CC0 assets (DemoContent) made the project's
#include "demo_games.inc"
#include "haunted_house.inc" // (--scene haunted: the CC0 haunted house, the engine's realistic interior measure)

// (2026-11-23) The Third Person template's Space: Try Traversal (vault, mantle, climb what's ahead), Jump when it can't
// (Unreal's Game Animation Sample)
static void template_traversal_on_jump(BpAsset* c) {
    BpGraph* g = c->ngraphs ? &c->graphs[0] : NULL;
    uint32_t key = 0, jump = 0;
    for (uint32_t i = 0; g && i < g->nnodes; i++) {
        if (g->nodes[i].kind == BPK_EVENT_KEY && !strcmp(g->nodes[i].text, "Space")) key = g->nodes[i].id;
        if (g->nodes[i].kind == BPK_JUMP) jump = g->nodes[i].id;
    }
    if (!key || !jump) return;
    bp_unlink_pin(g, key, 0);
    BpNode* tn = bp_add_node(c, g, BPK_TRY_TRAVERSAL, -600, 520, NULL, NULL);
    uint32_t tr = tn ? tn->id : 0;
    BpNode* bn = bp_add_node(c, g, BPK_BRANCH, -300, 520, NULL, NULL);
    uint32_t br = bn ? bn->id : 0;
    if (!tr || !br) return;
    bp_link(g, key, 0, tr, 0);
    bp_link(g, tr, 5, br, 0), bp_link(g, tr, 6, br, 1); // (its exec, Return Value -> Branch)
    bp_link(g, br, 3, jump, 0);                        // (False: nothing to traverse, a jump)
}

void editor_build_template(Editor* ed, int tpl) {
    ProjectSettings* ps = project();
    ps->default_pawn[0] = 0;
    switch (tpl) {
    case PROJECT_TEMPLATE_THIRD_PERSON: {
        template_ground(ed, 60.0f);
        course(ed);
        BpAsset* c = template_class("/Content/ThirdPerson", "BP_ThirdPersonCharacter", "Character");
        if (c) {
            camera_lag_on(c);
            template_traversal_on_jump(c);
            bp_save(c);
            snprintf(ps->default_pawn, sizeof ps->default_pawn, "%s", c->name);
            bp_spawn(&ed->world, c, (float[3]){ 0, 0, 100 }, (float[3]){ 0, 0, 0 });
        }
        break;
    }
    case PROJECT_TEMPLATE_FIRST_PERSON: {
        template_ground(ed, 60.0f);
        course(ed);
        // the projectile: a small ball the Projectile Movement throws, that bounces
        BpAsset* pr = template_class("/Content/FirstPerson", "BP_Projectile", "Actor");
        if (pr) {
            CTransform* t = (CTransform*)pr->comp_data[COMP_TRANSFORM];
            t->scale[0] = t->scale[1] = t->scale[2] = 0.12f;
            CStaticMesh* m = (CStaticMesh*)pr->comp_data[COMP_STATIC_MESH];
            m->mesh = SHAPE_SPHERE, m->base_color[0] = 0.95f, m->base_color[1] = 0.55f, m->base_color[2] = 0.1f, m->roughness = 0.3f;
            m->emissive[0] = 2.0f, m->emissive[1] = 0.8f;
            pr->comp_mask |= COMP_BIT(COMP_PROJECTILE);
            ecs_component_defaults(COMP_PROJECTILE, pr->comp_data[COMP_PROJECTILE]);
            CProjectile* q = (CProjectile*)pr->comp_data[COMP_PROJECTILE];
            q->initial_speed = 3500.0f, q->should_bounce = 1, q->bounciness = 0.5f, q->gravity_scale = 0.6f;
            bp_save(pr);
        }
        BpAsset* c = template_class("/Content/FirstPerson", "BP_FirstPersonCharacter", "Character");
        if (c) {
            // the camera at the eyes, turned by the mouse (a boom of length 0); a weapon in front of it. (Q12) A First
            // Person Camera (the Realistic preset: the bob in step with the feet, the look's inertia, the lean, breathing,
            // the landing, autofocus) and the body seen: the Mannequin a little behind the eyes, its head hidden, facing
            // where the player looks (Use Controller Rotation Yaw)
            int mesh = scs_index(c, "Mesh");
            if (mesh >= 0) c->scs[mesh].location[0] = -14.0f;
            ((CCharacter*)c->comp_data[COMP_CHARACTER])->use_controller_yaw = 1;
            int boom = scs_index(c, "CameraBoom"), cam = scs_index(c, "FollowCamera");
            if (boom >= 0) {
                ((CSpringArm*)c->scs[boom].data)->target_arm_length = 0.0f;
                ((CSpringArm*)c->scs[boom].data)->do_collision_test = 0;
                c->scs[boom].location[2] = 64.0f;
            }
            if (cam >= 0) {
                CCamera* fc = (CCamera*)c->scs[cam].data;
                fpcam_apply_preset(fc, FPP_REALISTIC);
                fc->fp |= mesh >= 0 ? FP_BODY : 0u;
            }
            if (cam >= 0 && c->nscs < BP_MAX_SCS) {
                int gun = bp_scs_add(c, SCS_STATIC_MESH, cam, "Weapon");
                if (gun >= 0) {
                    c->scs[gun].location[0] = 48.0f, c->scs[gun].location[1] = 14.0f, c->scs[gun].location[2] = -15.0f;
                    c->scs[gun].scale[0] = 0.3f, c->scs[gun].scale[1] = 0.035f, c->scs[gun].scale[2] = 0.05f;
                    CStaticMesh* gm = (CStaticMesh*)c->scs[gun].data;
                    gm->base_color[0] = gm->base_color[1] = gm->base_color[2] = 0.08f, gm->metallic = 0.9f, gm->roughness = 0.3f;
                }
            }
            CStaticMesh* body = (CStaticMesh*)c->comp_data[COMP_STATIC_MESH];
            (void)body;
            c->comp_mask &= ~COMP_BIT(COMP_STATIC_MESH);
            // Left Mouse Button: a projectile from the eyes, along the view
            BpGraph* g = &c->graphs[0];
            BpNode* key = bp_add_node(c, g, BPK_EVENT_KEY, -760, 600, "Left Mouse Button", NULL);
            BpNode* loc = bp_add_node(c, g, BPK_GET_LOCATION, -760, 760, NULL, NULL);
            BpNode* fwd = bp_add_node(c, g, BPK_CONTROL_FORWARD, -760, 860, NULL, NULL);
            BpNode* mul = bp_add_node(c, g, BPK_VMULS, -520, 860, NULL, NULL);
            BpNode* up = bp_add_node(c, g, BPK_MAKE_VECTOR, -520, 960, NULL, NULL);
            BpNode* add1 = bp_add_node(c, g, BPK_VADD, -300, 780, NULL, NULL);
            BpNode* add2 = bp_add_node(c, g, BPK_VADD, -120, 820, NULL, NULL);
            BpNode* rot = bp_add_node(c, g, BPK_GET_CONTROL_ROTATION, -120, 960, NULL, NULL);
            BpNode* sp = bp_add_node(c, g, BPK_SPAWN, 120, 620, "BP_Projectile", NULL);
            if (key && loc && fwd && mul && up && add1 && add2 && rot && sp) {
                mul->pins[1].lit.v[0] = 90.0f;
                up->pins[2].lit.v[0] = 64.0f;
                bp_link(g, fwd->id, 0, mul->id, 0);
                bp_link(g, loc->id, 1, add1->id, 0);
                bp_link(g, mul->id, 2, add1->id, 1);
                bp_link(g, add1->id, 2, add2->id, 0);
                bp_link(g, up->id, 3, add2->id, 1);
                bp_link(g, key->id, 0, sp->id, 0);
                bp_link(g, add2->id, 2, sp->id, 1);
                bp_link(g, rot->id, 0, sp->id, 2);
                snprintf(key->comment, sizeof key->comment, "Fire: a BP_Projectile from the eyes, along the view");
                BpNode* kick = bp_add_node(c, g, BPK_FP_RECOIL, 380, 620, NULL, NULL); // (Q12) the shot's kick on the First Person Camera
                int sp_out = -1, kick_in = -1;
                for (int pi = 0; kick && pi < sp->npins; pi++)
                    if (sp->pins[pi].type == BPT_EXEC && sp->pins[pi].out && sp_out < 0) sp_out = pi;
                for (int pi = 0; kick && pi < kick->npins; pi++)
                    if (kick->pins[pi].type == BPT_EXEC && !kick->pins[pi].out && kick_in < 0) kick_in = pi;
                if (sp_out >= 0 && kick_in >= 0) bp_link(g, sp->id, sp_out, kick->id, kick_in);
            }
            bp_save(c);
            snprintf(ps->default_pawn, sizeof ps->default_pawn, "%s", c->name);
            bp_spawn(&ed->world, c, (float[3]){ 0, 0, 100 }, (float[3]){ 0, 0, 0 });
        }
        break;
    }
    case PROJECT_TEMPLATE_TOP_DOWN: {
        template_ground(ed, 60.0f);
        course(ed);
        BpAsset* c = template_class("/Content/TopDown", "BP_TopDownCharacter", "Character");
        if (c) {
            // the camera high above, looking down at a fixed angle; the mouse doesn't turn it
            int boom = scs_index(c, "CameraBoom");
            if (boom >= 0) {
                CSpringArm* s = (CSpringArm*)c->scs[boom].data;
                s->target_arm_length = 1400.0f, s->use_pawn_control_rotation = 0, s->do_collision_test = 0;
                c->scs[boom].rotation[0] = -55.0f;
            }
            BpGraph* g = &c->graphs[0];
            for (uint32_t i = 0; i < g->nnodes;) // no mouse look
                if (g->nodes[i].kind == BPK_YAW_INPUT || g->nodes[i].kind == BPK_PITCH_INPUT ||
                    (g->nodes[i].kind == BPK_AXIS && (!strcmp(g->nodes[i].text, "Turn") || !strcmp(g->nodes[i].text, "LookUp"))))
                    bp_remove_node(g, g->nodes[i].id);
                else
                    i++;
            bp_save(c);
            snprintf(ps->default_pawn, sizeof ps->default_pawn, "%s", c->name);
            bp_spawn(&ed->world, c, (float[3]){ 0, 0, 100 }, (float[3]){ 0, 0, 0 });
        }
        // guards that patrol, see the player and chase (the Real Brain)
        const Brain* guard = NULL;
        for (uint32_t i = 0; i < ai_brain_count(); i++)
            if (!strcmp(ai_brain_at(i)->name, "Brain_Guard")) guard = ai_brain_at(i);
        for (int i = 0; i < 3 && guard; i++) {
            char nm[32];
            snprintf(nm, sizeof nm, "Guard%d", i + 1);
            ai_character(ed, nm, 900.0f - (float)i * 600.0f, 900.0f - (float)(i % 2) * 1800.0f, 180.0f, AI_MODE_BRAIN, guard->hash, 2);
        }
        break;
    }
    case PROJECT_TEMPLATE_PHYSICS: {
        physics_scene(ed, false, false);
        BpAsset* c = template_class("/Content/ThirdPerson", "BP_ThirdPersonCharacter", "Character");
        if (c) {
            bp_save(c);
            snprintf(ps->default_pawn, sizeof ps->default_pawn, "%s", c->name);
            bp_spawn(&ed->world, c, (float[3]){ -600, -500, 100 }, (float[3]){ 0, 0, 0 });
        }
        break;
    }
    case PROJECT_TEMPLATE_RPG: demo_rpg(ed); break;
    case PROJECT_TEMPLATE_HORROR: demo_horror(ed); break;
    case PROJECT_TEMPLATE_FIGHTING: demo_fighting(ed); break;
    case PROJECT_TEMPLATE_AR: { // Augmented Reality: a floor (hidden in AR: the real one), a thing to place by touch
        template_ground(ed, 40.0f);
        ps->android_ar = true, ps->android_ar_occlusion = true;
        BpAsset* ob = template_class("/Content/AR", "BP_ARObject", "Actor"); // what the touch places: a 30 cm box
        if (ob) {
            CTransform* t = (CTransform*)ob->comp_data[COMP_TRANSFORM];
            t->scale[0] = t->scale[1] = t->scale[2] = 0.3f;
            CStaticMesh* m = (CStaticMesh*)ob->comp_data[COMP_STATIC_MESH];
            m->mesh = SHAPE_CUBE, m->base_color[0] = 0.85f, m->base_color[1] = 0.35f, m->base_color[2] = 0.08f, m->roughness = 0.45f;
            bp_save(ob);
        }
        BpAsset* pl = template_class("/Content/AR", "BP_ARPlacer", "Actor"); // the touch: Event Touch -> AR Line Trace -> Spawn
        if (pl) {
            pl->comp_mask &= ~COMP_BIT(COMP_STATIC_MESH);
            BpGraph* g = &pl->graphs[0];
            BpNode* tch = bp_add_node(pl, g, BPK_EVENT_TOUCH, -900, 300, NULL, NULL);
            BpNode* brk = bp_add_node(pl, g, BPK_BREAK_VECTOR, -640, 420, NULL, NULL);
            BpNode* hit = bp_add_node(pl, g, BPK_AR_HIT, -420, 420, NULL, NULL);
            BpNode* br = bp_add_node(pl, g, BPK_BRANCH, -160, 300, NULL, NULL);
            BpNode* up = bp_add_node(pl, g, BPK_MAKE_VECTOR, -160, 520, NULL, NULL);
            BpNode* add = bp_add_node(pl, g, BPK_VADD, 60, 460, NULL, NULL);
            BpNode* sp = bp_add_node(pl, g, BPK_SPAWN, 260, 300, "BP_ARObject", NULL);
            if (tch && brk && hit && br && up && add && sp) {
                up->pins[2].lit.v[0] = 15.0f; // (the box's half height: it stands on the floor)
                bp_link(g, tch->id, 0, br->id, 0);
                bp_link(g, tch->id, 2, brk->id, 0);
                bp_link(g, brk->id, 1, hit->id, 0);
                bp_link(g, brk->id, 2, hit->id, 1);
                bp_link(g, hit->id, 2, br->id, 1);
                bp_link(g, hit->id, 3, add->id, 0);
                bp_link(g, up->id, 3, add->id, 1);
                bp_link(g, br->id, 2, sp->id, 0);
                bp_link(g, add->id, 2, sp->id, 1);
                snprintf(tch->comment, sizeof tch->comment, "Touch the real floor: a BP_ARObject stands where the finger points");
            }
            bp_save(pl);
            bp_spawn(&ed->world, pl, (float[3]){ 0, 0, 0 }, (float[3]){ 0, 0, 0 });
        }
        break;
    }
    case PROJECT_TEMPLATE_VR: { // (Q25) Unreal's VR Template, ours: vr_pawn.h's locomotion, hands, laser and grabbing
        template_ground(ed, 40.0f);
        ps->vr.start_in_vr = ps->vr.pc = ps->vr.quest = true;
        const float wood[3] = { 0.42f, 0.27f, 0.15f }, grey[3] = { 0.36f, 0.38f, 0.42f }, blue[3] = { 0.16f, 0.55f, 0.85f };
        block(ed, 150, 0, 40, 0.8f, 1.6f, 0.8f, 0, wood);   // a table (80 cm high)
        block(ed, -400, 350, 20, 3.0f, 3.0f, 0.4f, 0, grey); // a platform to teleport onto
        // cubes to grab and throw (Grabbable, their physics on), a ball held by its grip (Snap to Hand)
        for (int i = 0; i < 3; i++) {
            Entity c = mesh_actor(ed, SHAPE_CUBE, 150, -50.0f + (float)i * 45.0f, 91, blue, 0, 0.4f);
            CTransform* ct = ecs_get(&ed->world, c, COMP_TRANSFORM);
            ct->scale[0] = ct->scale[1] = ct->scale[2] = 0.18f, ct->flags = MOBILITY_MOVABLE;
            ecs_add(&ed->world, c, COMP_RIGID_BODY);
            ecs_add(&ed->world, c, COMP_INTERACTABLE);
            CInteractable* ia = ecs_get(&ed->world, c, COMP_INTERACTABLE);
            ecs_component_defaults(COMP_INTERACTABLE, ia);
            ia->flags = (uint16_t)(IA_ENABLED | IA_GRAB);
            snprintf(ia->prompt, sizeof ia->prompt, "Grab");
            char nm[32];
            snprintf(nm, sizeof nm, "GrabCube%d", i + 1);
            set_name(&ed->world, c, nm);
        }
        const float orange[3] = { 0.9f, 0.45f, 0.1f };
        Entity ball = mesh_actor(ed, SHAPE_SPHERE, 150, 70, 92, orange, 0, 0.35f);
        CTransform* bt = ecs_get(&ed->world, ball, COMP_TRANSFORM);
        bt->scale[0] = bt->scale[1] = bt->scale[2] = 0.08f, bt->flags = MOBILITY_MOVABLE;
        ecs_add(&ed->world, ball, COMP_RIGID_BODY), ecs_add(&ed->world, ball, COMP_INTERACTABLE);
        CInteractable* bi = ecs_get(&ed->world, ball, COMP_INTERACTABLE);
        ecs_component_defaults(COMP_INTERACTABLE, bi);
        bi->flags = (uint16_t)(IA_ENABLED | IA_GRAB | IA_GRAB_SNAP);
        set_name(&ed->world, ball, "GrabBall");
        // a Grab Socket on the table's end: a cube let go near it sits there
        const float gold[3] = { 0.85f, 0.7f, 0.2f };
        Entity sock = mesh_actor(ed, SHAPE_CYLINDER, 150, -110, 81, gold, 0.9f, 0.3f);
        CTransform* st = ecs_get(&ed->world, sock, COMP_TRANSFORM);
        st->scale[0] = st->scale[1] = 0.22f, st->scale[2] = 0.02f, st->location[2] = 91.0f, st->flags = MOBILITY_STATIC | TRANSFORM_NO_COLLISION;
        ecs_add(&ed->world, sock, COMP_INTERACTABLE);
        CInteractable* si = ecs_get(&ed->world, sock, COMP_INTERACTABLE);
        ecs_component_defaults(COMP_INTERACTABLE, si);
        si->flags = (uint16_t)(IA_ENABLED | IA_GRAB_SOCKET);
        set_name(&ed->world, sock, "GrabSocket");
        // a door and a lever the laser and the trigger use (interaction.h's devices)
        spawn_actor(ed, SPAWN_DEVICE, DEV_DOOR, (float[]){ 450, -250, 0 }, 0);
        spawn_actor(ed, SPAWN_DEVICE, DEV_LEVER, (float[]){ 300, 200, 0 }, 0);
        // the player: a Character whose head is the headset (no body: the hands are drawn), auto possessed
        BpAsset* c = template_class("/Content/VR", "BP_VRPawn", "Character");
        if (c) { // (only the hands: no body, no mannequin; its capsule still walks, falls and lands the teleports)
            c->comp_mask &= ~COMP_BIT(COMP_STATIC_MESH);
            int mesh = scs_index(c, "Mesh");
            if (mesh >= 0) {
                bool parent_of_others = false;
                for (uint32_t i = 0; i < c->nscs; i++)
                    if ((int)c->scs[i].parent == mesh) parent_of_others = true;
                if (!parent_of_others) { // (the components after it move down one, their parents' indices too)
                    for (uint32_t i = 0; i < c->nscs; i++)
                        if ((int)c->scs[i].parent > mesh && c->scs[i].parent != BP_SCS_ROOT) c->scs[i].parent--;
                    memmove(&c->scs[mesh], &c->scs[mesh + 1], sizeof c->scs[0] * (c->nscs - (uint32_t)mesh - 1));
                    c->nscs--;
                }
            }
            BpGraph* g = &c->graphs[0];
            BpNode* tp = bp_add_node(c, g, BPK_EVENT_VR_TELEPORTED, -600, 300, NULL, NULL);
            BpNode* hp = bp_add_node(c, g, BPK_VR_HAPTIC, -300, 300, NULL, NULL);
            if (tp && hp) { // (a short buzz in the right hand after each teleport: where the VR events go)
                hp->pins[1].lit.v[0] = 1.0f, hp->pins[2].lit.v[0] = 0.3f, hp->pins[3].lit.v[0] = 0.05f;
                bp_link(g, tp->id, 0, hp->id, 0);
                snprintf(tp->comment, sizeof tp->comment, "The VR pawn: Project Settings > VR sets its locomotion, turning, hands and laser");
            }
            bp_save(c);
            snprintf(ps->default_pawn, sizeof ps->default_pawn, "%s", c->name);
            bp_spawn(&ed->world, c, (float[3]){ -100, -60, 100 }, (float[3]){ 0, 0, 0 });
        }
        break;
    }
    default: template_ground(ed, 40.0f); break; // Blank
    }
    ed->selected = ENTITY_NONE;
    if (tpl == PROJECT_TEMPLATE_HORROR) { // (inside the clinic's foyer, at the eyes: from above, its roof in the dark)
        ed->cam.pos[0] = -480.0f, ed->cam.pos[1] = -420.0f, ed->cam.pos[2] = 170.0f, ed->cam.yaw = 40.0f, ed->cam.pitch = -6.0f;
    } else {
        ed->cam.pos[0] = -900.0f, ed->cam.pos[1] = -700.0f, ed->cam.pos[2] = 600.0f;
        ed->cam.yaw = 38.0f, ed->cam.pitch = -25.0f;
    }
    bp_components_update(&ed->world, 0.0f, false);
}

void (*g_load_progress)(float progress, const char* status);
void (*g_load_splash)(const char* path);
static void load_step(float p, const char* status) {
    if (g_load_progress) g_load_progress(p, status);
}

static void bake_step(int done, int total, const char* name) {
    char s[160];
    snprintf(s, sizeof s, TR("Baking the materials... %s"), name);
    load_step(0.82f + 0.17f * (float)done / (float)(total > 0 ? total : 1), s);
}

void editor_open_project_later(Editor* ed, const char* rproject, int tpl) {
    snprintf(ed->pending_project, sizeof ed->pending_project, "%s", rproject);
    ed->pending_tpl = tpl;
}

bool editor_open_project(Editor* ed, const char* rproject, int tpl) {
    if (ed->playing) editor_stop(ed, re_time_seconds());
    if (!project_load(rproject)) return false;
    online_apply_project(); // Project Settings > Online (Steam starts when set)
    online_init();
    if (g_load_splash) { // the project's loading screen (its picture or video), else the engine's
        char sp[900] = "";
        project_root(sp, sizeof sp);
        const char* s = project()->splash;
        if (s[0] && !strncmp(s, "/Content/", 9)) {
            size_t l = strlen(sp);
            snprintf(sp + l, sizeof sp - l, "/Content%s", s + 8);
        } else
            snprintf(sp, sizeof sp, "%s", s);
        g_load_splash(sp);
    }
    load_step(0.08f, TR("Opening the project..."));
    char root[600], content[700];
    project_root(root, sizeof root);
    snprintf(content, sizeof content, "%s/Content", root);
    char maps[760];
    snprintf(maps, sizeof maps, "%s/Maps", content);
    re_make_dirs(maps);
    // every system to the project's Content: the browser, Blueprints, materials, meshes, animations, AI
    load_step(0.18f, TR("Reading the content..."));
    content_init(content);
    { // (B14) Git: the project's repository (the Content Browser's states, Submit Content)
        char pr[900];
        project_root(pr, sizeof pr);
        sc_init(pr);
        plugins_init(pr, true); // (B14c) the plugins (the project's and the engine's): their nodes before the Blueprints compile
    }
    bp_shutdown();
    bp_init(content_root());
    input_init(content_root(), true); // (before anything reads the Blueprints: an Input Action event's value pin takes its type)
    ui_widgets_init(content_root());
    landscape_init(content_root());
    procgen_init(content_root());
    pcg_init(content_root(), true); // (Q15: the PCG Graphs read on demand; the examples written when missing)
    foliage_init(content_root());
    proto_init(content_root());
    level_stream_init(content_root(), ""); // (the Level Blueprints are found under it)
    load_step(0.32f, TR("Preparing the materials..."));
    mat_set_root(content_root());
    seq_init(content_root()); // the Level Sequences
    mat_make_engine_materials(content_root());
    load_step(0.46f, TR("Loading animations and AI..."));
    anim_init(content_root(), true);
    particles_set_root(content_root());
    ikrig_set_root(content_root());
    vehicle_set_root(content_root());
    ai_init(content_root(), true);
    dlg_init(content_root()); // (the Dialogues: read on demand)
    quest_init(content_root()); // (the Quests: read on demand)
    audio_init(content_root()); // the project's sounds (the mixer runs from the first project on)
    project_apply_audio();
    script_init(content_root()); // the scripts (C#, Python, C, Real): compiled into their classes before the level's actors need them
    load_step(0.6f, TR("Loading the meshes..."));
    render_set_content_root(content_root());
    render_meshes_reload(ed->vk);
    undo_clear(ed);
    ProjectSettings* ps = project();
    load_step(0.78f, TR("Opening the level..."));
    if (tpl >= 0) { // a new project: its template's level, saved as the startup map
        editor_build_template(ed, tpl);
        static const char* const map_names[PROJECT_TEMPLATE_COUNT] = { "Blank", "ThirdPersonMap", "FirstPersonMap", "TopDownMap", "PhysicsMap",
                                                                                     "RPGMap", "HorrorMap", "FightingMap", "ARMap" };
        char path[900];
        snprintf(path, sizeof path, "%s/%s.rlevel", maps, map_names[tpl]);
        editor_save_level(ed, path, re_time_seconds());
        snprintf(ps->editor_startup_map, sizeof ps->editor_startup_map, "/Content/Maps/%s.rlevel", map_names[tpl]);
        snprintf(ps->game_default_map, sizeof ps->game_default_map, "%s", ps->editor_startup_map);
        project_save(NULL);
    } else if (ps->editor_startup_map[0]) {
        char path[900];
        snprintf(path, sizeof path, "%s%s", content_root(), ps->editor_startup_map + 8);
        if (!editor_open_level(ed, path, re_time_seconds())) default_level(ed);
    }
    // (on the loading screen, not as the editor's first frozen seconds: the project's materials are read here, else
    // the first frame's look-up read them all and baked them there, ~4 s frozen as the loading screen faded; 0.17)
    mat_count();
    mat_bake_pending(bake_step);
    project_recent_add(project_file());
    ed->show_browser = false;
    load_step(1.0f, TR("Ready."));
    editor_status(ed, re_time_seconds(), TR("Project %s opened."), ps->name);
    return true;
}

bool editor_ai_check(Editor* ed) {
    World* w = &ed->world;
    static AiDebug d[16];
    uint32_t n = ai_debug(d, 16);
    const AiStats* s = ai_stats();
    const NavInfo* nav = ai_nav_info();
    printf("[RealEngine] AI: %u agents, %.3f ms; navigation %u walkable cells (%.1f ms)\n", s->agents, s->ms, nav->walkable, nav->build_ms);
    const NavMeshStats* nm = navmesh_stats();
    printf("[RealEngine] NavMesh: %u tiles, %u polygons, %s; %u batches (%.1f ms, the slowest tile %.2f ms); %u queries (avg %.1f us, max %.3f ms)\n", nm->tiles,
           nm->polys, navmesh_ready() ? "in use" : "not in use", nm->builds, nm->total_build_ms, nm->max_tile_ms, nm->queries, nm->query_us_avg, nm->query_ms_max);
    uint32_t moved = 0;
    for (uint32_t i = 0; i < n; i++) {
        const CTransform* t = ecs_get(w, d[i].pawn, COMP_TRANSFORM);
        const CAIAgent* a = ecs_get(w, d[i].pawn, COMP_AI_AGENT);
        const CName* nm = ecs_get(w, d[i].pawn, COMP_NAME);
        if (!t || !a) continue;
        printf("[RealEngine]   %s: %s%s at %.0f %.0f\n", nm ? nm->text : "?", d[i].what, d[i].sees ? " (sees its target)" : "", t->location[0], t->location[1]);
        moved += fabsf(t->location[0]) + fabsf(t->location[1]) > 0.0f;
    }
    bool ok = n >= 3 && nav->walkable > 1000;
    if (!ok) printf("[RealEngine] FAIL: the AI agents or the navigation are missing\n");
    return ok;
}

// --scene footik: a Mannequin with its left foot over a 25 cm step (its Anim Blueprint's Foot IK on): played, the left foot
// rises onto the step, the pelvis stays (editor_footik_check)
static Entity g_footik_actor;
static void footik_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    float g[3] = { 0.5f, 0.5f, 0.5f }, step[3] = { 0.35f, 0.42f, 0.55f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, g, 0, 0.7f);
    CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 20.0f;
    ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(&ed->world, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    Entity st = mesh_actor(ed, SHAPE_CUBE, 0.0f, -32.0f, 12.5f, step, 0, 0.6f);
    CTransform* stt = ecs_get(&ed->world, st, COMP_TRANSFORM);
    stt->scale[0] = 0.6f, stt->scale[1] = 0.5f, stt->scale[2] = 0.25f; // (y -57..-7: under the left foot only)
    stt->flags = MOBILITY_STATIC;
    const SkelMesh* sk = anim_mesh_name("SK_Mannequin");
    BpAsset* abp = (BpAsset*)bp_find_name(BPA_CLASS, "ABP_Mannequin");
    AnimGraph* ag = abp ? anim_graph_of(abp, false) : NULL;
    if (ag && !ag->foot_ik) { // (a project made before the Foot IK: on for this run)
        ag->foot_ik = true, ag->foot_reach = 45.0f, ag->rev++;
        snprintf(ag->foot_l, ANIM_NAME, "foot_l"), snprintf(ag->foot_r, ANIM_NAME, "foot_r");
    }
    if (ag && !ag->aim_bones[0][0]) { // (and its Aim Offset chain)
        static const char* const aim[4] = { "spine_01", "spine_02", "spine_03", "head" };
        static const float share[4] = { 0.2f, 0.25f, 0.25f, 0.3f };
        for (int k = 0; k < 4; k++) snprintf(ag->aim_bones[k], ANIM_NAME, "%s", aim[k]), ag->aim_share[k] = share[k];
        ag->rev++;
    }
    bool leg_ik = false;
    for (uint32_t i = 0; ag && i < ag->nnodes; i++) leg_ik |= ag->nodes[i].kind == AN_LEG_IK;
    if (ag && !leg_ik) ag->nnodes = 0, anim_graph_ensure_nodes(ag), ag->rev++; // (the settings above as the graph's nodes)
    Entity e = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SKELETAL_MESH));
    snprintf(((CName*)ecs_get(&ed->world, e, COMP_NAME))->text, RE_NAME_MAX, "Mannequin");
    CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    t->flags = MOBILITY_MOVABLE;
    CSkeletalMesh* c = ecs_get(&ed->world, e, COMP_SKELETAL_MESH);
    c->mesh = sk ? sk->hash : 0, c->anim_class = abp ? abp->hash : 0, c->play_rate = 1.0f, c->loop = 1;
    g_footik_actor = e;
    Entity cam = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CAMERA)); // (the view while playing)
    snprintf(((CName*)ecs_get(&ed->world, cam, COMP_NAME))->text, RE_NAME_MAX, "CameraActor");
    CTransform* ct = ecs_get(&ed->world, cam, COMP_TRANSFORM);
    ct->location[0] = 230.0f, ct->location[1] = -250.0f, ct->location[2] = 105.0f;
    ct->rotation[0] = -6.0f, ct->rotation[1] = 132.0f;
    ct->scale[0] = ct->scale[1] = ct->scale[2] = 1.0f;
    CCamera* cc = ecs_get(&ed->world, cam, COMP_CAMERA);
    cc->fov_deg = 70.0f, cc->auto_activate = 1;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -45.0f, lt->rotation[1] = 40.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = 260.0f; ed->cam.pos[1] = -260.0f; ed->cam.pos[2] = 120.0f;
    ed->cam.yaw = 135.0f; ed->cam.pitch = -8.0f;
}

// --scene ragdoll (notas-dev/ikrig.md): Mannequins with a Physics Asset made from their bones (in memory): 6 dropped as
// ragdolls from 60 cm onto the floor and a step, 3 idling with the bodies below spine_03 simulated (physical animation:
// their arms swing but hold), 3 whose right hand reaches a sphere by an IK Rig (a Chain from the upper arm). Played, the
// ragdolls rest on the floor (none under it, none flying), the hands touch their spheres (editor_ragdoll_check)
static Entity g_rag[12];
static int g_nrag;

// (2026-11-16) --scene active_ragdoll: 50 Mannequins idling as Active Ragdolls (ikrig.h PHYS_ACTIVE), one hit every half
// second (light and strong in turn): they sway, fall, get up; the cost measured (notas-dev/ikrig.md)
#define AR_N 50
static Entity g_ar[AR_N];
static double g_ar_t;
static int g_ar_hits, g_ar_seen[4];
static void active_ragdoll_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    g_ar_t = 0.0, g_ar_hits = 0;
    memset(g_ar_seen, 0, sizeof g_ar_seen);
    float g[3] = { 0.5f, 0.5f, 0.5f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, g, 0, 0.7f);
    CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 40.0f, ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(&ed->world, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    const SkelMesh* sk = anim_mesh_name("SK_Mannequin");
    if (!sk) return;
    PhysicsAsset* pa = phys_temp(sk->hash, "PHYS_Mannequin_Active");
    const AnimSeq* idle = anim_seq_name("MM_Idle");
    for (int i = 0; i < AR_N; i++) {
        Entity e = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SKELETAL_MESH));
        g_ar[i] = e;
        if (e == ENTITY_NONE) break;
        snprintf(((CName*)ecs_get(&ed->world, e, COMP_NAME))->text, RE_NAME_MAX, "Active%d", i);
        CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        t->location[0] = (float)(i % 10) * 160.0f - 720.0f, t->location[1] = (float)(i / 10) * 220.0f - 440.0f, t->location[2] = 0.0f;
        t->rotation[1] = (float)(i * 37 % 360), t->flags = MOBILITY_MOVABLE;
        CSkeletalMesh* c = ecs_get(&ed->world, e, COMP_SKELETAL_MESH);
        c->mesh = sk->hash, c->anim = idle ? idle->hash : 0, c->play_rate = 1.0f, c->loop = 1, c->physics_blend = 1.0f;
        c->physics_asset = pa ? pa->hash : 0, c->simulate = getenv("REALENGINE_AR_OFF") ? 0 : 3; // (Active Ragdoll; _OFF: the measure's "before")
    }
    editor_sky_preset(ed, 0, 0.0); // (a clear day: seen in the screenshots)
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -1500.0f, ed->cam.pos[2] = 500.0f, ed->cam.yaw = 90.0f, ed->cam.pitch = -18.0f;
}
void editor_active_ragdoll_tick(Editor* ed, double dt) {
    if (!ed->playing) return;
    g_ar_t += dt;
    if (g_ar_t < 1.0) return; // (standing first)
    int k = (int)((g_ar_t - 1.0) / 0.5);
    while (g_ar_hits <= k && g_ar_hits < 4 * AR_N) { // one hit every half second: light, then strong, in turn
        int i = (g_ar_hits * 7) % AR_N;
        bool strong = g_ar_hits % 2 == 1;
        float dir = (float)(g_ar_hits % 4) * 1.5707963f;
        float imp[3] = { cosf(dir) * (strong ? 900.0f : 150.0f), sinf(dir) * (strong ? 900.0f : 150.0f), strong ? 120.0f : 0.0f };
        phys_hit_reaction(anim_mesh_entity(&ed->world, g_ar[i]), "spine_03", imp, strong ? 0.1f : 0.5f, 0.8f);
        g_ar_hits++;
    }
    for (int i = 0; i < AR_N; i++) {
        int b = phys_balance_state(anim_mesh_entity(&ed->world, g_ar[i]));
        if (b >= 0 && b < 4) g_ar_seen[b]++;
    }
}
bool editor_active_ragdoll_check(Editor* ed) {
    bool ok = true;
    int counts[4] = { 0 }, bad = 0;
    float fastest = 0.0f;
    for (int i = 0; i < AR_N; i++) {
        IkPhysInfo info;
        if (!ikphys_info(anim_mesh_entity(&ed->world, g_ar[i]), &info)) { bad++; continue; }
        if (info.balance >= 0 && info.balance < 4) counts[info.balance]++;
        if (!(info.zmin == info.zmin) || info.zmin < -20.0f || info.zmax > 260.0f) bad++; // (NaN, under the floor, flying)
        fastest = fmaxf(fastest, info.vmax);
    }
    const IkPhysStats* s = ikphys_stats();
    printf("[RealEngine] Active ragdolls: %d hits; now %d balanced, %d stumbling, %d down, %d getting up; seen fallen %d frames, getting up %d; fastest %.0f cm/s; %u bodies, %u contacts, %.3f ms (CPU, the pose jobs summed)\n",
           g_ar_hits, counts[0], counts[1], counts[2], counts[3], g_ar_seen[2], g_ar_seen[3], fastest, s->bodies, s->contacts, s->ms);
    if (bad) printf("[RealEngine] FAIL: active ragdoll: %d characters without a state, under the floor or flying\n", bad), ok = false;
    if (!g_ar_seen[2] || !g_ar_seen[3]) printf("[RealEngine] FAIL: active ragdoll: the strong hits should knock some down and they should get up\n"), ok = false;
    if (counts[0] < AR_N / 2) printf("[RealEngine] FAIL: active ragdoll: most should stand (%d of %d)\n", counts[0], AR_N), ok = false;
    return ok;
}
// (2026-11-16) --scene hair_dynamic: the head sways and turns (the hair swings behind it); editor_hair_tick moves it
static Entity g_hair_head;
static double g_hair_t;
void editor_hair_tick(Editor* ed, double dt) {
    if (!g_hair_head || !ecs_alive(&ed->world, g_hair_head)) return;
    g_hair_t += dt;
    CTransform* t = ecs_get(&ed->world, g_hair_head, COMP_TRANSFORM);
    if (!t) return;
    float tt = (float)g_hair_t;
    t->location[0] = -22.0f + 9.0f * sinf(tt * 2.2f), t->rotation[1] = -90.0f + 35.0f * sinf(tt * 1.3f);
    ecs_touch(&ed->world, g_hair_head);
}

// (2026-11-16, crashes #135-137) --scene skin_churn: 48 skeletal actors of 5 meshes, each frame 3 of them gone and 3 new
// ones, every 2 s all 48 made again at once (a sublevel added rebuilds every Blueprint's parts): many first bends in one
// frame grow the bones' palette mid-frame. The jobs written before kept the old palette's address, freed: the GPU read
// freed memory, VK_ERROR_DEVICE_LOST in "skin (ossos e BLAS)" (render.c skin_bind). Also the skins' list reordered while
// the frame's jobs name them by place (skin_bind / skin_sweep)
static Entity g_churn[48];
static uint32_t g_churn_n, g_churn_frame;
static Entity churn_spawn(Editor* ed, uint32_t i) {
    World* w = &ed->world;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SKELETAL_MESH));
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "Churn%u", i);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE;
    t->location[0] = -600.0f + 80.0f * (float)(i % 16), t->location[1] = 300.0f + 150.0f * (float)((i / 16) % 4), t->location[2] = 0.0f;
    // (meshes of different sizes: a job left on another skin refit its BLAS with other geometry, which is what hung)
    static const char* const names[5] = { "SK_Mannequin", "SK_Dog", "SK_Bird", "SK_Fish", "SK_Lizard" };
    const SkelMesh* m = anim_mesh_name(names[(i * 3u + g_churn_frame) % 5u]);
    if (!m) m = anim_mesh_name("SK_Mannequin");
    ((CSkeletalMesh*)ecs_get(w, e, COMP_SKELETAL_MESH))->mesh = m ? m->hash : 0;
    return e;
}
static void skin_churn_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    float g[3] = { 0.5f, 0.5f, 0.5f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, g, 0, 0.7f);
    CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 30.0f, ft->flags = MOBILITY_STATIC;
    for (uint32_t i = 0; i < 48; i++) g_churn[i] = churn_spawn(ed, i); // (48 first bends in one frame: the palette grows mid-frame)
    g_churn_n = 48, g_churn_frame = 0;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    ((CTransform*)ecs_get(&ed->world, ed->selected, COMP_TRANSFORM))->rotation[0] = -45.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -700.0f, ed->cam.pos[2] = 250.0f, ed->cam.yaw = 90.0f, ed->cam.pitch = -12.0f;
}
void editor_skin_churn_tick(Editor* ed) {
    if (!g_churn_n) return;
    g_churn_frame++;
    uint32_t n = g_churn_frame % 120u == 60u ? 48u : 3u; // (now and then all of them made again at once, as a sublevel added does)
    for (uint32_t r = 0; r < n; r++) {
        uint32_t i = n == 48u ? r : (g_churn_frame * 7u + r * 5u) % 48u;
        if (g_churn[i] && ecs_alive(&ed->world, g_churn[i])) ecs_destroy(&ed->world, g_churn[i]);
        g_churn[i] = churn_spawn(ed, i);
    }
}

static void ragdoll_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    g_nrag = 0;
    float g[3] = { 0.5f, 0.5f, 0.5f }, step[3] = { 0.35f, 0.42f, 0.55f }, red[3] = { 0.9f, 0.2f, 0.15f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, g, 0, 0.7f);
    CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 30.0f, ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(&ed->world, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    Entity st = mesh_actor(ed, SHAPE_CUBE, -150.0f, 0.0f, 15.0f, step, 0, 0.6f); // (a step the first ragdolls fall across)
    CTransform* stt = ecs_get(&ed->world, st, COMP_TRANSFORM);
    stt->scale[0] = 0.8f, stt->scale[1] = 3.0f, stt->scale[2] = 0.3f, stt->flags = MOBILITY_STATIC;
    const SkelMesh* sk = anim_mesh_name("SK_Mannequin");
    if (!sk) return;
    PhysicsAsset* pa = phys_temp(sk->hash, "PHYS_Mannequin_Test");
    PhysicsAsset* pj = phys_temp(sk->hash, "PHYS_Mannequin_Jiggle");
    for (uint32_t i = 0; pj && i < pj->nbodies; i++) pj->bodies[i].strength = 0.25f; // (pulled toward the animation)
    const AnimSeq* idle = anim_seq_name("MM_Idle");
    for (int i = 0; i < 12; i++) {
        Entity e = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SKELETAL_MESH));
        if (e == ENTITY_NONE) break;
        snprintf(((CName*)ecs_get(&ed->world, e, COMP_NAME))->text, RE_NAME_MAX, i < 6 ? "Ragdoll%d" : i < 9 ? "Jiggle%d" : "Reach%d", i);
        CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        t->location[0] = i < 6 ? -150.0f + (float)(i % 2) * 40.0f : (float)((i - 6) % 3) * 120.0f + 100.0f;
        t->location[1] = i < 6 ? -250.0f + (float)i * 100.0f : i < 9 ? -150.0f : 150.0f;
        t->location[2] = i < 6 ? 60.0f : 0.0f;
        t->rotation[1] = (float)(i * 53 % 360), t->rotation[2] = i < 6 ? (float)(i * 7 % 20) - 10.0f : 0.0f;
        t->flags = MOBILITY_MOVABLE;
        CSkeletalMesh* c = ecs_get(&ed->world, e, COMP_SKELETAL_MESH);
        c->mesh = sk->hash, c->anim = idle ? idle->hash : 0, c->play_rate = 1.0f, c->loop = 1, c->physics_blend = 1.0f;
        if (i < 6) c->physics_asset = pa ? pa->hash : 0, c->simulate = 1;
        else if (i < 9) c->physics_asset = pj ? pj->hash : 0, c->simulate = 2, snprintf(c->simulate_bone, sizeof c->simulate_bone, "spine_03");
        else { // the IK: a rig of its own, its hand's goal following its sphere
            char nm[64];
            snprintf(nm, sizeof nm, "IK_Reach%d", i);
            IkRig* r = ikrig_temp(sk->hash, nm);
            if (r) {
                IkRigGoal* gl = &r->goals[r->ngoals++];
                snprintf(gl->name, ANIM_NAME, "Hand"), snprintf(gl->bone, ANIM_NAME, "hand_r"), snprintf(gl->target_actor, ANIM_NAME, "IKTarget%d", i);
                gl->alpha = 1.0f;
                IkRigSolver* so = &r->solvers[r->nsolvers++];
                so->kind = IKS_CHAIN, so->on = 1, so->iterations = 16;
                snprintf(so->start, ANIM_NAME, "upperarm_r"), snprintf(so->goal, ANIM_NAME, "Hand");
                c->ik_rig = r->hash;
            }
            float cy = cosf(t->rotation[1] * 0.017453293f), sy = sinf(t->rotation[1] * 0.017453293f);
            float lx = 25.0f, ly = 32.0f; // (in front and to the right of the shoulder, within the arm's reach)
            Entity tg = mesh_actor(ed, SHAPE_SPHERE, t->location[0] + lx * cy - ly * sy, t->location[1] + lx * sy + ly * cy, 118.0f, red, 0, 0.4f);
            CTransform* tt = ecs_get(&ed->world, tg, COMP_TRANSFORM);
            tt->scale[0] = tt->scale[1] = tt->scale[2] = 0.06f, tt->flags = MOBILITY_MOVABLE;
            snprintf(((CName*)ecs_get(&ed->world, tg, COMP_NAME))->text, RE_NAME_MAX, "IKTarget%d", i);
        }
        g_rag[g_nrag++] = e;
    }
    Entity cam = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CAMERA)); // (the view while playing)
    snprintf(((CName*)ecs_get(&ed->world, cam, COMP_NAME))->text, RE_NAME_MAX, "CameraActor");
    CTransform* ct = ecs_get(&ed->world, cam, COMP_TRANSFORM);
    ct->location[0] = 40.0f, ct->location[1] = -560.0f, ct->location[2] = 300.0f;
    ct->rotation[0] = -28.0f, ct->rotation[1] = 98.0f;
    ct->scale[0] = ct->scale[1] = ct->scale[2] = 1.0f;
    CCamera* cc = ecs_get(&ed->world, cam, COMP_CAMERA);
    cc->fov_deg = 75.0f, cc->auto_activate = 1;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -45.0f, lt->rotation[1] = 40.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = -40.0f; ed->cam.pos[1] = -720.0f; ed->cam.pos[2] = 330.0f;
    ed->cam.yaw = 88.0f; ed->cam.pitch = -24.0f;
    ed->dirty = true;
}

bool editor_ragdoll_check(Editor* ed) {
    bool ok = g_nrag == 12;
    for (int i = 0; i < g_nrag; i++) {
        IkPhysInfo info;
        Entity sk = anim_mesh_entity(&ed->world, g_rag[i]);
        if (!ikphys_info(sk, &info)) {
            printf("[RealEngine] FAIL: ragdoll %d: no state\n", i);
            ok = false;
            continue;
        }
        if (i < 6) {
            printf("[RealEngine] Ragdoll %d: %u bodies between %.1f and %.1f cm, fastest %.1f cm/s (body %d, turning %.1f rad/s at most: body %d), asleep %d\n", i, info.bodies, info.zmin, info.zmax, info.vmax, info.fastest, info.wmax, info.spinning, info.asleep);
            if (info.bodies < 10 || info.zmin < -2.0f || info.zmax > 75.0f || (info.vmax > 60.0f && !info.asleep)) {
                printf("[RealEngine] FAIL: ragdoll %d: it should lie on the floor or the step, at rest\n", i);
                ok = false;
            }
        } else if (i < 9) {
            printf("[RealEngine] Below spine_03 %d: %u bodies between %.1f and %.1f cm, fastest %.1f cm/s\n", i, info.bodies, info.zmin, info.zmax, info.vmax);
            if (!info.bodies || info.zmin < 60.0f) printf("[RealEngine] FAIL: below spine_03 %d: the arms should hang from the shoulders\n", i), ok = false;
        } else {
            printf("[RealEngine] IK reach %d: the hand %.2f cm from its goal\n", i, info.ik_error);
            if (info.ik_error < 0.0f || info.ik_error > 1.5f) printf("[RealEngine] FAIL: IK reach %d: the hand should touch its sphere\n", i), ok = false;
        }
    }
    const IkPhysStats* s = ikphys_stats();
    printf("[RealEngine] IK/Physics: %u rigs, %u simulated meshes, %u bodies, %u contacts, %.3f ms\n", s->rigs, s->ragdolls, s->bodies, s->contacts, s->ms);
    return ok;
}

static Entity g_snow_walker; // --scene snow's Walker (editor_snow_tick)
static double g_snow_t;
static float g_walk_speed = 150.0f; // cm/s

// --scene vehicles (notas-dev/veiculos.md): every kind at once, driven by a script (editor_vehicles_tick): a car and a
// sports car accelerate and turn, a truck tips its bed, a motorcycle leans through a curve, a tank turns on the spot then
// drives, a boat crosses a lake (a Water Body with waves), a locomotive pulls two wagons around a rail loop, an airplane
// takes off from a runway and climbs, a helicopter lifts off and hovers. editor_vehicles_check: each did its part
enum { VS_CAR, VS_SPORTS, VS_TRUCK, VS_MOTO, VS_TANK, VS_BOAT, VS_LOCO, VS_WAGON1, VS_WAGON2, VS_PLANE, VS_HELI, VS_COUNT };
static Entity g_veh[VS_COUNT];
static double g_veh_t;
static float g_veh_start[VS_COUNT][3];
// The cloths' scene (cloth.h; --scene cloth): a flag in the wind, a curtain over a ball and a box, silk on a sphere, a
// banner that tears, 8 more curtains (12 cloths, ~30 thousand vertices), the sun
static void cloth_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    float g[3] = { 0.35f, 0.35f, 0.36f }, grey[3] = { 0.6f, 0.6f, 0.6f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, g, 0, 0.8f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 80.0f, ft->flags = MOBILITY_STATIC;
    Entity ball = mesh_actor(ed, SHAPE_SPHERE, 0, 0, 90, grey, 0, 0.5f);
    ((CTransform*)ecs_get(w, ball, COMP_TRANSFORM))->scale[0] = ((CTransform*)ecs_get(w, ball, COMP_TRANSFORM))->scale[1] = ((CTransform*)ecs_get(w, ball, COMP_TRANSFORM))->scale[2] = 1.6f;
    Entity box = mesh_actor(ed, SHAPE_CUBE, 450, 0, 60, grey, 0, 0.6f);
    CTransform* bt = ecs_get(w, box, COMP_TRANSFORM);
    bt->scale[0] = 1.5f, bt->scale[1] = 1.5f, bt->scale[2] = 1.2f;
    static const struct { int preset; float x, y, z, wdt, hgt; int pin; float tear; } k[] = {
        { CLOTH_FLAG, -500, 0, 420, 240, 150, CLOTH_PIN_LEFT, 0.0f },
        { CLOTH_COTTON, 0, 150, 260, 260, 300, CLOTH_PIN_NONE, 0.0f }, // laid flat over the ball: it falls and drapes
        { CLOTH_SILK, 450, 0, 320, 240, 220, CLOTH_PIN_TOP_CORNERS, 0.0f }, // over the box
        { CLOTH_CANVAS, 900, 0, 380, 200, 260, CLOTH_PIN_TOP, 1.25f },  // a banner that tears in the wind
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CLOTH));
        snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", g_cloth_preset_names[k[i].preset]);
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->location[0] = k[i].x, t->location[1] = k[i].y, t->location[2] = k[i].z, t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE;
        CCloth* c = ecs_get(w, e, COMP_CLOTH);
        cloth_apply_preset(c, k[i].preset);
        c->width = k[i].wdt, c->height = k[i].hgt, c->pin = (uint32_t)k[i].pin, c->tear = k[i].tear;
        if (i == 3) c->wind[1] = 18.0f, c->gust = 1.0f; // (a gale on the banner)
        if (i == 1) t->rotation[2] = 90.0f; // (its sheet turned flat: the actor's XZ plane onto XY)
    }
    for (int i = 0; i < 8; i++) { // more curtains: the count heavy scenes need
        Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CLOTH));
        snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "Curtain%d", i + 1);
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->location[0] = -1200.0f + 300.0f * (float)i, t->location[1] = 900.0f, t->location[2] = 300.0f;
        t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE;
        CCloth* c = ecs_get(w, e, COMP_CLOTH);
        cloth_apply_preset(c, i & 1 ? CLOTH_SILK : CLOTH_COTTON);
        c->wind[1] = -3.0f, c->gust = 0.8f;
    }
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(w, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -45.0f, lt->rotation[1] = 40.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = 100.0f; ed->cam.pos[1] = -1500.0f; ed->cam.pos[2] = 300.0f;
    ed->cam.yaw = 90.0f; ed->cam.pitch = -6.0f;
    ed->dirty = true;
}
// The water's scenes (water.h; --scene rain, --scene ocean): a yard with a roof, steps and a slope under a storm (puddles,
// a stream down the slope, dry under the roof); or an ocean under a fresh wind with a boat
// --scene lake: a shallow lake (the bottom seen through it, white water at its shore and around the rocks and posts in
// it): a sandy floor 70 cm under the surface, a beach rising out of it, rocks and posts standing in the water
static void lake_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    float sand[3] = { 0.62f, 0.55f, 0.42f }, rock[3] = { 0.4f, 0.38f, 0.36f }, wood[3] = { 0.35f, 0.22f, 0.12f }, tile[3] = { 0.2f, 0.45f, 0.5f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, sand, 0, 0.9f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 80.0f, ft->flags = MOBILITY_STATIC;
    static const float parts[][10] = { // shape, x y z, scale x y z, pitch, color (0 sand 1 rock 2 wood 3 tile)
        { SHAPE_CUBE, 1700, 0, 20, 14, 30, 1.0f, -5, 0 },     // the beach: a slope out of the water
        { SHAPE_SPHERE, -300, 400, 30, 1.6f, 1.6f, 1.2f, 0, 1 }, { SHAPE_SPHERE, 200, -500, 40, 2.2f, 1.8f, 1.4f, 0, 1 },
        { SHAPE_SPHERE, 700, 300, 20, 1.0f, 1.0f, 0.9f, 0, 1 },
        { SHAPE_CUBE, -800, -300, 100, 0.25f, 0.25f, 2.0f, 0, 2 }, { SHAPE_CUBE, -800, 100, 100, 0.25f, 0.25f, 2.0f, 0, 2 },
        { SHAPE_CUBE, -800, 500, 100, 0.25f, 0.25f, 2.0f, 0, 2 }, // a pier's posts
        { SHAPE_CUBE, -400, -1200, 2, 6, 3, 0.04f, 0, 3 },    // a tiled patch on the bottom (to see through the water)
    };
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        const float* q = parts[i];
        const float* colr = q[9] == 1 ? rock : q[9] == 2 ? wood : q[9] == 3 ? tile : sand;
        Entity b = mesh_actor(ed, (int)q[0], q[1], q[2], q[3], colr, 0, 0.8f);
        CTransform* t = ecs_get(w, b, COMP_TRANSFORM);
        t->scale[0] = q[4], t->scale[1] = q[5], t->scale[2] = q[6], t->rotation[0] = q[7], t->flags = MOBILITY_STATIC;
        if (q[9] == 3) ((CStaticMesh*)ecs_get(w, b, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat"); // (a texture seen through the water)
    }
    Entity lk = water_spawn(w, (float[3]){ 0, 0, 70 }, 6000, 6000);
    CWater* cw = ecs_get(w, lk, COMP_WATER);
    cw->wave_height = 1.5f, cw->wave_length = 300.0f, cw->wave_speed = 90.0f, cw->clarity = 600.0f;
    cw->color[0] = 0.01f, cw->color[1] = 0.06f, cw->color[2] = 0.07f;
    snprintf(((CName*)ecs_get(w, lk, COMP_NAME))->text, RE_NAME_MAX, "Lake");
    ed->cam.pos[0] = -1400.0f; ed->cam.pos[1] = -1800.0f; ed->cam.pos[2] = 450.0f;
    ed->cam.yaw = 40.0f; ed->cam.pitch = -18.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(w, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -45.0f, lt->rotation[1] = 60.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->dirty = true;
}

static void water_scene(Editor* ed, bool ocean) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    float g[3] = { 0.35f, 0.33f, 0.3f }, wall[3] = { 0.6f, 0.58f, 0.55f }, roof[3] = { 0.25f, 0.12f, 0.1f };
    if (!ocean) {
        Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, g, 0, 0.8f);
        CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
        ft->scale[0] = ft->scale[1] = 80.0f, ft->flags = MOBILITY_STATIC;
        static const float boxes[][9] = { // x y z, scale, pitch yaw roll
            { 600, 0, 150, 0.3f, 8, 3, 0, 0, 0 },  { -600, 0, 150, 0.3f, 8, 3, 0, 0, 0 }, // two walls
            { 0, 0, 320, 13, 9, 0.2f, 0, 0, 0 },                                         // the roof between them
            { 0, 1400, 60, 10, 6, 1.2f, -8, 0, 0 },                                      // a slope (a stream runs down it)
            { -1600, -900, 15, 3, 3, 0.3f, 0, 0, 0 }, { -1600, -900, 45, 2, 2, 0.3f, 0, 0, 0 }, // steps
        };
        for (size_t i = 0; i < sizeof boxes / sizeof boxes[0]; i++) {
            Entity b = mesh_actor(ed, SHAPE_CUBE, boxes[i][0], boxes[i][1], boxes[i][2], i == 2 ? roof : wall, 0, 0.7f);
            CTransform* t = ecs_get(w, b, COMP_TRANSFORM);
            t->scale[0] = boxes[i][3], t->scale[1] = boxes[i][4], t->scale[2] = boxes[i][5];
            t->rotation[0] = boxes[i][6], t->rotation[1] = boxes[i][7], t->rotation[2] = boxes[i][8], t->flags = MOBILITY_STATIC;
        }
        Entity r = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_RAIN));
        snprintf(((CName*)ecs_get(w, r, COMP_NAME))->text, RE_NAME_MAX, "Rain");
        CTransform* rt = ecs_get(w, r, COMP_TRANSFORM);
        rt->scale[0] = rt->scale[1] = rt->scale[2] = 1.0f, rt->location[2] = 500.0f;
        CRain* rn = ecs_get(w, r, COMP_RAIN);
        rn->intensity = 60.0f, rn->start_water = 4.0f, rn->water_gain = 120.0f;
        if (getenv("REALENGINE_RAIN_LENS")) rn->lens_water = (float)atof(getenv("REALENGINE_RAIN_LENS")); // (2026-11-15: the water on the lens, measured apart)
        if (getenv("REALENGINE_RAIN_WETNESS")) rn->wetness = (float)atof(getenv("REALENGINE_RAIN_WETNESS")); // (a dry comparison)
        if (getenv("REALENGINE_WALKER_SHELTER")) rn->actor_wet_time = 3.0f; // (the Walker soaked at once: its prints and drips under the roof)
        ed->cam.pos[0] = -2200.0f; ed->cam.pos[1] = -2600.0f; ed->cam.pos[2] = 420.0f;
        ed->cam.yaw = 45.0f; ed->cam.pitch = -14.0f;
    } else {
        Entity o = water_spawn(w, (float[3]){ 0, 0, 0 }, 200000, 200000);
        CWater* cw = ecs_get(w, o, COMP_WATER);
        cw->ocean = 1, cw->wind_speed = 11.0f;
        snprintf(((CName*)ecs_get(w, o, COMP_NAME))->text, RE_NAME_MAX, "Ocean");
        VehicleAsset* a = veh_temp("OceanBoat", VT_BOAT);
        if (a) vehicle_spawn(w, a->hash, (float[3]){ 2500, 0, 40 }, 30.0f, "Boat");
        { // (2026-11-10) a dry pit below the sea: a Water Body Exclusion over a floor 2 m down (no ocean drawn there)
            Entity ex = water_spawn(w, (float[3]){ 1200, 1600, 0 }, 2000, 2000);
            if (ex) ((CWater*)ecs_get(w, ex, COMP_WATER))->exclusion = 1;
            float pc[3] = { 0.45f, 0.42f, 0.36f };
            Entity pf = mesh_actor(ed, SHAPE_CUBE, 1200, 1600, -250, pc, 0, 0.8f);
            CTransform* pt = ecs_get(w, pf, COMP_TRANSFORM);
            pt->scale[0] = pt->scale[1] = 20.0f, pt->scale[2] = 1.0f, pt->flags = MOBILITY_STATIC;
        }
        ed->cam.pos[0] = 0.0f; ed->cam.pos[1] = -1500.0f; ed->cam.pos[2] = 600.0f;
        ed->cam.yaw = 70.0f; ed->cam.pitch = -8.0f;
    }
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(w, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = ocean ? -25.0f : -50.0f, lt->rotation[1] = ocean ? 80.0f : 30.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->dirty = true;
}
static void vehicles_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    float g[3] = { 0.5f, 0.5f, 0.5f }, ramp_c[3] = { 0.35f, 0.42f, 0.55f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, -5000, 0, 0, g, 0, 0.7f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = 600.0f, ft->scale[1] = 600.0f, ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(w, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    ((CStaticMesh*)ecs_get(w, fl, COMP_STATIC_MESH))->uv_scale = 60.0f;
    Entity rp = mesh_actor(ed, SHAPE_CUBE, 3500, 2400, 20, ramp_c, 0, 0.6f); // (a low ramp the motorcycle's path crosses)
    CTransform* rt = ecs_get(w, rp, COMP_TRANSFORM);
    rt->scale[0] = 6.0f, rt->scale[1] = 4.0f, rt->scale[2] = 0.3f, rt->rotation[0] = 4.0f, rt->flags = MOBILITY_STATIC;
    water_spawn(w, (float[3]){ 55000, 0, -20 }, 50000, 50000);
    const float loop[8][3] = { { -6000, 5000, 0 }, { 0, 4000, 0 }, { 6000, 5000, 0 }, { 9000, 8000, 0 }, { 6000, 11000, 0 }, { 0, 12000, 0 }, { -6000, 11000, 0 }, { -9000, 8000, 0 } };
    rail_spawn(w, loop, 8, true, "RailLoop");
    static const int tm[VS_COUNT] = { VT_CAR, VT_SPORTS, VT_TRUCK, VT_MOTORCYCLE, VT_TANK, VT_BOAT, VT_LOCOMOTIVE, VT_WAGON, VT_WAGON, VT_AIRPLANE, VT_HELICOPTER };
    static const char* const nm[VS_COUNT] = { "Car", "SportsCar", "Truck", "Motorcycle", "Tank", "Boat", "Locomotive", "Wagon1", "Wagon2", "Airplane", "Helicopter" };
    static const float pos[VS_COUNT][3] = { { 0, -800, 0 }, { 0, -1600, 0 }, { 0, 800, 0 }, { 0, 1600, 0 }, { -2500, 0, 0 }, { 33000, 0, 0 },
                                            { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { -25000, -4000, 0 }, { -4000, 3200, 0 } };
    for (int i = 0; i < VS_COUNT; i++) {
        VehicleAsset* a = veh_temp(nm[i], tm[i]);
        if (!a) continue;
        float at[3] = { pos[i][0], pos[i][1], pos[i][2] + veh_rest_height(a) };
        if (i == VS_BOAT) at[2] = 40.0f;
        g_veh[i] = vehicle_spawn(w, a->hash, at, 0.0f, nm[i]);
        memcpy(g_veh_start[i], at, 12);
        CVehicle* c = g_veh[i] ? ecs_get(w, g_veh[i], COMP_VEHICLE) : NULL;
        if (c && tm[i] == VT_LOCOMOTIVE) snprintf(c->rail, sizeof c->rail, "RailLoop"), c->rail_start = 60.0f;
        if (c && tm[i] == VT_WAGON) {
            snprintf(c->rail, sizeof c->rail, "RailLoop");
            snprintf(c->couple_to, sizeof c->couple_to, "%s", i == VS_WAGON1 ? "Locomotive" : "Wagon1");
        }
    }
    Entity cam = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CAMERA));
    snprintf(((CName*)ecs_get(w, cam, COMP_NAME))->text, RE_NAME_MAX, "CameraActor");
    CTransform* ct = ecs_get(w, cam, COMP_TRANSFORM);
    ct->location[0] = -2600.0f, ct->location[1] = -3600.0f, ct->location[2] = 1500.0f;
    ct->rotation[0] = -22.0f, ct->rotation[1] = 50.0f;
    ct->scale[0] = ct->scale[1] = ct->scale[2] = 1.0f;
    CCamera* cc = ecs_get(w, cam, COMP_CAMERA);
    cc->fov_deg = 75.0f, cc->auto_activate = 1;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(w, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -40.0f, lt->rotation[1] = 30.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = -2600.0f; ed->cam.pos[1] = -3600.0f; ed->cam.pos[2] = 1500.0f;
    ed->cam.yaw = 50.0f; ed->cam.pitch = -22.0f;
    g_veh_t = 0.0;
    ed->dirty = true;
}

#include "editor_softbody.inc" // (Q9: the soft bodies' scenes, menus and Details)
#include "editor_spline.inc" // (Q14: the splines' and cables' menus, Details and scene)
#include "metro_scene.inc" // (--scene metro: the abandoned metro hall of the reference picture, CC0, notas-dev/demos.md)

// --scene cloth_mesh (notas-dev/tecido.md): a skirt written as an imported mesh (.rmesh in the project, its UV seam
// doubled as an importer leaves it), made a Cloth on the Walker's pelvis, colliding with its legs; the Walker goes round
// (editor_snow_tick) so the skirt swings
static void cloth_mesh_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    enum { SEG = 48, RING = 16 };
    GpuVertex* v = calloc((SEG + 1) * (RING + 1), sizeof(GpuVertex));
    uint32_t* idx = malloc(sizeof(uint32_t) * SEG * RING * 6);
    char path[1100];
    snprintf(path, sizeof path, "%s/Meshes", content_root());
    re_make_dirs(path);
    snprintf(path, sizeof path, "%s/Meshes/SM_TestSkirt.rmesh", content_root());
    if (v && idx) {
        uint32_t ni = 0;
        for (int r = 0; r <= RING; r++)
            for (int s = 0; s <= SEG; s++) { // (a flared tube: 17 cm around the waist, 36 at the hem, 58 long)
                GpuVertex* g = &v[r * (SEG + 1) + s];
                float a = 6.2831853f * (float)s / SEG, f = (float)r / RING, rad = 17.0f + 19.0f * f * f * 0.6f + 19.0f * f * 0.4f;
                g->pos[0] = rad * cosf(a), g->pos[1] = rad * sinf(a), g->pos[2] = -58.0f * f;
                g->uv[0] = (float)s / SEG, g->uv[1] = f, g->uv2[0] = g->uv2[1] = -1.0f;
            }
        for (int r = 0; r < RING; r++)
            for (int s = 0; s < SEG; s++) {
                uint32_t a = (uint32_t)(r * (SEG + 1) + s), b = a + 1, c = a + SEG + 1, d = c + 1;
                idx[ni++] = a, idx[ni++] = c, idx[ni++] = b, idx[ni++] = b, idx[ni++] = c, idx[ni++] = d; // (wound with its normals out, as an importer's)
            }
        MeshBuilt mb;
        if (mesh_build_indexed(v, (SEG + 1) * (RING + 1), idx, ni, false, false, &mb)) {
            if (!rmesh_write(path, &mb)) printf("[RealEngine] FAIL: cloth mesh: the skirt not written\n");
            mesh_built_free(&mb);
        }
    }
    free(v), free(idx);
    render_meshes_reload(ed->vk);
    float g[3] = { 0.5f, 0.5f, 0.52f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, -1100, -1500, 0, g, 0, 0.8f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 30.0f, ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(w, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_SKELETAL_MESH));
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "Walker");
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE, t->location[0] = -800.0f, t->location[1] = -1500.0f, t->location[2] = 90.0f;
    CCharacter* ch = ecs_get(w, e, COMP_CHARACTER);
    ch->max_walk_speed = 150.0f, ch->radius = 34.0f, ch->half_height = 88.0f, ch->on_ground = 1;
    CSkeletalMesh* sk = ecs_get(w, e, COMP_SKELETAL_MESH);
    const SkelMesh* m = anim_mesh_name("SK_Mannequin");
    sk->mesh = m ? m->hash : 0;
    g_snow_walker = e, g_snow_t = 0.0, g_walk_speed = 90.0f;
    Entity ce = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CLOTH));
    snprintf(((CName*)ecs_get(w, ce, COMP_NAME))->text, RE_NAME_MAX, "Skirt");
    CTransform* ct = ecs_get(w, ce, COMP_TRANSFORM);
    ct->scale[0] = ct->scale[1] = ct->scale[2] = 1.0f, ct->flags = MOBILITY_MOVABLE;
    CCloth* c = ecs_get(w, ce, COMP_CLOTH);
    ecs_component_defaults(COMP_CLOTH, c);
    cloth_apply_preset(c, CLOTH_COTTON);
    c->mesh = render_mesh_id_of("/Content/Meshes/SM_TestSkirt.rmesh"), c->pin_band = 2.0f, c->thickness = 1.5f;
    c->color[0] = 0.12f, c->color[1] = 0.2f, c->color[2] = 0.55f;
    snprintf(c->follow_actor, sizeof c->follow_actor, "Walker"), snprintf(c->follow_bone, sizeof c->follow_bone, "pelvis");
    c->follow_offset[2] = 8.0f;
    if (render_mesh_slot(c->mesh) < SHAPE_COUNT) printf("[RealEngine] FAIL: cloth mesh: the skirt mesh not loaded\n");
    { // the same mesh as a plain Static Mesh actor on a stand, made a cloth by Actor > Make Cloth from Selected
        Entity sk2 = mesh_actor(ed, SHAPE_CUBE, -1500, -1300, 112, (float[3]){ 0.6f, 0.15f, 0.12f }, 0, 0.8f);
        ((CStaticMesh*)ecs_get(w, sk2, COMP_STATIC_MESH))->mesh = c->mesh;
        ((CTransform*)ecs_get(w, sk2, COMP_TRANSFORM))->flags = MOBILITY_MOVABLE;
        Entity stand = mesh_actor(ed, SHAPE_CYLINDER, -1500, -1300, 55, g, 0, 0.6f);
        CTransform* stt = ecs_get(w, stand, COMP_TRANSFORM);
        stt->scale[0] = stt->scale[1] = 0.3f, stt->scale[2] = 1.1f, stt->flags = MOBILITY_STATIC;
        editor_select(ed, sk2, SEL_REPLACE);
        make_cloth_from_selected(ed);
    }
    ed->cam.pos[0] = -1100.0f, ed->cam.pos[1] = -2100.0f, ed->cam.pos[2] = 170.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -12.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(w, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -50.0f, lt->rotation[1] = 30.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->dirty = true;
}

// --scene snow: the Walker around a circle at a walk (its prints: water.c's trail)
void editor_snow_tick(Editor* ed, double dt) {
    CTransform* t = g_snow_walker ? ecs_get(&ed->world, g_snow_walker, COMP_TRANSFORM) : NULL;
    CCharacter* ch = g_snow_walker ? ecs_get(&ed->world, g_snow_walker, COMP_CHARACTER) : NULL;
    if (!t || !ch) return;
    if (getenv("REALENGINE_WALKER_STILL")) dt = 0.0; // (the shots of a cloth at rest)
    g_snow_t += dt;
    float a = (float)g_snow_t * g_walk_speed / 300.0f; // (1.5 m/s on a 3 m circle; the skirt's: a stroll)
    t->location[0] = -1100.0f + 300.0f * cosf(a), t->location[1] = -1500.0f + 300.0f * sinf(a), t->location[2] = 90.0f;
    t->rotation[1] = a * 57.29578f + 90.0f;
    if (getenv("REALENGINE_WALKER_SHELTER")) { // (2026-11-15, the wet marks) from the rain in under the roof and back, every 16 s
        float ph = fmodf((float)g_snow_t / 16.0f, 1.0f), in = ph < 0.5f;
        t->location[0] = -150.0f + 40.0f * sinf((float)g_snow_t * 1.7f);
        t->location[1] = in ? -1500.0f + ph * 2.0f * 1400.0f : -100.0f - (ph - 0.5f) * 2.0f * 1400.0f;
        t->rotation[1] = in ? 90.0f : 270.0f;
    }
    ch->on_ground = 1;
}

void editor_vehicles_tick(Editor* ed, double dt) {
    if (!ed->playing) return;
    World* w = &ed->world;
    g_veh_t += dt;
    float t = (float)g_veh_t;
    vehicle_set_control(w, g_veh[VS_CAR], VI_THROTTLE, 1.0f), vehicle_set_control(w, g_veh[VS_CAR], VI_STEER, t > 3.0f ? 0.25f : 0.0f);
    vehicle_set_control(w, g_veh[VS_SPORTS], VI_THROTTLE, 1.0f);
    vehicle_set_control(w, g_veh[VS_TRUCK], VI_THROTTLE, 0.7f);
    if (t > 1.0f) vehicle_part_target(w, g_veh[VS_TRUCK], "Bed", 1.0f);
    vehicle_set_control(w, g_veh[VS_MOTO], VI_THROTTLE, 0.8f), vehicle_set_control(w, g_veh[VS_MOTO], VI_STEER, t > 2.0f ? -0.15f : 0.0f);
    vehicle_set_control(w, g_veh[VS_TANK], VI_STEER, t < 3.0f ? 1.0f : 0.0f), vehicle_set_control(w, g_veh[VS_TANK], VI_THROTTLE, t < 3.0f ? 0.0f : 1.0f);
    vehicle_set_control(w, g_veh[VS_BOAT], VI_THROTTLE, 1.0f);
    vehicle_set_control(w, g_veh[VS_LOCO], VI_THROTTLE, 1.0f);
    VehInfo pi;
    if (vehicle_info(w, g_veh[VS_PLANE], &pi)) { // (rotate past 110 km/h, hold 10 degrees nose up, wings level)
        VehControls c = { 0 };
        c.throttle = 1.0f;
        if (pi.speed_kmh > 110.0f) c.pitch = fminf(fmaxf((10.0f - pi.pitch_deg) * 0.08f, -1.0f), 1.0f);
        c.roll = fminf(fmaxf(-pi.roll_deg * 0.05f, -1.0f), 1.0f);
        vehicle_set_controls(w, g_veh[VS_PLANE], &c);
    }
    VehControls hc = { 0 };
    hc.collective = t < 3.0f ? 0.6f : 0.0f;
    vehicle_set_controls(w, g_veh[VS_HELI], &hc);
}

bool editor_vehicles_check(Editor* ed) {
    World* w = &ed->world;
    bool ok = true;
    VehInfo in[VS_COUNT];
    for (int i = 0; i < VS_COUNT; i++)
        if (!vehicle_info(w, g_veh[i], &in[i])) {
            printf("[RealEngine] FAIL: vehicles: %d has no state\n", i);
            return false;
        }
    float moved[VS_COUNT];
    for (int i = 0; i < VS_COUNT; i++) {
        float dx = in[i].pos[0] - g_veh_start[i][0], dy = in[i].pos[1] - g_veh_start[i][1];
        moved[i] = sqrtf(dx * dx + dy * dy) / 100.0f;
    }
    printf("[RealEngine] Vehicles: car %.0f km/h gear %d, sports car %.0f km/h, truck %.0f km/h (bed %.2f), motorcycle %.0f km/h upright %d\n", in[VS_CAR].speed_kmh,
           in[VS_CAR].gear, in[VS_SPORTS].speed_kmh, in[VS_TRUCK].speed_kmh, vehicle_part_value(w, g_veh[VS_TRUCK], "Bed"), in[VS_MOTO].speed_kmh, in[VS_MOTO].upright);
    printf("[RealEngine] Vehicles: tank %.0f m, heading %.0f; boat %.0f km/h, %.0f%% of its floats in; train %.0f km/h, wagons %.1f / %.1f m behind\n", moved[VS_TANK],
           in[VS_TANK].yaw_deg, in[VS_BOAT].speed_kmh, in[VS_BOAT].submerged * 100.0f, in[VS_LOCO].speed_kmh, in[VS_LOCO].rail_s - in[VS_WAGON1].rail_s,
           in[VS_WAGON1].rail_s - in[VS_WAGON2].rail_s);
    printf("[RealEngine] Vehicles: airplane %.0f m up at %.0f km/h; helicopter %.0f m up, upright %d\n", in[VS_PLANE].pos[2] / 100.0f, in[VS_PLANE].speed_kmh,
           in[VS_HELI].pos[2] / 100.0f, in[VS_HELI].upright);
    const VehStats* vs = vehicle_stats();
    printf("[RealEngine] Vehicles: %u simulated, %u wheels, %u chassis contacts, %.3f ms\n", vs->vehicles, vs->wheels, vs->contacts, vs->ms);
    const AudioAutoStats* au = audio_auto_stats(); // (B8: each running engine heard through MS_Engine, its RPM and load)
    uint32_t running = 0;
    for (int i = 0; i < VS_COUNT; i++) running += in[i].engine_on;
    printf("[RealEngine] Vehicles: %u engines heard of %u running (Sound Graph MS_Engine; the last at %.0f RPM, load %.2f)\n", au->engines, running, au->rpm, au->load);
    if (running && au->engines != running) printf("[RealEngine] FAIL: vehicles: the engines' sound\n"), ok = false;
    for (int i = 0; i < VS_COUNT; i++)
        if (!in[i].upright && i != VS_PLANE) printf("[RealEngine] FAIL: vehicles: %d tipped over\n", i), ok = false;
    if (in[VS_CAR].speed_kmh < 60.0f || in[VS_SPORTS].speed_kmh < 80.0f || in[VS_TRUCK].speed_kmh < 20.0f || in[VS_MOTO].speed_kmh < 30.0f)
        printf("[RealEngine] FAIL: vehicles: the road vehicles' speeds\n"), ok = false;
    if (vehicle_part_value(w, g_veh[VS_TRUCK], "Bed") < 0.99f) printf("[RealEngine] FAIL: vehicles: the truck's bed\n"), ok = false;
    if (moved[VS_TANK] < 5.0f || fabsf(in[VS_TANK].yaw_deg) < 20.0f) printf("[RealEngine] FAIL: vehicles: the tank's turn\n"), ok = false;
    if (in[VS_BOAT].speed_kmh < 8.0f || in[VS_BOAT].submerged <= 0.0f) printf("[RealEngine] FAIL: vehicles: the boat\n"), ok = false;
    float gap1 = in[VS_LOCO].rail_s - in[VS_WAGON1].rail_s;
    if (in[VS_LOCO].speed_kmh < 15.0f || gap1 < 15.0f || gap1 > 25.0f) printf("[RealEngine] FAIL: vehicles: the train\n"), ok = false;
    if (in[VS_PLANE].pos[2] / 100.0f < 3.0f && in[VS_PLANE].speed_kmh < 100.0f) printf("[RealEngine] FAIL: vehicles: the airplane's run\n"), ok = false;
    if (in[VS_HELI].pos[2] / 100.0f < 5.0f) printf("[RealEngine] FAIL: vehicles: the helicopter\n"), ok = false;
    return ok;
}

// --scene procgen / --scene facility (notas-dev/procedural.md): a Procedural World and a player. procgen: the player
// rushes east at 650 m/s over the endless terrain (editor_procgen_tick): the chunks stream around it, the origin moves
// back every 2 km; facility: 30 rooms grown from the entrance. editor_procgen_check: what was made, where the player is
static Entity g_pg_player;
static double g_pg_t, g_pg_travel;
static void procgen_scene(Editor* ed, bool facility) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    Entity pw = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_PROC_WORLD));
    snprintf(((CName*)ecs_get(w, pw, COMP_NAME))->text, RE_NAME_MAX, "ProceduralWorld");
    CTransform* t = ecs_get(w, pw, COMP_TRANSFORM);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_STATIC;
    CProcWorld* c = ecs_get(w, pw, COMP_PROC_WORLD);
    c->mode = facility ? PROCGEN_FACILITY : PROCGEN_TERRAIN, c->rooms = 30, c->seed = 20261107u;
    c->view_distance = 80000.0f, c->creature_density = facility ? 2.0f : 0.5f, c->rebase_distance = 50000.0f; // (500 m: rebased often)
    // the player: a Character the script carries (its feet on the ground under it)
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_SKELETAL_MESH));
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "Player");
    CTransform* pt = ecs_get(w, e, COMP_TRANSFORM);
    pt->location[2] = 400.0f, pt->flags = MOBILITY_MOVABLE, pt->scale[0] = pt->scale[1] = pt->scale[2] = 1.0f;
    if (facility) pt->location[2] = 92.0f;
    CCharacter* ch = ecs_get(w, e, COMP_CHARACTER);
    ch->max_walk_speed = 600.0f, ch->jump_z_velocity = 520.0f, ch->gravity_scale = 1.0f, ch->radius = 34.0f, ch->half_height = 88.0f;
    ch->auto_possess = 1, ch->camera_distance = 450.0f;
    CSkeletalMesh* sk = ecs_get(w, e, COMP_SKELETAL_MESH);
    const SkelMesh* m = anim_mesh_name("SK_Mannequin");
    const BpAsset* abp = bp_find_name(BPA_CLASS, "ABP_Mannequin");
    sk->mesh = m ? m->hash : 0, sk->anim_class = abp ? abp->hash : 0;
    g_pg_player = e, g_pg_t = 0.0, g_pg_travel = 0.0;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(w, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -35.0f, lt->rotation[1] = 25.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = -3000.0f; ed->cam.pos[1] = -3000.0f; ed->cam.pos[2] = 4000.0f;
    ed->cam.yaw = 45.0f; ed->cam.pitch = -25.0f;
    ed->dirty = true;
}

void editor_procgen_tick(Editor* ed, double dt) {
    if (!ed->playing || !ecs_alive(&ed->world, g_pg_player)) return;
    g_pg_t += dt;
    CTransform* t = ecs_get(&ed->world, g_pg_player, COMP_TRANSFORM);
    const CProcWorld* c = NULL;
    for (uint32_t ai = 0; ai < ed->world.archetype_count && !c; ai++)
        if ((ed->world.archetypes[ai].mask & COMP_BIT(COMP_PROC_WORLD)) && ed->world.archetypes[ai].count) c = ecs_arch_column(&ed->world.archetypes[ai], COMP_PROC_WORLD);
    if (!t || !c || c->mode != PROCGEN_TERRAIN || g_pg_t < 1.0 || getenv("REALENGINE_PROCGEN_STILL")) return; // (STILL: the steady cost)
    float step = (float)(25000.0 * dt); // (250 m/s east - an airplane's speed -, its feet on the ground)
    t->location[0] += step, g_pg_travel += step;
    float z;
    if (procgen_height(t->location[0], t->location[1], &z)) t->location[2] = fmaxf(z, (float)c->water_level) + 92.0f;
    CCharacter* ch = ecs_get(&ed->world, g_pg_player, COMP_CHARACTER);
    if (ch) ch->velocity[0] = ch->velocity[1] = ch->velocity[2] = 0.0f;
    ecs_touch(&ed->world, g_pg_player);
}

bool editor_procgen_check(Editor* ed, bool facility) {
    const ProcStats* s = procgen_stats();
    bool ok = true;
    if (facility) {
        printf("[RealEngine] Procedural facility: %u rooms, %u creatures, %.3f ms to build\n", s->rooms, s->creatures, s->ms_peak > s->ms ? s->ms_peak : s->ms);
        if (s->rooms != 30) printf("[RealEngine] FAIL: procgen: the facility's rooms\n"), ok = false;
        return ok;
    }
    const CTransform* t = ecs_alive(&ed->world, g_pg_player) ? ecs_get(&ed->world, g_pg_player, COMP_TRANSFORM) : NULL;
    float z = 0.0f;
    bool ground = t && procgen_height(t->location[0], t->location[1], &z);
    printf("[RealEngine] Procedural world: traveled %.1f km, %u rebases (the world's 0 now %.1f km away), %u chunks, %u columns, %u trees, %u structures, %u creatures\n",
           g_pg_travel / 100000.0, s->rebases, s->origin[0] / 100000.0, s->chunks, s->columns, s->trees, s->structures, s->creatures);
    printf("[RealEngine] Procedural world: the player at %.0f, %.0f, %.0f cm (the ground %.0f), streaming %.3f ms (peak %.2f ms)\n", t ? t->location[0] : 0.0f,
           t ? t->location[1] : 0.0f, t ? t->location[2] : 0.0f, z, s->ms, s->ms_peak);
    bool still = getenv("REALENGINE_PROCGEN_STILL") != NULL; // (standing: no travel, no rebase)
    if ((!still && (s->rebases < 2 || fabsf(t ? t->location[0] : 0.0f) > 100000.0f)) || s->chunks < 200 || s->columns < 4000 || !ground || !t)
        printf("[RealEngine] FAIL: procgen: the streaming and the rebasing\n"), ok = false;
    return ok;
}

// --scene wp (B1, notas-dev/mundos-grandes.md): a 4 x 4 km level with a World Partition (128 m cells loaded within
// 256 m, HLOD regions of 4 x 4 cells) and 6000 props scattered over it (rocks, pillars, crates, towers: shapes of 1 to
// 12 m), a ground that never streams (Is Spatially Loaded: No), a sun and a sky. Data Layers: "Town" (24 houses near the
// far end, Activated) and "Event" (24 banners there, Unloaded). The HLODs are built with it. editor_wp_tick carries the
// player from the west end to the east one (400 m/s, over 9.5 s) and, at 3 s, unloads Town and activates Event (Set Data
// Layer Runtime State's call); editor_wp_check: the cells came and went, the HLOD showed, the layers did as told, no
// frame hitched (over twice the median: under 1 %) and the streaming kept within its budget
static Entity g_wp_player;
static double g_wp_t;
static float g_wp_speed = 40000.0f; // (Q27.7: cm/s; wp_fast: 250 km/h)
static float g_wp_dt[16384];
static uint32_t g_wp_ndt;
static void wp_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    spawn_actor(ed, SPAWN_WORLD_PARTITION, 0, (float[]){ 0, 0, 0 }, 0);
    CWorldPartition* c = ecs_get(w, ed->selected, COMP_WORLD_PARTITION);
    c->nlayers = 2;
    if (getenv("REALENGINE_WP_OFF")) c->enabled = 0; // (the measure without it: everything in the world)
    snprintf(c->layers[0].name, sizeof c->layers[0].name, "Town"), c->layers[0].initial = WP_LAYER_ACTIVATED;
    snprintf(c->layers[1].name, sizeof c->layers[1].name, "Event"), c->layers[1].initial = WP_LAYER_UNLOADED;
    float gc[3] = { 0.32f, 0.34f, 0.28f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, gc, 0, 0.95f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 4000.0f; // (4 km)
    ecs_add(w, fl, COMP_PARTITION);
    ((CPartition*)ecs_get(w, fl, COMP_PARTITION))->spatial = WP_SPATIAL_NO;
    uint32_t seed = 20261109u;
    for (int i = 0; i < 6000; i++) {
        float r[6];
        for (int k = 0; k < 6; k++) seed = seed * 1664525u + 1013904223u, r[k] = (float)(seed >> 8) / 16777216.0f;
        int shape = (int)(r[0] * 4.0f); // (cube, sphere, cylinder, cone)
        float x = -195000.0f + 390000.0f * r[1], y = -195000.0f + 390000.0f * r[2], s = 1.0f + 11.0f * r[3] * r[3];
        float col[3] = { 0.35f + 0.4f * r[4], 0.3f + 0.3f * r[5], 0.25f + 0.2f * r[3] };
        Entity e = mesh_actor(ed, shape, x, y, 50.0f * s, col, 0, 0.8f);
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->scale[0] = t->scale[1] = s, t->scale[2] = s * (shape == 2 ? 2.5f : 1.0f), t->location[2] = 50.0f * t->scale[2];
        t->rotation[2] = 360.0f * r[4], t->flags = MOBILITY_STATIC;
    }
    for (int i = 0; i < 48; i++) { // the Data Layers' actors, near the far end (the player passes them after the switch)
        float x = 150000.0f + 1500.0f * (float)(i % 24), y = 3000.0f + (i < 24 ? 0.0f : 3000.0f);
        float col[3] = { i < 24 ? 0.7f : 0.9f, i < 24 ? 0.6f : 0.2f, i < 24 ? 0.5f : 0.2f };
        Entity e = mesh_actor(ed, i < 24 ? SHAPE_CUBE : SHAPE_CYLINDER, x, y, 300.0f, col, 0, 0.7f);
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->scale[0] = t->scale[1] = i < 24 ? 6.0f : 0.5f, t->scale[2] = 6.0f, t->flags = MOBILITY_STATIC;
        ecs_add(w, e, COMP_PARTITION);
        ((CPartition*)ecs_get(w, e, COMP_PARTITION))->layers = i < 24 ? 1u : 2u;
    }
    // the player: a Character the script carries east (its feet on the ground)
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_SKELETAL_MESH));
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "Player");
    CTransform* pt = ecs_get(w, e, COMP_TRANSFORM);
    pt->location[0] = -190000.0f, pt->location[1] = 1000.0f, pt->location[2] = 92.0f;
    pt->flags = MOBILITY_MOVABLE, pt->scale[0] = pt->scale[1] = pt->scale[2] = 1.0f;
    CCharacter* ch = ecs_get(w, e, COMP_CHARACTER);
    ch->max_walk_speed = 600.0f, ch->jump_z_velocity = 520.0f, ch->gravity_scale = 1.0f, ch->radius = 34.0f, ch->half_height = 88.0f;
    ch->auto_possess = 1, ch->camera_distance = 450.0f;
    CSkeletalMesh* sk = ecs_get(w, e, COMP_SKELETAL_MESH);
    const SkelMesh* m = anim_mesh_name("SK_Mannequin");
    const BpAsset* abp = bp_find_name(BPA_CLASS, "ABP_Mannequin");
    sk->mesh = m ? m->hash : 0, sk->anim_class = abp ? abp->hash : 0;
    spawn_actor(ed, SPAWN_PLAYER_START, 0, (float[]){ -190000.0f, 1000.0f, 0.0f }, 0);
    g_wp_player = e, g_wp_t = 0.0, g_wp_ndt = 0;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(w, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -40.0f, lt->rotation[1] = 30.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    char msg[256];
    if (wp_build_hlods(w, content_root(), "Untitled", msg, sizeof msg)) render_meshes_reload(ed->vk); // (the regions' meshes for the play)
    else printf("[RealEngine] FAIL: World Partition: no HLOD built (%s)\n", msg);
    ed->cam.pos[0] = -192000.0f, ed->cam.pos[1] = -2000.0f, ed->cam.pos[2] = 3000.0f;
    ed->cam.yaw = 20.0f, ed->cam.pitch = -12.0f;
    ed->dirty = true;
}

void editor_wp_tick(Editor* ed, double dt) {
    if (!ed->playing || !ecs_alive(&ed->world, g_wp_player)) return;
    g_wp_t += dt;
    if (g_wp_t > 1.5 && !mat_compiling() && g_wp_ndt < sizeof g_wp_dt / sizeof g_wp_dt[0]) g_wp_dt[g_wp_ndt++] = (float)dt; // (after the start's caches and, Q22, the project's materials compiled)
    static bool switched;
    if (g_wp_t < 0.2) switched = false;
    if (g_wp_t >= 3.0 && !switched) { // (the Blueprint nodes' calls)
        switched = true;
        if (!wp_set_layer_state("Town", WP_LAYER_UNLOADED) || !wp_set_layer_state("Event", WP_LAYER_ACTIVATED))
            printf("[RealEngine] FAIL: World Partition: the Data Layers not found\n");
    }
    if (g_wp_t < 1.0) return;
    CTransform* t = ecs_get(&ed->world, g_wp_player, COMP_TRANSFORM);
    t->location[0] = fminf(t->location[0] + (float)(g_wp_speed * dt), 190000.0f); // (400 m/s east; wp_fast 250 km/h)
    t->location[2] = 92.0f;
    CCharacter* ch = ecs_get(&ed->world, g_wp_player, COMP_CHARACTER);
    if (ch) ch->velocity[0] = ch->velocity[1] = ch->velocity[2] = 0.0f;
    ecs_touch(&ed->world, g_wp_player);
}

static int float_cmp(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}

bool editor_wp_check(Editor* ed) {
    const WpStats* s = wp_stats();
    World* w = &ed->world;
    const CTransform* pt = ecs_alive(w, g_wp_player) ? ecs_get(w, g_wp_player, COMP_TRANSFORM) : NULL;
    uint32_t town = 0, event = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_PARTITION)) || !a->count) continue;
        const CPartition* p = ecs_arch_column(a, COMP_PARTITION);
        for (uint32_t i = 0; i < a->count; i++) town += (p[i].layers & 1u) != 0, event += (p[i].layers & 2u) != 0;
    }
    float med = 0.0f, worst = 0.0f;
    uint32_t over = 0;
    if (g_wp_ndt) {
        static float sorted[16384];
        memcpy(sorted, g_wp_dt, g_wp_ndt * sizeof sorted[0]);
        qsort(sorted, g_wp_ndt, sizeof sorted[0], float_cmp);
        med = sorted[g_wp_ndt / 2], worst = sorted[g_wp_ndt - 1];
        for (uint32_t i = 0; i < g_wp_ndt; i++) over += g_wp_dt[i] > 2.0f * med;
    }
    printf("[RealEngine] World Partition: %u actors kept in %u cells, %u in now (%u cells), %u loads and %u unloads, start %.2f ms; streaming %.3f ms (worst %.2f)\n",
           s->records, s->cells, s->records_in, s->cells_in, s->loads, s->unloads, s->begin_ms, s->ms, s->ms_peak);
    printf("[RealEngine] World Partition: HLOD %u of %u proxies and %u of %u regions shown; Data Layers: Town %u in, Event %u in; the player at %.1f km\n",
           s->proxies_shown, s->proxies, s->regions_shown, s->regions, town, event, pt ? pt->location[0] / 100000.0f : 0.0f);
    printf("[RealEngine] World Partition: frames %u, median %.3f ms, worst %.3f ms, %u over twice the median (%.2f %%)\n", g_wp_ndt, med * 1000.0f,
           worst * 1000.0f, over, g_wp_ndt ? 100.0f * (float)over / (float)g_wp_ndt : 0.0f);
    bool fast = g_wp_speed < 30000.0f; // (Q27.7: wp_fast)
    printf("[RealEngine] World Partition: at %.0f km/h, %u cells ahead; records missing in range in %u of %u frames (at most %u)\n", g_wp_speed * 0.036f,
           s->ahead, s->missing_frames, s->frames, s->missing_peak);
    bool ok = true;
    if (s->records < 6000 || s->cells < 800 || s->records_in > s->records / 10 || s->loads < (fast ? 30u : 200u) || s->unloads < (fast ? 30u : 200u))
        printf("[RealEngine] FAIL: World Partition: the grid didn't stream\n"), ok = false;
    if (s->missing_frames) printf("[RealEngine] FAIL: World Partition: cells in sight still loading in %u frames\n", s->missing_frames), ok = false;
    if (fast && over * 500 > g_wp_ndt) printf("[RealEngine] FAIL: World Partition: at 250 km/h over 0.2 %% of the frames hitched\n"), ok = false;
    if (!s->proxies_shown || !s->regions_shown || s->regions < 16) printf("[RealEngine] FAIL: World Partition: no HLOD shown\n"), ok = false;
    if (town || (!event && !fast)) printf("[RealEngine] FAIL: World Partition: the Data Layers\n"), ok = false;
    if (!pt || pt->location[0] < (fast ? -140000.0f : 180000.0f)) printf("[RealEngine] FAIL: World Partition: the player didn't cross\n"), ok = false;
    if (g_wp_ndt < 100 || over * 100 > g_wp_ndt || s->ms_peak > 6.0) printf("[RealEngine] FAIL: World Partition: a hitch\n"), ok = false;
    char dir[900]; // (the test's HLOD meshes gone: the other tests count the project's meshes)
    wp_hlod_dir(content_root(), "Untitled", dir, sizeof dir);
    re_remove_tree(dir);
    return ok;
}

bool editor_footik_check(Editor* ed) {
    float feet[2] = { 0, 0 }, pelvis = 0.0f;
    bool got = anim_foot_offsets(&ed->world, g_footik_actor, feet, &pelvis);
    printf("[RealEngine] Foot IK: left %.1f cm, right %.1f cm, pelvis %.1f cm\n", feet[0], feet[1], pelvis);
    bool ok = got && fabsf(feet[0] - 25.0f) < 2.0f && fabsf(feet[1]) < 1.5f && fabsf(pelvis) < 1.0f;
    if (!ok) printf("[RealEngine] FAIL: Foot IK: the left foot should rise 25 cm onto the step, the right stay, the pelvis stay\n");
    return ok;
}

// --scene mm_startstop: a Character with Motion Matching (ABP_Mannequin_MM) on an open floor, driven by a script
// (editor_mm_startstop_tick): idle, walk, stop, run, stop, jog, stop. Its starts and stops are the made ones
// (anim_mm_gen.inc): they must play, the feet must keep their place (editor_mm_startstop_check)
// --scene mm_examples: the same for the four characters of the Motion Matching examples pack (anim_mm_examples.inc) side
// by side, each at its own walk and run: only their idle, walk and run given, the rest made in their styles
// --scene mm_longtake: a Character whose database is one long take (ABP_LongTake: MM_LongTake alone): walk, a left turn
// walking, stop, a pivot back into a walk, run, stop - the search must find those moments in it, a clip must turn it
// (the take's root turn), the feet keep their place, it ends facing back
// --scene mm_authored: the walk / stop / run script on ABP_Authored (its own starts and stops, files with root motion):
// they must play before the made ones, the feet in place
// --scene mm_jump: a Character on ABP_JumpTake (MM_JumpTake alone: jumps in one take, their height on the root) asked to
// Jump standing, walking and running: each must be the take's (vertical root motion carrying the capsule: up 50 cm or
// more, landed), none unasked, the height out of the pose (the pelvis over the capsule's bottom as standing), on the
// floor; a fourth under a low ceiling where it stops: blocked, it falls back by gravity
enum { MMSS_STARTSTOP = 0, MMSS_EXAMPLES, MMSS_LONGTAKE, MMSS_AUTHORED, MMSS_JUMP };
static struct { Entity e; float walk, run; } g_mmss[4];
static int g_nmmss, g_mmss_kind;
static double g_mmss_t;
static struct { int asked, done; float z0, rise, pelvis, pelvis_rest; } g_mmj; // (mm_jump: the jumps asked, the capsule's rise, the pelvis's highest)
static void mmss_walker(Editor* ed, const char* name, const char* mesh, const char* abp, float y, float walk, float run) {
    World* w = &ed->world;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_SKELETAL_MESH));
    if (e == ENTITY_NONE || g_nmmss >= 4) return;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[1] = y, t->location[2] = 92.0f, t->flags = MOBILITY_MOVABLE;
    CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    c->max_walk_speed = walk, c->jump_z_velocity = 520.0f, c->gravity_scale = 1.0f, c->radius = 34.0f, c->half_height = 88.0f;
    CSkeletalMesh* sk = ecs_get(w, e, COMP_SKELETAL_MESH);
    const SkelMesh* m = anim_mesh_name(mesh);
    const BpAsset* a = bp_find_name(BPA_CLASS, abp);
    sk->mesh = m ? m->hash : 0, sk->anim_class = a ? a->hash : 0;
    g_mmss[g_nmmss].e = e, g_mmss[g_nmmss].walk = walk, g_mmss[g_nmmss++].run = run;
}

static void mm_startstop_scene(Editor* ed, int kind) {
    if (kind != MMSS_STARTSTOP) anim_make_pack(ANIM_PACK_MM_EXAMPLES); // (the examples, the long take)
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    float g[3] = { 0.5f, 0.5f, 0.5f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 1000.0f, 0, 0, g, 0, 0.7f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = 60.0f, ft->scale[1] = kind == MMSS_LONGTAKE ? 60.0f : 12.0f;
    memset(&g_mmj, 0, sizeof g_mmj);
    if (kind == MMSS_JUMP) { // a low ceiling where it stops (30 cm over its head): its last jump hits it
        Entity ce = mesh_actor(ed, SHAPE_CUBE, 2100.0f, 0.0f, 235.0f, g, 0, 0.7f);
        CTransform* cet = ecs_get(w, ce, COMP_TRANSFORM);
        cet->scale[0] = 8.0f, cet->scale[1] = 4.0f, cet->scale[2] = 0.5f, cet->flags = MOBILITY_STATIC;
    }
    ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(w, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    g_nmmss = 0, g_mmss_t = 0.0, g_mmss_kind = kind;
    if (kind == MMSS_EXAMPLES) {
        static const struct { const char* name; float walk, run; } x[4] = { { "Brute", 105, 320 }, { "Runner", 150, 540 }, { "Sneak", 75, 230 }, { "Soldier", 125, 420 } };
        for (int i = 0; i < 4; i++) {
            char sk[64], abp[64];
            snprintf(sk, sizeof sk, "SK_%s", x[i].name), snprintf(abp, sizeof abp, "ABP_%s", x[i].name);
            mmss_walker(ed, x[i].name, sk, abp, -300.0f + 200.0f * (float)i, x[i].walk, x[i].run);
        }
    } else
        mmss_walker(ed, "Walker", "SK_Mannequin",
                    kind == MMSS_LONGTAKE ? "ABP_LongTake" : kind == MMSS_AUTHORED ? "ABP_Authored" : kind == MMSS_JUMP ? "ABP_JumpTake" : "ABP_Mannequin_MM",
                    0.0f, 116.0f, 411.0f);
    Entity cam = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CAMERA)); // (from the side, the whole path)
    snprintf(((CName*)ecs_get(w, cam, COMP_NAME))->text, RE_NAME_MAX, "CameraActor");
    CTransform* ct = ecs_get(w, cam, COMP_TRANSFORM);
    ct->location[0] = kind == MMSS_LONGTAKE ? 300.0f : 700.0f, ct->location[1] = kind == MMSS_EXAMPLES ? -1500.0f : -1100.0f, ct->location[2] = 260.0f;
    ct->rotation[0] = -10.0f, ct->rotation[1] = 90.0f;
    ct->scale[0] = ct->scale[1] = ct->scale[2] = 1.0f;
    CCamera* cc = ecs_get(w, cam, COMP_CAMERA);
    cc->fov_deg = 70.0f, cc->auto_activate = 1;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(w, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -45.0f, lt->rotation[1] = 40.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = ct->location[0]; ed->cam.pos[1] = ct->location[1]; ed->cam.pos[2] = 260.0f;
    ed->cam.yaw = 90.0f; ed->cam.pitch = -10.0f;
}

// The script: at a walk, a run, between them (1, 2, 3), or no input (it stops at its body's pace), toward a direction
// (degrees: the long take's turns)
void editor_mm_startstop_tick(Editor* ed, double dt) {
    if (!ed->playing) return;
    typedef struct { float until; int speed; float dir; } Step;
    static const Step plan[] = { { 1.0f, 0, 0 }, { 3.5f, 1, 0 }, { 5.5f, 0, 0 }, { 8.0f, 2, 0 }, { 10.0f, 0, 0 }, { 12.0f, 3, 0 }, { 14.0f, 0, 0 } };
    static const Step take[] = { { 1.0f, 0, 0 }, { 3.5f, 1, 0 }, { 5.5f, 1, 90 }, { 7.5f, 0, 90 }, { 10.0f, 1, -90 }, { 12.0f, 2, -90 }, { 14.0f, 0, -90 } };
    static const Step jump[] = { { 3.0f, 0, 0 }, { 7.0f, 1, 0 }, { 11.0f, 2, 0 }, { 14.0f, 0, 0 } };
    static const float jump_at[4] = { 1.2f, 5.0f, 9.0f, 12.4f }; // (standing, walking, running; under the ceiling)
    const Step* p = g_mmss_kind == MMSS_LONGTAKE ? take : g_mmss_kind == MMSS_JUMP ? jump : plan;
    size_t n = g_mmss_kind == MMSS_LONGTAKE ? sizeof take / sizeof take[0] : g_mmss_kind == MMSS_JUMP ? sizeof jump / sizeof jump[0] : sizeof plan / sizeof plan[0];
    double was = g_mmss_t;
    g_mmss_t += dt;
    if (g_mmss_kind == MMSS_JUMP && g_nmmss) {
        Entity e = g_mmss[0].e;
        const CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        const CCharacter* c = ecs_get(&ed->world, e, COMP_CHARACTER);
        for (int k = 0; k < 4; k++)
            if (was < jump_at[k] && g_mmss_t >= jump_at[k]) bp_character_jump(&ed->world, e), g_mmj.asked++;
        float rows[3][4];
        if (t && c) {
            if (g_mmss_t < 1.0) g_mmj.z0 = t->location[2]; // (standing: the rest height)
            else g_mmj.rise = fmaxf(g_mmj.rise, t->location[2] - g_mmj.z0);
            if (anim_socket_world(&ed->world, e, "pelvis", rows)) {
                float over = rows[2][3] - (t->location[2] - c->half_height); // (the pelvis over the capsule's bottom)
                if (g_mmss_t < 1.0) g_mmj.pelvis_rest = over;
                else g_mmj.pelvis = fmaxf(g_mmj.pelvis, over);
            }
        }
    }
    int sp = 0;
    float dir = 0.0f;
    for (size_t i = 0; i < n; i++)
        if (g_mmss_t < p[i].until) {
            sp = p[i].speed, dir = p[i].dir * 0.0174533f;
            break;
        }
    for (int i = 0; i < g_nmmss; i++) {
        CCharacter* c = ecs_get(&ed->world, g_mmss[i].e, COMP_CHARACTER);
        if (!c || !sp) continue;
        c->max_walk_speed = sp == 1 ? g_mmss[i].walk : sp == 2 ? g_mmss[i].run : (g_mmss[i].walk + g_mmss[i].run) * 0.5f;
        c->input[0] += cosf(dir), c->input[1] += sinf(dir);
    }
}

// (Q27.12) --scene gi_fallback: outdoors under the sun and the sky (a carport's roof on four pillars, a block of a
// building, a wall), seen from outside: where the GI's fallback matters (Low without rays: under the roof the sky is
// hidden, the ground bounces the sun up). tools/gi_fallback_compare.py compares its GI-only view against the RealGI
static void gi_fallback_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    float g[3] = { 0.45f, 0.43f, 0.4f }, c[3] = { 0.6f, 0.6f, 0.62f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, g, 0, 0.8f);
    CTransform* t = ecs_get(w, fl, COMP_TRANSFORM);
    t->scale[0] = t->scale[1] = 60.0f, t->flags = MOBILITY_STATIC;
    static const float boxes[][6] = { // center x, y, z, size x, y, z (cm)
        { 0, 0, 310, 600, 600, 20 },                                                                         // the carport's roof
        { -280, -280, 150, 20, 20, 300 }, { 280, -280, 150, 20, 20, 300 }, { -280, 280, 150, 20, 20, 300 }, { 280, 280, 150, 20, 20, 300 },
        { 1200, 0, 400, 1000, 600, 800 },                                                                    // a building's block
        { 0, 900, 150, 1600, 30, 300 },                                                                      // a wall
    };
    for (size_t i = 0; i < sizeof boxes / sizeof boxes[0]; i++) {
        Entity e = mesh_actor(ed, SHAPE_CUBE, boxes[i][0], boxes[i][1], boxes[i][2], c, 0, 0.7f);
        t = ecs_get(w, e, COMP_TRANSFORM);
        t->scale[0] = boxes[i][3] / 100.0f, t->scale[1] = boxes[i][4] / 100.0f, t->scale[2] = boxes[i][5] / 100.0f, t->flags = MOBILITY_STATIC;
    }
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(w, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -50.0f, lt->rotation[1] = 35.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = -900.0f, ed->cam.pos[1] = -1100.0f, ed->cam.pos[2] = 300.0f, ed->cam.yaw = 45.0f, ed->cam.pitch = -12.0f;
}

// (Q27.11) --scene loco_styles: the Mannequin on Motion Matching walking on, its style switched every 2 s (Normal, Injured,
// Tired, Drunk, Carrying, Normal). The check: no pop at a switch (the head's move against the pelvis per frame no larger
// than walking makes), the tired one bows (the head lower over the pelvis), the layer's cost a character
static double g_mms_t;
static float g_mms_prev[3], g_mms_steady, g_mms_switch, g_mms_h_normal, g_mms_h_tired, g_mms_hand_n, g_mms_hand_c;
static uint32_t g_mms_nn, g_mms_nt, g_mms_nc;
static bool g_mms_have;
static void mm_styles_scene(Editor* ed) {
    mm_startstop_scene(ed, MMSS_STARTSTOP);
    g_mms_t = 0.0, g_mms_steady = g_mms_switch = g_mms_h_normal = g_mms_h_tired = g_mms_hand_n = g_mms_hand_c = 0.0f, g_mms_nn = g_mms_nt = g_mms_nc = 0, g_mms_have = false;
}
void editor_mm_styles_tick(Editor* ed, double dt) {
    if (!ed->playing || !g_nmmss) return;
    double was = g_mms_t;
    g_mms_t += dt;
    Entity e = g_mmss[0].e;
    static const int plan[6] = { ANIM_STYLE_NORMAL, ANIM_STYLE_INJURED, ANIM_STYLE_TIRED, ANIM_STYLE_DRUNK, ANIM_STYLE_CARRY, ANIM_STYLE_NORMAL };
    int k = (int)(g_mms_t / 2.0);
    if (k > 5) k = 5;
    if ((int)(was / 2.0) != k || was == 0.0) anim_set_locomotion_style(&ed->world, e, plan[k]);
    CCharacter* c = ecs_get(&ed->world, e, COMP_CHARACTER);
    if (c && g_mms_t > 0.5) c->max_walk_speed = g_mmss[0].walk, c->input[0] += 1.0f;
    float hd[3][4], pv[3][4];
    if (!anim_socket_world(&ed->world, e, "head", hd) || !anim_socket_world(&ed->world, e, "pelvis", pv)) return;
    float o[3] = { hd[0][3] - pv[0][3], hd[1][3] - pv[1][3], hd[2][3] - pv[2][3] };
    const CTransform* at = ecs_get(&ed->world, e, COMP_TRANSFORM); // (ahead: where the actor faces)
    float yw = at ? at->rotation[1] * 0.0174533f : 0.0f, fx = cosf(yw), fy = sinf(yw), ahead = o[0] * fx + o[1] * fy;
    if (g_mms_have && dt > 1e-4) {
        float d = sqrtf((o[0] - g_mms_prev[0]) * (o[0] - g_mms_prev[0]) + (o[1] - g_mms_prev[1]) * (o[1] - g_mms_prev[1]) + (o[2] - g_mms_prev[2]) * (o[2] - g_mms_prev[2])) / (float)dt;
        double in2 = fmod(g_mms_t, 2.0);
        if (g_mms_t > 1.0 && g_mms_t < 2.0) g_mms_steady = fmaxf(g_mms_steady, d); // (walking, normal: cm/s)
        if (g_mms_t > 2.0 && g_mms_t < 10.5 && in2 < 0.5) g_mms_switch = fmaxf(g_mms_switch, d);
    }
    memcpy(g_mms_prev, o, sizeof o), g_mms_have = true;
    if (g_mms_t > 1.0 && g_mms_t < 2.0) g_mms_h_normal += ahead, g_mms_nn++; // (the head ahead of the pelvis)
    if (g_mms_t > 5.0 && g_mms_t < 6.0) g_mms_h_tired += ahead, g_mms_nt++;
    float hr[3][4]; // (the carrying: the right hand ahead of the pelvis, the arms up holding it)
    if (anim_socket_world(&ed->world, e, "hand_r", hr)) {
        float ha = hr[2][3] - pv[2][3]; // (its height over the pelvis: held up at the chest)
        if (g_mms_t > 1.0 && g_mms_t < 2.0) g_mms_hand_n += ha;
        if (g_mms_t > 9.0 && g_mms_t < 10.0) g_mms_hand_c += ha, g_mms_nc++;
    }
}
bool editor_mm_styles_check(Editor* ed) {
    (void)ed;
    double ms;
    uint32_t n;
    anim_style_cost(&ms, &n);
    float hn = g_mms_nn ? g_mms_h_normal / (float)g_mms_nn : 0.0f, ht = g_mms_nt ? g_mms_h_tired / (float)g_mms_nt : 0.0f;
    float an = g_mms_nn ? g_mms_hand_n / (float)g_mms_nn : 0.0f, ac = g_mms_nc ? g_mms_hand_c / (float)g_mms_nc : 0.0f;
    printf("[RealEngine] Locomotion styles: the head over the pelvis moves at most %.0f cm/s walking, %.0f cm/s at a switch; tired: the head %.1f cm further ahead; carrying: the hand %.1f cm higher; "
           "the layer %.4f ms a character (%u)\n", g_mms_steady, g_mms_switch, ht - hn, ac - an, n ? ms / n : 0.0, n);
    bool ok = g_mms_steady > 0.0f && g_mms_switch <= 2.0f * g_mms_steady + 20.0f;
    if (!ok) printf("[RealEngine] FAIL: Locomotion styles: a switch popped the pose\n");
    if (ok && ac - an < 15.0f) printf("[RealEngine] FAIL: Locomotion styles: the carrying one should hold its hands up\n"), ok = false;
    if (ok && n && ms / n > 0.01) printf("[RealEngine] FAIL: Locomotion styles: the layer over 0.01 ms a character\n"), ok = false;
    return ok;
}

// (2026-11-23) --scene warp: root motion, Motion Warping and paired montages (anim_warp.inc). Two Mannequins on a floor;
// a clip made here (A_RootStep: MM_Walk with its root walking 150 cm/s forward, 1.2 s) and its montages:
//   1. AM_RootStep (Root Motion): A moves the clip's 180 cm forward, its mesh's root staying on the actor;
//   2. AM_WarpStep (a window 0..end on "Target"): A ends at a target set 250 cm ahead and 200 cm to the side, facing 90;
//   3. AM_PairA with its partner AM_PairB (150 cm ahead, facing back, 0.3 s to align): B's montage time is A's, and at
//      the end B stands where A's place, its root motion off, the offset and B's own root motion put it.
static struct { Entity a, b; double t; int step; float a0[4], target[4], mid_root; float pair_dt, pair_dt_max; bool ok, pair_seen; } g_wp;
static Entity wp_char(Editor* ed, const char* name, float x, float y, float yaw) {
    World* w = &ed->world;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_SKELETAL_MESH));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = 92.0f, t->rotation[1] = yaw, t->flags = MOBILITY_MOVABLE;
    CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    c->max_walk_speed = 150.0f, c->jump_z_velocity = 520.0f, c->gravity_scale = 1.0f, c->radius = 34.0f, c->half_height = 88.0f;
    CSkeletalMesh* sk = ecs_get(w, e, COMP_SKELETAL_MESH);
    const SkelMesh* m = anim_mesh_name("SK_Mannequin");
    const BpAsset* a = bp_find_name(BPA_CLASS, "ABP_Mannequin");
    sk->mesh = m ? m->hash : 0, sk->anim_class = a ? a->hash : 0;
    return e;
}
// A_RootStep from MM_Walk: its root's track walking 150 cm/s along +X, 1.2 s, not a loop
static bool wp_make_assets(void) {
    char dir[700];
    snprintf(dir, sizeof dir, "%s/WarpTest", content_root());
    re_make_dirs(dir);
    AnimSeq* walk = anim_seq_name("MM_Walk");
    if (!walk) walk = anim_seq_name("MM_Idle");
    if (!walk || !walk->nframes) return false;
    static AnimSeq s;
    s = *walk;
    snprintf(s.name, sizeof s.name, "A_RootStep");
    snprintf(s.path, sizeof s.path, "%s/A_RootStep.ranim", dir);
    uint32_t nf = (uint32_t)lroundf(1.2f * s.fps) + 1, nt = s.ntracks;
    int rt = -1;
    for (uint32_t k = 0; k < nt; k++)
        if (!strcmp(s.track_bone[k], "root")) rt = (int)k;
    if (rt < 0 && nt < ANIM_MAX_BONES) rt = (int)nt++, snprintf(s.track_bone[rt], ANIM_NAME, "root");
    s.rot = calloc((size_t)nf * nt * 4, sizeof(float)), s.pos = calloc((size_t)nf * nt * 3, sizeof(float));
    if (!s.rot || !s.pos || rt < 0) return free(s.rot), free(s.pos), false;
    for (uint32_t f = 0; f < nf; f++) {
        uint32_t src = f % walk->nframes;
        for (uint32_t k = 0; k < nt; k++) {
            float* q = &s.rot[((size_t)f * nt + k) * 4];
            float* p = &s.pos[((size_t)f * nt + k) * 3];
            if (k < walk->ntracks) memcpy(q, &walk->rot[((size_t)src * walk->ntracks + k) * 4], 16), memcpy(p, &walk->pos[((size_t)src * walk->ntracks + k) * 3], 12);
            else q[3] = 1.0f;
        }
        float* rp = &s.pos[((size_t)f * nt + (uint32_t)rt) * 3];
        rp[0] = 150.0f * (float)f / s.fps, rp[1] = 0.0f, rp[2] = 0.0f; // (the root on the floor, walking forward)
    }
    s.nframes = nf, s.ntracks = nt, s.length = (float)(nf - 1) / s.fps, s.loop = false, s.generator = 0, s.nnotifies = 0, s.ncurves = 0;
    s.map_for = NULL, s.curve_map_for = NULL, s.curve_keys = NULL, s.ncurve_keys = 0;
    bool ok = anim_seq_save(&s);
    free(s.rot), free(s.pos);
    static const char* const names[6] = { "AM_RootStep", "AM_WarpStep", "AM_PairA", "AM_PairB", "AM_PairBack", "AM_Combo" };
    for (int i = 0; i < 6 && ok; i++) {
        AnimMontage mo;
        memset(&mo, 0, sizeof mo);
        snprintf(mo.name, sizeof mo.name, "%s", names[i]);
        snprintf(mo.path, sizeof mo.path, "%s/%s.rmontage", dir, names[i]);
        snprintf(mo.anim, sizeof mo.anim, "A_RootStep"), snprintf(mo.slot, sizeof mo.slot, "DefaultSlot");
        mo.blend_in = 0.1f, mo.blend_out = 0.1f, mo.rate = 1.0f, mo.root_motion = true;
        mo.nsections = 1, snprintf(mo.sections[0].name, ANIM_NAME, "Default");
        if (i == 1) mo.nwarps = 1, snprintf(mo.warps[0].target, ANIM_NAME, "Target"), mo.warps[0].start = 0.0f, mo.warps[0].end = 1.2f,
                    mo.warps[0].flags = ANIM_WARP_TRANSLATION | ANIM_WARP_ROTATION | ANIM_WARP_IGNORE_Z;
        if (i == 2) snprintf(mo.partner, ANIM_NAME, "AM_PairB"), mo.partner_offset[0] = 150.0f, mo.partner_yaw = 180.0f, mo.partner_align = 0.3f;
        if (i == 4) snprintf(mo.partner, ANIM_NAME, "AM_PairB"), mo.partner_offset[0] = -120.0f, mo.partner_yaw = 0.0f, mo.partner_align = 0.3f; // (from behind)
        if (i == 5) { // (2026-11-23) the combo: two attacks, a Combo window chaining the first to the second, a Hit window on the right hand
            mo.root_motion = false, mo.nsections = 2;
            snprintf(mo.sections[0].name, ANIM_NAME, "Attack1"), snprintf(mo.sections[1].name, ANIM_NAME, "Attack2"), mo.sections[1].start = 0.6f;
            mo.nwindows = 2;
            snprintf(mo.windows[0].name, ANIM_NAME, "Chain"), mo.windows[0].kind = ANIM_WIN_COMBO, mo.windows[0].start = 0.25f, mo.windows[0].end = 0.5f;
            snprintf(mo.windows[0].next, ANIM_NAME, "Attack2");
            snprintf(mo.windows[1].name, ANIM_NAME, "Swing"), mo.windows[1].kind = ANIM_WIN_HIT, mo.windows[1].start = 0.05f, mo.windows[1].end = 0.55f;
            snprintf(mo.windows[1].bone, ANIM_NAME, "hand_r"), mo.windows[1].radius = 35.0f;
        }
        ok = anim_montage_save(&mo);
    }
    anim_rescan();
    return ok && anim_seq_name("A_RootStep") && anim_montage_name("AM_PairA") && anim_montage_name("AM_PairB");
}
static void warp_scene(Editor* ed) {
    bool assets = wp_make_assets();
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    float g[3] = { 0.5f, 0.5f, 0.5f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, g, 0, 0.7f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 40.0f, ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(w, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    memset(&g_wp, 0, sizeof g_wp);
    g_wp.a = wp_char(ed, "Primary", 0.0f, 0.0f, 0.0f), g_wp.b = wp_char(ed, "Partner", 100.0f, 400.0f, -90.0f);
    g_wp.ok = assets;
    if (!assets) printf("[RealEngine] FAIL: warp: the test clip and montages weren't made\n");
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(w, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -45.0f, lt->rotation[1] = 40.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = 200.0f, ed->cam.pos[1] = -1100.0f, ed->cam.pos[2] = 500.0f, ed->cam.yaw = 90.0f, ed->cam.pitch = -22.0f;
}
static void wp_place(World* w, Entity e, float out[4]) {
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (t) out[0] = t->location[0], out[1] = t->location[1], out[2] = t->location[2], out[3] = t->rotation[1];
}
void editor_warp_tick(Editor* ed, double dt) {
    if (!ed->playing || !g_wp.a) return;
    World* w = &ed->world;
    double was = g_wp.t;
    g_wp.t += dt;
    float now[4] = { 0, 0, 0, 0 };
    wp_place(w, g_wp.a, now);
    if (was < 0.5 && g_wp.t >= 0.5) { // 1. root motion
        memcpy(g_wp.a0, now, sizeof now);
        if (!anim_play_montage(w, g_wp.a, "AM_RootStep", 1.0f)) printf("[RealEngine] FAIL: warp: AM_RootStep didn't play\n"), g_wp.ok = false;
    }
    if (was < 1.1 && g_wp.t >= 1.1) { // (mid-clip: the mesh's root on the actor, not walked off)
        float rr[3][4];
        if (anim_socket_world(w, g_wp.a, "root", rr)) g_wp.mid_root = hypotf(rr[0][3] - now[0], rr[1][3] - now[1]);
    }
    if (was < 2.2 && g_wp.t >= 2.2) {
        float dx = now[0] - g_wp.a0[0], dy = now[1] - g_wp.a0[1];
        printf("[RealEngine] warp: root motion moved the Primary %.1f, %.1f cm (the clip: 180 forward), its mesh's root %.1f cm off it mid-clip\n", dx, dy, g_wp.mid_root);
        if (fabsf(dx - 180.0f) > 6.0f || fabsf(dy) > 3.0f) printf("[RealEngine] FAIL: warp: the root motion didn't carry the actor the clip's way\n"), g_wp.ok = false;
        if (g_wp.mid_root > 12.0f) printf("[RealEngine] FAIL: warp: the mesh walked off its actor (the root path wasn't taken out)\n"), g_wp.ok = false;
        // 2. warped to a target 250 ahead, 200 to the left, facing 90
        g_wp.target[0] = now[0] + 250.0f, g_wp.target[1] = now[1] + 200.0f, g_wp.target[2] = now[2], g_wp.target[3] = 90.0f;
        anim_warp_target_set(w, g_wp.a, "Target", g_wp.target, 90.0f);
        if (!anim_play_montage(w, g_wp.a, "AM_WarpStep", 1.0f)) printf("[RealEngine] FAIL: warp: AM_WarpStep didn't play\n"), g_wp.ok = false;
    }
    if (was < 3.9 && g_wp.t >= 3.9) {
        float ex = now[0] - g_wp.target[0], ey = now[1] - g_wp.target[1], eyaw = remainderf(now[3] - 90.0f, 360.0f);
        printf("[RealEngine] warp: Motion Warping ended %.1f, %.1f cm and %.1f deg from its target\n", ex, ey, eyaw);
        if (hypotf(ex, ey) > 4.0f || fabsf(eyaw) > 2.0f) printf("[RealEngine] FAIL: warp: the warp window didn't end at its target\n"), g_wp.ok = false;
        // 3. the pair: B slides into its place 150 ahead of A, facing it, then both play together
        if (!anim_play_paired(w, g_wp.a, g_wp.b, "AM_PairA", 1.0f)) printf("[RealEngine] FAIL: warp: Play Paired Montage failed\n"), g_wp.ok = false;
        g_wp.pair_seen = true;
    }
    if (g_wp.t > 4.3 && g_wp.t < 4.9) { // (in step: the partner's montage time is the primary's)
        float ta = -1.0f, tb = -2.0f;
        if (anim_montage_playing(w, g_wp.a, &ta) && anim_montage_playing(w, g_wp.b, &tb)) g_wp.pair_dt_max = fmaxf(g_wp.pair_dt_max, fabsf(ta - tb));
        else g_wp.pair_dt_max = 1.0f;
    }
    if (was < 5.6 && g_wp.t >= 5.6) { // the end: B where A, its root motion off, the offset and B's root motion put it
        float b[4] = { 0, 0, 0, 0 };
        wp_place(w, g_wp.b, b);
        float ay = now[3] * 0.0174533f, ex = now[0] + cosf(ay) * -210.0f, ey = now[1] + sinf(ay) * -210.0f; // (-180 + 150 - 180)
        float eyaw = remainderf(b[3] - (now[3] + 180.0f), 360.0f);
        printf("[RealEngine] warp: paired: the partner's time off the primary's by %.4f s at most; it ended %.1f cm and %.1f deg from its place\n",
               g_wp.pair_dt_max, hypotf(b[0] - ex, b[1] - ey), eyaw);
        if (g_wp.pair_dt_max > 0.001f) printf("[RealEngine] FAIL: warp: the paired montages weren't in step\n"), g_wp.ok = false;
        if (hypotf(b[0] - ex, b[1] - ey) > 4.0f || fabsf(eyaw) > 2.0f) printf("[RealEngine] FAIL: warp: the partner didn't stay in its place from the primary\n"), g_wp.ok = false;
        // (2026-11-23) the pick: the partner put behind the primary facing the same way -> AM_PairBack of the set "AM_Pair";
        // AM_PairA (ahead, facing back) can't play from there; in front facing it, AM_PairA again
        CTransform* bt = ecs_get(w, g_wp.b, COMP_TRANSFORM);
        if (bt) {
            float ay = now[3] * 0.0174533f;
            bt->location[0] = now[0] - cosf(ay) * 115.0f, bt->location[1] = now[1] - sinf(ay) * 115.0f, bt->rotation[1] = now[3] + 8.0f;
            const char* m1 = anim_find_paired(w, g_wp.a, g_wp.b, "AM_Pair", 60.0f, 45.0f);
            bool can_a = anim_can_play_paired(w, g_wp.a, g_wp.b, "AM_PairA", 60.0f, 45.0f), can_b = anim_can_play_paired(w, g_wp.a, g_wp.b, "AM_PairBack", 60.0f, 45.0f);
            bt->location[0] = now[0] + cosf(ay) * 160.0f, bt->location[1] = now[1] + sinf(ay) * 160.0f, bt->rotation[1] = now[3] + 175.0f;
            const char* m2 = anim_find_paired(w, g_wp.a, g_wp.b, "AM_Pair", 60.0f, 45.0f);
            printf("[RealEngine] warp: the pick: behind -> %s (Can Play A %d, Back %d); in front -> %s\n", m1 ? m1 : "none", can_a, can_b, m2 ? m2 : "none");
            if (!m1 || strcmp(m1, "AM_PairBack") || can_a || !can_b || !m2 || strcmp(m2, "AM_PairA"))
                printf("[RealEngine] FAIL: warp: the paired montage picked by the partner's place is wrong\n"), g_wp.ok = false;
        }
        g_wp.step = 1;
    }
}
bool editor_warp_check(Editor* ed) {
    (void)ed;
    if (!g_wp.step) printf("[RealEngine] FAIL: warp: the steps didn't all run (%.1f s)\n", g_wp.t), g_wp.ok = false;
    printf("[RealEngine] warp: %s\n", g_wp.ok ? "ok" : "FAILED");
    return g_wp.ok;
}

// (2026-11-23) --scene traversal: Try Traversal (anim_traversal.inc). Four Mannequins in lanes, each 90 cm before an
// obstacle: a wall 95 high and 30 deep (Vault: over it to the floor behind), a block 95 high and 300 deep (Mantle: onto
// it), a block 175 high (Climb: onto it), a wall 400 high (None: out of reach). The check: each one's action, where it
// ends (on the floor behind / on the top, standing: its height the floor's plus its standing offset), its hands on the
// ledge when they meet it (the warp), the check's cost
static struct { Entity e[4]; int act[4]; float hand_err[4], hand_dz[4], hand_dx[4], hand_mt[4], start[4][3], top[4]; double t; bool ok, done; } g_tv;
static void traversal_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    float g[3] = { 0.5f, 0.5f, 0.5f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, g, 0, 0.7f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 40.0f, ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(w, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    memset(&g_tv, 0, sizeof g_tv);
    g_tv.ok = true;
    static const float box[4][3] = { { 30, 200, 95 }, { 300, 200, 95 }, { 300, 200, 175 }, { 60, 200, 400 } }; // depth, width, height
    float c[3] = { 0.55f, 0.5f, 0.45f };
    for (int i = 0; i < 4; i++) {
        float y = -600.0f + 400.0f * (float)i;
        Entity b = mesh_actor(ed, SHAPE_CUBE, 90.0f + box[i][0] * 0.5f, y, box[i][2] * 0.5f, c, 0, 0.8f);
        CTransform* bt = ecs_get(w, b, COMP_TRANSFORM);
        bt->scale[0] = box[i][0] / 100.0f, bt->scale[1] = box[i][1] / 100.0f, bt->scale[2] = box[i][2] / 100.0f, bt->flags = MOBILITY_STATIC;
        static const char* const names[4] = { "Vaulter", "Mantler", "Climber", "Blocked" };
        g_tv.e[i] = wp_char(ed, names[i], 0.0f, y, 0.0f), g_tv.top[i] = box[i][2];
    }
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(w, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -45.0f, lt->rotation[1] = 40.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = -650.0f, ed->cam.pos[1] = -1150.0f, ed->cam.pos[2] = 330.0f, ed->cam.yaw = 52.0f, ed->cam.pitch = -10.0f;
}
void editor_traversal_tick(Editor* ed, double dt) {
    if (!ed->playing || !g_tv.e[0]) return;
    World* w = &ed->world;
    double was = g_tv.t, t0 = getenv("RE_TRAV_T") ? atof(getenv("RE_TRAV_T")) : 0.8; // (later: to watch it in --shot frames)
    g_tv.t += dt;
    if (was < t0 && g_tv.t >= t0)
        for (int i = 0; i < 4; i++) {
            const CTransform* t = ecs_get(w, g_tv.e[i], COMP_TRANSFORM);
            if (t) memcpy(g_tv.start[i], t->location, 12);
            float h = 0.0f;
            g_tv.act[i] = anim_try_traversal(w, g_tv.e[i], 150.0f, 230.0f, true, &h);
            g_tv.hand_err[i] = -1.0f;
        }
    // the hands where the ledge is when they meet it (the FrontLedge window's end)
    // the hands where the ledge is when they meet it (the FrontLedge window's end, by the montage's own time; the
    // pose is a frame behind at most) - in height and along the way (the front face at x 90)
    // (while they hold it - its keys from the plant to the push: the pose of a far character is sampled less often,
    // so its best frame of the hold) - their height against the top, along the way against the front (x 90; the clips
    // put the palms 3 cm past it)
    static const float plant[4][2] = { { 0.26f * 1.15f, 0.45f * 1.15f }, { 0.32f * 1.25f, 0.50f * 1.25f }, { 0.36f * 1.85f, 0.62f * 1.85f }, { 0, 0 } };
    for (int i = 0; i < 3; i++) {
        float mt = -1.0f, hl[3][4], hr[3][4];
        if (!g_tv.act[i] || !anim_montage_playing(w, g_tv.e[i], &mt) || mt < plant[i][0] || mt > plant[i][1]) continue;
        if (anim_socket_world(w, g_tv.e[i], "hand_l", hl) && anim_socket_world(w, g_tv.e[i], "hand_r", hr)) {
            float dz = 0.5f * (hl[2][3] + hr[2][3]) - g_tv.top[i], dx = 0.5f * (hl[0][3] + hr[0][3]) - 93.0f, e = fmaxf(fabsf(dz), fabsf(dx));
            if (g_tv.hand_err[i] < 0.0f || e < g_tv.hand_err[i]) g_tv.hand_err[i] = e, g_tv.hand_dz[i] = dz, g_tv.hand_dx[i] = dx, g_tv.hand_mt[i] = mt;
        }
    }
    if (was < t0 + 2.8 && g_tv.t >= t0 + 2.8) {
        static const char* const want[4] = { "Vault", "Mantle", "Climb", "None" };
        double ms;
        uint32_t n;
        anim_traversal_cost(&ms, &n);
        for (int i = 0; i < 4; i++) {
            const CTransform* t = ecs_get(w, g_tv.e[i], COMP_TRANSFORM);
            const char* got = anim_traversal_name(g_tv.act[i]);
            float x = t ? t->location[0] : 0, z = t ? t->location[2] : 0, stand = g_tv.start[i][2];
            printf("[RealEngine] traversal: %s: %s, ended at x %.1f z %.1f (started %.1f, %.1f), the hands %.1f cm off the ledge (%.1f up, %.1f along, at %.3f s)\n", want[i], got, x, z,
                   g_tv.start[i][0], stand, g_tv.hand_err[i], g_tv.hand_dz[i], g_tv.hand_dx[i], g_tv.hand_mt[i]);
            if (strcmp(got, want[i])) printf("[RealEngine] FAIL: traversal: the %s obstacle gave %s\n", want[i], got), g_tv.ok = false;
            float want_z = stand + (i == 1 || i == 2 ? g_tv.top[i] : 0.0f);
            bool where = i == 0 ? x > 90.0f + 30.0f + 40.0f && fabsf(z - want_z) < 4.0f
                       : i == 3 ? fabsf(x - g_tv.start[i][0]) < 2.0f && fabsf(z - want_z) < 4.0f
                                : x > 90.0f + 30.0f && x < 90.0f + 300.0f && fabsf(z - want_z) < 4.0f;
            if (!where) printf("[RealEngine] FAIL: traversal: the %s one didn't end where it should (z wanted %.1f)\n", want[i], want_z), g_tv.ok = false;
            if (i < 3 && (g_tv.hand_err[i] < 0.0f || g_tv.hand_err[i] > 12.0f)) printf("[RealEngine] FAIL: traversal: the %s one's hands missed the ledge\n", want[i]), g_tv.ok = false;
        }
        printf("[RealEngine] traversal: a check %.4f ms (%u)\n", n ? ms / n : 0.0, n);
        if (n && ms / n > 0.1) printf("[RealEngine] FAIL: traversal: a check over 0.1 ms\n"), g_tv.ok = false;
        g_tv.done = true;
    }
}
// (2026-11-23) --scene anim_warping: Orientation and Stride Warping (anim_nodes.inc) put in ABP_Mannequin's graph (in
// memory) before its Output. The Mannequin walks 45 degrees off its facing (Use Controller Rotation Yaw): with the warp the
// line of its hips turns toward the move and its chest's doesn't; without it both stay with the facing. Then it walks
// ahead: the animation's speed read from its planted foot matches the Character's; the stride forced to 1.6: the feet
// spread about 1.6 times as far
static struct { Entity e; double t; int ow, sw; float hip_on, chest_on, hip_off, chest_off, spread0, spread1, spd_anim, spd_char; int n_on, n_off, ns;
                float lo0, hi0, lo1, hi1; bool ok, done; } g_aw;
static AnimGraph* aw_graph(void) {
    BpAsset* a = bp_find_name(BPA_CLASS, "ABP_Mannequin");
    return a ? anim_graph_of(a, false) : NULL;
}
static void anim_warping_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    float gcol[3] = { 0.5f, 0.5f, 0.5f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, gcol, 0, 0.7f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 80.0f, ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(w, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    memset(&g_aw, 0, sizeof g_aw);
    g_aw.ok = true, g_aw.ow = g_aw.sw = -1;
    AnimGraph* g = aw_graph();
    int out = -1;
    for (uint32_t i = 0; g && i < g->nnodes; i++)
        if (g->nodes[i].kind == AN_OUTPUT) out = (int)i;
    if (!g || out < 0 || g->nnodes + 2 > ANIM_MAX_NODES) {
        printf("[RealEngine] FAIL: anim warping: no ABP_Mannequin graph to put the nodes in\n"), g_aw.ok = false;
    } else {
        int prev = g->nodes[out].in[0];
        g_aw.ow = (int)g->nnodes++, anim_node_init(&g->nodes[g_aw.ow], AN_ORIENT_WARP), g->nodes[g_aw.ow].in[0] = (int16_t)prev;
        g_aw.sw = (int)g->nnodes++, anim_node_init(&g->nodes[g_aw.sw], AN_STRIDE_WARP), g->nodes[g_aw.sw].in[0] = (int16_t)g_aw.ow;
        g->nodes[g_aw.sw].f[2] = 0.0f; // (the stride off until its own phase)
        g->nodes[out].in[0] = (int16_t)g_aw.sw;
    }
    g_aw.e = wp_char(ed, "Walker", 0.0f, 0.0f, 0.0f);
    CCharacter* c = ecs_get(w, g_aw.e, COMP_CHARACTER);
    if (c) c->use_controller_yaw = 1, c->control_yaw = 0.0f, c->max_walk_speed = 150.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = -300.0f, ed->cam.pos[1] = -700.0f, ed->cam.pos[2] = 300.0f, ed->cam.yaw = 60.0f, ed->cam.pitch = -15.0f;
}
// a line's angle in the ground plane against the actor's right (deg)
static float aw_line(World* w, Entity e, const char* a, const char* b) {
    float A[3][4], B[3][4];
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (!t || !anim_socket_world(w, e, a, A) || !anim_socket_world(w, e, b, B)) return 0.0f;
    float yaw = t->rotation[1] * 0.0174533f, rx = -sinf(yaw), ry = cosf(yaw), fx = cosf(yaw), fy = sinf(yaw);
    float dx = B[0][3] - A[0][3], dy = B[1][3] - A[1][3];
    return atan2f(dx * fx + dy * fy, dx * rx + dy * ry) / 0.0174533f;
}
void editor_anim_warping_tick(Editor* ed, double dt) {
    if (!ed->playing || !g_aw.e || g_aw.ow < 0) return;
    World* w = &ed->world;
    AnimGraph* g = aw_graph();
    double was = g_aw.t;
    g_aw.t += dt;
    CCharacter* c = ecs_get(w, g_aw.e, COMP_CHARACTER);
    if (!c || !g) return;
    bool phase_on = g_aw.t < 3.0, phase_off = g_aw.t >= 3.0 && g_aw.t < 5.5, ahead = g_aw.t >= 5.5;
    g->nodes[g_aw.ow].f[1] = phase_on ? 1.0f : 0.0f;
    if (ahead) c->use_controller_yaw = 0;
    float dir = ahead ? 0.0f : 45.0f * 0.0174533f; // (the move: 45 degrees to the left of the facing, then ahead)
    c->input[0] += cosf(dir), c->input[1] += sinf(dir);
    // the hips' line against the chest's (both from left to right) while walking 45 off
    float hip = aw_line(w, g_aw.e, "thigh_l", "thigh_r"), chest = aw_line(w, g_aw.e, "upperarm_l", "upperarm_r");
    if (g_aw.t > 1.5 && phase_on) g_aw.hip_on += hip, g_aw.chest_on += chest, g_aw.n_on++;
    if (g_aw.t > 4.3 && phase_off) g_aw.hip_off += hip, g_aw.chest_off += chest, g_aw.n_off++;
    // ahead: the animation's speed against the Character's; then the stride forced to 1.6: the feet's spread along the move
    float mv, ow, as, sc;
    anim_warp_state(w, g_aw.e, &mv, &ow, &as, &sc);
    float L[3][4], Rr[3][4];
    const CTransform* t = ecs_get(w, g_aw.e, COMP_TRANSFORM);
    float spread = 0.0f;
    if (t && anim_socket_world(w, g_aw.e, "foot_l", L) && anim_socket_world(w, g_aw.e, "foot_r", Rr)) {
        float yaw = t->rotation[1] * 0.0174533f;
        spread = (L[0][3] - Rr[0][3]) * cosf(yaw) + (L[1][3] - Rr[1][3]) * sinf(yaw);
    }
    if (g_aw.t > 6.5 && g_aw.t < 8.0) {
        g_aw.spd_anim += as, g_aw.spd_char += mv >= 0.0f ? sqrtf(c->velocity[0] * c->velocity[0] + c->velocity[1] * c->velocity[1]) : 0.0f, g_aw.ns++;
        g_aw.lo0 = fminf(g_aw.lo0, spread), g_aw.hi0 = fmaxf(g_aw.hi0, spread);
    }
    if (was < 8.0 && g_aw.t >= 8.0) g->nodes[g_aw.sw].f[0] = g->nodes[g_aw.sw].f[1] = 1.6f, g->nodes[g_aw.sw].f[2] = 1.0f, g->nodes[g_aw.sw].f[3] = 0.0f;
    if (g_aw.t > 9.0 && g_aw.t < 10.5) g_aw.lo1 = fminf(g_aw.lo1, spread), g_aw.hi1 = fmaxf(g_aw.hi1, spread);
    if (was < 10.5 && g_aw.t >= 10.5) {
        float hon = g_aw.n_on ? g_aw.hip_on / g_aw.n_on : 0, con = g_aw.n_on ? g_aw.chest_on / g_aw.n_on : 0;
        float hof = g_aw.n_off ? g_aw.hip_off / g_aw.n_off : 0, cof = g_aw.n_off ? g_aw.chest_off / g_aw.n_off : 0;
        float sa = g_aw.ns ? g_aw.spd_anim / g_aw.ns : 0, scv = g_aw.ns ? g_aw.spd_char / g_aw.ns : 0;
        float r = (g_aw.hi0 - g_aw.lo0) > 1.0f ? (g_aw.hi1 - g_aw.lo1) / (g_aw.hi0 - g_aw.lo0) : 0.0f;
        printf("[RealEngine] anim warping: 45 off: the hips %.1f, the chest %.1f deg (warped); %.1f, %.1f (not); the animation's speed %.0f vs the Character's %.0f cm/s; "
               "the stride 1.6: the feet spread %.2fx\n", hon - con, con, hof - cof, cof, sa, scv, r);
        if (fabsf(fabsf(hon - con) - 45.0f) > 12.0f) printf("[RealEngine] FAIL: anim warping: the legs didn't turn toward the move\n"), g_aw.ok = false;
        if (fabsf(con) > 12.0f) printf("[RealEngine] FAIL: anim warping: the chest turned off its facing\n"), g_aw.ok = false;
        if (fabsf(hof - cof) > 12.0f) printf("[RealEngine] FAIL: anim warping: without the warp the legs turned\n"), g_aw.ok = false;
        if (scv < 50.0f || fabsf(sa - scv) > 0.25f * scv) printf("[RealEngine] FAIL: anim warping: the animation's speed misread\n"), g_aw.ok = false;
        if (r < 1.3f || r > 1.9f) printf("[RealEngine] FAIL: anim warping: the stride didn't stretch\n"), g_aw.ok = false;
        g_aw.done = true;
    }
}
bool editor_anim_warping_check(Editor* ed) {
    (void)ed;
    if (!g_aw.done) printf("[RealEngine] FAIL: anim warping: the check didn't run (%.1f s)\n", g_aw.t), g_aw.ok = false;
    printf("[RealEngine] anim warping: %s\n", g_aw.ok ? "ok" : "FAILED");
    return g_aw.ok;
}

// (2026-11-23) --scene combo: a montage's windows (anim_combo.inc). AM_Combo: Attack1 (0 s), Attack2 (0.6 s); a Combo window
// "Chain" 0.25-0.5 to Attack2; a Hit window "Swing" 0.05-0.55 on the right hand. A plays it without input: it ends with
// Attack1, the dummy B in front hit once (not once a frame); again with the button inside Chain: it goes on into Attack2;
// the button before any window: refused
static struct { Entity a, b; double t; bool ok, done, ended_alone, chained, early_refused, chain_open; uint32_t hits1; float t2;
                float base[2], peak_front[2], peak_right[2], after[2]; int nbase; const char* side[2]; } g_cb;
// the dummy's head over its pelvis in its own frame (forward, right; cm)
static bool cb_head(World* w, Entity e, float out[2]) {
    float h[3][4], p[3][4];
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (!t || !anim_socket_world(w, e, "head", h) || !anim_socket_world(w, e, "pelvis", p)) return false;
    float y = t->rotation[1] * 0.0174533f, dx = h[0][3] - p[0][3], dy = h[1][3] - p[1][3];
    out[0] = dx * cosf(y) + dy * sinf(y), out[1] = -dx * sinf(y) + dy * cosf(y);
    return true;
}
static void combo_scene(Editor* ed) {
    bool assets = wp_make_assets();
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    float gcol[3] = { 0.5f, 0.5f, 0.5f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, gcol, 0, 0.7f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 40.0f, ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(w, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    memset(&g_cb, 0, sizeof g_cb);
    g_cb.ok = assets;
    if (!assets) printf("[RealEngine] FAIL: combo: the test montages weren't made\n");
    g_cb.a = wp_char(ed, "Fighter", 0.0f, 0.0f, 0.0f), g_cb.b = wp_char(ed, "Dummy", 58.0f, 0.0f, 180.0f);
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = 30.0f, ed->cam.pos[1] = -500.0f, ed->cam.pos[2] = 200.0f, ed->cam.yaw = 90.0f, ed->cam.pitch = -10.0f;
}
void editor_combo_tick(Editor* ed, double dt) {
    if (!ed->playing || !g_cb.a) return;
    World* w = &ed->world;
    double was = g_cb.t;
    g_cb.t += dt;
#define AT(s) (was < (s) && g_cb.t >= (s))
    float mt = -1.0f;
    const char* on = anim_montage_playing(w, g_cb.a, &mt);
    if (AT(0.5)) anim_play_montage(w, g_cb.a, "AM_Combo", 1.0f);
    if (AT(1.6)) { // (0.6 + its blend out: ended by itself after Attack1)
        g_cb.ended_alone = !on || strcmp(on, "AM_Combo");
        g_cb.hits1 = anim_montage_hit_count(g_cb.a);
        anim_play_montage(w, g_cb.a, "AM_Combo", 1.0f);
    }
    if (AT(1.62)) g_cb.early_refused = !anim_montage_combo_input(w, g_cb.a); // (mtime ~0.02: before Chain's buffer)
    if (on && !strcmp(on, "AM_Combo") && g_cb.t > 1.9 && g_cb.t < 2.2 && mt > 0.27f && mt < 0.48f && !g_cb.chain_open) {
        g_cb.chain_open = anim_montage_window_open(w, g_cb.a, "Chain");
        anim_montage_combo_input(w, g_cb.a);
    }
    { // (2026-11-23) hit reactions on the dummy: from its front at 3.0, from its right at 4.0
        float hd[2];
        bool got = cb_head(w, g_cb.b, hd);
        const CTransform* bt = ecs_get(w, g_cb.b, COMP_TRANSFORM);
        if (got && g_cb.t > 2.6 && g_cb.t < 2.95) g_cb.base[0] += hd[0], g_cb.base[1] += hd[1], g_cb.nbase++;
        if (bt && AT(3.0)) {
            float y = bt->rotation[1] * 0.0174533f, loc[3] = { bt->location[0] + cosf(y) * 60.0f, bt->location[1] + sinf(y) * 60.0f, bt->location[2] + 40.0f };
            g_cb.side[0] = anim_hit_reaction(w, g_cb.b, loc, 1.0f, "");
        }
        if (bt && AT(4.0)) {
            float y = bt->rotation[1] * 0.0174533f, loc[3] = { bt->location[0] - sinf(y) * 60.0f, bt->location[1] + cosf(y) * 60.0f, bt->location[2] + 40.0f };
            g_cb.side[1] = anim_hit_reaction(w, g_cb.b, loc, 1.0f, "");
        }
        if (got && g_cb.nbase) {
            float d0 = hd[0] - g_cb.base[0] / g_cb.nbase, d1 = hd[1] - g_cb.base[1] / g_cb.nbase;
            if (g_cb.t > 3.0 && g_cb.t < 3.35) g_cb.peak_front[0] = fminf(g_cb.peak_front[0], d0), g_cb.peak_front[1] = fmaxf(g_cb.peak_front[1], d0);
            if (g_cb.t > 4.0 && g_cb.t < 4.35) g_cb.peak_right[1] = fminf(g_cb.peak_right[1], d1);
            if (g_cb.t > 5.2 && g_cb.t < 5.3) g_cb.after[0] = d0, g_cb.after[1] = d1;
        }
    }
    if (AT(5.4)) {
        printf("[RealEngine] combo: hit reactions: from the front (%s) the head went %.1f cm back, from the right (%s) %.1f cm left; 1.3 s later %.1f, %.1f cm off\n",
               g_cb.side[0] ? g_cb.side[0] : "-", -g_cb.peak_front[0], g_cb.side[1] ? g_cb.side[1] : "-", -g_cb.peak_right[1], g_cb.after[0], g_cb.after[1]);
        if (!g_cb.side[0] || strcmp(g_cb.side[0], "Front") || !g_cb.side[1] || strcmp(g_cb.side[1], "Right")) printf("[RealEngine] FAIL: combo: the hit's side misread\n"), g_cb.ok = false;
        if (g_cb.peak_front[0] > -3.0f || g_cb.peak_right[1] > -3.0f) printf("[RealEngine] FAIL: combo: the hit didn't throw the head away from it\n"), g_cb.ok = false;
        if (fabsf(g_cb.after[0]) > 1.5f || fabsf(g_cb.after[1]) > 1.5f) printf("[RealEngine] FAIL: combo: the hit's reaction didn't settle\n"), g_cb.ok = false;
        g_cb.done = true;
    }
    if (AT(2.5)) { // (mtime ~0.9 when chained: Attack2)
        g_cb.chained = on && !strcmp(on, "AM_Combo") && mt > 0.6f, g_cb.t2 = mt;
        printf("[RealEngine] combo: alone it ended after Attack1 (%d), the dummy hit %u time(s); the early press refused (%d); Chain open (%d); "
               "pressed inside Chain: on into Attack2 (%d, at %.2f s)\n", g_cb.ended_alone, g_cb.hits1, g_cb.early_refused, g_cb.chain_open, g_cb.chained, g_cb.t2);
        if (!g_cb.ended_alone) printf("[RealEngine] FAIL: combo: without input the montage went on past its first attack\n"), g_cb.ok = false;
        if (g_cb.hits1 != 1) printf("[RealEngine] FAIL: combo: the Hit window should touch the dummy once\n"), g_cb.ok = false;
        if (!g_cb.early_refused) printf("[RealEngine] FAIL: combo: a press before the window was taken\n"), g_cb.ok = false;
        if (!g_cb.chain_open) printf("[RealEngine] FAIL: combo: Is In Montage Window missed Chain\n"), g_cb.ok = false;
        if (!g_cb.chained) printf("[RealEngine] FAIL: combo: the press inside Chain didn't go on into Attack2\n"), g_cb.ok = false;
    }
#undef AT
}
bool editor_combo_check(Editor* ed) {
    (void)ed;
    if (!g_cb.done) printf("[RealEngine] FAIL: combo: the check didn't run (%.1f s)\n", g_cb.t), g_cb.ok = false;
    printf("[RealEngine] combo: %s\n", g_cb.ok ? "ok" : "FAILED");
    return g_cb.ok;
}

// (2026-11-23) --scene cover: third-person cover (anim_cover.inc). H before a wall 200 high, L before one 100 high, both
// 85 cm from them. Enter Cover: H ends against its wall (its radius + 2 cm off) with its back to it; L's is low, it
// crouches (its pelvis down); H pushed right along the wall stops at its end (At Right Edge); Cover Peek leans it out
// that way; Exit Cover lets it go
static struct { Entity h, l; double t; bool ok, done, in_h, in_l, low_l, edge; float pel0, pel_c, head0, head_p, xh, yaw_h, y_end; int nh; } g_cv;
static float cv_lateral(World* w, Entity e, float* pelvis_z) { // the head over the pelvis to the body's right (cm)
    float h[3][4], p[3][4];
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (!t || !anim_socket_world(w, e, "head", h) || !anim_socket_world(w, e, "pelvis", p)) return 0.0f;
    if (pelvis_z) *pelvis_z = p[2][3];
    float y = t->rotation[1] * 0.0174533f;
    return -(h[0][3] - p[0][3]) * sinf(y) + (h[1][3] - p[1][3]) * cosf(y);
}
static void cover_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    float gcol[3] = { 0.5f, 0.5f, 0.5f }, wc[3] = { 0.55f, 0.5f, 0.45f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, gcol, 0, 0.7f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 40.0f, ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(w, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    static const float wall[2][4] = { { -300.0f, 300.0f, 200.0f, 0 }, { 300.0f, 200.0f, 100.0f, 0 } }; // y, width, height
    for (int i = 0; i < 2; i++) {
        Entity b = mesh_actor(ed, SHAPE_CUBE, 200.0f, wall[i][0], wall[i][2] * 0.5f, wc, 0, 0.8f);
        CTransform* bt = ecs_get(w, b, COMP_TRANSFORM);
        bt->scale[0] = 0.3f, bt->scale[1] = wall[i][1] / 100.0f, bt->scale[2] = wall[i][2] / 100.0f, bt->flags = MOBILITY_STATIC;
    }
    memset(&g_cv, 0, sizeof g_cv);
    g_cv.ok = true;
    g_cv.h = wp_char(ed, "High", 100.0f, -300.0f, 0.0f), g_cv.l = wp_char(ed, "Low", 100.0f, 300.0f, 0.0f);
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = -500.0f, ed->cam.pos[1] = 0.0f, ed->cam.pos[2] = 350.0f, ed->cam.yaw = 0.0f, ed->cam.pitch = -20.0f;
}
void editor_cover_tick(Editor* ed, double dt) {
    if (!ed->playing || !g_cv.h) return;
    World* w = &ed->world;
    double was = g_cv.t;
    g_cv.t += dt;
#define AT(s) (was < (s) && g_cv.t >= (s))
    float pz = 0.0f, lat = cv_lateral(w, g_cv.l, &pz);
    (void)lat;
    if (g_cv.t > 0.3 && g_cv.t < 0.5) g_cv.pel0 = pz; // (L standing)
    if (AT(0.5)) g_cv.in_h = anim_cover_enter(w, g_cv.h, 200.0f), g_cv.in_l = anim_cover_enter(w, g_cv.l, 200.0f);
    if (AT(1.3)) {
        bool lo = false;
        anim_cover_state(w, g_cv.l, &lo, NULL, NULL);
        g_cv.low_l = lo, g_cv.pel_c = pz;
        const CTransform* t = ecs_get(w, g_cv.h, COMP_TRANSFORM);
        if (t) g_cv.xh = t->location[0], g_cv.yaw_h = t->rotation[1];
    }
    CCharacter* c = ecs_get(w, g_cv.h, COMP_CHARACTER);
    if (c && g_cv.t > 1.4 && g_cv.t < 3.4) c->input[1] += 1.0f; // (pushed toward +y: its right, with its back to the wall facing -x)
    float hl = cv_lateral(w, g_cv.h, NULL);
    if (g_cv.t > 3.2 && g_cv.t < 3.4) g_cv.head0 += hl, g_cv.nh++;
    if (AT(3.4)) {
        bool r = false;
        anim_cover_state(w, g_cv.h, NULL, NULL, &r);
        g_cv.edge = r;
        const CTransform* t = ecs_get(w, g_cv.h, COMP_TRANSFORM);
        if (t) g_cv.y_end = t->location[1];
        anim_cover_peek(w, g_cv.h, true);
    }
    if (g_cv.t > 3.8 && g_cv.t < 4.0) g_cv.head_p = hl;
    if (AT(4.0)) anim_cover_peek(w, g_cv.h, false), anim_cover_exit(w, g_cv.h);
    if (AT(4.2)) {
        float lean = g_cv.head_p - (g_cv.nh ? g_cv.head0 / g_cv.nh : 0.0f);
        bool still = anim_cover_state(w, g_cv.h, NULL, NULL, NULL);
        printf("[RealEngine] cover: in (%d, %d); H at x %.1f (wanted %.1f) facing %.1f; L low (%d), its pelvis %.1f cm down; H stopped at y %.1f at its right end (%d); "
               "peeking it leaned %.1f cm out; out again (%d)\n", g_cv.in_h, g_cv.in_l, g_cv.xh, 185.0f - 36.0f, g_cv.yaw_h, g_cv.low_l, g_cv.pel0 - g_cv.pel_c, g_cv.y_end, g_cv.edge, lean, !still);
        if (!g_cv.in_h || !g_cv.in_l) printf("[RealEngine] FAIL: cover: Enter Cover found no wall\n"), g_cv.ok = false;
        if (fabsf(g_cv.xh - 149.0f) > 4.0f || fabsf(remainderf(g_cv.yaw_h - 180.0f, 360.0f)) > 6.0f) printf("[RealEngine] FAIL: cover: H isn't against its wall with its back to it\n"), g_cv.ok = false;
        if (!g_cv.low_l || g_cv.pel0 - g_cv.pel_c < 20.0f) printf("[RealEngine] FAIL: cover: L didn't crouch behind its low wall\n"), g_cv.ok = false;
        if (!g_cv.edge || g_cv.y_end > -150.0f + 5.0f || g_cv.y_end < -150.0f - 70.0f) printf("[RealEngine] FAIL: cover: H didn't stop at its wall's end\n"), g_cv.ok = false;
        if (lean < 8.0f) printf("[RealEngine] FAIL: cover: peeking didn't lean it out\n"), g_cv.ok = false;
        if (still) printf("[RealEngine] FAIL: cover: Exit Cover didn't let it go\n"), g_cv.ok = false;
        g_cv.done = true;
    }
#undef AT
}
bool editor_cover_check(Editor* ed) {
    (void)ed;
    if (!g_cv.done) printf("[RealEngine] FAIL: cover: the check didn't run (%.1f s)\n", g_cv.t), g_cv.ok = false;
    printf("[RealEngine] cover: %s\n", g_cv.ok ? "ok" : "FAILED");
    return g_cv.ok;
}

bool editor_traversal_check(Editor* ed) {
    (void)ed;
    if (!g_tv.done) printf("[RealEngine] FAIL: traversal: the check didn't run (%.1f s)\n", g_tv.t), g_tv.ok = false;
    printf("[RealEngine] traversal: %s\n", g_tv.ok ? "ok" : "FAILED");
    return g_tv.ok;
}

bool editor_mm_startstop_check(Editor* ed) {
    const AnimStats* as = anim_stats();
    bool all = g_nmmss > 0;
    float far = 0.0f, yaw = 0.0f;
    for (int i = 0; i < g_nmmss; i++) {
        const CTransform* t = ecs_get(&ed->world, g_mmss[i].e, COMP_TRANSFORM);
        const CSkeletalMesh* sk = ecs_get(&ed->world, g_mmss[i].e, COMP_SKELETAL_MESH);
        all &= t && sk && sk->mesh && sk->anim_class && hypotf(t->location[0], t->location[1]) > 500.0f; // (its mesh, its ABP, it went)
        if (t) far = fmaxf(far, hypotf(t->location[0], t->location[1])), yaw = t->rotation[1];
    }
    if (g_mmss_kind == MMSS_LONGTAKE) {
        float face = fmodf(yaw + 90.0f + 540.0f + 3600.0f, 360.0f) - 180.0f; // (from facing back, -y: yaw -90)
        printf("[RealEngine] Motion Matching long take: %u searches, %u switches, a clip turned the actor %.2f s; the feet slide %.1f%% of "
               "the travel (%.0f cm), %.0f cm from the start, facing %.0f deg from back (-y)\n",
               as->mm_searches, as->mm_switches, as->mm_turn_s, as->foot_slide * 100.0f, as->travel_cm, far, face);
        bool ok = all && as->mm_switches >= 3 && as->mm_turn_s > 0.3f && as->foot_slide < 0.3f && fabsf(face) < 35.0f;
        if (!ok) printf("[RealEngine] FAIL: Motion Matching should find the long take's moments, turn with it, its feet in place\n");
        return ok;
    }
    if (g_mmss_kind == MMSS_JUMP) {
        const CCharacter* c = g_nmmss ? ecs_get(&ed->world, g_mmss[0].e, COMP_CHARACTER) : NULL;
        const CTransform* t = g_nmmss ? ecs_get(&ed->world, g_mmss[0].e, COMP_TRANSFORM) : NULL;
        printf("[RealEngine] Motion Matching jumps: %d asked, %u by the take (landed %u, blocked %u, switched %u, never left %u), %.2f s carried by "
               "the clip, the capsule up %.0f cm, the pelvis at most %.0f cm over its bottom (%.0f standing), %s, %.0f cm from the start\n",
               g_mmj.asked, as->mm_jumps, as->mm_jump_ends[0], as->mm_jump_ends[1], as->mm_jump_ends[2], as->mm_jump_ends[3], as->mm_jump_s,
               g_mmj.rise, g_mmj.pelvis, g_mmj.pelvis_rest, c && c->on_ground ? "on the floor" : "in the air", far);
        bool ok = all && g_mmj.asked == 4 && as->mm_jumps == 4 && as->mm_jump_ends[0] == 3 && as->mm_jump_ends[1] == 1 && g_mmj.rise > 50.0f && g_mmj.pelvis < g_mmj.pelvis_rest + 35.0f &&
                  c && c->on_ground && t && fabsf(t->location[2] - g_mmj.z0) < 2.0f;
        if (!ok) printf("[RealEngine] FAIL: Motion Matching should jump with the take's jumps (its vertical root motion), only when asked, and land (blocked by the ceiling)\n");
        return ok;
    }
    if (g_mmss_kind == MMSS_AUTHORED) {
        printf("[RealEngine] Motion Matching authored: %u of the user's own one-shots played, %u made starts and %u made stops; the feet slide "
               "%.1f%% of the travel (%.0f cm), %.0f cm from the start\n",
               as->mm_authored, as->mm_starts, as->mm_stops, as->foot_slide * 100.0f, as->travel_cm, far);
        // (its own: the walk start, the walk stop, the run stop; made: only what it has not - the run's and the jog's start, the jog's stop)
        bool ok = all && as->mm_authored >= 3 && as->mm_starts + as->mm_stops <= 5 && as->foot_slide < 0.25f;
        if (!ok) printf("[RealEngine] FAIL: Motion Matching should play the user's own starts and stops first, the feet in place\n");
        return ok;
    }
    printf("[RealEngine] Motion Matching starts and stops: %d characters, %u starts and %u stops played, %u made (their feet sliding %.1f%% as made); "
           "in play the feet slide %.1f%% of the travel (%.0f cm, to %.0f cm), %.1f%% while the made ones play (%.0f cm)\n",
           g_nmmss, as->mm_starts, as->mm_stops, as->mm_made, as->mm_made_slide * 100.0f, as->foot_slide * 100.0f, as->travel_cm, far,
           as->mm_play_slide * 100.0f, as->mm_play_travel);
    int n = g_nmmss > 0 ? g_nmmss : 1;
    bool ok = all && as->mm_made > 0 && as->mm_starts >= 2u * (unsigned)n && as->mm_stops >= 2u * (unsigned)n && as->mm_made_slide < 0.08f &&
              as->mm_play_slide < 0.18f && as->foot_slide < 0.25f;
    if (!ok) printf("[RealEngine] FAIL: Motion Matching's made starts and stops should play, their feet keeping their place\n");
    return ok;
}

// --scene crowdN (crowd.c): a 100 m floor, the player at its Player Start, a Crowd Spawner of N Mannequins 20 m away
// (Surround, Hive), a camera high above; played, they close in and ring the player (editor_crowd_check)
// --scene landscape: a Landscape (200 m, hills) with a Foliage actor's plants in the wind (filled) and a Character dropped from 30 m over it: it must
// stand on the ground (editor_landscape_check), the navigation must cover the hills
static Entity g_land, g_land_player;
static double g_land_paint_t = -1.0;
// (B11) --scene landscape_paint: the three layers painted once the heights exist (the circles at -40, 0, +40 m)
void editor_landscape_paint_tick(Editor* ed, double dt) {
    if (g_land_paint_t < 0.0) return;
    g_land_paint_t += dt;
    if (g_land_paint_t < 0.5 || g_land_paint_t > 10.0) return;
    for (int k = 0; k < 3; k++) {
        landscape_set_paint_layer(k + 1);
        for (int s = 0; s < 6; s++) landscape_brush(&ed->world, g_land, LAND_TOOL_PAINT, (float[]){ -4000.0f + 4000.0f * (float)k, 0, 0 }, 1800.0f, 5000.0f, 0.1f);
        landscape_stroke_end(&ed->world, g_land);
    }
    g_land_paint_t = 100.0; // (once)
}
static void landscape_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    g_land = spawn_actor(ed, SPAWN_LANDSCAPE, 0, (float[]){ 0, 0, 0 }, 0);
    CLandscape* lc = ecs_get(&ed->world, g_land, COMP_LANDSCAPE);
    snprintf(lc->name, sizeof lc->name, "TestLandscape");
    Entity fol = spawn_actor(ed, SPAWN_FOLIAGE, 0, (float[]){ 0, 0, 0 }, 0); // (its plants: a Foliage actor's, Fill once)
    snprintf(((CFoliage*)ecs_get(&ed->world, fol, COMP_FOLIAGE))->name, 32, "@fill");
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 3000 }, 0);
    CTransform* st = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    st->rotation[0] = -35.0f, st->rotation[1] = 40.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, 0, 2800 }, 0);
    Entity pl = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_SKELETAL_MESH));
    snprintf(((CName*)ecs_get(&ed->world, pl, COMP_NAME))->text, RE_NAME_MAX, "Player");
    CTransform* pt = ecs_get(&ed->world, pl, COMP_TRANSFORM);
    pt->location[0] = 2500.0f, pt->location[1] = -1500.0f, pt->location[2] = 3000.0f;
    pt->scale[0] = pt->scale[1] = pt->scale[2] = 1.0f, pt->flags = MOBILITY_MOVABLE;
    CCharacter* pc = ecs_get(&ed->world, pl, COMP_CHARACTER);
    pc->max_walk_speed = 450.0f, pc->jump_z_velocity = 520.0f, pc->gravity_scale = 1.0f, pc->radius = 34.0f, pc->half_height = 88.0f, pc->auto_possess = 1;
    pc->camera_distance = 900.0f, pc->control_pitch = -12.0f;
    const SkelMesh* pm = anim_mesh_name("SK_Mannequin");
    const BpAsset* pabp = bp_find_name(BPA_CLASS, "ABP_Mannequin");
    CSkeletalMesh* psk = ecs_get(&ed->world, pl, COMP_SKELETAL_MESH);
    psk->mesh = pm ? pm->hash : 0, psk->anim_class = pabp ? pabp->hash : 0;
    g_land_player = pl;
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = -6000.0f; ed->cam.pos[1] = -6000.0f; ed->cam.pos[2] = 4000.0f;
    ed->cam.yaw = 45.0f; ed->cam.pitch = -22.0f;
}

// --scene landscape_hm: Unreal's Import from File and Foliage mode. A heightmap (.r16, 257 x 257: a ramp along X) into a
// 128 m landscape of 128 x 128 quads (its heights read back within 1 cm), then a Foliage actor's brush: painted up to the
// density inside the circle, on the ground, then erased (editor_landscape_hm_check)
static Entity g_hm_land, g_hm_fol;
static void landscape_hm_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    char dir[700], path[800];
    snprintf(dir, sizeof dir, "%s/Landscapes", content_root());
    re_make_dirs(dir);
    snprintf(path, sizeof path, "%s/TestRamp.r16", dir);
    static uint16_t px[257 * 257];
    for (int y = 0; y < 257; y++)
        for (int x = 0; x < 257; x++) px[y * 257 + x] = (uint16_t)(x * 65535 / 256);
    FILE* f = re_fopen(path, "wb");
    if (f) fwrite(px, 2, 257 * 257, f), fclose(f);
    g_hm_land = spawn_actor(ed, SPAWN_LANDSCAPE, 0, (float[]){ 0, 0, 0 }, 0);
    CLandscape* lc = ecs_get(&ed->world, g_hm_land, COMP_LANDSCAPE);
    snprintf(lc->name, sizeof lc->name, "TestHeightmap");
    lc->res = 128, lc->chunks = 2, lc->size = 12800.0f, lc->height = 1000.0f;
    landscape_import_heightmap(g_hm_land, path, false);
    g_hm_fol = spawn_actor(ed, SPAWN_FOLIAGE, 0, (float[]){ 0, 0, 0 }, 0);
    snprintf(((CFoliage*)ecs_get(&ed->world, g_hm_fol, COMP_FOLIAGE))->name, 32, "TestPaint");
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 3000 }, 0);
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, 0, 2800 }, 0);
    editor_select(ed, g_hm_fol, SEL_REPLACE); // (the Foliage mode on: its panel in the screenshots)
    ed->foliage_mode = true;
    ed->cam.pos[0] = -9000.0f; ed->cam.pos[1] = -4000.0f; ed->cam.pos[2] = 3500.0f;
    ed->cam.yaw = 25.0f; ed->cam.pitch = -18.0f;
}

bool editor_landscape_hm_check(Editor* ed) {
    bool ok = true;
    float worst = 0.0f;
    for (int gx = 0; gx <= 128; gx += 16) { // (a grid point: the image's sample 2 gx, (2 gx / 256) of the height)
        float z = -1e9f, want = (float)gx / 128.0f * 1000.0f;
        if (!landscape_height(-6400.0f + (float)gx * 100.0f, 300.0f, &z)) z = -1e9f;
        worst = fmaxf(worst, fabsf(z - want));
    }
    ok &= worst < 1.0f;
    // the brush at the middle (500 cm): the default type's 12 / 100 mÂ², up to ~9 in the circle, on the ground
    float at[3] = { 0.0f, 0.0f, 500.0f }, gz;
    if (landscape_height(0.0f, 0.0f, &gz)) at[2] = gz;
    double t0 = re_time_seconds();
    for (int k = 0; k < 3; k++) foliage_paint(&ed->world, g_hm_fol, at, 500.0f, 1.0f, false, 0.5f);
    double paint_ms = (re_time_seconds() - t0) * 1000.0;
    uint32_t painted = foliage_count(g_hm_fol, -1);
    bool inside = true;
    for (uint32_t ai = 0; ai < ed->world.archetype_count; ai++) {
        EcsArchetype* ar = &ed->world.archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CAttach* att = ecs_arch_column(ar, COMP_ATTACH);
        const CTransform* tr = ecs_arch_column(ar, COMP_TRANSFORM);
        for (uint32_t r = 0; r < ar->count; r++) {
            if (att[r].kind != ATTACH_FOLIAGE) continue;
            float z;
            inside &= hypotf(tr[r].location[0], tr[r].location[1]) <= 500.0f && landscape_height(tr[r].location[0], tr[r].location[1], &z) &&
                      fabsf(tr[r].location[2] + 2.0f - z) < 1.0f;
        }
    }
    foliage_paint(&ed->world, g_hm_fol, at, 500.0f, 1.0f, true, 0.1f);
    uint32_t left = foliage_count(g_hm_fol, -1);
    printf("[RealEngine] Heightmap: %s, worst height error %.3f cm; Foliage brush: %u painted (%.2f ms for 3 dabs), %s, %u left after erasing\n",
           landscape_stats()->import_msg, worst, painted, paint_ms, inside ? "all inside and on the ground" : "SOME OUTSIDE", left);
    ok &= painted >= 5 && painted <= 12 && inside && left == 0;
    if (!ok) printf("[RealEngine] FAIL: Landscape heightmap / Foliage brush\n");
    return ok;
}

bool editor_landscape_check(Editor* ed) {
    const LandStats* ls = landscape_stats();
    const CTransform* pt = ecs_get(&ed->world, g_land_player, COMP_TRANSFORM);
    const CCharacter* pc = ecs_get(&ed->world, g_land_player, COMP_CHARACTER);
    float gz = 0.0f;
    bool ground = pt && landscape_height(pt->location[0], pt->location[1], &gz);
    float feet = pt && pc ? pt->location[2] - pc->half_height : -1e9f;
    if (!ai_nav_info() || !ai_nav_info()->walkable) ai_nav_build(&ed->world); // (no AI here: built for the check)
    const NavInfo* nav = ai_nav_info();
    const FoliageStats* fs = foliage_stats();
    printf("[RealEngine] Landscape: %u landscapes, %u chunks, %u triangles, %u plants (built in %.1f ms, the plants %.1f ms); the Character's feet %.1f cm over the ground (%s), navigation %u walkable cells\n",
           ls->landscapes, ls->chunks, ls->triangles, fs->instances, ls->build_ms, fs->build_ms, feet - gz, pc && pc->on_ground ? "standing" : "in the air",
           nav ? nav->walkable : 0u);
    bool ok = ls->landscapes == 1 && ls->chunks == 16 && fs->instances > 1000 && ground && fabsf(feet - gz) < 3.0f && pc->on_ground && nav && nav->walkable > 10000;
    if (!ok) printf("[RealEngine] FAIL: Landscape: its chunks and plants, a Character standing on it\n");
    // (B11) a layer painted at the middle, the stroke undone, the heights exported as .r16
    landscape_set_paint_layer(2);
    float before = landscape_layer_weight(g_land, 2, 0, 0);
    for (int k = 0; k < 10; k++) landscape_brush(&ed->world, g_land, LAND_TOOL_PAINT, (float[]){ 0, 0, 0 }, 3000.0f, 5000.0f, 0.1f);
    landscape_stroke_end(&ed->world, g_land);
    float painted = landscape_layer_weight(g_land, 2, 0, 0), other = landscape_layer_weight(g_land, 1, 0, 0), far = landscape_layer_weight(g_land, 2, 8000, 8000);
    bool undone = landscape_undo(&ed->world, g_land);
    float after = landscape_layer_weight(g_land, 2, 0, 0);
    char r16[900];
    float lo = 0.0f, hi = 0.0f;
    snprintf(r16, sizeof r16, "%s/Landscapes/TestLandscape.r16", content_root());
    bool exp = landscape_export_heightmap(g_land, r16, &lo, &hi);
    uint64_t sz = 0;
    re_file_stat(r16, &sz, NULL, NULL);
    const CLandscape* lc = ecs_get(&ed->world, g_land, COMP_LANDSCAPE);
    uint64_t want = lc ? (uint64_t)(lc->res + 1) * (lc->res + 1) * 2u : 0u;
    re_remove_file(r16);
    printf("[RealEngine] Landscape B11: layer 2 painted %.2f at the middle (layer 1 %.2f, far %.2f), undone %d -> %.2f; .r16 %llu bytes (%llu wanted), %.0f..%.0f cm\n",
           painted, other, far, undone, after, (unsigned long long)sz, (unsigned long long)want, lo, hi);
    if (painted < 0.9f || far > before + 0.01f || !undone || fabsf(after - before) > 0.01f || !exp || sz != want || hi <= lo) {
        printf("[RealEngine] FAIL: Landscape: the painted layers, the undo or the heightmap export\n");
        ok = false;
    }
    return ok;
}

// --scene proto: a blockout (stairs up to a platform, a room with a door, a corridor, a wall); a Character dropped over
// the platform stands on its slab; the navigation goes up the stairs (editor_proto_check)
static Entity g_proto_player, g_proto_platform, g_proto_spline;
static void proto_scene(Editor* ed) {
    template_ground(ed, 40.0f);
    World* w = &ed->world;
    Entity st = spawn_actor(ed, SPAWN_PROTO, PROTO_STAIRS, (float[]){ -200, 0, 0 }, 0);
    ((CProto*)ecs_get(w, st, COMP_PROTO))->size[2] = 240.0f;
    g_proto_platform = spawn_actor(ed, SPAWN_PROTO, PROTO_PLATFORM, (float[]){ 200, 0, 0 }, 0);
    CProto* pf = ecs_get(w, g_proto_platform, COMP_PROTO);
    pf->size[0] = 400.0f, pf->size[1] = 400.0f, pf->size[2] = 240.0f;
    Entity rm = spawn_actor(ed, SPAWN_PROTO, PROTO_ROOM, (float[]){ 0, 900, 0 }, 0);
    ((CProto*)ecs_get(w, rm, COMP_PROTO))->flags = PROTO_CEILING;
    spawn_actor(ed, SPAWN_PROTO, PROTO_CORRIDOR, (float[]){ 1400, 900, 0 }, 0);
    Entity wl = spawn_actor(ed, SPAWN_PROTO, PROTO_WALL, (float[]){ 0, -700, 0 }, 0);
    ((CProto*)ecs_get(w, wl, COMP_PROTO))->opening[0] = 140.0f;
    spawn_actor(ed, SPAWN_PROTO, PROTO_RAMP, (float[]){ -900, -300, 0 }, 0);
    { // (2026-11-10) a wall along a spline: an S through four points (its boxes follow the curve)
        g_proto_spline = spawn_actor(ed, SPAWN_PROTO, PROTO_WALL, (float[]){ -1200, -1400, 0 }, 0);
        CProto* sw = ecs_get(w, g_proto_spline, COMP_PROTO);
        static const float pts[4][3] = { { -600, 0, 0 }, { -200, 300, 0 }, { 200, -300, 0 }, { 600, 0, 0 } };
        sw->npoints = 4;
        memcpy(sw->points, pts, sizeof pts);
    }
    Entity pl = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_SKELETAL_MESH));
    snprintf(((CName*)ecs_get(w, pl, COMP_NAME))->text, RE_NAME_MAX, "Player");
    CTransform* pt = ecs_get(w, pl, COMP_TRANSFORM);
    pt->location[0] = 250.0f, pt->location[1] = 50.0f, pt->location[2] = 900.0f;
    pt->scale[0] = pt->scale[1] = pt->scale[2] = 1.0f, pt->flags = MOBILITY_MOVABLE;
    CCharacter* pc = ecs_get(w, pl, COMP_CHARACTER);
    pc->max_walk_speed = 450.0f, pc->jump_z_velocity = 520.0f, pc->gravity_scale = 1.0f, pc->radius = 34.0f, pc->half_height = 88.0f, pc->auto_possess = 1;
    pc->camera_distance = 1100.0f, pc->control_pitch = -25.0f, pc->control_yaw = 120.0f;
    const SkelMesh* pm = anim_mesh_name("SK_Mannequin");
    const BpAsset* pabp = bp_find_name(BPA_CLASS, "ABP_Mannequin");
    CSkeletalMesh* psk = ecs_get(w, pl, COMP_SKELETAL_MESH);
    psk->mesh = pm ? pm->hash : 0, psk->anim_class = pabp ? pabp->hash : 0;
    g_proto_player = pl;
    proto_update(w);
    ed->selected = ENTITY_NONE;
}

bool editor_proto_check(Editor* ed) {
    World* w = &ed->world;
    uint32_t boxes = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH))) continue;
        const CAttach* at = ecs_arch_column((EcsArchetype*)ar, COMP_ATTACH);
        for (uint32_t r = 0; r < ar->count; r++) boxes += at[r].kind == ATTACH_PROTO;
    }
    const CTransform* pt = ecs_get(w, g_proto_player, COMP_TRANSFORM);
    const CCharacter* pc = ecs_get(w, g_proto_player, COMP_CHARACTER);
    float feet = pt && pc ? pt->location[2] - pc->half_height : -1.0f;
    if (!ai_nav_info() || !ai_nav_info()->walkable) ai_nav_build(w);
    float from[3] = { -600, 0, 10 }, to[3] = { -30, 0, 250 }, path[64][3]; // (the top step: solid down to the floor)
    uint32_t np = 0;
    bool up = ai_nav_path(from, to, path, 64, &np) && np > 0 && path[np - 1][2] > 200.0f;
    printf("[RealEngine] Prototyping: %u boxes; the Character's feet at %.1f cm (the platform: 240), %s; a path up the stairs: %s (%u points)\n",
           boxes, feet, pc && pc->on_ground ? "standing" : "in the air", up ? "yes" : "no", np);
    // (2026-11-10) the spline wall: many boxes, turned along the S (their yaws spread)
    uint32_t sb = 0;
    float ymin = 1e9f, ymax = -1e9f;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CAttach* at = ecs_arch_column((EcsArchetype*)ar, COMP_ATTACH);
        const CTransform* bt = ecs_arch_column((EcsArchetype*)ar, COMP_TRANSFORM);
        for (uint32_t r = 0; r < ar->count; r++)
            if (at[r].kind == ATTACH_PROTO && at[r].owner == g_proto_spline) sb++, ymin = fminf(ymin, bt[r].rotation[1]), ymax = fmaxf(ymax, bt[r].rotation[1]);
    }
    printf("[RealEngine] Prototyping: the spline wall in %u boxes, their yaw from %.0f to %.0f degrees\n", sb, ymin, ymax);
    bool ok = boxes > 20 && fabsf(feet - 240.0f) < 3.0f && pc && pc->on_ground && up && sb >= 10 && ymax - ymin > 60.0f;
    if (!ok) printf("[RealEngine] FAIL: Prototyping: the boxes, the Character on the platform, the path up the stairs\n");
    return ok;
}

// --scene packs: every animation pack made, then the engine's characters and creatures playing their clips in a row
// (the male and female Mannequins, the Locomotion pack's strafe and crouch walk, the dog, the fish, the bird, the lizard)
static uint32_t g_pack_files;
static void packs_scene(Editor* ed) {
    g_pack_files = 0;
    for (int p = 0; p < ANIM_PACK_COUNT; p++) g_pack_files += anim_make_pack(p);
    template_ground(ed, 30.0f);
    World* w = &ed->world;
    static const struct { const char* mesh; const char* clip; float x, y, z, yaw; } show[] = {
        { "SK_Mannequin_Male", "MM_Walk", -300, -400, 0, 90 }, { "SK_Mannequin_Female", "MM_Run", -300, -150, 0, 90 },
        { "SK_Mannequin", "MM_StrafeLeft", -300, 100, 0, 90 }, { "SK_Mannequin", "MM_CrouchWalk", -300, 350, 0, 90 },
        { "SK_Dog", "Dog_Run", 150, -350, 0, 90 }, { "SK_Fish", "Fish_Swim", 150, -50, 60, 90 },
        { "SK_Bird", "Bird_Fly", 150, 200, 120, 90 }, { "SK_Lizard", "Lizard_Walk", 150, 420, 0, 90 },
    };
    for (size_t i = 0; i < sizeof show / sizeof show[0]; i++) {
        Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SKELETAL_MESH));
        if (e == ENTITY_NONE) break;
        snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", show[i].mesh);
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->location[0] = show[i].x, t->location[1] = show[i].y, t->location[2] = show[i].z, t->rotation[1] = show[i].yaw;
        t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE;
        CSkeletalMesh* sk = ecs_get(w, e, COMP_SKELETAL_MESH);
        const SkelMesh* m = anim_mesh_name(show[i].mesh);
        const AnimSeq* a = anim_seq_name(show[i].clip);
        sk->mesh = m ? m->hash : 0, sk->anim = a ? a->hash : 0, sk->play_rate = 1.0f, sk->loop = 1;
    }
    ed->selected = ENTITY_NONE;
    Entity cam = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CAMERA)); // (the play's view: the row)
    snprintf(((CName*)ecs_get(w, cam, COMP_NAME))->text, RE_NAME_MAX, "CameraActor");
    CTransform* ct = ecs_get(w, cam, COMP_TRANSFORM);
    ct->location[0] = 1000.0f, ct->location[2] = 230.0f, ct->rotation[0] = -9.0f, ct->rotation[1] = 180.0f;
    ct->scale[0] = ct->scale[1] = ct->scale[2] = 1.0f;
    CCamera* cc = ecs_get(w, cam, COMP_CAMERA);
    cc->fov_deg = 75.0f, cc->auto_activate = 1;
    ed->cam.pos[0] = 1100.0f; ed->cam.pos[1] = 0.0f; ed->cam.pos[2] = 260.0f;
    ed->cam.yaw = 180.0f; ed->cam.pitch = -8.0f;
}

bool editor_packs_check(Editor* ed) {
    (void)ed;
    const AnimStats* s = anim_stats();
    bool meshes = anim_mesh_name("SK_Mannequin_Male") && anim_mesh_name("SK_Mannequin_Female") && anim_mesh_name("SK_Dog") &&
                  anim_mesh_name("SK_Fish") && anim_mesh_name("SK_Bird") && anim_mesh_name("SK_Lizard");
    printf("[RealEngine] Packs: %u files made; %u skeletal meshes animated (%u parts), %.3f ms\n", g_pack_files, s->meshes, s->parts, s->ms);
    bool ok = meshes && g_pack_files >= 29 && s->meshes >= 8;
    if (!ok) printf("[RealEngine] FAIL: Packs: the characters and creatures, their clips, playing\n");
    return ok;
}

// --scene autorig: a statue of a person in a T-pose (the male Mannequin's parts as one Static Mesh), rigged by Auto Rig,
// then walking with the Mannequin's walk (editor_autorig_check)
static TlendReport g_rig_report;
static bool g_rig_ok;
static Entity g_rig_actor;
// --scene heavy: the heavy model (heavy_test.c: 2.1 M triangles, 600 k of them Masked cards, 4096 materials) 3 m from the
// camera, eight copies from 8 to 60 m (their LODs, mesh_build.c), the sun, the sky and a point light of 30 cm (area
// shadows through the cards). The fase 0 of the heavy meshes' work (notas-dev/render.md): the test asks 100 fps at Epic
// rocks: --scene rocks (the cluster LOD's measure, notas-dev/render.md "LOD de clusters"): only the opaque rock, 100 of
// them (1.54 M triangles each) in a grid, the camera low among them: many near at once, each one near and far
// mode 0: the heavy scene; 1: rocks (the cluster LOD DAG); 2: rocks_tess (the same rocks displaced by Nanite
// Tessellation, Q17); 3: tess_seams (one displaced rock whole in the view, the seam check on: notas-dev/tessellation.md)
static void heavy_scene_ex(Editor* ed, int mode) {
    bool rocks = mode >= 1;
    template_ground(ed, 80.0f);
    World* w = &ed->world;
    if (!project_file()[0]) { // (the build's Content would carry 2 M triangles into every other test)
        printf("[RealEngine] FAIL: heavy test: needs a project (--new-project folder/Name)\n");
        return;
    }
    HeavyAssets ha;
    if (!heavy_test_assets(content_root(), &ha)) printf("[RealEngine] FAIL: heavy test: its assets not written\n");
    if (ha.ms > 0.0) printf("[RealEngine] Heavy test: assets made in %.0f ms (kept for the next runs)\n", ha.ms);
    render_meshes_reload(ed->vk);
    Material* rm = mat_load(mode >= 2 ? ha.rock_tess_mat : ha.rock_mat);
    Material* cm = mat_load(ha.cards_mat);
    if (mode >= 2 && (!rm || !rm->tessellation)) printf("[RealEngine] FAIL: rocks_tess: the displaced rock's material (Enable Tessellation) not loaded\n");
    tess_set_seam_check(mode == 3);
    static const float at[9][3] = { { 0, 0, 0 }, { 800, 500, 0 }, { 900, -700, 0 }, { 1600, 200, 0 }, { 2200, -1200, 0 },
                                    { 3000, 900, 0 }, { 3800, -300, 0 }, { 4800, 1500, 0 }, { 6000, -600, 0 } };
    if (rocks && !render_any_dag() && !getenv("REALENGINE_NO_DAG")) printf("[RealEngine] FAIL: rocks test: the rock has no cluster LOD DAG\n");
    for (int i = 0; rocks && i < (mode == 3 ? 1 : 100); i++) {
        Entity e = mesh_actor(ed, SHAPE_CUBE, (float)(i % 10) * 520.0f, (float)(i / 10) * 520.0f, 0, (float[]){ 0.6f, 0.55f, 0.5f }, 0, 0.7f);
        CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
        sm->mesh = render_mesh_id_of(ha.rock_mesh);
        sm->material = rm ? rm->hash : 0u;
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->flags = MOBILITY_STATIC;
        float s = 1.2f + (float)((i * 7) % 5) * 0.3f;
        t->scale[0] = t->scale[1] = t->scale[2] = s;
        t->rotation[1] = (float)(i * 47 % 360);
        char nm[32];
        snprintf(nm, sizeof nm, "Rock%d", i);
        set_name(w, e, nm);
    }
    for (int i = 0; !rocks && i < 9; i++) {
        for (int part = 0; part < 2; part++) {
            Entity e = mesh_actor(ed, SHAPE_CUBE, at[i][0], at[i][1], at[i][2], (float[]){ 0.6f, 0.55f, 0.5f }, 0, 0.7f);
            CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
            sm->mesh = render_mesh_id_of(part ? ha.cards_mesh : ha.rock_mesh);
            sm->material = part ? (cm ? cm->hash : 0u) : (rm ? rm->hash : 0u);
            CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
            t->flags = MOBILITY_STATIC;
            char nm[32];
            snprintf(nm, sizeof nm, "%s%d", part ? "HeavyCards" : "HeavyRock", i);
            set_name(w, e, nm);
        }
    }
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_POINT, (float[]){ -180, -200, 330 }, 0);
    CLight* pl = ecs_get(w, ed->selected, COMP_LIGHT);
    pl->intensity = 60000.0f, pl->source_radius = 30.0f, pl->cast_shadows = 1;
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = -380.0f; ed->cam.pos[1] = -160.0f; ed->cam.pos[2] = 170.0f;
    ed->cam.yaw = 20.0f; ed->cam.pitch = -6.0f;
    if (rocks) ed->cam.pos[0] = 1300.0f, ed->cam.pos[1] = 1300.0f, ed->cam.pos[2] = 140.0f, ed->cam.yaw = 45.0f, ed->cam.pitch = -4.0f;
    if (mode == 3) ed->cam.pos[0] = -650.0f, ed->cam.pos[1] = 0.0f, ed->cam.pos[2] = 60.0f, ed->cam.yaw = 0.0f, ed->cam.pitch = -4.0f; // (the rock whole in the view)
}
static void heavy_scene(Editor* ed) { heavy_scene_ex(ed, 0); }

// (2026-11-10) Masked / Two Sided materials with the Principled's extra lobes (--scene masked_lobes; notas-dev/render.md):
// rows of cards cut by a checker (hair cards, leaves) with a clear coat and anisotropy, in front of the same cards plain:
// the masked runs' RE_EXT pipelines draw them. The materials into the project's Content/MaskedLobes
static void masked_lobes_scene(Editor* ed) {
    template_ground(ed, 40.0f);
    World* w = &ed->world;
    if (!project_file()[0]) {
        printf("[RealEngine] FAIL: masked lobes test: needs a project (--new-project folder/Name)\n");
        return;
    }
    char dir[900], path[1000];
    snprintf(dir, sizeof dir, "%s/MaskedLobes", content_root());
    re_make_dirs(dir);
    Material* mm[2] = { 0 };
    for (int i = 0; i < 2; i++) { // 0: coat + anisotropy, 1: plain (the same cut)
        snprintf(path, sizeof path, "%s/M_Cards%s.rmat", dir, i ? "Plain" : "Coat");
        FILE* f = re_fopen(path, "wb");
        if (f) {
            fprintf(f, "{\"format\":\"RealEngine Material\",\"version\":1,\"resolution\":512,\"blend_mode\":\"Masked\",\"opacity_mask_clip\":0.5,"
                       "\"two_sided\":true,\"parameters\":[],\"nodes\":["
                       "{\"idname\":\"ShaderNodeTexChecker\",\"x\":-300,\"y\":300,\"inputs\":{\"Scale\":[12,0,0,1]}},"
                       "{\"idname\":\"ShaderNodeBsdfPrincipled\",\"x\":0,\"y\":300,\"inputs\":{\"Base Color\":[0.35,0.18,0.08,1],"
                       "\"Roughness\":[0.45,0,0,1]%s}},{\"idname\":\"ShaderNodeOutputMaterial\",\"x\":300,\"y\":300}],"
                       "\"links\":[{\"from\":0,\"from_socket\":\"Fac\",\"to\":1,\"to_socket\":\"Alpha\"},"
                       "{\"from\":1,\"from_socket\":\"BSDF\",\"to\":2,\"to_socket\":\"Surface\"}]}",
                    i ? "" : ",\"Coat Weight\":[1,0,0,1],\"Coat Roughness\":[0.05,0,0,1],\"Anisotropic\":[0.8,0,0,1]");
            fclose(f);
        }
        mm[i] = mat_load(path);
        if (!mm[i] || !mat_cuts(mm[i])) printf("[RealEngine] FAIL: masked lobes test: M_Cards%d not Masked\n", i);
    }
    // (its lobes are known once baked, in the background: the renderer then says "the masked runs' RE_EXT pipelines")
    for (int k = 0; k < 80; k++) { // 2 x 40 cards, a little turned, the coat row in front
        int row = k / 40, c = k % 40;
        Entity e = mesh_actor(ed, SHAPE_CUBE, -800.0f + 40.0f * (float)c, row ? 260.0f : 60.0f, 90.0f, (float[]){ 1, 1, 1 }, 0, 0.5f);
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->scale[0] = 0.9f, t->scale[1] = 0.01f, t->scale[2] = 1.6f;
        t->rotation[1] = (float)((c * 37) % 50) - 25.0f;
        ((CStaticMesh*)ecs_get(w, e, COMP_STATIC_MESH))->material = mm[row] ? mm[row]->hash : 0u;
    }
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_POINT, (float[]){ -150, -250, 300 }, 0);
    CLight* pl = ecs_get(w, ed->selected, COMP_LIGHT);
    pl->intensity = 50000.0f, pl->cast_shadows = 1;
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = 0.0f; ed->cam.pos[1] = -500.0f; ed->cam.pos[2] = 170.0f;
    ed->cam.yaw = 90.0f; ed->cam.pitch = -6.0f;
}

// Translucent materials (--scene glass; notas-dev/render.md "TranslÃºcidos"): three big colored panes over one another in
// front of a sphere and a cube, then a field of 400 small panes (smoke, particles: many layers), the OIT pass's measure.
// The materials into the project's Content/GlassTest (a project needed, like the heavy scene)
// (2026-11-17, Q4) --scene lightfn: Light Functions (notas-dev/render.md "Light Function"): a projector's slide on the
// back wall (spot), blinds around a point light, a flickering bulb over the floor (spot), clouds passing in the sun's
// light; REALENGINE_NO_LIGHTFN the measure's "before" (the same lights without them). FAIL when an example is missing
static void light_function_scene(Editor* ed) {
    template_ground(ed, 40.0f);
    World* w = &ed->world;
    const float g[3] = { 0.7f, 0.7f, 0.7f };
    block(ed, 0, 450, 250, 12, 0.4f, 5, 0, g);    // the back wall (the slide)
    block(ed, -700, 0, 200, 0.4f, 8, 4, 0, g);    // a side wall (the blinds)
    block(ed, 200, 0, 50, 1, 1, 1, 0, g);         // a box under the bulb
    static const char* const names[4] = { "Examples/LightFunctions/LF_Projector.rmat", "Examples/LightFunctions/LF_Blinds.rmat",
                                          "Examples/LightFunctions/LF_Flicker.rmat", "Examples/LightFunctions/LF_Clouds.rmat" };
    uint32_t lf[4];
    for (int k = 0; k < 4; k++) {
        lf[k] = content_material(names[k]);
        Material* m = lf[k] ? mat_find_hash(lf[k]) : NULL;
        if (!m || m->domain != MAT_DOMAIN_LIGHT_FUNCTION) printf("[RealEngine] FAIL: lightfn: %s missing or not a Light Function\n", names[k]);
    }
    if (getenv("REALENGINE_NO_LIGHTFN")) lf[0] = lf[1] = lf[2] = lf[3] = 0;
    static const float at[3][3] = { { 0, -350, 220 }, { -450, 0, 160 }, { 200, 0, 330 } };
    static const float rot[3][2] = { { -5, 90 }, { 0, 0 }, { -90, 0 } };
    static const int kind[3] = { LIGHT_SPOT, LIGHT_POINT, LIGHT_SPOT };
    for (int k = 0; k < 3; k++) {
        spawn_actor(ed, SPAWN_LIGHT, kind[k], at[k], 0);
        CLight* l = ecs_get(w, ed->selected, COMP_LIGHT);
        CTransform* t = ecs_get(w, ed->selected, COMP_TRANSFORM);
        l->intensity = k == 0 ? 60000.0f : 8000.0f, l->attenuation_radius = 1500.0f, l->outer_cone = 30.0f, l->inner_cone = 25.0f;
        l->light_function = lf[k];
        t->rotation[0] = rot[k][0], t->rotation[1] = rot[k][1];
    }
    for (uint32_t i = 1; i < w->slot_count; i++) { // the sun: clouds passing
        Entity e = ecs_entity_at(w, i);
        CLight* l = e != ENTITY_NONE ? ecs_get(w, e, COMP_LIGHT) : NULL;
        if (l && l->type == LIGHT_DIRECTIONAL) l->light_function = lf[3], l->light_function_scale = 1500.0f, l->intensity = 3000.0f; // (dusk: the lamps show)
    }
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = 0.0f; ed->cam.pos[1] = -900.0f; ed->cam.pos[2] = 260.0f;
    ed->cam.yaw = 90.0f; ed->cam.pitch = -10.0f;
}

// (2026-11-17, Q4) --scene interiors: Interior Mapping (notas-dev/materiais.md): a street of four buildings whose faces
// are windows with rooms behind them (no geometry inside), two by day and two at night with a random share of rooms
// lit (the Night as a parameter of a Material Instance); REALENGINE_NO_INTERIOR the measure's "before" (the facades only)
static void interiors_scene(Editor* ed) {
    template_ground(ed, 60.0f);
    World* w = &ed->world;
    char dir[900], path[1000];
    snprintf(dir, sizeof dir, "%s/InteriorsTest", content_root());
    re_make_dirs(dir);
    Material* mm[4] = { 0 };
    for (int k = 0; k < 4; k++) { // (0, 1: made-up rooms by day and at night; 2, 3: the example atlas's)
        snprintf(path, sizeof path, "%s/M_Building%d.rmat", dir, k);
        FILE* f = re_fopen(path, "wb");
        if (f) {
            fprintf(f, "{\"format\":\"RealEngine Material\",\"version\":1,\"resolution\":256,\"parameters\":[],\"nodes\":["
                       "{\"idname\":\"ShaderNodeBsdfPrincipled\",\"x\":0,\"y\":300,\"inputs\":{\"Base Color\":[%s,1],\"Roughness\":[0.8,0,0,1]}},"
                       "{\"idname\":\"ShaderNodeOutputMaterial\",\"x\":600,\"y\":300},"
                       "{\"idname\":\"RealEngineInteriorMapping\",\"x\":300,\"y\":300,%s\"inputs\":{\"Rooms U\":[4,0,0,0],\"Rooms V\":[10,0,0,0],"
                       "\"Room Depth\":[1.2,0,0,0],\"Frame\":[0.12,0,0,0],\"Furniture\":[0.8,0,0,0],\"Curtains\":[0.6,0,0,0],\"Lit Rooms\":[0.55,0,0,0],\"Seed\":[%d,0,0,0]}},"
                       "{\"idname\":\"ShaderNodeValue\",\"x\":0,\"y\":600,\"value\":[%d,0,0,0],\"param\":\"Night\"}],"
                       "\"links\":[{\"from\":0,\"from_socket\":\"BSDF\",\"to\":2,\"to_socket\":\"Shader\"},{\"from\":2,\"from_socket\":\"Shader\",\"to\":1,\"to_socket\":\"Surface\"},"
                       "{\"from\":3,\"from_socket\":\"Value\",\"to\":2,\"to_socket\":\"Night\"}]}",
                    (k & 1) ? "0.55,0.5,0.45" : "0.62,0.6,0.58", k >= 2 ? "\"image\":\"/Content/Materials/Examples/Textures/T_RoomsAtlas.png\",\"props\":[3,3]," : "", 11 + k * 7, k & 1);
            fclose(f);
        }
        mm[k] = mat_load(path);
    }
    mat_bake_pending(NULL);
    if (!getenv("REALENGINE_NO_INTERIOR") && (!mm[0] || !mm[0]->interior || !mat_ext(mm[0])))
        printf("[RealEngine] FAIL: interiors: M_Building0 has no Interior Mapping\n");
    for (int k = 0; k < 4; k++) { // two by day, two at night
        Entity e = mesh_actor(ed, SHAPE_CUBE, -2250.0f + 1500.0f * (float)k, 1800.0f, 1500.0f, (float[]){ 1, 1, 1 }, 0, 0.8f);
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->scale[0] = 12.0f, t->scale[1] = 12.0f, t->scale[2] = 30.0f; // (12 x 30 m faces: 4 x 10 rooms of 3 m)
        int mi = (k & 1) * 2 + k / 2; // (by day: made-up, atlas; at night: made-up, atlas)
        ((CStaticMesh*)ecs_get(w, e, COMP_STATIC_MESH))->material = mm[mi] ? mm[mi]->hash : 0u;
        char nm[32];
        snprintf(nm, sizeof nm, "Building%d", k);
        set_name(w, e, nm);
    }
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = 0.0f; ed->cam.pos[1] = -1200.0f; ed->cam.pos[2] = 250.0f;
    ed->cam.yaw = 90.0f; ed->cam.pitch = 22.0f;
}

static void glass_scene(Editor* ed) {
    template_ground(ed, 40.0f);
    World* w = &ed->world;
    if (!project_file()[0]) {
        printf("[RealEngine] FAIL: glass test: needs a project (--new-project folder/Name)\n");
        return;
    }
    char dir[900], path[1000];
    snprintf(dir, sizeof dir, "%s/GlassTest", content_root());
    re_make_dirs(dir);
    static const float col[3][3] = { { 0.9f, 0.15f, 0.1f }, { 0.1f, 0.8f, 0.2f }, { 0.15f, 0.3f, 0.95f } };
    Material* gm[3] = { 0 };
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/M_Glass%d.rmat", dir, i);
        FILE* f = re_fopen(path, "wb");
        if (f) {
            fprintf(f, "{\"format\":\"RealEngine Material\",\"version\":1,\"resolution\":256,\"blend_mode\":\"Translucent\",\"two_sided\":true,"
                       "\"parameters\":[],\"nodes\":[{\"idname\":\"ShaderNodeBsdfPrincipled\",\"x\":0,\"y\":300,\"inputs\":{\"Base Color\":[%.2f,%.2f,%.2f,1],"
                       "\"Roughness\":[0.08,0,0,1],\"Alpha\":[0.4,0,0,1]}},{\"idname\":\"ShaderNodeOutputMaterial\",\"x\":300,\"y\":300}],"
                       "\"links\":[{\"from\":0,\"from_socket\":\"BSDF\",\"to\":1,\"to_socket\":\"Surface\"}]}",
                    col[i][0], col[i][1], col[i][2]);
            fclose(f);
        }
        gm[i] = mat_load(path);
        if (!gm[i] || !mat_translucent(gm[i])) printf("[RealEngine] FAIL: glass test: M_Glass%d not Translucent\n", i);
    }
    mesh_actor(ed, SHAPE_SPHERE, 0, 300, 100, (float[]){ 0.8f, 0.75f, 0.7f }, 0, 0.4f);
    mesh_actor(ed, SHAPE_CUBE, 260, 360, 50, (float[]){ 0.3f, 0.35f, 0.4f }, 0, 0.6f);
    for (int i = 0; i < 3; i++) { // the big panes, overlapping
        Entity e = mesh_actor(ed, SHAPE_CUBE, -120.0f + 120.0f * (float)i, 40.0f * (float)i, 110, (float[]){ 1, 1, 1 }, 0, 0.1f);
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->scale[0] = 2.2f, t->scale[1] = 0.04f, t->scale[2] = 2.0f;
        ((CStaticMesh*)ecs_get(w, e, COMP_STATIC_MESH))->material = gm[i] ? gm[i]->hash : 0u;
        char nm[32];
        snprintf(nm, sizeof nm, "Glass%d", i);
        set_name(w, e, nm);
    }
    for (int k = 0; k < 400; k++) { // the field: 20 x 20 small panes, layers behind layers
        float x = -900.0f + 90.0f * (float)(k % 20), y = 700.0f + 60.0f * (float)(k / 20);
        Entity e = mesh_actor(ed, SHAPE_CUBE, x, y, 60.0f + 25.0f * (float)((k * 7) % 5), (float[]){ 1, 1, 1 }, 0, 0.1f);
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->scale[0] = 0.8f, t->scale[1] = 0.02f, t->scale[2] = 1.2f;
        ((CStaticMesh*)ecs_get(w, e, COMP_STATIC_MESH))->material = gm[k % 3] ? gm[k % 3]->hash : 0u;
    }
    { // (2026-11-16) a clear glass ball (Transmission 1, IOR 1.5, 30 cm thick, a little dispersion): the floor's grid seen bent
        snprintf(path, sizeof path, "%s/M_GlassBall.rmat", dir);
        FILE* f = re_fopen(path, "wb");
        if (f) {
            fprintf(f, "{\"format\":\"RealEngine Material\",\"version\":1,\"resolution\":256,\"blend_mode\":\"Translucent\",\"two_sided\":false,\"refraction\":1,\"refraction_thickness\":30,\"dispersion\":0.35,"
                       "\"parameters\":[],\"nodes\":[{\"idname\":\"ShaderNodeBsdfPrincipled\",\"x\":0,\"y\":300,\"inputs\":{\"Base Color\":[0.95,0.97,1,1],"
                       "\"Roughness\":[0.02,0,0,1],\"IOR\":[1.5,0,0,1],\"Transmission Weight\":[1,0,0,1]}},{\"idname\":\"ShaderNodeOutputMaterial\",\"x\":300,\"y\":300}],"
                       "\"links\":[{\"from\":0,\"from_socket\":\"BSDF\",\"to\":1,\"to_socket\":\"Surface\"}]}");
            fclose(f);
        }
        Material* ball = mat_load(path);
        if (!ball || !mat_refract(ball)) printf("[RealEngine] FAIL: glass test: M_GlassBall not refractive\n");
        Entity e = mesh_actor(ed, SHAPE_SPHERE, -330.0f, -150.0f, 60.0f, (float[]){ 1, 1, 1 }, 0, 0.02f);
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->scale[0] = t->scale[1] = t->scale[2] = 1.2f;
        ((CStaticMesh*)ecs_get(w, e, COMP_STATIC_MESH))->material = ball ? ball->hash : 0u;
        set_name(w, e, "GlassBall");
    }
    { // (2026-11-17, Q4) Transmission: an amber pane (Refractive, absorbing, its colored shadow on the floor) and a red
      // curtain (Thin, its weight a parameter "Transmission") with the lamp behind it
        snprintf(path, sizeof path, "%s/M_AmberGlass.rmat", dir);
        FILE* f = re_fopen(path, "wb");
        if (f) {
            fprintf(f, "{\"format\":\"RealEngine Material\",\"version\":1,\"resolution\":256,\"refraction\":1,\"refraction_thickness\":2,\"dispersion\":0,"
                       "\"absorption_r\":0.95,\"absorption_g\":0.55,\"absorption_b\":0.12,\"absorption_distance\":2,"
                       "\"parameters\":[],\"nodes\":[{\"idname\":\"ShaderNodeBsdfPrincipled\",\"x\":0,\"y\":300,\"inputs\":{\"Base Color\":[1,0.85,0.6,1],"
                       "\"Roughness\":[0.05,0,0,1],\"IOR\":[1.5,0,0,1],\"Transmission Weight\":[1,0,0,1]}},{\"idname\":\"ShaderNodeOutputMaterial\",\"x\":300,\"y\":300}],"
                       "\"links\":[{\"from\":0,\"from_socket\":\"BSDF\",\"to\":1,\"to_socket\":\"Surface\"}]}");
            fclose(f);
        }
        Material* amber = mat_load(path);
        mat_bake_pending(NULL); // (its transmission is read when it compiles)
        if (!amber || !mat_glass_shadow(amber) || !mat_translucent(amber)) printf("[RealEngine] FAIL: glass test: M_AmberGlass not a refractive glass with a colored shadow\n");
        snprintf(path, sizeof path, "%s/M_Curtain.rmat", dir);
        f = re_fopen(path, "wb");
        if (f) {
            fprintf(f, "{\"format\":\"RealEngine Material\",\"version\":1,\"resolution\":256,\"two_sided\":true,\"transmission_mode\":\"Thin\","
                       "\"parameters\":[],\"nodes\":[{\"idname\":\"ShaderNodeBsdfPrincipled\",\"x\":0,\"y\":300,\"inputs\":{\"Base Color\":[0.8,0.12,0.1,1],"
                       "\"Roughness\":[0.8,0,0,1]}},{\"idname\":\"ShaderNodeOutputMaterial\",\"x\":300,\"y\":300},"
                       "{\"idname\":\"ShaderNodeValue\",\"x\":-300,\"y\":300,\"value\":[0.6,0,0,0],\"param\":\"Transmission\"}],"
                       "\"links\":[{\"from\":0,\"from_socket\":\"BSDF\",\"to\":1,\"to_socket\":\"Surface\"},"
                       "{\"from\":2,\"from_socket\":\"Value\",\"to\":0,\"to_socket\":\"Transmission Weight\"}]}");
            fclose(f);
        }
        Material* cur = mat_load(path);
        mat_bake_pending(NULL);
        if (!cur || mat_translucent(cur) || !mat_ext(cur)) printf("[RealEngine] FAIL: glass test: M_Curtain (Thin) is not an opaque material with its transmission\n");
        Entity e = mesh_actor(ed, SHAPE_CUBE, 330.0f, -150.0f, 100.0f, (float[]){ 1, 1, 1 }, 0, 0.02f);
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->scale[0] = 1.0f, t->scale[1] = 0.02f, t->scale[2] = 1.2f; // (small: the translucent test keeps its 150 fps)
        ((CStaticMesh*)ecs_get(w, e, COMP_STATIC_MESH))->material = amber ? amber->hash : 0u;
        set_name(w, e, "AmberPane");
        e = mesh_actor(ed, SHAPE_PLANE, -120.0f, -60.0f, 120.0f, (float[]){ 1, 1, 1 }, 0, 0.02f);
        t = ecs_get(w, e, COMP_TRANSFORM);
        t->rotation[0] = 90.0f, t->scale[0] = 1.0f, t->scale[1] = 1.4f;
        ((CStaticMesh*)ecs_get(w, e, COMP_STATIC_MESH))->material = cur ? cur->hash : 0u;
        set_name(w, e, "Curtain");
    }
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_POINT, (float[]){ -200, -150, 330 }, 0);
    CLight* pl = ecs_get(w, ed->selected, COMP_LIGHT);
    pl->intensity = 40000.0f, pl->cast_shadows = 1;
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = 0.0f; ed->cam.pos[1] = -650.0f; ed->cam.pos[2] = 220.0f;
    ed->cam.yaw = 90.0f; ed->cam.pitch = -8.0f;
}

static void autorig_scene(Editor* ed) {
    template_ground(ed, 20.0f);
    World* w = &ed->world;
    char disk[1100];
    snprintf(disk, sizeof disk, "%s/AutoRigTest", content_root());
    re_make_dirs(disk);
    snprintf(disk, sizeof disk, "%s/AutoRigTest/Statue.rmesh", content_root());
    g_rig_ok = autorig_test_statue(disk, "SK_Mannequin_Male", true);
    render_meshes_reload(ed->vk);
    Entity st = mesh_actor(ed, SHAPE_CUBE, 0, 0, 0, (float[]){ 0.7f, 0.7f, 0.7f }, 0, 0.6f);
    set_name(w, st, "Statue");
    ((CStaticMesh*)ecs_get(w, st, COMP_STATIC_MESH))->mesh = render_mesh_id_of("/Content/AutoRigTest/Statue.rmesh");
    g_rig_ok = g_rig_ok && autorig_humanoid(w, st, "/Content/AutoRig", content_root(), &g_rig_report);
    render_meshes_reload(ed->vk);
    anim_rescan();
    g_rig_actor = ENTITY_NONE;
    for (uint32_t i = 1; i < w->slot_count; i++) {
        Entity x = ecs_entity_at(w, i);
        if (x && ecs_get(w, x, COMP_SKELETAL_MESH) && !ecs_get(w, x, COMP_ATTACH)) g_rig_actor = x;
    }
    CSkeletalMesh* sk = g_rig_actor ? ecs_get(w, g_rig_actor, COMP_SKELETAL_MESH) : NULL;
    const AnimSeq* walk = anim_seq_name("MM_Walk");
    if (sk && walk) sk->anim = walk->hash;
    ed->selected = ENTITY_NONE;
    Entity cam = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CAMERA));
    snprintf(((CName*)ecs_get(w, cam, COMP_NAME))->text, RE_NAME_MAX, "CameraActor");
    CTransform* ct = ecs_get(w, cam, COMP_TRANSFORM);
    ct->location[0] = 330.0f, ct->location[1] = -230.0f, ct->location[2] = 150.0f, ct->rotation[0] = -6.0f, ct->rotation[1] = 145.0f;
    ct->scale[0] = ct->scale[1] = ct->scale[2] = 1.0f;
    CCamera* cc = ecs_get(w, cam, COMP_CAMERA);
    cc->fov_deg = 60.0f, cc->auto_activate = 1;
    ed->cam.pos[0] = 420.0f; ed->cam.pos[1] = -300.0f; ed->cam.pos[2] = 170.0f;
    ed->cam.yaw = 145.0f; ed->cam.pitch = -10.0f;
}

bool editor_autorig_check(Editor* ed) {
    const CTransform* t = g_rig_actor ? ecs_get(&ed->world, g_rig_actor, COMP_TRANSFORM) : NULL;
    const AnimStats* s = anim_stats();
    printf("[RealEngine] Auto Rig: %s; %u parts animated, the actor's scale %.2f\n", g_rig_report.text, s->parts, t ? t->scale[0] : 0.0f);
    bool ok = g_rig_ok && t && strstr(g_rig_report.text, "T-pose") && s->parts >= 15 && fabsf(t->scale[0] - 1.0f) < 0.1f;
    if (!ok) printf("[RealEngine] FAIL: Auto Rig: the statue as a Skeletal Mesh (T-pose found, its parts, its size)\n");
    return ok;
}

static Entity g_crowd_spawner;
static float g_crowd_start_dist;
static void crowd_scene(Editor* ed, uint32_t n) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    float g[3] = { 0.5f, 0.5f, 0.5f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, g, 0, 0.7f);
    CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 100.0f;
    ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(&ed->world, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    // the player: a Mannequin Character the player takes (it stands: the crowd comes to it)
    Entity pl = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_SKELETAL_MESH));
    snprintf(((CName*)ecs_get(&ed->world, pl, COMP_NAME))->text, RE_NAME_MAX, "Player");
    CTransform* pt = ecs_get(&ed->world, pl, COMP_TRANSFORM);
    pt->location[2] = 92.0f, pt->scale[0] = pt->scale[1] = pt->scale[2] = 1.0f, pt->flags = MOBILITY_MOVABLE;
    CCharacter* pc = ecs_get(&ed->world, pl, COMP_CHARACTER);
    pc->max_walk_speed = 450.0f, pc->jump_z_velocity = 520.0f, pc->gravity_scale = 1.0f, pc->radius = 34.0f, pc->half_height = 88.0f, pc->auto_possess = 1;
    pc->camera_distance = 2600.0f, pc->control_pitch = -55.0f; // (its view from high above: the ring shows)
    const SkelMesh* pm = anim_mesh_name("SK_Mannequin");
    const BpAsset* pabp = bp_find_name(BPA_CLASS, "ABP_Mannequin");
    CSkeletalMesh* psk = ecs_get(&ed->world, pl, COMP_SKELETAL_MESH);
    psk->mesh = pm ? pm->hash : 0, psk->anim_class = pabp ? pabp->hash : 0;
    Entity e = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CROWD));
    snprintf(((CName*)ecs_get(&ed->world, e, COMP_NAME))->text, RE_NAME_MAX, "CrowdSpawner");
    CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    t->location[0] = 2000.0f, t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    CCrowd* c = ecs_get(&ed->world, e, COMP_CROWD);
    c->count = n, c->radius = 900.0f + sqrtf((float)n) * 6.0f;
    c->sight = 1e6f; // (they all know where the player is: the test measures the closing in)
    g_crowd_spawner = e;
    g_crowd_start_dist = 2000.0f;
    Entity cam = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CAMERA));
    snprintf(((CName*)ecs_get(&ed->world, cam, COMP_NAME))->text, RE_NAME_MAX, "CameraActor");
    CTransform* ct = ecs_get(&ed->world, cam, COMP_TRANSFORM);
    ct->location[0] = -1600.0f, ct->location[1] = -1600.0f, ct->location[2] = 2600.0f;
    ct->rotation[0] = -45.0f, ct->rotation[1] = 30.0f;
    ct->scale[0] = ct->scale[1] = ct->scale[2] = 1.0f;
    CCamera* cc = ecs_get(&ed->world, cam, COMP_CAMERA);
    cc->fov_deg = 80.0f, cc->auto_activate = 1;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -50.0f, lt->rotation[1] = 35.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = -1600.0f; ed->cam.pos[1] = -1600.0f; ed->cam.pos[2] = 2600.0f;
    ed->cam.yaw = 30.0f; ed->cam.pitch = -45.0f;
}

// (Q27.1) --scene panicN: the crowd scene's N Mannequins as a city's pedestrians (Wander, sight 0), the camera at eye
// height among them; played, at 1.5 s a Crowd Alarm at the crowd's middle (a shot): the frames of the second after it
// against the median of the second before (GTA 6 fell from 30 to ~20 fps at a shot into an airport crowd), the most
// promoted in a frame, and how many panicked
static bool g_panic_test;
static double g_panic_t, g_panic_ms[4096];
static int g_panic_n, g_panic_fire = -1;
static uint32_t g_panic_peak_sim_frame;
static double g_panic_sim_before, g_panic_sim_after;
static uint32_t g_panic_max_panicked, g_panic_near;
#define PANIC_RADIUS 3500.0f // (a shot heard 35 m away)

static void panic_scene(Editor* ed, uint32_t n) {
    crowd_scene(ed, n);
    CCrowd* c = ecs_get(&ed->world, g_crowd_spawner, COMP_CROWD);
    c->behavior = CROWD_WANDER, c->sight = 0.0f, c->radius = sqrtf((float)n) * 120.0f; // (a city's density: ~1 a 1.4 m2)
    Entity pl = bp_player(&ed->world);
    CTransform* pt = pl ? ecs_get(&ed->world, pl, COMP_TRANSFORM) : NULL;
    if (pt) pt->location[0] = 2000.0f - c->radius * 0.5f; // (the player among them, halfway out)
    CCharacter* pc = pl ? ecs_get(&ed->world, pl, COMP_CHARACTER) : NULL;
    if (pc) pc->camera_distance = 400.0f, pc->control_pitch = -8.0f;
    g_panic_test = true, g_panic_t = 0.0, g_panic_n = 0, g_panic_fire = -1;
    g_panic_sim_before = g_panic_sim_after = 0.0, g_panic_max_panicked = 0;
}

static void panic_tick(Editor* ed, double dt) {
    if (!g_panic_test || g_panic_n >= 4096) return;
    g_panic_t += dt;
    g_panic_ms[g_panic_n++] = dt * 1000.0;
    const CrowdStats* s = crowd_stats();
    if (g_panic_fire < 0 && g_panic_t >= 1.0) { // (the wave crosses 35 m in 2.3 s: inside the 4 s measure)
        const CTransform* t = ecs_get(&ed->world, g_crowd_spawner, COMP_TRANSFORM);
        if (t) crowd_alarm(t->location, PANIC_RADIUS), g_panic_near = crowd_count_near(t->location, PANIC_RADIUS);
        g_panic_fire = g_panic_n;
    }
    if (g_panic_fire < 0 && g_panic_t > 0.4) g_panic_sim_before = fmax(g_panic_sim_before, s->sim_ms);
    if (g_panic_fire >= 0 && g_panic_t < 2.0) g_panic_sim_after = fmax(g_panic_sim_after, s->sim_ms);
    if (s->panicked > g_panic_max_panicked) g_panic_max_panicked = s->panicked;
    g_panic_peak_sim_frame = s->promote_peak;
}

static int cmp_dbl(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

bool editor_panic_check(Editor* ed) {
    (void)ed;
    const CrowdStats* s = crowd_stats();
    // the second before the alarm (from 0.5 s) and the second after it
    // (a sustained drop is what GTA 6 showed; a lone hitch of the OS or a shared GPU shows without any alarm too: the
    // mean of the second after and its 95th percentile, not its worst frame)
    double before[2048], after[2048], after_sum = 0.0, t = 0.0;
    int nb = 0, na = 0;
    for (int i = 0; i < g_panic_n; i++) {
        t += g_panic_ms[i] / 1000.0;
        if (g_panic_fire >= 0 && i < g_panic_fire && t > 0.4 && nb < 2048) before[nb++] = g_panic_ms[i];
        if (g_panic_fire >= 0 && i >= g_panic_fire && t < 1.0 + 1.0 + 0.05 && na < 2048) after[na++] = g_panic_ms[i], after_sum += g_panic_ms[i];
    }
    qsort(before, (size_t)nb, sizeof before[0], cmp_dbl);
    qsort(after, (size_t)na, sizeof after[0], cmp_dbl);
    double med = nb ? before[nb / 2] : 0.0, after_mean = na ? after_sum / na : 0.0, after_p95 = na ? after[na * 95 / 100] : 0.0;
    double after_max = na ? after[na - 1] : 0.0;
    printf("[RealEngine] Crowd panic: %u agents (%u within the alarm's 35 m), %u panicked at most (%u now), %u promoted at most in a frame; frame median %.2f ms before, "
           "the second after: mean %.2f ms (%.2fx), 95th percentile %.2f ms (%.2fx), worst %.2f ms; simulation worst %.3f ms before, %.3f ms after\n",
           s->agents, g_panic_near, g_panic_max_panicked, s->panicked, g_panic_peak_sim_frame, med, after_mean, med > 0.0 ? after_mean / med : 0.0, after_p95, med > 0.0 ? after_p95 / med : 0.0, after_max,
           g_panic_sim_before, g_panic_sim_after);
    bool ok = g_panic_fire >= 0 && nb > 10;
    if (!ok) printf("[RealEngine] FAIL: Crowd panic: the alarm didn't fire inside the measure\n");
    if (ok && (g_panic_near < 100u || g_panic_max_panicked < g_panic_near * 9u / 10u)) {
        printf("[RealEngine] FAIL: Crowd panic: %u of the %u within its radius panicked (the wave should reach 90%%)\n", g_panic_max_panicked, g_panic_near);
        ok = false;
    }
    if (ok && g_panic_peak_sim_frame > 64u) {
        printf("[RealEngine] FAIL: Crowd panic: %u promoted in a frame > 64\n", g_panic_peak_sim_frame);
        ok = false;
    }
    if (ok && (after_mean > med * 1.25 || after_p95 > med * 1.5)) {
        printf("[RealEngine] FAIL: Crowd panic: the second after the alarm (mean %.2f, 95th percentile %.2f ms) dropped from the median %.2f ms\n",
               after_mean, after_p95, med);
        ok = false;
    }
    return ok;
}

// (Q27.10) --scene city_routine: 200 agents with City Routine around a shop (a Smart Object of 12 slots, open 9:00 to
// 11:00) and the clock at 8:30 running 30 game minutes a real second. Closed (before 9) no one goes; open, they come;
// at 11 they leave; at 6 s a crime by the shop: the nearest calls the police. The check: all of that, the routine's cost
static Entity g_city_shop;
static double g_city_t;
static uint32_t g_city_closed, g_city_open_max, g_city_after, g_city_calls;
static double g_city_sim_max;
static void city_routine_scene(Editor* ed) {
    crowd_scene(ed, 200);
    World* w = &ed->world;
    CCrowd* c = ecs_get(w, g_crowd_spawner, COMP_CROWD);
    c->behavior = CROWD_WANDER, c->sight = 0.0f, c->radius = 700.0f, c->routine = 1;
    const CTransform* st = ecs_get(w, g_crowd_spawner, COMP_TRANSFORM);
    float col[3] = { 0.8f, 0.6f, 0.3f };
    g_city_shop = mesh_actor(ed, SHAPE_CUBE, st->location[0] + 300.0f, st->location[1], 100.0f, col, 0, 0.6f);
    ecs_add(w, g_city_shop, COMP_INTERACTABLE);
    CInteractable* ia = ecs_get(w, g_city_shop, COMP_INTERACTABLE);
    ia->so_slots = 12, ia->open_q = 9 * 4, ia->close_q = 11 * 4;
    snprintf(((CName*)ecs_get(w, g_city_shop, COMP_NAME))->text, RE_NAME_MAX, "Shop");
    city_set_time(8.5f, 1800.0f);
    g_city_t = 0.0, g_city_closed = g_city_open_max = g_city_after = g_city_calls = 0, g_city_sim_max = 0.0;
}
void editor_city_routine_tick(Editor* ed, double dt) {
    if (!ed->playing) return;
    double t = g_city_t += dt;
    if (t < 0.2) city_set_time(8.5f + (float)t * 0.5f, 1800.0f); // (the Play's start: the clock from 8:30)
    uint32_t v = city_visitors(g_city_shop);
    float h = city_hour();
    if (h > 8.6f && h < 8.95f && v > g_city_closed) g_city_closed = v;
    if (h > 9.2f && h < 10.95f && v > g_city_open_max) g_city_open_max = v;
    if (h > 11.2f && h < 11.6f && v > g_city_after) g_city_after = v;
    if (t >= 6.5 && t - dt < 6.5) { // (11:45: the crime by the shop)
        const CTransform* st = ecs_get(&ed->world, g_crowd_spawner, COMP_TRANSFORM);
        if (st) city_report_crime(st->location, 2500.0f);
    }
    g_city_calls = city_police_calls();
    const CrowdStats* s = crowd_stats();
    if (t > 1.0 && s->sim_ms > g_city_sim_max) g_city_sim_max = s->sim_ms;
}
bool editor_city_routine_check(Editor* ed) {
    (void)ed;
    const CityStats* cs = city_stats();
    printf("[RealEngine] City routine: shop closed %u visitors, open up to %u (12 slots), after closing %u; police calls %u, %u events; "
           "crowd %.4f ms an agent at worst (200)\n", g_city_closed, g_city_open_max, g_city_after, g_city_calls, cs->events, g_city_sim_max / 200.0);
    bool ok = g_city_closed == 0 && g_city_open_max >= 6 && g_city_open_max <= 12 && g_city_after == 0;
    if (!ok) printf("[RealEngine] FAIL: City routine: the shop's visitors should follow its hours\n");
    if (ok && g_city_calls < 1) printf("[RealEngine] FAIL: City routine: the witness should call the police\n"), ok = false;
    return ok;
}

// (Q27.9) --scene npc_variety: 200 agents wandering near the player (the crowd scene's), each its own look (skin, top,
// bottom, build) by the seed. The check: nearly all distinct, the crowd's costs as before. REALENGINE_CROWD_LOOKS=0: all
// alike (the comparison)
static void npc_variety_scene(Editor* ed) {
    crowd_scene(ed, 200);
    CCrowd* c = ecs_get(&ed->world, g_crowd_spawner, COMP_CROWD);
    c->behavior = CROWD_WANDER, c->sight = 0.0f, c->radius = 900.0f;
    const char* lk = getenv("REALENGINE_CROWD_LOOKS");
    c->looks = lk ? (float)atof(lk) : 1.0f;
    Entity pl = bp_player(&ed->world);
    CTransform* pt = pl ? ecs_get(&ed->world, pl, COMP_TRANSFORM) : NULL;
    const CTransform* st = ecs_get(&ed->world, g_crowd_spawner, COMP_TRANSFORM);
    if (pt && st) pt->location[0] = st->location[0] - 1300.0f, pt->location[1] = st->location[1];
    CCharacter* pc = pl ? ecs_get(&ed->world, pl, COMP_CHARACTER) : NULL;
    if (pc) pc->camera_distance = 350.0f, pc->control_pitch = -6.0f;
}
bool editor_npc_variety_check(Editor* ed) {
    (void)ed;
    const CrowdStats* s = crowd_stats();
    printf("[RealEngine] NPC variety: %u agents, %u distinct looks (4 bytes each); crowd CPU %.3f ms (pack %.3f ms)\n", s->agents, s->looks, s->sim_ms, s->pack_ms);
    float lk = getenv("REALENGINE_CROWD_LOOKS") ? (float)atof(getenv("REALENGINE_CROWD_LOOKS")) : 1.0f;
    bool ok = s->agents == 200 && (lk < 1.0f || s->looks >= 190);
    if (!ok) printf("[RealEngine] FAIL: NPC variety: 200 agents should look nearly all different\n");
    return ok;
}

// (Q27.8) --scene city_night: a street at night (a dim moon, a lamp post), a road sign 12 m ahead with the reflective film
// (M_RoadSign), a red reflector and a car-paint sphere with flakes (M_CarPaintFlakes) beside the road, and a headlight
// (a spot) at the camera. Phase A (1-3 s) the sign with its film: it shines back at the headlight beside the eye; phase B
// (4.5-6.5 s) the same sign with Retroreflection 0 (in memory, not saved): only its paint. The check: A much brighter
static Entity g_cn_light, g_cn_sign;
static double g_cn_t;
static double g_cn_lum[2], g_cn_mean[2]; // (the brightest point: the sign; the frame's mean)
static void city_night_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    float asph[3] = { 0.06f, 0.06f, 0.065f }, white[3] = { 1, 1, 1 };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, asph, 0, 0.35f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 60.0f, ft->flags = MOBILITY_STATIC;
    g_cn_sign = mesh_actor(ed, SHAPE_CUBE, 1200.0f, 0.0f, 220.0f, white, 0, 0.5f);
    CTransform* st = ecs_get(w, g_cn_sign, COMP_TRANSFORM);
    st->scale[0] = 0.05f, st->scale[1] = 2.4f, st->scale[2] = 1.6f, st->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(w, g_cn_sign, COMP_STATIC_MESH))->material = content_material("Examples/M_RoadSign.rmat");
    Entity rf = mesh_actor(ed, SHAPE_CYLINDER, 900.0f, -260.0f, 60.0f, white, 0, 0.3f);
    CTransform* rt = ecs_get(w, rf, COMP_TRANSFORM);
    rt->scale[0] = rt->scale[1] = 0.12f, rt->scale[2] = 0.08f, rt->rotation[0] = 90.0f, rt->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(w, rf, COMP_STATIC_MESH))->material = content_material("Examples/M_Reflector.rmat");
    Entity car = mesh_actor(ed, SHAPE_SPHERE, 700.0f, 300.0f, 90.0f, white, 0, 0.3f);
    CTransform* ct = ecs_get(w, car, COMP_TRANSFORM);
    ct->scale[0] = 3.0f, ct->scale[1] = 1.6f, ct->scale[2] = 1.0f, ct->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(w, car, COMP_STATIC_MESH))->material = content_material("Examples/M_CarPaintFlakes.rmat");
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0); // (a moon)
    CLight* moon = ecs_get(w, ed->selected, COMP_LIGHT);
    moon->intensity = 0.3f, moon->color[0] = 0.7f, moon->color[1] = 0.8f, moon->color[2] = 1.0f;
    CTransform* mt = ecs_get(w, ed->selected, COMP_TRANSFORM);
    mt->rotation[0] = -35.0f, mt->rotation[1] = 120.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_POINT, (float[]){ 400.0f, 500.0f, 600.0f }, 0); // (a street lamp)
    CLight* lamp = ecs_get(w, ed->selected, COMP_LIGHT);
    lamp->intensity = 800.0f, lamp->color[0] = 1.0f, lamp->color[1] = 0.75f, lamp->color[2] = 0.45f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SPOT, (float[]){ 0.0f, -40.0f, 160.0f }, 0); // (the headlight)
    g_cn_light = ed->selected;
    CLight* hl = ecs_get(w, g_cn_light, COMP_LIGHT);
    hl->intensity = 20000.0f, hl->outer_cone = 7.0f, hl->inner_cone = 5.0f, hl->attenuation_radius = 6000.0f; // (a narrow beam on the sign: the floor by the camera stays dark)
    CTransform* ht = ecs_get(w, g_cn_light, COMP_TRANSFORM);
    ht->rotation[0] = 2.9f, ht->rotation[1] = 0.0f; // (up at the sign 12 m ahead)
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = 0.0f, ed->cam.pos[2] = 170.0f, ed->cam.yaw = 0.0f, ed->cam.pitch = 1.5f;
    g_cn_t = 0.0, g_cn_lum[0] = g_cn_lum[1] = 0.0;
}
void editor_city_night_tick(Editor* ed, double dt) {
    dt = dt > 0.05 ? 0.05 : dt; // (the first frames' long ones: each phase gets its frames)
    double t = g_cn_t += dt, mx, mean;
    CTransform* ht = ecs_alive(&ed->world, g_cn_light) ? ecs_get(&ed->world, g_cn_light, COMP_TRANSFORM) : NULL;
    if (!ht) return;
    (void)ht;
    char path[900];
    snprintf(path, sizeof path, "%s/Materials/Examples/M_RoadSign.rmat", content_root());
    if (t > 3.5 && t - dt <= 3.5) { // (phase B: the film off, in memory)
        Material* m = mat_load(path);
        if (m) m->retro = 0.0f, mat_mark_dirty(m);
    }
    if ((t > 1.0 && t - dt <= 1.0) || (t > 4.5 && t - dt <= 4.5)) render_flicker_reset(); // (the probe's sums from here)
    if (t > 3.0 && t - dt <= 3.0) render_probe_lum(&mx, &mean), g_cn_lum[0] = mx, g_cn_mean[0] = mean;
    if (t > 6.5 && t - dt <= 6.5) {
        render_probe_lum(&mx, &mean), g_cn_lum[1] = mx, g_cn_mean[1] = mean;
        Material* m = mat_load(path); // (the film back, as the asset has it)
        if (m) m->retro = 8.0f, mat_mark_dirty(m);
    }
}
bool editor_city_night_check(Editor* ed) {
    (void)ed;
    double r = g_cn_mean[1] > 0.0 ? g_cn_mean[0] / g_cn_mean[1] : 0.0;
    printf("[RealEngine] City night: the headlight by the eye: the sign with its film %.3g nits (mean; brightest %.3g), without %.3g (brightest %.3g): %.1fx\n",
           g_cn_mean[0], g_cn_lum[0], g_cn_mean[1], g_cn_lum[1], r);
    bool ok = r > 1.5;
    if (!ok) printf("[RealEngine] FAIL: City night: the retroreflective sign should shine back at the headlight beside the eye\n");
    return ok;
}

// (Q27.6) --scene weather_regional: a Rain actor with Regional Weather (no storms of its own) and one storm put 1.5 km
// east (500 m, 60 mm/h). The camera here 2 s (sunny), at the storm 3 s (raining: the window's puddles start from what the
// place gathered while away), back here; the storm cleared and 10 minutes skipped: the place still has its water
static double g_wx_t;
static void weather_regional_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    float g[3] = { 0.45f, 0.45f, 0.45f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, g, 0, 0.6f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 100.0f, ft->flags = MOBILITY_STATIC;
    Entity fb = mesh_actor(ed, SHAPE_PLANE, 150000.0f, 0, 0, g, 0, 0.6f);
    ft = ecs_get(w, fb, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 100.0f, ft->flags = MOBILITY_STATIC;
    Entity r = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_RAIN));
    snprintf(((CName*)ecs_get(w, r, COMP_NAME))->text, RE_NAME_MAX, "Rain");
    CRain* rn = ecs_get(w, r, COMP_RAIN);
    rn->regional = 1, rn->storminess = 0.0f, rn->wind_speed = 0.0f;
    wfield_configure(true, 0.0f, rn->weather_seed, 0.0f, 0.0f);
    wfield_clear();
    wfield_add_storm(150000.0f, 0.0f, 50000.0f, 60.0f, 3600.0f);
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(w, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -40.0f, lt->rotation[1] = 30.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -600.0f, ed->cam.pos[2] = 250.0f, ed->cam.yaw = 90.0f, ed->cam.pitch = -15.0f;
    g_wx_t = 0.0;
    water_persist_probe(150000.0f, 0.0f);
}
static float g_wx_here, g_wx_there, g_wx_before[2], g_wx_away[2];
static bool g_wx_skipped;
void editor_weather_regional_tick(Editor* ed, double dt) {
    double t = g_wx_t += dt;
    float v[2];
    if (t < 2.0) {
        ed->cam.pos[0] = 0.0f;
        if (t > 1.5) g_wx_here = water_rain_intensity();
    } else if (t < 5.0) {
        ed->cam.pos[0] = 150000.0f;
        if (t > 4.5) g_wx_there = water_rain_intensity();
        if (t > 4.8 && water_persist_read(v)) memcpy(g_wx_before, v, sizeof v);
    } else {
        ed->cam.pos[0] = 0.0f;
        if (t > 5.5 && !g_wx_skipped) wfield_clear(), water_persist_skip(600.0f), g_wx_skipped = true; // (the storm over, 10 minutes on)
        if (t > 6.5 && water_persist_read(v)) memcpy(g_wx_away, v, sizeof v);
    }
}
bool editor_weather_regional_check(Editor* ed) {
    (void)ed;
    printf("[RealEngine] Weather regional: here %.2f mm/h, at the storm %.1f mm/h; its ground: %.2f cm of water (wet %.2f), 10 min later %.2f cm (wet %.2f)\n",
           g_wx_here, g_wx_there, g_wx_before[0], g_wx_before[1], g_wx_away[0], g_wx_away[1]);
    bool ok = g_wx_here < 0.1f && g_wx_there > 30.0f;
    if (!ok) printf("[RealEngine] FAIL: Weather regional: it should be dry here and raining at the storm\n");
    if (ok && (g_wx_before[0] < 0.05f || g_wx_away[0] < 0.05f || g_wx_away[1] < 0.5f))
        printf("[RealEngine] FAIL: Weather regional: the storm's ground should keep its water 10 minutes later\n"), ok = false;
    return ok;
}

// (Q27.4) --scene vehicle_crash: two rows of 25 cars 30 m apart facing each other, 60 cm off center (their corners meet),
// all at full throttle: they crash at ~40 km/h. The check: no car through another, the crashes dented them, crushed
// fronts lost power, bent front wheels pull (their toe), the vehicles' CPU
static Entity g_crash[50];

static void vehicle_crash_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    float g[3] = { 0.5f, 0.5f, 0.5f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 5000, 0, g, 0, 0.7f);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = 200.0f, ft->scale[1] = 200.0f, ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(w, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    VehicleAsset* a = veh_temp("CrashCar", VT_CAR);
    for (int i = 0; a && i < 50; i++) {
        int row = i / 25, k = i % 25;
        float at[3] = { row ? 1500.0f : -1500.0f, (float)k * 400.0f + (row ? 60.0f : 0.0f), veh_rest_height(a) };
        char nm[32];
        snprintf(nm, sizeof nm, "Crash%d", i);
        g_crash[i] = vehicle_spawn(w, a->hash, at, row ? 180.0f : 0.0f, nm);
    }
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(w, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -40.0f, lt->rotation[1] = 30.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = -2500.0f, ed->cam.pos[1] = -1500.0f, ed->cam.pos[2] = 1800.0f;
    ed->cam.yaw = 50.0f, ed->cam.pitch = -30.0f;
}

void editor_vehicle_crash_tick(Editor* ed, double dt) {
    (void)dt;
    for (int i = 0; i < 50; i++)
        if (g_crash[i]) vehicle_set_control(&ed->world, g_crash[i], VI_THROTTLE, 1.0f);
}

bool editor_vehicle_crash_check(Editor* ed) {
    World* w = &ed->world;
    uint32_t dented = 0, weak = 0, pulls = 0, hits = 0;
    float dmg = 0.0f, closest = 1e9f;
    VehInfo vi[50];
    bool have[50] = { 0 };
    for (int i = 0; i < 50; i++) {
        if (!g_crash[i] || !vehicle_info(w, g_crash[i], &vi[i])) continue;
        have[i] = true;
        hits += vi[i].hits, dmg += vi[i].damage;
        dented += vi[i].damage > 0.001f, weak += vi[i].engine_damage > 0.05f;
        pulls += fabsf(vi[i].toe_deg[0]) > 0.2f || fabsf(vi[i].toe_deg[1]) > 0.2f;
    }
    for (int i = 0; i < 50; i++)
        for (int j = i + 1; j < 50; j++) {
            if (!have[i] || !have[j]) continue;
            float dx = vi[i].pos[0] - vi[j].pos[0], dy = vi[i].pos[1] - vi[j].pos[1];
            closest = fminf(closest, sqrtf(dx * dx + dy * dy));
        }
    const VehStats* vs = vehicle_stats();
    printf("[RealEngine] Vehicle crash: 50 cars, %u hits, %u dented (mean damage %.2f), %u lost power, %u pull (bent toe), closest centers %.0f cm; "
           "vehicles %.3f ms\n", hits, dented, dmg / 50.0f, weak, pulls, closest, vs->ms);
    bool ok = dented >= 40 && weak >= 30 && pulls >= 10;
    if (!ok) printf("[RealEngine] FAIL: Vehicle crash: the crashes should dent most cars, crush their fronts and bend some wheels\n");
    if (ok && closest < 150.0f) printf("[RealEngine] FAIL: Vehicle crash: two cars went into each other (%.0f cm)\n", closest), ok = false;
    if (ok && vs->ms > 1.0) printf("[RealEngine] FAIL: Vehicle crash: %.3f ms > 1 ms\n", vs->ms), ok = false;
    return ok;
}

// (Q27.3) --scene hair_crowd: 100 heads with dynamic hair (6000 strands, 12 segments each) in a 10 x 10 grid 1.2 m apart,
// the camera at the first row: the budget shares the strands by the size on the screen, the nearest 16 simulate
static void hair_crowd_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    float c[3] = { 0.6f, 0.6f, 0.6f }, skin[3] = { 0.7f, 0.5f, 0.42f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 540, 540, 0, c, 0, 0.6f);
    CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 20.0f;
    for (int i = 0; i < 100; i++) {
        Entity e = mesh_actor(ed, SHAPE_SPHERE, (float)(i % 10) * 120.0f, (float)(i / 10) * 120.0f, 160.0f, skin, 0, 0.5f);
        CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        t->scale[0] = 0.22f, t->scale[1] = 0.22f, t->scale[2] = 0.26f;
        t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
        ecs_add(&ed->world, e, COMP_GROOM);
        t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        t->rotation[1] = 90.0f; // (their faces toward the camera, at -Y)
        CGroom* g = ecs_get(&ed->world, e, COMP_GROOM);
        g->strands = 6000, g->segments = 12, g->dynamic = 1, g->lod = 1, g->seed = (uint32_t)i + 1u;
        g->wind[0] = 200.0f, g->wind[1] = 60.0f, g->gusts = 0.4f;
    }
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -40.0f, lt->rotation[1] = 60.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = 540.0f, ed->cam.pos[1] = -260.0f, ed->cam.pos[2] = 190.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -5.0f;
}

bool editor_hair_crowd_check(Editor* ed) {
    (void)ed;
    const GroomStats* s = groom_stats();
    printf("[RealEngine] Hair crowd: %u grooms, %u of %u strands drawn, %u segments (budget %u, factor %.2f), %u simulated\n", s->grooms, s->drawn,
           s->strands, s->segments, 1500000u, s->budget_k, s->simulated);
    bool ok = s->grooms == 100;
    if (!ok) printf("[RealEngine] FAIL: Hair crowd: the 100 grooms should be drawn\n");
    if (ok && s->segments > 1500000u + 100000u) printf("[RealEngine] FAIL: Hair crowd: %u segments over the budget\n", s->segments), ok = false;
    if (ok && (s->simulated == 0 || s->simulated > 16)) printf("[RealEngine] FAIL: Hair crowd: %u simulated (1-16)\n", s->simulated), ok = false;
    return ok;
}

// (Q27.2) --scene traffic: a city's grid of streets (8 x 8 junctions 100 m apart: 112 two-way roads, signals of 12 s at
// every junction of three roads or more), ~1700 cars (7.5 per 100 m of lane), the camera high over it. Played: queues at
// the red lights without overlapping, nobody runs a red with room to stop, the cars cross junctions, the cost
static void traffic_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    const int N = 8;
    const float S = 10000.0f;
    float g[3] = { 0.35f, 0.35f, 0.36f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, (N - 1) * S * 0.5f, (N - 1) * S * 0.5f, 0, g, 0, 0.8f);
    CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = (float)N * S / 100.0f;
    ft->flags = MOBILITY_STATIC;
    for (int axis = 0; axis < 2; axis++)
        for (int a = 0; a < N; a++)
            for (int b = 0; b + 1 < N; b++) {
                Entity e = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SPLINE));
                snprintf(((CName*)ecs_get(&ed->world, e, COMP_NAME))->text, RE_NAME_MAX, "Road_%c%d_%d", axis ? 'V' : 'H', a, b);
                CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
                t->location[0] = axis ? (float)a * S : (float)b * S, t->location[1] = axis ? (float)b * S : (float)a * S;
                t->rotation[1] = axis ? 90.0f : 0.0f; // (the yaw: pitch, yaw, roll as Unreal's rotator)
                t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_STATIC;
                CRail* r = ecs_get(&ed->world, e, COMP_SPLINE);
                r->kind = SPLINE_KIND_SPLINE, r->npoints = 2;
                r->points[1][0] = S;
                for (int k = 0; k < 2; k++) r->pscale[k][0] = r->pscale[k][1] = 1.0f;
                r->road_lanes = 1, r->road_speed = 50.0f, r->road_density = 7.5f, r->road_lane_width = 350.0f, r->road_signal = 12.0f;
            }
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -50.0f, lt->rotation[1] = 35.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = -3000.0f, ed->cam.pos[1] = -3000.0f, ed->cam.pos[2] = 9000.0f;
    ed->cam.yaw = 45.0f, ed->cam.pitch = -35.0f;
}

bool editor_traffic_check(Editor* ed) {
    (void)ed;
    const TrafficStats* s = traffic_stats();
    printf("[RealEngine] Traffic: %u cars on %u roads (%u lanes, %u junctions, %u with signals); %u junctions crossed, %u stopped now, "
           "mean %.1f km/h, smallest gap %.0f cm, %u red lights run; %.3f ms\n",
           s->cars, s->roads, s->lanes, s->junctions, s->signals, s->passed, s->stopped, s->mean_speed, s->min_gap, s->red_runs, s->ms);
    bool ok = s->cars >= 1500 && s->signals > 0;
    if (!ok) printf("[RealEngine] FAIL: Traffic: the grid should hold ~2000 cars and its signals\n");
    if (ok && s->min_gap < -1.0f) printf("[RealEngine] FAIL: Traffic: two cars overlap (%.0f cm)\n", s->min_gap), ok = false;
    if (ok && s->red_runs) printf("[RealEngine] FAIL: Traffic: %u red lights run with room to stop\n", s->red_runs), ok = false;
    if (ok && (s->passed < 150 || s->stopped < 20)) printf("[RealEngine] FAIL: Traffic: it should flow (%u crossed) and queue at the reds (%u stopped)\n", s->passed, s->stopped), ok = false;
    if (ok && s->ms > 1.0) printf("[RealEngine] FAIL: Traffic: %.3f ms > 1 ms\n", s->ms), ok = false;
    return ok;
}

bool editor_crowd_check(Editor* ed) {
    const CrowdStats* s = crowd_stats();
    Entity pl = bp_player(&ed->world);
    const CTransform* pt = pl ? ecs_get(&ed->world, pl, COMP_TRANSFORM) : NULL;
    float d = pt ? crowd_mean_distance(&ed->world, g_crowd_spawner, pt->location) : -1.0f;
    printf("[RealEngine] Crowd: %u agents (%u charging, %u on the ring, %u know), simulation %.3f ms, flow field %.2f ms (%u builds); "
           "mean distance to the player %.0f cm (started %.0f); think %.3f, hash %.3f, step %.3f, pack %.3f ms\n",
           s->agents, s->chasing, s->surrounding, s->knowing, s->sim_ms, s->flow_ms, s->flow_builds, d, g_crowd_start_dist, s->think_ms,
           s->hash_ms, s->step_ms, s->pack_ms);
    // (closer by a quarter: up to 5000; a bigger crowd's ring is tens of layers deep, its mean stays far: only that it came)
    // (2000 agents: ~1490-1516 cm of 2000 in 4 s, before and after the online code (2026-11-04): most of them hold the
    // ring around the player, whose radius sets the mean; 0.75 sat on that edge)
    float want = s->agents > 5000 ? g_crowd_start_dist * 0.9f : g_crowd_start_dist * 0.8f;
    bool ok = pt && s->agents > 0 && d >= 0.0f && d < want && s->flow_builds > 0;
    if (!ok) printf("[RealEngine] FAIL: Crowd: the agents should close in on the player along the flow field\n");
    return ok;
}

// The particles' test scene (--scene particles): the templates' systems in Content/ParticleTest (made when missing)
// around the origin on the grid floor, and a sphere with the Tlender's particles; "particles5m": the galaxy alone
static void particles_scene(Editor* ed, const char* name) {
    template_ground(ed, 60.0f);
    bool galaxy_only = !strcmp(name, "particles5m");
    int only = name[9] == ':' ? atoi(name + 10) : -1; // ("particles:N": one of them, 9 the Tlender's)
    static const int tmpl[] = { PST_GALAXY, PST_FIRE, PST_SMOKE, PST_SPARKS, PST_FOUNTAIN, PST_MAGIC, PST_DEBRIS, PST_SNOW, PST_RIBBON };
    static const float at[][3] = { { 0, 0, 1500 }, { -600, 300, 0 }, { -300, 300, 0 }, { 0, 300, 0 }, { 300, 300, 0 }, { 600, 300, 0 }, { 0, -300, 0 }, { 0, 0, 1200 }, { 600, -300, 60 } };
    char dir[700];
    snprintf(dir, sizeof dir, "%s/ParticleTest", content_root());
    re_make_dirs(dir);
    for (size_t i = 0; i < sizeof tmpl / sizeof tmpl[0]; i++) {
        if ((galaxy_only && i > 0) || (only >= 0 && (int)i != only)) continue;
        char nm[64], vp[200];
        snprintf(nm, sizeof nm, "PS_%s", g_ps_template_names[tmpl[i]]);
        for (char* c = nm; *c; c++)
            if (*c == ' ' || *c == '(' || *c == ')') *c = '_';
        snprintf(vp, sizeof vp, "/Content/ParticleTest/%s.rparticle", nm);
        ParticleSystemAsset* a = ps_load(vp);
        uint32_t h = a ? a->hash : ps_create("/Content/ParticleTest", nm, tmpl[i]);
        Entity e = particles_spawn(&ed->world, h, at[i], NULL, false);
        if (e != ENTITY_NONE) set_name(&ed->world, e, nm);
    }
    if (!galaxy_only && (only < 0 || only == 9)) { // the Tlender's: a sphere emitting from its faces (Blender's defaults, a glow)
        Entity s = mesh_actor(ed, SHAPE_SPHERE, -600, -300, 120, (float[]){ 0.3f, 0.5f, 0.8f }, 0, 0.5f);
        set_name(&ed->world, s, "TlenderEmitter");
        ecs_add(&ed->world, s, COMP_PARTICLE_TL);
        CParticleTl* tl = ecs_get(&ed->world, s, COMP_PARTICLE_TL);
        if (tl) tl->count = 20000, tl->emissive = 4000, tl->size = 0.02f, tl->normal_factor = 2, tl->color[0] = 1, tl->color[1] = 0.6f, tl->color[2] = 0.2f;
    }
    ed->selected = ENTITY_NONE;
}

// --add-floor: the templates' grid floor added to the open level, in memory only (to test a level without its floor)
void editor_add_test_floor(Editor* ed, float size) {
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, (float[]){ 0.5f, 0.5f, 0.5f }, 0, 0.75f);
    CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = size;
    ft->flags = MOBILITY_STATIC;
    set_name(&ed->world, fl, "TestFloor");
    ((CStaticMesh*)ecs_get(&ed->world, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
}


// (the shadow_ghost scene) the cube slides along x between -300 and 300 cm at 4 m/s while playing
static Entity g_ghost_cube;
static void ghost_step(Editor* ed, double dt) {
    (void)dt;
    CTransform* t = ecs_alive(&ed->world, g_ghost_cube) ? ecs_get(&ed->world, g_ghost_cube, COMP_TRANSFORM) : NULL;
    if (!t) return;
    t->location[0] = 300.0f * sinf((float)ed->play_time * 1.333f);
    ecs_touch(&ed->world, g_ghost_cube);
}

void editor_test_scene(Editor* ed, const char* name) {
    if (!strncmp(name, "crowd", 5)) {
        int n = atoi(name + 5);
        crowd_scene(ed, n > 0 ? (uint32_t)n : 1000u);
        return;
    }
    if (!strcmp(name, "gi_fallback")) { // (Q27.12)
        gi_fallback_scene(ed);
        return;
    }
    if (!strcmp(name, "loco_styles")) { // (Q27.11)
        mm_styles_scene(ed);
        return;
    }
    if (!strcmp(name, "warp")) { // (2026-11-23) root motion, Motion Warping, paired montages
        warp_scene(ed);
        return;
    }
    if (!strcmp(name, "traversal")) { // (2026-11-23) Try Traversal
        traversal_scene(ed);
        return;
    }
    if (!strcmp(name, "anim_warping")) { // (2026-11-23) Orientation and Stride Warping
        anim_warping_scene(ed);
        return;
    }
    if (!strcmp(name, "combo")) { // (2026-11-23) montage windows: Combo, Hit
        combo_scene(ed);
        return;
    }
    if (!strcmp(name, "cover")) { // (2026-11-23) third-person cover
        cover_scene(ed);
        return;
    }
    if (!strcmp(name, "city_routine")) { // (Q27.10)
        city_routine_scene(ed);
        return;
    }
    if (!strcmp(name, "npc_variety")) { // (Q27.9)
        npc_variety_scene(ed);
        return;
    }
    if (!strcmp(name, "city_night")) { // (Q27.8)
        city_night_scene(ed);
        return;
    }
    if (!strcmp(name, "weather_regional")) { // (Q27.6)
        weather_regional_scene(ed);
        return;
    }
    if (!strcmp(name, "vehicle_crash")) { // (Q27.4)
        vehicle_crash_scene(ed);
        return;
    }
    if (!strcmp(name, "hair_crowd")) { // (Q27.3)
        hair_crowd_scene(ed);
        return;
    }
    if (!strcmp(name, "traffic")) { // (Q27.2)
        traffic_scene(ed);
        return;
    }
    if (!strncmp(name, "panic", 5)) { // (Q27.1)
        int n = atoi(name + 5);
        panic_scene(ed, n > 0 ? (uint32_t)n : 10000u);
        return;
    }
    if (!strncmp(name, "mm_", 3)) {
        mm_startstop_scene(ed, !strcmp(name, "mm_examples") ? MMSS_EXAMPLES : !strcmp(name, "mm_longtake") ? MMSS_LONGTAKE
                               : !strcmp(name, "mm_authored")                ? MMSS_AUTHORED
                               : !strcmp(name, "mm_jump")                    ? MMSS_JUMP : MMSS_STARTSTOP);
        return;
    }
    if (!strcmp(name, "footik")) {
        footik_scene(ed);
        return;
    }
    if (!strcmp(name, "ragdoll")) {
        ragdoll_scene(ed);
        return;
    }
    if (!strcmp(name, "active_ragdoll")) {
        active_ragdoll_scene(ed);
        return;
    }
    if (!strcmp(name, "splines")) { // (Q14, editor_spline.inc)
        splines_scene(ed);
        return;
    }
    if (!strcmp(name, "toon")) { // (Q11, editor_toon.c)
        editor_toon_scene(ed);
        return;
    }
    if (!strcmp(name, "fps_camera")) { // (Q12, fpcam_test.c)
        editor_q12_scene(ed, name);
        return;
    }
    if (!strcmp(name, "socket_offset") || !strcmp(name, "cloth_on_body") || !strcmp(name, "ragdoll_q10") || !strcmp(name, "getup_q10")) { // (Q10, editor_q10.c)
        editor_q10_scene(ed, name);
        return;
    }
    if (!strcmp(name, "skin_churn")) {
        skin_churn_scene(ed);
        return;
    }
    if (!strcmp(name, "vehicles")) {
        vehicles_scene(ed);
        return;
    }
    if (!strcmp(name, "cloth")) {
        cloth_scene(ed);
        return;
    }
    if (!strcmp(name, "lake")) {
        lake_scene(ed);
        return;
    }
    if (!strcmp(name, "cloth_mesh")) {
        cloth_mesh_scene(ed);
        return;
    }
    if (!strcmp(name, "sofa")) { // (Q9, notas-dev/soft-body.md)
        sofa_scene(ed);
        return;
    }
    if (!strcmp(name, "softbody")) {
        softbody_scene(ed);
        return;
    }
    if (!strcmp(name, "softbody_rigid")) {
        softbody_rigid_scene(ed);
        return;
    }
    if (!strcmp(name, "rain") || !strcmp(name, "ocean") || !strcmp(name, "snow")) {
        water_scene(ed, name[0] == 'o');
        if (name[0] == 's') // (the same yard under a snowfall: the cover fast, to see it in a test)
            for (uint32_t ai = 0; ai < ed->world.archetype_count; ai++) {
                EcsArchetype* a = &ed->world.archetypes[ai];
                if (!(a->mask & COMP_BIT(COMP_RAIN))) continue;
                CRain* r = ecs_arch_column(a, COMP_RAIN);
                for (uint32_t i = 0; i < a->count; i++) r[i].snow = 1, r[i].intensity = 30.0f, r[i].cover_time = 4.0f, r[i].start_water = 0.0f;
            }
        if (name[0] == 'r') { // (2026-11-10) the blockout pieces under the rain: prototype stairs and a room with doors
            World* w = &ed->world;
            Entity st = spawn_actor(ed, SPAWN_PROTO, PROTO_STAIRS, (float[]){ -300, -2300, 0 }, 0);
            ((CProto*)ecs_get(w, st, COMP_PROTO))->size[2] = 160.0f;
            spawn_actor(ed, SPAWN_PROTO, PROTO_ROOM, (float[]){ 1300, -1900, 0 }, 0);
            ed->selected = ENTITY_NONE;
        }
        if (name[0] == 's' || name[0] == 'r') { // someone walking in a circle in it (editor_snow_tick): the feet's prints in the cover; in the rain the floor under it stays wet
            World* w = &ed->world;
            Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_SKELETAL_MESH));
            snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "Walker");
            CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
            t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE, t->location[0] = -800.0f, t->location[1] = -1500.0f, t->location[2] = 90.0f;
            CCharacter* ch = ecs_get(w, e, COMP_CHARACTER);
            ch->max_walk_speed = 150.0f, ch->radius = 34.0f, ch->half_height = 88.0f, ch->on_ground = 1;
            CSkeletalMesh* sk = ecs_get(w, e, COMP_SKELETAL_MESH);
            const SkelMesh* m = anim_mesh_name("SK_Mannequin");
            sk->mesh = m ? m->hash : 0;
            g_snow_walker = e, g_snow_t = 0.0, g_walk_speed = 150.0f;
        }
        return;
    }
    if (!strcmp(name, "wp") || !strcmp(name, "wp_fast")) { // (B1: World Partition; Q27.7: a car's 250 km/h)
        g_wp_speed = name[2] ? 25000.0f / 3.6f : 40000.0f;
        wp_scene(ed);
        return;
    }
    if (!strcmp(name, "procgen") || !strcmp(name, "facility")) {
        procgen_scene(ed, name[0] == 'f');
        return;
    }
    if (!strncmp(name, "particles", 9)) { // the particles' scene (notas-dev/particulas.md): the galaxy's 5 M and the effects
        particles_scene(ed, name);
        return;
    }
    if (!strcmp(name, "ai")) {
        ai_scene(ed);
        return;
    }
    if (!strcmp(name, "rocks") || !strcmp(name, "rocks_tess") || !strcmp(name, "tess_seams")) { // (Q17: displaced, the seam check)
        heavy_scene_ex(ed, !strcmp(name, "rocks") ? 1 : !strcmp(name, "rocks_tess") ? 2 : 3);
        return;
    }
    if (!strcmp(name, "heavy")) {
        heavy_scene(ed);
        return;
    }
    if (!strcmp(name, "interiors")) { // (Q4)
        interiors_scene(ed);
        return;
    }
    if (!strcmp(name, "lightfn")) { // (Q4)
        light_function_scene(ed);
        return;
    }
    if (!strcmp(name, "glass")) {
        glass_scene(ed);
        return;
    }
    if (!strcmp(name, "masked_lobes")) {
        masked_lobes_scene(ed);
        return;
    }
    // "volume" (A9.5a): the example puff (24 OpenVDB frames) on the grid floor, a low sun through its smoke
    if (!strcmp(name, "volume")) {
        ecs_shutdown(&ed->world);
        ecs_init(&ed->world);
        ed->selected = ENTITY_NONE;
        float c[3] = { 0.6f, 0.6f, 0.6f };
        Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, c, 0, 0.8f);
        CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
        ft->scale[0] = ft->scale[1] = 20.0f;
        ((CStaticMesh*)ecs_get(&ed->world, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
        spawn_actor(ed, SPAWN_VOLUME, 0, (float[]){ 0, 0, 0 }, 0);
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
        CTransform* st = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
        st->rotation[0] = -25.0f, st->rotation[1] = 60.0f;
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 300 }, 0);
        ed->selected = ENTITY_NONE;
        ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -520.0f, ed->cam.pos[2] = 180.0f;
        ed->cam.yaw = 90.0f, ed->cam.pitch = -6.0f;
        ed->dirty = true;
        return;
    }
    // "splat" (A9.5d): the examples on the grid floor: the bush (.splat, Z up) and the flower bed (3DGS .ply, -Y up), lit
    // by a sun and the sky, a cube among them (the scene's depth in front of and behind the Gaussians)
    if (!strcmp(name, "splat")) {
        ecs_shutdown(&ed->world);
        ecs_init(&ed->world);
        ed->selected = ENTITY_NONE;
        splat_examples();
        float c[3] = { 0.6f, 0.6f, 0.6f };
        Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, c, 0, 0.8f);
        CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
        ft->scale[0] = ft->scale[1] = 20.0f;
        ((CStaticMesh*)ecs_get(&ed->world, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
        spawn_actor(ed, SPAWN_SPLAT, 0, (float[]){ -120, 0, 0 }, 0);
        spawn_actor(ed, SPAWN_SPLAT, 0, (float[]){ 160, -40, 0 }, 0);
        CSplat* fb = ecs_get(&ed->world, ed->selected, COMP_SPLAT);
        snprintf(fb->path, sizeof fb->path, "Splats/Example_Flowers.ply");
        fb->up_axis = 1;
        mesh_actor(ed, SHAPE_CUBE, 20, -120, 30, c, 0, 0.5f);
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
        CTransform* st = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
        st->rotation[0] = -40.0f, st->rotation[1] = 50.0f;
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 300 }, 0);
        ed->selected = ENTITY_NONE;
        ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -480.0f, ed->cam.pos[2] = 180.0f;
        ed->cam.yaw = 90.0f, ed->cam.pitch = -14.0f;
        ed->dirty = true;
        return;
    }
    // "fogvol" (A9.5b): two Local Fog Volumes (a sphere of white mist, a box of green swamp haze) on the grid floor, a low
    // sun through them (its shafts), a lamp inside the box; no height fog (the froxels run for them alone)
    if (!strcmp(name, "fogvol")) {
        ecs_shutdown(&ed->world);
        ecs_init(&ed->world);
        ed->selected = ENTITY_NONE;
        float c[3] = { 0.6f, 0.6f, 0.6f };
        Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, c, 0, 0.8f);
        CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
        ft->scale[0] = ft->scale[1] = 30.0f;
        ((CStaticMesh*)ecs_get(&ed->world, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
        for (int i = 0; i < 4; i++) mesh_actor(ed, SHAPE_CYLINDER, -600.0f + 400.0f * (float)i, 300.0f, 150.0f, c, 0, 0.7f); // (pillars: the shafts)
        spawn_actor(ed, SPAWN_FOG_VOLUME, 0, (float[]){ -400, 0, 0 }, 0);
        spawn_actor(ed, SPAWN_FOG_VOLUME, 0, (float[]){ 450, 0, -150 }, 0);
        CFogVolume* bx = ecs_get(&ed->world, ed->selected, COMP_FOG_VOLUME);
        bx->shape = 1, bx->density = 0.8f, bx->albedo[0] = 0.55f, bx->albedo[1] = 0.8f, bx->albedo[2] = 0.45f, bx->edge_falloff = 0.3f;
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_POINT, (float[]){ 450, 0, 150 }, 0);
        CLight* lp = ecs_get(&ed->world, ed->selected, COMP_LIGHT);
        lp->intensity = 3000.0f, lp->attenuation_radius = 900.0f, lp->color[0] = 1.0f, lp->color[1] = 0.75f, lp->color[2] = 0.4f;
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 800 }, 0);
        CTransform* st = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
        st->rotation[0] = -20.0f, st->rotation[1] = -70.0f;
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -600, 600 }, 0);
        ed->selected = ENTITY_NONE;
        ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -1500.0f, ed->cam.pos[2] = 350.0f;
        ed->cam.yaw = 90.0f, ed->cam.pitch = -8.0f;
        ed->dirty = true;
        return;
    }
    // "river" (B12c, B12f): a Water Body River (the default 40 m spline) over a floor 2 m under it; three crates with
    // Buoyancy dropped into it upstream (they float and drift downstream: editor_river_check after --play) and a
    // Character in the deep water (it swims: its head above the surface). "underwater": the camera under the surface
    if (!strcmp(name, "river") || !strcmp(name, "underwater")) {
        ecs_shutdown(&ed->world);
        ecs_init(&ed->world);
        ed->selected = ENTITY_NONE;
        float c[3] = { 0.55f, 0.5f, 0.42f };
        Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, -200, c, 0, 0.9f);
        CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
        ft->scale[0] = ft->scale[1] = 60.0f;
        spawn_actor(ed, SPAWN_RIVER, 0, (float[]){ 0, 0, 0 }, 0);
        CWater* wb = ecs_get(&ed->world, ed->selected, COMP_WATER);
        if (wb) wb->clarity = 300.0f, wb->color[0] = 0.02f, wb->color[1] = 0.07f, wb->color[2] = 0.06f;
        float crate[3] = { 0.5f, 0.35f, 0.2f };
        for (int i = 0; i < 3; i++) {
            Entity e = mesh_actor(ed, SHAPE_CUBE, -1650.0f + 80.0f * (float)i, -60.0f + 60.0f * (float)i, 60.0f, crate, 0, 0.7f);
            CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
            t->scale[0] = t->scale[1] = t->scale[2] = 0.6f;
            t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
            ecs_add(&ed->world, e, COMP_RIGID_BODY), ecs_add(&ed->world, e, COMP_BUOYANCY);
            CRigidBody* rb = ecs_get(&ed->world, e, COMP_RIGID_BODY);
            rb->simulate = 1, rb->enable_gravity = 1, rb->mass = 60.0f; // (wood: lighter than its water)
        }
        Entity ch = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER));
        set_name(&ed->world, ch, "Swimmer");
        CCharacter* cc = ecs_get(&ed->world, ch, COMP_CHARACTER); // (the Character's defaults, as a class's)
        cc->max_walk_speed = 450.0f, cc->jump_z_velocity = 520.0f, cc->gravity_scale = 1.0f, cc->radius = 34.0f, cc->half_height = 88.0f;
        CTransform* ct = ecs_get(&ed->world, ch, COMP_TRANSFORM);
        ct->location[0] = -1000.0f, ct->location[1] = 250.0f, ct->location[2] = -40.0f, ct->scale[0] = ct->scale[1] = ct->scale[2] = 1.0f;
        ct->flags = (ct->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 800 }, 0);
        CTransform* st = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
        st->rotation[0] = -40.0f, st->rotation[1] = 30.0f;
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -600, 600 }, 0);
        ed->selected = ENTITY_NONE;
        if (!strcmp(name, "underwater")) { // (in the river, a meter down, looking along it)
            ed->cam.pos[0] = -800.0f, ed->cam.pos[1] = 260.0f, ed->cam.pos[2] = -100.0f;
            ed->cam.yaw = 0.0f, ed->cam.pitch = 5.0f;
        } else {
            ed->cam.pos[0] = -1900.0f, ed->cam.pos[1] = -900.0f, ed->cam.pos[2] = 700.0f;
            ed->cam.yaw = 30.0f, ed->cam.pitch = -25.0f;
        }
        ed->dirty = true;
        return;
    }
    // "fire" (A14): a campfire on the grid floor, a wall and a pillar it lights, a low sun (dusk), the camera 4 m away
    if (!strcmp(name, "fire")) {
        ecs_shutdown(&ed->world);
        ecs_init(&ed->world);
        ed->selected = ENTITY_NONE;
        float c[3] = { 0.6f, 0.6f, 0.6f };
        Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, c, 0, 0.8f);
        CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
        ft->scale[0] = ft->scale[1] = 20.0f;
        ((CStaticMesh*)ecs_get(&ed->world, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
        Entity wl = mesh_actor(ed, SHAPE_CUBE, 0, 250, 150, c, 0, 0.8f);
        CTransform* wt = ecs_get(&ed->world, wl, COMP_TRANSFORM);
        wt->scale[0] = 6.0f, wt->scale[1] = 0.3f, wt->scale[2] = 3.0f;
        mesh_actor(ed, SHAPE_CYLINDER, 200, 60, 50, c, 0, 0.6f);
        spawn_actor(ed, SPAWN_FIRE, 0, (float[]){ 0, 0, 40 }, 0); // (its box from 40 cm below it: on the floor)
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
        CTransform* st = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
        st->rotation[0] = -8.0f, st->rotation[1] = 40.0f;
        CLight* sl = ecs_get(&ed->world, ed->selected, COMP_LIGHT);
        sl->intensity = 2000.0f; // (dusk: the fire carries the scene)
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 300 }, 0);
        ed->selected = ENTITY_NONE;
        ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -420.0f, ed->cam.pos[2] = 140.0f;
        ed->cam.yaw = 90.0f, ed->cam.pitch = -6.0f;
        ed->dirty = true;
        return;
    }
    // "skin": the skin's screen space scattering (A9.3a, sss.comp): a head-sized sphere and, beside it, plates 2 mm to 2 cm
    // thick (an ear's, a hand's) and a finger, all of Examples/M_HumanSkin, a lamp behind the plates (the transmission:
    // the thin ones glow red, the thick ones stay dark), the sun low behind, the sky; two eyes (A9.3c, M_Eye on SM_Eye)
    // on the head looking at the camera. "eyes": the same, the camera 20 cm from the eyes
    bool hair_dyn = !strcmp(name, "hair_dynamic"); // (2026-11-16: the same head, its hair simulated, the head swaying in a wind)
    if (hair_dyn) name = "hair";
    if (!strcmp(name, "skin") || !strcmp(name, "eyes") || !strcmp(name, "hair")) { // ("hair": a Groom on the head, A9.3b)
        ecs_shutdown(&ed->world);
        ecs_init(&ed->world);
        ed->selected = ENTITY_NONE;
        float c[3] = { 0.6f, 0.6f, 0.6f };
        Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, c, 0, 0.6f);
        CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
        ft->scale[0] = ft->scale[1] = 4.0f;
        ((CStaticMesh*)ecs_get(&ed->world, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
        uint32_t skin = content_material("Examples/M_HumanSkin.rmat");
        if (!skin) printf("[RealEngine] FAIL: skin: Examples/M_HumanSkin.rmat missing\n");
        // (x, z, sx, sy, sz, shape): the head, four plates, a finger
        static const float parts[][6] = { { -22, 40, 0.22f, 0.22f, 0.26f, SHAPE_SPHERE }, { 6, 40, 0.06f, 0.002f, 0.08f, SHAPE_CUBE },
                                          { 15, 40, 0.06f, 0.005f, 0.08f, SHAPE_CUBE },    { 24, 40, 0.06f, 0.01f, 0.08f, SHAPE_CUBE },
                                          { 33, 40, 0.06f, 0.02f, 0.08f, SHAPE_CUBE },     { 42, 40, 0.017f, 0.017f, 0.08f, SHAPE_CYLINDER } };
        for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
            const float* p = parts[i];
            Entity e = mesh_actor(ed, (int)p[5], p[0], 0, p[1], c, 0, 0.5f);
            ((CStaticMesh*)ecs_get(&ed->world, e, COMP_STATIC_MESH))->material = skin;
            CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
            t->scale[0] = p[2], t->scale[1] = p[3], t->scale[2] = p[4];
            if (i == 0 && !strcmp(name, "hair")) { // (its defaults: long, brown, A9.3b; the head's front, +X, to -Y as the eyes')
                ecs_add(&ed->world, e, COMP_GROOM);
                t = ecs_get(&ed->world, e, COMP_TRANSFORM);
                t->rotation[1] = -90.0f;
                if (hair_dyn) {
                    CGroom* gr = ecs_get(&ed->world, e, COMP_GROOM);
                    gr->dynamic = 1, gr->wind[0] = 250.0f, gr->wind[1] = 80.0f, gr->gusts = 0.5f;
                    t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
                    g_hair_head = e, g_hair_t = 0.0;
                }
            }
        }
        if (eye_mesh()) render_meshes_reload(ed->vk);
        uint32_t em = render_mesh_id_of("/Content/Meshes/Examples/SM_Eye.rmesh"), eyem = content_material("Examples/M_Eye.rmat");
        for (int i = 0; i < 2; i++) { // (in front of the head, a little sunk into it; +X, their look, toward the camera)
            Entity e = mesh_actor(ed, SHAPE_SPHERE, -22.0f + (i ? 3.3f : -3.3f), -10.2f, 44.0f, c, 0, 0.5f);
            CStaticMesh* sm = ecs_get(&ed->world, e, COMP_STATIC_MESH);
            if (render_mesh_slot(em) >= SHAPE_COUNT) sm->mesh = em;
            else printf("[RealEngine] FAIL: eyes: SM_Eye not loaded\n");
            sm->material = eyem;
            CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
            t->scale[0] = t->scale[1] = t->scale[2] = 0.024f;
            t->rotation[1] = -90.0f + (i ? -4.0f : 4.0f); // (yaw: +X to -Y, a little converged)
        }
        if (!eyem) printf("[RealEngine] FAIL: eyes: Examples/M_Eye.rmat missing\n");
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_POINT, (float[]){ 20, 30, 42 }, 0); // (behind the plates)
        CLight* l = ecs_get(&ed->world, ed->selected, COMP_LIGHT);
        l->intensity = 3000.0f, l->source_radius = 2.0f, l->attenuation_radius = 400.0f, l->cast_shadows = 1;
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 300 }, 0);
        CTransform* st = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
        st->rotation[0] = -20.0f, st->rotation[1] = -105.0f; // (low, from behind: the plates backlit)
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -100, 200 }, 0);
        ed->selected = ENTITY_NONE;
        if (!strcmp(name, "hair")) { // (three quarters, from the front left: the hair's highlights and its falling locks)
            ed->cam.pos[0] = -62.0f, ed->cam.pos[1] = -48.0f, ed->cam.pos[2] = 52.0f;
            ed->cam.yaw = 50.0f, ed->cam.pitch = -8.0f;
        } else if (!strcmp(name, "eyes")) { // (a lamp in front to the side: the highlight on the cornea, the iris's caustic)
            spawn_actor(ed, SPAWN_LIGHT, LIGHT_POINT, (float[]){ 10, -40, 60 }, 0);
            CLight* fl2 = ecs_get(&ed->world, ed->selected, COMP_LIGHT);
            fl2->intensity = 2500.0f, fl2->source_radius = 3.0f, fl2->attenuation_radius = 300.0f, fl2->cast_shadows = 1;
            ed->selected = ENTITY_NONE;
            ed->cam.pos[0] = -22.0f, ed->cam.pos[1] = -24.0f, ed->cam.pos[2] = 45.0f;
            ed->cam.yaw = 90.0f, ed->cam.pitch = -2.0f;
        } else {
            ed->cam.pos[0] = 8.0f, ed->cam.pos[1] = -70.0f, ed->cam.pos[2] = 46.0f;
            ed->cam.yaw = 90.0f, ed->cam.pitch = -4.0f;
        }
        ed->dirty = true;
        return;
    }
    if (!strcmp(name, "haunted")) {
        haunted_scene(ed);
        return;
    }
    if (!strcmp(name, "metro")) {
        metro_scene(ed);
        return;
    }
    if (!strcmp(name, "autorig")) {
        autorig_scene(ed);
        return;
    }
    if (!strcmp(name, "packs")) {
        packs_scene(ed);
        return;
    }
    if (!strcmp(name, "proto")) {
        proto_scene(ed);
        return;
    }
    if (!strcmp(name, "landscape_hm")) {
        landscape_hm_scene(ed);
        return;
    }
    if (!strcmp(name, "landscape")) {
        landscape_scene(ed);
        return;
    }
    if (!strcmp(name, "landscape_paint")) { // (B11) its material with three Landscape Layers, painted in three circles while playing
        landscape_scene(ed);
        CLandscape* plc = ecs_get(&ed->world, g_land, COMP_LANDSCAPE);
        if (plc) snprintf(plc->name, sizeof plc->name, "PaintLandscape"); // (its own files: the landscape test's stay unpainted)
        char p[700];
        snprintf(p, sizeof p, "%s/LandTest", content_root());
        re_make_dirs(p);
        snprintf(p, sizeof p, "%s/LandTest/M_LandLayers.rmat", content_root());
        Material* m = mat_load(p);
        if (!m) m = mat_create(p, false, NULL);
        if (m) {
            snprintf(m->layer_path[0], sizeof m->layer_path[0], "/Content/Materials/Examples/M_Marble.rmat");
            snprintf(m->layer_path[1], sizeof m->layer_path[1], "/Content/Materials/Examples/M_RustedMetal.rmat");
            snprintf(m->layer_path[2], sizeof m->layer_path[2], "/Content/Materials/Examples/M_BrushedMetal.rmat");
            for (int k = 0; k < 3; k++) m->layer_tile[k] = 300.0f;
            mat_save(m), mat_mark_dirty(m);
            CLandscape* lc = ecs_get(&ed->world, g_land, COMP_LANDSCAPE);
            if (lc) lc->material = m->hash;
        }
        g_land_paint_t = 0.0;
        ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -9000.0f, ed->cam.pos[2] = 4500.0f, ed->cam.yaw = 90.0f, ed->cam.pitch = -28.0f;
        return;
    }
    if (!strncmp(name, "ai_squad", 8)) {
        ai_squad_scene(ed, !strcmp(name, "ai_squad_mm"));
        return;
    }
    if (!strncmp(name, "anim", 4)) {
        int n = atoi(name + 4);
        anim_scene(ed, n > 0 ? (uint32_t)n : 25u);
        return;
    }
    if (!strncmp(name, "phys_", 5) && phys_test_scene(ed, name)) return; // (Q8) the rigid bodies' tests (phys_test.c)
    if (!strncmp(name, "terror_", 7) && terror_test_scene(ed, name)) return; // (Q13) the horror kit's tests (terror_test.c)
    if (!strncmp(name, "pcg_", 4) && pcg_test_scene(ed, name)) return; // (Q15) the PCG's tests (pcg_test.c)
    if (!strncmp(name, "udim", 4) && udim_test_scene(ed, name)) return; // (Q18) UDIM's (udim_test.c)
    if (!strncmp(name, "emit", 4) && emissive_test_scene(ed, name)) return; // (2026-11-23) emissive lighting (emissive_test.c)
    if (!strncmp(name, "vista", 5) && farview_test_scene(ed, name)) return; // (Q21) the lookout (farview_test.c)
    if (!strcmp(name, "ai_senses") && ai_senses_test_scene(ed, name)) return; // (Q24) the damage sense, the avoidance (ai_senses_test.c)
    if (!strcmp(name, "physics_shapes")) { // the collision shapes (notas-dev/fisica.md)
        shapes_scene(ed);
        return;
    }
    if (!strncmp(name, "physics", 7)) {
        physics_scene(ed, !strncmp(name, "physics_stress", 14), !strcmp(name, "physics_stress_shapes"));
        return;
    }
    if (!strcmp(name, "decals")) { // the decals' test (M6): grime on the floor, on a wall, a bullet-hole-sized one fading
        ecs_shutdown(&ed->world);
        ecs_init(&ed->world);
        ed->selected = ENTITY_NONE;
        float c[3] = { 0.6f, 0.6f, 0.6f };
        Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, c, 0, 0.6f);
        CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
        ft->scale[0] = ft->scale[1] = 20.0f;
        ((CStaticMesh*)ecs_get(&ed->world, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
        Entity wl = mesh_actor(ed, SHAPE_CUBE, 0, 400, 150, c, 0, 0.6f);
        CTransform* wt = ecs_get(&ed->world, wl, COMP_TRANSFORM);
        wt->scale[0] = 8.0f, wt->scale[1] = 0.2f, wt->scale[2] = 3.0f;
        ((CStaticMesh*)ecs_get(&ed->world, wl, COMP_STATIC_MESH))->material = content_material("Examples/M_DetailPlaster.rmat");
        uint32_t grime = content_material("Examples/M_DecalGrime.rmat");
        { // M7: a slab of cobblestones with parallax beside the grime (the decals' scene: the RE_EXT pipelines)
            Entity cb = mesh_actor(ed, SHAPE_CUBE, -330, 150, 5, c, 0, 0.6f);
            CTransform* cbt = ecs_get(&ed->world, cb, COMP_TRANSFORM);
            cbt->scale[0] = cbt->scale[1] = 2.5f, cbt->scale[2] = 0.1f;
            CStaticMesh* cbm = ecs_get(&ed->world, cb, COMP_STATIC_MESH);
            cbm->material = content_material("Examples/M_ParallaxCobble.rmat");
            cbm->uv_scale = 1.0f;
        }
        static const float floor_at[4][3] = { { -200, 0, 0 }, { 0, 50, 0 }, { 220, -60, 0 }, { 60, 220, 0 } };
        for (int i = 0; i < 4; i++) {
            float size[3] = { 50.0f, 60.0f + 30.0f * (float)i, 60.0f + 30.0f * (float)i };
            decal_spawn(&ed->world, grime, floor_at[i], (float[3]){ -90.0f, 0.0f, 37.0f * (float)i }, size, 0.0f);
        }
        decal_spawn(&ed->world, grime, (float[3]){ 0, 370, 180 }, (float[3]){ 0, 90, 0 }, (float[3]){ 40, 120, 90 }, 0.0f); // (on the wall: facing +Y)
        decal_spawn(&ed->world, grime, (float[3]){ -80, -120, 0 }, (float[3]){ -90, 0, 0 }, (float[3]){ 20, 15, 15 }, 30.0f); // (a small one, a life span)
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
        CTransform* st = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
        st->rotation[0] = -45.0f, st->rotation[1] = 30.0f;
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
        ed->selected = ENTITY_NONE;
        ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -520.0f, ed->cam.pos[2] = 260.0f;
        ed->cam.yaw = 90.0f, ed->cam.pitch = -22.0f;
        return;
    }
    if (!strcmp(name, "materials")) { // the materials' test: the grid floor and the examples on shapes
        ecs_shutdown(&ed->world);
        ecs_init(&ed->world);
        ed->selected = ENTITY_NONE;
        float c[3] = { 0.6f, 0.6f, 0.6f };
        Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, c, 0, 0.6f);
        CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
        ft->scale[0] = ft->scale[1] = 20.0f;
        CStaticMesh* fm = ecs_get(&ed->world, fl, COMP_STATIC_MESH);
        fm->material = content_material("M_Grid.rmat");
        static const char* const ex[5] = { "Examples/M_Marble.rmat", "Examples/M_TintedChecker.rmat",
                                           "Examples/M_RustedMetal.rmat", "Examples/M_EmissivePanel.rmat", "Examples/M_AnimatedEnergy.rmat" };
        for (int i = 0; i < 5; i++) {
            Entity e = mesh_actor(ed, i % 2 ? SHAPE_CUBE : SHAPE_SPHERE, -400.0f + 200.0f * (float)i, 0, 60, c, 0, 0.5f);
            CStaticMesh* m = ecs_get(&ed->world, e, COMP_STATIC_MESH);
            m->material = content_material(ex[i]);
            CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
            t->scale[0] = t->scale[1] = t->scale[2] = 1.2f;
            if (i == 1) { // what a Blueprint's Set Vector Parameter Value does: the checker's own instance, red
                mat_update(ed->vk, VK_NULL_HANDLE); // (compiled now: the instance copies its values)
                Material* mm = mat_find_hash(m->material);
                uint32_t slot = mat_dynamic_for(e, mm);
                mat_set_param(slot, "Tint", (float[4]){ 1.0f, 0.15f, 0.1f, 1.0f });
            }
        }
        // M5: a mesh's vertex colors (the Color Attribute) and a detail normal (plaster with fine bumps), a third row
        if (vcol_sphere_mesh()) render_meshes_reload(ed->vk);
        {
            uint32_t vm = render_mesh_id_of("/Content/Meshes/Examples/SM_VertexColorSphere.rmesh");
            Entity e = mesh_actor(ed, SHAPE_SPHERE, -130.0f, 480.0f, 60, c, 0, 0.5f);
            CStaticMesh* sm = ecs_get(&ed->world, e, COMP_STATIC_MESH);
            if (render_mesh_slot(vm) >= SHAPE_COUNT) sm->mesh = vm;
            else printf("[RealEngine] FAIL: materials: the vertex color sphere not loaded\n");
            sm->material = content_material("Examples/M_VertexColor.rmat");
            CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
            t->scale[0] = t->scale[1] = t->scale[2] = 1.2f;
            Entity d = mesh_actor(ed, SHAPE_SPHERE, 130.0f, 480.0f, 60, c, 0, 0.5f);
            ((CStaticMesh*)ecs_get(&ed->world, d, COMP_STATIC_MESH))->material = content_material("Examples/M_DetailPlaster.rmat");
            t = ecs_get(&ed->world, d, COMP_TRANSFORM);
            t->scale[0] = t->scale[1] = t->scale[2] = 1.2f;
        }
        // the Principled's extra lobes (M1b): car paint (coat), brushed metal, skin, glass, behind the first row
        static const char* const lobes[6] = { "Examples/M_CarPaint.rmat", "Examples/M_BrushedMetal.rmat", "Examples/M_Skin.rmat",
                                              "Examples/M_Glass.rmat", "Examples/M_Velvet.rmat", "Examples/M_SoapFilm.rmat" };
        for (int i = 0; i < 6; i++) { // (the glass on a cylinder: a translucent material on the spheres' mesh would draw them all
                                     // by the masked pipelines, without RE_EXT: render.c's "aviso")
            Entity e = mesh_actor(ed, i == 3 ? SHAPE_CYLINDER : SHAPE_SPHERE, -500.0f + 200.0f * (float)i, 250.0f, 60, c, 0, 0.5f);
            ((CStaticMesh*)ecs_get(&ed->world, e, COMP_STATIC_MESH))->material = content_material(lobes[i]);
            CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
            t->scale[0] = t->scale[1] = t->scale[2] = 1.2f;
        }
        // (2026-11-16) the Wet Surface: wet concrete and sweaty skin (sphere and cube), a third row
        static const char* const wets[4] = { "Examples/M_WetConcrete.rmat", "Examples/M_WetConcrete.rmat", "Examples/M_SweatySkin.rmat",
                                             "Examples/M_SweatySkin.rmat" };
        for (int i = 0; i < 4; i++) {
            Entity e = mesh_actor(ed, i & 1 ? SHAPE_CUBE : SHAPE_SPHERE, -300.0f + 200.0f * (float)i, -200.0f, 60, c, 0, 0.5f);
            ((CStaticMesh*)ecs_get(&ed->world, e, COMP_STATIC_MESH))->material = content_material(wets[i]);
            CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
            t->scale[0] = t->scale[1] = t->scale[2] = 1.2f;
            t->rotation[2] = 25.0f;
        }
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
        CTransform* st = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
        st->rotation[0] = -40.0f;
        st->rotation[1] = 30.0f;
        spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
        ed->selected = ENTITY_NONE;
        ed->cam.pos[0] = 0.0f; ed->cam.pos[1] = -520.0f; ed->cam.pos[2] = 200.0f;
        ed->cam.yaw = 90.0f; ed->cam.pitch = -14.0f;
        ed->dirty = true;
        return;
    }
    // "shadow_ghost" (2026-11-10, "a sombra tÃ¡ com ghosting" no Realista, sÃ³ luzes pontuais): a night floor (no sky light),
    // three point lights casting shadows, a cube sliding back and forth at 4 m/s between them (in Play: --play-eject, a
    // still camera). Its shadows must not trail behind it (shadow_split.comp's history)
    if (!strcmp(name, "shadow_ghost")) {
        ecs_shutdown(&ed->world);
        ecs_init(&ed->world);
        ed->selected = ENTITY_NONE;
        const float grey[3] = { 0.35f, 0.35f, 0.35f }, red[3] = { 0.8f, 0.15f, 0.1f };
        Entity fl = mesh_actor(ed, SHAPE_CUBE, 0, 0, -10, grey, 0, 0.8f);
        CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
        ft->scale[0] = 20.0f, ft->scale[1] = 20.0f, ft->scale[2] = 0.2f, ft->flags = MOBILITY_STATIC;
        g_ghost_cube = mesh_actor(ed, SHAPE_CUBE, 0, 0, 60, red, 0, 0.6f);
        CTransform* ct = ecs_get(&ed->world, g_ghost_cube, COMP_TRANSFORM);
        ct->scale[0] = ct->scale[1] = ct->scale[2] = 1.2f, ct->flags = MOBILITY_MOVABLE;
        static const float lamps[3][3] = { { -250, -200, 180 }, { 250, -150, 160 }, { 0, 250, 200 } };
        for (int i = 0; i < 3; i++) {
            spawn_actor(ed, SPAWN_LIGHT, LIGHT_POINT, (float[]){ lamps[i][0], lamps[i][1], lamps[i][2] }, 0);
            CLight* l = ecs_get(&ed->world, ed->selected, COMP_LIGHT);
            l->intensity = 6000.0f, l->source_radius = 3.0f, l->attenuation_radius = 3000.0f, l->cast_shadows = 1;
        }
        ed->selected = ENTITY_NONE;
        ed->test_step = ghost_step;
        ed->cam.pos[0] = -700.0f; ed->cam.pos[1] = -700.0f; ed->cam.pos[2] = 500.0f;
        ed->cam.yaw = 45.0f; ed->cam.pitch = -30.0f;
        ed->dirty = true;
        return;
    }
    // "spec_leak": light through walls (notas-dev/luz.md "Especular vazando"): two rooms split by a 5 cm wall made of two
    // overlapping cubes, with a T wall crossing into it; behind it a strong soft lamp 30 cm away and a second light; the
    // dark side's floor and the wall's face metallic and smooth (a leak shows as their highlight). The camera sees only
    // the dark side: everything it sees should stay black (the test spec_leak: --max-scene-lum)
    if (!strcmp(name, "spec_leak")) {
        ecs_shutdown(&ed->world);
        ecs_init(&ed->world);
        ed->selected = ENTITY_NONE;
        const float plaster[3] = { 0.8f, 0.8f, 0.78f }, steel[3] = { 0.9f, 0.9f, 0.9f };
        // (x, y, z, sx, sy, sz, metal): the lit side's floor, the dark side's (metal), the ceiling, the outer walls, the
        // two halves of the 5 cm wall (overlapping 20 cm in the middle, into the floor and the ceiling), the T crossing it
        static const float boxes[][7] = {
            { 210, 0, -10, 4.2f, 6.4f, 0.2f, 0 },    { -210, 0, -10, 4.2f, 6.4f, 0.2f, 1 }, { 0, 0, 310, 8.4f, 6.4f, 0.2f, 0 },
            { 0, -310, 150, 8.4f, 0.2f, 3.2f, 0 },   { 0, 310, 150, 8.4f, 0.2f, 3.2f, 0 },  { -410, 0, 150, 0.2f, 6.4f, 3.2f, 0 },
            { 410, 0, 150, 0.2f, 6.4f, 3.2f, 0 },    { 0, -140, 150, 0.05f, 3.2f, 3.2f, 1 }, { 0, 150, 150, 0.05f, 3.0f, 3.2f, 1 },
            { -50, -100, 150, 1.05f, 0.05f, 3.2f, 1 },
        };
        for (size_t i = 0; i < sizeof boxes / sizeof boxes[0]; i++) {
            const float* b = boxes[i];
            Entity e = mesh_actor(ed, SHAPE_CUBE, b[0], b[1], b[2], b[6] > 0 ? steel : plaster, b[6], b[6] > 0 ? 0.25f : 0.8f);
            CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
            t->scale[0] = b[3]; t->scale[1] = b[4]; t->scale[2] = b[5];
            t->flags = MOBILITY_STATIC;
        }
        // the lamp behind the wall (a 10 cm sphere: an area light, the filtered shadows' path) and a second light
        static const float lamps[2][6] = { { 30, 0, 150, 60000.0f, 10.0f, 2500.0f }, { 300, 150, 250, 20000.0f, 5.0f, 2500.0f } };
        for (int i = 0; i < 2; i++) {
            spawn_actor(ed, SPAWN_LIGHT, LIGHT_POINT, (float[]){ lamps[i][0], lamps[i][1], lamps[i][2] }, 0);
            CLight* l = ecs_get(&ed->world, ed->selected, COMP_LIGHT);
            l->intensity = lamps[i][3], l->source_radius = lamps[i][4], l->attenuation_radius = lamps[i][5];
            l->cast_shadows = 1;
        }
        ed->selected = ENTITY_NONE;
        ed->cam.pos[0] = -380.0f; ed->cam.pos[1] = -260.0f; ed->cam.pos[2] = 170.0f;
        ed->cam.yaw = 39.0f; ed->cam.pitch = -12.0f;
        ed->dirty = true;
        return;
    }
    // "house_thin": the inner wall 5 cm thick, thinner than a cache texel (15 cm at High): its texels straddle it
    bool thin = !strcmp(name, "house_thin");
    // "house_movable": the walls left Movable (what a new actor is): standing still they must block the GI the same
    bool movable = !strcmp(name, "house_movable");
    // (Q20) "house_ghost": a pillar sweeping across the view, in front of the lit door (editor_ghost_tick): the ghosting
    // probe's trail behind a moving actor (render_ghosting)
    bool ghost = !strcmp(name, "house_ghost");
    if (strcmp(name, "house") != 0 && !thin && !movable && !ghost) return;
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    const float wall[3] = { 0.8f, 0.8f, 0.78f }, floor_c[3] = { 0.55f, 0.42f, 0.3f };
    // (x, y, z) center and (sx, sy, sz) scale of each cube: floor, ceiling, four walls, an inner wall with a door
    static const float boxes[][6] = {
        { 0, 0, -10, 10.4f, 8.4f, 0.2f },     { 0, 0, 310, 10.4f, 8.4f, 0.2f },
        { 0, -410, 150, 10.4f, 0.2f, 3.0f },  { 0, 410, 150, 10.4f, 0.2f, 3.0f },
        { -510, 0, 150, 0.2f, 8.0f, 3.0f },   { 510, 0, 150, 0.2f, 8.0f, 3.0f },
        { 100, -250, 150, 0.2f, 3.0f, 3.0f }, { 100, 250, 150, 0.2f, 3.0f, 3.0f }, { 100, 0, 260, 0.2f, 2.0f, 0.8f },
    };
    for (size_t i = 0; i < sizeof boxes / sizeof boxes[0]; i++) {
        const float* b = boxes[i];
        Entity e = mesh_actor(ed, SHAPE_CUBE, b[0], b[1], b[2], i == 0 ? floor_c : wall, 0, 0.8f);
        CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        t->scale[0] = b[3]; t->scale[1] = b[4]; t->scale[2] = b[5];
        if (thin && i >= 6) t->scale[0] = 0.05f;
        t->flags = movable ? MOBILITY_MOVABLE : MOBILITY_STATIC;
    }
    // a red box in the room (Movable) and the spot light on the far wall
    mesh_actor(ed, SHAPE_CUBE, -250, 200, 50, (float[]){ 0.8f, 0.1f, 0.08f }, 0, 0.6f);
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SPOT, (float[]){ -150, -100, 200 }, 0);
    CLight* l = ecs_get(&ed->world, ed->selected, COMP_LIGHT);
    l->intensity = 20000.0f;
    l->attenuation_radius = 2000.0f;
    CTransform* lt = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -20.0f; // toward the -x wall
    lt->rotation[1] = 180.0f;
    ed->selected = ENTITY_NONE;
    if (ghost) {
        Entity e = mesh_actor(ed, SHAPE_CUBE, 165, 90, 80, (float[]){ 0.85f, 0.85f, 0.85f }, 0, 0.5f);
        CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        t->scale[0] = t->scale[1] = 0.25f, t->scale[2] = 1.6f;
        set_name(&ed->world, e, "GhostMover");
        ed->selected = ENTITY_NONE; // (no outline over the measure)
    }
    ed->cam.pos[0] = 400.0f; ed->cam.pos[1] = 300.0f; ed->cam.pos[2] = 170.0f;
    ed->cam.yaw = -150.0f; ed->cam.pitch = -5.0f;
    ed->dirty = true;
}

// (Q20) --scene house_ghost: the pillar back and forth across the view at 2 m/s (a walk), ~3 m in front of the camera,
// along the inner wall (in front of the lit door)
void editor_ghost_tick(Editor* ed, double dt) {
    static double t;
    t += dt > 0.1 ? 0.1 : dt;
    Entity e = ENTITY_NONE;
    for (uint32_t i = 1; i < ed->world.slot_count && e == ENTITY_NONE; i++) {
        Entity c = ecs_entity_at(&ed->world, i);
        const CName* n = c ? ecs_get(&ed->world, c, COMP_NAME) : NULL;
        if (n && !strcmp(n->text, "GhostMover")) e = c;
    }
    CTransform* tr = e ? ecs_get(&ed->world, e, COMP_TRANSFORM) : NULL;
    if (!tr) return;
    double ph = fmod(t * 200.0 / 260.0, 2.0), u = ph < 1.0 ? ph : 2.0 - ph; // (a triangle wave over the 2.6 m path)
    tr->location[0] = 165.0f, tr->location[1] = (float)(-40.0 + 260.0 * u);
    ecs_touch(&ed->world, e);
}

// Unreal's default level, smaller: a floor, a few shapes, the sun, the sky light, a warm point light, a player start
static void default_level(Editor* ed) {
    Entity floor = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, (float[]){ 0.45f, 0.45f, 0.45f }, 0, 0.8f);
    CTransform* t = ecs_get(&ed->world, floor, COMP_TRANSFORM);
    t->scale[0] = t->scale[1] = 40.0f;
    t->flags = MOBILITY_STATIC;
    set_name(&ed->world, floor, "Floor");
    ((CTransform*)ecs_get(&ed->world, mesh_actor(ed, SHAPE_CUBE, 0, 0, 50, (float[]){ 0.7f, 0.7f, 0.72f }, 0, 0.5f), COMP_TRANSFORM))->flags = MOBILITY_STATIC;
    mesh_actor(ed, SHAPE_SPHERE, 220, 160, 50, (float[]){ 0.95f, 0.72f, 0.35f }, 1.0f, 0.25f);
    mesh_actor(ed, SHAPE_CYLINDER, -180, 230, 50, (float[]){ 0.7f, 0.12f, 0.1f }, 0, 0.4f);
    Entity cone = mesh_actor(ed, SHAPE_CONE, 180, -240, 50, (float[]){ 0.15f, 0.45f, 0.8f }, 0, 0.35f);
    ecs_add(&ed->world, cone, COMP_ROTATING);
    ((CRotating*)ecs_get(&ed->world, cone, COMP_ROTATING))->rate[1] = 90.0f;
    set_name(&ed->world, cone, "RotatingCone");
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, -300, 600 }, 0);
    CLight* sun = ecs_get(&ed->world, ed->selected, COMP_LIGHT);
    sun->color[0] = sun->color[1] = sun->color[2] = 1.0f; // white above the atmosphere: it tints the sun itself
    CTransform* st = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    st->rotation[0] = -40.0f;
    st->rotation[1] = 35.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_POINT, (float[]){ -220, -160, 140 }, 0);
    CLight* pl = ecs_get(&ed->world, ed->selected, COMP_LIGHT);
    pl->color[0] = 1.0f; pl->color[1] = 0.55f; pl->color[2] = 0.25f;
    pl->intensity = 120000.0f; // cd: a floodlight, seen even in full sun
    pl->attenuation_radius = 800.0f;
    spawn_actor(ed, SPAWN_PLAYER_START, 0, (float[]){ -500, 0, 100 }, 0);
    spawn_actor(ed, SPAWN_CAMERA, 0, (float[]){ -650, -420, 260 }, 0);
    CTransform* ct = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    ct->rotation[0] = -15.0f;
    ct->rotation[1] = 33.0f;
    ed->selected = ENTITY_NONE;
}

static void reset_camera(Editor* ed) {
    ed->cam = (EdCamera){ { -700.0f, -420.0f, 330.0f }, 30.0f, -20.0f, 90.0f, 600.0f };
}

// --- Undo / Redo ---

static void step_free(struct UndoStep* s) {
    re_vm_release(&s->mem);
    levels_undo_free(s->levels);
    memset(s, 0, sizeof *s);
}

static bool step_take(Editor* ed, struct UndoStep* s) {
    size_t n = ecs_serialized_size(&ed->world);
    step_free(s);
    if (!re_vm_reserve(&s->mem, n) || !re_vm_ensure(&s->mem, n)) return false;
    s->data = s->mem.base;
    s->size = ecs_serialize(&ed->world, s->data);
    s->selected = ed->selected;
    s->levels = levels_undo_take(); // (the sublevels' list with the world: their actors' marks agree with it)
    return true;
}

static void step_apply(Editor* ed, const struct UndoStep* s) {
    if (ecs_deserialize(&ed->world, s->data, s->size)) ed->selected = ecs_alive(&ed->world, s->selected) ? s->selected : ENTITY_NONE;
    levels_undo_apply(ed, s->levels);
    levels_resync(ed); // (what came back is not a new actor for the current level)
    ed->renaming = ENTITY_NONE;
}

static void push_step(struct UndoStep* stack, int* count, struct UndoStep* s) {
    if (*count == RE_UNDO_MAX) { // the oldest goes
        step_free(&stack[0]);
        memmove(&stack[0], &stack[1], sizeof stack[0] * (RE_UNDO_MAX - 1));
        memset(&stack[RE_UNDO_MAX - 1], 0, sizeof stack[0]);
        (*count)--;
    }
    stack[(*count)++] = *s;
    memset(s, 0, sizeof *s);
}

static void clear_redo(Editor* ed) {
    for (int i = 0; i < ed->redo_count; i++) step_free(&ed->redo[i]);
    ed->redo_count = 0;
}

void editor_undo_push(Editor* ed) {
    editor_construct_restore(ed); // (the Construction Scripts' work is shown, never kept: sugestÃ£o #64)
    if (ed->playing) return; // the play world is thrown away at Stop
    struct UndoStep s = { 0 };
    if (!step_take(ed, &s)) return;
    push_step(ed->undo, &ed->undo_count, &s);
    clear_redo(ed);
    ed->dirty = true;
}

// (2026-11-23) The last step taken off without applying it: a gizmo drag cancelled (its actors already back)
void editor_undo_drop(Editor* ed) {
    if (ed->undo_count > 0) step_free(&ed->undo[--ed->undo_count]);
}

void editor_undo_clear(Editor* ed) { undo_clear(ed); }
static void undo_clear(Editor* ed) {
    for (int i = 0; i < ed->undo_count; i++) step_free(&ed->undo[i]);
    ed->undo_count = 0;
    clear_redo(ed);
    step_free(&ed->pending);
}

void editor_undo(Editor* ed, double now) {
    editor_construct_forget(); // (another world: the Construction Scripts run on it anew)
    if (ed->playing) return;
    if (!ed->undo_count) {
        editor_status(ed, now, TR("Nothing to undo."));
        return;
    }
    struct UndoStep cur = { 0 };
    if (!step_take(ed, &cur)) return;
    push_step(ed->redo, &ed->redo_count, &cur);
    struct UndoStep* s = &ed->undo[--ed->undo_count];
    step_apply(ed, s);
    step_free(s);
    ed->dirty = true;
}

void editor_redo(Editor* ed, double now) {
    editor_construct_forget(); // (another world: the Construction Scripts run on it anew)
    if (ed->playing) return;
    if (!ed->redo_count) {
        editor_status(ed, now, TR("Nothing to redo."));
        return;
    }
    struct UndoStep cur = { 0 };
    if (!step_take(ed, &cur)) return;
    push_step(ed->undo, &ed->undo_count, &cur);
    struct UndoStep* s = &ed->redo[--ed->redo_count];
    step_apply(ed, s);
    step_free(s);
    ed->dirty = true;
}

// --- Levels (.rlevel) ---

static void update_title(Editor* ed) {
    const char* name = ed->level_path[0] ? ed->level_path + strlen(ed->level_path) : "Untitled";
    if (ed->level_path[0])
        while (name > ed->level_path && name[-1] != '/' && name[-1] != '\\') name--;
    char t[256];
    if (ed->show_browser) snprintf(t, sizeof t, "Rivora Engine - Project Browser");
    else if (project_file()[0]) snprintf(t, sizeof t, "%s%s - %s - Rivora Engine", name, ed->dirty ? "*" : "", project()->name);
    else snprintf(t, sizeof t, "%s%s - Rivora Engine", name, ed->dirty ? "*" : "");
    if (strcmp(t, ed->title_shown) != 0) {
        snprintf(ed->title_shown, sizeof ed->title_shown, "%s", t);
        glfwSetWindowTitle(ed->window, t);
    }
}

static bool ask_level_path(Editor* ed, bool save, char* path, size_t cap) {
    if (ed->level_path[0]) snprintf(path, cap, "%s", ed->level_path);
    else {
        char maps[600];
        snprintf(maps, sizeof maps, "%s/Maps", content_root());
        re_make_dirs(maps);
        snprintf(path, cap, "%s/Untitled.rlevel", maps);
    }
    for (char* c = path; *c; c++)
        if (*c == '/') *c = '\\';
    return platform_file_dialog_ext(ed->window, save, TR("Rivora Engine Levels"), "rlevel", path, cap);
}

bool editor_save_level(Editor* ed, const char* path, double now) {
    editor_construct_restore(ed); // (the Construction Scripts' work is shown, never kept: sugestÃ£o #64)
    char chosen[1024];
    if (!path || !path[0]) {
        if (!ask_level_path(ed, true, chosen, sizeof chosen)) return false;
        path = chosen;
    }
    if (ed->playing) { // the editor's world, not the play one: what Stop gives back
        editor_status(ed, now, TR("Stop playing before saving."));
        return false;
    }
    size_t bytes = 0;
    procgen_purge(&ed->world); // (the procedural world: only its actor saved; made again next frame)
    pcg_purge(&ed->world); // (Q15: what the PCG components made; made again next frame)
    if (!levels_save(ed, path, &bytes)) { // (the sublevels' actors in their own files: editor_levels.c)
        editor_status(ed, now, TR("Could not save %s."), path);
        return false;
    }
    if (path != ed->level_path) snprintf(ed->level_path, sizeof ed->level_path, "%s", path);
    ed->dirty = false;
    content_mark_dirty();
    if (gi_lightmaps_present() && !levels_lighting_summed()) { // the baked lighting next to it (a sum of Lighting Levels: each bake wrote its own)
        char lp[1100];
        level_rlight_path(path, lp, sizeof lp);
        gi_lightmaps_save(ed->vk, lp);
    }
    project_save_thumbnail(ed); // the Project Browser's picture of it
    editor_status(ed, now, TR("Saved %s (%.1f KB)."), path, (double)bytes / 1024.0);
    return true;
}

bool editor_open_level(Editor* ed, const char* path, double now) {
    editor_construct_forget(); // (another world: the Construction Scripts run on it anew)
    char chosen[1024];
    if (!path || !path[0]) {
        if (!ask_level_path(ed, false, chosen, sizeof chosen)) return false;
        path = chosen;
    }
    if (ed->playing) editor_stop(ed, now);
    if (!level_load(&ed->world, path, &ed->cam)) {
        editor_status(ed, now, TR("%s is not a level this version can open."), path);
        return false;
    }
    bp_components_rebuild(&ed->world, NULL); // the Blueprints' component trees as their classes are now
    ed->selected = ed->renaming = ENTITY_NONE;
    undo_clear(ed);
    char lp[1100];
    level_rlight_path(path, lp, sizeof lp);
    if (!gi_lightmaps_load(ed->vk, lp)) gi_lightmaps_clear(ed->vk); // no baked lighting: the dynamic GI
    gi_reset_probes();
    if (path != ed->level_path) snprintf(ed->level_path, sizeof ed->level_path, "%s", path);
    levels_on_open(ed); // its sublevels (Window > Levels) and the Lighting Levels' lighting added
    ed->dirty = false;
    editor_status(ed, now, TR("Opened %s (%u actors)."), path, ed->world.alive);
    return true;
}

// --- Init ---

bool editor_init(Editor* ed, GLFWwindow* window, VkCtx* vk) {
    asset_viewer_init(vk); // (its thumbnails' decoder starts)
    memset(ed, 0, sizeof *ed);
    ed->window = window;
    ed->vk = vk;
    g_plugins_ed = ed;
    plugins_set_world(&ed->world);
    plugins_set_status(plugins_status_cb);
    ed->saved_scalability = -1; // (the current one is saved)
    ed->realtime = true; // (Unreal's default: the viewport moves)
    if (!ecs_init(&ed->world)) return false;
    ed->sel = malloc(sizeof(Entity) * ED_MAX_SEL); // once, at startup
    ed->sel_start = malloc(sizeof(CTransform) * ED_MAX_SEL);
    ed->sel_mark = calloc(RE_ECS_MAX_ENTITIES, 1);
    if (!ed->sel || !ed->sel_start || !ed->sel_mark) return false;
    ed->sel = malloc(sizeof(Entity) * ED_MAX_SEL); // once, at startup
    ed->sel_start = malloc(sizeof(CTransform) * ED_MAX_SEL);
    ed->sel_mark = calloc(RE_ECS_MAX_ENTITIES, 1);
    if (!ed->sel || !ed->sel_start || !ed->sel_mark) return false;
    ed->show_outliner = ed->show_details = true;
    ed->right_width = 400.0f;
    ed->outliner_frac = 0.42f;
    ed->drawer_frac = 0.42f;
    ed->tool = TOOL_MOVE;
    ed->content_docked_frac = 0.35f;
    ed->view_mode = VIEW_LIT;
    ed->show_grid = ed->show_icons = true;
    ed->ui_scale = 1.0f, ed->mouse_sens = 0.15f, ed->cam_fov = 90.0f, ed->play_possessed = true, ed->browser_on_start = true;
    ed->less_cpu_in_background = true, ed->autosave_minutes = 10, ed->thumb_size = 96, ed->confirm_quit = true;
    ed->exposure_ev100 = 14.0f; // a sunny day's shade (the "sunny 16" rule gives 15 in full sun)
    ed->exposure_comp = EXPOSURE_COMP_DEFAULT;
    ed->auto_min_ev100 = -4.0f;
    ed->auto_max_ev100 = 18.0f;
    ed->auto_exposure = true;
    ed->aerial_scale = 1.0f;
    ed->scalability = 2; // High: MSAA 4x (Epic 4x too, Cinematic 8x)
    ed->msaa = 4;
    ed->gi = (GiSettings){ true, true, 64, 200.0f, 1.0f, 0.3f, true, true, 4 };
    ed->gi.screen_clouds = true; // (High)
    ed->snap_move = ed->snap_rotate = true;
    ed->snap_move_step = 10.0f;
    ed->gizmo_size = 100.0f; // (2026-11-23: px at 100%, Editor Preferences > Viewport)
    ed->snap_rotate_step = 10.0f;
    ed->snap_scale_step = 0.25f;
    reset_camera(ed);
    default_level(ed);
    char root[600], exe[512];
    if (!re_exe_dir(exe, sizeof exe)) snprintf(exe, sizeof exe, ".");
    snprintf(root, sizeof root, "%s/Content", exe); // the project folder comes with the project system
    content_init(root);
    bp_init(content_root());
    input_init(content_root(), true); // (before anything reads the Blueprints: an Input Action event's value pin takes its type)
    ui_widgets_init(content_root());
    landscape_init(content_root());
    procgen_init(content_root());
    pcg_init(content_root(), true); // (Q15: the PCG Graphs read on demand; the examples written when missing)
    foliage_init(content_root());
    proto_init(content_root());
    level_stream_init(content_root(), ""); // (the Level Blueprints are found under it)
    mat_set_root(content_root()); // the project's materials (found by their paths' hashes)
    seq_init(content_root()); // its Level Sequences
    mat_make_engine_materials(content_root()); // M_Grid and the node examples, when missing
    anim_init(content_root(), true); // the Mannequin, its animations and Anim Blueprint, when missing
    particles_set_root(content_root());
    ikrig_set_root(content_root());
    vehicle_set_root(content_root());
    audio_init(content_root()); // the mixer (its own thread; silent under REALENGINE_BACKGROUND)
    async_init(); // the background tasks (async.h): imports, bakes, the meshes read again (cards at the bottom right)
    g_assist_ed = ed;
    assistant_ui_set_action(assistant_action); // the Assistant's articles' buttons (Help > Assistant, F1)
    script_init(content_root()); // the scripts (their classes: like Blueprints')
    ai_init(content_root(), true); // the AI examples (BT_Chase, Brain_Guard), when missing
    dlg_init(content_root()); // the Dialogues (dialogue.h: read on demand)
    quest_init(content_root()); // the Quests (quest.h: read on demand)
    g_import_ed = ed;
    g_mesh_mat_ed = ed; // (2026-11-23)
    asset_viewer_on_mesh_material(mesh_material_saved); // (2026-11-23)
    content_set_importer("tlend", import_tlend_cb); // the Tlender's scenes become assets
    content_set_importer("obj", import_tlend_cb);   // (and OBJ, glTF files, the same way)
    content_set_importer("gltf", import_tlend_cb);
    content_set_importer("glb", import_tlend_cb);
    content_set_importer("fbx", import_tlend_cb);
    content_set_importer("rasset", import_tlend_cb); // (#164) the exchange format (the Tlender's Export > Real Engine)
    { // sounds become Sound Waves (audio_import.c)
        static const char* const snd[] = { "wav", "mp3", "ogg", "flac", "m4a", "aac", "wma", "opus" };
        for (size_t i = 0; i < sizeof snd / sizeof snd[0]; i++) content_set_importer(snd[i], audio_import_cb);
    }
    content_set_level_hooks(&(ContentLevelHooks){ level_open_cb, level_create_cb, save_all_cb });
    content_set_editor_hooks(&(ContentEditorHooks){ content_actor_cb, content_meshes_cb }); // (Q6: Create > Cloth, Vehicle, LODs)
    editor_load_prefs(ed);
    ed->cam.fov_deg = ed->cam_fov; // (Editor Preferences > Viewport)
    return true;
}

static void set_scalability(Editor* ed, int level, double now);

// --- Preferences (%APPDATA%\RealEngine\editor.ini: language, VSync, scalability) ---

static bool prefs_path(char* out, size_t cap) {
    char dir[512];
    if (!re_config_dir(dir, sizeof dir)) return false;
    snprintf(out, cap, "%s\\editor.ini", dir);
    return true;
}

bool g_editor_prefs_readonly; // the tests (main.c, an automated run): the user's editor.ini is read, never written

void editor_save_prefs(Editor* ed) {
    char path[600];
    if (g_editor_prefs_readonly || !prefs_path(path, sizeof path)) return; // (a test once left the preset on Low there)
    FILE* f = re_fopen(path, "w");
    if (!f) return;
    fprintf(f, "language=%s\nvsync=%d\nscalability=%d\nshow_stats=%d\n", i18n_language_code(i18n_language()),
            ed->vk->vsync ? 1 : 0, ed->saved_scalability >= 0 ? ed->saved_scalability : ed->scalability, ed->show_stats ? 1 : 0);
    // Editor Preferences (project_ui.c)
    fprintf(f, "ui_scale=%.3f\nmouse_sens=%.4f\ncam_fov=%.1f\ninvert_y=%d\nless_cpu=%d\nplay_possessed=%d\nbrowser_on_start=%d\n",
            ed->ui_scale, ed->mouse_sens, ed->cam_fov, ed->invert_y, ed->less_cpu_in_background, ed->play_possessed, ed->browser_on_start);
    fprintf(f, "graph_node_style2=%d\ngraph_link_style2=%d\n", graph_node_style(), graph_link_style()); // (graph_style.h; "2": after Q5's Orbit)
    fprintf(f, "theme=%s\n", theme_current_name()); // (Editor Preferences > Appearance: theme.h)
    fprintf(f, "confirm_quit=%d\nautosave_minutes=%d\nframe_limit=%d\nthumb_size=%d\ncam_speed=%.1f\nshow_grid=%d\nshow_icons=%d\n",
            ed->confirm_quit, ed->autosave_minutes, ed->frame_limit, ed->thumb_size, ed->cam.speed, ed->show_grid, ed->show_icons);
    fprintf(f, "snap_move=%.3f\nsnap_rotate=%.3f\nsnap_scale=%.3f\n", ed->snap_move_step, ed->snap_rotate_step, ed->snap_scale_step);
    fprintf(f, "gizmo_size=%.0f\ngizmo_orient=%d\ngizmo_pivot=%d\ngizmo_only_loc=%d\n", ed->gizmo_size, ed->gizmo_orient, ed->gizmo_pivot, ed->gizmo_only_loc ? 1 : 0);
    async_ui_write_prefs(f); // where the task cards were moved, thin or not
    mcp_write_prefs(f);      // (2026-11-24) the MCP server: on, its port
    asset_picker_write_prefs(f); // (Q6) the asset picker's view and preview size
    assistant_ui_write_prefs(f); // the Assistant's recipes: as nodes or as code (which language)
    look_write_prefs(f); // (Q26) the look seen (the new default offered once), the target platform
    fclose(f);
}

// Before the window's first frame: the language (the system's when there is no file), VSync and the scalability
void editor_load_prefs(Editor* ed) {
    i18n_set_language(re_system_language_portuguese() ? LANG_PT_BR : LANG_EN);
    char path[600], line[256];
    FILE* f = prefs_path(path, sizeof path) ? re_fopen(path, "r") : NULL;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* v = eq + 1;
        if (!strcmp(line, "language")) {
            int l = i18n_language_from_code(v);
            if (l >= 0) i18n_set_language(l);
        } else if (!strcmp(line, "vsync")) {
            ed->vk->vsync = atoi(v) != 0;
            ed->vk->swapchain_dirty = true;
        } else if (!strcmp(line, "scalability")) {
            int s = atoi(v);
            if (s >= 0 && s < RENDER_SCALABILITY_LEVELS) set_scalability(ed, s, 0.0);
        } else if (!strcmp(line, "show_stats")) {
            ed->show_stats = atoi(v) != 0;
        } else if (!strcmp(line, "ui_scale")) ed->ui_scale = fminf(fmaxf((float)atof(v), 0.6f), 2.5f);
        else if (!strcmp(line, "mouse_sens")) ed->mouse_sens = (float)atof(v);
        else if (!strcmp(line, "cam_fov")) ed->cam_fov = fminf(fmaxf((float)atof(v), 30.0f), 150.0f);
        else if (!strcmp(line, "invert_y")) ed->invert_y = atoi(v) != 0;
        else if (!strcmp(line, "less_cpu")) ed->less_cpu_in_background = atoi(v) != 0;
        else if (!strcmp(line, "play_possessed")) ed->play_possessed = atoi(v) != 0;
        else if (!strcmp(line, "browser_on_start")) ed->browser_on_start = atoi(v) != 0;
        else if (!strcmp(line, "graph_node_style")) graph_set_styles_ini(atoi(v), graph_link_style(), false); // (before Q5: the first style moves to Orbit)
        else if (!strcmp(line, "graph_link_style")) graph_set_styles_ini(graph_node_style(), atoi(v), false);
        else if (!strcmp(line, "graph_node_style2")) graph_set_styles(atoi(v), graph_link_style());
        else if (!strcmp(line, "graph_link_style2")) graph_set_styles(graph_node_style(), atoi(v));
        else if (!strcmp(line, "theme")) theme_select_name(v);
        else if (!strcmp(line, "confirm_quit")) ed->confirm_quit = atoi(v) != 0;
        else if (!strcmp(line, "autosave_minutes")) ed->autosave_minutes = atoi(v);
        else if (!strcmp(line, "frame_limit")) ed->frame_limit = atoi(v);
        else if (!strcmp(line, "thumb_size")) ed->thumb_size = atoi(v);
        else if (!strcmp(line, "cam_speed")) ed->cam.speed = fmaxf((float)atof(v), 1.0f);
        else if (!strcmp(line, "show_grid")) ed->show_grid = atoi(v) != 0;
        else if (!strcmp(line, "show_icons")) ed->show_icons = atoi(v) != 0;
        else if (!strcmp(line, "snap_move")) ed->snap_move_step = fmaxf((float)atof(v), 0.01f);
        else if (!strcmp(line, "gizmo_size")) ed->gizmo_size = fminf(fmaxf((float)atof(v), 40.0f), 300.0f); // (2026-11-23)
        else if (!strcmp(line, "gizmo_orient")) ed->gizmo_orient = atoi(v) >= 0 && atoi(v) < GIZMO_ORIENT_COUNT ? atoi(v) : 0;
        else if (!strcmp(line, "gizmo_pivot")) ed->gizmo_pivot = atoi(v) >= 0 && atoi(v) < GIZMO_PIVOT_COUNT ? atoi(v) : 0;
        else if (!strcmp(line, "gizmo_only_loc")) ed->gizmo_only_loc = atoi(v) != 0;
        else if (!strcmp(line, "snap_rotate")) ed->snap_rotate_step = fmaxf((float)atof(v), 0.01f);
        else if (!strcmp(line, "snap_scale")) ed->snap_scale_step = fmaxf((float)atof(v), 0.001f);
        else if (!async_ui_read_pref(line, v) && !asset_picker_read_pref(line, v) && !look_read_pref(line, v) && !mcp_read_pref(line, v)) assistant_ui_read_pref(line, v);
    }
    fclose(f);
    look_after_prefs(ed, true, g_editor_prefs_readonly); // (Q26) an editor.ini from before 0.44: the new look, once
}

void editor_shutdown(Editor* ed) {
    dock_layout_autosave(ed); // (Q2: the layout back the next time; main.c saved it while ImGui lived: here a no-op)
    pie_multi_stop(); // (B13: the other players' windows go with the editor)
    plugins_shutdown(); // (B14c) their shutdowns (the DLLs stay mapped until the process ends)
    async_shutdown(); // (the running work finishes first: it may be writing the project's files)
    audio_shutdown();
    editor_save_prefs(ed);
    bp_editor_save_all(ed);
    bp_shutdown();
    undo_clear(ed);
    ecs_snapshot_free(&ed->snapshot);
    ecs_shutdown(&ed->world);
    free(ed->ol_rows);
    free(ed->sel);
    free(ed->sel_start);
    free(ed->sel_mark);
    free(ed->clip);
}

// --- Play in Editor ---

void editor_set_possessed(Editor* ed, bool possessed) {
    if (!ed->playing) return;
    if (!possessed && ed->possessed) ed->cam = ed->pie_cam; // ejecting keeps the player's view
    ed->possessed = possessed;
    ed->mouse_released = false;
    if (possessed) glfwGetCursorPos(ed->window, &ed->last_cursor[0], &ed->last_cursor[1]);
    glfwSetInputMode(ed->window, GLFW_CURSOR, possessed ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

// The game's options in Play (game_settings.h): the project's Saved/Config/GameUserSettings.ini; the volumes, keys and
// sensitivity applied when Play starts; the preset changed in the options menu applies to the view until Play stops
// (then the editor's own comes back); the window's settings are the packaged game's (not the editor's window)
static Editor* g_gus_ed;
static int g_pie_scalability = -1; // the editor's before the Play's options changed it (-1: unchanged)
static bool g_pie_saved_set;        // (the editor.ini keeps the editor's own while the Play's options change it)
double g_pie_tick_ms;              // the Play's game step, ms (the pause's measure: notas-dev/pausa-opcoes.md)
static void pie_apply_video(const GameUserSettings* s, bool display) {
    (void)display;
    Editor* ed = g_gus_ed;
    if (!ed || !ed->playing || s->scalability == ed->scalability) return;
    if (g_pie_scalability < 0) {
        g_pie_scalability = ed->scalability;
        if (ed->saved_scalability < 0) ed->saved_scalability = ed->scalability, g_pie_saved_set = true; // (never written to editor.ini)
    }
    render_scalability(s->scalability, &ed->msaa, &ed->gi);
    ed->scalability = s->scalability;
}
static void pie_settings_begin(Editor* ed) {
    cvar_bind_view(&ed->gi, &ed->msaa); // (Q16) Set Console Variable's r.* on the editor's view, given back at Stop
    cvar_begin_play();
    char path[1200];
    gus_default_path(NULL, false, path, sizeof path);
    gus_set_path(path);
    gus_set_host(NULL); // (what was saved: applied without the view's preset)
    gus_reset();
    gus_load();
    gus_apply(false);
    g_gus_ed = ed;
    gus_set_host(&(GusHost){ pie_apply_video, false });
    if (project()->pause_menu) pause_menu_assets(); // (WBP_PauseMenu / WBP_Options in Content/UI when missing)
    if (project()->pause_menu) gus_warm(); // (Q22: the Options' Upscaler choices asked now, not in the frame that draws them)
}
static void pie_settings_end(Editor* ed) {
    render_set_style_override(-1); // (a Set Render Style while playing: the project's again)
    { // (Q16) a Build Lighting the play asked for: stopped, the level's own lighting back
        char rl[1100] = "";
        if (ed->level_path[0]) level_rlight_path(ed->level_path, rl, sizeof rl);
        rtl_end_play(ed->vk, rl);
    }
    gus_set_host(NULL);
    if (g_pie_scalability >= 0) {
        render_scalability(g_pie_scalability, &ed->msaa, &ed->gi);
        ed->scalability = g_pie_scalability;
        g_pie_scalability = -1;
        if (g_pie_saved_set) ed->saved_scalability = -1, g_pie_saved_set = false;
    }
    audio_set_master_volume(1.0f); // (the editor's previews at their own volume)
    for (int c = AUDIO_CLASS_MUSIC; c <= AUDIO_CLASS_VOICE; c++) audio_set_user_class_volume(c, 1.0f);
}

void editor_play(Editor* ed, double now) {
    editor_construct_restore(ed); // (the Construction Scripts' work is shown, never kept: sugestÃ£o #64)
    seq_editor_restore(ed); // (the Sequencer's preview is put back: play starts from the level as saved)
    if (ed->playing) return;
    bp_editor_save_all(ed); // Unreal compiles what changed before playing
    double t0 = re_time_seconds();
    if (!ecs_snapshot_save(&ed->world, &ed->snapshot)) {
        editor_status(ed, now, TR("Play failed: no memory for the world's snapshot."));
        return;
    }
    ed->snapshot_ms = (re_time_seconds() - t0) * 1000.0;
    ed->snapshot_bytes = ed->snapshot.bytes;
    ed->cam_before_play = ed->cam;
    ed->pie_cam = ed->cam;
    // The spectator starts at the level's Player Start (eye height), else at the editor's view
    level_player_start(&ed->world, &ed->pie_cam);
    ed->playing = true;
    ed->paused = false;
    ed->play_time = 0.0;
    audio_world_begin_play(&ed->world); // the auto-activated Audio Components start
    ed->renaming = ENTITY_NONE;
    ed->flying = false;
    ed->gizmo_active = 0;
    editor_set_possessed(ed, ed->play_possessed || ed->bp_test); // (Editor Preferences > Play: possessed or ejected)
    editor_status(ed, now, TR("Play In Editor: snapshot %.1f KB in %.3f ms."), ed->snapshot_bytes / 1024.0, ed->snapshot_ms);
    // the level's streaming and its Level Blueprint (a Level Script Actor: gone with the snapshot when play stops)
    {
        const char* base = strrchr(ed->level_path, '/');
        const char* base2 = strrchr(ed->level_path, '\\');
        if (base2 > base) base = base2;
        char lname[128];
        snprintf(lname, sizeof lname, "%s", base ? base + 1 : ed->level_path);
        char* dot = strrchr(lname, '.');
        if (dot) *dot = 0;
        level_stream_init(content_root(), lname);
        bp_set_level_hooks(&(BpLevelHooks){ level_stream_request, level_stream_done, level_open_request, level_stream_persistent });
        level_bp_spawn(&ed->world, ed->level_path);
        levels_play_begin(ed); // the sublevels not Initially Loaded out, the names known to Load Stream Level
    }
    wp_begin(&ed->world, ed->level_path, content_root(), ed->pie_cam.pos); // (B1: what is out of range leaves before its BeginPlay)
    pie_settings_begin(ed);
    bp_begin_play(&ed->world); // the actors' Blueprints: instances, then BeginPlay
    physics_begin_play(&ed->world); // the rigid bodies
    spline_begin_play(&ed->world); // (Q14) the cables from their rest
    vehicle_begin_play(&ed->world); // the vehicles, their rails and waters
    procgen_begin_play(&ed->world); // the procedural world made again as the play's
    pcg_begin_play(&ed->world); // (Q15) the PCG components: kept as the editor made them, On Demand ones cleared
    anim_begin_play(&ed->world); // the skeletal meshes' instances
    ai_begin_play(&ed->world); // the AI agents and the navigation grid
    crowd_begin_play(&ed->world); // the crowds (on that grid)
    traffic_begin_play(&ed->world); // (Q27.2) the roads' cars
    project_begin_play(&ed->world); // the Game Mode's classes and the Default Pawn (Project Settings)
    if (bp_stats()->instances)
        editor_status(ed, now, TR("Play In Editor: %u Blueprint actors (BeginPlay %.3f ms), snapshot %.3f ms."), bp_stats()->instances,
                      bp_stats()->ms_last_frame, ed->snapshot_ms);
}

// Open Level while playing: the systems end, the other level comes in, they start again (Stop still brings back the
// editor's level: the snapshot is the one taken when play began)
static void pie_open_level(Editor* ed, const char* path) {
    wp_end(&ed->world);
    bp_end_play(&ed->world);
    physics_end_play(&ed->world);
    vehicle_end_play(&ed->world);
    procgen_end_play(&ed->world);
    pcg_end_play(&ed->world);
    anim_end_play(&ed->world);
    ai_end_play(&ed->world);
    crowd_end_play(&ed->world);
    traffic_end_play(&ed->world);
    double swap_ms = 0.0; // (2026-11-23) a preloaded level swaps in at once (still loading: finished here at full speed)
    if (level_preload_take(path, &ed->world, NULL, NULL, &swap_ms)) printf("[RealEngine] Open Level: %s was preloaded, swapped in in %.2f ms\n", path, swap_ms);
    else if (!level_load(&ed->world, path, NULL)) {
        printf("[RealEngine] Open Level: %s could not be read\n", path);
        return;
    }
    bp_components_rebuild(&ed->world, NULL);
    ed->selected = ed->renaming = ENTITY_NONE;
    editor_select_none(ed);
    char lp[1100];
    level_rlight_path(path, lp, sizeof lp);
    if (!gi_lightmaps_load(ed->vk, lp)) gi_lightmaps_clear(ed->vk);
    gi_reset_probes();
    const char* base = strrchr(path, '/');
    const char* base2 = strrchr(path, '\\');
    if (base2 > base) base = base2;
    char lname[128];
    snprintf(lname, sizeof lname, "%s", base ? base + 1 : path);
    char* dot = strrchr(lname, '.');
    if (dot) *dot = 0;
    level_stream_init(content_root(), lname);
    level_bp_spawn(&ed->world, path);
    levels_play_open(ed, path); // (its sublevels)
    level_player_start(&ed->world, &ed->pie_cam);
    wp_begin(&ed->world, path, content_root(), ed->pie_cam.pos); // (B1: the other level's grid)
    bp_begin_play(&ed->world);
    physics_begin_play(&ed->world);
    vehicle_begin_play(&ed->world); // the vehicles, their rails and waters
    procgen_begin_play(&ed->world); // the procedural world made again as the play's
    pcg_begin_play(&ed->world); // (Q15) the PCG components: kept as the editor made them, On Demand ones cleared
    anim_begin_play(&ed->world);
    ai_begin_play(&ed->world);
    crowd_begin_play(&ed->world);
    traffic_begin_play(&ed->world);
    project_begin_play(&ed->world);
    printf("[RealEngine] Open Level: %s (%u actors)\n", lname, ed->world.alive);
    online_level_loaded(&ed->world, path); // (in a session: the clients follow)
}

// --- (B13) Play with several players (Unreal's Number of Players, Net Mode: Play As Listen Server): this editor hosts a
// LAN session; each other player is another editor process that opens the same level (saved for it under
// Saved/PIE) and joins it (--pie-client) in a window of its own, closed when Play stops ---
#define PIE_MAX_CLIENTS 3
static int g_pie_players = 1, g_pie_nclients;
static ReProcess* g_pie_child[PIE_MAX_CLIENTS];
static void pie_multi_stop(void) {
    for (int i = 0; i < g_pie_nclients; i++) re_process_kill(g_pie_child[i]), g_pie_child[i] = NULL; // (our own children: the windows this Play opened)
    g_pie_nclients = 0;
}
int editor_pie_clients(void) { return g_pie_nclients; }
void editor_pie_set_players(int n) { g_pie_players = n < 1 ? 1 : n > PIE_MAX_CLIENTS + 1 ? PIE_MAX_CLIENTS + 1 : n; }
void editor_play_multi(Editor* ed, double now) { pie_multi_play(ed, now); }

void editor_simulate(Editor* ed, double now) {
    if (ed->playing) {
        if (ed->simulating) editor_stop(ed, now);
        return;
    }
    bool keep = ed->play_possessed;
    ed->play_possessed = false; // (ejected from the start: the editor's camera, nothing possessed)
    editor_play(ed, now);
    ed->play_possessed = keep;
    if (!ed->playing) return;
    ed->simulating = true;
    editor_status(ed, now, TR("Simulating: the game runs without the player (Esc stops, F8 possesses)."));
}
// (Q25) VR Preview (Unreal's Play > VR Preview): the level as it is now (unsaved changes too) into Saved/PIE with its baked
// lighting, played by the game's executable from the project's folder on the headset (OpenXR; --vr). Its own process:
// the headset's Vulkan extensions and its frame pacing never touch the editor's
static void copy_small_file(const char* from, const char* to) {
    FILE* a = re_fopen(from, "rb");
    if (!a) return;
    FILE* b = re_fopen(to, "wb");
    if (b) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
        fclose(b);
    }
    fclose(a);
}
void editor_vr_preview(Editor* ed, double now) {
    if (!project_file()[0]) {
        editor_status(ed, now, "%s", TR("VR Preview needs a project (File > New Project or Open Project)."));
        return;
    }
    char dir[900], lvl[960], exe[1100], exe_dir[900];
    snprintf(dir, sizeof dir, "%s/../Saved/PIE", content_root());
    re_make_dirs(dir);
    snprintf(lvl, sizeof lvl, "%s/VR_Preview.rlevel", dir);
    LevelCamera cam = { { ed->cam.pos[0], ed->cam.pos[1], ed->cam.pos[2] }, ed->cam.yaw, ed->cam.pitch, ed->cam_fov, ed->cam.speed };
    size_t bytes = 0;
    if (!re_exe_dir(exe_dir, sizeof exe_dir) || !level_save(&ed->world, &cam, lvl, &bytes)) {
        editor_status(ed, now, "%s", TR("VR Preview: the level could not be written."));
        return;
    }
    if (ed->level_path[0]) { // (its baked lighting beside it)
        char a[1100], b[1100];
        level_rlight_path(ed->level_path, a, sizeof a), level_rlight_path(lvl, b, sizeof b);
        copy_small_file(a, b);
    }
    snprintf(exe, sizeof exe, "%s/rivora_game.exe", exe_dir);
    char cmd[3600];
    snprintf(cmd, sizeof cmd, "\"%s\" --windowed --vr --project \"%s\" --level \"%s\"", exe, project_file(), lvl);
    if (!re_process_start(cmd)) {
        editor_status(ed, now, "%s", TR("VR Preview: the game's executable could not start."));
        return;
    }
    editor_status(ed, now, "%s", TR("VR Preview: the level on the headset (put it on; no headset: the monitor). Close its window to stop."));
}

static void pie_multi_play(Editor* ed, double now) {
    if (g_pie_players <= 1) {
        editor_play(ed, now);
        return;
    }
    char dir[900], lvl[960], exe[1024];
    snprintf(dir, sizeof dir, "%s/../Saved/PIE", content_root());
    re_make_dirs(dir);
    snprintf(lvl, sizeof lvl, "%s/PIE_Level.rlevel", dir);
    LevelCamera cam = { { ed->cam.pos[0], ed->cam.pos[1], ed->cam.pos[2] }, ed->cam.yaw, ed->cam.pitch, ed->cam_fov, ed->cam.speed };
    size_t bytes = 0;
    char exe_dir[900];
    bool have_exe = re_exe_dir(exe_dir, sizeof exe_dir);
    if (have_exe) snprintf(exe, sizeof exe, "%s/rivora.exe", exe_dir);
    if (!level_save(&ed->world, &cam, lvl, &bytes) || !have_exe) {
        editor_status(ed, now, "%s", TR("Play: the level for the other players could not be written."));
        editor_play(ed, now);
        return;
    }
    editor_play(ed, now);
    if (!online_host(&ed->world, "PIE", g_pie_players, true)) {
        editor_status(ed, now, "%s", TR("Play: the session could not be hosted (the port taken?): playing alone."));
        return;
    }
    const char* bg = getenv("REALENGINE_BACKGROUND");
    for (int i = 0; i < g_pie_players - 1 && i < PIE_MAX_CLIENTS; i++) {
        char cmd[3200];
        snprintf(cmd, sizeof cmd, "\"%s\" %s%s%s --open \"%s\" --play --pie-client 127.0.0.1 --pie-window %d%s", exe, project_file()[0] ? "--project \"" : "",
                 project_file()[0] ? project_file() : "", project_file()[0] ? "\"" : "", lvl, i + 1, bg ? " --bench 30" : "");
        if ((g_pie_child[g_pie_nclients] = re_process_start(cmd)) != NULL) g_pie_nclients++;
    }
    editor_status(ed, now, TR("Play: listen server, %d other players joining in their windows."), g_pie_nclients);
}

void editor_stop(Editor* ed, double now) {
    editor_construct_forget(); // (another world: the Construction Scripts run on it anew)
    ed->simulating = false;
    if (!ed->playing) return;
    pie_multi_stop(); // (B13: the other players' windows close with the play)
    wp_end(&ed->world); // (B1: the kept actors dropped; the snapshot brings the level back whole)
    online_end_play(&ed->world); // (the session closes with Play)
    bp_end_play(&ed->world);
    progress_session_end(); // (save_progress.h: the play time, the checkpoint, the Save Game object forgotten)
    physics_end_play(&ed->world);
    spline_end_play(&ed->world); // (Q14)
    vehicle_end_play(&ed->world);
    procgen_end_play(&ed->world);
    pcg_end_play(&ed->world);
    anim_end_play(&ed->world);
    ai_end_play(&ed->world);
    crowd_end_play(&ed->world);
    traffic_end_play(&ed->world);
    audio_world_end_play(&ed->world); // (the game's sounds stop; the editor's previews go on)
    pie_settings_end(ed);
    double t0 = re_time_seconds();
    ecs_snapshot_restore(&ed->world, &ed->snapshot);
    ed->restore_ms = (re_time_seconds() - t0) * 1000.0;
    levels_play_end(ed); // (the editor's sublevels and their lighting again)
    ed->playing = ed->paused = ed->possessed = ed->mouse_released = false;
    ed->cam = ed->cam_before_play;
    glfwSetInputMode(ed->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    if (!ecs_alive(&ed->world, ed->selected)) ed->selected = ENTITY_NONE;
    editor_status(ed, now, TR("Stopped after %.1f s: world restored in %.3f ms."), ed->play_time, ed->restore_ms);
}

// --- Actions ---

// --- The selection: several actors, ed->selected the primary one (the last picked: Details, the gizmo's pivot) ---

bool editor_is_selected(const Editor* ed, Entity e) {
    if (e == ENTITY_NONE || !ed->sel_mark) return e != ENTITY_NONE && e == ed->selected;
    if (!ed->sel_mark[entity_index(e)]) return false;
    for (uint32_t i = ed->nsel; i-- > 0;) // marks are per slot: the entity's generation must match too
        if (ed->sel[i] == e) return true;
    return false;
}

static void sel_remove_at(Editor* ed, uint32_t i) {
    ed->sel_mark[entity_index(ed->sel[i])] = 0;
    memmove(&ed->sel[i], &ed->sel[i + 1], sizeof(Entity) * (ed->nsel - i - 1));
    ed->nsel--;
}

void editor_select_none(Editor* ed) {
    for (uint32_t i = 0; i < ed->nsel; i++) ed->sel_mark[entity_index(ed->sel[i])] = 0;
    ed->nsel = 0;
    ed->selected = ENTITY_NONE;
}

void editor_select(Editor* ed, Entity e, SelMode mode) {
    if (mode == SEL_REPLACE) editor_select_none(ed);
    if (e == ENTITY_NONE || !ecs_alive(&ed->world, e) || (mode != SEL_REMOVE && levels_locked(ed, e))) { // (a locked level: not picked)
        ed->selected = ed->nsel ? ed->sel[ed->nsel - 1] : ENTITY_NONE;
        return;
    }
    bool have = editor_is_selected(ed, e);
    if (have && (mode == SEL_TOGGLE || mode == SEL_REMOVE)) {
        for (uint32_t i = 0; i < ed->nsel; i++)
            if (ed->sel[i] == e) { sel_remove_at(ed, i); break; }
    } else if (!have && mode != SEL_REMOVE && ed->nsel < ED_MAX_SEL) {
        ed->sel[ed->nsel++] = e;
        ed->sel_mark[entity_index(e)] = 1;
    } else if (have) { // already in: it becomes the primary one (moved to the end)
        for (uint32_t i = 0; i < ed->nsel; i++)
            if (ed->sel[i] == e) { sel_remove_at(ed, i); break; }
        ed->sel[ed->nsel++] = e;
        ed->sel_mark[entity_index(e)] = 1;
    }
    ed->selected = ed->nsel ? ed->sel[ed->nsel - 1] : ENTITY_NONE;
}

// Ctrl+A: every actor the editor shows (not the hidden ones)
void editor_select_all(Editor* ed) {
    editor_select_none(ed);
    for (uint32_t i = 1; i < ed->world.slot_count && ed->nsel < ED_MAX_SEL; i++) {
        Entity e = ecs_entity_at(&ed->world, i);
        if (e == ENTITY_NONE) continue;
        const CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        if ((t && (t->flags & TRANSFORM_HIDDEN)) || bp_is_component(&ed->world, e)) continue;
        ed->sel[ed->nsel++] = e;
        ed->sel_mark[i] = 1;
    }
    ed->selected = ed->nsel ? ed->sel[ed->nsel - 1] : ENTITY_NONE;
}

// Ctrl+I: the shown actors that were not selected
static void invert_selection(Editor* ed) {
    uint8_t* was = calloc(ed->world.slot_count, 1); // an editor action, not a frame
    if (!was) return;
    for (uint32_t i = 0; i < ed->nsel; i++) was[entity_index(ed->sel[i])] = 1;
    editor_select_all(ed);
    for (uint32_t i = 0; i < ed->nsel;)
        if (was[entity_index(ed->sel[i])]) sel_remove_at(ed, i);
        else i++;
    ed->selected = ed->nsel ? ed->sel[ed->nsel - 1] : ENTITY_NONE;
    free(was);
}

// Code that sets ed->selected alone (a spawn, an Outliner click, undo) gets a selection of just that; dead actors leave
void editor_sync_selection(Editor* ed) {
    for (uint32_t i = 0; i < ed->nsel;)
        if (!ecs_alive(&ed->world, ed->sel[i])) sel_remove_at(ed, i);
        else i++;
    if (ed->selected != ENTITY_NONE && !ecs_alive(&ed->world, ed->selected)) ed->selected = ENTITY_NONE;
    if (ed->selected == ENTITY_NONE) {
        if (ed->nsel) editor_select_none(ed);
    } else if (!editor_is_selected(ed, ed->selected)) {
        Entity e = ed->selected;
        editor_select(ed, e, SEL_REPLACE);
    }
}

// A copy of e's name made unique ("Cube2" -> "Cube3"...)
static void fresh_name(Editor* ed, Entity e) {
    CName* name = ecs_get(&ed->world, e, COMP_NAME);
    if (!name) return;
    char base[RE_NAME_MAX], fresh[RE_NAME_MAX];
    snprintf(base, sizeof base, "%s", name->text);
    size_t len = strlen(base);
    while (len > 0 && isdigit((unsigned char)base[len - 1])) len--;
    base[len] = 0;
    editor_unique_name(ed, base[0] ? base : "Actor", fresh, sizeof fresh);
    snprintf(name->text, sizeof name->text, "%s", fresh);
}

static void delete_selected(Editor* ed) {
    if (!ed->nsel) return;
    editor_undo_push(ed);
    for (uint32_t i = 0; i < ed->nsel; i++)
        if (ecs_alive(&ed->world, ed->sel[i])) ecs_destroy(&ed->world, ed->sel[i]);
    editor_select_none(ed);
    ed->renaming = ENTITY_NONE;
}

// Ctrl+D / Shift+D: copies of every selected actor, which become the selection (offset by 'offset' cm along Y)
static void duplicate_selected(Editor* ed, double now, float offset) {
    if (!ed->nsel) return;
    editor_undo_push(ed);
    uint32_t n = ed->nsel;
    Entity* src = malloc(sizeof(Entity) * n); // an editor action, not a frame
    if (!src) return;
    memcpy(src, ed->sel, sizeof(Entity) * n);
    editor_select_none(ed);
    for (uint32_t i = 0; i < n; i++) {
        Entity c = ecs_clone(&ed->world, src[i]);
        if (c == ENTITY_NONE) continue;
        fresh_name(ed, c);
        CTransform* t = ecs_get(&ed->world, c, COMP_TRANSFORM);
        if (t) t->location[1] += offset;
        CSaveable* sv = ecs_get(&ed->world, c, COMP_SAVEABLE);
        if (sv && sv->id) sv->id = progress_new_id(); // (a key of its own in the saves: save_progress.h)
        if (ecs_get(&ed->world, c, COMP_BLUEPRINT)) bp_components_build(&ed->world, c, NULL); // its own components
        editor_select(ed, c, SEL_ADD);
    }
    free(src);
    editor_status(ed, now, n > 1 ? TR("Duplicated %u actors.") : TR("Duplicated."), n);
}

// Ctrl+C: the selected actors' components into the clipboard (their mask, then each component's bytes)
static void copy_selected(Editor* ed, double now) {
    if (!ed->nsel) return;
    size_t len = 0;
    for (uint32_t i = 0; i < ed->nsel; i++) {
        uint64_t m = ecs_mask(&ed->world, ed->sel[i]);
        len += sizeof m;
        for (int c = 0; c < COMP_COUNT; c++)
            if (m & COMP_BIT(c)) len += g_components[c].size;
    }
    uint8_t* buf = malloc(len);
    if (!buf) return;
    size_t at = 0;
    for (uint32_t i = 0; i < ed->nsel; i++) {
        uint64_t m = ecs_mask(&ed->world, ed->sel[i]);
        memcpy(buf + at, &m, sizeof m);
        at += sizeof m;
        for (int c = 0; c < COMP_COUNT; c++)
            if (m & COMP_BIT(c)) {
                memcpy(buf + at, ecs_get(&ed->world, ed->sel[i], (ComponentId)c), g_components[c].size);
                at += g_components[c].size;
            }
    }
    free(ed->clip);
    ed->clip = buf;
    ed->clip_len = len;
    ed->clip_count = ed->nsel;
    editor_status(ed, now, TR("Copied %u actors."), ed->nsel);
}

// Ctrl+V: the clipboard's actors again, where they were (like Unreal), as the new selection
static void paste_clipboard(Editor* ed, double now) {
    if (!ed->clip_count) return;
    editor_undo_push(ed);
    editor_select_none(ed);
    size_t at = 0;
    for (uint32_t i = 0; i < ed->clip_count && at + sizeof(uint64_t) <= ed->clip_len; i++) {
        uint64_t m;
        memcpy(&m, ed->clip + at, sizeof m);
        at += sizeof m;
        Entity e = ecs_create(&ed->world, m);
        for (int c = 0; c < COMP_COUNT; c++)
            if (m & COMP_BIT(c)) {
                if (e != ENTITY_NONE) memcpy(ecs_get(&ed->world, e, (ComponentId)c), ed->clip + at, g_components[c].size);
                at += g_components[c].size;
            }
        if (e == ENTITY_NONE) continue;
        fresh_name(ed, e);
        editor_select(ed, e, SEL_ADD);
    }
    editor_status(ed, now, TR("Pasted %u actors."), ed->nsel);
}

static void begin_rename(Editor* ed, Entity e) {
    const CName* n = ecs_get(&ed->world, e, COMP_NAME);
    if (!n) return;
    ed->renaming = e;
    snprintf(ed->rename_buf, sizeof ed->rename_buf, "%s", n->text);
    ed->rename_focus = true;
}

// Unreal's End: down until the actor's bounds touch the ground (z = 0)
static void snap_to_floor(Editor* ed) {
    CTransform* t = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    if (!t) return;
    editor_undo_push(ed);
    const CStaticMesh* m = ecs_get(&ed->world, ed->selected, COMP_STATIC_MESH);
    if (!m) {
        t->location[2] = 0.0f;
        return;
    }
    float mn[3], mx[3], rows[3][4];
    render_mesh_bounds(m->mesh, mn, mx);
    render_transform_rows(t, rows);
    float lowest = 0.0f; // the lowest corner below the pivot
    for (int c = 0; c < 8; c++) {
        float p[3] = { c & 1 ? mx[0] : mn[0], c & 2 ? mx[1] : mn[1], c & 4 ? mx[2] : mn[2] };
        float z = rows[2][0] * p[0] + rows[2][1] * p[1] + rows[2][2] * p[2];
        if (c == 0 || z < lowest) lowest = z;
    }
    t->location[2] = -lowest;
}

static void toggle_fullscreen(Editor* ed) {
    ed->fullscreen = !ed->fullscreen;
    if (ed->fullscreen) {
        glfwGetWindowPos(ed->window, &ed->windowed_rect[0], &ed->windowed_rect[1]);
        glfwGetWindowSize(ed->window, &ed->windowed_rect[2], &ed->windowed_rect[3]);
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(mon);
        glfwSetWindowMonitor(ed->window, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(ed->window, NULL, ed->windowed_rect[0], ed->windowed_rect[1], ed->windowed_rect[2],
                             ed->windowed_rect[3], 0);
    }
}

static uint64_t file_size(const char* path);
static bool copy_over(const char* src, const char* dst);
// Every .rlevel under the project's Content (with its .rlight and .rlevelbp) copied to the same place in a package
typedef struct PkgWalk { char src[1100], dst[1300]; uint64_t bytes; int depth; } PkgWalk;
static void pkg_one(void* user, const ReDirEntry* e) {
    PkgWalk* p = user;
    if (e->name[0] == '.') return;
    if (e->is_dir) {
        if (p->depth >= 12) return;
        PkgWalk sub = { "", "", 0, p->depth + 1 };
        snprintf(sub.src, sizeof sub.src, "%s\\%s", p->src, e->name);
        snprintf(sub.dst, sizeof sub.dst, "%s\\%s", p->dst, e->name);
        re_dir_walk(sub.src, pkg_one, &sub);
        p->bytes += sub.bytes;
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (!dot || (_stricmp(dot, ".rlevel") && _stricmp(dot, ".rlight") && _stricmp(dot, ".rlevelbp") && _stricmp(dot, ".rlevels"))) return;
    char s[1400], d[1500];
    snprintf(s, sizeof s, "%s\\%s", p->src, e->name);
    snprintf(d, sizeof d, "%s\\%s", p->dst, e->name);
    re_make_dirs(p->dst);
    if (copy_over(s, d)) p->bytes += file_size(d);
}
static uint64_t package_levels(const char* content_src, const char* content_dst) {
    PkgWalk w = { "", "", 0, 0 };
    snprintf(w.src, sizeof w.src, "%s", content_src);
    snprintf(w.dst, sizeof w.dst, "%s", content_dst);
    re_dir_walk(w.src, pkg_one, &w);
    return w.bytes;
}

static void new_level(Editor* ed, double now) {
    editor_construct_forget(); // (another world: the Construction Scripts run on it anew)
    if (ed->playing) editor_stop(ed, now);
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ed->renaming = ENTITY_NONE;
    undo_clear(ed);
    default_level(ed);
    reset_camera(ed);
    gi_lightmaps_clear(ed->vk);
    gi_reset_probes();
    ed->level_path[0] = 0;
    levels_on_new(ed);
    ed->dirty = false;
    editor_status(ed, now, TR("New level."));
}

static void set_vsync(Editor* ed, bool on, double now) {
    g_prefs_dirty = true;
    ed->vk->vsync = on;
    ed->vk->swapchain_dirty = true;
    editor_status(ed, now, on ? TR("VSync on.") : TR("VSync off (mailbox / immediate)."));
}

// Unreal's Engine Scalability: today the anti-aliasing; the GI and shadows join with their settings
static void set_scalability(Editor* ed, int level, double now) {
    g_prefs_dirty = true;
    render_scalability(level, &ed->msaa, &ed->gi);
    ed->scalability = level;
    editor_status(ed, now, TR("Scalability: %s (MSAA %dx)."), TR(render_scalability_name(level)), ed->msaa);
}

void editor_set_vsync(Editor* ed, bool on, double now) { set_vsync(ed, on, now); }
void editor_set_scalability(Editor* ed, int level, double now) {
    set_scalability(ed, level < 0 ? 0 : level >= RENDER_SCALABILITY_LEVELS ? RENDER_SCALABILITY_LEVELS - 1 : level, now);
}
void editor_prefs_changed(void) { g_prefs_dirty = true; }

// --- Build Lighting ---

// the bake's card (async.h): its progress, its Cancel; it runs a slice per frame itself (gi_bake.c)
static float bake_poll(void* arg, char* st, int cap) {
    (void)arg;
    if (!gi_baking()) return -1.0f;
    snprintf(st, (size_t)cap, "%s", TR("Tracing the indirect light of the Static meshes"));
    return gi_bake_progress();
}
static void bake_cancel(void* arg) {
    (void)arg;
    gi_bake_cancel();
}

// Build > Build HLODs (B1, world_partition.c): the regions' merged meshes written, the meshes read again (in the
// background) so the next play shows them
static void build_hlods(Editor* ed, double now) {
    char lname[128], msg[256];
    wp_level_name(ed->level_path, lname, sizeof lname);
    bool ok = wp_build_hlods(&ed->world, content_root(), lname, msg, sizeof msg);
    content_mark_dirty();
    render_meshes_reload_async(ed->vk);
    editor_status(ed, now, ok ? TR("Build HLODs: %s.") : TR("Build HLODs: nothing made (%s)."), msg);
}

bool editor_bake_start(Editor* ed, int q, double now) {
    char why[128];
    if (gi_bake_start(ed->vk, &ed->world, (BakeQuality)q, why, sizeof why)) {
        editor_status(ed, now, TR("Building lighting..."));
        async_track(TR("Building lighting"), bake_poll, bake_cancel, NULL, 0);
        return true;
    }
    editor_status(ed, now, TR("Could not build the lighting: %s"), why);
    return false;
}
static void build_lighting(Editor* ed, BakeQuality q, double now) {
    if (levels_build_lighting(ed, (int)q, now)) return; // (Lighting Levels: one bake each, summed)
    editor_bake_start(ed, (int)q, now);
}

// (Q16) the game's Build Lighting while playing (runtime_lighting.h): started and ended by rtl_frame, its card here
static float game_bake_poll(void* arg, char* st, int cap) {
    (void)arg;
    if (!rtl_busy()) return -1.0f;
    snprintf(st, (size_t)cap, "%s", rtl_blocking() ? TR("The game waits behind its loading screen") : TR("The dynamic GI lights the level meanwhile"));
    return rtl_progress();
}
static void game_bake_cancel(void* arg) {
    (void)arg;
    rtl_cancel();
}
static void game_bake_tick(Editor* ed) {
    static bool s_card;
    rtl_frame(ed->vk, &ed->world);
    if (rtl_busy() && !s_card) async_track(TR("Building lighting (game)"), game_bake_poll, game_bake_cancel, NULL, 0);
    s_card = rtl_busy();
}

// Each frame: a bake that just ended is saved with the level (when the level has a file)
static void bake_tick(Editor* ed, double now) {
    if (rtl_owns_bake()) return; // (Q16: the game's bake, not the level's: never saved as its lighting)
    if (!gi_bake_take_finished()) return;
    if (levels_bake_finished(ed, now)) return; // (Lighting Levels: saved to its level, the next one started)
    if (ed->level_path[0]) {
        char p[1100];
        level_rlight_path(ed->level_path, p, sizeof p);
        if (gi_lightmaps_save(ed->vk, p)) editor_status(ed, now, TR("Lighting built and saved: %s"), p);
        else editor_status(ed, now, TR("Lighting built, but %s could not be written."), p);
    } else {
        editor_status(ed, now, TR("Lighting built. Save the level to keep it."));
    }
}

// --- Package Game ---

static uint64_t file_size(const char* path) {
    uint64_t size = 0;
    bool dir = false;
    return re_file_stat(path, &size, NULL, &dir) && !dir ? size : 0;
}

// A package made again replaces its files (re_copy_file keeps an existing one)
static bool copy_over(const char* src, const char* dst) {
    re_remove_file(dst);
    return re_copy_file(src, dst);
}

// Build > Package Game: a folder ready to ship. The game's executable (realengine_game.exe, named after the level),
// the level and its baked lighting in Content/, game.ini (the title, the start level, the scalability) and the C
// runtime's DLLs when the editor has them next to it. The level is saved first (the package is what the file has).
// Every file of these kinds under Content into the package's Content (same folders)
typedef struct PackWalk { const char* from_root; const char* to_root; uint64_t total; } PackWalk;
static void pack_dir(PackWalk* p, const char* rel, int depth);
static void pack_entry(void* user, const ReDirEntry* e) {
    PackWalk* p = ((void**)user)[0];
    const char* rel = ((void**)user)[1];
    char sub[1200];
    snprintf(sub, sizeof sub, "%s/%s", rel, e->name);
    if (e->is_dir) {
        pack_dir(p, sub, 1);
        return;
    }
    static const char* const exts[] = { ".rmesh", ".rskm", ".ranim", ".rmontage", ".rbt", ".rbrain", ".rbp", ".rstruct",
                                        ".renum", ".rinterface", ".rdata", ".rmat", ".rmi", ".rland", ".rfol", ".rseq",
                                        // the sounds (audio.h): Sound Waves and the audio assets
                                        ".rsound", ".rscue", ".rsatt", ".rsclass", ".rsmix", ".rsconc", ".rsubmix",
                                        // the scripts (script.h): compiled when the game starts
                                        ".real", ".cs", ".py", ".c", ".rdlg", ".rquest", ".rphysmat", ".rshake", ".rpcg" }; // (.rdlg: Dialogues; .rquest: Quests; .rland: the sculpted heights; .rfol: the painted foliage; .rseq: Level Sequences)
    const char* dot = strrchr(e->name, '.');
    bool want = false;
    for (size_t i = 0; dot && i < sizeof exts / sizeof exts[0]; i++) want |= !_stricmp(dot, exts[i]);
    if (!want) return;
    char from[1300], to[1300], folder[1300];
    snprintf(from, sizeof from, "%s%s", p->from_root, sub);
    snprintf(to, sizeof to, "%s%s", p->to_root, sub);
    snprintf(folder, sizeof folder, "%s", to);
    char* cut = strrchr(folder, '/');
    if (cut) *cut = 0, re_make_dirs(folder);
    if (copy_over(from, to)) p->total += file_size(to);
}
static void pack_dir(PackWalk* p, const char* rel, int depth) {
    if (depth > 12) return;
    char dir[1300];
    snprintf(dir, sizeof dir, "%s%s", p->from_root, rel);
    void* user[2] = { p, (void*)rel };
    re_dir_walk(dir, pack_entry, user);
}
static uint64_t package_assets(const char* content_from, const char* content_to) {
    PackWalk p = { content_from, content_to, 0 };
    pack_dir(&p, "", 0);
    return p.total;
}

// The package in two parts: what reads the editor's state (the level saved, the assets' list, game.ini) on the main
// thread, then the files copied by a worker (async.h: a card with its progress and a Cancel; the editor keeps going)
enum { PK_REQUIRED, PK_OPTIONAL, PK_OR_REMOVE }; // (missing: fails; skipped; the old copy removed)
typedef struct PkCopy { char from[1100], to[1300]; uint8_t kind; } PkCopy;
typedef struct PackJob {
    Editor* ed;
    bool show, ok, identity, android, play;
    char dir[1200], content[1300], croot[1100], name[128], exe[1400], fail[1500];
    bool pack; // (B15a) Shipping on the PC: the Content folder into one Content.rpak
    char apk[1400], apk_cmd[4096], apk_log[1400]; // (Package for Android: the APK, the packager's command and its log)
    char icon[1200], product[256], company[256], version[64], description[600], copyright[300], file[160];
    PkCopy* c;
    int n, cap;
    uint64_t total;
} PackJob;

static void pk_add(PackJob* j, const char* from, const char* to, uint8_t kind) {
    if (j->n == j->cap) {
        int cap = j->cap ? j->cap * 2 : 256;
        PkCopy* c = realloc(j->c, sizeof *c * (size_t)cap);
        if (!c) return;
        j->c = c, j->cap = cap;
    }
    PkCopy* c = &j->c[j->n++];
    snprintf(c->from, sizeof c->from, "%s", from);
    snprintf(c->to, sizeof c->to, "%s", to);
    c->kind = kind;
}

static void pk_free(PackJob* j) {
    if (!j) return;
    free(j->c);
    free(j);
}

static PackJob* package_prepare(Editor* ed, const char* out, bool show, double now) {
    if (ed->playing) {
        editor_status(ed, now, TR("Stop playing before packaging."));
        return NULL;
    }
    if ((!ed->level_path[0] || ed->dirty) && !editor_save_level(ed, ed->level_path, now)) return NULL;
    const ProjectSettings* ps = project();
    if (ps->pause_menu) pause_menu_assets(); // (WBP_PauseMenu / WBP_Options: Content/UI goes with the game)
    if (!out || !out[0]) out = ps->package_dir; // (the Package Game window's Output Folder)
    if (!out || !out[0]) {
        editor_status(ed, now, TR("Choose the output folder first."));
        return NULL;
    }
    PackJob* j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->ed = ed, j->show = show;
    // the game's name: Package Game's Game Name, else the level's file name; kept to what every file system takes
    const char* base = ed->level_path + strlen(ed->level_path);
    while (base > ed->level_path && base[-1] != '/' && base[-1] != '\\') base--;
    if (ps->game_name[0]) base = ps->game_name;
    char* name = j->name;
    size_t k = 0;
    for (const char* c = base; *c && (ps->game_name[0] || *c != '.') && k + 1 < sizeof j->name; c++)
        name[k++] = (isalnum((unsigned char)*c) || *c == '-' || *c == '_' || *c == ' ') ? *c : '_';
    name[k] = 0;
    if (!name[0]) snprintf(j->name, sizeof j->name, "Game");
    char exe_dir[1024], src[1300], dst[1400];
    snprintf(j->dir, sizeof j->dir, "%s\\%s", out, name);
    snprintf(j->content, sizeof j->content, "%s\\Content", j->dir);
    const char* dir = j->dir;
    const char* content = j->content;
    if (!re_exe_dir(exe_dir, sizeof exe_dir) || !re_make_dirs(content)) {
        editor_status(ed, now, TR("Could not create %s."), content);
        pk_free(j);
        return NULL;
    }
    snprintf(src, sizeof src, "%s\\rivora_game.exe", exe_dir);
    snprintf(j->exe, sizeof j->exe, "%s\\%s.exe", dir, name);
    pk_add(j, src, j->exe, PK_REQUIRED);
    // its identity: the icon (Explorer, the taskbar, the window) and the version (the file's Properties)
    if (ps->icon[0] && !strncmp(ps->icon, "/Content", 8)) snprintf(j->icon, sizeof j->icon, "%s%s", content_root(), ps->icon + 8);
    else snprintf(j->icon, sizeof j->icon, "%s", ps->icon);
    snprintf(j->product, sizeof j->product, "%s", ps->name[0] ? ps->name : name);
    snprintf(j->company, sizeof j->company, "%s", ps->company);
    snprintf(j->version, sizeof j->version, "%s", ps->version[0] ? ps->version : "1.0");
    snprintf(j->description, sizeof j->description, "%s", ps->description);
    snprintf(j->copyright, sizeof j->copyright, "%s", ps->copyright);
    snprintf(j->file, sizeof j->file, "%s.exe", name);
    j->identity = true;
    j->pack = (ps->shipping || getenv("REALENGINE_PACK")) && !getenv("REALENGINE_NO_PACK");
    snprintf(dst, sizeof dst, "%s\\%s.rlevel", content, name);
    pk_add(j, ed->level_path, dst, PK_REQUIRED);
    char light[1100];
    level_rlight_path(ed->level_path, light, sizeof light);
    snprintf(dst, sizeof dst, "%s\\%s.rlight", content, name);
    pk_add(j, light, dst, PK_OR_REMOVE); // (no baked lighting now: none left from an older package)
    char lbp[1100]; // its Level Blueprint, beside it as the game looks for it
    level_bp_path(ed->level_path, lbp, sizeof lbp);
    snprintf(dst, sizeof dst, "%s\\%s.rlevelbp", content, name);
    pk_add(j, lbp, dst, PK_OR_REMOVE);
    char sub[1100]; // its sublevels' list (the sublevels themselves: the project's other levels, below)
    sublevels_list_path(ed->level_path, sub, sizeof sub);
    snprintf(dst, sizeof dst, "%s\\%s.rlevels", content, name);
    pk_add(j, sub, dst, PK_OR_REMOVE);
    // (the project's other levels and the assets the systems load by their hash: walked by the worker, package_run)
    // the Blueprints (every asset of the project: a class names its parents, structures, interfaces by name), at the
    // same place under Content (their hashes are of that path)
    const char* croot = content_root();
    snprintf(j->croot, sizeof j->croot, "%s", croot);
    size_t crl = strlen(croot);
    for (uint32_t i = 0; i < bp_asset_count(); i++) {
        const BpAsset* a = bp_asset_at(i);
        if (strncmp(a->path, croot, crl)) continue;
        snprintf(dst, sizeof dst, "%s%s", content, a->path + crl);
        pk_add(j, a->path, dst, PK_REQUIRED);
    }
    // the materials and their images under Content (found by the hashes of their paths)
    for (uint32_t i = 0; i < mat_count(); i++) {
        const Material* m = mat_at(i);
        const char* files[1 + MAT_MAX_NODES];
        int nf = 0;
        files[nf++] = m->path;
        for (int q = 0; m->graph && q < m->graph->node_count && nf < 1 + MAT_MAX_NODES; q++)
            if (m->graph->nodes[q].type == NODE_TEX_IMAGE && m->graph->nodes[q].image[0]) files[nf++] = m->graph->nodes[q].image;
        for (int q = 0; q < nf; q++) {
            char srcp[1100];
            if (!strncmp(files[q], "/Content", 8)) snprintf(srcp, sizeof srcp, "%s%s", croot, files[q] + 8);
            else snprintf(srcp, sizeof srcp, "%s", files[q]);
            for (char* c = srcp; *c; c++)
                if (*c == '/') *c = '\\';
            char rootb[1100];
            snprintf(rootb, sizeof rootb, "%s", croot);
            for (char* c = rootb; *c; c++)
                if (*c == '/') *c = '\\';
            size_t rl = strlen(rootb);
            if (_strnicmp(srcp, rootb, rl)) continue; // an image outside the project: left where it is
            snprintf(dst, sizeof dst, "%s%s", content, srcp + rl);
            pk_add(j, srcp, dst, PK_OPTIONAL);
        }
    }
    static const char* const dlls[3] = { "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll" };
    for (int i = 0; i < 3; i++) {
        snprintf(src, sizeof src, "%s\\%s", exe_dir, dlls[i]);
        snprintf(dst, sizeof dst, "%s\\%s", dir, dlls[i]);
        pk_add(j, src, dst, PK_OPTIONAL);
    }
    { // (2026-11-23, the open source release) the engine's license and the third-party notices beside every game: what
      // the MIT License and the libraries' licenses ask of whoever ships their code (the editor's package has them)
        static const char* const lic[2][2] = { { "LICENSE", "Rivora_LICENSE.txt" }, { "THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.md" } };
        for (int i = 0; i < 2; i++) {
            snprintf(src, sizeof src, "%s\\%s", exe_dir, lic[i][0]);
            if (!re_file_stat(src, NULL, NULL, NULL)) snprintf(src, sizeof src, "%s\\..\\..\\%s", exe_dir, lic[i][0]); // (a build: the repository's root)
            snprintf(dst, sizeof dst, "%s\\%s", dir, lic[i][1]);
            pk_add(j, src, dst, PK_OPTIONAL);
        }
    }
    { // the project's upscaler (upscale_vendor.c: its runtime beside the game, its license with it; FSR, DLSS, XeSS)
        static const char* const ups[UPS_COUNT][2] = { { NULL, NULL }, { "amd_fidelityfx_vk.dll", "amd_fidelityfx_LICENSE.txt" },
                                                       { "nvngx_dlss.dll", "nvngx_dlss_LICENSE.txt" }, { "libxess.dll", "libxess_LICENSE.txt" } };
        int k = project_file()[0] ? ps->upscaler : 0;
        for (int i = 0; k > UPS_TSR && k < UPS_COUNT && i < 2; i++) {
            snprintf(src, sizeof src, "%s\\%s", exe_dir, ups[k][i]);
            snprintf(dst, sizeof dst, "%s\\%s", dir, ups[k][i]);
            pk_add(j, src, dst, PK_OPTIONAL);
        }
    }
    snprintf(dst, sizeof dst, "%s\\game.ini", dir);
    FILE* f = re_fopen(dst, "w");
    if (!f) {
        editor_status(ed, now, TR("Packaging failed: could not write %s."), dst);
        pk_free(j);
        return NULL;
    }
    // the project's game settings (Package Game, Project Settings > Platforms, Rendering): the window, the scalability;
    // the start level: Game Default Map when it is one of the project's (packaged where it is under Content), else this one
    char start[600];
    snprintf(start, sizeof start, "Content/%s.rlevel", name);
    if (project_file()[0] && !strncmp(ps->game_default_map, "/Content/", 9)) {
        char disk[1200];
        snprintf(disk, sizeof disk, "%s%s", content_root(), ps->game_default_map + 8);
        if (file_size(disk)) snprintf(start, sizeof start, "Content%s", ps->game_default_map + 8);
    }
    fprintf(f, "title=%s\nlevel=%s\nscalability=%d\nfullscreen=%d\nvsync=%d\nwidth=%d\nheight=%d\nshipping=%d\nversion=%s\n",
            project_file()[0] ? ps->window_title : name, start, project_file()[0] ? ps->game_scalability : ed->scalability,
            ps->fullscreen ? 1 : 0, ps->vsync ? 1 : 0, ps->resolution_w, ps->resolution_h, ps->shipping ? 1 : 0,
            ps->version[0] ? ps->version : "1.0");
    // the Quality mode's path tracer as the game's view (sugestÃ£o #45; the game falls back to Lit without ray tracing)
    fprintf(f, "path_tracing=%d\n", project_file()[0] && ps->render_mode == PROJECT_RENDER_QUALITY && ps->game_path_tracing && ps->ray_tracing ? 1 : 0);
    if (project_file()[0] && ps->hdr_output) // (A9.4) HDR10 when the player's display has it
        fprintf(f, "hdr=1\nhdr_paper=%.0f\nhdr_peak=%.0f\n", ps->hdr_paper > 0.0f ? ps->hdr_paper : 200.0f, ps->hdr_peak > 0.0f ? ps->hdr_peak : 1000.0f);
    if (project_file()[0]) { // (Q25) VR: Start in VR and the headset's frame (the Quest's package adds vr=1 itself: package_apk.ps1 -Quest)
        const ProjectVr* v = &ps->vr;
        fprintf(f, "vr=%d\nvr_refresh=%d\nvr_tracking=%d\nvr_quality=%d\nvr_foveation=%d\nvr_foveation_dynamic=%d\nvr_spacewarp=%d\nvr_render_scale=%.2f\n",
                v->start_in_vr && v->pc ? 1 : 0, v->refresh_hz, v->tracking, v->quality, v->foveation, v->foveation_dynamic ? 1 : 0, v->spacewarp ? 1 : 0,
                v->render_scale > 0.0f ? v->render_scale : 1.0f);
    }
    fclose(f);
    // the project's settings themselves (gravity, input, the Game Mode's classes) and the assets the systems load by
    // their hash (meshes, skeletal meshes, animations, montages, AI)
    snprintf(dst, sizeof dst, "%s/Game.rproject", dir);
    project_save_copy(dst);
    if (ps->online_steam) { // Steam: the developer's steam_api64.dll (the project's folder, from the Steamworks SDK) next to the game
        char root[900], from[1200], to[1200];
        project_root(root, sizeof root);
        snprintf(from, sizeof from, "%s/steam_api64.dll", root);
        snprintf(to, sizeof to, "%s/steam_api64.dll", dir);
        if (file_size(from)) pk_add(j, from, to, PK_OPTIONAL);
        else printf("[RealEngine] Package: Use Steam is on but the project has no steam_api64.dll (put the Steamworks SDK's next to the .rproject)\n");
    }
    for (uint32_t i = 0; i < plugin_count(); i++) { // (B14c) the plugins loaded now: their plugin.json and DLL in Plugins/<Name>
        const PluginInfo* p = plugin_at(i);
        if (!p->loaded || p->faulted || p->builtin) continue; // (a built-in one is the engine's: its choice in Config/Plugins.ini, below)
        char from[1200], to[1200];
        snprintf(from, sizeof from, "%s/plugin.json", p->dir);
        snprintf(to, sizeof to, "%s/Plugins/%s/plugin.json", dir, p->name);
        pk_add(j, from, to, PK_REQUIRED);
        snprintf(from, sizeof from, "%s/%s.dll", p->dir, p->module);
        snprintf(to, sizeof to, "%s/Plugins/%s/%s.dll", dir, p->name, p->module);
        pk_add(j, from, to, PK_REQUIRED);
    }
    if (ps->splash[0] && !strncmp(ps->splash, "/Content/", 9)) { // the loading screen's picture or video
        char from[1200], to[1200];
        snprintf(from, sizeof from, "%s%s", croot, ps->splash + 8);
        snprintf(to, sizeof to, "%s%s", content, ps->splash + 8);
        pk_add(j, from, to, PK_OPTIONAL);
    }
    { // (2026-11-10) the project's plugin choices (a built-in one off: the Gameplay Framework) next to the game's exe
        char from[1200], to[1200], pdir[1000];
        snprintf(pdir, sizeof pdir, "%s", project_file());
        char* sl = strrchr(pdir, '/');
        char* bs = strrchr(pdir, '\\');
        if (bs > sl) sl = bs;
        if (sl) {
            *sl = 0;
            snprintf(from, sizeof from, "%s/Config/Plugins.ini", pdir);
            snprintf(to, sizeof to, "%s/Config/Plugins.ini", dir);
            pk_add(j, from, to, PK_OPTIONAL);
        }
    }
    return j;
}

// the files (any thread): each copy with its folder made, then the levels and the hashed assets walked
static void package_run(PackJob* j, AsyncTask* t) {
    int steps = j->n + 2;
    for (int i = 0; i < j->n; i++) {
        const PkCopy* c = &j->c[i];
        if (t) {
            async_progress_n(t, i, steps);
            const char* nm = c->to + strlen(c->to);
            while (nm > c->to && nm[-1] != '/' && nm[-1] != '\\') nm--;
            async_status(t, "%s", nm);
            if (async_cancelled(t)) return;
        }
        if (c->kind != PK_REQUIRED && !file_size(c->from)) {
            if (c->kind == PK_OR_REMOVE) re_remove_file(c->to);
            continue;
        }
        char folder[1300];
        snprintf(folder, sizeof folder, "%s", c->to);
        char* cut = strrchr(folder, '/');
        char* cut2 = strrchr(folder, '\\');
        if (cut2 > cut) cut = cut2;
        if (cut) *cut = 0, re_make_dirs(folder);
        if (!copy_over(c->from, c->to)) {
            if (c->kind == PK_REQUIRED) {
                snprintf(j->fail, sizeof j->fail, "%s", c->from);
                return;
            }
            continue;
        }
        j->total += file_size(c->to);
        if (i == 0 && j->identity) { // the executable's icon and version
            PackageIdentity id = { j->icon, j->product, j->company, j->version, j->description, j->copyright, j->file };
            char err[200];
            if (!package_set_identity(j->exe, &id, err, sizeof err)) printf("[RealEngine] Package: the game's icon and version were not written: %s\n", err);
            else if (j->icon[0]) printf("[RealEngine] Package: icon of %d images\n", package_icon_count(j->exe));
        }
    }
    if (t) async_progress_n(t, j->n, steps), async_status(t, "%s", TR("The project's other levels..."));
    // the project's other levels (Open Level, Load Stream Level find them by name), each with its light and Level Blueprint
    j->total += package_levels(j->croot, j->content);
    if (t) async_progress_n(t, j->n + 1, steps), async_status(t, "%s", TR("The project's assets..."));
    j->total += package_assets(j->croot, j->content);
    // (B15a) Shipping: the Content folder into one checked file (rpak.c), the loose files gone (not for Android: its APK
    // holds them in its own zip)
    if (j->pack && !j->android) {
        if (t) async_status(t, "%s", TR("Packing the content..."));
        char pk[1400];
        snprintf(pk, sizeof pk, "%s\\Content.rpak", j->dir);
        RpakStats st = { 0 };
        if (!rpak_write_dir(j->content, pk, &st)) {
            snprintf(j->fail, sizeof j->fail, "%s", pk);
            return;
        }
        re_remove_tree(j->content);
        printf("[RealEngine] Package: Content.rpak, %u files, %.1f -> %.1f MB (%.0f ms)\n", st.files, (double)st.raw / 1048576.0,
               (double)st.packed / 1048576.0, st.ms);
    }
    j->ok = true;
}

static bool package_finish(PackJob* j, double now) {
    Editor* ed = j->ed;
    bool ok = j->ok && !j->fail[0];
    if (j->android) {
        if (ok && j->play) { // the store refuses a version code it has seen: the next bundle gets the next one
            ProjectSettings* ps = project();
            int sent = ps->android_version_code;
            if (ps->android_auto_version) {
                ps->android_version_code++;
                if (project_file()[0]) project_save(NULL);
            }
            editor_status(ed, now, TR("Google Play bundle: %s (version code %d; upload it in the Play Console). The APK beside it installs for a test."), j->apk, sent);
            if (j->show) re_show_in_explorer(j->apk);
        } else if (ok) {
            editor_status(ed, now, TR("Android game packaged: %s (install it with adb install, or copy it to the device)."), j->apk);
            if (j->show) re_show_in_explorer(j->apk);
        } else editor_status(ed, now, TR("The APK failed: %s"), j->fail[0] ? j->fail : j->dir);
        return ok;
    }
    if (j->fail[0]) editor_status(ed, now, TR("Packaging failed: could not copy %s."), j->fail);
    else if (ok) {
        editor_status(ed, now, TR("Game packaged in %s (%.1f MB): run %s.exe."), j->dir, (double)j->total / (1024.0 * 1024.0), j->name);
        if (j->show) re_show_in_explorer(j->dir);
    }
    return ok;
}

bool editor_package_game(Editor* ed, const char* out, bool show, double now) {
    PackJob* j = package_prepare(ed, out, show, now);
    if (!j) return false;
    package_run(j, NULL);
    bool ok = package_finish(j, now);
    pk_free(j);
    return ok;
}

// Package for Android (notas-dev/android.md): the game's folder made into an APK by android/package_apk.ps1 (aapt2,
// zipalign, apksigner of the Android SDK; the prebuilt arm64 runtime); its output in <out>/<name>_android.log
static bool package_android_run(PackJob* j, AsyncTask* t) {
    if (t) async_status(t, "%s", TR("Building the APK..."));
    int code = re_run_command(j->apk_cmd, j->apk_log);
    if (code != 0) {
        snprintf(j->fail, sizeof j->fail, "%s (%s %d; %s)", j->apk, TR("exit code"), code, j->apk_log);
        return false;
    }
    return true;
}

static void package_work(AsyncTask* t, void* arg) {
    PackJob* j = arg;
    package_run(j, t);
    if (!j->fail[0] && j->ok && j->android && !(t && async_cancelled(t)) && !package_android_run(j, t)) {
        if (t) async_fail(t, TR("The APK failed: %s"), j->fail);
        return;
    }
    if (j->fail[0] && t) async_fail(t, TR("Could not copy %s."), j->fail);
}
static void package_done(AsyncTask* t, void* arg, bool ok) {
    (void)t, (void)ok;
    PackJob* j = arg;
    if (async_cancelled(t)) editor_status(j->ed, re_time_seconds(), "%s", TR("Packaging cancelled."));
    else package_finish(j, re_time_seconds());
    pk_free(j);
}

// The Package Game window's: the same in the background (false: it could not start; the status bar says why)
bool editor_package_game_async(Editor* ed, const char* out, bool show, double now) {
    PackJob* j = package_prepare(ed, out, show, now);
    if (!j) return false;
    char label[200];
    snprintf(label, sizeof label, TR("Packaging %s"), j->name);
    async_run(label, package_work, package_done, j, ASYNC_CANCELLABLE);
    editor_status(ed, now, "%s", TR("Packaging in the background (the card at the bottom right)."));
    return true;
}

// Build > Package for Android: the game packaged (as for Windows) into <out>/<name>, then made into <out>/<name>.apk
static bool android_script(char* out, size_t cap) {
    char exe[1024];
    if (!re_exe_dir(exe, sizeof exe)) return false;
    static const char* const rel[] = { "android", "..\\android", "..\\..\\android" }; // (the engine's folder; a build's)
    for (int i = 0; i < 3; i++) {
        snprintf(out, cap, "%s\\%s\\package_apk.ps1", exe, rel[i]);
        if (file_size(out)) return true;
    }
    return false;
}

// Project Settings > Android > Create Upload Key: <project>/Build/Android/upload.keystore (+ .pass), set in the project
bool editor_android_upload_key(Editor* ed, double now) {
    char script[1200], root[1024], ks[1200], cmd[3000];
    if (!android_script(script, sizeof script)) {
        editor_status(ed, now, "%s", TR("The Android packager (android/package_apk.ps1) is missing from the engine's folder."));
        return false;
    }
    ProjectSettings* ps = project();
    project_root(root, sizeof root);
    snprintf(ks, sizeof ks, "%s\\Build\\Android\\upload.keystore", root);
    if (file_size(ks)) {
        editor_status(ed, now, TR("There is already an upload key: %s"), ks);
        return false;
    }
    char owner[96];
    snprintf(owner, sizeof owner, "%s", ps->company[0] ? ps->company : ps->name[0] ? ps->name : "Rivora Engine Game");
    for (char* c = owner; *c; c++)
        if (*c == '"') *c = '\'';
    snprintf(cmd, sizeof cmd, "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\" -NewUploadKey \"%s\" -KeyOwner \"%s\"", script, ks, owner);
    char log[1300];
    snprintf(log, sizeof log, "%s\\Build\\Android\\upload_key.log", root);
    char dir[1100];
    snprintf(dir, sizeof dir, "%s\\Build\\Android", root);
    re_make_dirs(dir);
    if (re_run_command(cmd, log) != 0 || !file_size(ks)) {
        editor_status(ed, now, TR("The upload key could not be made (%s)."), log);
        return false;
    }
    snprintf(ps->android_keystore, sizeof ps->android_keystore, "%s", ks);
    snprintf(ps->android_key_alias, sizeof ps->android_key_alias, "upload");
    snprintf(ps->android_key_pass_file, sizeof ps->android_key_pass_file, "%s.pass", ks);
    if (project_file()[0]) project_save(NULL);
    editor_status(ed, now, TR("Upload key made: %s. Keep a copy of it and of its .pass outside this PC: without it the game can't be updated."), ks);
    return true;
}

bool editor_package_android(Editor* ed, const char* out, bool show, bool async, double now) {
    return editor_package_android_as(ed, out, show, async, false, now);
}

// (Q25) Build > Package for Meta Quest: the Android package as a VR app (package_apk.ps1 -Quest: the Khronos OpenXR
// loader in the APK, Meta's VR manifest, landscape, game.ini's vr=1), <out>/<name>_Quest.apk
static bool g_pkg_quest;
bool editor_package_quest(Editor* ed, const char* out, bool show, bool async, double now) {
    g_pkg_quest = true;
    bool ok = editor_package_android_as(ed, out, show, async, false, now);
    g_pkg_quest = false;
    return ok;
}

bool editor_package_android_as(Editor* ed, const char* out, bool show, bool async, bool play, double now) {
    char script[1200];
    if (!android_script(script, sizeof script)) {
        editor_status(ed, now, "%s", TR("The Android packager (android/package_apk.ps1) is missing from the engine's folder."));
        return false;
    }
    const ProjectSettings* ps = project();
    if (play && (!ps->android_keystore[0] || !file_size(ps->android_keystore))) {
        editor_status(ed, now, "%s", TR("Google Play needs your upload key: Project Settings > Android > Create Upload Key."));
        return false;
    }
    if (ps->android_touch_controls) ui_touch_controls_asset(); // (made before the copies: Content/UI goes with the game)
    PackJob* j = package_prepare(ed, out, show, now);
    if (!j) return false;
    j->android = true;
    if (!out || !out[0]) out = ps->package_dir;
    snprintf(j->apk, sizeof j->apk, "%s\\%s%s.%s", out, j->name, g_pkg_quest ? "_Quest" : "", play ? "aab" : "apk");
    snprintf(j->apk_log, sizeof j->apk_log, "%s\\%s_android.log", out, j->name);
    // the package name: the project's, else com.realengine.<name> (letters and digits only)
    char pkg[128];
    if (ps->android_package[0]) snprintf(pkg, sizeof pkg, "%s", ps->android_package);
    else {
        int k = snprintf(pkg, sizeof pkg, "com.realengine.");
        for (const char* c = j->name; *c && k + 1 < (int)sizeof pkg; c++)
            if (isalnum((unsigned char)*c)) pkg[k++] = (char)tolower((unsigned char)*c);
        pkg[k] = 0;
        if (!pkg[15] || isdigit((unsigned char)pkg[15])) snprintf(pkg + 15, sizeof pkg - 15, "game");
    }
    static const char* const orient[3] = { "sensorLandscape", "sensorPortrait", "fullUser" };
    int o = ps->android_orientation >= 0 && ps->android_orientation < 3 ? ps->android_orientation : 0;
    char title[96];
    snprintf(title, sizeof title, "%s", ps->window_title[0] ? ps->window_title : j->name);
    for (char* c = title; *c; c++)
        if (*c == '"') *c = '\'';
    int n = snprintf(j->apk_cmd, sizeof j->apk_cmd,
                     "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\" -GameDir \"%s\" -Out \"%s\" -Package %s -Title \"%s\" "
                     "-Orientation %s -VersionName %s -VersionCode %d -Format %s -IconBackground \"%s\"",
                     script, j->dir, j->apk, pkg, title, orient[o], j->version, ps->android_version_code > 0 ? ps->android_version_code : 1,
                     play ? "both" : "apk", ps->android_icon_bg[0] ? ps->android_icon_bg : "#101418");
    if (play && n > 0 && n < (int)sizeof j->apk_cmd) { // Google Play's listing pictures beside the bundle, the feature graphic from the loading picture
        n += snprintf(j->apk_cmd + n, sizeof j->apk_cmd - (size_t)n, " -StoreAssets \"%s\\%s_GooglePlay\"", out, j->name);
        if (ps->splash[0] && !strncmp(ps->splash, "/Content", 8) && n > 0 && n < (int)sizeof j->apk_cmd)
            n += snprintf(j->apk_cmd + n, sizeof j->apk_cmd - (size_t)n, " -FeatureImage \"%s%s\"", content_root(), ps->splash + 8);
        j->play = true;
    }
    if (j->icon[0] && n > 0 && n < (int)sizeof j->apk_cmd) n += snprintf(j->apk_cmd + n, sizeof j->apk_cmd - (size_t)n, " -Icon \"%s\"", j->icon);
    if (ps->android_ar && n > 0 && n < (int)sizeof j->apk_cmd) // (ARCore in the APK; its depth hiding the virtual things or not)
        n += snprintf(j->apk_cmd + n, sizeof j->apk_cmd - (size_t)n, ps->android_ar_occlusion ? " -AR" : " -AR -ARNoOcclusion");
    if (g_pkg_quest && n > 0 && n < (int)sizeof j->apk_cmd) // (Q25) a Meta Quest VR app
        n += snprintf(j->apk_cmd + n, sizeof j->apk_cmd - (size_t)n, " -Quest%s%s", ps->vr.hand_tracking ? " -HandTracking" : "",
                      ps->vr.mr_enabled ? " -MixedReality" : ""); // (Q25b: the passthrough's feature, the Scene permission)
    if (ps->android_keystore[0] && n > 0 && n < (int)sizeof j->apk_cmd)
        snprintf(j->apk_cmd + n, sizeof j->apk_cmd - (size_t)n, " -Keystore \"%s\" -KeyAlias \"%s\" -KeyPassFile \"%s\"", ps->android_keystore,
                 ps->android_key_alias[0] ? ps->android_key_alias : "game", ps->android_key_pass_file);
    if (!async) {
        package_work(NULL, j);
        bool ok = package_finish(j, now);
        pk_free(j);
        return ok;
    }
    char label[200];
    snprintf(label, sizeof label, TR("Packaging %s for Android"), j->name);
    async_run(label, package_work, package_done, j, ASYNC_CANCELLABLE);
    editor_status(ed, now, "%s", TR("Packaging for Android in the background (the card at the bottom right)."));
    return true;
}

// Unreal's Landscape mode: the level's first landscape selected with its sculpting brush on (one is made if there is none)
static void editor_landscape_mode(Editor* ed, double now) {
    Entity land = ENTITY_NONE;
    for (uint32_t ai = 0; ai < ed->world.archetype_count && land == ENTITY_NONE; ai++) {
        EcsArchetype* ar = &ed->world.archetypes[ai];
        if ((ar->mask & COMP_BIT(COMP_LANDSCAPE)) && ar->count) land = ((Entity*)ar->entities.base)[0];
    }
    if (land == ENTITY_NONE) {
        add_actor(ed, SPAWN_LANDSCAPE, 0, now);
        land = ed->selected;
    }
    editor_select(ed, land, SEL_REPLACE);
    ed->land_tool = 1, ed->foliage_mode = false;
    ed->show_details = true;
    editor_status(ed, now, TR("Landscape Mode: Manage and Sculpt on the left (left button sculpts, Shift the opposite)."));
}

// Unreal's Foliage mode: the level's Foliage actor selected (one is made if there is none), its brush on
static void editor_foliage_mode(Editor* ed, double now) {
    Entity fol = ENTITY_NONE;
    for (uint32_t ai = 0; ai < ed->world.archetype_count && fol == ENTITY_NONE; ai++) {
        EcsArchetype* ar = &ed->world.archetypes[ai];
        if ((ar->mask & COMP_BIT(COMP_FOLIAGE)) && ar->count) fol = ((Entity*)ar->entities.base)[0];
    }
    if (fol == ENTITY_NONE) {
        add_actor(ed, SPAWN_FOLIAGE, 0, now);
        fol = ed->selected;
    }
    editor_select(ed, fol, SEL_REPLACE);
    ed->foliage_mode = true, ed->land_tool = 0;
    editor_status(ed, now, TR("Foliage Mode: the types and the brush on the left (left button paints, Shift erases)."));
}

void editor_save_all(Editor* ed, double now) {
    if (ed->playing) return;
    bp_editor_save_all(ed); // (the Blueprints that changed, compiled)
    editor_save_level(ed, ed->level_path, now);
}

// Unreal's Open Asset (Ctrl+P): type part of a name, the assets of the project that match, Enter or a click opens one
static void open_asset_dialog(Editor* ed) {
    if (!ed->show_open_asset) return;
    static char find[64];
    static char found[200][512];
    static int n = -1;
    if (!igIsPopupOpen_Str("###open_asset", 0)) igOpenPopup_Str("###open_asset", 0), find[0] = 0, n = -1;
    ImGuiViewport* vp = igGetMainViewport();
    igSetNextWindowPos((ImVec2){ vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.22f }, ImGuiCond_Always, (ImVec2){ 0.5f, 0.0f });
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 34, igGetFontSize() * 26 }, ImGuiCond_Always);
    char title[96];
    snprintf(title, sizeof title, "%s###open_asset", TR("Open Asset"));
    if (!igBeginPopupModal(title, &ed->show_open_asset, ImGuiWindowFlags_NoResize)) return;
    if (igIsWindowAppearing()) igSetKeyboardFocusHere(0);
    igSetNextItemWidth(-1);
    bool typed = igInputTextWithHint("##find", TR("Search Assets"), find, sizeof find, 0, NULL, NULL);
    if (typed || n < 0) n = content_search(find, found, 200);
    int open = -1;
    if (igIsKeyPressed_Bool(ImGuiKey_Enter, false) && n > 0) open = 0;
    igBeginChild_Str("##found", (ImVec2){ 0, 0 }, ImGuiChildFlags_Borders, 0);
    for (int i = 0; i < n; i++) {
        const char* name = strrchr(found[i], '/');
        char row[600];
        snprintf(row, sizeof row, "%s##%d", name ? name + 1 : found[i], i);
        if (igSelectable_Bool(row, false, 0, (ImVec2){ 0, 0 })) open = i;
        igSetItemTooltip("%s", found[i]);
    }
    if (!n) igTextDisabled("%s", TR("No asset by that name."));
    igEndChild();
    if (open >= 0) content_open_asset(found[open]), ed->show_open_asset = false, igCloseCurrentPopup();
    if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) ed->show_open_asset = false, igCloseCurrentPopup();
    igEndPopup();
}

// Build > Package Game, Platforms > Package Project: the Package Game window (project_ui.c: every option in one place)
static void package_game(Editor* ed, double now) {
    (void)now;
    ed->show_package = true;
}

// --- Shortcuts (Unreal's) ---

// The Play's pause menu key (Project Settings > Pause Menu; Esc stops the Play): a letter, a digit, F1-F12, Tab, Backspace
static ImGuiKey pie_pause_key(void) {
    const ProjectSettings* ps = project();
    const char* k = ps->pie_pause_key;
    if (!ps->pause_menu || !k[0]) return ImGuiKey_None;
    if (!k[1] && k[0] >= 'a' && k[0] <= 'z') return (ImGuiKey)(ImGuiKey_A + (k[0] - 'a'));
    if (!k[1] && k[0] >= 'A' && k[0] <= 'Z') return (ImGuiKey)(ImGuiKey_A + (k[0] - 'A'));
    if (!k[1] && k[0] >= '0' && k[0] <= '9') return (ImGuiKey)(ImGuiKey_0 + (k[0] - '0'));
    if ((k[0] == 'F' || k[0] == 'f') && atoi(k + 1) >= 1 && atoi(k + 1) <= 12) return (ImGuiKey)(ImGuiKey_F1 + atoi(k + 1) - 1);
    if (!_stricmp(k, "Tab")) return ImGuiKey_Tab;
    if (!_stricmp(k, "BackSpace") || !_stricmp(k, "Backspace")) return ImGuiKey_Backspace;
    return ImGuiKey_None;
}

static void shortcuts(Editor* ed, double now) {
    ImGuiIO* io = igGetIO_Nil();
    bool pie_input = ed->playing && ed->possessed && !ed->mouse_released;
    if (igIsKeyChordPressed_Nil(ImGuiMod_Alt | ImGuiKey_P) && !ed->playing) pie_multi_play(ed, now);
    if (igIsKeyChordPressed_Nil(ImGuiMod_Alt | ImGuiKey_S) && (!ed->playing || ed->simulating)) editor_simulate(ed, now); // (Unreal's Simulate)
    if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_R) && !ed->playing) ed->realtime = !ed->realtime; // (Unreal's Realtime)
    if (ed->playing) {
        if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) { editor_stop(ed, now); return; }
        if (igIsKeyPressed_Bool(ImGuiKey_F8, false)) editor_set_possessed(ed, !ed->possessed);
        if (igIsKeyChordPressed_Nil(ImGuiMod_Shift | ImGuiKey_F1) && ed->possessed && !ed->mouse_released) {
            ed->mouse_released = true;
            glfwSetInputMode(ed->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        if (igIsKeyPressed_Bool(ImGuiKey_Pause, false)) {
            ed->paused = !ed->paused;
            editor_status(ed, now, ed->paused ? TR("Paused.") : TR("Resumed."));
        }
        ImGuiKey pk = pie_pause_key();
        if (pk != ImGuiKey_None && !io->WantTextInput && igIsKeyPressed_Bool(pk, false) && !io->KeyCtrl && !io->KeyAlt) {
            if (plugins_gameplay_enabled()) pause_menu_key(&ed->world, false); // the pause menu (its Options: back; the Gameplay Framework's)
            return;                            // (P is the NavMesh's out of Play)
        }
    }
    if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_Space)) {
        if (!ed->content_docked) ed->drawer_open = !ed->drawer_open;
        else igSetWindowFocus_Str(dock_panel_id(DP_CONTENT)); // (Q26: docked by default: its tab to the front)
    }
    if (igIsKeyPressed_Bool(ImGuiKey_F11, false) && !bp_debug_paused()) toggle_fullscreen(ed); // (paused at a breakpoint: F10 / F11 step)
    if (igIsKeyPressed_Bool(ImGuiKey_F10, false) && !bp_debug_paused()) dv_set_open(!dv_open()); // the Debug Viewer
    if (igIsKeyPressed_Bool(ImGuiKey_F1, false) && !script_ide_focused()) assistant_ui_open(ui_tip_hovered_name()); // (Q5: the control under the mouse searched; the Code Editor's F1: the API reference)
    // typing, playing, flying or in a Blueprint window: the letters are theirs
    if (io->WantTextInput || pie_input || ed->flying || bp_editor_focused() || script_ide_focused() || seq_editor_focused()) return; // (the Sequencer's Ctrl+C / V / Z, Del: its keys, not the level's actors)
    if (ed->gizmo_active) return; // (2026-11-23) a gizmo drag: X/Y/Z, the digits, Esc... are its (editor_viewport.c)

    if (io->KeyCtrl) {
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_D)) duplicate_selected(ed, now, 120.0f);
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_A)) editor_select_all(ed);
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_I)) invert_selection(ed);
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_C)) copy_selected(ed, now);
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_X) && ed->nsel) { copy_selected(ed, now); delete_selected(ed); }
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_V)) paste_clipboard(ed, now);
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_Z)) { // (B11: in the Landscape mode a stroke first)
            if (!(ed->land_tool && ecs_get(&ed->world, ed->selected, COMP_LANDSCAPE) && landscape_undo(&ed->world, ed->selected))) editor_undo(ed, now);
            else editor_status(ed, now, "%s", TR("Landscape: the last stroke undone."));
        }
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_Y)) editor_redo(ed, now);
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_S)) editor_save_level(ed, ed->level_path, now);
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) editor_save_all(ed, now); // (Unreal's Save All)
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_P)) ed->show_open_asset = true;
        if (igIsKeyChordPressed_Nil(ImGuiMod_Shift | ImGuiKey_1)) ed->land_tool = 0, ed->foliage_mode = false; // (Unreal's modes)
        if (igIsKeyChordPressed_Nil(ImGuiMod_Shift | ImGuiKey_2)) editor_landscape_mode(ed, now);
        if (igIsKeyChordPressed_Nil(ImGuiMod_Shift | ImGuiKey_3)) editor_foliage_mode(ed, now);
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_H)) hide_selected(ed, false);
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_O)) editor_open_level(ed, "", now);
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_N)) new_level(ed, now);
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_GraveAccent)) // Unreal's Ctrl+`: World <-> Local
            ed->gizmo_orient = ed->gizmo_orient == GIZMO_ORIENT_LOCAL ? GIZMO_ORIENT_WORLD : GIZMO_ORIENT_LOCAL;
        return;
    }
    if (io->KeyAlt && !ed->playing) { // Unreal's viewport types: Alt+G Perspective, Alt+J Top, Alt+H Front, Alt+K Left (Shift: the other side)
        bool sh = io->KeyShift;
        if (igIsKeyPressed_Bool(ImGuiKey_G, false)) viewport_set_projection(ed, VP_PROJ_PERSPECTIVE);
        if (igIsKeyPressed_Bool(ImGuiKey_J, false)) viewport_set_projection(ed, sh ? VP_PROJ_BOTTOM : VP_PROJ_TOP);
        if (igIsKeyPressed_Bool(ImGuiKey_H, false)) viewport_set_projection(ed, sh ? VP_PROJ_BACK : VP_PROJ_FRONT);
        if (igIsKeyPressed_Bool(ImGuiKey_K, false)) viewport_set_projection(ed, sh ? VP_PROJ_RIGHT : VP_PROJ_LEFT);
    }
    if (io->KeyAlt) return;
    if (igIsKeyPressed_Bool(ImGuiKey_Q, false)) ed->tool = TOOL_SELECT;
    if (igIsKeyPressed_Bool(ImGuiKey_W, false)) ed->tool = TOOL_MOVE;
    if (igIsKeyPressed_Bool(ImGuiKey_E, false)) ed->tool = TOOL_ROTATE;
    if (igIsKeyPressed_Bool(ImGuiKey_R, false)) ed->tool = TOOL_SCALE;
    if (igIsKeyPressed_Bool(ImGuiKey_Space, false)) ed->tool = ed->tool == TOOL_SCALE ? TOOL_MOVE : ed->tool + 1; // Unreal cycles
    if (igIsKeyPressed_Bool(ImGuiKey_F, false)) viewport_focus(ed, ed->selected);
    if (igIsKeyPressed_Bool(ImGuiKey_G, false)) ed->game_view = !ed->game_view;
    if (igIsKeyPressed_Bool(ImGuiKey_End, false)) snap_to_floor(ed);
    CTransform* sel_t = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    if (igIsKeyPressed_Bool(ImGuiKey_H, false) && !igGetIO_Nil()->KeyCtrl && sel_t) hide_selected(ed, true); // Unreal's H: hide in the editor
    if (igIsKeyPressed_Bool(ImGuiKey_Delete, false) && !levels_panel_focused()) delete_selected(ed); // (in Window > Levels: the sublevel)
    if (igIsKeyChordPressed_Nil(ImGuiMod_Shift | ImGuiKey_D) && ed->nsel) { // Blender's: copies that follow the mouse
        duplicate_selected(ed, now, 0.0f);
        editor_grab_begin(ed);
    }
    if (igIsKeyPressed_Bool(ImGuiKey_Escape, false) && !ed->playing && !ed->grab) editor_select_none(ed); // Unreal's Esc
    if (igIsKeyPressed_Bool(ImGuiKey_F2, false)) begin_rename(ed, ed->selected);
    if (igIsKeyPressed_Bool(ImGuiKey_Escape, false) && !ed->drawer_open) ed->selected = ENTITY_NONE;
    // Unreal's view mode keys: Alt+4 Lit, Alt+3 Unlit, Alt+2 Wireframe (here F4/F3/F2 are taken: plain digits)
    for (int i = 0; i < 9 && !io->KeyShift; i++)
        if (igIsKeyPressed_Bool((ImGuiKey)(ImGuiKey_1 + i), false) && i < VIEW_MODE_COUNT) ed->view_mode = (uint32_t)i;
    if (igIsKeyPressed_Bool(ImGuiKey_0, false)) ed->view_mode = VIEW_METALLIC;
    if (igIsKeyPressed_Bool(ImGuiKey_P, false) && !io->KeyShift) ed->show_nav = !ed->show_nav; // Unreal's P: the NavMesh
    if (igIsKeyChordPressed_Nil(ImGuiMod_Shift | ImGuiKey_P) && gi_available()) // the reference, and back
        ed->view_mode = ed->view_mode == VIEW_PATH_TRACING ? VIEW_LIT : VIEW_PATH_TRACING;
}

// --- Menus ---

static void language_menu(void) {
    for (int l = 0; l < LANG_COUNT; l++)
        if (ui_menu_item(ICON_LANGUAGE, i18n_language_name(l), NULL, i18n_language() == l, true)) i18n_set_language(l), g_prefs_dirty = true;
}

static void main_menu_items(Editor* ed, double now);
static void main_menu(Editor* ed, double now) {
    // (Q26) the bar of the mockups: the logo, the name and the version, the menus, then the frame's readout and the
    // target platforms (editor_look.c); taller than a frame
    look_menu_bar_push();
    if (igBeginMainMenuBar()) {
        look_menu_brand(ed);
        main_menu_items(ed, now);
        look_menu_readouts(ed);
        igEndMainMenuBar();
    }
    look_menu_bar_pop();
}

static void main_menu_items(Editor* ed, double now) {
    bool has_sel = ecs_alive(&ed->world, ed->selected);
    if (igBeginMenu(TR("File"), true)) {
        if (ui_menu_item(ICON_NEW, TR("New Project..."), NULL, false, !ed->playing)) ed->show_browser = true;
        if (ui_menu_item(ICON_OPEN, TR("Open Project..."), NULL, false, !ed->playing)) ed->show_browser = true, project_ui_browser_refresh();
        igSeparator();
        if (ui_menu_item(ICON_NEW, TR("New Level"), "Ctrl+N", false, true)) new_level(ed, now);
        if (ui_menu_item(ICON_OPEN, TR("Open Level..."), "Ctrl+O", false, true)) editor_open_level(ed, "", now);
        if (ui_menu_item(ICON_SAVE, TR("Save Current Level"), "Ctrl+S", false, !ed->playing)) editor_save_level(ed, ed->level_path, now);
        if (ui_menu_item(ICON_SAVE, TR("Save Current Level As..."), NULL, false, !ed->playing)) editor_save_level(ed, "", now);
        if (ui_menu_item(ICON_SAVE, TR("Save All"), "Ctrl+Shift+S", false, !ed->playing)) editor_save_all(ed, now);
        if (ui_begin_menu(ICON_FOLDER, TR("Source Control"), true)) { // (B14: Git)
            if (ui_menu_item(ICON_SAVE, TR("Submit Content..."), NULL, false, true)) g_show_submit = true;
            if (ui_menu_item(ICON_UPDATE, TR("Refresh Status"), NULL, false, sc_active())) sc_refresh();
            if (ui_menu_item(ICON_IMPORT, TR("Pull"), NULL, false, sc_active() && !sc_busy())) sc_pull(); // (B14: the remote's commits)
            if (ui_menu_item(ICON_FORWARD, TR("Push"), NULL, false, sc_active() && !sc_busy())) sc_push();
            if (ui_menu_item(ICON_FOLDER, TR("Track Binaries with Git LFS"), NULL, false, sc_active())) sc_lfs_track(), editor_status(ed, now, "%s", sc_last_message());
            if (!sc_active()) igTextDisabled("%s", TR("The project isn't in a Git repository."));
            igEndMenu();
        }
        igSeparator();
        if (ui_menu_item(ICON_SEARCH, TR("Open Asset..."), "Ctrl+P", false, true)) ed->show_open_asset = true;
        if (ui_menu_item(ICON_IMPORT, TR("Import Into Level..."), NULL, false, !ed->playing)) content_import_dialog();
        igSeparator();
        if (ui_menu_item(ICON_EXIT, TR("Exit"), NULL, false, true) && editor_request_quit(ed)) ed->quit = true;
        igEndMenu();
    }
    if (igBeginMenu(TR("Edit"), true)) {
        if (ui_menu_item(ICON_UNDO, TR("Undo"), "Ctrl+Z", false, ed->undo_count > 0 && !ed->playing)) editor_undo(ed, now);
        if (ui_menu_item(ICON_REDO, TR("Redo"), "Ctrl+Y", false, ed->redo_count > 0 && !ed->playing)) editor_redo(ed, now);
        igSeparator();
        if (ui_menu_item(ICON_DUPLICATE, TR("Duplicate"), "Ctrl+D", false, has_sel)) duplicate_selected(ed, now, 120.0f);
        if (ui_menu_item(ICON_DUPLICATE, TR("Duplicate and Move"), "Shift+D", false, has_sel)) { duplicate_selected(ed, now, 0.0f); editor_grab_begin(ed); }
        igSeparator();
        if (ui_menu_item(ICON_DUPLICATE, TR("Copy"), "Ctrl+C", false, has_sel)) copy_selected(ed, now);
        if (ui_menu_item(ICON_DELETE, TR("Cut"), "Ctrl+X", false, has_sel)) { copy_selected(ed, now); delete_selected(ed); }
        if (ui_menu_item(ICON_NEW, TR("Paste"), "Ctrl+V", false, ed->clip_count > 0)) paste_clipboard(ed, now);
        if (ui_menu_item(ICON_DELETE, TR("Delete"), "Delete", false, has_sel)) delete_selected(ed);
        igSeparator();
        ui_menu_item_ptr(ICON_SETTINGS, TR("Editor Preferences..."), NULL, &ed->show_prefs, true);
        ui_menu_item_ptr(ICON_SLIDERS, TR("Project Settings..."), NULL, &ed->show_project_settings, true);
        ui_menu_item_ptr(ICON_COMPONENT, TR("Plugins"), NULL, &g_show_plugins, true); // (B14c, as Unreal's Edit > Plugins)
        igEndMenu();
    }
    if (igBeginMenu(TR("Window"), true)) {
        ui_menu_item_ptr(ICON_SELECT, TR("Modes"), NULL, look_modes_flag(), true); // (Q26)
        ui_menu_item_ptr(ICON_PLUS, TR("Place Actors"), NULL, &ed->show_place, true);
        ui_menu_item_ptr(ICON_PANEL, TR("Outliner"), NULL, &ed->show_outliner, true);
        ui_menu_item_ptr(ICON_PLANE, TR("Cloth Simulation"), NULL, &ed->show_cloth, true);
        ui_menu_item_ptr(ICON_LEVEL, TR("Levels"), NULL, levels_show_flag(), true);
        ui_menu_item_ptr(ICON_CAMERA, TR("Cinematics"), NULL, &g_show_cine, true); // (B9: Movie Render Queue, Take Recorder)
        ui_menu_item_ptr(ICON_INFO, TR("Details"), NULL, &ed->show_details, true);
        ui_menu_item_ptr(ICON_FOLDER, TR("Content Drawer"), "Ctrl+Space", &ed->drawer_open, !ed->content_docked);
        ui_menu_item_ptr(ICON_FOLDER, TR("Content Browser (docked)"), NULL, &ed->content_docked, true);
        ui_menu_item_ptr(ICON_LOG, TR("Output Log"), NULL, &ed->show_log, true);
        ui_menu_item_ptr(ICON_LOG, TR("Import Report"), NULL, imprep_show_flag(), true); // (Q19) the last import's conversions
        ui_menu_item_ptr(ICON_MONITOR, TR("Platform Support"), NULL, look_platforms_flag(), true); // (Q26) each target's readiness, the devices
        ui_menu_item_ptr(ICON_PERSPECTIVE, TR("Viewport 2"), NULL, &ed->show_view2, true);
        igSeparator();
        dock_layout_menu(ed); // (Q2: Save Layout As, the saved ones, Default)
        igSeparator();
        ui_menu_item_ptr(ICON_STATS, TR("Stats"), NULL, &ed->show_stats, true);
        if (ui_menu_item(ICON_TIMER, TR("Debug Viewer"), "F10", dv_open(), true)) dv_set_open(!dv_open());
        ui_menu_item_ptr(ICON_GAMEPAD, TR("Game View"), "G", &ed->game_view, true);
        if (ui_menu_item(ICON_FULLSCREEN, TR("Fullscreen"), "F11", ed->fullscreen, true)) toggle_fullscreen(ed);
        igEndMenu();
    }
    if (igBeginMenu(TR("Tools"), true)) {
        if (ui_menu_item(ICON_TIMER, TR("Measure Frame Time (3 s)"), NULL, false, ed->bench_request <= 0.0)) ed->bench_request = 3.0;
        if (ui_menu_item(ICON_FUNCTION, TR("Editor Utilities"), NULL, false, true)) editor_utilities_open();
        ui_menu_item_ptr(ICON_BRAIN, TR("AI Connection (MCP)"), NULL, mcp_ui_show_flag(), true); // (2026-11-24: mcp.h)
        if (ui_menu_item(ICON_STATS, TR("Trace (Insights)"), NULL, g_show_trace, true)) g_show_trace = !g_show_trace; // (B14b)
        if (plugin_menu_count() && ui_begin_menu(ICON_COMPONENT, TR("Plugins"), true)) { // (B14c) the plugins' own items
            for (uint32_t i = 0; i < plugin_menu_count(); i++)
                if (ui_menu_item(ICON_PLUGIN, plugin_menu_label(i), NULL, false, true)) plugin_menu_run(i);
            igEndMenu();
        }
        if (ui_menu_item(ICON_EXPOSURE, TR("De-light Selected Actor's Texture"), NULL, false, ed->selected != ENTITY_NONE && !ed->playing))
            delight_selected(ed, now);
        if (ui_begin_menu(ICON_ANIMATION, TR("Animation Packs"), !ed->playing)) {
            static const char* const packs[ANIM_PACK_COUNT] = { "Locomotion (Mannequins)", "Quadruped", "Fish", "Bird", "Reptile",
                                                                "Motion Matching Examples" };
            for (int p = 0; p < ANIM_PACK_COUNT; p++)
                if (ui_menu_item(ICON_ANIMATION, TR(packs[p]), NULL, false, true)) {
                    uint32_t n = anim_make_pack(p);
                    content_mark_dirty();
                    render_meshes_reload_async(ed->vk);
                    editor_status(ed, now, n ? TR("Animation pack added: %u files.") : TR("The pack could not be made."), n);
                }
            igEndMenu();
        }
        igEndMenu();
    }
    if (igBeginMenu(TR("Build"), true)) {
        bool can = gi_available() && !ed->playing && !gi_baking();
        // Unreal's: Build All Levels, Build Lighting Only, the quality as a submenu
        static int quality = BAKE_PREVIEW;
        if (ui_menu_item(ICON_BAKE, TR("Build All Levels"), NULL, false, can)) build_lighting(ed, quality, now);
        if (ui_menu_item(ICON_BAKE, TR("Build Lighting Only"), NULL, false, can)) build_lighting(ed, quality, now);
        if (ui_begin_menu(ICON_SLIDERS, TR("Lighting Quality"), true)) {
            if (ui_menu_item(ICON_PACKAGE, TR("Preview"), NULL, quality == BAKE_PREVIEW, true)) quality = BAKE_PREVIEW;
            if (ui_menu_item(ICON_PACKAGE, TR("Production"), NULL, quality == BAKE_PRODUCTION, true)) quality = BAKE_PRODUCTION;
            igEndMenu();
        }
        if (ui_menu_item(ICON_DELETE, TR("Clear Baked Lighting"), NULL, false, gi_lightmaps_present() && !gi_baking())) {
            gi_lightmaps_clear(ed->vk);
            editor_status(ed, now, TR("Baked lighting cleared: the Static meshes use the dynamic GI."));
        }
        if (!gi_available()) igTextDisabled("%s", TR("Needs hardware ray tracing."));
        igSetItemTooltip("%s", TR("Bakes the indirect light of the Static meshes (Mobility: Static) with the GI itself."));
        igSeparator();
        if (ui_menu_item(ICON_CUBE, TR("Build HLODs"), NULL, false, !ed->playing && wp_settings(&ed->world) != ENTITY_NONE)) build_hlods(ed, now);
        igSetItemTooltip("%s", TR("World Partition: each region's meshes merged and simplified into one, seen far away (Content/HLOD)."));
        if (ui_menu_item(ICON_MOVEMENT, TR("Build Paths"), NULL, false, true)) {
            navmesh_rebuild(&ed->world);
            ed->show_nav = true;
            editor_status(ed, now, TR("Building the navigation (NavMesh): P shows it."));
        }
        igSeparator();
        if (ui_menu_item(ICON_GAMEPAD, TR("Package Game..."), NULL, false, !ed->playing)) package_game(ed, now);
        if (ui_menu_item(ICON_GAMEPAD, TR("Package for Android: APK (no Play Store)"), NULL, false, !ed->playing && project()->package_dir[0]))
            editor_package_android(ed, NULL, true, true, now);
        if (ui_menu_item(ICON_GAMEPAD, TR("Package for Google Play Store: AAB"), NULL, false, !ed->playing && project()->package_dir[0]))
            editor_package_android_as(ed, NULL, true, true, true, now);
        if (ui_menu_item(ICON_GAMEPAD, TR("Package for Meta Quest (VR)"), NULL, false, !ed->playing && project()->package_dir[0])) // (Q25)
            editor_package_quest(ed, NULL, true, true, now);
        igSetItemTooltip("%s", TR("A folder with the game ready to run: its executable, the level and its baked lighting."));
        igEndMenu();
    }
    if (igBeginMenu(TR("Select"), true)) {
        if (ui_menu_item(ICON_SELECT, TR("Select All"), "Ctrl+A", false, true)) editor_select_all(ed);
        if (ui_menu_item(ICON_SELECT, TR("Select None"), "Esc", false, has_sel)) editor_select_none(ed);
        if (ui_menu_item(ICON_SELECT, TR("Invert Selection"), "Ctrl+I", false, true)) invert_selection(ed);
        igSeparator();
        igTextDisabled("%s", TR("Box select: drag on empty space in the viewport or the Outliner"));
        igTextDisabled("%s", TR("Shift adds, Ctrl removes; Ctrl+click toggles one"));
        igEndMenu();
    }
    if (igBeginMenu(TR("Actor"), true)) {
        if (ui_begin_menu(ICON_PLUS, TR("Add"), true)) {
            quick_add_items(ed, now);
            igEndMenu();
        }
        igSeparator();
        if (ui_menu_item(ICON_RENAME, TR("Rename"), "F2", false, has_sel)) begin_rename(ed, ed->selected);
        if (ui_menu_item(ICON_FOCUS, TR("Focus"), "F", false, has_sel)) viewport_focus(ed, ed->selected);
        if (ui_menu_item(ICON_FLOOR, TR("Snap to Floor"), "End", false, has_sel)) snap_to_floor(ed);
        igSeparator();
        if (ui_begin_menu(ICON_GAMEPAD, TR("Make Vehicle from Selected"), has_sel && !ed->playing)) {
            make_vehicle_menu(ed);
            igEndMenu();
        }
        if (ui_menu_item(ICON_GAMEPAD, TR("Make Cloth from Selected"), NULL, false, has_sel && !ed->playing)) make_cloth_from_selected(ed);
        if (ui_menu_item(ICON_SPHERE, TR("Make Soft Body from Selected"), NULL, false, has_sel && !ed->playing)) make_soft_from_selected(ed);
        if (ui_begin_menu(ICON_EYE, TR("Visibility"), true)) { // (Unreal's: hide in the editor; the game still shows them)
            if (ui_menu_item(ICON_EYE, TR("Hide Selected"), "H", false, has_sel)) hide_selected(ed, true);
            if (ui_menu_item(ICON_EYE, TR("Show All"), "Ctrl+H", false, true)) hide_selected(ed, false);
            igSeparator();
            for (int on = 0; on < 2; on++) // the selection's icons in the viewport (Details > Show Icon, each actor)
                if (ui_menu_item(on ? ICON_EYE : ICON_EYE_CLOSED, TR(on ? "Show Icons of Selected" : "Hide Icons of Selected"), NULL, false, has_sel)) {
                    editor_undo_push(ed);
                    for (uint32_t s = 0; s < ed->nsel; s++) {
                        CTransform* st = ecs_get(&ed->world, ed->sel[s], COMP_TRANSFORM);
                        if (st) st->flags = on ? (st->flags & ~TRANSFORM_NO_ICON) : (st->flags | TRANSFORM_NO_ICON);
                    }
                }
            igEndMenu();
        }
        igEndMenu();
    }
    if (igBeginMenu(TR("Help"), true)) {
        if (ui_menu_item(ICON_HELP, TR("Assistant"), "F1", assistant_ui_is_open(), true)) assistant_ui_open(NULL);
        if (ui_menu_item(ICON_FUNCTION, TR("API Reference"), NULL, false, true)) script_api_reference_open(NULL);
        services_help_menu(ed);
        igSeparator();
        { // (2026-11-23) free and open source: the site's donation page (endpoints.ini [support] donate_url)
            const char* don = endpoints_get("support", "donate_url");
            if (don && don[0] && ui_menu_item(ICON_GLOBE, TR("Support Rivora Engine (Donate)"), NULL, false, true)) update_open_url(don);
        }
        if (ui_menu_item(ICON_INFO, TR("About Rivora Engine"), NULL, false, true)) ed->show_about = true;
        igEndMenu();
    }
}

// --- Toolbar ---

// (Q26) The Play group's buttons: a bare glyph on the toolbar (the mockups'), its background under the mouse; greyed
// while it can't act (Pause and Stop before Play)
static bool play_button(const char* id, int kind, uint32_t col, bool active, bool enabled, const char* tip) {
    float h = igGetFrameHeight();
    igPushStyleColor_U32(ImGuiCol_Button, active ? TH(TK_ACCENT_DIM) : THEME_NONE);
    igBeginDisabled(!enabled);
    bool pressed = igButton(id, (ImVec2){ h * 1.25f, h });
    igEndDisabled();
    igPopStyleColor(1);
    if (tip) ui_item_tip(tip);
    if (!enabled) col = theme_alpha(col, 80);
    ImVec2 a = igGetItemRectMin(), b = igGetItemRectMax();
    ImDrawList* dl = igGetWindowDrawList();
    float cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.5f, s = h * 0.27f;
    if (kind == 0)
        ImDrawList_AddTriangleFilled(dl, (ImVec2){ cx - s * 0.8f, cy - s }, (ImVec2){ cx + s, cy }, (ImVec2){ cx - s * 0.8f, cy + s }, col);
    else if (kind == 1) {
        ImDrawList_AddRectFilled(dl, (ImVec2){ cx - s * 0.85f, cy - s }, (ImVec2){ cx - s * 0.25f, cy + s }, col, 1.0f, 0);
        ImDrawList_AddRectFilled(dl, (ImVec2){ cx + s * 0.25f, cy - s }, (ImVec2){ cx + s * 0.85f, cy + s }, col, 1.0f, 0);
    } else if (kind == 2) {
        ImDrawList_AddRectFilled(dl, (ImVec2){ cx - s * 0.85f, cy - s * 0.85f }, (ImVec2){ cx + s * 0.85f, cy + s * 0.85f }, col, 2.0f, 0);
    } else {
        ImDrawList_AddTriangleFilled(dl, (ImVec2){ cx - s, cy + s * 0.1f }, (ImVec2){ cx, cy - s }, (ImVec2){ cx + s, cy + s * 0.1f }, col);
        ImDrawList_AddRectFilled(dl, (ImVec2){ cx - s, cy + s * 0.45f }, (ImVec2){ cx + s, cy + s }, col, 0, 0);
    }
    return pressed;
}

// (Q26) A toolbar button that opens a menu: its icon and a small chevron (the mockups' groups); the name and the text
// in its tooltip
static bool tb_icon_menu(const char* id, UiIcon icon, uint32_t col, const char* name, const char* tip) {
    float h = igGetFrameHeight(), is = h * 0.66f, cs = h * 0.36f, w = h * 0.22f + is + 2.0f + cs + h * 0.2f;
    char bid[48];
    snprintf(bid, sizeof bid, "##%s", id);
    bool r = igButton(bid, (ImVec2){ w, h });
    ui_tip_item(icon, name, tip, NULL);
    ImVec2 a = igGetItemRectMin();
    ImDrawList* dl = igGetWindowDrawList();
    ui_icon(dl, icon, a.x + h * 0.22f, a.y + (h - is) * 0.5f, is, col);
    ui_icon(dl, ICON_CHEVRON, a.x + h * 0.22f + is + 2.0f, a.y + (h - cs) * 0.5f, cs, THA(TK_TEXT, 170));
    return r;
}

static void tb_separator(void) { // a thin line between the groups
    igSameLine(0, 8);
    ImVec2 p = igGetCursorScreenPos();
    float h = igGetFrameHeight();
    ImDrawList_AddLine(igGetWindowDrawList(), (ImVec2){ p.x, p.y + h * 0.2f }, (ImVec2){ p.x, p.y + h * 0.8f }, TH(TK_BORDER_STRONG), 1.0f);
    igDummy((ImVec2){ 1, h });
    igSameLine(0, 8);
}

static void platforms_menu(Editor* ed, double now) { // (the Platforms dropdown: every packager, the chosen target first)
    int t = look_target();
    igSeparatorText(t == LOOK_PC ? TR("Windows (target)") : "Windows");
    if (ui_menu_item(ICON_MONITOR, TR("Package Project"), NULL, false, !ed->playing)) package_game(ed, now);
    if (ui_menu_item(ICON_SLIDERS, TR("Packaging Settings..."), NULL, false, true)) ed->show_package = true;
    igSeparatorText(t == LOOK_ANDROID ? TR("Android (target)") : "Android");
    if (ui_menu_item(ICON_ANDROID, TR("Package for Android: APK (no Play Store)"), NULL, false, !ed->playing && project()->package_dir[0]))
        editor_package_android(ed, NULL, true, true, now);
    if (ui_menu_item(ICON_ANDROID, TR("Package for Google Play Store: AAB"), NULL, false, !ed->playing && project()->package_dir[0]))
        editor_package_android_as(ed, NULL, true, true, true, now);
    igSeparatorText(t == LOOK_VR ? TR("VR (target)") : "VR");
    if (ui_menu_item(ICON_VR, TR("Package for Meta Quest (VR)"), NULL, false, !ed->playing && project()->package_dir[0]))
        editor_package_quest(ed, NULL, true, true, now);
    if (ui_menu_item(ICON_VR, TR("VR Preview"), NULL, false, !ed->playing)) editor_vr_preview(ed, now);
    igSeparator();
    ui_menu_item_ptr(ICON_MONITOR, TR("Platform Support"), NULL, look_platforms_flag(), true);
}

static void toolbar(Editor* ed, double now) {
    // (Q26) the mockups' toolbar: the mode, the groups (icon + chevron), Play / Pause / Stop / Eject in the middle,
    // Platforms, Settings on the right; taller buttons
    igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, (ImVec2){ 4, 4 });
    igPushStyleVar_Vec2(ImGuiStyleVar_FramePadding, (ImVec2){ 8, 5 });
    igPushStyleColor_U32(ImGuiCol_Button, TH(TK_BG_CONTROL));
    // Unreal's modes: Selection, Landscape (its sculpting tools on the landscape: the first one is selected)
    const char* mode_name = ed->land_tool ? TR("Landscape Mode") : ed->foliage_mode ? TR("Foliage Mode") : TR("Select Mode");
    if (ui_dropdown_button("tb_mode", ed->land_tool || ed->foliage_mode ? ICON_FLOOR : ICON_SELECT, mode_name, TH(TK_TEXT_STRONG),
                           TR("Editor modes: Selection, Landscape, Foliage (Shift+1 / Shift+2 / Shift+3)")))
        igOpenPopup_Str("tb_mode_menu", 0);
    if (igBeginPopup("tb_mode_menu", 0)) {
        if (ui_menu_item(ICON_SELECT, TR("Selection Mode"), "Shift+1", !ed->land_tool && !ed->foliage_mode, true)) ed->land_tool = 0, ed->foliage_mode = false;
        if (ui_menu_item(ICON_FLOOR, TR("Landscape Mode"), "Shift+2", ed->land_tool != 0, true)) editor_landscape_mode(ed, now);
        if (ui_menu_item(ICON_FLOOR, TR("Foliage Mode"), "Shift+3", ed->foliage_mode, true)) editor_foliage_mode(ed, now);
        igEndPopup();
    }
    tb_separator();
    if (tb_icon_menu("tb_save", ICON_SAVE, TH(TK_TEXT), TR("Save"), TR("Save Current Level (Ctrl+S), Save All, Open Level")))
        igOpenPopup_Str("tb_save_menu", 0);
    if (igBeginPopup("tb_save_menu", 0)) {
        if (ui_menu_item(ICON_SAVE, TR("Save Current Level"), "Ctrl+S", false, !ed->playing)) editor_save_level(ed, ed->level_path, now);
        if (ui_menu_item(ICON_SAVE, TR("Save All"), "Ctrl+Shift+S", false, !ed->playing)) editor_save_all(ed, now);
        if (ui_menu_item(ICON_OPEN, TR("Open Level..."), "Ctrl+O", false, true)) editor_open_level(ed, "", now);
        igEndPopup();
    }
    igSameLine(0, 4);
    if (tb_icon_menu("tb_add", ICON_PLUS, TH(TK_OK), TR("Add"), TR("Quick Add: shapes, lights, cameras, every class to place")))
        igOpenPopup_Str("tb_add_menu", 0);
    if (igBeginPopup("tb_add_menu", 0)) {
        quick_add_items(ed, now);
        igSeparator();
        ui_menu_item_ptr(ICON_PANEL, TR("Place Actors Panel"), NULL, &ed->show_place, true); // (UE5's)
        igEndPopup();
    }
    igSameLine(0, 4);
    // Unreal's Blueprints menu: the level's own Level Blueprint (one per level, saved next to it)
    if (tb_icon_menu("tb_bp", ICON_BLUEPRINT, TH(TK_ASSET_BLUEPRINT), TR("Blueprints"), TR("Open Level Blueprint, new classes")))
        igOpenPopup_Str("tb_bp_menu", 0);
    if (igBeginPopup("tb_bp_menu", 0)) {
        if (ui_menu_item(ICON_LEVEL, TR("Open Level Blueprint"), NULL, false, true)) {
            if (!ed->level_path[0]) editor_status(ed, now, TR("Save the level first: its Level Blueprint is saved next to it."));
            else {
                struct BpAsset* lb = level_bp_open_or_create(ed->level_path);
                char vp[800];
                const char* root = content_root();
                size_t rl = strlen(root);
                if (lb && !_strnicmp(lb->path, root, rl)) {
                    snprintf(vp, sizeof vp, "/Content%s", lb->path + rl);
                    for (char* c = vp; *c; c++)
                        if (*c == '\\') *c = '/';
                    bp_editor_open(vp);
                } else editor_status(ed, now, TR("The level must be inside the project's Content folder to have a Level Blueprint."));
            }
        }
        igSeparator();
        igTextDisabled("%s", TR("New classes: Content Browser > Add > Blueprint Class"));
        igEndPopup();
    }
    igSameLine(0, 4);
    if (tb_icon_menu("tb_cine", ICON_SEQUENCE, TH(TK_ASSET_ANIMATION), TR("Cinematics"), TR("Movie Render Queue and Take Recorder; Level Sequences are made in the Content Browser")))
        igOpenPopup_Str("tb_cine_menu", 0);
    if (igBeginPopup("tb_cine_menu", 0)) {
        ui_menu_item_ptr(ICON_CAMERA, TR("Cinematics"), NULL, &g_show_cine, true); // (B9)
        igTextDisabled("%s", TR("New Level Sequence: Content Browser > Add > Level Sequence"));
        igEndPopup();
    }

    // Play / Pause / Stop / Eject, centered like Unreal's (the target VR: Play is the VR Preview)
    float h = igGetFrameHeight();
    float group_w = h * 1.25f * 4 + h * 0.9f + 4 * 5;
    igSameLine(0, 0);
    float x = igGetCursorPosX() + igGetContentRegionAvail().x * 0.5f - group_w * 0.5f;
    if (x > igGetCursorPosX()) igSetCursorPosX(x);
    bool vr = look_target() == LOOK_VR;
    if (play_button("##play", 0, TH(TK_ACCENT2), false, !ed->playing, vr ? TR("VR Preview: plays the level on the headset (the target is VR)") : TR("Play In Editor (Alt+P)"))) {
        if (vr) editor_vr_preview(ed, now);
        else pie_multi_play(ed, now);
    }
    igSameLine(0, 4);
    if (play_button("##pause", 1, TH(TK_TEXT), ed->paused, ed->playing, TR("Pause / resume (Pause)"))) ed->paused = !ed->paused;
    igSameLine(0, 4);
    if (play_button("##stop", 2, TH(TK_ERROR), false, ed->playing, TR("Stop (Esc)"))) editor_stop(ed, now);
    igSameLine(0, 4);
    if (play_button("##eject", 3, TH(TK_TEXT), ed->playing && !ed->possessed, ed->playing, ed->possessed ? TR("Eject (F8)") : TR("Possess (F8)")))
        editor_set_possessed(ed, !ed->possessed);
    igSameLine(0, 2);
    igPushStyleColor_U32(ImGuiCol_Button, THEME_NONE);
    if (igArrowButton("##playopts", ImGuiDir_Down)) igOpenPopup_Str("play_opts", 0); // (Unreal's Play options: Number of Players)
    igPopStyleColor(1);
    if (igBeginPopup("play_opts", 0)) {
        igSeparatorText(TR("Modes"));
        if (ui_menu_item(ICON_GAMEPAD, TR("Selected Viewport"), "Alt+P", false, !ed->playing)) pie_multi_play(ed, now);
        if (ui_menu_item(ICON_GLOBE, TR("Simulate"), "Alt+S", false, !ed->playing)) editor_simulate(ed, now);
        if (ui_menu_item(ICON_GAMEPAD, TR("VR Preview"), NULL, false, !ed->playing)) editor_vr_preview(ed, now); // (Q25)
        igSetItemTooltip("%s", TR("Plays the level on the VR headset (Quest Link, Air Link, SteamVR) in a window of its own, the VR pawn's locomotion, hands and laser on."));
        igSeparatorText(TR("Multiplayer Options"));
        igSetNextItemWidth(igGetFontSize() * 8.0f);
        igSliderInt(TR("Number of Players"), &g_pie_players, 1, 4, "%d", 0);
        igTextDisabled("%s", TR("More than 1: this editor hosts (Listen Server) and each other player opens in a window of its own that joins it."));
        igEndPopup();
    }
    if (ed->playing && g_pie_nclients) {
        igSameLine(0, 8);
        igTextDisabled(TR("%d players"), online_player_count());
    }

    // Platforms (Unreal's): package for each platform, the chosen target's first
    tb_separator();
    static const UiIcon tic[LOOK_TARGET_COUNT] = { ICON_MONITOR, ICON_ANDROID, ICON_VR };
    if (ui_dropdown_button("tb_platforms", tic[look_target()], TR("Platforms"), TH(TK_TEXT), TR("Package the game for Windows, Android or Meta Quest; the platforms' readiness")))
        igOpenPopup_Str("tb_platforms_menu", 0);
    if (igBeginPopup("tb_platforms_menu", 0)) {
        platforms_menu(ed, now);
        igEndPopup();
    }

    // Settings (Engine Scalability), on the right
    const char* label = TR("Settings");
    float bw = h * 0.35f + h * 0.62f + 6.0f + igCalcTextSize(label, NULL, true, -1).x + 4.0f + h * 0.45f + h * 0.3f;
    igSameLine(0, 0);
    float rx = igGetCursorPosX() + igGetContentRegionAvail().x - bw;
    if (rx > igGetCursorPosX()) igSetCursorPosX(rx);
    if (ui_dropdown_button("tb_settings", ICON_SETTINGS, label, TH(TK_TEXT), TR("Engine Scalability, VSync")))
        igOpenPopup_Str("tb_settings_menu", 0);
    igPopStyleColor(1);
    igPopStyleVar(1); // (the popups: the normal padding)
    if (igBeginPopup("tb_settings_menu", 0)) {
        igSeparatorText(TR("Engine Scalability"));
        for (int i = 0; i < RENDER_SCALABILITY_LEVELS; i++) // (Cinematic: the Quality mode's)
            if (ui_menu_item(ICON_STATS, TR(render_scalability_name(i)), NULL, ed->scalability == i, true)) set_scalability(ed, i, now);
        igSeparatorText(TR("Global Illumination"));
        igBeginDisabled(!gi_available());
        ui_menu_item_ptr(ICON_VM_GI, TR("Dynamic GI (probes)"), NULL, &ed->gi.probes, true);
        ui_menu_item_ptr(ICON_VM_METALLIC, TR("Traced Reflections"), NULL, &ed->gi.reflections, true);
        ui_menu_item_ptr(ICON_SUN, TR("Ray Traced Shadows"), NULL, &ed->gi.rt_shadows, true);
        ui_menu_item_ptr(ICON_SUN, TR("Virtual Shadow Maps (Sun)"), NULL, &ed->gi.vsm_sun, true);
        igSetNextItemWidth(igGetFontSize() * 10);
        igSliderFloat(TR("GI Intensity"), &ed->gi.intensity, 0.0f, 4.0f, "%.2f", 0);
        igSetNextItemWidth(igGetFontSize() * 10);
        igSliderFloat(TR("Reflection Max Roughness"), &ed->gi.reflection_roughness, 0.05f, 1.0f, "%.2f", 0);
        igEndDisabled();
        if (!gi_available()) igTextDisabled("%s", TR("Needs hardware ray tracing."));
        ui_menu_item_ptr(ICON_VM_LIGHTING, TR("Contact AO"), NULL, &ed->gi.contact_ao, true);
        igSetNextItemWidth(igGetFontSize() * 10);
        igSliderFloat(TR("Aerial Perspective Scale"), &ed->aerial_scale, 0.0f, 1000.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        igSetItemTooltip("%s", TR("The air between the camera and the scene: 1 is physical (km to matter), 0 turns it off."));
        igSeparator();
        bool vs = ed->vk->vsync;
        if (ui_menu_item_ptr(ICON_TIMER, TR("VSync"), NULL, &vs, true)) set_vsync(ed, vs, now);
        igEndPopup();
    }
    igPopStyleVar(1);
}

// --- Viewport toolbar (floats on the viewport, like Unreal's) ---

static bool small_toggle(const char* id, UiIcon icon, bool on, const char* tip) {
    return ui_icon_button(id, icon, NULL, on ? TH(TK_N15) : TH(TK_N12), on, tip);
}

static void viewport_toolbar(Editor* ed, float x, float y, float w) {
    float h = igGetFrameHeight() + 8.0f;
    (void)h;
    // items inside the viewport, after its invisible button (which allows the overlap): they get the clicks
    igSetCursorScreenPos((ImVec2){ x + 6, y + 10 });
    igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, (ImVec2){ 3, 4 });
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){ 8, 8 }); // the popups (the viewport has none)
    igPushStyleColor_Vec4(ImGuiCol_Button, THVA(TK_BG_APP, 0.88f)); // (Q26: dark pills over the scene, rounder)
    igPushStyleVar_Float(ImGuiStyleVar_FrameRounding, TMV(TM_RADIUS_PANEL));
    igBeginGroup();
    if (ui_icon_button("vp_menu", ICON_MENU, NULL, TH(TK_N14), false, TR("Viewport options"))) igOpenPopup_Str("vp_menu_pop", 0);
    if (igBeginPopup("vp_menu_pop", 0)) {
        // Unreal's: Realtime (the viewport moves: animations, particles) and Simulate (the game runs, not possessed)
        ui_menu_item_ptr(ICON_TIMER, TR("Realtime"), "Ctrl+R", &ed->realtime, !ed->playing);
        igSetItemTooltip("%s", TR("Animations and particles play in the viewport without Play (off: the scene holds still, less GPU)."));
        if (ui_menu_item(ICON_GLOBE, ed->simulating ? TR("Stop Simulating") : TR("Simulate"), "Alt+S", ed->simulating, !ed->playing || ed->simulating))
            editor_simulate(ed, re_time_seconds());
        igSetItemTooltip("%s", TR("Runs the game here without the player: physics, AI, Blueprints, animations. Esc stops and the level comes back as it was."));
        igSeparator();
        ui_menu_item_ptr(ICON_GAMEPAD, TR("Game View"), "G", &ed->game_view, true);
        ui_menu_item_ptr(ICON_STATS, TR("Stats"), NULL, &ed->show_stats, true);
        ui_menu_item_ptr(ICON_EXPOSURE, TR("Auto Exposure"), NULL, &ed->auto_exposure, true);
        igSetNextItemWidth(igGetFontSize() * 10);
        if (!ed->auto_exposure) {
            igSliderFloat(TR("EV100"), &ed->exposure_ev100, -6.0f, 20.0f, "%.1f", 0);
            igSetItemTooltip("%s", TR("The camera's exposure value at ISO 100: 15 full sun, 12 overcast, 7 indoors, -2 moonlight."));
            igSetNextItemWidth(igGetFontSize() * 10);
        }
        igSliderFloat(TR("Exposure Compensation"), &ed->exposure_comp, -5.0f, 5.0f, "%+.1f EV", 0);
        if (ed->auto_exposure) {
            igSetNextItemWidth(igGetFontSize() * 10);
            igDragFloatRange2(TR("Min / Max EV100"), &ed->auto_min_ev100, &ed->auto_max_ev100, 0.1f, -10.0f, 22.0f, "%.1f", "%.1f", 0);
        }
        igEndPopup();
    }
    // the camera: Perspective (its field of view) or one of the orthographic views (sugestÃ£o #66), Unreal's menu
    igSameLine(0, 3);
    int proj = ed->vp_proj >= 0 && ed->vp_proj < VP_PROJ_COUNT ? ed->vp_proj : 0;
    if (ui_dropdown_button("vp_persp", proj ? ICON_GRID : ICON_PERSPECTIVE, TR(g_vp_proj_names[proj]), TH(TK_N14), TR("Viewport camera")))
        igOpenPopup_Str("vp_persp_pop", 0);
    if (igBeginPopup("vp_persp_pop", 0)) {
        static const char* const keys[VP_PROJ_COUNT] = { "Alt+G", "Alt+J", "Alt+Shift+J", "Alt+K", "Alt+Shift+K", "Alt+H", "Alt+Shift+H" };
        igSeparatorText(TR("Perspective"));
        if (ui_menu_item(ICON_PERSPECTIVE, TR("Perspective"), keys[0], proj == 0, true)) viewport_set_projection(ed, VP_PROJ_PERSPECTIVE);
        igSeparatorText(TR("Orthographic"));
        for (int p = 1; p < VP_PROJ_COUNT; p++)
            if (ui_menu_item(ICON_GRID, TR(g_vp_proj_names[p]), keys[p], proj == p, true)) viewport_set_projection(ed, p);
        igSeparator();
        igSetNextItemWidth(igGetFontSize() * 10);
        if (proj == 0) igSliderFloat(TR("Field of View"), &ed->cam.fov_deg, 30.0f, 120.0f, "%.0f\xc2\xb0", 0);
        else igDragFloat(TR("Ortho Half Height"), &ed->vp_ortho, ed->vp_ortho * 0.01f, 50.0f, 1.0e6f, "%.0f cm", 0);
        igEndPopup();
    }
    // the view mode: its icon, its name, the menu of all of them (each with its icon, like Unreal's)
    igSameLine(0, 3);
    if (ui_dropdown_button("vp_vm", (UiIcon)(ICON_VM_LIT + ed->view_mode), TR(g_view_mode_names[ed->view_mode]), TH(TK_N15),
                           TR("View Mode")))
        igOpenPopup_Str("vp_modes", 0);
    if (igBeginPopup("vp_modes", 0)) {
        igSeparatorText(TR("View Mode"));
        static const char* const keys[VIEW_MODE_COUNT] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "P", NULL };
        for (int i = 0; i < VIEW_MODE_COUNT; i++) {
            if (i == VIEW_SOLID) igSeparatorText(TR("Blender"));
            if (i == VIEW_ROUGHNESS) igSeparatorText(TR("Buffer Visualization"));
            if (i == VIEW_PATH_TRACING) igSeparatorText(TR("Global Illumination"));
            if (i == VIEW_LOD) igSeparatorText(TR("Real Geometry"));
            if (i == VIEW_SURFACE_CACHE) igSeparatorText(TR("RealGI"));
            if (i == VIEW_LIGHT_COMPLEXITY) igSeparatorText(TR("Performance"));
            if (i == VIEW_PATH_TRACING && !gi_available()) {
                igTextDisabled("%s", TR("Needs hardware ray tracing."));
                i = VIEW_GI_ONLY; // the rest needs no ray tracing
                continue;
            }
            if (ui_menu_item((UiIcon)(ICON_VM_LIT + i), TR(g_view_mode_names[i]), keys[i], ed->view_mode == (uint32_t)i, true))
                ed->view_mode = (uint32_t)i;
        }
        igEndPopup();
    }
    igSameLine(0, 3);
    if (ui_dropdown_button("vp_show", ICON_EYE, TR("Show"), TH(TK_N14), TR("What the viewport shows"))) igOpenPopup_Str("vp_show", 0);
    if (igBeginPopup("vp_show", 0)) {
        ui_menu_item_ptr(ICON_GRID, TR("Grid"), NULL, &ed->show_grid, true);
        ui_menu_item_ptr(ICON_BULB, TR("Actor Icons"), NULL, &ed->show_icons, true);
        ui_menu_item_ptr(ICON_GRAPH, TR("AI Debug (')"), NULL, &ed->show_ai, true);
        ui_menu_item_ptr(ICON_MOVEMENT, TR("Navigation (P)"), NULL, &ed->show_nav, true);
        ui_menu_item_ptr(ICON_STATS, TR("Stats"), NULL, &ed->show_stats, true);
        igEndPopup();
    }

    // right side: the tools, snapping, the camera's speed
    float bh = igGetFrameHeight();
    float right_w = bh * 6 + 3 * 5 + 14 + (bh + igGetFontSize() * 3.0f) * 3 + 3 * 5 + 14 + igGetFontSize() * 4.5f + 3 + bh; // (the pivot's button too)
    igSameLine(0, 0);
    float rx = igGetCursorPosX() + igGetContentRegionAvail().x - right_w - 6.0f;
    if (rx > igGetCursorPosX()) igSetCursorPosX(rx);
    static const UiIcon ti[4] = { ICON_SELECT, ICON_MOVE, ICON_ROTATE, ICON_SCALE };
    static const char* const tt[4] = { "Select (Q)", "Move (W)", "Rotate (E)", "Scale (R)" };
    for (int i = 0; i < 4; i++) {
        char id[16];
        snprintf(id, sizeof id, "vp_tool%d", i);
        if (small_toggle(id, ti[i], ed->tool == (EdTool)i, TR(tt[i]))) ed->tool = (EdTool)i;
        igSameLine(0, 3);
    }
    // (2026-11-23) the gizmo's axes (the globe: World, the cube: Local, the camera: View) and its pivot, as the Tlender's
    // header: a click opens the choices (Ctrl+` still toggles World / Local)
    {
        static const UiIcon oi[GIZMO_ORIENT_COUNT] = { ICON_GLOBE, ICON_CUBE, ICON_CAMERA };
        static const char* const on[GIZMO_ORIENT_COUNT] = { "World", "Local", "View" };
        static const char* const ot[GIZMO_ORIENT_COUNT] = { "Along the world's axes", "Along the active actor's own axes",
                                                            "Along the screen: X right, Y up, Z into the screen" };
        int o = ed->gizmo_orient >= 0 && ed->gizmo_orient < GIZMO_ORIENT_COUNT ? ed->gizmo_orient : 0;
        if (small_toggle("vp_coord", oi[o], o != GIZMO_ORIENT_WORLD, TR("Transform Orientation: the gizmo's axes (Ctrl+`: World / Local)")))
            igOpenPopup_Str("vp_coord_pop", 0);
        if (igBeginPopup("vp_coord_pop", 0)) {
            igSeparatorText(TR("Transform Orientation"));
            for (int k = 0; k < GIZMO_ORIENT_COUNT; k++) {
                if (ui_menu_item(oi[k], TR(on[k]), NULL, o == k, true)) ed->gizmo_orient = k;
                igSetItemTooltip("%s", TR(ot[k]));
            }
            igEndPopup();
        }
        igSameLine(0, 3);
        static const char* const pn[GIZMO_PIVOT_COUNT] = { "Active Actor", "Median Point", "Bounding Box Center", "Individual Origins" };
        static const char* const pt[GIZMO_PIVOT_COUNT] = {
            "Rotate and scale about the active actor (the last one selected)",
            "Rotate and scale about the mean of the selected actors' places",
            "Rotate and scale about the center of the box around the selected actors",
            "Each actor rotates and scales about its own origin" };
        int pv = ed->gizmo_pivot >= 0 && ed->gizmo_pivot < GIZMO_PIVOT_COUNT ? ed->gizmo_pivot : 0;
        if (small_toggle("vp_pivot", ICON_FOCUS, pv != GIZMO_PIVOT_ACTIVE || ed->gizmo_only_loc, TR("Pivot Point: where rotate and scale turn")))
            igOpenPopup_Str("vp_pivot_pop", 0);
        if (igBeginPopup("vp_pivot_pop", 0)) {
            igSeparatorText(TR("Pivot Point"));
            for (int k = 0; k < GIZMO_PIVOT_COUNT; k++) {
                if (ui_menu_item(ICON_FOCUS, TR(pn[k]), NULL, pv == k, true)) ed->gizmo_pivot = k;
                igSetItemTooltip("%s", TR(pt[k]));
            }
            igSeparator();
            ui_menu_item_ptr(ICON_MOVE, TR("Only Locations"), NULL, &ed->gizmo_only_loc, true);
            igSetItemTooltip("%s", TR("Rotate and scale move the actors about the pivot without turning or sizing them"));
            igEndPopup();
        }
    }
    igSameLine(0, 3);
    igSameLine(0, 14);
    struct { const char* id; UiIcon icon; bool* on; float* step; const char* tip; const float* opts; int n; const char* fmt; } snaps[3] = {
        { "vp_snap_m", ICON_GRID, &ed->snap_move, &ed->snap_move_step, "Grid snap (cm)", (const float[]){ 1, 5, 10, 50, 100, 500 }, 6, "%.0f" },
        { "vp_snap_r", ICON_ANGLE, &ed->snap_rotate, &ed->snap_rotate_step, "Rotation snap (degrees)", (const float[]){ 5, 10, 15, 30, 45, 90 }, 6, "%.0f\xc2\xb0" },
        { "vp_snap_s", ICON_SCALE, &ed->snap_scale, &ed->snap_scale_step, "Scale snap", (const float[]){ 0.0625f, 0.125f, 0.25f, 0.5f, 1.0f }, 5, "%.3g" },
    };
    for (int s = 0; s < 3; s++) {
        if (small_toggle(snaps[s].id, snaps[s].icon, *snaps[s].on, TR(snaps[s].tip))) *snaps[s].on = !*snaps[s].on;
        igSameLine(0, 1);
        char lab[32];
        snprintf(lab, sizeof lab, snaps[s].fmt, *snaps[s].step);
        char bid[48];
        snprintf(bid, sizeof bid, "%s##%s_v", lab, snaps[s].id);
        if (igButton(bid, (ImVec2){ igGetFontSize() * 3.0f, 0 })) igOpenPopup_Str(snaps[s].id, 0);
        if (igBeginPopup(snaps[s].id, 0)) {
            for (int k = 0; k < snaps[s].n; k++) {
                snprintf(lab, sizeof lab, snaps[s].fmt, snaps[s].opts[k]);
                if (ui_menu_item(ICON_SNAP, lab, NULL, *snaps[s].step == snaps[s].opts[k], true)) *snaps[s].step = snaps[s].opts[k], *snaps[s].on = true;
            }
            igEndPopup();
        }
        igSameLine(0, 3);
    }
    igSameLine(0, 14);
    igSetNextItemWidth(igGetFontSize() * 4.5f);
    igDragFloat("##camspeed", &ed->cam.speed, 5.0f, 10.0f, 20000.0f, "%.0f cm/s", ImGuiSliderFlags_Logarithmic);
    igSetItemTooltip("%s", TR("Camera speed (or the wheel while flying)"));
    igSameLine(0, 3);
    if (small_toggle("vp_layout", ICON_PANEL, ed->show_view2, TR("A second viewport in a window of its own (Window > Viewport 2)"))) ed->show_view2 = !ed->show_view2;
    igEndGroup();
    igPopStyleVar(1);
    igPopStyleColor(1);
    igPopStyleVar(2);
}

// --- Outliner ---

static bool name_matches(const char* name, const char* filter) {
    if (!filter[0]) return true;
    for (const char* s = name; *s; s++) {
        size_t k = 0;
        while (filter[k] && s[k] && tolower((unsigned char)s[k]) == tolower((unsigned char)filter[k])) k++;
        if (!filter[k]) return true;
    }
    return false;
}

static const char* type_label(World* w, Entity e) {
    const CLight* l = ecs_get(w, e, COMP_LIGHT);
    if (l) return g_light_names[l->type < LIGHT_TYPE_COUNT ? l->type : 1];
    if (ecs_get(w, e, COMP_STATIC_MESH)) return "Static Mesh Actor";
    if (ecs_get(w, e, COMP_CAMERA)) return "Camera Actor";
    if (ecs_get(w, e, COMP_PLAYER_START)) return "Player Start";
    if (ecs_get(w, e, COMP_FOG)) return "Exponential Height Fog";
    if (ecs_get(w, e, COMP_CLOUD)) return "Volumetric Cloud";
    if (ecs_get(w, e, COMP_POST_PROCESS)) return "Post Process Volume";
    if (ecs_get(w, e, COMP_CROWD)) return "Crowd Spawner";
    if (ecs_get(w, e, COMP_LANDSCAPE)) return "Landscape";
    if (ecs_get(w, e, COMP_FOLIAGE)) return "Foliage";
    if (ecs_get(w, e, COMP_PROTO)) return "Prototype";
    if (ecs_get(w, e, COMP_AUDIO_VOLUME)) return "Audio Volume";
    if (ecs_get(w, e, COMP_DECAL)) return "Decal Actor";
    const CNav* nav = ecs_get(w, e, COMP_NAV);
    if (nav) return nav->kind == NAV_LINK_PROXY ? "Nav Link Proxy" : nav->kind == NAV_MODIFIER_VOLUME ? "Nav Modifier Volume" : "Nav Mesh Bounds Volume";
    if (ecs_get(w, e, COMP_PARTICLE_SYSTEM)) return "Particle System";
    if (ecs_get(w, e, COMP_VEHICLE)) return "Vehicle";
    if (ecs_get(w, e, COMP_WATER)) {
        const CWater* cw = ecs_get(w, e, COMP_WATER);
        return cw->exclusion ? "Water Body Exclusion" : cw->ocean ? "Ocean" : "Water Body";
    }
    if (ecs_get(w, e, COMP_RAIN)) return "Rain";
    if (ecs_get(w, e, COMP_CLOTH)) return ((const CCloth*)ecs_get(w, e, COMP_CLOTH))->kind == CLOTH_KIND_SOFT ? "Soft Body" : "Cloth";
    if (ecs_get(w, e, COMP_RAIL)) return g_spline_kind_names[spline_kind(ecs_get(w, e, COMP_RAIL))]; // (Q14: Rail, Spline, Cable)
    if (ecs_get(w, e, COMP_PROC_WORLD)) return pcg_is_component(w, e) ? "PCG Volume" : "Procedural World";
    if (ecs_get(w, e, COMP_AUDIO)) return "Ambient Sound";
    const CCollision* tg = ecs_get(w, e, COMP_COLLISION);
    if (tg) return tg->shape == COLLISION_SPHERE ? "Trigger Sphere" : tg->shape == COLLISION_CAPSULE ? "Trigger Capsule" : "Trigger Box";
    return "Actor";
}

bool editor_actor_thumb(World* w, Entity e, ImDrawList* dl, float x0, float y0, float x1, float y1) {
    const CSkeletalMesh* sk = ecs_get(w, e, COMP_SKELETAL_MESH);
    if (sk && sk->mesh) {
        const SkelMesh* m = anim_mesh(sk->mesh);
        const char* root = content_root();
        size_t rn = root ? strlen(root) : 0;
        if (!m || !rn || _strnicmp(m->path, root, rn)) return false;
        char vp[600];
        snprintf(vp, sizeof vp, "/Content%s", m->path + rn); // (its file under the Content folder: its vpath)
        for (char* c = vp; *c; c++)
            if (*c == '\\') *c = '/';
        return asset_thumb(dl, x0, y0, x1, y1, vp, ASSET_SKELETAL_MESH);
    }
    const CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    if (sm && render_mesh_slot(sm->mesh) >= SHAPE_COUNT) { // an imported mesh (the basic shapes keep their icons)
        char ref[32];
        snprintf(ref, sizeof ref, "#mesh/%x", sm->mesh);
        return asset_thumb(dl, x0, y0, x1, y1, ref, ASSET_STATIC_MESH);
    }
    return false;
}

static UiIcon type_icon(World* w, Entity e) {
    const CLight* l = ecs_get(w, e, COMP_LIGHT);
    if (l) {
        static const UiIcon li[LIGHT_TYPE_COUNT] = { ICON_SUN, ICON_BULB, ICON_SPOT, ICON_RECT, ICON_SKY };
        return li[l->type < LIGHT_TYPE_COUNT ? l->type : 1];
    }
    const CStaticMesh* m = ecs_get(w, e, COMP_STATIC_MESH);
    if (m) return (UiIcon)(ICON_CUBE + (m->mesh < SHAPE_COUNT ? m->mesh : 0));
    if (ecs_get(w, e, COMP_CAMERA)) return ICON_CAMERA;
    if (ecs_get(w, e, COMP_PLAYER_START)) return ICON_PLAYER;
    if (ecs_get(w, e, COMP_FOG)) return ICON_GLOBE;
    if (ecs_get(w, e, COMP_CLOUD)) return ICON_SKY;
    if (ecs_get(w, e, COMP_POST_PROCESS)) return ICON_EXPOSURE;
    if (ecs_get(w, e, COMP_CROWD)) return ICON_CHARACTER;
    if (ecs_get(w, e, COMP_LANDSCAPE)) return ICON_FLOOR;
    if (ecs_get(w, e, COMP_FOLIAGE)) return ICON_FLOOR;
    if (ecs_get(w, e, COMP_PROTO)) return ICON_CUBE;
    if (ecs_get(w, e, COMP_AUDIO) || ecs_get(w, e, COMP_AUDIO_VOLUME)) return ICON_SOUND;
    if (ecs_get(w, e, COMP_NAV)) return ICON_MOVEMENT;
    if (ecs_get(w, e, COMP_PARTICLE_SYSTEM)) return ICON_BULB;
    if (ecs_get(w, e, COMP_RAIN)) return ICON_SKY;
    if (ecs_get(w, e, COMP_CLOTH)) return ICON_PLANE;
    if (ecs_get(w, e, COMP_WATER)) return ICON_GLOBE;
    return ICON_ACTOR;
}

static void panel_header(const char* title) {
    if (igIsWindowDocked()) return; // (Q2: the dock's tab names the panel)
    ImVec2 p = igGetCursorScreenPos();
    float h = igGetFrameHeight();
    ImVec2 ts = igCalcTextSize(title, NULL, true, -1);
    ImDrawList_AddRectFilled(igGetWindowDrawList(), (ImVec2){ p.x - 6, p.y - 6 }, (ImVec2){ p.x + ts.x + 18, p.y + h + 2 },
                             TH(TK_N7), 4.0f, ImDrawFlags_RoundCornersTop);
    ImDrawList_AddRectFilled(igGetWindowDrawList(), (ImVec2){ p.x - 6, p.y - 6 }, (ImVec2){ p.x + ts.x + 18, p.y - 4 },
                             TH(TK_ACCENT), 0, 0);
    igSetCursorScreenPos((ImVec2){ p.x + 6, p.y });
    igAlignTextToFramePadding();
    igTextUnformatted(title, NULL);
}

// --- The Outliner's folders (Unreal's: colored, nested, actors dragged into them) ---

static const float g_folder_colors[10][3] = { { 0.85f, 0.65f, 0.25f }, { 0.85f, 0.3f, 0.25f }, { 0.95f, 0.55f, 0.2f }, { 0.9f, 0.85f, 0.3f },
                                              { 0.35f, 0.8f, 0.35f }, { 0.25f, 0.75f, 0.75f }, { 0.3f, 0.55f, 0.95f }, { 0.6f, 0.4f, 0.9f },
                                              { 0.9f, 0.45f, 0.75f }, { 0.65f, 0.65f, 0.65f } };

static Entity folder_entity(World* w, uint32_t id) {
    for (uint32_t ai = 0; id && ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_OUTLINER_FOLDER))) continue;
        const COutlinerFolder* f = ecs_arch_column(a, COMP_OUTLINER_FOLDER);
        for (uint32_t i = 0; i < a->count; i++)
            if (f[i].id == id) return ecs_arch_entities(a)[i];
    }
    return ENTITY_NONE;
}
static uint32_t actor_folder(World* w, Entity e) {
    const CFolder* f = ecs_get(w, e, COMP_FOLDER);
    return f && folder_entity(w, f->id) ? f->id : 0;
}
static void set_actor_folder(World* w, Entity e, uint32_t id) {
    if (id && !ecs_get(w, e, COMP_FOLDER)) ecs_add(w, e, COMP_FOLDER);
    CFolder* f = ecs_get(w, e, COMP_FOLDER);
    if (f) f->id = id;
    ecs_touch(w, e);
}
static uint32_t new_folder(Editor* ed, const char* name, uint32_t parent) {
    World* w = &ed->world;
    Entity fe = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_OUTLINER_FOLDER));
    if (fe == ENTITY_NONE) return 0;
    COutlinerFolder* f = ecs_get(w, fe, COMP_OUTLINER_FOLDER);
    static uint32_t seq;
    f->id = (uint32_t)(re_time_seconds() * 1000.0) * 2654435761u ^ ++seq * 40503u;
    if (!f->id) f->id = 1;
    f->parent = parent;
    uint32_t nf = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++)
        if (w->archetypes[ai].mask & COMP_BIT(COMP_OUTLINER_FOLDER)) nf += w->archetypes[ai].count;
    memcpy(f->color, g_folder_colors[nf % 10], 12);
    CName* n = ecs_get(w, fe, COMP_NAME);
    snprintf(n->text, sizeof n->text, "%s", name);
    ecs_touch(w, fe);
    return f->id;
}
static bool folder_under(World* w, uint32_t id, uint32_t ancestor) { // id is ancestor or inside it
    for (int g = 0; id && g < 64; g++) {
        if (id == ancestor) return true;
        Entity fe = folder_entity(w, id);
        const COutlinerFolder* f = fe ? ecs_get(w, fe, COMP_OUTLINER_FOLDER) : NULL;
        id = f ? f->parent : 0;
    }
    return false;
}
// The folder goes: what was in it (actors and folders) moves up to its parent
static void delete_folder(Editor* ed, uint32_t id) {
    World* w = &ed->world;
    Entity fe = folder_entity(w, id);
    if (!fe) return;
    uint32_t parent = ((COutlinerFolder*)ecs_get(w, fe, COMP_OUTLINER_FOLDER))->parent;
    for (uint32_t i = 1; i < w->slot_count; i++) {
        Entity e = ecs_entity_at(w, i);
        if (e == ENTITY_NONE) continue;
        CFolder* cf = ecs_get(w, e, COMP_FOLDER);
        if (cf && cf->id == id) cf->id = parent, ecs_touch(w, e);
        COutlinerFolder* of = ecs_get(w, e, COMP_OUTLINER_FOLDER);
        if (of && of->parent == id) of->parent = parent, ecs_touch(w, e);
    }
    ecs_destroy(w, fe);
}

static int folder_cmp(const void* x, const void* y) {
    return _stricmp(((const CName*)((const void* const*)x)[1])->text, ((const CName*)((const void* const*)y)[1])->text);
}

// The rows in the folders' order: each folder (by name), its subfolders and actors (when open), then the loose actors
static void outliner_tree(Editor* ed, Entity* fents, uint32_t nf, uint32_t parent, uint8_t depth, const uint32_t* head, const uint32_t* next) {
    World* w = &ed->world;
    for (uint32_t k = 0; k < nf && ed->ol_nrows < ed->ol_cap; k++) {
        const COutlinerFolder* f = ecs_get(w, fents[k], COMP_OUTLINER_FOLDER);
        uint32_t fp = f->parent && folder_entity(w, f->parent) ? f->parent : 0;
        if (fp != parent) continue;
        ed->ol_depth[ed->ol_nrows] = depth;
        ed->ol_rows[ed->ol_nrows++] = fents[k];
        if (f->flags & FOLDER_COLLAPSED) continue;
        outliner_tree(ed, fents, nf, f->id, (uint8_t)(depth + 1), head, next);
        for (uint32_t s = head[k]; s && ed->ol_nrows < ed->ol_cap; s = next[s]) { // its actors (by slot)
            ed->ol_depth[ed->ol_nrows] = (uint8_t)(depth + 1);
            ed->ol_rows[ed->ol_nrows++] = ecs_entity_at(w, s);
        }
    }
}

static void outliner(Editor* ed, double now) {
    World* w = &ed->world;
    panel_header(TR("Outliner"));
    static uint32_t s_level_rev; // (2026-11-17, Q3) the level shown (Window > Levels): the list made again when it changes
    uint32_t level_rev = levels_outliner_revision();
    igSameLine(0, 18);
    igSetNextItemWidth(-igGetFrameHeight() * 2 - 8);
    igInputTextWithHint("##filter", TR("Search..."), ed->filter, sizeof ed->filter, 0, NULL, NULL);
    igSameLine(0, 4);
    if (ui_icon_button("ol_add", ICON_PLUS, NULL, TH(TK_LIME), false, TR("Add an actor"))) igOpenPopup_Str("ol_add_menu", 0);
    if (igBeginPopup("ol_add_menu", 0)) {
        quick_add_items(ed, now);
        igEndPopup();
    }
    igSameLine(0, 4);
    if (ui_icon_button("ol_folder", ICON_FOLDER, NULL, TH(TK_AMBER), false, TR("New folder (the selected actors go in it)"))) {
        editor_undo_push(ed);
        uint32_t id = new_folder(ed, "NewFolder", ed->ol_folder_sel);
        for (uint32_t s = 0; id && s < ed->nsel; s++) set_actor_folder(w, ed->sel[s], id);
        Entity fe = folder_entity(w, id);
        if (fe) begin_rename(ed, fe);
    }

    // the rows: every actor matching the filter, in creation order (folders: their tree); made again only when the
    // world changed (a million actors: walked once per edit, not per frame)
    levels_outliner_header(ed); // (with sublevels: "Level: Day v", only its actors listed; All Levels: every one)
    if (!ed->ol_valid || ed->ol_changes != w->changes || ed->ol_revision != w->revision || strcmp(ed->ol_filter, ed->filter) || s_level_rev != level_rev) {
        s_level_rev = level_rev;
        bool by_level = levels_outliner_level() != -2;
        if (ed->ol_cap < w->alive + 1) {
            Entity* r = realloc(ed->ol_rows, sizeof(Entity) * (w->alive + 1)); // the world grew (an edit, not a frame)
            uint8_t* d = realloc(ed->ol_depth, w->alive + 1);
            if (r) ed->ol_rows = r;
            if (d) ed->ol_depth = d;
            if (r && d) ed->ol_cap = w->alive + 1;
        }
        ed->ol_nrows = 0;
        // the folders, by name
        enum { MAXF = 512 };
        static Entity fents[MAXF];
        static uint32_t head[MAXF];
        uint32_t nf = 0;
        void* pairs[MAXF][2];
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            if (!(a->mask & COMP_BIT(COMP_OUTLINER_FOLDER))) continue;
            const Entity* ents = ecs_arch_entities(a);
            for (uint32_t i = 0; i < a->count && nf < MAXF; i++) pairs[nf][0] = (void*)(uintptr_t)ents[i], pairs[nf][1] = ecs_get(w, ents[i], COMP_NAME), nf++;
        }
        qsort(pairs, nf, sizeof pairs[0], folder_cmp);
        for (uint32_t k = 0; k < nf; k++) fents[k] = (Entity)(uintptr_t)pairs[k][0], head[k] = 0;
        // each folder's actors (a list per folder, by slot: the creation order)
        static uint32_t* next;
        static uint32_t next_cap;
        if (next_cap < w->slot_count + 1) {
            uint32_t* nn = realloc(next, sizeof(uint32_t) * (w->slot_count + 1));
            if (nn) next = nn, next_cap = w->slot_count + 1;
        }
        bool tree = !ed->filter[0] && !by_level && nf && next;
        uint32_t tail[MAXF] = { 0 };
        for (uint32_t i = 1; ed->ol_rows && i < w->slot_count && ed->ol_nrows < ed->ol_cap; i++) {
            Entity e = ecs_entity_at(w, i);
            if (e == ENTITY_NONE || bp_is_component(w, e) || ecs_get(w, e, COMP_OUTLINER_FOLDER)) continue; // (a Blueprint's components: in its editor)
            const CName* n = ecs_get(w, e, COMP_NAME);
            if (!name_matches(n ? n->text : "", ed->filter)) continue;
            if (by_level && !levels_outliner_shows(ed, e)) continue;
            uint32_t fid = tree ? actor_folder(w, e) : 0;
            int k = -1;
            for (uint32_t q = 0; fid && q < nf && k < 0; q++)
                if (((COutlinerFolder*)ecs_get(w, fents[q], COMP_OUTLINER_FOLDER))->id == fid) k = (int)q;
            if (k >= 0) { // later, under its folder
                next[i] = 0;
                if (tail[k]) next[tail[k]] = i;
                else head[k] = i;
                tail[k] = i;
                continue;
            }
            // (loose: listed now, moved after the folders below; only !tree kept them, so the first folder made every
            // actor outside it vanish from the list)
            ed->ol_depth[ed->ol_nrows] = 0, ed->ol_rows[ed->ol_nrows++] = e;
        }
        if (tree) { // the folders first, then the loose actors
            uint32_t loose = ed->ol_nrows;
            Entity* keep = loose ? malloc(sizeof(Entity) * loose) : NULL; // (an edit, not a frame)
            if (keep) memcpy(keep, ed->ol_rows, sizeof(Entity) * loose);
            ed->ol_nrows = 0;
            outliner_tree(ed, fents, nf, 0, 0, head, next);
            for (uint32_t i = 0; keep && i < loose && ed->ol_nrows < ed->ol_cap; i++) ed->ol_depth[ed->ol_nrows] = 0, ed->ol_rows[ed->ol_nrows++] = keep[i];
            free(keep);
        }
        ed->ol_changes = w->changes;
        ed->ol_revision = w->revision;
        snprintf(ed->ol_filter, sizeof ed->ol_filter, "%s", ed->filter);
        ed->ol_valid = true;
    }
    Entity* rows = ed->ol_rows;
    int nrows = (int)ed->ol_nrows;
    uint32_t drop_folder = UINT32_MAX; // actors / a folder dropped on a folder row this frame (0: the root)
    Entity drop_payload = ENTITY_NONE;

    float footer = igGetFrameHeightWithSpacing();
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    Entity rename_now = ENTITY_NONE, delete_now = ENTITY_NONE, duplicate_now = ENTITY_NONE;
    float icon = igGetFontSize();
    igPushStyleColor_Vec4(ImGuiCol_Header, THV(TK_ACCENT_DIM)); // Unreal's blue rows for the selection
    if (igBeginTable("outliner_table", 3, tf, (ImVec2){ 0, -footer }, 0)) {
        igTableSetupScrollFreeze(0, 1);
        igTableSetupColumn("##eye", ImGuiTableColumnFlags_WidthFixed, icon * 1.3f, 0);
        igTableSetupColumn(TR("Item Label"), ImGuiTableColumnFlags_WidthStretch, 0.62f, 0);
        igTableSetupColumn(TR("Type"), ImGuiTableColumnFlags_WidthStretch, 0.38f, 0);
        igTableHeadersRow();
        ImGuiListClipper clip; // on the stack: the C++ constructor would allocate
        memset(&clip, 0, sizeof clip);
        ImGuiListClipper_Begin(&clip, nrows, -1.0f);
        while (ImGuiListClipper_Step(&clip)) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                Entity e = rows[r];
                CName* n = ecs_get(w, e, COMP_NAME);
                CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
                COutlinerFolder* of = ecs_get(w, e, COMP_OUTLINER_FOLDER);
                float indent = (float)(ed->ol_depth ? ed->ol_depth[r] : 0) * icon * 1.1f;
                igTableNextRow(0, 0);
                igPushID_Int((int)e);
                igTableNextColumn();
                if (of) { // a folder row: open / closed, its colored icon, drops take actors in
                    igTableNextColumn();
                    if (indent > 0) igSetCursorPosX(igGetCursorPosX() + indent);
                    ImVec2 p = igGetCursorScreenPos();
                    bool collapsed = (of->flags & FOLDER_COLLAPSED) != 0;
                    if (igInvisibleButton("fold", (ImVec2){ icon, icon }, 0)) of->flags ^= FOLDER_COLLAPSED, ed->ol_valid = false;
                    ImDrawList* fdl = igGetWindowDrawList();
                    float cx = p.x + icon * 0.5f, cy = p.y + icon * 0.5f, s = icon * 0.25f;
                    if (collapsed) ImDrawList_AddTriangleFilled(fdl, (ImVec2){ cx - s * 0.6f, cy - s }, (ImVec2){ cx + s, cy }, (ImVec2){ cx - s * 0.6f, cy + s }, TH(TK_N13));
                    else ImDrawList_AddTriangleFilled(fdl, (ImVec2){ cx - s, cy - s * 0.6f }, (ImVec2){ cx + s, cy - s * 0.6f }, (ImVec2){ cx, cy + s }, TH(TK_N13));
                    igSameLine(0, 2);
                    uint32_t fc = 0xFF000000u | ((uint32_t)(of->color[2] * 255) << 16) | ((uint32_t)(of->color[1] * 255) << 8) | (uint32_t)(of->color[0] * 255);
                    ui_icon_item(ICON_FOLDER, icon, fc);
                    igSameLine(0, 6);
                    if (ed->renaming == e) {
                        if (ed->rename_focus) igSetKeyboardFocusHere(0);
                        igSetNextItemWidth(-1);
                        bool done = igInputText("##rename", ed->rename_buf, sizeof ed->rename_buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll, NULL, NULL);
                        if (done && n && ed->rename_buf[0]) editor_undo_push(ed), snprintf(n->text, sizeof n->text, "%s", ed->rename_buf), ecs_touch(w, e);
                        if (done || (!ed->rename_focus && !igIsItemActive())) ed->renaming = ENTITY_NONE;
                        ed->rename_focus = false;
                    } else {
                        char label[RE_NAME_MAX + 8];
                        snprintf(label, sizeof label, "%s##frow", n ? n->text : "Folder");
                        if (igSelectable_Bool(label, ed->ol_folder_sel == of->id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick, (ImVec2){ 0, 0 })) {
                            ed->ol_folder_sel = ed->ol_folder_sel == of->id && !igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left) ? 0 : of->id;
                            if (igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) of->flags ^= FOLDER_COLLAPSED, ed->ol_valid = false;
                        }
                        if (igBeginDragDropSource(0)) { // a folder dragged into another
                            igSetDragDropPayload("RE_OL_FOLDER", &e, sizeof e, ImGuiCond_Once);
                            igText("%s", n ? n->text : "");
                            igEndDragDropSource();
                        }
                        if (igBeginDragDropTarget()) {
                            const ImGuiPayload* pl = igAcceptDragDropPayload("RE_OL_ACTORS", 0);
                            if (pl) drop_folder = of->id;
                            pl = igAcceptDragDropPayload("RE_OL_FOLDER", 0);
                            if (pl) drop_folder = of->id, drop_payload = *(const Entity*)pl->Data;
                            igEndDragDropTarget();
                        }
                        if (igBeginPopupContextItem("folder_menu", ImGuiPopupFlags_MouseButtonRight)) {
                            if (ui_menu_item(ICON_RENAME, TR("Rename"), "F2", false, true)) rename_now = e;
                            if (ui_menu_item(ICON_FOLDER, TR("Create Subfolder"), NULL, false, true)) {
                                editor_undo_push(ed);
                                uint32_t id = new_folder(ed, "NewFolder", of->id);
                                Entity fe = folder_entity(w, id);
                                if (fe) rename_now = fe;
                            }
                            if (ui_menu_item(ICON_MOVE, TR("Move Selected Actors Here"), NULL, false, ed->nsel > 0)) {
                                editor_undo_push(ed);
                                for (uint32_t s = 0; s < ed->nsel; s++) set_actor_folder(w, ed->sel[s], of->id);
                            }
                            if (ui_menu_item(ICON_SELECT, TR("Select Actors in Folder"), NULL, false, true)) {
                                editor_select_none(ed);
                                for (uint32_t i = 1; i < w->slot_count; i++) {
                                    Entity a = ecs_entity_at(w, i);
                                    if (a != ENTITY_NONE && actor_folder(w, a) && folder_under(w, actor_folder(w, a), of->id)) editor_select(ed, a, SEL_ADD);
                                }
                            }
                            if (ui_begin_menu(ICON_FOLDER, TR("Set Color"), true)) {
                                for (int c = 0; c < 10; c++) {
                                    igPushID_Int(c);
                                    if (igColorButton("##c", (ImVec4){ g_folder_colors[c][0], g_folder_colors[c][1], g_folder_colors[c][2], 1 }, 0, (ImVec2){ icon * 1.4f, icon * 1.4f }))
                                        editor_undo_push(ed), memcpy(of->color, g_folder_colors[c], 12), ecs_touch(w, e), igCloseCurrentPopup();
                                    if (c % 5 != 4) igSameLine(0, 4);
                                    igPopID();
                                }
                                if (igColorEdit3(TR("Custom"), of->color, ImGuiColorEditFlags_NoInputs)) ecs_touch(w, e);
                                igEndMenu();
                            }
                            igSeparator();
                            if (ui_menu_item(ICON_DELETE, TR("Delete Folder"), NULL, false, true)) {
                                editor_undo_push(ed);
                                delete_folder(ed, of->id);
                                ed->ol_folder_sel = 0;
                                igEndPopup();
                                igPopID();
                                continue;
                            }
                            igEndPopup();
                        }
                    }
                    igTableNextColumn();
                    igTextDisabled("%s", TR("Folder"));
                    igPopID();
                    continue;
                }
                bool hidden = t && (t->flags & TRANSFORM_HIDDEN);
                if (t) {
                    ImVec2 p = igGetCursorScreenPos();
                    if (igInvisibleButton("eye", (ImVec2){ icon, icon }, 0)) {
                        editor_undo_push(ed);
                        t->flags ^= TRANSFORM_HIDDEN;
                        ecs_touch(&ed->world, rows[r]);
                    }
                    ui_icon(igGetWindowDrawList(), hidden ? ICON_EYE_CLOSED : ICON_EYE, p.x, p.y, icon,
                            hidden ? TH(TK_N11) : TH(TK_N14));
                    igSetItemTooltip("%s", TR("Hidden in the editor (H)"));
                }
                igTableNextColumn();
                if (indent > 0) igSetCursorPosX(igGetCursorPosX() + indent);
                { // the actor's own picture (an imported model, a Skeletal Mesh in T pose), else its type's icon
                    ImVec2 ip = igGetCursorScreenPos();
                    float pad = -icon * 0.15f; // (a little bigger than the icons: a model reads at that size)
                    if (editor_actor_thumb(w, e, igGetWindowDrawList(), ip.x + pad, ip.y + pad, ip.x + icon - pad, ip.y + icon - pad))
                        igDummy((ImVec2){ icon, icon });
                    else
                        ui_icon_item(type_icon(w, e), icon, hidden ? TH(TK_N11) : TH(TK_N14));
                }
                igSameLine(0, 6);
                if (ed->renaming == e) {
                    if (ed->rename_focus) igSetKeyboardFocusHere(0);
                    igSetNextItemWidth(-1);
                    bool done = igInputText("##rename", ed->rename_buf, sizeof ed->rename_buf,
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll, NULL, NULL);
                    if (done && n && ed->rename_buf[0] && strcmp(n->text, ed->rename_buf) != 0) {
                        editor_undo_push(ed);
                        snprintf(n->text, sizeof n->text, "%s", ed->rename_buf);
                    }
                    if (done || (!ed->rename_focus && !igIsItemActive())) ed->renaming = ENTITY_NONE;
                    ed->rename_focus = false;
                } else {
                    char label[RE_NAME_MAX + 8];
                    snprintf(label, sizeof label, "%s##row", n ? n->text : "(unnamed)");
                    if (hidden) igPushStyleColor_Vec4(ImGuiCol_Text, THV(TK_N11));
                    if (igSelectable_Bool(label, editor_is_selected(ed, e), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                                          (ImVec2){ 0, 0 })) {
                        ImGuiIO* io = igGetIO_Nil();
                        if (io->KeyShift && ed->ol_anchor != ENTITY_NONE) { // the rows from the anchor to this one
                            int a = -1;
                            for (int k = 0; k < nrows && a < 0; k++)
                                if (rows[k] == ed->ol_anchor) a = k;
                            if (!io->KeyCtrl) editor_select_none(ed);
                            int lo = a < 0 ? r : a < r ? a : r, hi = a < 0 ? r : a < r ? r : a;
                            for (int k = lo; k <= hi; k++) editor_select(ed, rows[k], SEL_ADD);
                            editor_select(ed, e, SEL_ADD); // the clicked one is the primary
                        } else {
                            editor_select(ed, e, io->KeyCtrl ? SEL_TOGGLE : SEL_REPLACE);
                            ed->ol_anchor = e;
                        }
                        if (igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) rename_now = e;
                    }
                    if (igBeginDragDropSource(0)) { // the selection dragged onto a folder
                        if (!editor_is_selected(ed, e)) editor_select(ed, e, SEL_REPLACE);
                        igSetDragDropPayload("RE_OL_ACTORS", &e, sizeof e, ImGuiCond_Once);
                        igText(TR("%u actor(s) into a folder"), ed->nsel);
                        igEndDragDropSource();
                        ed->ol_marquee = false;
                    } else if (igIsItemClicked(ImGuiMouseButton_Left)) ed->ol_marquee = true, ed->ol_press_row = r;
                    if (igIsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) ed->ol_hover_row = r;
                    if (hidden) igPopStyleColor(1);
                    if (igBeginPopupContextItem("row_menu", ImGuiPopupFlags_MouseButtonRight)) {
                        if (!editor_is_selected(ed, e)) editor_select(ed, e, SEL_REPLACE);
                        if (ui_menu_item(ICON_RENAME, TR("Rename"), "F2", false, true)) rename_now = e;
                        if (ui_menu_item(ICON_DUPLICATE, TR("Duplicate"), "Ctrl+D", false, true)) duplicate_now = e;
                        if (ui_menu_item(ICON_FOCUS, TR("Focus"), "F", false, true)) viewport_focus(ed, e);
                        if (ui_begin_menu(ICON_FOLDER, TR("Move To"), true)) {
                            if (ui_menu_item(ICON_FOLDER, TR("Create New Folder"), NULL, false, true)) {
                                editor_undo_push(ed);
                                uint32_t id = new_folder(ed, "NewFolder", actor_folder(w, e));
                                for (uint32_t s = 0; id && s < ed->nsel; s++) set_actor_folder(w, ed->sel[s], id);
                                Entity fe = folder_entity(w, id);
                                if (fe) rename_now = fe;
                            }
                            if (ui_menu_item(ICON_OUTLINER, TR("None (the root)"), NULL, false, actor_folder(w, e) != 0)) {
                                editor_undo_push(ed);
                                for (uint32_t s = 0; s < ed->nsel; s++) set_actor_folder(w, ed->sel[s], 0);
                            }
                            igSeparator();
                            for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
                                const EcsArchetype* a = &w->archetypes[ai];
                                if (!(a->mask & COMP_BIT(COMP_OUTLINER_FOLDER))) continue;
                                const COutlinerFolder* fs = ecs_arch_column(a, COMP_OUTLINER_FOLDER);
                                const CName* fn = ecs_arch_column(a, COMP_NAME);
                                for (uint32_t i = 0; i < a->count; i++) {
                                    igPushID_Int((int)fs[i].id);
                                    if (ui_menu_item(ICON_FOLDER, fn[i].text, NULL, actor_folder(w, e) == fs[i].id, true)) {
                                        editor_undo_push(ed);
                                        for (uint32_t s = 0; s < ed->nsel; s++) set_actor_folder(w, ed->sel[s], fs[i].id);
                                    }
                                    igPopID();
                                }
                            }
                            igEndMenu();
                        }
                        igSeparator();
                        if (ui_menu_item(ICON_DELETE, TR("Delete"), "Delete", false, true)) delete_now = e;
                        igEndPopup();
                    }
                }
                igTableNextColumn();
                const CBlueprint* obp = ecs_get(w, e, COMP_BLUEPRINT);
                BpAsset* ocls = obp ? bp_find_hash(obp->asset_hash) : NULL;
                if (ocls) { // (Q26, Unreal's) a Blueprint's actor: a link that opens its class's editor
                    char link[RE_NAME_MAX + 32];
                    snprintf(link, sizeof link, "%s %s##ol_edit_bp", TR("Edit"), ocls->name);
                    if (igTextLink(link)) { // tips: none (a link named by its class; its tooltip below)
                        char vpath[700];
                        const char* root = content_root();
                        size_t rl = strlen(root);
                        snprintf(vpath, sizeof vpath, "/Content%s", !strncmp(ocls->path, root, rl) ? ocls->path + rl : ocls->path);
                        for (char* c = vpath; *c; c++)
                            if (*c == '\\') *c = '/';
                        bp_editor_open(vpath);
                    }
                    ui_item_tip(TR("Opens this actor's Blueprint Class in its editor."));
                } else
                    igTextDisabled("%s", TR(type_label(w, e)));
                igPopID();
            }
        }
        igEndTable();
    }
    igPopStyleColor(1);
    // the list's empty part takes drops too: back to the root
    if (igBeginDragDropTarget()) {
        if (igAcceptDragDropPayload("RE_OL_ACTORS", 0)) drop_folder = 0;
        const ImGuiPayload* pl = igAcceptDragDropPayload("RE_OL_FOLDER", 0);
        if (pl) drop_folder = 0, drop_payload = *(const Entity*)pl->Data;
        igEndDragDropTarget();
    }
    if (drop_folder != UINT32_MAX) {
        editor_undo_push(ed);
        COutlinerFolder* moved = drop_payload ? ecs_get(w, drop_payload, COMP_OUTLINER_FOLDER) : NULL;
        if (moved) { // a folder into another (not into itself or its own subfolders)
            if (!drop_folder || !folder_under(w, drop_folder, moved->id)) moved->parent = drop_folder, ecs_touch(w, drop_payload);
        } else
            for (uint32_t s = 0; s < ed->nsel; s++) set_actor_folder(w, ed->sel[s], drop_folder);
        ed->ol_valid = false;
    }
    // a drag over the rows selects every row it crossed (the list's box selection; Shift or Ctrl add to the selection)
    if (ed->ol_marquee) {
        ImGuiIO* io = igGetIO_Nil();
        if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
            ed->ol_marquee = false;
        } else if (ed->ol_hover_row != ed->ol_press_row && ed->ol_press_row < nrows && ed->ol_hover_row < nrows) {
            int lo = ed->ol_press_row < ed->ol_hover_row ? ed->ol_press_row : ed->ol_hover_row;
            int hi = ed->ol_press_row < ed->ol_hover_row ? ed->ol_hover_row : ed->ol_press_row;
            if (!io->KeyShift && !io->KeyCtrl) editor_select_none(ed);
            for (int k = lo; k <= hi; k++) editor_select(ed, rows[k], SEL_ADD);
            editor_select(ed, rows[ed->ol_hover_row], SEL_ADD);
        }
    }
    if (rename_now) begin_rename(ed, rename_now);
    // the context menu acts on the selection when its row is in it, else on its row alone
    if (duplicate_now) {
        if (!editor_is_selected(ed, duplicate_now)) editor_select(ed, duplicate_now, SEL_REPLACE);
        duplicate_selected(ed, now, 120.0f);
    }
    if (delete_now) {
        if (!editor_is_selected(ed, delete_now)) editor_select(ed, delete_now, SEL_REPLACE);
        delete_selected(ed);
    }
    igTextDisabled(TR("%u actors"), w->alive);
    if (ed->nsel) {
        igSameLine(0, 4);
        igTextDisabled(TR("(%u selected)"), ed->nsel);
    }
}

// --- Details ---

static bool g_changed; // a field changed this frame (Details): the undo history gets the values from before

static bool vec3_row(const char* label, float v[3], float speed, const char* fmt, float reset) {
    igTableNextRow(0, 0);
    igTableNextColumn();
    igAlignTextToFramePadding();
    igTextUnformatted(TR(label), NULL);
    igTableNextColumn();
    igPushID_Str(label);
    static const ImVec4 axis[3] = { { 0.75f, 0.2f, 0.2f, 1 }, { 0.3f, 0.65f, 0.25f, 1 }, { 0.25f, 0.4f, 0.85f, 1 } };
    bool differs = v[0] != reset || v[1] != reset || v[2] != reset;
    float rb = differs ? igGetFrameHeight() + 4 : 0;
    float w = (igGetContentRegionAvail().x - 8.0f - rb) / 3.0f;
    bool changed = false;
    for (int k = 0; k < 3; k++) {
        if (k) igSameLine(0, 4);
        igPushID_Int(k);
        ImVec2 p = igGetCursorScreenPos();
        igSetNextItemWidth(w);
        changed |= igDragFloat("##v", &v[k], speed, 0.0f, 0.0f, fmt, 0);
        ImDrawList_AddRectFilled(igGetWindowDrawList(), p, (ImVec2){ p.x + 3, p.y + igGetFrameHeight() },
                                 igColorConvertFloat4ToU32(axis[k]), 2.0f, 0);
        igPopID();
    }
    if (differs) { // Unreal's yellow "reset to default" arrow
        igSameLine(0, 4);
        if (igButton("\xe2\x86\xba##reset", (ImVec2){ igGetFrameHeight(), 0 })) {
            v[0] = v[1] = v[2] = reset;
            changed = true;
        }
        igSetItemTooltip("%s", TR("Reset to default"));
    }
    igPopID();
    return changed;
}

static void label_cell(const char* label) {
    igTableNextRow(0, 0);
    igTableNextColumn();
    igAlignTextToFramePadding();
    igTextUnformatted(TR(label), NULL);
    ui_tip_last_item(label);
    igTableNextColumn();
    igSetNextItemWidth(-1);
}

static bool row_float(const char* label, float* v, float speed, float mn, float mx, const char* fmt) {
    label_cell(label);
    igPushID_Str(label);
    bool c = igDragFloat("##f", v, speed, mn, mx, fmt, 0); // (a typed value passes the drag's range, Blender's)
    igPopID();
    return c;
}

static bool row_slider(const char* label, float* v, float mn, float mx) {
    label_cell(label);
    igPushID_Str(label);
    bool c = igSliderFloat("##s", v, mn, mx, "%.3f", 0);
    igPopID();
    return c;
}

static bool row_color(const char* label, float c[3], bool hdr) {
    label_cell(label);
    igPushID_Str(label);
    bool r = igColorEdit3("##c", c, ImGuiColorEditFlags_Float | (hdr ? ImGuiColorEditFlags_HDR : 0));
    igPopID();
    return r;
}

static bool row_check(const char* label, uint32_t* v);
// (Q8) the Cone and 6DOF constraint's rows: each axis' motion, the limits, soft limits, the drives (Unreal's Physics Constraint)
static bool constraint_6dof_rows(CConstraint* pc) {
    bool ch = false;
    static const char* const motions[CMOTION_COUNT] = { "Free", "Limited", "Locked" };
    static const char* const axes[6] = { "Linear X", "Linear Y", "Linear Z", "Twist (X)", "Swing 1 (Z)", "Swing 2 (Y)" };
    if (pc->type == CONSTRAINT_6DOF)
        for (int k = 0; k < 6; k++) {
            uint32_t* m = k < 3 ? &pc->lin_motion[k] : &pc->ang_motion[k - 3];
            label_cell(axes[k]);
            const char* tn[CMOTION_COUNT];
            for (int i = 0; i < CMOTION_COUNT; i++) tn[i] = TR(motions[i]);
            int v = *m < CMOTION_COUNT ? (int)*m : CMOTION_LOCKED;
            igPushID_Int(k);
            if (igCombo_Str_arr("##motion", &v, tn, CMOTION_COUNT, -1)) *m = (uint32_t)v, ch = true;
            igPopID();
        }
    if (pc->type == CONSTRAINT_6DOF) ch |= row_float("Linear Limit", &pc->lin_limit, 1.0f, 0.0f, 1e5f, "%.0f cm");
    ch |= row_float("Twist Limit", &pc->twist_limit, 0.5f, 0.0f, 180.0f, "%.0f\xc2\xb0");
    ch |= row_float("Swing 1 Limit", &pc->swing1_limit, 0.5f, 0.0f, 180.0f, "%.0f\xc2\xb0");
    ch |= row_float("Swing 2 Limit", &pc->swing2_limit, 0.5f, 0.0f, 180.0f, "%.0f\xc2\xb0");
    uint32_t sl = pc->soft_limits & 1u, sa = (pc->soft_limits >> 1) & 1u;
    if (row_check("Soft Linear Limit", &sl)) pc->soft_limits = (pc->soft_limits & ~1u) | sl, ch = true;
    if (row_check("Soft Angular Limit", &sa)) pc->soft_limits = (pc->soft_limits & ~2u) | (sa << 1), ch = true;
    if (pc->soft_limits) {
        ch |= row_float("Limit Frequency", &pc->limit_hz, 0.1f, 0.0f, 120.0f, "%.1f Hz");
        ch |= row_float("Limit Damping", &pc->limit_damping, 0.05f, 0.0f, 10.0f, "%.2f");
    }
    uint32_t lp = pc->lin_drive & CDRIVE_POSITION ? 1u : 0u, lv = pc->lin_drive & CDRIVE_VELOCITY ? 1u : 0u;
    uint32_t ap = pc->ang_drive & CDRIVE_POSITION ? 1u : 0u, av = pc->ang_drive & CDRIVE_VELOCITY ? 1u : 0u;
    if (pc->type == CONSTRAINT_6DOF) {
        if (row_check("Linear Position Drive", &lp)) pc->lin_drive = (pc->lin_drive & ~(uint32_t)CDRIVE_POSITION) | (lp ? CDRIVE_POSITION : 0u), ch = true;
        if (lp) ch |= vec3_row("Position Target", pc->lin_target, 1.0f, "%.0f", 0.0f);
        if (row_check("Linear Velocity Drive", &lv)) pc->lin_drive = (pc->lin_drive & ~(uint32_t)CDRIVE_VELOCITY) | (lv ? CDRIVE_VELOCITY : 0u), ch = true;
        if (lv) ch |= vec3_row("Velocity Target", pc->lin_target_vel, 1.0f, "%.0f", 0.0f);
        if (lp || lv) ch |= row_float("Max Force (0: any)", &pc->lin_drive_max, 100.0f, 0.0f, 1e12f, "%.0f");
    }
    if (row_check("Angular Orientation Drive", &ap)) pc->ang_drive = (pc->ang_drive & ~(uint32_t)CDRIVE_POSITION) | (ap ? CDRIVE_POSITION : 0u), ch = true;
    if (ap) ch |= vec3_row("Orientation Target", pc->ang_target, 1.0f, "%.0f", 0.0f);
    if (row_check("Angular Velocity Drive", &av)) pc->ang_drive = (pc->ang_drive & ~(uint32_t)CDRIVE_VELOCITY) | (av ? CDRIVE_VELOCITY : 0u), ch = true;
    if (av) ch |= vec3_row("Angular Velocity Target", pc->ang_target_vel, 1.0f, "%.0f", 0.0f);
    if (ap || av) ch |= row_float("Max Torque (0: any)", &pc->ang_drive_max, 100.0f, 0.0f, 1e12f, "%.0f");
    if (lp || lv || ap || av) {
        ch |= row_float("Drive Frequency", &pc->drive_hz, 0.1f, 0.0f, 120.0f, "%.1f Hz");
        ch |= row_float("Drive Damping", &pc->drive_damping, 0.05f, 0.0f, 10.0f, "%.2f");
    }
    return ch;
}

static bool row_check(const char* label, uint32_t* v) {
    label_cell(label);
    igPushID_Str(label);
    bool b = *v != 0, c = igCheckbox("##b", &b);
    if (c) *v = b;
    igPopID();
    return c;
}

// A Post Process Volume's setting: Unreal's override checkbox before the name (unchecked: the value is greyed and the
// volume leaves it as it is), then the value
static bool row_post(CPostProcess* p, int f) {
    const PostFieldInfo* fi = &g_post_fields[f];
    igTableNextRow(0, 0);
    igTableNextColumn();
    igPushID_Int(f);
    bool on = (p->overrides >> f & 1u) != 0, c = igCheckbox("##ov", &on);
    if (c) p->overrides = on ? p->overrides | 1u << f : p->overrides & ~(1u << f);
    igSameLine(0, 4);
    igAlignTextToFramePadding();
    igTextUnformatted(TR(fi->name), NULL);
    igTableNextColumn();
    igSetNextItemWidth(-1);
    igBeginDisabled(!on);
    float span = fi->hi - fi->lo;
    const char* fmt = f == PP_TEMPERATURE ? "%.0f K" : f == PP_DOF_FOCAL_DISTANCE ? "%.0f cm" : span > 50.0f ? "%.1f" : "%.3f";
    if (f == PP_GAIN) { // Gain with the color tint beside it (Unreal's color wheels: a multiplier per channel)
        float avail = igGetContentRegionAvail().x;
        igSetNextItemWidth(avail * 0.45f);
        c |= igDragFloat("##v", &p->s.v[f], span / 400.0f, fi->lo, fi->hi, fmt, 0);
        igSameLine(0, 4);
        igSetNextItemWidth(-1);
        c |= igColorEdit3("##tint", p->s.color_tint, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
    } else
        c |= igDragFloat("##v", &p->s.v[f], span / 400.0f, fi->lo, fi->hi, fmt, 0); // (typed: past the range)
    igEndDisabled();
    if (on && igIsItemDeactivatedAfterEdit()) c = true;
    igPopID();
    return c;
}

// A camera's Post Process Settings (a Camera Actor's and a Blueprint's Camera Component alike): each setting's override box,
// its value, the blend weight; they apply while the camera is the player's view
bool editor_camera_post_ui(CCamera* c) {
    bool ch = false;
    char lab[96];
    snprintf(lab, sizeof lab, "%s###cam_post", TR("Post Process"));
    if (!igCollapsingHeader_TreeNodeFlags(lab, 0)) return false;
    igPushID_Str("cam_post");
    float wt = c->post_weight > 0.0f ? c->post_weight : 1.0f;
    if (igSliderFloat(PL(TR("Post Process Blend Weight")), &wt, 0.0f, 1.0f, "%.2f", 0)) c->post_weight = wt, ch = true;
    const char* grp = "";
    for (int k = 0; k < CAMERA_POST_FIELDS; k++) {
        int f = g_camera_post_fields[k];
        const PostFieldInfo* fi = &g_post_fields[f];
        if (strcmp(grp, fi->group)) igSeparatorText(TR(grp = fi->group));
        igPushID_Int(k);
        bool on = (c->post_overrides >> k & 1u) != 0;
        if (igCheckbox("##ov", &on)) c->post_overrides = on ? c->post_overrides | 1u << k : c->post_overrides & ~(1u << k), ch = true;
        if (!on && !c->post[k] && fi->def) c->post[k] = fi->def; // (a setting never set: its default)
        igSameLine(0, 4);
        igBeginDisabled(!on);
        float span = fi->hi - fi->lo;
        ch |= igDragFloat(PL(TR(fi->name)), &c->post[k], span / 400.0f, fi->lo, fi->hi, f == PP_TEMPERATURE ? "%.0f K" : f == PP_DOF_FOCAL_DISTANCE ? "%.0f cm" : "%.3f", 0);
        igEndDisabled();
        igPopID();
    }
    igPopID();
    return ch;
}

// --- (Q6) the Details' asset fields: a click opens the asset picker (asset_picker.h) on the asset's type ---

// A Static Mesh by its id (the basic shapes too, first in the picker)
static bool field_static_mesh(const char* id, uint32_t* mesh) {
    uint32_t slot = render_mesh_slot(*mesh);
    char cur[600], out[600];
    if (slot < SHAPE_COUNT) snprintf(cur, sizeof cur, "#mesh/%x", render_mesh_id(slot));
    else snprintf(cur, sizeof cur, "%s", render_mesh_vpath(slot));
    const char* shown = slot < SHAPE_COUNT ? TR(g_shape_names[slot]) : render_mesh_name(slot);
    int r = asset_field(id, shown, cur, ASSET_STATIC_MESH, AK(ASSET_STATIC_MESH), false, out, sizeof out);
    if (r == ASSET_FIELD_OPENED)
        for (uint32_t k = 0; k < SHAPE_COUNT; k++) {
            char ref[32];
            snprintf(ref, sizeof ref, "#mesh/%x", render_mesh_id(k));
            asset_picker_add(TR(g_shape_names[k]), ref, ASSET_STATIC_MESH, -1, 0, 0);
        }
    if (r != ASSET_FIELD_PICKED || !out[0]) return false;
    bool shape = !strncmp(out, "#mesh/", 6);
    uint32_t m = shape ? (uint32_t)strtoul(out + 6, NULL, 16) : render_mesh_id_of(out);
    if (!shape && !m) return false;
    *mesh = m;
    return true;
}

// A material or a Material Instance by its hash (0: none_label); lf_only: the Light Function domain's
static bool field_material(const char* id, uint32_t* hash, const char* none_label, bool lf_only) {
    Material* cur = *hash ? mat_find_hash(*hash) : NULL;
    char vp[700] = "", out[700];
    if (cur) mat_vpath(cur, vp, sizeof vp);
    int r = asset_field(id, cur ? cur->name : none_label, vp, cur && cur->instance ? ASSET_MATERIAL_INSTANCE : ASSET_MATERIAL,
                        lf_only ? 0 : AK(ASSET_MATERIAL) | AK(ASSET_MATERIAL_INSTANCE), true, out, sizeof out);
    if (r == ASSET_FIELD_OPENED && lf_only)
        for (uint32_t i = 0; i < mat_count(); i++) { // (the materials of the Light Function domain)
            Material* m = mat_at(i);
            const Material* p = m->instance ? m->parent : m;
            char mv[700];
            if (p && p->domain == MAT_DOMAIN_LIGHT_FUNCTION && mat_vpath(m, mv, sizeof mv))
                asset_picker_add(m->name, mv, m->instance ? ASSET_MATERIAL_INSTANCE : ASSET_MATERIAL, -1, 1, m->hash);
        }
    if (r != ASSET_FIELD_PICKED) return false;
    if (!out[0]) {
        *hash = 0;
        return true;
    }
    char path[1200];
    snprintf(path, sizeof path, "%s%s", content_root(), out + 8);
    Material* m = mat_load(path);
    if (!m) return false;
    *hash = m->hash;
    return true;
}

// A Skeletal Mesh by its hash (0: none_label, when it may be empty)
static bool field_skeletal_mesh(const char* id, uint32_t* hash, const char* none_label) {
    const SkelMesh* cur = *hash ? anim_mesh(*hash) : NULL;
    char vp[700] = "", out[700];
    if (cur) anim_mesh_vpath(cur, vp, sizeof vp);
    if (asset_field(id, cur ? cur->name : none_label, vp, ASSET_SKELETAL_MESH, AK(ASSET_SKELETAL_MESH), none_label != NULL, out,
                    sizeof out) != ASSET_FIELD_PICKED)
        return false;
    if (!out[0]) {
        *hash = 0;
        return true;
    }
    SkelMesh* m = anim_mesh_load(out);
    if (!m) return false;
    *hash = m->hash;
    return true;
}

// An asset with its disk path in a registry: its "/Content/..." path
static void vpath_of_disk(const char* disk, char* out, size_t cap) {
    size_t n = strlen(content_root());
    snprintf(out, cap, "/Content%s", strlen(disk) > n ? disk + n : "");
    for (char* c = out; *c; c++)
        if (*c == '\\') *c = '/';
}

// (Q13) Unreal's Lighting Channels: three boxes (0 is stored as "channel 0 only"; at least one stays on)
static bool row_channels(uint32_t* mask) {
    label_cell("Lighting Channels");
    uint32_t m = *mask ? *mask : 1u;
    bool ch = false;
    for (int k = 0; k < 3; k++) {
        char id[16];
        snprintf(id, sizeof id, "%d##lc%d", k, k);
        bool on = (m >> k) & 1u;
        if (k) igSameLine(0, 10);
        if (igCheckbox(id, &on)) {
            uint32_t n = on ? m | 1u << k : m & ~(1u << k);
            if (n) *mask = n, ch = true;
        }
    }
    return ch;
}
// (Q13) a sound picked from the project's Sound Waves and Sound Cues (its hash; 0: none)
static bool row_sound_hash(const char* label, uint32_t* hash) {
    label_cell(label);
    bool ch = false;
    const char* cur = *hash ? au_sound_name(*hash) : "";
    igPushID_Str(label);
    if (igBeginCombo("##snd", cur && cur[0] ? cur : TR("None"), 0)) {
        if (igSelectable_Bool(TR("None"), *hash == 0, 0, (ImVec2){ 0, 0 })) *hash = 0, ch = true;
        for (uint32_t i = 0, n = audio_wave_count(); i < n; i++)
            if (igSelectable_Bool(audio_wave_at(i)->name, cur && !strcmp(cur, audio_wave_at(i)->name), 0, (ImVec2){ 0, 0 }))
                *hash = audio_sound_by_name(audio_wave_at(i)->name), ch = true;
        for (uint32_t i = 0, n = au_asset_count(); i < n; i++) {
            AuAsset* a = au_asset_at(i);
            if (a->kind == AUA_CUE && igSelectable_Bool(a->name, cur && !strcmp(cur, a->name), 0, (ImVec2){ 0, 0 })) *hash = audio_sound_by_name(a->name), ch = true;
        }
        igEndCombo();
    }
    igPopID();
    return ch;
}
// (Q13) a Tension Director channel's curve: its value at tension 0, 1/3, 2/3, 1 and a plot of it
static bool row_tension_curve(int c, float v[4]) {
    const TensionChannelInfo* ci = &g_tension_channels[c];
    label_cell(ci->name);
    igPushID_Int(c);
    bool ch = false;
    float w = (igGetContentRegionAvail().x - igGetFontSize() * 4.0f - 12.0f) / 4.0f;
    float sp = (ci->hi - ci->lo) / 400.0f;
    for (int k = 0; k < 4; k++) {
        if (k) igSameLine(0, 3);
        igSetNextItemWidth(w);
        char id[8];
        snprintf(id, sizeof id, "##k%d", k);
        ch |= igDragFloat(id, &v[k], sp, ci->lo, ci->hi, ci->hi > 100.0f ? "%.0f" : "%.2f", 0);
    }
    igSameLine(0, 3);
    float pts[16];
    for (int i = 0; i < 16; i++) { // (the curve as the director reads it: linear between the keys here, the plot only)
        float x = (float)i / 15.0f * 3.0f;
        int j = x >= 3.0f ? 2 : (int)x;
        pts[i] = v[j] + (v[j + 1] - v[j]) * (x - (float)j);
    }
    igPlotLines_FloatPtr("##plot", pts, 16, 0, NULL, ci->lo, ci->hi, (ImVec2){ igGetFontSize() * 4.0f, igGetFrameHeight() }, sizeof(float));
    igPopID();
    return ch;
}

static bool begin_section(const char* title, const char* id) {
    char lab[96];
    snprintf(lab, sizeof lab, "%s###%s", TR(title), id);
    if (!igCollapsingHeader_TreeNodeFlags(lab, ImGuiTreeNodeFlags_DefaultOpen)) return false;
    if (!igBeginTable(id, 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, (ImVec2){ 0, 0 }, 0)) return false;
    igTableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 7.5f, 0);
    igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
    return true;
}

// A Crowd Spawner's settings (crowd.h): the mesh and its clips, what they do, how they think, the difficulty
// (A9.4) Unreal's Cine Camera: the physical lens (filmback, lens presets, focal length, aperture, focus, the iris's blades)
// and exposure (ISO, shutter speed, the exposure presets). The field of view follows the focal length and the sensor
static void cine_defaults(CCamera* c) {
    if (c->focal_mm <= 0.0f) c->focal_mm = 35.0f;
    if (c->sensor_mm <= 0.0f) c->sensor_mm = 36.0f;
    if (c->aperture <= 0.0f) c->aperture = 2.8f;
    if (c->iso <= 0.0f) c->iso = 100.0f;
    if (c->shutter_s <= 0.0f) c->shutter_s = 1.0f / 60.0f;
}
static void cine_fov(CCamera* c) { c->fov_deg = 2.0f * atanf(c->sensor_mm / (2.0f * c->focal_mm)) * (180.0f / 3.14159265f); }
static bool cine_flag(const char* label, CCamera* c, uint32_t bit) {
    uint32_t on = (c->cine & bit) != 0;
    if (!row_check(label, &on)) return false;
    c->cine = on ? c->cine | bit : c->cine & ~bit;
    cine_defaults(c);
    if (bit == CINE_LENS && on) cine_fov(c);
    return true;
}
static bool cine_rows(CCamera* c) {
    bool ch = false;
    ch |= cine_flag("Physical Lens (Cine Camera)", c, CINE_LENS);
    if (c->cine & CINE_LENS) {
        static const struct { const char* name; float w; } backs[] = { { "Full Frame (36 mm)", 36.0f }, { "Super 35 (24.89 mm)", 24.89f },
                                                                       { "APS-C (23.6 mm)", 23.6f }, { "Micro Four Thirds (17.3 mm)", 17.3f },
                                                                       { "Super 16 (12.52 mm)", 12.52f }, { "IMAX 70 mm (70.41 mm)", 70.41f } };
        static const struct { const char* name; float f, n; } lenses[] = { { "12 mm f/2.8", 12, 2.8f }, { "24 mm f/1.4", 24, 1.4f }, { "35 mm f/1.4", 35, 1.4f },
                                                                           { "50 mm f/1.2", 50, 1.2f }, { "85 mm f/1.8", 85, 1.8f }, { "135 mm f/2", 135, 2.0f },
                                                                           { "200 mm f/2.8", 200, 2.8f } };
        label_cell("Filmback Preset");
        if (igBeginCombo("##filmback", TR("Choose..."), 0)) {
            for (size_t i = 0; i < sizeof backs / sizeof backs[0]; i++)
                if (igSelectable_Bool(backs[i].name, fabsf(c->sensor_mm - backs[i].w) < 0.01f, 0, (ImVec2){ 0, 0 })) c->sensor_mm = backs[i].w, cine_fov(c), ch = true;
            igEndCombo();
        }
        label_cell("Lens Preset");
        if (igBeginCombo("##lens", TR("Choose..."), 0)) {
            for (size_t i = 0; i < sizeof lenses / sizeof lenses[0]; i++)
                if (igSelectable_Bool(lenses[i].name, false, 0, (ImVec2){ 0, 0 })) c->focal_mm = lenses[i].f, c->aperture = lenses[i].n, cine_fov(c), ch = true;
            igEndCombo();
        }
        if (row_float("Focal Length", &c->focal_mm, 0.5f, 4.0f, 1200.0f, "%.0f mm")) c->focal_mm = fmaxf(c->focal_mm, 4.0f), cine_fov(c), ch = true;
        if (row_float("Sensor Width", &c->sensor_mm, 0.1f, 4.0f, 80.0f, "%.2f mm")) c->sensor_mm = fmaxf(c->sensor_mm, 4.0f), cine_fov(c), ch = true;
        ch |= row_float("Aperture (F-stop)", &c->aperture, 0.05f, 0.7f, 32.0f, "f/%.1f");
        ch |= row_float("Focus Distance", &c->focus_cm, 5.0f, 0.0f, 1e6f, c->focus_cm > 0.0f ? "%.0f cm" : "off");
        float blades = (float)c->blades;
        if (row_float("Diaphragm Blades", &blades, 0.05f, 0.0f, 16.0f, blades < 3.0f ? "round" : "%.0f")) c->blades = (uint32_t)(blades + 0.5f), ch = true;
        if (c->blades >= 3) ch |= row_float("Blade Rotation", &c->blade_rot, 0.5f, 0.0f, 360.0f, "%.0f\xc2\xb0");
    }
    ch |= cine_flag("Manual Exposure (ISO, Shutter)", c, CINE_EXPOSURE);
    ch |= cine_flag("Motion Blur from Shutter", c, CINE_SHUTTER_BLUR);
    if (c->cine & (CINE_EXPOSURE | CINE_SHUTTER_BLUR)) {
        static const struct { const char* name; float iso, n, den; } expo[] = { { "Sunny 16 (ISO 100, f/16, 1/125)", 100, 16, 125 },
                                                                                { "Overcast (ISO 200, f/5.6, 1/125)", 200, 5.6f, 125 },
                                                                                { "Indoor (ISO 800, f/2.8, 1/60)", 800, 2.8f, 60 },
                                                                                { "Night Street (ISO 3200, f/1.8, 1/50)", 3200, 1.8f, 50 },
                                                                                { "Cinema 180 Degrees (1/48 at 24 fps)", 0, 0, 48 } };
        label_cell("Exposure Preset");
        if (igBeginCombo("##expo", TR("Choose..."), 0)) {
            for (size_t i = 0; i < sizeof expo / sizeof expo[0]; i++)
                if (igSelectable_Bool(expo[i].name, false, 0, (ImVec2){ 0, 0 })) {
                    if (expo[i].iso > 0.0f) c->iso = expo[i].iso, c->aperture = expo[i].n;
                    c->shutter_s = 1.0f / expo[i].den, ch = true;
                }
            igEndCombo();
        }
        ch |= row_float("ISO", &c->iso, 5.0f, 25.0f, 102400.0f, "%.0f");
        float den = c->shutter_s > 0.0f ? 1.0f / c->shutter_s : 60.0f;
        if (row_float("Shutter Speed", &den, 0.5f, 1.0f, 8000.0f, "1/%.0f s")) c->shutter_s = 1.0f / fmaxf(den, 1.0f), ch = true;
        if (!(c->cine & CINE_LENS)) ch |= row_float("Aperture (F-stop)", &c->aperture, 0.05f, 0.7f, 32.0f, "f/%.1f");
        if (c->cine & CINE_EXPOSURE) {
            float N = c->aperture > 0.0f ? c->aperture : 2.8f, t = c->shutter_s > 0.0f ? c->shutter_s : 1.0f / 60.0f;
            label_cell("EV100");
            igText("%.1f", log2f(N * N / t) - log2f((c->iso > 0.0f ? c->iso : 100.0f) / 100.0f));
        }
    }
    return ch;
}

static bool row_seq(const char* label, uint32_t* hash) {
    label_cell(label);
    igPushID_Str(label);
    const AnimSeq* cur = *hash ? anim_seq(*hash) : NULL;
    bool c = false;
    if (igBeginCombo("##seq", cur ? cur->name : TR("(the Mannequin's)"), 0)) {
        if (igSelectable_Bool(TR("(the Mannequin's)"), !cur, 0, (ImVec2){ 0, 0 })) *hash = 0, c = true;
        for (uint32_t i = 0; i < anim_seq_count(); i++) {
            const AnimSeq* q = anim_seq_at(i);
            if (asset_selectable(q->name, q == cur, q->path, ASSET_ANIMATION)) *hash = q->hash, c = true;
        }
        igEndCombo();
    }
    igPopID();
    return c;
}

// A heightmap file picked (the native dialog): its path into out; false when cancelled
static bool pick_heightmap(Editor* ed, char* out, size_t cap) {
    return platform_file_dialog_ext(ed->window, false, TR("Heightmap (16-bit PNG, TIFF, R16, RAW)"), "png;tif;tiff;r16;raw;bmp;jpg", out, cap);
}

// The grid of a heightmap's size (at most 1024): Unreal's resolution taken from the file
static void res_of_heightmap(CLandscape* c, const char* path) {
    uint32_t iw, ih;
    if (!landscape_heightmap_size(path, &iw, &ih)) return;
    uint32_t side = iw > ih ? iw : ih, r = 64;
    while (r < side - 1 && r < 1024) r *= 2;
    c->res = r;
}

// The heightmap rows of a landscape (Unreal's Import from File): the heights from it, or a displacement over them
static bool heightmap_rows(Editor* ed, Entity e, CLandscape* c) {
    bool ch = false;
    label_cell("Heightmap");
    float bw = (igGetContentRegionAvail().x - igGetStyle()->ItemSpacing.x) * 0.5f;
    char path[600];
    snprintf(path, sizeof path, "%s", ed->land_hm);
    if (igButton(TR("Import..."), (ImVec2){ bw, 0 }) && pick_heightmap(ed, path, sizeof path)) {
        snprintf(ed->land_hm, sizeof ed->land_hm, "%s", path);
        if (ed->land_hm_res) res_of_heightmap(c, path);
        landscape_import_heightmap(e, path, false), ch = true;
    }
    ui_tip_last_item("Import Heightmap");
    igSameLine(0, -1);
    if (igButton(TR("Add Displacement..."), (ImVec2){ bw, 0 }) && pick_heightmap(ed, path, sizeof path)) {
        snprintf(ed->land_hm, sizeof ed->land_hm, "%s", path);
        landscape_import_heightmap(e, path, true), ch = true;
    }
    ui_tip_last_item("Add Displacement");
    label_cell("");
    igCheckbox(TR("Resolution from the image"), &ed->land_hm_res);
    const LandStats* ls = landscape_stats();
    if (ls->import_msg[0]) {
        label_cell("");
        if (ls->import_msg[0] == '!') igTextColored(THV(TK_ERROR), TR("Could not read %s."), ls->import_msg + 1);
        else igTextDisabled(TR("Imported: %s"), ls->import_msg);
    }
    return ch;
}

// A Landscape's settings (landscape.h): how it is made (noise or a heightmap), the sculpting brush
static bool landscape_details(Editor* ed, Entity e, CLandscape* c) {
    bool ch = false, again = false;
    if (ed->land_radius <= 0.0f) ed->land_radius = 800.0f, ed->land_strength = 600.0f; // (the brush's start)
    label_cell("Size");
    float m = c->size / 100.0f;
    if (igDragFloat("##lsize", &m, 1.0f, 10.0f, 10000.0f, "%.0f m", 0)) c->size = m * 100.0f, ch = true;
    label_cell("Resolution");
    static const uint32_t ress[5] = { 64, 128, 256, 512, 1024 };
    static const char* const resn[5] = { "64 x 64", "128 x 128", "256 x 256", "512 x 512", "1024 x 1024" };
    int ri = 2;
    for (int k = 0; k < 5; k++)
        if (c->res == ress[k]) ri = k;
    if (igCombo_Str_arr("##lres", &ri, resn, 5, -1)) c->res = ress[ri], ch = true;
    ch |= row_float("Height", &c->height, 5.0f, 0.0f, 1e6f, "%.0f cm");
    ch |= row_slider("Roughness", &c->roughness, 0.0f, 1.0f);
    label_cell("Seed");
    int sd = (int)c->seed;
    if (igDragInt("##lseed", &sd, 1.0f, 0, 1 << 30, "%d", 0)) c->seed = (uint32_t)sd, ch = true, again = true;
    ch |= row_color("Color", c->color, false);
    label_cell("");
    if (igButton(TR("Generate Again"), (ImVec2){ -1, 0 })) again = true, ch = true;
    if (again) landscape_regenerate(&ed->world, e);
    ch |= heightmap_rows(ed, e, c);
    // sculpting
    label_cell("Sculpt");
    static const char* const tools[LAND_TOOL_COUNT] = { "Off", "Raise", "Lower", "Smooth", "Flatten", "Paint", "Erase" };
    const char* tl[LAND_TOOL_COUNT];
    for (int k = 0; k < LAND_TOOL_COUNT; k++) tl[k] = TR(tools[k]);
    if (igCombo_Str_arr("##ltool", &ed->land_tool, tl, LAND_TOOL_COUNT, -1) && ed->land_tool) ed->foliage_mode = false;
    if (ed->land_tool) {
        row_float("Brush Radius", &ed->land_radius, 5.0f, 50.0f, 1e5f, "%.0f cm");
        row_float("Brush Strength", &ed->land_strength, 5.0f, 1.0f, 1e5f, "%.0f cm/s");
    }
    label_cell("");
    if (igButton(TR("Export Heightmap (.r16)"), (ImVec2){ -1, 0 })) { // (B11)
        char p[900];
        float lo = 0.0f, hi = 0.0f;
        snprintf(p, sizeof p, "%s/Landscapes/%s.r16", content_root(), c->name);
        bool ok = landscape_export_heightmap(e, p, &lo, &hi);
        editor_status(ed, re_time_seconds(), ok ? TR("Heightmap written: %s (black %.0f cm, white %.0f cm).") : TR("Heightmap not written: %s"), p, lo, hi);
        content_mark_dirty();
    }
    return ch;
}

// A Foliage actor's types and brush (foliage.h, Unreal's Foliage mode): each type's mesh, material, density, sizes,
// slope and wind; Paint / Erase, Fill (the landscapes at the density) and Remove All
static bool foliage_details(Editor* ed, Entity e, CFoliage* c) {
    bool ch = false;
    if (ed->fol_radius <= 0.0f) ed->fol_radius = 600.0f, ed->fol_density = 1.0f; // (the brush's start)
    if (c->ntypes > FOLIAGE_TYPES) c->ntypes = FOLIAGE_TYPES;
    if (igBeginTable("fol_imp_t", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, (ImVec2){ 0, 0 }, 0)) { // (Q21)
        igTableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 7.5f, 0);
        igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        ch |= row_check("Impostors", &c->impostors); // (far away the plants drawn by their octahedral impostor: farview.h)
        igEndTable();
    }
    int remove = -1;
    for (uint32_t i = 0; i < c->ntypes; i++) {
        FoliageType* t = &c->types[i];
        igPushID_Int((int)i);
        char head[128];
        snprintf(head, sizeof head, "%s %u: %s (%u)###ft", TR("Foliage Type"), i + 1, render_mesh_name(render_mesh_slot(t->mesh)), foliage_count(e, (int)i));
        bool paint = t->paint != 0;
        if (igCheckbox("##paint", &paint)) t->paint = paint, ch = true;
        igSetItemTooltip("%s", TR("Painted (and erased) by the brush"));
        igSameLine(0, 4);
        if (igTreeNodeEx_Str(head, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
            if (igBeginTable("ft_t", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, (ImVec2){ 0, 0 }, 0)) {
                igTableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 7.5f, 0);
                igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
                label_cell("Mesh");
                if (igBeginCombo("##fmesh", render_mesh_name(render_mesh_slot(t->mesh)), 0)) {
                    for (uint32_t s = 0; s < render_mesh_count(); s++)
                        if (asset_selectable_mesh(render_mesh_name(s), render_mesh_id(s) == t->mesh, render_mesh_id(s))) t->mesh = render_mesh_id(s), ch = true;
                    igEndCombo();
                }
                label_cell("Material");
                Material* cur = t->material ? mat_find_hash(t->material) : NULL;
                if (igBeginCombo("##fmat", cur ? cur->name : TR("None (the colors below)"), 0)) {
                    if (igSelectable_Bool(TR("None (the colors below)"), !cur, 0, (ImVec2){ 0, 0 })) t->material = 0, ch = true;
                    for (uint32_t k = 0; k < mat_count(); k++) {
                        Material* mm = mat_at(k);
                        if (asset_selectable_material(mm->name, mm == cur, mm->path)) t->material = mm->hash, ch = true;
                    }
                    igEndCombo();
                }
                ch |= row_color("Color", t->color, false);
                ch |= row_float("Density", &t->density, 0.5f, 0.0f, 5000.0f, "%.1f / 100 m\xc2\xb2");
                label_cell("Scale Min/Max");
                ch |= igDragFloat2("##fscale", t->scale, 0.01f, 0.01f, 50.0f, "%.2f", 0);
                ch |= row_float("Stretch", &t->stretch, 0.02f, 0.1f, 20.0f, "%.2f");
                ch |= row_float("Max Slope", &t->slope, 0.5f, 1.0f, 89.0f, "%.0f\xc2\xb0");
                ch |= row_slider("Wind", &t->wind, 0.0f, 1.0f);
                label_cell("");
                float bw = (igGetContentRegionAvail().x - igGetStyle()->ItemSpacing.x * 2.0f) / 3.0f;
                if (igButton(TR("Fill"), (ImVec2){ bw, 0 })) foliage_fill(&ed->world, e, (int)i), ch = true;
                ui_tip_last_item("Fill");
                igSameLine(0, -1);
                if (igButton(TR("Remove All"), (ImVec2){ bw, 0 })) foliage_clear(&ed->world, e, (int)i), ch = true;
                igSameLine(0, -1);
                igBeginDisabled(c->ntypes <= 1);
                if (igButton(TR("Delete"), (ImVec2){ bw, 0 })) remove = (int)i;
                igEndDisabled();
                igEndTable();
            }
            igTreePop();
        }
        igPopID();
    }
    if (remove >= 0) { // (its instances go with it; the types after it move down one: their instances follow)
        foliage_clear(&ed->world, e, remove);
        foliage_retype(&ed->world, e, remove);
        memmove(&c->types[remove], &c->types[remove + 1], sizeof(FoliageType) * (c->ntypes - (uint32_t)remove - 1));
        c->ntypes--, ch = true;
    }
    if (c->ntypes < FOLIAGE_TYPES && igButton(TR("+ Add Foliage Type"), (ImVec2){ -1, 0 })) {
        FoliageType* t = &c->types[c->ntypes++];
        *t = c->types[0], t->mesh = SHAPE_SPHERE, t->stretch = 1.0f, t->density = 4.0f, t->material = 0; // (a bush-like start)
        t->color[0] = 0.1f, t->color[1] = 0.3f, t->color[2] = 0.08f;
        ch = true;
    }
    igSeparatorText(TR("Brush"));
    if (igBeginTable("fol_brush", 2, ImGuiTableFlags_BordersInnerV, (ImVec2){ 0, 0 }, 0)) {
        igTableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 7.5f, 0);
        igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        label_cell("Tool");
        int tool = ed->fol_erase ? 1 : 0;
        const char* tn[2] = { TR("Paint"), TR("Erase") };
        if (igCombo_Str_arr("##ftool", &tool, tn, 2, -1)) ed->fol_erase = tool == 1;
        row_float("Brush Radius", &ed->fol_radius, 5.0f, 20.0f, 1e5f, "%.0f cm");
        row_float("Paint Density", &ed->fol_density, 0.01f, 0.01f, 10.0f, "%.2fx");
        label_cell("");
        if (igButton(TR("Fill All Types"), (ImVec2){ -1, 0 })) foliage_fill(&ed->world, e, -1), ch = true;
        igEndTable();
    }
    const FoliageStats* fs = foliage_stats();
    igTextDisabled(TR("%u instances; last edit %.2f ms, last build %.1f ms"), foliage_count(e, -1), fs->edit_ms, fs->build_ms);
    if (ed->foliage_mode) igTextDisabled("%s", TR("Left button: paint the checked types (Shift: erase). Fill covers the landscapes at each type's density."));
    else if (igButton(TR("Foliage Mode (Shift+3)"), (ImVec2){ -1, 0 })) ed->foliage_mode = true, ed->land_tool = 0;
    return ch;
}

// Unreal's mode panels (in place of Place Actors): the Landscape mode's Manage (a new landscape from noise or a
// heightmap file) and Sculpt, the Foliage mode's types and brush
static void mode_panel(Editor* ed, double now) {
    World* w = &ed->world;
    if (ed->foliage_mode) {
        panel_header(TR("Foliage"));
        CFoliage* c = ecs_get(w, ed->selected, COMP_FOLIAGE);
        if (!c) {
            igTextWrapped("%s", TR("Select the level's Foliage actor (or make one) to paint."));
            if (igButton(TR("Foliage Mode (Shift+3)"), (ImVec2){ -1, 0 })) editor_foliage_mode(ed, now);
            return;
        }
        if (foliage_details(ed, ed->selected, c)) ed->dirty = true;
        return;
    }
    panel_header(TR("Landscape"));
    igSeparatorText(TR("Manage"));
    // New Landscape: Create New (noise) or Import from File (Unreal's)
    const char* kinds[2] = { TR("Create New"), TR("Import from File") };
    igSetNextItemWidth(-1);
    igCombo_Str_arr("##lnew", &ed->land_new, kinds, 2, -1);
    if (ed->land_new == 1) {
        char path[600];
        snprintf(path, sizeof path, "%s", ed->land_hm);
        const char* shown = strrchr(ed->land_hm, '\\') ? strrchr(ed->land_hm, '\\') + 1 : ed->land_hm;
        if (igButton(ed->land_hm[0] ? shown : TR("Heightmap File..."), (ImVec2){ -1, 0 }) && pick_heightmap(ed, path, sizeof path))
            snprintf(ed->land_hm, sizeof ed->land_hm, "%s", path);
        igCheckbox(TR("Resolution from the image"), &ed->land_hm_res);
    }
    igBeginDisabled(ed->land_new == 1 && !ed->land_hm[0]);
    if (igButton(ed->land_new == 1 ? TR("Import") : TR("Create"), (ImVec2){ -1, 0 })) {
        add_actor(ed, SPAWN_LANDSCAPE, 0, now);
        CLandscape* c = ecs_get(w, ed->selected, COMP_LANDSCAPE);
        if (c && ed->land_new == 1) {
            if (ed->land_hm_res) res_of_heightmap(c, ed->land_hm);
            landscape_import_heightmap(ed->selected, ed->land_hm, false);
        }
        if (!ed->land_tool) ed->land_tool = LAND_TOOL_RAISE;
    }
    igEndDisabled();
    CLandscape* sel = ecs_get(w, ed->selected, COMP_LANDSCAPE);
    if (!sel) {
        igTextWrapped("%s", TR("Select a Landscape to sculpt it, or make one above."));
        return;
    }
    igSeparatorText(TR("Sculpt"));
    static const char* const tools[LAND_TOOL_COUNT] = { "Off", "Raise", "Lower", "Smooth", "Flatten", "Paint", "Erase" };
    for (int k = 1; k < LAND_TOOL_PAINT; k++) {
        if (k > 1) igSameLine(0, 4);
        if (ui_icon_button(tools[k], ICON_FLOOR, TR(tools[k]), ed->land_tool == k ? TH(TK_LIME) : TH(TK_N14), ed->land_tool == k, NULL)) ed->land_tool = k;
    }
    igSeparatorText(TR("Paint")); // (B11: the material's Landscape Layers)
    for (int k = LAND_TOOL_PAINT; k < LAND_TOOL_COUNT; k++) {
        if (k > LAND_TOOL_PAINT) igSameLine(0, 4);
        if (ui_icon_button(tools[k], ICON_FLOOR, TR(tools[k]), ed->land_tool == k ? TH(TK_LIME) : TH(TK_N14), ed->land_tool == k, NULL)) ed->land_tool = k;
    }
    {
        static int layer = 1;
        igSameLine(0, 12);
        igSetNextItemWidth(igGetFontSize() * 6.0f);
        if (igSliderInt(TR("Layer"), &layer, 1, 3, "%d", 0)) landscape_set_paint_layer(layer);
        const Material* lm = sel->material ? mat_find_hash(sel->material) : NULL;
        if (!lm || !lm->layer_path[layer - 1][0])
            igTextDisabled("%s", TR("Give the landscape's material Landscape Layers (its Details) to see what you paint."));
        else igTextDisabled("%s", lm->layer_path[layer - 1]);
    }
    if (igBeginTable("land_brush", 2, ImGuiTableFlags_BordersInnerV, (ImVec2){ 0, 0 }, 0)) {
        igTableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 7.5f, 0);
        igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
        if (ed->land_radius <= 0.0f) ed->land_radius = 800.0f, ed->land_strength = 600.0f;
        row_float("Brush Radius", &ed->land_radius, 5.0f, 50.0f, 1e5f, "%.0f cm");
        row_float("Brush Strength", &ed->land_strength, 5.0f, 1.0f, 1e5f, "%.0f cm/s");
        if (heightmap_rows(ed, ed->selected, sel)) ed->dirty = true;
        igEndTable();
    }
    igTextDisabled("%s", TR("Left button: sculpt (Shift: the opposite). The chunks are written again as you go."));
    igTextDisabled("%s", TR("The plants: the Foliage mode (Shift+3)."));
}

// A prototyping shape's numbers (proto.h): built again as they change
// A wall's doors as an Unreal array: "N Array elements" with + and the bin, unfolded: one row per door (x removes it)
// (Q26) The Modes panel: Unreal's three editor modes as rows (the active one in the accent), then the active mode's
// tools (the Landscape's sculpting and layers, the Foliage's painting); in Selection, a hint where the classes are
static void modes_panel(Editor* ed, double now) {
    static const UiIcon ic[3] = { ICON_SELECT, ICON_LANDSCAPE, ICON_FOLIAGE };
    static const char* const nm[3] = { "Selection Mode", "Landscape Mode", "Foliage Mode" };
    static const char* const key[3] = { "Shift+1", "Shift+2", "Shift+3" };
    static const char* const tip[3] = { "Select, move, rotate and scale actors (W / E / R).",
                                        "Sculpt and paint the level's Landscape (made when there is none).",
                                        "Paint plants on the ground with the Foliage actor's brush." };
    int cur = ed->land_tool ? 1 : ed->foliage_mode ? 2 : 0;
    float fs = igGetFontSize(), rh = fs * 2.0f, w = igGetContentRegionAvail().x;
    ImDrawList* dl = igGetWindowDrawList();
    for (int m = 0; m < 3; m++) {
        igPushID_Int(m);
        ImVec2 p = igGetCursorScreenPos();
        bool hit = igInvisibleButton("##mode", (ImVec2){ w, rh }, 0); // tips: none (a row drawn with its name; its tooltip below)
        bool hov = igIsItemHovered(0), sel = cur == m;
        if (hov) ui_tip_item(ic[m], TR(nm[m]), TR(tip[m]), key[m]);
        if (sel || hov) ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + rh }, sel ? TH(TK_ACCENT_DIM) : TH(TK_BG_HOVER), TMV(TM_RADIUS_FRAME), 0);
        if (sel) ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + 3.0f, p.y + rh }, TH(TK_ACCENT), 1.5f, 0);
        float is = fs * 1.15f;
        ui_icon(dl, ic[m], p.x + fs * 0.6f, p.y + (rh - is) * 0.5f, is, sel ? TH(TK_TEXT_STRONG) : TH(TK_TEXT));
        ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + fs * 0.6f + is + fs * 0.6f, p.y + (rh - fs) * 0.5f }, sel ? TH(TK_TEXT_STRONG) : TH(TK_TEXT), TR(nm[m]), NULL);
        ImVec2 ks = igCalcTextSize(key[m], NULL, false, -1);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + w - ks.x - fs * 0.6f, p.y + (rh - fs) * 0.5f }, TH(TK_TEXT_DIM), key[m], NULL);
        if (hit && !ed->playing) {
            if (m == 0) ed->land_tool = 0, ed->foliage_mode = false;
            else if (m == 1 && !ed->land_tool) editor_landscape_mode(ed, now);
            else if (m == 2 && !ed->foliage_mode) editor_foliage_mode(ed, now);
        }
        igPopID();
    }
    igSpacing();
    igSeparator();
    igSpacing();
    if (cur) mode_panel(ed, now);
    else igTextWrapped("%s", TR("The classes to place are in Place Actors (the tab beside this one); the selected actor's properties in Details."));
}

static bool door_array(const char* wall, uint32_t* flags, uint32_t shift) {
    bool ch = false;
    int nd = (int)PROTO_DOORS_AT(*flags, shift);
    igTableNextRow(0, 0);
    igTableNextColumn();
    igPushID_Str(wall);
    igIndent(12.0f);
    bool open = igTreeNodeEx_Str(TR("Doors"), ImGuiTreeNodeFlags_SpanAvailWidth);
    igSetItemTooltip("%s", TR("The doors in this wall, spread evenly along it (1 to 8)."));
    igUnindent(12.0f);
    igTableNextColumn();
    igAlignTextToFramePadding();
    igTextDisabled(TR("%d Array elements"), nd);
    igSameLine(0, 8);
    igBeginDisabled(nd >= 8);
    if (ui_icon_button("add", ICON_PLUS, NULL, TH(TK_N14), false, TR("Add a door"))) nd++, ch = true;
    igEndDisabled();
    igSameLine(0, 4);
    igBeginDisabled(nd <= 1);
    if (ui_icon_button("clear", ICON_DELETE, NULL, TH(TK_N14), false, TR("Remove the doors but one"))) nd = 1, ch = true;
    igEndDisabled();
    if (open) {
        for (int i = 0; i < nd; i++) {
            igTableNextRow(0, 0);
            igTableNextColumn();
            igIndent(28.0f);
            igAlignTextToFramePadding();
            igText(TR("Door %d"), i + 1);
            igUnindent(28.0f);
            igTableNextColumn();
            igPushID_Int(i);
            igTextDisabled("%s", TR("spread evenly"));
            if (nd > 1) {
                igSameLine(0, 8);
                if (igSmallButton("x")) nd--, ch = true;
                igSetItemTooltip("%s", TR("Remove this door"));
            }
            igPopID();
        }
        igTreePop();
    }
    igPopID();
    if (ch) {
        nd = nd < 1 ? 1 : nd > 8 ? 8 : nd;
        *flags = (*flags & ~(7u << shift)) | ((uint32_t)(nd - 1) << shift);
    }
    return ch;
}

static bool proto_details(CProto* p) {
    bool ch = false;
    label_cell("Shape");
    int k = (int)(p->kind < PROTO_KIND_COUNT ? p->kind : 0);
    const char* kn[PROTO_KIND_COUNT];
    for (int i = 0; i < PROTO_KIND_COUNT; i++) kn[i] = TR(g_proto_kind_names[i]);
    if (igCombo_Str_arr("##pkind", &k, kn, PROTO_KIND_COUNT, -1)) p->kind = (uint32_t)k, ch = true;
    ch |= row_float("Length", &p->size[0], 5.0f, 1.0f, 1e5f, "%.0f cm");
    ch |= row_float("Width", &p->size[1], 5.0f, 1.0f, 1e5f, "%.0f cm");
    ch |= row_float("Height", &p->size[2], 5.0f, 1.0f, 1e5f, "%.0f cm");
    if (p->kind >= PROTO_WALL) ch |= row_float("Thickness", &p->thickness, 1.0f, 1.0f, 1000.0f, "%.0f cm");
    if (p->kind == PROTO_STAIRS) {
        label_cell("Steps");
        int st = (int)p->steps;
        if (igDragInt("##psteps", &st, 0.2f, 1, 256, "%d", 0)) p->steps = (uint32_t)(st < 1 ? 1 : st), ch = true;
        float rise = p->size[2] / (float)(p->steps ? p->steps : 1);
        label_cell("");
        igTextDisabled(TR("%.1f cm per step%s"), rise, rise > 45.0f ? TR(" (over 45: a Character can't climb it)") : "");
    }
    if (p->kind == PROTO_WALL || p->kind == PROTO_ROOM || p->kind == PROTO_CORRIDOR) {
        ch |= row_float("Door Width", &p->opening[0], 1.0f, 0.0f, 1e5f, "%.0f cm");
        if (p->opening[0] > 0.0f) {
            ch |= row_float("Door Height", &p->opening[1], 1.0f, 1.0f, 1e5f, "%.0f cm");
            ch |= row_float("Door Offset", &p->opening[2], 1.0f, -1e5f, 1e5f, "%.0f cm");
            // (2026-11-10) more doors: each wall that has doors gets an array of them (Unreal's: the count, + and the
            // bin, the elements below), spread evenly along that wall
            static const struct { const char* label; uint32_t bit, shift; bool room_only; } d[4] = {
                { "Front Door", PROTO_DOOR_FRONT, PROTO_DOORS_SHIFT, false }, { "Back Door", PROTO_DOOR_BACK, PROTO_DOORS_BACK_SHIFT, false },
                { "Left Door", PROTO_DOOR_LEFT, PROTO_DOORS_LEFT_SHIFT, true }, { "Right Door", PROTO_DOOR_RIGHT, PROTO_DOORS_RIGHT_SHIFT, true } };
            for (int i = 0; i < 4; i++) {
                if (p->kind == PROTO_WALL && i > 0) break; // (a wall: one row of doors)
                if (d[i].room_only && p->kind != PROTO_ROOM) continue;
                bool always = p->kind == PROTO_WALL || (d[i].bit == PROTO_DOOR_FRONT && p->kind == PROTO_ROOM); // (a room's front door: always)
                uint32_t on = always || (p->flags & d[i].bit) != 0;
                if (!always && row_check(d[i].label, &on)) p->flags = on ? p->flags | d[i].bit : p->flags & ~d[i].bit, ch = true;
                if (on) ch |= door_array(always && p->kind == PROTO_WALL ? "Doors" : d[i].label, &p->flags, d[i].shift);
            }
        }
    }
    if (p->kind == PROTO_ROOM || p->kind == PROTO_CORRIDOR) {
        uint32_t ceil = (p->flags & PROTO_CEILING) != 0;
        if (row_check("Ceiling", &ceil)) p->flags = ceil ? p->flags | PROTO_CEILING : p->flags & ~PROTO_CEILING, ch = true;
    }
    ch |= row_color("Color", p->color, false);
    return ch;
}

static bool crowd_details(CCrowd* c) {
    bool ch = false;
    label_cell("Agents");
    int n = (int)c->count;
    if (igDragInt("##count", &n, 10.0f, 0, 100000, "%d", 0)) c->count = (uint32_t)(n < 0 ? 0 : n), ch = true;
    label_cell("Skeletal Mesh");
    ch |= field_skeletal_mesh("##mesh", &c->mesh, "SK_Mannequin"); // (Q6: the asset picker; none: the Mannequin)
    ch |= row_seq("Idle", &c->anim[0]);
    ch |= row_seq("Walk", &c->anim[1]);
    ch |= row_seq("Run", &c->anim[2]);
    ch |= row_float("Spawn Radius", &c->radius, 10.0f, 50.0f, 1e6f, "%.0f cm");
    ch |= row_float("Walk Speed", &c->walk_speed, 1.0f, 0.0f, 5000.0f, "%.0f cm/s");
    ch |= row_float("Run Speed", &c->run_speed, 1.0f, 0.0f, 5000.0f, "%.0f cm/s");
    static const char* const beh[CROWD_BEHAVIOR_COUNT] = { "Wander", "Seek", "Surround", "Flee" };
    static const char* const mind[CROWD_MIND_COUNT] = { "Hive", "Individual" };
    label_cell("Behavior");
    int b = (int)(c->behavior < CROWD_BEHAVIOR_COUNT ? c->behavior : 0);
    const char* bl[CROWD_BEHAVIOR_COUNT];
    for (int k = 0; k < CROWD_BEHAVIOR_COUNT; k++) bl[k] = TR(beh[k]);
    if (igCombo_Str_arr("##beh", &b, bl, CROWD_BEHAVIOR_COUNT, -1)) c->behavior = (uint32_t)b, ch = true;
    label_cell("Mind");
    int m = (int)(c->mind < CROWD_MIND_COUNT ? c->mind : 0);
    const char* ml[CROWD_MIND_COUNT] = { TR(mind[0]), TR(mind[1]) };
    if (igCombo_Str_arr("##mind", &m, ml, CROWD_MIND_COUNT, -1)) c->mind = (uint32_t)m, ch = true;
    ch |= row_float("Sight", &c->sight, 10.0f, 0.0f, 1e6f, "%.0f cm");
    if (c->mind == CROWD_INDIVIDUAL) ch |= row_float("Memory", &c->memory, 0.1f, 0.0f, 120.0f, "%.1f s");
    ch |= row_float("Prediction", &c->prediction, 0.05f, 0.0f, 10.0f, "%.2f s");
    ch |= row_float("Reaction", &c->reaction, 0.01f, 0.0f, 5.0f, "%.2f s");
    ch |= row_slider("Aggression", &c->aggression, 0.0f, 1.0f);
    if (c->behavior == CROWD_SURROUND) ch |= row_float("Ring Radius", &c->ring_radius, 5.0f, 100.0f, 1e5f, "%.0f cm");
    ch |= row_float("Separation", &c->separation, 1.0f, 20.0f, 1000.0f, "%.0f cm");
    ch |= row_slider("Variety", &c->variety, 0.0f, 1.0f);
    ch |= row_slider("Looks", &c->looks, 0.0f, 1.0f); // (Q27.9)
    ch |= row_check("City Routine", &c->routine); // (Q27.10)
    label_cell("Seed");
    int sd = (int)c->seed;
    if (igDragInt("##seed", &sd, 1.0f, 0, 1 << 30, "%d", 0)) c->seed = (uint32_t)sd, ch = true;
    return ch;
}

// (the Details' undo step keeps each component's bytes before an edit: the biggest must fit, checked in editor_init)
#define DETAILS_COMP_MAX 4096

// World Partition (B1): the level's settings, its Data Layers and the map of its cells
static void wp_map(Editor* ed, const CWorldPartition* c);
static bool wp_details(Editor* ed, World* w, Entity e) {
    bool changed = false;
    CWorldPartition* wpc = ecs_get(w, e, COMP_WORLD_PARTITION);
    if (!wpc || !begin_section("World Partition", "sec_wp")) return false;
    changed |= row_check("Enable Streaming", &wpc->enabled);
    float cs = wpc->cell_size / 100.0f, lr = wpc->loading_range / 100.0f, ms = wpc->hlod_min_size / 100.0f, kp = wpc->hlod_keep * 100.0f;
    if (row_float("Cell Size", &cs, 1.0f, 16.0f, 4096.0f, "%.0f m")) wpc->cell_size = cs * 100.0f, changed = true;
    if (row_float("Loading Range", &lr, 1.0f, 16.0f, 50000.0f, "%.0f m")) wpc->loading_range = lr * 100.0f, changed = true;
    changed |= row_float("Streaming Budget", &wpc->budget_ms, 0.05f, 0.1f, 16.0f, "%.2f ms");
    changed |= row_check("HLOD", &wpc->hlod);
    label_cell("HLOD Region");
    int rg = (int)wpc->hlod_region;
    if (igSliderInt("##wprg", &rg, 1, 16, TR("%d cells a side"), 0)) wpc->hlod_region = (uint32_t)rg, changed = true;
    if (row_float("HLOD Triangles", &kp, 0.1f, 0.5f, 100.0f, "%.1f %%")) wpc->hlod_keep = kp / 100.0f, changed = true;
    if (row_float("HLOD Min Size", &ms, 0.05f, 0.0f, 100.0f, "%.2f m")) wpc->hlod_min_size = ms * 100.0f, changed = true;
    igEndTable();
    if (igButton(TR("Build HLODs"), (ImVec2){ 0, 0 })) build_hlods(ed, re_time_seconds());
    igSetItemTooltip("%s", TR("Each region's meshes merged and simplified into one (its materials as vertex colors), seen past the Loading Range."));
    igSeparatorText(TR("Data Layers"));
    int del = -1;
    const char* st[WP_LAYER_STATE_COUNT];
    for (int k = 0; k < WP_LAYER_STATE_COUNT; k++) st[k] = TR(g_wp_layer_state_names[k]);
    for (uint32_t i = 0; i < wpc->nlayers && i < WP_MAX_LAYERS; i++) {
        CWpLayer* L = &wpc->layers[i];
        igPushID_Int((int)i);
        bool vis = !L->editor_hidden;
        if (igCheckbox("##vis", &vis)) L->editor_hidden = !vis, wp_editor_layer_visible(w, (int)i, vis), changed = true;
        igSetItemTooltip("%s", TR("Its actors shown in the editor's viewport"));
        igSameLine(0, 4);
        igSetNextItemWidth(igGetFontSize() * 8.0f);
        if (igInputText("##name", L->name, sizeof L->name, 0, NULL, NULL)) changed = true;
        igSameLine(0, 4);
        igSetNextItemWidth(igGetFontSize() * 6.5f);
        int s0 = (int)L->initial;
        if (igCombo_Str_arr("##init", &s0, st, WP_LAYER_STATE_COUNT, -1)) L->initial = (uint32_t)s0, changed = true;
        igSetItemTooltip("%s", TR("Its state when the game starts: Unloaded (out), Loaded (in, hidden), Activated (in and shown). Set Data Layer Runtime State changes it."));
        igSameLine(0, 4);
        if (igSmallButton("x")) del = (int)i;
        igPopID();
    }
    if (del >= 0) {
        if (wpc->layers[del].editor_hidden) wp_editor_layer_visible(w, del, true);
        memmove(&wpc->layers[del], &wpc->layers[del + 1], (wpc->nlayers - (uint32_t)del - 1) * sizeof wpc->layers[0]);
        wpc->nlayers--;
        wp_editor_layer_removed(w, del);
        changed = true;
    }
    if (wpc->nlayers < WP_MAX_LAYERS && igButton(TR("+ Data Layer"), (ImVec2){ 0, 0 })) {
        CWpLayer* L = &wpc->layers[wpc->nlayers];
        memset(L, 0, sizeof *L);
        snprintf(L->name, sizeof L->name, "Layer%u", wpc->nlayers + 1);
        L->initial = WP_LAYER_ACTIVATED;
        wpc->nlayers++;
        changed = true;
    }
    wp_map(ed, wpc);
    return changed;
}

// An actor's (any with a place, when the level has a World Partition): Is Spatially Loaded, its Data Layers. Its
// component added on the first change (after the rest of the Details: adding moves the actor's row)
static bool wp_actor_details(World* w, Entity e, bool* add, CPartition* out) {
    Entity wpe = wp_settings(w);
    uint64_t m = ecs_mask(w, e);
    if (wpe == ENTITY_NONE || e == wpe || !(m & COMP_BIT(COMP_TRANSFORM)) || (m & (COMP_BIT(COMP_ATTACH) | COMP_BIT(COMP_OUTLINER_FOLDER))))
        return false;
    if (!begin_section("World Partition", "sec_wpa")) return false;
    const CWorldPartition* c = ecs_get(w, wpe, COMP_WORLD_PARTITION);
    CPartition* cp = (m & COMP_BIT(COMP_PARTITION)) ? ecs_get(w, e, COMP_PARTITION) : NULL;
    CPartition v = cp ? *cp : (CPartition){ 0 };
    bool changed = false;
    label_cell("Is Spatially Loaded");
    int sp = (int)v.spatial;
    const char* sn[WP_SPATIAL_COUNT];
    for (int k = 0; k < WP_SPATIAL_COUNT; k++) sn[k] = TR(g_wp_spatial_names[k]);
    if (igCombo_Str_arr("##wpsp", &sp, sn, WP_SPATIAL_COUNT, -1)) v.spatial = (uint32_t)sp, changed = true;
    label_cell("Streams");
    igTextDisabled("%s", wp_actor_spatial(w, e) ? TR("yes: with its grid cell") : TR("no: always in the world"));
    for (uint32_t i = 0; i < c->nlayers && i < WP_MAX_LAYERS; i++) {
        label_cell(i == 0 ? "Data Layers" : "");
        igPushID_Int((int)i);
        bool on = (v.layers >> i) & 1u;
        if (igCheckbox(c->layers[i].name, &on)) v.layers = on ? (v.layers | 1u << i) : (v.layers & ~(1u << i)), changed = true;
        igPopID();
    }
    igEndTable();
    if (changed) {
        if (cp) *cp = v;
        else *add = true, *out = v;
    }
    return changed;
}

static void details(Editor* ed) {
    static bool sizes_checked;
    if (!sizes_checked) { // (a component past the undo's room would overrun it)
        sizes_checked = true;
        for (int c = 0; c < COMP_COUNT; c++)
            if (g_components[c].size > DETAILS_COMP_MAX) printf("[RealEngine] FAIL: the component %s (%u bytes) is past the Details' undo room\n", g_components[c].name, g_components[c].size);
    }
    World* w = &ed->world;
    panel_header(TR("Details"));
    igSpacing();
    if (!ecs_alive(w, ed->selected)) { // (Q26) centered in the panel, as the mockups have it
        const char* hint = TR("Select an actor to view its details.");
        ImVec2 ts = igCalcTextSize(hint, NULL, false, -1), av = igGetContentRegionAvail();
        if (av.x > ts.x) igSetCursorPosX(igGetCursorPosX() + (av.x - ts.x) * 0.5f);
        igSetCursorPosY(igGetCursorPosY() + fminf(fmaxf(av.y * 0.3f - ts.y, 0.0f), igGetFontSize() * 6.0f));
        igTextDisabled("%s", hint);
        return;
    }
    Entity e = ed->selected;
    // The components as they are before this frame's edits (for the undo step of the first change)
    static uint8_t before[COMP_COUNT][DETAILS_COMP_MAX];
    uint64_t mask = ecs_mask(w, e);
    for (int c = 0; c < COMP_COUNT; c++)
        if (mask & COMP_BIT(c)) memcpy(before[c], ecs_get(w, e, (ComponentId)c), g_components[c].size);
    g_changed = false;

    CName* n = ecs_get(w, e, COMP_NAME);
    ui_icon_item(type_icon(w, e), igGetFrameHeight(), TH(TK_N14));
    igSameLine(0, 6);
    if (n) {
        float fh = igGetFrameHeight(); // the "+ Add" button's width (ui_icon_button): room for the translated label
        igSetNextItemWidth(-(fh * 1.52f + 10.0f + igCalcTextSize(TR("Add"), NULL, true, -1).x));
        if (igInputText("##name", n->text, sizeof n->text, 0, NULL, NULL)) {
            g_changed = true;
            ecs_touch(w, e);
        }
        igSameLine(0, 4);
    }
    if (look_primary_button("dt_add", ICON_PLUS, TR("Add"), TR("Add a component"))) igOpenPopup_Str("add_component", 0); // (Q26: the primary button)
    if (igBeginPopup("add_component", 0)) {
        bool any = false;
        for (int c = 0; c < COMP_COUNT; c++) {
            if (!g_components[c].user_addable || (mask & COMP_BIT(c))) continue;
            if (BP_COMP_GAMEPLAY(c) && !plugins_gameplay_enabled()) continue; // (the Gameplay Framework plugin off)
            any = true;
            if (ui_menu_item(ICON_COMPONENT, TR(g_components[c].name), NULL, false, true)) {
                editor_undo_push(ed);
                ecs_add(w, e, (ComponentId)c);
                if (c == COMP_ROTATING) ((CRotating*)ecs_get(w, e, COMP_ROTATING))->rate[1] = 90.0f;
                CTransform* mt = ecs_get(w, e, COMP_TRANSFORM);
                if (c == COMP_RIGID_BODY && mt) mt->flags = (mt->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
                if (c == COMP_SKELETAL_MESH) { // the Mannequin to start with (Unreal's default)
                    CSkeletalMesh* nk = ecs_get(w, e, COMP_SKELETAL_MESH);
                    const SkelMesh* mq = anim_mesh_name("SK_Mannequin");
                    if (nk && mq) nk->mesh = mq->hash;
                    if (mt) mt->flags = (mt->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
                }
                if (c == COMP_SAVEABLE) ((CSaveable*)ecs_get(w, e, COMP_SAVEABLE))->id = progress_new_id(); // (its key in the saves, kept when renamed)
                bool ia = c == COMP_INTERACTABLE || c == COMP_INTERACTOR;
                if (c == COMP_DEVICE && ecs_add(w, e, COMP_INTERACTABLE)) { // (a device is used through its Interactable)
                    mt = ecs_get(w, e, COMP_TRANSFORM);
                    if (mt) mt->flags = (mt->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
                    ia = true;
                }
                if (ia && interaction_prompt_asset()) // (interaction.h)
                    editor_status(ed, re_time_seconds(), "%s", TR("Interaction added: in Play the player's Interactor focuses it and shows Content/UI/WBP_InteractPrompt."));
                if (c == COMP_DIALOGUE_SPEAKER) dlg_widget_asset(); // (WBP_Dialogue into Content/UI when missing)
                if (c == COMP_QUEST_LOG) quest_tracker_asset(); // (WBP_QuestTracker into Content/UI when missing)
                if (c == COMP_GAME_RULES && rules_hud_asset() && rules_end_asset()) // (game_rules.h: the example widgets)
                    editor_status(ed, re_time_seconds(), "%s", TR("Game Rules added: in Play the match runs; Content/UI/WBP_MatchHUD and WBP_MatchEnd show it."));
                if (c == COMP_ABILITY_SYSTEM && abil_bar_asset()) // (WBP_AbilityBar into Content/UI when missing)
                    editor_status(ed, re_time_seconds(), "%s", TR("Ability System added. Give it Gameplay Abilities and Effects; the example bar: Content/UI/WBP_AbilityBar (Show Ability Bar)."));
                if (c == COMP_HUD_LAYOUT && hud_layout_asset()) // (WBP_GameHUD into Content/UI when missing: hud.h)
                    editor_status(ed, re_time_seconds(), "%s", TR("HUD Layout added: in Play it shows Content/UI/WBP_GameHUD (bars, compass, messages, markers, crosshair)."));
                if (c == COMP_HEALTH && health_bar_asset()) // (the example widget into Content/UI when missing)
                    editor_status(ed, re_time_seconds(), "%s", TR("Health added. The example health bar: Content/UI/WBP_HealthBar (Create Widget, or Show Health Bar on the player)."));
            }
        }
        if (!any) igTextDisabled("%s", TR("Every component is added."));
        igEndPopup();
    }
    igTextDisabled("%s", TR(type_label(w, e)));

    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (t && begin_section("Transform", "sec_transform")) {
        g_changed |= vec3_row("Location", t->location, 1.0f, "%.1f", 0.0f);
        g_changed |= vec3_row("Rotation", t->rotation, 0.5f, "%.1f\xc2\xb0", 0.0f);
        g_changed |= vec3_row("Scale", t->scale, 0.01f, "%.3f", 1.0f);
        label_cell("Mobility");
        static const char* const mob[3] = { "Static", "Stationary", "Movable" };
        uint32_t cur = t->flags & TRANSFORM_MOBILITY_MASK;
        float bw = (igGetContentRegionAvail().x - 8) / 3.0f;
        for (int i = 0; i < 3; i++) {
            if (i) igSameLine(0, 4);
            if (cur == (uint32_t)i) igPushStyleColor_Vec4(ImGuiCol_Button, ACCENT);
            char lab[48];
            snprintf(lab, sizeof lab, "%s##mob%d", TR(mob[i]), i);
            if (igButton(lab, (ImVec2){ bw, 0 }) && cur != (uint32_t)i) {
                t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | (uint32_t)i;
                g_changed = true;
            }
            if (cur == (uint32_t)i) igPopStyleColor(1);
        }
        label_cell("Show Icon");
        bool show_icon = !(t->flags & TRANSFORM_NO_ICON);
        if (igCheckbox("##show_icon", &show_icon)) {
            editor_undo_push(ed);
            t->flags = show_icon ? (t->flags & ~TRANSFORM_NO_ICON) : (t->flags | TRANSFORM_NO_ICON);
            g_changed = true;
        }
        igEndTable();
    }
    CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    if (sm && begin_section("Static Mesh", "sec_mesh")) {
        label_cell("Static Mesh");
        if (field_static_mesh("##mesh", &sm->mesh)) g_changed = true; // (Q6: the asset picker, the basic shapes first)
        g_changed |= row_check("Cast Shadow", &sm->cast_shadow);
        g_changed |= row_channels(&sm->light_channels); // (Q13)
        if (t) { // Unreal's Collision Presets, simply: blocks (Characters, traces, the camera boom) or NoCollision
            uint32_t coll = !(t->flags & TRANSFORM_NO_COLLISION);
            if (row_check("Collision Enabled", &coll)) t->flags = coll ? t->flags & ~TRANSFORM_NO_COLLISION : t->flags | TRANSFORM_NO_COLLISION, g_changed = true;
        }
        if (!ed->playing && !ecs_get(w, e, COMP_ATTACH)) { // (an imported car, boat, plane...: a vehicle from it)
            label_cell("");
            if (igButton(TR("Make Vehicle..."), (ImVec2){ -1, 0 })) igOpenPopup_Str("make_vehicle", 0);
            if (igBeginPopup("make_vehicle", 0)) {
                make_vehicle_menu(ed);
                igEndPopup();
            }
        }
        if (render_mesh_slot(sm->mesh) >= SHAPE_COUNT && !ed->playing) { // an imported mesh of a person: rigged onto the Mannequin
            label_cell("");
            if (igButton(TR("Auto Rig (Humanoid)"), (ImVec2){ -1, 0 })) {
                editor_undo_push(ed);
                TlendReport r;
                bool ok = autorig_humanoid(w, e, "/Content/AutoRig", content_root(), &r);
                if (ok) render_meshes_reload_async(ed->vk), anim_rescan(), content_mark_dirty();
                editor_status(ed, igGetTime(), ok ? TR("Rigged: %s") : TR("Could not rig: %s"), r.text);
                igEndTable();
                return;
            }
            igSetItemTooltip("%s", TR("Makes a Skeletal Mesh on the Mannequin's skeleton from this mesh (feet on the ground, facing +X; a T-pose's arms are laid down): every Mannequin animation plays on it."));
        }
        igEndTable();
    }
    if (sm && begin_section("Materials", "sec_material")) {
        // the material asset (a .rmat or a Material Instance); none: the colors below
        label_cell("Material");
        if (field_material("##mat", &sm->material, TR("None (the colors below)"), false)) g_changed = true; // (Q6: the asset picker; a drop too)
        Material* cur = sm->material ? mat_find_hash(sm->material) : NULL;
        if (cur) {
            if (sm->uv_scale <= 0.0f) sm->uv_scale = 1.0f;
            g_changed |= row_float("UV Tiling", &sm->uv_scale, 0.01f, 0.01f, 1000.0f, "%.2f");
            label_cell("");
            if (igSmallButton(TR("Edit Material"))) {
                char vp[700];
                if (mat_vpath(cur, vp, sizeof vp)) mat_editor_open(vp);
            }
        }
        g_changed |= row_color("Base Color", sm->base_color, false);
        g_changed |= row_slider("Metallic", &sm->metallic, 0.0f, 1.0f);
        g_changed |= row_slider("Roughness", &sm->roughness, 0.0f, 1.0f);
        g_changed |= row_color("Emissive Color", sm->emissive, true);
        { // (Q11) the actor's own outline over its material's
            label_cell("Actor Outline");
            int om = (int)outline_actor_mode(sm->outline);
            const char* on[OUTLINE_ACTOR_COUNT] = { TR("As the material"), TR("On"), TR("Off") };
            if (igCombo_Str_arr("##actor_outline", &om, on, OUTLINE_ACTOR_COUNT, -1)) {
                float c[3];
                toon_outline_color(sm->outline, c);
                if (om == OUTLINE_ACTOR_ON && !(sm->outline & 0xFFFFFFu) && sm->outline_width <= 0.0f) c[0] = c[1] = c[2] = 0.0f, sm->outline_width = 3.0f;
                sm->outline = toon_outline_pack(om, c);
                g_changed = true;
            }
            if (om == OUTLINE_ACTOR_ON) {
                float c[3];
                toon_outline_color(sm->outline, c);
                if (row_color("Actor Outline Color", c, false)) sm->outline = toon_outline_pack(om, c), g_changed = true;
                g_changed |= row_float("Actor Outline Width", &sm->outline_width, 0.05f, 0.0f, 12.0f, "%.1f px");
            }
        }
        igEndTable();
    }
    CLight* l = ecs_get(w, e, COMP_LIGHT);
    if (l && begin_section("Light", "sec_light")) {
        label_cell("Type");
        int type = (int)l->type;
        const char* names[LIGHT_TYPE_COUNT];
        for (int i = 0; i < LIGHT_TYPE_COUNT; i++) names[i] = TR(g_light_names[i]);
        if (igCombo_Str_arr("##type", &type, names, LIGHT_TYPE_COUNT, -1)) {
            l->type = (uint32_t)type;
            g_changed = true;
        }
        if (!l->ext) { // a light saved before the options: their defaults (what lights.c used for it)
            l->temperature = 6500.0f;
            l->affects_world = 1;
            l->indirect_intensity = l->specular_scale = l->volumetric_scattering = 1.0f;
            l->ext = 1;
        }
        // physical units (notas-dev/luz.md), stored in lux for the sun, candela for the others, a scale for the sky light;
        // shown in the units the user picks (Intensity Units, like Unreal's; Watts are Blender's: the Tlender's lights)
        if (l->type == LIGHT_SKY) {
            g_changed |= row_float("Intensity", &l->intensity, 0.01f, 0.0f, 100.0f, "%.2f");
        } else {
            static const char* const local_units[LIGHT_UNITS_COUNT] = { "Candelas", "Lumens", "EV", "Watts (Blender)" };
            static const char* const sun_units[2] = { "Lux", "W/m\xc2\xb2 (Blender)" };
            bool sun = l->type == LIGHT_DIRECTIONAL;
            if (sun && l->units != LIGHT_UNITS_CANDELAS && l->units != LIGHT_UNITS_WATTS) l->units = LIGHT_UNITS_CANDELAS;
            if (l->units >= LIGHT_UNITS_COUNT) l->units = LIGHT_UNITS_CANDELAS;
            label_cell("Intensity Units");
            int u = sun ? (l->units == LIGHT_UNITS_WATTS ? 1 : 0) : (int)l->units;
            const char* un[LIGHT_UNITS_COUNT];
            for (int i = 0; i < (sun ? 2 : LIGHT_UNITS_COUNT); i++) un[i] = TR(sun ? sun_units[i] : local_units[i]);
            igSetNextItemWidth(-1);
            if (igCombo_Str_arr("##units", &u, un, sun ? 2 : LIGHT_UNITS_COUNT, -1)) { // the light keeps its brightness
                l->units = sun ? (u ? LIGHT_UNITS_WATTS : LIGHT_UNITS_CANDELAS) : (uint32_t)u;
                g_changed = true;
            }
            float v = lights_to_units(l, (LightUnits)l->units);
            static const char* const local_fmt[LIGHT_UNITS_COUNT] = { "%.1f cd", "%.0f lm", "%.2f EV", "%.2f W" };
            const char* fmt = sun ? (l->units == LIGHT_UNITS_WATTS ? "%.2f W/m\xc2\xb2" : "%.0f lux") : local_fmt[l->units];
            float speed = l->units == LIGHT_UNITS_EV ? 0.05f : fmaxf(fabsf(v) * 0.005f, 0.01f);
            if (row_float("Intensity", &v, speed, l->units == LIGHT_UNITS_EV ? -20.0f : 0.0f, 1e9f, fmt)) {
                lights_from_units(l, (LightUnits)l->units, v);
                g_changed = true;
            }
        }
        g_changed |= row_color("Light Color", l->color, false);
        g_changed |= row_check("Use Temperature", &l->use_temperature);
        if (l->use_temperature) g_changed |= row_float("Temperature", &l->temperature, 10.0f, 1700.0f, 12000.0f, "%.0f K");
        if (l->type == LIGHT_POINT || l->type == LIGHT_SPOT || l->type == LIGHT_RECT)
            g_changed |= row_float("Attenuation Radius", &l->attenuation_radius, 5.0f, 1.0f, 100000.0f, "%.0f cm");
        if (l->type == LIGHT_POINT || l->type == LIGHT_SPOT)
            g_changed |= row_float("Source Radius", &l->source_radius, 0.5f, 0.0f, 1000.0f, "%.1f cm");
        if (l->type == LIGHT_SPOT) {
            g_changed |= row_float("Inner Cone Angle", &l->inner_cone, 0.25f, 0.0f, 80.0f, "%.1f\xc2\xb0");
            g_changed |= row_float("Outer Cone Angle", &l->outer_cone, 0.25f, 1.0f, 80.0f, "%.1f\xc2\xb0");
        }
        if (l->type == LIGHT_RECT) {
            g_changed |= row_float("Source Width", &l->source_width, 1.0f, 1.0f, 10000.0f, "%.0f cm");
            g_changed |= row_float("Source Height", &l->source_height, 1.0f, 1.0f, 10000.0f, "%.0f cm");
        }
        if (l->type != LIGHT_SKY) g_changed |= row_check("Cast Shadows", &l->cast_shadows);
        if (l->type == LIGHT_DIRECTIONAL) { // (Q21) the terrain's shadow kilometers past the virtual maps (farview.h)
            g_changed |= row_check("Far Shadow", &l->far_shadow);
            if (l->far_shadow) g_changed |= row_slider("Far Shadow Strength", &l->far_shadow_strength, 0.0f, 1.0f);
        }
        g_changed |= row_check("Affects World", &l->affects_world);
        if (l->type != LIGHT_SKY) {
            // Unreal's scales (1: physical): the light's share in the GI, its highlights, its light in the volumetric fog
            g_changed |= row_float("Indirect Lighting Intensity", &l->indirect_intensity, 0.01f, 0.0f, 3.9f, "%.2f");
            g_changed |= row_float("Specular Scale", &l->specular_scale, 0.01f, 0.0f, 3.9f, "%.2f");
            g_changed |= row_float("Volumetric Scattering Intensity", &l->volumetric_scattering, 0.01f, 0.0f, 3.9f, "%.2f");
        }
        if (l->type == LIGHT_SPOT || l->type == LIGHT_POINT || l->type == LIGHT_DIRECTIONAL) { // (2026-11-17, Q4) Unreal's Light Function
            label_cell("Light Function");
            if (field_material("##lightfn", &l->light_function, TR("None"), true)) g_changed = true; // (Q6: the Light Function materials)
            if (l->light_function && l->type == LIGHT_DIRECTIONAL) {
                if (l->light_function_scale <= 1.0f) l->light_function_scale = 1000.0f;
                g_changed |= row_float("Light Function Scale", &l->light_function_scale, 10.0f, 10.0f, 1000000.0f, "%.0f cm");
            }
        }
        if (l->type != LIGHT_SKY) { // (Q11) how it treats the toon
            label_cell("Toon Light");
            int tm = (int)(l->toon_mode > 2u ? 0u : l->toon_mode);
            const char* tn[3] = { TR("As the material"), TR("Force Toon"), TR("Never Toon") };
            if (igCombo_Str_arr("##toon_light", &tm, tn, 3, -1)) l->toon_mode = (uint32_t)tm, g_changed = true;
        }
        if (l->type != LIGHT_SKY) g_changed |= row_channels(&l->light_channels); // (Q13) Unreal's Lighting Channels
        igEndTable();
    }
    if (l && l->type != LIGHT_SKY && begin_section("Light Flicker", "sec_flicker")) { // (Q13) the horror kit (terror.h)
        label_cell("Flicker Pattern");
        int fp = (int)l->flicker;
        if (igBeginCombo("##flk", TR(light_flicker_name(fp)), 0)) {
            for (int k = 0; k < FLICKER_COUNT; k++)
                if (igSelectable_Bool(TR(light_flicker_name(k)), fp == k, 0, (ImVec2){ 0, 0 })) {
                    l->flicker = (uint32_t)k, g_changed = true;
                    if (k == FLICKER_LIGHTNING && l->flicker_sound_delay <= 0.0f) l->flicker_sound_delay = 1.5f; // (the thunder ~500 m away)
                }
            igEndCombo();
        }
        if (l->flicker) {
            float amt = l->flicker_amount > 0.0f ? l->flicker_amount : 1.0f, spd = l->flicker_speed > 0.0f ? l->flicker_speed : 1.0f;
            if (row_slider("Flicker Amount", &amt, 0.0f, 1.0f)) l->flicker_amount = fmaxf(amt, 1e-3f), g_changed = true;
            if (row_float("Flicker Speed", &spd, 0.01f, 0.05f, 20.0f, "x%.2f")) l->flicker_speed = spd, g_changed = true;
            g_changed |= row_sound_hash("Flicker Sound", &l->flicker_sound);
            if (l->flicker_sound) g_changed |= row_float("Flicker Sound Delay", &l->flicker_sound_delay, 0.05f, 0.0f, 30.0f, "%.2f s");
        }
        g_changed |= row_slider("Tension Flicker", &l->flicker_tension, 0.0f, 1.0f);
        label_cell("Rim Light");
        int rm = (int)l->rim;
        const char* rim_names[3] = { TR("Off"), TR("Behind the Player"), TR("Behind an Actor") };
        if (igCombo_Str_arr("##rim", &rm, rim_names, 3, -1)) l->rim = (uint32_t)rm, g_changed = true;
        if (l->rim == RIM_ACTOR) {
            label_cell("Rim Target");
            g_changed |= igInputTextWithHint("##rimt", TR("an actor's name"), l->rim_target, sizeof l->rim_target, 0, NULL, NULL);
        }
        if (l->rim) {
            float rd = l->rim_distance > 0.0f ? l->rim_distance : 250.0f, rh = l->rim_height != 0.0f ? l->rim_height : 120.0f;
            if (row_float("Rim Distance", &rd, 1.0f, 10.0f, 5000.0f, "%.0f cm")) l->rim_distance = rd, g_changed = true;
            if (row_float("Rim Height", &rh, 1.0f, -500.0f, 1000.0f, "%.0f cm")) l->rim_height = rh != 0.0f ? rh : 0.01f, g_changed = true;
            g_changed |= row_float("Rim Side", &l->rim_side, 1.0f, -1000.0f, 1000.0f, "%.0f cm");
        }
        if (l->type == LIGHT_SPOT || l->type == LIGHT_POINT) g_changed |= row_check("Beam Dust", &l->beam_dust);
        igEndTable();
    }
    CParticleSystem* psc = ecs_get(w, e, COMP_PARTICLE_SYSTEM);
    if (psc && begin_section("Particle System", "sec_particles")) { // Unreal's Niagara Component
        label_cell("Particle System");
        ParticleSystemAsset* cur = ps_find_hash(psc->asset);
        { // (Q6) the asset picker
            char vp[700] = "", out[700];
            if (cur) vpath_of_disk(cur->path, vp, sizeof vp);
            if (asset_field("##ps", cur ? cur->name : TR("(none)"), vp, ASSET_PARTICLE_SYSTEM, AK(ASSET_PARTICLE_SYSTEM), false, out, sizeof out) ==
                    ASSET_FIELD_PICKED && out[0]) {
                ParticleSystemAsset* a = ps_load(out);
                if (a) psc->asset = a->hash, g_changed = true, cur = a;
            }
        }
        if (cur) {
            label_cell("");
            if (igButton(TR("Edit..."), (ImVec2){ -1, 0 })) {
                char vp[700];
                snprintf(vp, sizeof vp, "/Content%s", cur->path + strlen(content_root()));
                particle_editor_open(vp);
            }
        }
        uint32_t aa = psc->auto_activate;
        if (row_check("Auto Activate", &aa)) psc->auto_activate = (uint8_t)aa, psc->active = (uint8_t)aa, g_changed = true;
        g_changed |= row_float("Time Scale", &psc->time_scale, 0.01f, 0.0f, 100.0f, "%.2f");
        g_changed |= row_float("Spawn Rate Scale", &psc->spawn_scale, 0.01f, 0.0f, 100.0f, "%.2f");
        g_changed |= row_float("Size Scale", &psc->size_scale, 0.01f, 0.0f, 100.0f, "%.2f");
        g_changed |= row_color("Tint", psc->color, true);
        float seed = (float)psc->seed;
        if (row_float("Seed", &seed, 1.0f, 0.0f, 1e6f, "%.0f")) psc->seed = (uint32_t)seed, g_changed = true;
        igEndTable();
    }
    CVehicle* veh = ecs_get(w, e, COMP_VEHICLE);
    if (veh && begin_section("Vehicle", "sec_vehicle")) { // Unreal's Chaos Vehicle Movement Component
        label_cell("Vehicle");
        VehicleAsset* cur = veh_find(veh->asset);
        { // (Q6) the asset picker
            char vp[700] = "", out[700];
            if (cur && cur->path[0]) vpath_of_disk(cur->path, vp, sizeof vp);
            if (asset_field("##veh", cur ? cur->name : TR("(none)"), vp, ASSET_VEHICLE, AK(ASSET_VEHICLE), false, out, sizeof out) ==
                    ASSET_FIELD_PICKED && out[0]) {
                VehicleAsset* a = veh_load(out);
                if (a) veh->asset = a->hash, g_changed = true, cur = a;
            }
        }
        if (cur && cur->path[0]) {
            label_cell("");
            if (igButton(TR("Edit..."), (ImVec2){ -1, 0 })) {
                char vp[700];
                snprintf(vp, sizeof vp, "/Content%s", cur->path + strlen(content_root()));
                vehicle_editor_open(vp);
            }
        }
        g_changed |= row_check("Auto Possess", &veh->auto_possess);
        g_changed |= row_check("Engine On at Start", &veh->start_engine);
        if (cur && cur->kind == VK_TRAIN) {
            label_cell("Rail");
            igSetNextItemWidth(-1);
            g_changed |= igInputText("##rail", veh->rail, sizeof veh->rail, 0, NULL, NULL);
            g_changed |= row_float("Start Along Rail", &veh->rail_start, 0.5f, 0.0f, 100000.0f, "%.1f m");
            label_cell("Couple To");
            igSetNextItemWidth(-1);
            g_changed |= igInputText("##couple", veh->couple_to, sizeof veh->couple_to, 0, NULL, NULL);
            g_changed |= row_float("Coupling Gap", &veh->couple_gap, 0.05f, 0.0f, 50.0f, "%.2f m");
        }
        igEndTable();
    }
    CProcWorld* pgw = ecs_get(w, e, COMP_PROC_WORLD);
    if (pgw && pgw->mode == PROCGEN_PCG) g_changed |= pcg_details(w, e); // (Q15, pcg_editor.c)
    else if (pgw && begin_section("Procedural World", "sec_procgen")) {
        label_cell("Mode");
        {
            const char* modes[PROCGEN_MODE_COUNT] = { TR(g_procgen_mode_names[0]), TR(g_procgen_mode_names[1]) };
            int v = (int)pgw->mode;
            igSetNextItemWidth(-1);
            if (igCombo_Str_arr("##pgmode", &v, modes, PROCGEN_MODE_COUNT, -1)) pgw->mode = (uint32_t)v, g_changed = true;
        }
        float seed = (float)pgw->seed;
        if (row_float("Seed", &seed, 1.0f, 0.0f, 4e9f, "%.0f")) pgw->seed = (uint32_t)seed, g_changed = true;
        if (pgw->mode == PROCGEN_TERRAIN) {
            g_changed |= row_float("Block Width", &pgw->block, 5.0f, 25.0f, 5000.0f, "%.0f cm");
            g_changed |= row_float("Height Step", &pgw->step, 1.0f, 5.0f, 1000.0f, "%.0f cm");
            g_changed |= row_float("View Distance", &pgw->view_distance, 1000.0f, 5000.0f, 2000000.0f, "%.0f cm");
            g_changed |= row_float("Mountain Height", &pgw->height, 50.0f, 0.0f, 200000.0f, "%.0f cm");
            g_changed |= row_float("Water Level", &pgw->water_level, 10.0f, -100000.0f, 100000.0f, "%.0f cm");
            g_changed |= row_float("Biome Size", &pgw->biome_scale, 1000.0f, 1000.0f, 10000000.0f, "%.0f cm");
            g_changed |= row_float("Tree Density", &pgw->tree_density, 0.1f, 0.0f, 100.0f, "%.1f");
            g_changed |= row_float("Structure Density", &pgw->structure_density, 0.1f, 0.0f, 100.0f, "%.1f / km2");
            g_changed |= row_float("Rebase Distance", &pgw->rebase_distance, 1000.0f, 0.0f, 10000000.0f, "%.0f cm");
        } else {
            float rooms = (float)pgw->rooms;
            if (row_float("Rooms", &rooms, 1.0f, 1.0f, 128.0f, "%.0f")) pgw->rooms = (uint32_t)rooms, g_changed = true;
            g_changed |= row_float("Room Size", &pgw->room_size, 10.0f, 300.0f, 5000.0f, "%.0f cm");
            g_changed |= row_float("Room Height", &pgw->room_height, 5.0f, 200.0f, 2000.0f, "%.0f cm");
            g_changed |= row_slider("Loops", &pgw->loop_chance, 0.0f, 1.0f);
        }
        g_changed |= row_float("Creature Density", &pgw->creature_density, 0.05f, 0.0f, 50.0f, "%.2f");
        for (int k = 0; k < 2; k++) { // the creatures' and the structures' Blueprint classes
            uint32_t* cls = k ? &pgw->structure_class : &pgw->creature_class;
            label_cell(k ? "Structure Class" : "Creature Class");
            const BpAsset* cur = *cls ? bp_find_hash(*cls) : NULL;
            igPushID_Int(k + 9100);
            igSetNextItemWidth(-1);
            if (igBeginCombo("##pgcls", cur ? cur->name : TR(k ? "(stone ruins)" : "(Mannequin with BT_Chase)"), 0)) {
                if (igSelectable_Bool(TR(k ? "(stone ruins)" : "(Mannequin with BT_Chase)"), !cur, 0, (ImVec2){ 0, 0 })) *cls = 0, g_changed = true;
                for (uint32_t i = 0; i < bp_asset_count(); i++) {
                    BpAsset* a = bp_asset_at(i);
                    if (a->kind == BPA_CLASS && igSelectable_Bool(a->name, a == cur, 0, (ImVec2){ 0, 0 })) *cls = a->hash, g_changed = true;
                }
                igEndCombo();
            }
            igPopID();
        }
        const ProcStats* ps = procgen_stats();
        label_cell("");
        if (pgw->mode == PROCGEN_TERRAIN) igTextDisabled(TR("%u chunks, %u columns, %u trees, %u structures"), ps->chunks, ps->columns, ps->trees, ps->structures);
        else igTextDisabled(TR("%u rooms"), ps->rooms);
        igEndTable();
    }
    CWater* wat = ecs_get(w, e, COMP_WATER);
    if (wat && begin_section("Water Body", "sec_water")) {
        g_changed |= row_float("Wave Height", &wat->wave_height, 0.5f, 0.0f, 1000.0f, "%.0f cm");
        g_changed |= row_float("Wave Length", &wat->wave_length, 5.0f, 10.0f, 100000.0f, "%.0f cm");
        g_changed |= row_float("Wave Speed", &wat->wave_speed, 1.0f, 0.0f, 10000.0f, "%.0f cm/s");
        g_changed |= row_float("Current X", &wat->current[0], 1.0f, -5000.0f, 5000.0f, "%.0f cm/s");
        g_changed |= row_float("Current Y", &wat->current[1], 1.0f, -5000.0f, 5000.0f, "%.0f cm/s");
        g_changed |= row_float("Density", &wat->density, 1.0f, 1.0f, 20000.0f, "%.0f kg/m3");
        g_changed |= row_float("Depth", &wat->depth, 10.0f, 0.0f, 1000000.0f, "%.0f cm");
        g_changed |= row_check("Ocean", &wat->ocean);
        g_changed |= row_check("Exclusion", &wat->exclusion); // (no ocean inside its rectangle: land, a dock)
        if (wat->ocean) { // (water.h: the FFT waves from the wind's spectrum)
            g_changed |= row_float("Wind Speed", &wat->wind_speed, 0.1f, 0.5f, 40.0f, "%.1f m/s");
            g_changed |= row_float("Wind Direction", &wat->wind_dir, 1.0f, -360.0f, 360.0f, "%.0f deg");
            g_changed |= row_float("Swell", &wat->swell, 0.01f, 0.0f, 1.0f, "%.2f");
            g_changed |= row_float("Choppiness", &wat->choppiness, 0.01f, 0.0f, 2.0f, "%.2f");
            g_changed |= row_float("Foam", &wat->foam, 0.01f, 0.0f, 1.0f, "%.2f");
        }
        label_cell("Water Color");
        g_changed |= igColorEdit3("##wcol", wat->color, ImGuiColorEditFlags_Float);
        label_cell("Scatter Color");
        g_changed |= igColorEdit3("##wsc", wat->scatter, ImGuiColorEditFlags_Float);
        g_changed |= row_float("Clarity", &wat->clarity, 10.0f, 10.0f, 100000.0f, "%.0f cm");
        g_changed |= row_float("Shore Foam", &wat->shore_foam, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Interaction", &wat->interaction, 0.01f, 0.0f, 1.0f, "%.2f");
        igEndTable();
    }
    CCloth* clo = ecs_get(w, e, COMP_CLOTH);
    if (clo && clo->kind == CLOTH_KIND_SOFT && begin_section("Soft Body", "sec_soft")) { // (Q9: softbody.h, notas-dev/soft-body.md)
        g_changed |= soft_details(clo);
        igEndTable();
    } else if (clo && clo->kind != CLOTH_KIND_SOFT && begin_section("Cloth", "sec_cloth")) { // (cloth.h, notas-dev/tecido.md)
        label_cell("Cloth Kind");
        if (igBeginCombo("##clkind", TR("Sheet"), 0)) {
            igSelectable_Bool(TR("Sheet"), true, 0, (ImVec2){ 0, 0 });
            if (igSelectable_Bool(TR("Soft Body"), false, 0, (ImVec2){ 0, 0 })) clo->kind = CLOTH_KIND_SOFT, clo->reset = 1, g_changed = true;
            igEndCombo();
        }
        label_cell("Cloth Preset");
        int pr = (int)clo->preset;
        if (igBeginCombo("##clpre", TR(g_cloth_preset_names[pr < CLOTH_PRESET_COUNT ? pr : 0]), 0)) {
            for (int k = 0; k < CLOTH_PRESET_COUNT; k++)
                if (igSelectable_Bool(TR(g_cloth_preset_names[k]), k == pr, 0, (ImVec2){ 0, 0 })) {
                    if (k) cloth_apply_preset(clo, k);
                    else clo->preset = CLOTH_CUSTOM;
                    g_changed = true;
                }
            igEndCombo();
        }
        label_cell("Pinned");
        int pn = (int)clo->pin;
        if (igBeginCombo("##clpin", TR(g_cloth_pin_names[pn < CLOTH_PIN_COUNT ? pn : 0]), 0)) {
            for (int k = 0; k < CLOTH_PIN_COUNT; k++)
                if (igSelectable_Bool(TR(g_cloth_pin_names[k]), k == pn, 0, (ImVec2){ 0, 0 })) clo->pin = (uint32_t)k, clo->reset = 1, g_changed = true;
            igEndCombo();
        }
        bool custom = false;
        label_cell("Cloth Mesh"); // (a grid, or an imported mesh: cloth_mesh.h)
        {
            uint32_t ms = clo->mesh ? render_mesh_slot(clo->mesh) : 0;
            if (igBeginCombo("##clmesh", clo->mesh ? render_mesh_name(ms) : TR("(Grid)"), 0)) {
                if (igSelectable_Bool(TR("(Grid)"), !clo->mesh, 0, (ImVec2){ 0, 0 })) clo->mesh = 0, clo->reset = 1, g_changed = true;
                for (uint32_t s = SHAPE_COUNT; s < render_mesh_count(); s++)
                    if (igSelectable_Bool(render_mesh_name(s), clo->mesh && ms == s, 0, (ImVec2){ 0, 0 })) clo->mesh = render_mesh_id(s), clo->reset = 1, g_changed = true;
                igEndCombo();
            }
        }
        if (clo->mesh) {
            g_changed |= row_float("Pin Band", &clo->pin_band, 0.1f, 0.0f, 500.0f, "%.1f cm");
            label_cell("Mesh Scale");
            g_changed |= igDragFloat3("##clmsc", clo->mesh_scale, 0.01f, 0.01f, 100.0f, "%.2f", 0);
        } else {
            g_changed |= row_float("Width", &clo->width, 1.0f, 5.0f, 5000.0f, "%.0f cm");
            g_changed |= row_float("Height", &clo->height, 1.0f, 5.0f, 5000.0f, "%.0f cm");
            int rx = (int)clo->res_x, ry = (int)clo->res_y;
            label_cell("Vertices Across");
            if (igSliderInt("##clrx", &rx, 2, 128, "%d", 0)) clo->res_x = (uint32_t)rx, g_changed = true;
            label_cell("Vertices Down");
            if (igSliderInt("##clry", &ry, 2, 128, "%d", 0)) clo->res_y = (uint32_t)ry, g_changed = true;
        }
        custom |= row_float("Density", &clo->density, 1.0f, 10.0f, 5000.0f, "%.0f g/m2");
        custom |= row_float("Stretch Stiffness", &clo->stretch, 10.0f, 10.0f, 1e6f, "%.0f N/m");
        custom |= row_float("Shear Stiffness", &clo->shear, 5.0f, 1.0f, 1e6f, "%.0f N/m");
        custom |= row_float("Bend Stiffness", &clo->bend, 0.05f, 0.0f, 1e5f, "%.2f N/m");
        custom |= row_float("Damping", &clo->damping, 0.001f, 0.0f, 1.0f, "%.3f");
        custom |= row_float("Friction", &clo->friction, 0.01f, 0.0f, 1.0f, "%.2f");
        if (custom) clo->preset = CLOTH_CUSTOM, g_changed = true;
        g_changed |= row_float("Thickness", &clo->thickness, 0.05f, 0.1f, 20.0f, "%.2f cm");
        label_cell("Wind");
        g_changed |= igDragFloat3("##clwind", clo->wind, 0.1f, -60.0f, 60.0f, "%.1f m/s", 0);
        g_changed |= row_float("Gust", &clo->gust, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Gravity Scale", &clo->gravity, 0.01f, -2.0f, 4.0f, "%.2f");
        g_changed |= row_float("Tear Strain", &clo->tear, 0.01f, 0.0f, 5.0f, clo->tear > 1.0f ? "%.2fx" : "never");
        label_cell("Color");
        g_changed |= igColorEdit3("##clcol", clo->color, ImGuiColorEditFlags_Float);
        g_changed |= row_float("Roughness", &clo->roughness, 0.01f, 0.3f, 1.0f, "%.2f");
        g_changed |= row_float("Sheen", &clo->sheen, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Translucency", &clo->translucency, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_check("Cast Shadow", &clo->cast_shadow);
        g_changed |= row_check("Self Collision", &clo->self_collision);
        if (clo->self_collision) g_changed |= row_float("Self Collision Distance", &clo->self_distance, 0.05f, 0.0f, 20.0f, clo->self_distance > 0.0f ? "%.2f cm" : "auto");
        int iters = (int)clo->iterations, subs = (int)clo->substeps;
        label_cell("Iterations");
        if (igSliderInt("##clit", &iters, 1, 24, "%d", 0)) clo->iterations = (uint32_t)iters, g_changed = true;
        label_cell("Substeps");
        if (igSliderInt("##clsub", &subs, 1, 4, "%d", 0)) clo->substeps = (uint32_t)subs, g_changed = true;
        label_cell("Follow Actor");
        igSetNextItemWidth(-1);
        g_changed |= igInputText("##clfa", clo->follow_actor, sizeof clo->follow_actor, 0, NULL, NULL);
        label_cell("Follow Bone");
        igSetNextItemWidth(-1);
        g_changed |= igInputText("##clfb", clo->follow_bone, sizeof clo->follow_bone, 0, NULL, NULL);
        g_changed |= row_check("Follow Whole Turn", &clo->follow_turn);
        label_cell("");
        if (igButton(TR("Reset Cloth"), (ImVec2){ -1, 0 })) clo->reset = 1;
        igEndTable();
    }
    CRain* rn = ecs_get(w, e, COMP_RAIN);
    if (rn && begin_section("Rain", "sec_rain")) { // (water.h, notas-dev/agua.md)
        CRain before = *rn;
        label_cell("Preset"); // (2026-11-14: the look and the sound together; changing a field after it makes it Custom)
        igSetNextItemWidth(-1);
        if (igBeginCombo("##rpreset", TR(g_rain_preset_names[rn->preset < RAIN_PRESET_COUNT ? rn->preset : 0]), 0)) {
            for (uint32_t i = 1; i < RAIN_PRESET_COUNT; i++)
                if (igSelectable_Bool(TR(g_rain_preset_names[i]), rn->preset == i, 0, (ImVec2){ 0, 0 })) rain_apply_preset(rn, i), g_changed = true;
            igEndCombo();
        }
        g_changed |= row_float("Rain Intensity", &rn->intensity, 0.5f, 0.0f, 300.0f, "%.1f mm/h");
        g_changed |= row_float("Wind Speed", &rn->wind_speed, 0.1f, 0.0f, 40.0f, "%.1f m/s");
        g_changed |= row_float("Wind Direction", &rn->wind_dir, 1.0f, -360.0f, 360.0f, "%.0f deg");
        g_changed |= row_float("Drop Size", &rn->drop_size, 0.05f, 0.3f, 8.0f, "%.2f mm");
        g_changed |= row_float("Drops", &rn->drops, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Drop Amount", &rn->drop_amount, 0.01f, 0.0f, 4.0f, "%.2fx");
        g_changed |= row_float("Fall Speed", &rn->fall_speed, 0.01f, 0.25f, 3.0f, "%.2fx");
        g_changed |= row_float("Streak Length", &rn->streak_length, 0.01f, 0.25f, 4.0f, "%.2fx");
        g_changed |= row_float("Drop Width", &rn->drop_width, 0.01f, 0.25f, 4.0f, "%.2fx");
        label_cell("Drop Color");
        g_changed |= igColorEdit3("##rdcol", rn->drop_color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        g_changed |= row_float("Gusts", &rn->gusts, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Far Rain", &rn->far_rain, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Splashes", &rn->splashes, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Splash Size", &rn->splash_size, 0.01f, 0.25f, 4.0f, "%.2fx");
        g_changed |= row_float("Wetness", &rn->wetness, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Wet Time", &rn->wet_time, 0.5f, 0.1f, 3600.0f, "%.0f s");
        g_changed |= row_float("Dry Time", &rn->dry_time, 1.0f, 0.1f, 36000.0f, "%.0f s");
        g_changed |= row_float("Actor Wet Time", &rn->actor_wet_time, 0.5f, 0.5f, 3600.0f, "%.0f s");
        g_changed |= row_float("Actor Dry Time", &rn->actor_dry_time, 1.0f, 0.5f, 36000.0f, "%.0f s");
        g_changed |= row_float("Puddles", &rn->puddles, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Water Gain", &rn->water_gain, 0.5f, 0.0f, 1000.0f, "%.0fx");
        g_changed |= row_float("Flow", &rn->flow, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Infiltration", &rn->infiltration, 0.1f, 0.0f, 500.0f, "%.1f mm/h");
        g_changed |= row_float("Evaporation", &rn->evaporation, 0.1f, 0.0f, 500.0f, "%.1f mm/h");
        g_changed |= row_float("Micro Relief", &rn->micro_relief, 0.05f, 0.0f, 20.0f, "%.2f cm");
        g_changed |= row_float("Streaks", &rn->streaks, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Ripples", &rn->ripples, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Area", &rn->area, 1.0f, 8.0f, 1000.0f, "%.0f m");
        int res = (int)rn->resolution;
        label_cell("Simulation Resolution");
        if (igSliderInt("##rres", &res, 128, 1024, "%d", 0)) rn->resolution = (uint32_t)res, g_changed = true;
        g_changed |= row_float("Start Water", &rn->start_water, 0.1f, 0.0f, 2000.0f, "%.1f mm");
        g_changed |= row_check("Regional Weather", &rn->regional); // (Q27.6)
        if (rn->regional) g_changed |= row_float("Storminess", &rn->storminess, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_check("Snow", &rn->snow);
        if (rn->snow) {
            g_changed |= row_float("Flake Size", &rn->flake_size, 0.1f, 1.0f, 40.0f, "%.1f mm");
            g_changed |= row_float("Cover Time", &rn->cover_time, 1.0f, 1.0f, 36000.0f, "%.0f s");
            g_changed |= row_float("Cover Angle", &rn->cover_angle, 0.5f, 1.0f, 89.0f, "%.0f deg");
            g_changed |= row_float("Melt Time", &rn->melt_time, 1.0f, 0.0f, 360000.0f, "%.0f s");
            g_changed |= row_float("Start Cover", &rn->start_cover, 0.01f, 0.0f, 1.0f, "%.2f");
        }
        // (2026-11-14) its sound: what it falls on, how loud, muffled under a roof, a sound of your own; thunder and lightning
        g_changed |= row_float("Sound Volume", &rn->sound_volume, 0.01f, 0.0f, 2.0f, "%.2f");
        label_cell("Sound Style");
        igSetNextItemWidth(-1);
        if (igBeginCombo("##rsstyle", TR(g_rain_sound_names[rn->sound_style < RAIN_SOUND_COUNT ? rn->sound_style : 0]), 0)) {
            for (uint32_t i = 0; i < RAIN_SOUND_COUNT; i++)
                if (igSelectable_Bool(TR(g_rain_sound_names[i]), rn->sound_style == i, 0, (ImVec2){ 0, 0 })) rn->sound_style = i, g_changed = true;
            igEndCombo();
        }
        label_cell("Custom Sound");
        igSetNextItemWidth(-1);
        g_changed |= audio_pick("##rcsnd", &rn->custom_sound, -1);
        g_changed |= row_float("Muffle Under Roof", &rn->muffle, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Thunder", &rn->thunder, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Thunder Volume", &rn->thunder_volume, 0.01f, 0.0f, 2.0f, "%.2f");
        g_changed |= row_float("Lightning", &rn->lightning, 0.01f, 0.0f, 1.0f, "%.2f");
        // (2026-11-15, "permitir o shading da chuva ser alterado, para o dev escolher o estilo e poder modificar como
        // quiser") the shading: a style's defaults, then every knob (they don't make the Preset Custom)
        label_cell("Shading Style");
        igSetNextItemWidth(-1);
        if (igBeginCombo("##rshade", TR(g_rain_shading_names[rn->shading_style < RAIN_SHADE_COUNT ? rn->shading_style : 0]), 0)) {
            for (uint32_t i = 0; i < RAIN_SHADE_COUNT; i++)
                if (igSelectable_Bool(TR(g_rain_shading_names[i]), rn->shading_style == i, 0, (ImVec2){ 0, 0 })) rain_apply_shading_style(rn, i), g_changed = true;
            igEndCombo();
        }
        g_changed |= row_float("Darkening", &rn->darkening, 0.01f, 0.0f, 2.0f, "%.2f");
        g_changed |= row_float("Water Film", &rn->film, 0.01f, 0.0f, 2.0f, "%.2f");
        g_changed |= row_float("Film Roughness", &rn->film_roughness, 0.002f, 0.005f, 0.5f, "%.3f");
        label_cell("Puddle Color");
        g_changed |= igColorEdit3("##rpcol", rn->puddle_color, ImGuiColorEditFlags_Float);
        g_changed |= row_float("Puddle Clarity", &rn->puddle_clarity, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Puddle Edge", &rn->puddle_edge, 0.01f, 0.0f, 1.0f, "%.2f");
        g_changed |= row_float("Surface Splashes", &rn->surface_splashes, 0.01f, 0.0f, 2.0f, "%.2f");
        g_changed |= row_float("Wall Drops", &rn->wall_drops, 0.01f, 0.0f, 2.0f, "%.2f");
        g_changed |= row_float("Wall Drop Size", &rn->wall_drop_size, 0.01f, 0.2f, 4.0f, "%.2f cm");
        g_changed |= row_float("Drop Light", &rn->drop_light, 0.01f, 0.0f, 4.0f, "%.2f");
        g_changed |= row_float("Drop Opacity", &rn->drop_opacity, 0.01f, 0.0f, 2.0f, "%.2f");
        g_changed |= row_float("Rain Mist", &rn->mist, 0.01f, 0.0f, 3.0f, "%.2f");
        g_changed |= row_float("Wet Marks", &rn->wet_marks, 0.01f, 0.0f, 2.0f, "%.2f");
        g_changed |= row_float("Wet Marks Dry Time", &rn->wet_marks_dry_time, 1.0f, 1.0f, 3600.0f, "%.0f s");
        g_changed |= row_float("Actor Drips", &rn->actor_drips, 0.01f, 0.0f, 2.0f, "%.2f");
        g_changed |= row_float("Lens Water", &rn->lens_water, 0.01f, 0.0f, 2.0f, "%.2f");
        g_changed |= row_float("Lens Drop Size", &rn->lens_drop_size, 0.01f, 0.3f, 3.0f, "%.2fx");
        if (rn->lens_water > 0.0f) { // (how they behave on the lens: all the dev's)
            g_changed |= row_float("Lens Gravity", &rn->lens_gravity, 0.01f, 0.0f, 3.0f, "%.2f");
            g_changed |= row_float("Lens Stickiness", &rn->lens_stickiness, 0.01f, 0.0f, 3.0f, "%.2f");
            g_changed |= row_float("Lens Inertia", &rn->lens_inertia, 0.01f, 0.0f, 3.0f, "%.2f");
            g_changed |= row_float("Lens Shake Off", &rn->lens_shake_off, 0.01f, 0.0f, 3.0f, "%.2f");
            g_changed |= row_float("Lens Airflow", &rn->lens_airflow, 0.01f, 0.0f, 3.0f, "%.2f");
            g_changed |= row_float("Lens Dry Time", &rn->lens_dry_time, 0.1f, 0.1f, 600.0f, "%.1f s");
            g_changed |= row_float("Lens Trail", &rn->lens_trail, 0.01f, 0.0f, 2.0f, "%.2f");
            g_changed |= row_float("Lens Blur", &rn->lens_blur, 0.01f, 0.0f, 2.0f, "%.2f");
            g_changed |= row_float("Lens Refraction", &rn->lens_refraction, 0.01f, 0.0f, 2.0f, "%.2f");
        }
        if (rn->preset != RAIN_PRESET_CUSTOM && before.preset == rn->preset && memcmp(&before, rn, offsetof(CRain, shading_style)) != 0) rn->preset = RAIN_PRESET_CUSTOM;
        igEndTable();
    }
    CRail* rl = ecs_get(w, e, COMP_RAIL);
    if (rl) g_changed |= spline_details(ed, w, e, rl); // (Q14: a Rail, a Spline and its meshes, a Cable: editor_spline.inc)
    CParticleTl* ptl = ecs_get(w, e, COMP_PARTICLE_TL);
    if (ptl && begin_section("Particles (Tlender)", "sec_particles_tl")) { // Blender's Particle Properties
        float count = (float)ptl->count;
        if (row_float("Number", &count, 10.0f, 1.0f, 30000000.0f, "%.0f")) ptl->count = (uint32_t)count, g_changed = true;
        g_changed |= row_float("Frame Start", &ptl->frame_start, 1.0f, -100000.0f, 100000.0f, "%.0f");
        g_changed |= row_float("End", &ptl->frame_end, 1.0f, -100000.0f, 100000.0f, "%.0f");
        g_changed |= row_float("Lifetime", &ptl->lifetime, 1.0f, 1.0f, 100000.0f, "%.0f");
        g_changed |= row_slider("Lifetime Randomness", &ptl->lifetime_random, 0.0f, 1.0f);
        g_changed |= row_float("Frame Rate", &ptl->fps, 1.0f, 1.0f, 240.0f, "%.0f fps");
        label_cell("Emit From");
        {
            static const char* const from[3] = { "Verts", "Faces", "Volume" };
            const char* tr3[3] = { TR(from[0]), TR(from[1]), TR(from[2]) };
            int v = ptl->emit_from;
            igSetNextItemWidth(-1);
            if (igCombo_Str_arr("##from", &v, tr3, 3, -1)) ptl->emit_from = (uint8_t)v, g_changed = true;
        }
        uint32_t loop = ptl->loop;
        if (row_check("Loop", &loop)) ptl->loop = (uint8_t)loop, g_changed = true;
        g_changed |= row_float("Normal", &ptl->normal_factor, 0.01f, -100.0f, 100.0f, "%.2f m/s");
        g_changed |= row_float("Randomize", &ptl->random_factor, 0.01f, 0.0f, 100.0f, "%.2f m/s");
        g_changed |= row_float("Object Velocity X", &ptl->object_velocity[0], 0.01f, -100.0f, 100.0f, "%.2f m/s");
        g_changed |= row_float("Object Velocity Y", &ptl->object_velocity[1], 0.01f, -100.0f, 100.0f, "%.2f m/s");
        g_changed |= row_float("Object Velocity Z", &ptl->object_velocity[2], 0.01f, -100.0f, 100.0f, "%.2f m/s");
        label_cell("Physics");
        {
            static const char* const ph[2] = { "None", "Newtonian" };
            const char* tr2[2] = { TR(ph[0]), TR(ph[1]) };
            int v = ptl->physics;
            igSetNextItemWidth(-1);
            if (igCombo_Str_arr("##phys", &v, tr2, 2, -1)) ptl->physics = (uint8_t)v, g_changed = true;
        }
        g_changed |= row_float("Gravity", &ptl->gravity, 0.01f, -10.0f, 10.0f, "%.2f");
        g_changed |= row_slider("Drag", &ptl->drag, 0.0f, 1.0f);
        g_changed |= row_float("Brownian", &ptl->brownian, 0.01f, 0.0f, 100.0f, "%.2f");
        g_changed |= row_float("Size", &ptl->size, 0.001f, 0.0005f, 100.0f, "%.3f m");
        g_changed |= row_slider("Scale Randomness", &ptl->size_random, 0.0f, 1.0f);
        g_changed |= row_float("Rotation Randomness", &ptl->rotation_random, 1.0f, 0.0f, 180.0f, "%.0f\xc2\xb0");
        g_changed |= row_float("Angular Velocity", &ptl->angular_velocity, 1.0f, -10000.0f, 10000.0f, "%.0f\xc2\xb0/s");
        label_cell("Render As");
        {
            static const char* const ra[2] = { "Halo", "Object" };
            const char* tr2[2] = { TR(ra[0]), TR(ra[1]) };
            int v = ptl->render_as;
            igSetNextItemWidth(-1);
            if (igCombo_Str_arr("##ra", &v, tr2, 2, -1)) ptl->render_as = (uint8_t)v, g_changed = true;
        }
        if (ptl->render_as == PTL_RENDER_OBJECT) {
            label_cell("Instance Object");
            uint32_t cur = render_mesh_slot(ptl->mesh);
            if (igBeginCombo("##ptlmesh", cur < SHAPE_COUNT ? TR(g_shape_names[cur]) : render_mesh_name(cur), 0)) {
                for (uint32_t s = 0; s < render_mesh_count(); s++)
                    if (igSelectable_Bool(s < SHAPE_COUNT ? TR(g_shape_names[s]) : render_mesh_name(s), cur == s, 0, (ImVec2){ 0, 0 }))
                        ptl->mesh = render_mesh_id(s), g_changed = true;
                igEndCombo();
            }
            uint32_t rs = ptl->receive_shadows;
            if (row_check("Receive Shadows", &rs)) ptl->receive_shadows = (uint8_t)rs, g_changed = true;
        }
        g_changed |= row_color("Color", ptl->color, false);
        g_changed |= row_float("Glow", &ptl->emissive, 10.0f, 0.0f, 1e6f, "%.0f nits");
        uint32_t add = ptl->additive, loc = ptl->local;
        if (row_check("Additive", &add)) ptl->additive = (uint8_t)add, g_changed = true;
        if (row_check("Local Space", &loc)) ptl->local = (uint8_t)loc, g_changed = true;
        float seed = (float)ptl->seed;
        if (row_float("Seed", &seed, 1.0f, 0.0f, 1e6f, "%.0f")) ptl->seed = (uint32_t)seed, g_changed = true;
        igEndTable();
    }
    CFog* fog = ecs_get(w, e, COMP_FOG);
    if (fog && begin_section("Exponential Height Fog", "sec_fog")) {
        g_changed |= row_float("Fog Density", &fog->density, 0.0001f, 0.0f, 1.0f, "%.4f /m");
        g_changed |= row_float("Height Falloff", &fog->height_falloff, 0.001f, 0.0f, 2.0f, "%.3f /m");
        g_changed |= row_color("Scattering Color", fog->albedo, false);
        g_changed |= row_slider("Anisotropy", &fog->anisotropy, -0.9f, 0.9f);
        g_changed |= row_color("Emissive", fog->emissive, true);
        g_changed |= row_float("Start Distance", &fog->start_distance, 10.0f, 0.0f, 1000000.0f, "%.0f cm");
        g_changed |= row_slider("Max Opacity", &fog->max_opacity, 0.0f, 1.0f);
        g_changed |= row_check("Volumetric Fog", &fog->volumetric);
        g_changed |= row_float("Volumetric Distance", &fog->volumetric_distance, 100.0f, 1000.0f, 50000.0f, "%.0f cm");
        g_changed |= row_slider("Sky Atmosphere Ambient", &fog->sky_atmosphere_ambient, 0.0f, 1.0f); // (Q21)
        igEndTable();
    }
    CCloud* cl = ecs_get(w, e, COMP_CLOUD);
    if (cl && begin_section("Volumetric Cloud", "sec_cloud")) {
        g_changed |= row_slider("Coverage", &cl->coverage, 0.0f, 1.0f);
        g_changed |= row_float("Density", &cl->density, 0.001f, 0.0f, 1.0f, "%.3f /m");
        g_changed |= row_float("Layer Bottom", &cl->bottom, 1000.0f, 0.0f, 2000000.0f, "%.0f cm");
        g_changed |= row_float("Layer Top", &cl->top, 1000.0f, 0.0f, 2000000.0f, "%.0f cm");
        g_changed |= row_float("Wind X", &cl->wind[0], 0.1f, -100.0f, 100.0f, "%.1f m/s");
        g_changed |= row_float("Wind Y", &cl->wind[1], 0.1f, -100.0f, 100.0f, "%.1f m/s");
        g_changed |= row_color("Albedo", cl->albedo, false);
        g_changed |= row_slider("Detail", &cl->detail, 0.0f, 1.0f);
        g_changed |= row_slider("Shadow Strength", &cl->shadow_strength, 0.0f, 1.0f);
        g_changed |= row_slider("Cirrus", &cl->cirrus, 0.0f, 1.0f); // (the high ice layer, sugestÃ£o #44)
        igEndTable();
    }
    CPostProcess* ppv = ecs_get(w, e, COMP_POST_PROCESS);
    if (ppv && begin_section("Post Process Volume", "sec_ppv")) {
        uint32_t en = (ppv->flags & PPV_ENABLED) != 0, unbound = (ppv->flags & PPV_UNBOUND) != 0;
        if (row_check("Enabled", &en)) ppv->flags = en ? ppv->flags | PPV_ENABLED : ppv->flags & ~PPV_ENABLED, g_changed = true;
        if (row_check("Infinite Extent (Unbound)", &unbound))
            ppv->flags = unbound ? ppv->flags | PPV_UNBOUND : ppv->flags & ~PPV_UNBOUND, g_changed = true;
        g_changed |= row_float("Priority", &ppv->priority, 0.1f, -100.0f, 100.0f, "%.1f");
        g_changed |= row_float("Blend Radius", &ppv->blend_radius, 5.0f, 0.0f, 100000.0f, "%.0f cm");
        g_changed |= row_slider("Blend Weight", &ppv->blend_weight, 0.0f, 1.0f);
        { // (A9.4) Unreal's Color Grading LUT: a .cube file or a PNG strip inside the project's Content, and its intensity
            label_cell("Color Grading LUT");
            igPushID_Str("ppv_lut");
            float bw = igGetFrameHeight() * 1.6f;
            igSetNextItemWidth(igGetContentRegionAvail().x - bw - 4.0f);
            g_changed |= igInputTextWithHint("##lut", "/Content/LUTs/Film.cube", ppv->lut, sizeof ppv->lut, 0, NULL, NULL);
            igSameLine(0, 4);
            if (igButton("...", (ImVec2){ bw, 0 })) {
                char path[1024] = "";
                if (platform_file_dialog_ext(ed->window, false, TR("Color grading LUT (.cube, PNG strip)"), "cube;png", path, sizeof path)) {
                    char root[800];
                    snprintf(root, sizeof root, "%s", content_root());
                    for (char* c = root; *c; c++)
                        if (*c == '\\') *c = '/';
                    for (char* c = path; *c; c++)
                        if (*c == '\\') *c = '/';
                    size_t rl = strlen(root);
                    if (!_strnicmp(path, root, rl)) snprintf(ppv->lut, sizeof ppv->lut, "/Content%s", path + rl), g_changed = true;
                    else editor_status(ed, re_time_seconds(), "%s", TR("The LUT must be inside the project's Content folder."));
                }
            }
            igPopID();
            g_changed |= row_slider("Color Grading LUT Intensity", &ppv->lut_intensity, 0.0f, 1.0f);
        }
        igEndTable();
        // the settings by Unreal's groups (each with its override checkbox)
        for (int f = 0; f < PP_FIELD_COUNT;) {
            const char* grp = g_post_fields[f].group;
            char id[48];
            snprintf(id, sizeof id, "sec_pp_%d", f);
            bool open = begin_section(grp, id);
            for (; f < PP_FIELD_COUNT && !strcmp(g_post_fields[f].group, grp); f++)
                if (open) g_changed |= row_post(ppv, f);
            if (open) igEndTable();
        }
        if (begin_section("Horror", "sec_pp_horror")) { // (Q13) the horror kit (terror.h): the exposure, the Tension Director's curves
            uint32_t he = (ppv->terror_flags & TERROR_PPV_EXPOSURE) != 0, ht = (ppv->terror_flags & TERROR_PPV_TENSION) != 0;
            if (row_check("Horror Exposure", &he)) {
                ppv->terror_flags = he ? ppv->terror_flags | TERROR_PPV_EXPOSURE : ppv->terror_flags & ~TERROR_PPV_EXPOSURE, g_changed = true;
                if (he && ppv->terror_meter <= 0.0f) ppv->terror_meter = 0.95f, ppv->terror_toe = 0.5f, ppv->terror_ev_floor = 1.0f;
            }
            if (he) {
                g_changed |= row_slider("Toe", &ppv->terror_toe, 0.0f, 1.0f);
                g_changed |= row_float("EV Floor", &ppv->terror_ev_floor, 0.05f, -10.0f, 20.0f, "%.1f EV100");
                g_changed |= row_slider("Metered Share", &ppv->terror_meter, 0.5f, 0.995f);
            }
            if (row_check("Tension Curves", &ht)) {
                ppv->terror_flags = ht ? ppv->terror_flags | TERROR_PPV_TENSION : ppv->terror_flags & ~TERROR_PPV_TENSION, g_changed = true;
                bool blank = true;
                for (int c = 0; c < TENSION_CHANNELS; c++)
                    for (int k = 0; k < 4; k++) blank &= ppv->tension_curve[c][k] == 0.0f;
                if (ht && blank) // (the defaults to start from)
                    for (int c = 0; c < TENSION_CHANNELS; c++) memcpy(ppv->tension_curve[c], g_tension_channels[c].def, sizeof ppv->tension_curve[c]);
            }
            if (ht) {
                for (int c = 0; c < TENSION_CHANNELS; c++) g_changed |= row_tension_curve(c, ppv->tension_curve[c]);
                float rise = ppv->tension_rise > 0.0f ? ppv->tension_rise : 0.5f, fall = ppv->tension_fall > 0.0f ? ppv->tension_fall : 0.25f;
                if (row_float("Tension Rise", &rise, 0.01f, 0.01f, 20.0f, "%.2f /s")) ppv->tension_rise = rise, g_changed = true;
                if (row_float("Tension Fall", &fall, 0.01f, 0.01f, 20.0f, "%.2f /s")) ppv->tension_fall = fall, g_changed = true;
                label_cell("Tension Sound Class");
                g_changed |= igInputTextWithHint("##tsc", "SC_Ambience", ppv->tension_sound_class, sizeof ppv->tension_sound_class, 0, NULL, NULL);
                label_cell("Tension Parameter");
                g_changed |= igInputTextWithHint("##tpar", "Tension", ppv->tension_param, sizeof ppv->tension_param, 0, NULL, NULL);
            }
            igEndTable();
        }
    }
    CCamera* cam = ecs_get(w, e, COMP_CAMERA);
    if (cam && begin_section("Camera", "sec_camera")) {
        g_changed |= row_float("Field of View", &cam->fov_deg, 0.25f, 5.0f, 170.0f, "%.1f\xc2\xb0");
        g_changed |= row_check("Auto Activate for Player", &cam->auto_activate);
        // the camera's own motion blurring the view (the moving actors blur either way: the Post Process Volume's amount)
        uint32_t cb = !cam->no_camera_blur;
        if (row_check("Camera Motion Blur", &cb)) cam->no_camera_blur = !cb, g_changed = true;
        g_changed |= cine_rows(cam);
        igEndTable();
        g_changed |= editor_camera_post_ui(cam);
        g_changed |= fpcam_ui(cam); // (Q12, fpcam_ui.c)
    }
    CBlueprint* bpc = ecs_get(w, e, COMP_BLUEPRINT);
    if (bpc && begin_section("Blueprint", "sec_bp")) {
        BpAsset* cls = bp_find_hash(bpc->asset_hash);
        igTableNextRow(0, 0);
        igTableNextColumn();
        igAlignTextToFramePadding();
        igTextUnformatted(TR("Class"), NULL);
        igTableNextColumn();
        if (cls) {
            igText("%s (%s)", cls->name, g_bp_native_names[bp_native_of(cls)]);
            igSameLine(0, 8);
            if (igSmallButton(TR("Edit Blueprint"))) {
                char vpath[700];
                const char* root = content_root();
                size_t rl = strlen(root);
                snprintf(vpath, sizeof vpath, "/Content%s", !strncmp(cls->path, root, rl) ? cls->path + rl : cls->path);
                bp_editor_open(vpath);
            }
        } else
            igTextColored(THV(TK_ERROR), "%s", TR("Missing Blueprint Class"));
        { // (Unreal's Actor Tick > Tick Even When Paused: this actor goes on while the game is paused)
            uint32_t twp = (bpc->flags & BP_FLAG_TICK_WHEN_PAUSED) != 0;
            if (row_check("Tick When Paused", &twp))
                bpc->flags = twp ? bpc->flags | BP_FLAG_TICK_WHEN_PAUSED : bpc->flags & ~BP_FLAG_TICK_WHEN_PAUSED, g_changed = true;
        }
        // the class's Instance Editable variables (its own and its Blueprint parents'), this actor's value or the default
        // (Unreal's Details "Default" category: a value set here shows the yellow arrow back to the class's)
        const BpAsset* chain[8];
        uint32_t nchain = 0;
        for (const BpAsset* c = cls; c && nchain < 8; c = c->parent[0] ? bp_find_name(BPA_CLASS, c->parent) : NULL) chain[nchain++] = c;
        for (uint32_t ci = 0; ci < nchain; ci++) {
            for (uint32_t vi = 0; vi < chain[ci]->nvars; vi++) {
                const BpVar* bv = &chain[ci]->vars[vi];
                if (!bv->editable || !bp_var_overridable(bv)) continue;
                CBpOverride* o = bp_override_find(bpc, bv->name);
                BpValue val = bv->def;
                char text[64];
                snprintf(text, sizeof text, "%s", o ? o->text : bv->sdef);
                if (o) memcpy(val.v, o->v, sizeof val.v);
                igTableNextRow(0, 0);
                igTableNextColumn();
                igAlignTextToFramePadding();
                igTextUnformatted(bv->name, NULL);
                if (bv->tooltip[0]) igSetItemTooltip("%s", bv->tooltip);
                igTableNextColumn();
                igPushID_Str(bv->name);
                float rb = o ? igGetFrameHeight() + 4 : 0;
                igSetNextItemWidth(igGetContentRegionAvail().x - rb);
                bool text_type = bp_type_stringish(bv->type);
                if (bp_value_edit("##ov", bv->type, bv->ref, &val, text_type ? text : NULL, sizeof text)) {
                    bp_override_set(bpc, bv->name, val.v, text_type ? text : NULL);
                    g_changed = true;
                }
                if (o) {
                    igSameLine(0, 4);
                    if (igButton("\xe2\x86\xba##reset", (ImVec2){ igGetFrameHeight(), 0 })) bp_override_clear(bpc, bv->name), g_changed = true;
                    igSetItemTooltip("%s", TR("Reset to default"));
                }
                igPopID();
            }
        }
        igEndTable();
    }
    g_changed |= wp_details(ed, w, e); // (B1: the World Partition actor's)
    CGameplayTags* gtc = ecs_get(w, e, COMP_GAMEPLAY_TAGS);
    if (gtc && begin_section("Gameplay Tags", "sec_tags")) { // Unreal's tag container: one row a tag, "+" adds, "x" removes
        for (uint32_t i = 0; i < gtc->count && i < GT_MAX_TAGS; i++) {
            igPushID_Int((int)i);
            igTableNextRow(0, 0);
            igTableNextColumn();
            igAlignTextToFramePadding();
            igTextDisabled("[%u]", i);
            igTableNextColumn();
            igSetNextItemWidth(igGetContentRegionAvail().x - igGetFrameHeight() - 4);
            char buf[GT_TAG_LEN];
            snprintf(buf, sizeof buf, "%s", gtc->tags[i]);
            if (gt_tag_picker("##tag", buf, sizeof buf, igGetContentRegionAvail().x - igGetFrameHeight() - 4)) { // (the project's list, with a search)
                char clean[GT_TAG_LEN];
                if (gt_clean(buf, clean, sizeof clean) && strcmp(clean, gtc->tags[i])) snprintf(gtc->tags[i], GT_TAG_LEN, "%s", clean), g_changed = true;
            }
            igSameLine(0, 4);
            bool del = igButton("x##del", (ImVec2){ igGetFrameHeight(), 0 });
            igSetItemTooltip("%s", TR("Remove"));
            igPopID();
            if (del) {
                gt_remove(w, e, gtc->tags[i]);
                g_changed = true;
                break;
            }
        }
        igTableNextRow(0, 0);
        igTableNextColumn();
        igTableNextColumn();
        static char add_buf[GT_TAG_LEN];
        bool enter = gt_tag_picker("##newtag", add_buf, sizeof add_buf, igGetContentRegionAvail().x - igGetFrameHeight() - 4); // (chosen: added)
        igSameLine(0, 4);
        if ((igButton("+##addtag", (ImVec2){ igGetFrameHeight(), 0 }) || enter) && gt_add(w, e, add_buf)) add_buf[0] = 0, g_changed = true;
        igSetItemTooltip("%s", TR("Add a Gameplay Tag (the dots make a hierarchy: Damage.Fire is under Damage)"));
        igEndTable();
    }
    CInventory* invc = ecs_get(w, e, COMP_INVENTORY); // (inventory_ui.c)
    if (invc && igCollapsingHeader_TreeNodeFlags(TR("Inventory"), ImGuiTreeNodeFlags_DefaultOpen)) g_changed |= inv_ui_inventory(invc);
    CGameRules* grc = ecs_get(w, e, COMP_GAME_RULES); // (rules_ui.c)
    if (grc && igCollapsingHeader_TreeNodeFlags(TR("Game Rules"), ImGuiTreeNodeFlags_DefaultOpen)) g_changed |= rules_ui_component(grc);
    CHudLayout* hlc = ecs_get(w, e, COMP_HUD_LAYOUT); // (hud_widget.c)
    if (hlc && igCollapsingHeader_TreeNodeFlags(TR("HUD Layout"), ImGuiTreeNodeFlags_DefaultOpen)) g_changed |= hud_ui_layout(hlc);
    CDialogueSpeaker* dsc = ecs_get(w, e, COMP_DIALOGUE_SPEAKER); // (dialogue_editor.c)
    if (dsc && igCollapsingHeader_TreeNodeFlags(TR("Dialogue Speaker"), ImGuiTreeNodeFlags_DefaultOpen)) g_changed |= dlg_ui_speaker(dsc);
    CQuestLog* qlc = ecs_get(w, e, COMP_QUEST_LOG); // (quest_editor.c)
    if (qlc && igCollapsingHeader_TreeNodeFlags(TR("Quest Log"), ImGuiTreeNodeFlags_DefaultOpen)) g_changed |= quest_ui_log(qlc);
    CAbilitySystem* asc = ecs_get(w, e, COMP_ABILITY_SYSTEM); // (ability_ui.c)
    if (asc && igCollapsingHeader_TreeNodeFlags(TR("Ability System"), ImGuiTreeNodeFlags_DefaultOpen)) g_changed |= abil_ui_system(asc);
    CPickup* pkc = ecs_get(w, e, COMP_PICKUP);
    if (pkc && igCollapsingHeader_TreeNodeFlags(TR("Pickup"), ImGuiTreeNodeFlags_DefaultOpen)) {
        CTransform* pt = ecs_get(w, e, COMP_TRANSFORM);
        if (inv_ui_pickup(pkc, ecs_get(w, e, COMP_STATIC_MESH), pt ? pt->scale : NULL)) g_changed = true, ecs_touch(w, e);
    }
    CInteractable* iac = ecs_get(w, e, COMP_INTERACTABLE); // (interaction_ui.c)
    if (iac && igCollapsingHeader_TreeNodeFlags(TR("Interactable"), ImGuiTreeNodeFlags_DefaultOpen)) g_changed |= ia_ui_interactable(iac);
    CDevice* dvc = ecs_get(w, e, COMP_DEVICE);
    if (dvc && igCollapsingHeader_TreeNodeFlags(TR(g_device_names[dvc->kind % DEV_KIND_COUNT]), ImGuiTreeNodeFlags_DefaultOpen)) g_changed |= ia_ui_device(w, e, dvc);
    CInteractor* irc = ecs_get(w, e, COMP_INTERACTOR);
    if (irc && igCollapsingHeader_TreeNodeFlags(TR("Interactor"), ImGuiTreeNodeFlags_DefaultOpen)) g_changed |= ia_ui_interactor(irc);
    CCharacter* chr = ecs_get(w, e, COMP_CHARACTER);
    if (chr && begin_section("Character Movement", "sec_char")) {
        g_changed |= row_float("Max Walk Speed", &chr->max_walk_speed, 5.0f, 0.0f, 5000.0f, "%.0f cm/s");
        g_changed |= row_float("Jump Z Velocity", &chr->jump_z_velocity, 5.0f, 0.0f, 5000.0f, "%.0f cm/s");
        g_changed |= row_float("Gravity Scale", &chr->gravity_scale, 0.01f, 0.0f, 10.0f, "%.2f");
        g_changed |= row_float("Capsule Radius", &chr->radius, 0.5f, 5.0f, 500.0f, "%.0f cm");
        g_changed |= row_float("Capsule Half Height", &chr->half_height, 0.5f, 5.0f, 500.0f, "%.0f cm");
        g_changed |= row_float("Camera Distance", &chr->camera_distance, 5.0f, 0.0f, 3000.0f, "%.0f cm");
        g_changed |= row_check("Auto Possess Player", &chr->auto_possess);
        g_changed |= row_check("Use Controller Rotation Yaw", &chr->use_controller_yaw); // (Q12)
        igEndTable();
    }
    CCollision* col = ecs_get(w, e, COMP_COLLISION);
    if (col) { // a Trigger's shape (Unreal's Shape section)
        char lab[96];
        snprintf(lab, sizeof lab, "%s###sec_shape", TR("Shape"));
        if (igCollapsingHeader_TreeNodeFlags(lab, ImGuiTreeNodeFlags_DefaultOpen) &&
            igBeginTable("sec_shape_t", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, (ImVec2){ 0, 0 }, 0)) {
            igTableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 7.5f, 0);
            igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
            if (col->shape == COLLISION_BOX) g_changed |= vec3_row("Box Extent", col->extent, 1.0f, "%.0f cm", 50.0f);
            else g_changed |= row_float(col->shape == COLLISION_SPHERE ? "Sphere Radius" : "Capsule Radius", &col->extent[0], 1.0f, 1.0f, 1e5f, "%.0f cm");
            if (col->shape == COLLISION_CAPSULE) g_changed |= row_float("Capsule Half Height", &col->extent[1], 1.0f, 1.0f, 1e5f, "%.0f cm");
            g_changed |= row_check("Generate Overlap Events", &col->generate_overlap_events);
            g_changed |= row_check("Block All", &col->block);
            igEndTable();
        }
    }
    CRotating* r = ecs_get(w, e, COMP_ROTATING);
    if (r) {
        bool keep = true;
        char lab[96];
        snprintf(lab, sizeof lab, "%s###sec_rotating", TR("Rotating Movement"));
        if (igCollapsingHeader_BoolPtr(lab, &keep, ImGuiTreeNodeFlags_DefaultOpen) &&
            igBeginTable("sec_rot", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, (ImVec2){ 0, 0 }, 0)) {
            igTableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 7.5f, 0);
            igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
            g_changed |= vec3_row("Rotation Rate", r->rate, 1.0f, "%.1f\xc2\xb0/s", 0.0f);
            igEndTable();
        }
        if (!keep) {
            editor_undo_push(ed);
            ecs_remove(w, e, COMP_ROTATING);
            return;
        }
    }
    CProto* prt = ecs_get(w, e, COMP_PROTO);
    if (prt) {
        bool keep = true;
        char lab[96];
        snprintf(lab, sizeof lab, "%s###sec_proto", TR("Prototype"));
        if (igCollapsingHeader_BoolPtr(lab, &keep, ImGuiTreeNodeFlags_DefaultOpen) &&
            igBeginTable("sec_proto_t", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, (ImVec2){ 0, 0 }, 0)) {
            igTableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 7.5f, 0);
            igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
            g_changed |= proto_details(prt);
            if (proto_spline_kind(prt->kind)) { // (2026-11-10) the shape along a spline, as a river
                label_cell("Spline");
                if (prt->npoints < 2) {
                    if (igButton(TR("Make Spline"), (ImVec2){ -1, 0 })) {
                        editor_undo_push(ed);
                        float L = fmaxf(prt->size[0], 100.0f);
                        prt->npoints = 3;
                        for (int k = 0; k < 3; k++) prt->points[k][0] = -L * 0.5f + L * 0.5f * (float)k, prt->points[k][1] = 0.0f, prt->points[k][2] = 0.0f;
                        prt->points[1][1] = L * 0.25f; // (a gentle curve to start from)
                        g_changed = true;
                    }
                    igSetItemTooltip("%s", TR("The shape follows a curve through points you drag in the viewport, as a river."));
                } else {
                    igTextDisabled(TR("%u points"), prt->npoints);
                    igSameLine(0, 8);
                    if (igSmallButton(TR("Straight Again"))) editor_undo_push(ed), prt->npoints = 0, g_changed = true;
                    igSetItemTooltip("%s", TR("Back to the straight shape (its Length)."));
                    igTextDisabled("%s", TR("Drag the points in the viewport (Shift: up and down; Alt: a copy of the point); right click a point: add or delete."));
                }
            }
            igEndTable();
        }
        if (!keep) {
            editor_undo_push(ed);
            ecs_remove(w, e, COMP_PROTO);
            return;
        }
    }
    CLandscape* lsc = ecs_get(w, e, COMP_LANDSCAPE);
    if (lsc) {
        bool keep = true;
        char lab[96];
        snprintf(lab, sizeof lab, "%s###sec_land", TR("Landscape"));
        if (igCollapsingHeader_BoolPtr(lab, &keep, ImGuiTreeNodeFlags_DefaultOpen) &&
            igBeginTable("sec_land_t", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, (ImVec2){ 0, 0 }, 0)) {
            igTableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 7.5f, 0);
            igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
            g_changed |= landscape_details(ed, e, lsc);
            igEndTable();
            const LandStats* ls = landscape_stats();
            igTextDisabled(TR("%u chunks, %u triangles; last build %.1f ms"), ls->chunks, ls->triangles, ls->build_ms);
            if (ed->land_tool) igTextDisabled("%s", TR("Left button: sculpt (Shift: the opposite). The chunks are written again as you go."));
        }
        if (!keep) {
            editor_undo_push(ed);
            ecs_remove(w, e, COMP_LANDSCAPE);
            return;
        }
    }
    CFoliage* fol = ecs_get(w, e, COMP_FOLIAGE);
    if (fol) {
        bool keep = true;
        char lab[96];
        snprintf(lab, sizeof lab, "%s###sec_fol", TR("Foliage"));
        if (igCollapsingHeader_BoolPtr(lab, &keep, ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ed->foliage_mode) igTextDisabled(TR("%u instances: the types and the brush are in the Foliage panel (left)."), foliage_count(e, -1));
            else g_changed |= foliage_details(ed, e, fol);
        }
        if (!keep) {
            editor_undo_push(ed);
            ecs_remove(w, e, COMP_FOLIAGE);
            return;
        }
    }
    CCrowd* crd = ecs_get(w, e, COMP_CROWD);
    if (crd) {
        bool keep = true;
        char lab[96];
        snprintf(lab, sizeof lab, "%s###sec_crowd", TR("Crowd"));
        if (igCollapsingHeader_BoolPtr(lab, &keep, ImGuiTreeNodeFlags_DefaultOpen) &&
            igBeginTable("sec_crowd_t", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, (ImVec2){ 0, 0 }, 0)) {
            igTableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 7.5f, 0);
            igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
            g_changed |= crowd_details(crd);
            igEndTable();
            const CrowdStats* cs = crowd_stats();
            if (cs->agents) igTextDisabled(TR("%u agents in %u crowds, simulation %.2f ms"), cs->agents, cs->crowds, cs->sim_ms);
        }
        if (!keep) {
            editor_undo_push(ed);
            ecs_remove(w, e, COMP_CROWD);
            return;
        }
    }
    CAIAgent* aia = ecs_get(w, e, COMP_AI_AGENT);
    if (aia) { // Unreal's AI Controller and AI Perception on the pawn
        bool keep = true;
        char lab[96];
        snprintf(lab, sizeof lab, "%s###sec_ai", TR("AI"));
        if (igCollapsingHeader_BoolPtr(lab, &keep, ImGuiTreeNodeFlags_DefaultOpen)) {
            igPushItemWidth(-igGetFontSize() * 9.0f);
            if (ai_component_details(aia)) g_changed = true;
            igPopItemWidth();
        }
        if (!keep) {
            editor_undo_push(ed);
            ecs_remove(w, e, COMP_AI_AGENT);
            return;
        }
    }
    CSkeletalMesh* skm = ecs_get(w, e, COMP_SKELETAL_MESH);
    if (skm) { // Unreal's Mesh and Animation sections (anim.c)
        bool keep = true;
        char lab[96];
        snprintf(lab, sizeof lab, "%s###sec_skm", TR("Skeletal Mesh"));
        if (igCollapsingHeader_BoolPtr(lab, &keep, ImGuiTreeNodeFlags_DefaultOpen)) {
            const SkelMesh* skel = anim_mesh(skm->mesh);
            if (anim_mesh_old_parts(skel)) { // (2026-11-10) imported before the smooth skins: a dress in strips, seams
                igPushStyleColor_U32(ImGuiCol_Text, TH(TK_WARN));
                igTextWrapped("%s", TR("Imported before the smooth skinning: cut in rigid parts by bone (cloth in strips, seams at the joints). Reimport it to bend like in the Tlender."));
                igPopStyleColor(1);
                char vp[600];
                if (ui_icon_button("##reimport", ICON_IMPORT, TR("Reimport"), TH(TK_N14), false, TR("Reads the source file again into the same folder.")) && anim_mesh_vpath(skel, vp, sizeof vp))
                    content_reimport(vp);
            }
            igPushItemWidth(-igGetFontSize() * 7.5f);
            if (anim_component_details(skm)) g_changed = true;
            igPopItemWidth();
        }
        if (!keep) {
            editor_undo_push(ed);
            ecs_remove(w, e, COMP_SKELETAL_MESH);
            return;
        }
    }
    CAudio* au = ecs_get(w, e, COMP_AUDIO);
    if (au) { // Unreal's Sound section (audio_world.c)
        bool keep = true;
        char lab[96];
        snprintf(lab, sizeof lab, "%s###sec_audio", TR("Sound"));
        if (igCollapsingHeader_BoolPtr(lab, &keep, ImGuiTreeNodeFlags_DefaultOpen)) {
            igPushItemWidth(-igGetFontSize() * 7.5f);
            if (audio_component_details(au)) g_changed = true;
            igPopItemWidth();
        }
        if (!keep) {
            editor_undo_push(ed);
            ecs_remove(w, e, COMP_AUDIO);
            return;
        }
    }
    CDecal* dcl = ecs_get(w, e, COMP_DECAL);
    if (dcl && begin_section("Decal", "sec_decal")) { // Unreal's Decal Component: Decal Material, Sort Order, Fade
        label_cell("Decal Material");
        if (field_material("##dmat", &dcl->material, TR("None"), false)) g_changed = true; // (Q6: the asset picker; a drop too)
        g_changed |= row_slider("Opacity", &dcl->opacity, 0.0f, 1.0f);
        g_changed |= row_float("Fade by Angle", &dcl->angle_fade, 1.0f, 1.0f, 89.0f, "%.0f deg");
        g_changed |= row_float("Fade Distance", &dcl->fade_distance, 10.0f, 0.0f, 1000000.0f, "%.0f cm");
        g_changed |= row_float("Life Span", &dcl->life_span, 0.1f, 0.0f, 3600.0f, "%.1f s");
        if (dcl->life_span > 0.0f) g_changed |= row_float("Fade Out", &dcl->fade_out, 0.05f, 0.0f, 60.0f, "%.2f s");
        label_cell("Sort Order");
        if (igDragInt("##dsort", &dcl->sort_order, 0.1f, -100, 100, "%d", 0)) g_changed = true;
        igEndTable();
    }
    CVolume* vol = ecs_get(w, e, COMP_VOLUME);
    if (vol && begin_section("Heterogeneous Volume", "sec_volume")) { // (A9.5a) the file or sequence, its axes, its look
        label_cell("File");
        igSetNextItemWidth(-1);
        if (igInputTextWithHint("##vpath", "Volumes/smoke_0001.vdb", vol->path, sizeof vol->path, 0, NULL, NULL)) g_changed = true;
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("An OpenVDB file (EmberGen, Houdini, Blender, the Tlender); a sequence: its first file and Frames."));
        int fr = (int)vol->frames, res = (int)vol->resolution;
        label_cell("Frames");
        if (igDragInt("##vfr", &fr, 0.2f, 1, 240, "%d", 0)) vol->frames = (uint32_t)fr, g_changed = true;
        g_changed |= row_float("Frame Rate", &vol->fps, 0.1f, 1.0f, 120.0f, "%.1f fps");
        label_cell("Loop");
        bool lp = vol->loop != 0;
        if (igCheckbox("##vloop", &lp)) vol->loop = lp ? 1u : 0u, g_changed = true;
        label_cell("Resolution");
        if (igDragInt("##vres", &res, 0.2f, 16, 128, "%d", 0)) vol->resolution = (uint32_t)res, g_changed = true;
        label_cell("Up Axis");
        int ax = vol->up_axis ? 1 : 0;
        const char* an[2] = { "Z", "Y" };
        if (igCombo_Str_arr("##vup", &ax, an, 2, -1)) vol->up_axis = (uint32_t)ax, g_changed = true;
        g_changed |= row_float("Unit", &vol->unit, 1.0f, 0.01f, 100000.0f, "%.1f cm");
        g_changed |= row_float("Density", &vol->density, 0.05f, 0.0f, 500.0f, "%.2f /m");
        g_changed |= row_float("Flame Temperature", &vol->flame_temperature, 10.0f, 300.0f, 4000.0f, "%.0f K");
        g_changed |= row_float("Temperature Scale", &vol->temperature_scale, 0.01f, 0.0f, 100.0f, "%.2f");
        g_changed |= row_float("Brightness", &vol->brightness, 50.0f, 0.0f, 500000.0f, "%.0f nits");
        g_changed |= row_slider("Smoke Albedo", &vol->smoke_albedo, 0.0f, 1.0f);
        igEndTable();
    }
    CSplat* spl = ecs_get(w, e, COMP_SPLAT);
    if (spl && begin_section("Gaussian Splat", "sec_splat")) { // (A9.5d) the file, its axes and unit, its light
        label_cell("File");
        igSetNextItemWidth(-1);
        if (igInputTextWithHint("##spath", "Splats/scene.ply", spl->path, sizeof spl->path, 0, NULL, NULL)) g_changed = true;
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("A 3DGS .ply (as trained from a phone's capture) or a .splat, under Content or a full path."));
        label_cell("Up Axis");
        int ax = (int)(spl->up_axis > 2 ? 0 : spl->up_axis);
        const char* an[3] = { "Z", "-Y (3DGS)", "Y" };
        if (igCombo_Str_arr("##sup", &ax, an, 3, -1)) spl->up_axis = (uint32_t)ax, g_changed = true;
        g_changed |= row_float("Unit", &spl->unit, 1.0f, 0.01f, 100000.0f, "%.1f cm");
        label_cell("Light");
        int md = spl->mode ? 1 : 0;
        const char* mn[2] = { TR("Lit by the Scene"), TR("Own Light") };
        if (igCombo_Str_arr("##smode", &md, mn, 2, -1)) spl->mode = (uint32_t)md, g_changed = true;
        if (spl->mode) g_changed |= row_float("Brightness", &spl->brightness, 10.0f, 0.0f, 1000000.0f, "%.0f nits");
        g_changed |= row_float("Opacity", &spl->opacity, 0.01f, 0.0f, 4.0f, "%.2f");
        igEndTable();
        const SplatStats* ss = splat_stats();
        igTextDisabled("%u %s", ss->gaussians, TR("gaussians drawn"));
    }
    CFogVolume* fvol = ecs_get(w, e, COMP_FOG_VOLUME);
    if (fvol && begin_section("Local Fog Volume", "sec_fogvol")) { // (A9.5b) Unreal's: its shape, extinction, falloffs, color
        label_cell("Shape");
        int sh = fvol->shape ? 1 : 0;
        const char* sn[2] = { TR("Sphere"), TR("Box") };
        if (igCombo_Str_arr("##fvshape", &sh, sn, 2, -1)) fvol->shape = (uint32_t)sh, g_changed = true;
        g_changed |= row_float("Density", &fvol->density, 0.005f, 0.0f, 20.0f, "%.3f /m");
        g_changed |= row_slider("Edge Falloff", &fvol->edge_falloff, 0.01f, 1.0f);
        g_changed |= row_float("Height Falloff", &fvol->height_falloff, 0.01f, 0.0f, 20.0f, "%.2f");
        label_cell("Albedo");
        if (igColorEdit3("##fvalb", fvol->albedo, ImGuiColorEditFlags_Float)) g_changed = true;
        label_cell("Emissive");
        if (igColorEdit3("##fvem", fvol->emissive, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)) g_changed = true;
        igEndTable();
        igTextDisabled("%s", TR("Lit with the volumetric fog (Medium and up): the sun's shafts and the lamps' halos go through it."));
    }
    CRiver* riv = ecs_get(w, e, COMP_RIVER);
    if (riv && begin_section("River", "sec_river")) { // (B12c) Unreal's Water Body River: its spline's points
        igEndTable();
        igTextDisabled("%s", TR("Each point: place (cm, the actor's), width, depth, speed of the water along the river."));
        uint32_t np = riv->npoints > RIVER_MAX_POINTS ? RIVER_MAX_POINTS : riv->npoints;
        int del = -1;
        for (uint32_t i = 0; i < np; i++) {
            igPushID_Int((int)i);
            igSeparatorText(TR("Point"));
            igSetNextItemWidth(-1);
            if (igDragFloat3("##pp", riv->points[i], 5.0f, -1e6f, 1e6f, "%.0f", 0)) g_changed = true;
            igSetNextItemWidth(igGetContentRegionAvail().x * 0.3f);
            if (igDragFloat("##pw", &riv->width[i], 2.0f, 10.0f, 100000.0f, "W %.0f", 0)) g_changed = true;
            igSameLine(0, 4), igSetNextItemWidth(igGetContentRegionAvail().x * 0.45f);
            if (igDragFloat("##pd", &riv->depth[i], 1.0f, 1.0f, 10000.0f, "D %.0f", 0)) g_changed = true;
            igSameLine(0, 4), igSetNextItemWidth(igGetContentRegionAvail().x * 0.75f);
            if (igDragFloat("##ps", &riv->speed[i], 1.0f, 0.0f, 5000.0f, "V %.0f", 0)) g_changed = true;
            igSameLine(0, 4);
            if (np > 2 && igSmallButton("x")) del = (int)i;
            igPopID();
        }
        if (del >= 0) {
            for (uint32_t i = (uint32_t)del; i + 1 < np; i++) {
                memcpy(riv->points[i], riv->points[i + 1], 12);
                riv->width[i] = riv->width[i + 1], riv->depth[i] = riv->depth[i + 1], riv->speed[i] = riv->speed[i + 1];
            }
            riv->npoints = --np, g_changed = true;
        }
        if (np < RIVER_MAX_POINTS && igButton(TR("Add Point"), (ImVec2){ 0, 0 })) { // (past the last, the way it went)
            uint32_t a = np - 1, b = np >= 2 ? np - 2 : np - 1;
            for (int k = 0; k < 3; k++) riv->points[np][k] = riv->points[a][k] + (riv->points[a][k] - riv->points[b][k]);
            riv->width[np] = riv->width[a], riv->depth[np] = riv->depth[a], riv->speed[np] = riv->speed[a];
            riv->npoints = np + 1, g_changed = true;
        }
    }
    CBuoyancy* buo = ecs_get(w, e, COMP_BUOYANCY);
    if (buo && begin_section("Buoyancy", "sec_buoyancy")) { // (B12f) Unreal's Buoyancy Component
        g_changed |= row_float("Buoyancy", &buo->buoyancy, 0.01f, 0.0f, 10.0f, "%.2f");
        g_changed |= row_float("Water Drag", &buo->water_drag, 0.01f, 0.0f, 20.0f, "%.2f /s");
        g_changed |= row_float("Angular Drag", &buo->angular_drag, 0.01f, 0.0f, 20.0f, "%.2f /s");
        label_cell("Pontoons");
        int pt5 = buo->pontoons >= 5 ? 1 : 0;
        static const char* const pn[2] = { "1", "5" };
        if (igCombo_Str_arr("##bpont", &pt5, pn, 2, -1)) buo->pontoons = pt5 ? 5u : 1u, g_changed = true;
        igEndTable();
        if (!ecs_get(w, e, COMP_RIGID_BODY)) igTextColored(THV(TK_WARN), "%s", TR("Needs Simulate Physics (add a Rigid Body)."));
    }
    CFire* fir = ecs_get(w, e, COMP_FIRE);
    if (fir && begin_section("Fire", "sec_fire")) { // (A14) the gas's grid, its burning, its look, its light
        label_cell("Preset");
        int pr = (int)fir->preset;
        const char* pn[5];
        for (int i = 0; i < 5; i++) pn[i] = TR(g_fire_preset_names[i]);
        if (igCombo_Str_arr("##fpreset", &pr, pn, 5, -1)) fire_apply_preset(fir, pr), g_changed = true;
        label_cell("Enabled");
        bool on = fir->enabled != 0;
        if (igCheckbox("##fon", &on)) fir->enabled = on ? 1u : 0u, g_changed = true;
        g_changed |= row_float("Width", &fir->width, 1.0f, 2.0f, 2000.0f, "%.0f cm");
        g_changed |= row_float("Height", &fir->height, 1.0f, 2.0f, 4000.0f, "%.0f cm");
        int res = (int)fir->resolution, it = (int)fir->iterations;
        label_cell("Resolution");
        if (igDragInt("##fres", &res, 0.2f, 12, 96, "%d", 0)) fir->resolution = (uint32_t)res, g_changed = true;
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("Cells across the box: finer flames cost more (the cost grows with the cube)."));
        label_cell("Pressure Iterations");
        if (igDragInt("##fit", &it, 0.2f, 4, 80, "%d", 0)) fir->iterations = (uint32_t)it, g_changed = true;
        g_changed |= row_float("Emitter Radius", &fir->emitter_radius, 0.5f, 0.2f, 1000.0f, "%.1f cm");
        g_changed |= row_float("Fuel", &fir->fuel, 0.01f, 0.0f, 4.0f, "%.2f");
        g_changed |= row_float("Burn Rate", &fir->burn, 0.05f, 0.0f, 20.0f, "%.2f /s");
        g_changed |= row_float("Heat", &fir->heat, 10.0f, 0.0f, 4000.0f, "%.0f K");
        g_changed |= row_float("Buoyancy", &fir->buoyancy, 0.05f, 0.0f, 40.0f, "%.2f");
        g_changed |= row_float("Vorticity", &fir->vorticity, 0.05f, 0.0f, 20.0f, "%.2f");
        g_changed |= row_float("Cooling", &fir->cooling, 0.05f, 0.0f, 20.0f, "%.2f /s");
        g_changed |= row_float("Smoke", &fir->smoke, 0.01f, 0.0f, 4.0f, "%.2f");
        g_changed |= row_float("Smoke Fade", &fir->dissipation, 0.01f, 0.0f, 10.0f, "%.2f /s");
        g_changed |= row_float("Turbulence", &fir->turbulence, 0.5f, 0.0f, 500.0f, "%.0f cm/s");
        g_changed |= row_float("Brightness", &fir->brightness, 100.0f, 0.0f, 500000.0f, "%.0f nits");
        g_changed |= row_float("Smoke Density", &fir->smoke_density, 0.002f, 0.0f, 2.0f, "%.3f");
        g_changed |= row_slider("Smoke Albedo", &fir->smoke_albedo, 0.0f, 1.0f);
        g_changed |= row_float("Light Intensity", &fir->light_intensity, 10.0f, 0.0f, 1000000.0f, "%.0f cd");
        g_changed |= row_float("Light Radius", &fir->light_radius, 5.0f, 0.0f, 100000.0f, "%.0f cm");
        g_changed |= row_slider("Flicker", &fir->flicker, 0.0f, 1.0f);
        igEndTable();
    }
    CGroom* grm = ecs_get(w, e, COMP_GROOM);
    if (grm && begin_section("Groom", "sec_groom")) { // Unreal's Groom Component (A9.3b): the strands grown, their look
        label_cell("Preset");
        int pr = (int)grm->preset;
        const char* pn[GROOM_PRESET_COUNT];
        for (int i = 0; i < GROOM_PRESET_COUNT; i++) pn[i] = TR(g_groom_preset_names[i]);
        if (igCombo_Str_arr("##gpreset", &pr, pn, GROOM_PRESET_COUNT, -1)) groom_apply_preset(grm, pr), g_changed = true;
        int ns = (int)grm->strands, sg = (int)grm->segments;
        label_cell("Strands");
        if (igDragInt("##gstrands", &ns, 100.0f, 100, 200000, "%d", 0)) grm->strands = (uint32_t)ns, g_changed = true;
        label_cell("Segments");
        if (igDragInt("##gsegs", &sg, 0.2f, 2, 32, "%d", 0)) grm->segments = (uint32_t)sg, g_changed = true;
        g_changed |= row_float("Length", &grm->length, 0.1f, 0.5f, 150.0f, "%.1f cm");
        g_changed |= row_slider("Length Variation", &grm->length_var, 0.0f, 1.0f);
        g_changed |= row_float("Root Width", &grm->root_width, 0.002f, 0.01f, 2.0f, "%.3f mm");
        g_changed |= row_float("Tip Width", &grm->tip_width, 0.002f, 0.01f, 2.0f, "%.3f mm");
        g_changed |= row_float("Curl", &grm->curl, 0.02f, 0.0f, 10.0f, "%.2f cm");
        g_changed |= row_float("Curl Frequency", &grm->curl_freq, 0.05f, 0.0f, 30.0f, "%.1f");
        g_changed |= row_slider("Gravity", &grm->gravity, 0.0f, 1.0f);
        g_changed |= row_slider("Clumping", &grm->clump, 0.0f, 1.0f);
        g_changed |= row_slider("Scalp", &grm->scalp, -1.0f, 1.0f);
        g_changed |= row_slider("Hairline", &grm->hairline, -1.0f, 1.0f);
        g_changed |= row_slider("Melanin", &grm->melanin, 0.0f, 1.0f);
        g_changed |= row_slider("Redness", &grm->redness, 0.0f, 1.0f);
        g_changed |= row_slider("Roughness", &grm->roughness, 0.1f, 0.9f);
        label_cell("Dye");
        if (igColorEdit3("##gdye", grm->dye, ImGuiColorEditFlags_Float)) g_changed = true;
        g_changed |= row_slider("Self Occlusion", &grm->occlusion, 0.0f, 1.0f);
        label_cell("LOD");
        bool lod = grm->lod != 0;
        if (igCheckbox("##glod", &lod)) grm->lod = lod ? 1u : 0u, g_changed = true;
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("Far away, fewer strands are drawn and the rest wider: the same look for less work."));
        // (2026-11-16) dynamic hair: simulated on the GPU, colliding with the head, the character's bones and the scene
        label_cell("Dynamic");
        bool dyn = grm->dynamic != 0;
        if (igCheckbox("##gdyn", &dyn)) grm->dynamic = dyn ? 1u : 0u, g_changed = true;
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("Simulated: it falls, swings with the head, the wind moves it and it collides (the head, the character's bones, the scene). Off: it stays as grown."));
        if (grm->dynamic) {
            g_changed |= row_slider("Stiffness", &grm->stiffness, 0.0f, 1.0f);
            g_changed |= row_slider("Damping", &grm->damping, 0.0f, 0.5f);
            label_cell("Wind");
            if (igDragFloat3("##gwind", grm->wind, 5.0f, -3000.0f, 3000.0f, "%.0f", 0)) g_changed = true;
            g_changed |= row_slider("Gusts", &grm->gusts, 0.0f, 1.0f);
            label_cell("Collide");
            bool col = grm->collide != 0;
            if (igCheckbox("##gcol", &col)) grm->collide = col ? 1u : 0u, g_changed = true;
            g_changed |= row_float("Collision Radius", &grm->collision_radius, 0.05f, 0.0f, 10.0f, "%.2f cm");
        }
        // (2026-11-16) the guides: the Tlender's hair, an OBJ's curves (.rgroom): the strands grown from them
        label_cell("Guides");
        static GroomGuidesEntry gl[256];
        static uint32_t ngl, gl_hash;
        static char gl_name[96];
        if (gl_hash != grm->guides) { // (its name: a walk once a change)
            gl_hash = grm->guides, gl_name[0] = 0;
            ngl = grm->guides ? groom_guides_list(gl, 256) : 0;
            for (uint32_t i = 0; i < ngl; i++)
                if (groom_guides_hash(gl[i].vpath) == grm->guides) snprintf(gl_name, sizeof gl_name, "%s", gl[i].name);
        }
        if (igBeginCombo("##gguides", grm->guides ? (gl_name[0] ? gl_name : "?") : TR("None (grown on the head)"), 0)) {
            static double listed;
            if (igIsWindowAppearing() || re_time_seconds() - listed > 2.0) ngl = groom_guides_list(gl, 256), listed = re_time_seconds();
            if (igSelectable_Bool(TR("None (grown on the head)"), !grm->guides, 0, (ImVec2){ 0, 0 })) grm->guides = 0, g_changed = true;
            for (uint32_t i = 0; i < ngl; i++) {
                uint32_t h = groom_guides_hash(gl[i].vpath);
                if (igSelectable_Bool(gl[i].name, h == grm->guides, 0, (ImVec2){ 0, 0 })) grm->guides = h, g_changed = true;
            }
            igEndCombo();
        }
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("Hair made elsewhere (the Tlender's hair, an OBJ's curves): each strand follows its nearest guides. None: grown on the head by the settings above."));
        int seed = (int)grm->seed;
        label_cell("Seed");
        if (igDragInt("##gseed", &seed, 0.2f, 0, 100000, "%d", 0)) grm->seed = (uint32_t)seed, g_changed = true;
        igEndTable();
    }
    CAudioVolume* av = ecs_get(w, e, COMP_AUDIO_VOLUME);
    if (av) { // Unreal's Audio Volume: Reverb, Ambient Zone
        char lab[96];
        snprintf(lab, sizeof lab, "%s###sec_audiovol", TR("Audio Volume"));
        if (igCollapsingHeader_TreeNodeFlags(lab, ImGuiTreeNodeFlags_DefaultOpen)) {
            igPushItemWidth(-igGetFontSize() * 7.5f);
            if (audio_volume_details(av)) g_changed = true;
            igPopItemWidth();
        }
    }
    CRigidBody* rb = ecs_get(w, e, COMP_RIGID_BODY);
    if (rb) { // Unreal's Physics section (physics.c)
        bool keep = true;
        char lab[96];
        snprintf(lab, sizeof lab, "%s###sec_physics", TR("Physics"));
        if (igCollapsingHeader_BoolPtr(lab, &keep, ImGuiTreeNodeFlags_DefaultOpen) &&
            igBeginTable("sec_phys", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, (ImVec2){ 0, 0 }, 0)) {
            igTableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 7.5f, 0);
            igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0);
            if (row_check("Simulate Physics", &rb->simulate)) {
                g_changed = true;
                if (rb->simulate && t) t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE; // Unreal: Movable
            }
            label_cell("Collision Shape");
            uint32_t cs = rb->collision_shape < COLSHAPE_COUNT ? rb->collision_shape : 0;
            if (igBeginCombo("##colshape", TR(g_collision_shape_names[cs]), 0)) {
                for (uint32_t k = 0; k < COLSHAPE_COUNT; k++)
                    if (igSelectable_Bool(TR(g_collision_shape_names[k]), k == cs, 0, (ImVec2){ 0, 0 })) rb->collision_shape = k, g_changed = true;
                igEndCombo();
            }
            if (!rb->simulate) g_changed |= row_check("Use Complex Collision", &rb->complex_collision); // (a static's: its triangles)
            g_changed |= row_float("Mass (kg, 0: auto)", &rb->mass, 0.5f, 0.0f, 1e6f, "%.1f kg");
            g_changed |= row_check("Enable Gravity", &rb->enable_gravity);
            g_changed |= row_float("Linear Damping", &rb->linear_damping, 0.005f, 0.0f, 100.0f, "%.3f");
            g_changed |= row_float("Angular Damping", &rb->angular_damping, 0.005f, 0.0f, 100.0f, "%.3f");
            g_changed |= row_float("Friction", &rb->friction, 0.01f, 0.0f, 2.0f, "%.2f");
            g_changed |= row_float("Restitution", &rb->restitution, 0.01f, 0.0f, 1.0f, "%.2f");
            { // (Q8) the Physical Material: friction, bounce, density as an asset (over the two above)
                label_cell("Physical Material");
                const char* cur = rb->phys_material ? physmat_vpath(rb->phys_material) : "";
                const char* sl = strrchr(cur, '/');
                char out[600];
                if (asset_field("##physmat", cur[0] ? (sl ? sl + 1 : cur) : TR("None"), cur, ASSET_PHYS_MATERIAL, AK(ASSET_PHYS_MATERIAL), true, out,
                                sizeof out) == ASSET_FIELD_PICKED) {
                    rb->phys_material = out[0] ? physmat_vpath_hash(out) : 0u, g_changed = true;
                    if (out[0]) physmat_remember(out);
                }
                if (rb->phys_material && !physmat_get(rb->phys_material)) igTextDisabled("%s", TR("(not found: the fields above are used)"));
            }
            g_changed |= row_check("Use CCD", &rb->ccd);
            g_changed |= row_float("Max Angular Velocity (0: any)", &rb->max_angular_velocity, 10.0f, 0.0f, 1e6f, "%.0f\xc2\xb0/s");
            g_changed |= row_check("Fracture", &rb->fracture);
            if (rb->fracture) {
                g_changed |= row_float("Fracture Speed", &rb->fracture_speed, 5.0f, 50.0f, 1e5f, "%.0f cm/s");
                label_cell("Pieces per Axis");
                int pc = (int)rb->fracture_pieces;
                if (igSliderInt("##pieces", &pc, 2, 4, "%d", 0)) rb->fracture_pieces = (uint32_t)pc, g_changed = true;
            }
            g_changed |= vec3_row("Initial Velocity", rb->velocity, 1.0f, "%.0f", 0.0f);
            igEndTable();
        }
        if (!keep) {
            editor_undo_push(ed);
            ecs_remove(w, e, COMP_RIGID_BODY);
            return;
        }
    }
    CNav* navc = ecs_get(w, e, COMP_NAV);
    if (navc && begin_section("Navigation", "sec_nav")) { // Unreal's Nav Mesh Bounds Volume, Nav Link Proxy, Nav Modifier Volume
        if (navc->kind == NAV_BOUNDS_VOLUME) {
            const NavMeshStats* ns = navmesh_stats();
            label_cell("NavMesh");
            igTextDisabled(TR("%u tiles, %u polygons (P shows it)"), ns->tiles, ns->polys);
        } else if (navc->kind == NAV_LINK_PROXY) {
            label_cell("Left Point");
            g_changed |= igDragFloat3("##navl", navc->left, 1.0f, -1e5f, 1e5f, "%.0f", 0);
            label_cell("Right Point");
            g_changed |= igDragFloat3("##navr", navc->right, 1.0f, -1e5f, 1e5f, "%.0f", 0);
            label_cell("Direction");
            const char* dn[3] = { TR("Both Ways"), TR("Left to Right"), TR("Right to Left") };
            int dir = (int)(navc->direction < 3 ? navc->direction : 0);
            if (igCombo_Str_arr("##navdir", &dir, dn, 3, -1)) navc->direction = (uint32_t)dir, g_changed = true;
            g_changed |= row_float("Snap Radius", &navc->snap_radius, 1.0f, 1.0f, 1000.0f, "%.0f cm");
        } else {
            label_cell("Area Class");
            const char* an[NAV_AREA_COUNT];
            for (int k = 0; k < NAV_AREA_COUNT; k++) an[k] = TR(g_nav_area_names[k]);
            int ar = (int)(navc->area < NAV_AREA_COUNT ? navc->area : NAV_AREA_DEFAULT);
            if (igCombo_Str_arr("##navarea", &ar, an, NAV_AREA_COUNT, -1)) navc->area = (uint32_t)ar, g_changed = true;
        }
        igEndTable();
    }
    CConstraint* pc = ecs_get(w, e, COMP_CONSTRAINT);
    if (pc && begin_section("Physics Constraint", "sec_constraint")) { // Unreal's: the two actors, the joint, its limit and motor
        for (int side = 0; side < 2; side++) {
            Entity* who = side ? &pc->actor2 : &pc->actor1;
            label_cell(side ? "Constraint Actor 2" : "Constraint Actor 1");
            const CName* cur = *who && ecs_alive(w, *who) ? ecs_get(w, *who, COMP_NAME) : NULL;
            igPushID_Int(side);
            if (igBeginCombo("##actor", cur ? cur->text : TR(side ? "None (the world)" : "None"), 0)) {
                if (igSelectable_Bool(TR(side ? "None (the world)" : "None"), !*who, 0, (ImVec2){ 0, 0 })) *who = ENTITY_NONE, g_changed = true;
                for (uint32_t ai = 0; ai < w->archetype_count; ai++) { // (the actors with physics)
                    const EcsArchetype* ar = &w->archetypes[ai];
                    if (!(ar->mask & COMP_BIT(COMP_RIGID_BODY)) || !(ar->mask & COMP_BIT(COMP_NAME))) continue;
                    const CName* nm = ecs_arch_column(ar, COMP_NAME);
                    const Entity* ae = ecs_arch_entities(ar);
                    for (uint32_t i = 0; i < ar->count; i++) {
                        igPushID_Int((int)ae[i]);
                        if (igSelectable_Bool(nm[i].text, *who == ae[i], 0, (ImVec2){ 0, 0 })) *who = ae[i], g_changed = true;
                        igPopID();
                    }
                }
                igEndCombo();
            }
            igPopID();
        }
        label_cell("Joint");
        static const char* const types[CONSTRAINT_TYPE_COUNT] = { "Fixed", "Hinge", "Ball and Socket", "Prismatic (Slide)", "Cone and Twist", "6DOF" };
        const char* tn[CONSTRAINT_TYPE_COUNT];
        for (int k = 0; k < CONSTRAINT_TYPE_COUNT; k++) tn[k] = TR(types[k]);
        int ty = (int)pc->type;
        if (igCombo_Str_arr("##jtype", &ty, tn, CONSTRAINT_TYPE_COUNT, -1)) {
            pc->type = (uint32_t)ty, g_changed = true;
            if (ty == CONSTRAINT_CONE && pc->swing1_limit <= 0.0f && pc->swing2_limit <= 0.0f) // (a cone to start from)
                pc->swing1_limit = pc->swing2_limit = 45.0f, pc->twist_limit = 30.0f;
            if (ty == CONSTRAINT_6DOF && pc->lin_limit <= 0.0f && pc->swing1_limit <= 0.0f)
                for (int k = 0; k < 3; k++) pc->lin_motion[k] = pc->ang_motion[k] = CMOTION_LOCKED;
        }
        if (pc->type == CONSTRAINT_HINGE || pc->type == CONSTRAINT_PRISMATIC) {
            bool hinge = pc->type == CONSTRAINT_HINGE;
            g_changed |= row_float(hinge ? "Angle Limit (0: free)" : "Slide Limit (0: free)", &pc->limit, hinge ? 0.5f : 1.0f, 0.0f, hinge ? 180.0f : 1e5f, hinge ? "%.0f\xc2\xb0" : "%.0f cm");
            g_changed |= row_check("Motor", &pc->motor);
            if (pc->motor) {
                g_changed |= row_float("Motor Speed", &pc->motor_speed, 1.0f, -1e5f, 1e5f, hinge ? "%.0f\xc2\xb0/s" : "%.0f cm/s");
                g_changed |= row_float("Motor Strength (0: any)", &pc->motor_strength, 100.0f, 0.0f, 1e9f, "%.0f");
            }
        }
        if (pc->type == CONSTRAINT_CONE || pc->type == CONSTRAINT_6DOF) g_changed |= constraint_6dof_rows(pc);
        if (ed->playing) { // (the run's: what it holds this step)
            label_cell("Force / Torque");
            igText("%.0f / %.0f%s", pc->force, pc->torque, pc->broken ? TR(" (broken)") : "");
        }
        g_changed |= row_float("Break Force (0: never)", &pc->break_force, 100.0f, 0.0f, 1e9f, "%.0f");
        g_changed |= row_float("Break Torque (0: never)", &pc->break_torque, 100.0f, 0.0f, 1e12f, "%.0f");
        uint32_t dis = !pc->keep_collision;
        if (row_check("Disable Collision", &dis)) pc->keep_collision = !dis, g_changed = true;
        igEndTable();
    }
    CHealth* hc = ecs_get(w, e, COMP_HEALTH);
    if (hc && begin_section("Health", "sec_health")) { // (health.h: life, shield, regeneration, damage types, death)
        if (ed->playing) { // the run's state, as the game sees it
            label_cell("Status");
            float pct = hc->max_health > 0.0f ? hc->health / hc->max_health : 0.0f;
            char ov[64];
            if (hc->flags & HEALTH_DEAD) snprintf(ov, sizeof ov, "%s", TR("Dead"));
            else snprintf(ov, sizeof ov, "%.0f / %.0f", hc->health, hc->max_health);
            igProgressBar(pct, (ImVec2){ -1, 0 }, ov);
        }
        g_changed |= row_float("Max Health", &hc->max_health, 1.0f, 0.0f, 1e7f, "%.0f");
        g_changed |= row_float("Health", &hc->health, 1.0f, 0.0f, 1e7f, "%.0f");
        g_changed |= row_float("Max Shield", &hc->max_shield, 1.0f, 0.0f, 1e7f, "%.0f");
        if (hc->max_shield > 0.0f) g_changed |= row_float("Shield", &hc->shield, 1.0f, 0.0f, hc->max_shield, "%.0f");
        g_changed |= row_float("Regen Delay", &hc->regen_delay, 0.05f, 0.0f, 600.0f, "%.2f s");
        g_changed |= row_float("Regen Rate", &hc->regen_rate, 0.5f, 0.0f, 1e6f, "%.1f /s");
        if (hc->max_shield > 0.0f) g_changed |= row_float("Shield Regen Rate", &hc->shield_regen_rate, 0.5f, 0.0f, 1e6f, "%.1f /s");
        static const struct { uint32_t bit; const char* label; } fl[] = { { HEALTH_INVINCIBLE, "Invincible" }, { HEALTH_NUMBERS, "Damage Numbers" },
                                                                            { HEALTH_SHOW_BAR, "Show Health Bar" } };
        for (size_t k = 0; k < sizeof fl / sizeof fl[0]; k++) {
            uint32_t on = (hc->flags & fl[k].bit) != 0;
            if (row_check(fl[k].label, &on)) hc->flags = on ? (hc->flags | fl[k].bit) : (hc->flags & ~fl[k].bit), g_changed = true;
        }
        label_cell("On Death");
        static const char* const deaths[HEALTH_DEATH_COUNT] = { "Only the Event", "Destroy Actor", "Ragdoll" };
        const char* dn[HEALTH_DEATH_COUNT];
        for (int k = 0; k < HEALTH_DEATH_COUNT; k++) dn[k] = TR(deaths[k]);
        int dsel = (int)(hc->death < HEALTH_DEATH_COUNT ? hc->death : 0);
        if (igCombo_Str_arr("##hdeath", &dsel, dn, HEALTH_DEATH_COUNT, -1)) hc->death = (uint32_t)dsel, g_changed = true;
        if (hc->death == HEALTH_DEATH_DESTROY) g_changed |= row_float("Destroy Delay", &hc->destroy_delay, 0.05f, 0.0f, 600.0f, "%.2f s");
        if (hc->death == HEALTH_DEATH_RAGDOLL) g_changed |= row_float("Destroy Delay (0: keep)", &hc->destroy_delay, 0.05f, 0.0f, 600.0f, "%.2f s");
        for (int k = 0; k < HEALTH_MAX_MULT; k++) { // the damage types: a Gameplay Tag and its multiplier
            igPushID_Int(k);
            label_cell(k ? "" : "Damage Multipliers");
            float wtag = igGetContentRegionAvail().x * 0.62f;
            igSetNextItemWidth(wtag);
            char buf[HEALTH_TAG_LEN];
            snprintf(buf, sizeof buf, "%s", hc->mult[k].tag);
            if (igInputTextWithHint("##mtag", "Damage.Fire", buf, sizeof buf, 0, NULL, NULL)) {
                char clean[HEALTH_TAG_LEN];
                if (!gt_clean(buf, clean, sizeof clean)) clean[0] = 0;
                if (strcmp(clean, hc->mult[k].tag)) {
                    if (!hc->mult[k].tag[0] && hc->mult[k].scale == 0.0f) hc->mult[k].scale = 1.0f; // (a new type starts at x1)
                    snprintf(hc->mult[k].tag, HEALTH_TAG_LEN, "%s", clean), g_changed = true;
                }
            }
            igSetItemTooltip("%s", TR("A damage type (Gameplay Tag): it covers the types under it. Empty: unused"));
            igSameLine(0, 4);
            igSetNextItemWidth(-1);
            g_changed |= igDragFloat("##mscale", &hc->mult[k].scale, 0.01f, 0.0f, 100.0f, "x%.2f", 0);
            igPopID();
        }
        igEndTable();
    }
    CSaveable* svc = ecs_get(w, e, COMP_SAVEABLE);
    if (svc && begin_section("Saveable", "sec_saveable")) { // (save_progress.h: what of this actor the progress's saves keep)
        if (ed->playing) {
            label_cell("Key");
            igTextDisabled("%08X%s", svc->key, svc->key ? "" : TR(" (spawned: saved with its class)"));
        }
        static const struct { uint32_t bit; const char* label; } sf[] = {
            { SAVEABLE_TRANSFORM, "Save Transform" }, { SAVEABLE_HEALTH, "Save Health" }, { SAVEABLE_INVENTORY, "Save Inventory" },
            { SAVEABLE_VARIABLES, "Save SaveGame Variables" }, { SAVEABLE_ALL_VARIABLES, "Save All Variables" },
            { SAVEABLE_DESTROYED, "Remember Destroyed" }, { SAVEABLE_HIDDEN, "Save Hidden In Game" } };
        for (size_t k = 0; k < sizeof sf / sizeof sf[0]; k++) {
            uint32_t on = (svc->flags & sf[k].bit) != 0;
            if (row_check(sf[k].label, &on)) svc->flags = on ? (svc->flags | sf[k].bit) : (svc->flags & ~sf[k].bit), g_changed = true;
        }
        igEndTable();
    }
    CCheckpoint* ckc = ecs_get(w, e, COMP_CHECKPOINT);
    if (ckc && begin_section("Checkpoint", "sec_checkpoint")) { // (save_progress.h: a volume that saves and is the respawn point)
        if (ed->playing) {
            label_cell("Status");
            igTextDisabled("%s", TR((ckc->reached & 1u) ? "Reached" : "Not reached"));
        }
        label_cell("Box Extent");
        if (igDragFloat3("##ckext", ckc->extent, 1.0f, 1.0f, 1e6f, "%.0f", 0)) g_changed = true;
        label_cell("Spawn Offset");
        if (igDragFloat3("##ckoff", ckc->spawn_offset, 1.0f, -1e6f, 1e6f, "%.0f", 0)) g_changed = true;
        static const struct { uint32_t bit; const char* label; } cf[] = {
            { CHECKPOINT_SAVE, "Save When Reached" }, { CHECKPOINT_ONCE, "Only Once" }, { CHECKPOINT_RESPAWN_ON_DEATH, "Respawn on Death" } };
        for (size_t k = 0; k < sizeof cf / sizeof cf[0]; k++) {
            uint32_t on = (ckc->flags & cf[k].bit) != 0;
            if (row_check(cf[k].label, &on)) ckc->flags = on ? (ckc->flags | cf[k].bit) : (ckc->flags & ~cf[k].bit), g_changed = true;
        }
        if (ckc->flags & CHECKPOINT_RESPAWN_ON_DEATH) g_changed |= row_float("Respawn Delay", &ckc->respawn_delay, 0.05f, 0.0f, 600.0f, "%.2f s");
        label_cell("Slot Name");
        igSetNextItemWidth(-1);
        if (igInputTextWithHint("##ckslot", "Checkpoint", ckc->slot, sizeof ckc->slot, 0, NULL, NULL)) g_changed = true;
        igEndTable();
    }
    if (t && begin_section("Rendering", "sec_render")) {
        uint32_t hidden = (t->flags & TRANSFORM_HIDDEN) != 0;
        if (row_check("Hidden in Editor", &hidden)) {
            t->flags = hidden ? (t->flags | TRANSFORM_HIDDEN) : (t->flags & ~TRANSFORM_HIDDEN);
            g_changed = true;
        }
        igEndTable();
    }
    bool wp_add = false;
    CPartition wp_new;
    g_changed |= wp_actor_details(w, e, &wp_add, &wp_new); // (B1: any actor's, once the level has a World Partition)
    igSpacing();
    const EcsSlot* s = &ecs_slots(w)[entity_index(e)];
    igTextDisabled(TR("Entity %u (generation %u), archetype %u, row %u"), entity_index(e), entity_gen(e), s->archetype, s->row);

    // The first change of an edit (a drag, typing, a click) becomes one undo step with the values from before it;
    // the step stays open while a field is held, so a whole drag is one step
    static bool editing;
    if (g_changed && !editing && !ed->playing && ecs_alive(w, e) && ecs_mask(w, e) == mask) {
        static uint8_t after[COMP_COUNT][DETAILS_COMP_MAX];
        for (int c = 0; c < COMP_COUNT; c++)
            if (mask & COMP_BIT(c)) {
                void* p = ecs_get(w, e, (ComponentId)c);
                memcpy(after[c], p, g_components[c].size);
                memcpy(p, before[c], g_components[c].size);
            }
        editor_undo_push(ed);
        for (int c = 0; c < COMP_COUNT; c++)
            if (mask & COMP_BIT(c)) memcpy(ecs_get(w, e, (ComponentId)c), after[c], g_components[c].size);
        editing = true;
    }
    if (!igIsAnyItemActive()) editing = false;
    if (wp_add && !ed->playing && ecs_add(w, e, COMP_PARTITION)) { // (after the undo step: the component is new)
        *(CPartition*)ecs_get(w, e, COMP_PARTITION) = wp_new;
        ecs_touch(w, e);
        ed->dirty = true;
    }
}

// World Partition's map (in its Details, Unreal's World Partition window): the level's cells (a square each, its actors'
// count as its shade), the camera; while playing the cells in the world (green) and those shown by their HLOD (blue)
static void wp_map(Editor* ed, const CWorldPartition* c) {
    const float cs = c->cell_size >= 100.0f ? c->cell_size : 12800.0f;
    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    static WpCellView cells[4096];
    uint32_t n = 0;
    const WpCellView* live = NULL;
    uint32_t nl = ed->playing ? wp_cells_view(&live) : 0;
    if (nl) {
        n = nl < 4096 ? nl : 4096;
        memcpy(cells, live, n * sizeof cells[0]);
    } else { // (the editor: the actors that would stream, by cell)
        World* w = &ed->world;
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            if (!(a->mask & COMP_BIT(COMP_TRANSFORM)) || (a->mask & COMP_BIT(COMP_ATTACH)) || !a->count) continue;
            const Entity* ents = ecs_arch_entities(a);
            const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
            for (uint32_t i = 0; i < a->count; i++) {
                if (!wp_actor_spatial(w, ents[i])) continue;
                int32_t cx, cy;
                wp_cell_of(cs, t[i].location, &cx, &cy);
                uint32_t k = 0;
                while (k < n && (cells[k].cx != cx || cells[k].cy != cy)) k++;
                if (k == n) {
                    if (n == 4096) continue;
                    cells[n++] = (WpCellView){ cx, cy, 0, false, false };
                }
                cells[k].actors++;
            }
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        if (cells[i].cx < x0) x0 = cells[i].cx;
        if (cells[i].cy < y0) y0 = cells[i].cy;
        if (cells[i].cx > x1) x1 = cells[i].cx;
        if (cells[i].cy > y1) y1 = cells[i].cy;
    }
    if (!n) {
        igTextDisabled("%s", TR("No actor streams yet: place props, lights or Blueprints away from the origin."));
        return;
    }
    uint32_t most = 1;
    for (uint32_t i = 0; i < n; i++) most = cells[i].actors > most ? cells[i].actors : most;
    float W = igGetContentRegionAvail().x, span = (float)((x1 - x0 + 1) > (y1 - y0 + 1) ? (x1 - x0 + 1) : (y1 - y0 + 1));
    float px = W / span;
    if (px > 24.0f) px = 24.0f;
    ImVec2_c oc = igGetCursorScreenPos();
    ImVec2 o = { oc.x, oc.y };
    ImDrawList* dl = igGetWindowDrawList();
    float hgt = (float)(y1 - y0 + 1) * px;
    for (uint32_t i = 0; i < n; i++) {
        float x = o.x + (float)(cells[i].cx - x0) * px, y = o.y + hgt - (float)(cells[i].cy - y0 + 1) * px; // (north up)
        uint32_t shade = 60u + (uint32_t)(120.0f * (float)cells[i].actors / (float)most);
        uint32_t col = cells[i].in ? TH(TK_GREEN) : cells[i].region_shown ? TH(TK_BLUE) : (0xFF000000u | shade << 16 | shade << 8 | shade);
        ImDrawList_AddRectFilled(dl, (ImVec2){ x + 1, y + 1 }, (ImVec2){ x + px - 1, y + px - 1 }, col, 0, 0);
    }
    const float* cam = ed->playing ? ed->render_view.pos : ed->cam.pos;
    float cx = o.x + (cam[0] / cs - (float)x0) * px, cy = o.y + hgt - (cam[1] / cs - (float)y0) * px;
    ImDrawList_AddCircle(dl, (ImVec2){ cx, cy }, c->loading_range / cs * px, TH(TK_AMBER), 48, 1.5f);
    ImDrawList_AddCircleFilled(dl, (ImVec2){ cx, cy }, 3.0f, TH(TK_N15), 12);
    igDummy((ImVec2){ W, hgt + 4.0f });
    const WpStats* s = wp_stats();
    if (ed->playing && wp_active())
        igTextDisabled(TR("%u of %u cells in, %u of %u actors, HLOD: %u proxies, %u regions; %.2f ms (worst %.2f)"), s->cells_in, s->cells,
                       s->records_in, s->records, s->proxies_shown, s->regions_shown, s->ms, s->ms_peak);
    else
        igTextDisabled(TR("%u cells of %.0f m; the circle: the Loading Range around the camera"), n, cs / 100.0f);
}

// --- Status bar, Output Log, About ---

static void status_bar(Editor* ed, double now) {
    // (Q26) the mockups' bar: Content Drawer, Output Log, Statistics, Profiler, Device Manager on the left, the last
    // message in the middle; Ready, Shader Compile, Derived Data and the version on the right (editor_look.c)
    bool on = ed->drawer_open;
    if (ui_icon_button("sb_drawer", ICON_FOLDER, TR("Content Drawer"), TH(TK_ASSET_FOLDER), on, "Ctrl+Space")) {
        if (!ed->content_docked) ed->drawer_open = !ed->drawer_open;
        else igSetWindowFocus_Str(dock_panel_id(DP_CONTENT)); // (docked: its tab to the front)
    }
    igSameLine(0, 4);
    if (ui_icon_button("sb_log", ICON_LOG, TR("Output Log"), TH(TK_N14), ed->show_log, NULL)) ed->show_log = !ed->show_log;
    look_status_left(ed);
    igSameLine(0, 4);
    if (services_status_badge(ed)) igSameLine(0, 4);
    // (Build Lighting's progress and its Cancel: its card at the bottom right, async_ui.c)
    igSameLine(0, 16);
    igAlignTextToFramePadding();
    // a fresh message pops its icon in; playing and unsaved changes pulse a dot (green / amber)
    float isz = igGetTextLineHeight() + 4.0f;
    ImVec2 ip = igGetCursorScreenPos();
    ip.y += (igGetFrameHeight() - isz) * 0.5f;
    ImDrawList* sdl = igGetWindowDrawList();
    if (ed->status_msg[0] && now < ed->status_until) {
        double age = now - (ed->status_until - 6.0);
        float fade = (float)fmin(1.0, (ed->status_until - now) / 0.6); // (the last 0.6 s: it fades away)
        ui_icon_anim(sdl, ICON_INFO, ip.x, ip.y, isz, theme_alpha(TH(TK_INFO), (unsigned)(fade * 255.0f)), UI_ANIM_POP, age);
        igDummy((ImVec2){ isz, isz });
        igSameLine(0, 4);
        igPushStyleVar_Float(ImGuiStyleVar_Alpha, igGetStyle()->Alpha * fade);
        igTextUnformatted(ed->status_msg, NULL);
        igPopStyleVar(1);
    } else if (ed->playing || ed->dirty) {
        ui_pulse_dot(sdl, ip.x, ip.y, isz, ed->playing ? TH(TK_GREEN) : TH(TK_AMBER), now);
        igDummy((ImVec2){ isz, isz });
        igSameLine(0, 4);
        igTextDisabled("%s", ed->dirty ? TR("Unsaved changes") : TR("Playing"));
    }
    look_status_right(ed, now);
}

static void output_log(Editor* ed) {
    if (!ed->show_log) return;
    // (Q26) a panel of the layout (a tab beside the Content Browser by default), dragged anywhere like the others
    if (dock_panel_begin(DP_LOG, TR("Output Log"), &ed->show_log, 0)) {
        if (g_font_mono) igPushFont(g_font_mono, 0.0f);
        for (int i = 0; i < ed->log_count; i++) igTextUnformatted(ed->log[i], NULL);
        if (g_font_mono) igPopFont();
        if (!ed->log_count) igTextDisabled("%s", TR("The editor's messages show here."));
        if (igGetScrollY() >= igGetScrollMaxY() - 4) igSetScrollHereY(1.0f);
    }
    dock_panel_end();
}

bool editor_request_quit(Editor* ed) {
    if (!ed->confirm_quit || !ed->dirty || ed->playing) return true;
    glfwSetWindowShouldClose(ed->window, GLFW_FALSE);
    ed->quit_asking = true;
    return false;
}

static void quit_popup(Editor* ed, double now) {
    char title[64];
    snprintf(title, sizeof title, "%s###quit", TR("Save changes?"));
    if (ed->quit_asking) {
        igOpenPopup_Str(title, 0);
        ed->quit_asking = false;
    }
    if (!igBeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;
    igText("%s", TR("The level has unsaved changes."));
    float bw = igGetFontSize() * 8.0f;
    if (igButton(TR("Save and Quit"), (ImVec2){ bw, 0 })) {
        if (editor_save_level(ed, ed->level_path, now)) ed->quit = true;
        igCloseCurrentPopup();
    }
    igSameLine(0, -1);
    if (igButton(TR("Don't Save"), (ImVec2){ bw, 0 })) ed->quit = true, igCloseCurrentPopup();
    igSameLine(0, -1);
    if (igButton(TR("Cancel"), (ImVec2){ bw, 0 }) || igIsKeyPressed_Bool(ImGuiKey_Escape, false)) igCloseCurrentPopup();
    igEndPopup();
}

static void about_popup(Editor* ed) {
    char title[64];
    snprintf(title, sizeof title, "%s###about", TR("About Rivora Engine"));
    if (ed->show_about) {
        igOpenPopup_Str(title, 0);
        ed->show_about = false;
    }
    if (igBeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        igText("Rivora Engine %s", RE_VERSION_STRING);
        igTextDisabled("%s", TR("C11 + Vulkan 1.3, data-oriented (archetype ECS)."));
        // (2026-11-23) the open source release: the license, the notices, who it isn't, the donations
        igTextWrapped("%s", TR("Free and open source software under the MIT License. Games made with it are yours: sell them freely, no royalties."));
        igTextDisabled("%s", TR("Not affiliated with or endorsed by Epic Games. Names of other products belong to their owners."));
        if (igButton(TR("License and Third-Party Notices"), (ImVec2){ 0, 0 })) {
            char dir[1024], np[1100];
            if (!re_exe_dir(dir, sizeof dir)) snprintf(dir, sizeof dir, ".");
            snprintf(np, sizeof np, "%s/THIRD_PARTY_NOTICES.md", dir);
            crash_open_file(np);
        }
        const char* don = endpoints_get("support", "donate_url");
        if (don && don[0]) {
            igSameLine(0, -1);
            if (igButton(TR("Support Rivora Engine (Donate)"), (ImVec2){ 0, 0 })) update_open_url(don);
        }
        igSeparator();
        igTextUnformatted(TR("Alt+P play   Esc stop   F8 possess/eject   Shift+F1 free the mouse   Pause pause\n"
                             "Right mouse + WASD/QE fly (wheel: speed)   Middle mouse pan   Wheel dolly\n"
                             "Q/W/E/R tools (Space cycles)   F focus   End snap to floor   Ctrl+D duplicate   Delete delete\n"
                             "Ctrl+Z/Y undo/redo   Ctrl+S save   Ctrl+O open   F2 rename   G game view   F11 fullscreen\n"
                             "1-9, 0 view modes   Ctrl+Space Content Drawer"), NULL);
        if (igButton("OK", (ImVec2){ 120, 0 })) igCloseCurrentPopup();
        igEndPopup();
    }
}


// The possessed pawn's input (a Blueprint Character): W/S A/D and the mouse as Unreal's axes, every key's press and
// release as Key Events. Nothing while ejected (F8) or with the mouse released (Shift+F1).
static void pawn_input(Editor* ed) {
    static uint8_t s_down[512];
    float move[2] = { 0, 0 }, look[2] = { 0, 0 };
    bool active = (ed->possessed || ed->bp_test == 3) && !ed->mouse_released && bp_player(&ed->world) != ENTITY_NONE;
    if (active) {
        GLFWwindow* win = ed->window;
        const ProjectSettings* ps = project(); // Project Settings > Input
        const GameUserSettings* us = gus_applied(); // (the player's options: sensitivity, Invert Y, the movement keys)
        move[0] = (float)((glfwGetKey(win, gus_remapped_code("IA_Move", ps->key_forward)) == GLFW_PRESS) - (glfwGetKey(win, gus_remapped_code("IA_Move", ps->key_back)) == GLFW_PRESS));
        move[1] = (float)((glfwGetKey(win, gus_remapped_code("IA_Move", ps->key_right)) == GLFW_PRESS) - (glfwGetKey(win, gus_remapped_code("IA_Move", ps->key_left)) == GLFW_PRESS));
        double cx, cy;
        glfwGetCursorPos(win, &cx, &cy);
        float sens = ps->mouse_sensitivity * (us->sensitivity > 0.0f ? us->sensitivity : 1.0f);
        look[0] = (float)(cx - ed->last_cursor[0]) * sens;
        look[1] = (float)(ed->last_cursor[1] - cy) * sens * ((ps->invert_mouse_y != us->invert_y) ? -1.0f : 1.0f);
        ed->last_cursor[0] = cx, ed->last_cursor[1] = cy;
        if (ed->bp_test) move[0] = ed->bp_test != 2 ? 1.0f : 0.0f, look[0] = look[1] = 0.0f; // the same walk (or rest) every run
        if (ed->bp_test == 1 && ed->bp_test_house) move[0] = fmod(ed->play_time, 2.0) < 1.0 ? 1.0f : -1.0f; // to and fro
        for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST && k < 500; k++) {
            uint8_t d = glfwGetKey(win, k) == GLFW_PRESS;
            if (d != s_down[k]) bp_key_event(&ed->world, k, d);
            s_down[k] = d;
        }
        for (int b = 0; b < 2; b++) { // Left / Right Mouse Button: 500, 501
            uint8_t d = glfwGetMouseButton(win, b) == GLFW_PRESS;
            if (d != s_down[500 + b]) bp_key_event(&ed->world, 500 + b, d);
            s_down[500 + b] = d;
        }
        if (ed->bp_test == 1 && !ed->bp_test_house) bp_key_event(&ed->world, 'W', true), s_down['W'] = 0; // (W held: Enhanced Input's test)
    } else
        memset(s_down, 0, sizeof s_down);
    bp_set_input(move, look);
}
// (2026-11-23) The editor has the focus: its main window or any window ImGui made for a panel undocked out of it (a
// Material or Blueprint editor dragged to the other monitor). The main window alone made "Use Less CPU when in
// Background" sleep 30 ms a frame while one worked in an undocked graph: nodes and links dragged at ~30 fps, behind
static bool editor_any_window_focused(const Editor* ed) {
    if (glfwGetWindowAttrib(ed->window, GLFW_FOCUSED)) return true;
    ImGuiPlatformIO* pio = igGetPlatformIO_Nil();
    for (int i = 0; pio && i < pio->Viewports.Size; i++) {
        GLFWwindow* w = (GLFWwindow*)pio->Viewports.Data[i]->PlatformHandle;
        if (w && w != ed->window && glfwGetWindowAttrib(w, GLFW_FOCUSED)) return true;
    }
    return false;
}


void editor_frame(Editor* ed, double dt, double now) {
    ImGuiIO* io = igGetIO_Nil();
    extern const char* g_re_search_hint; // (imgui_hooks.c: every combo's search field, in the editor's language)
    g_re_search_hint = TR("Search...");
    editor_sync_selection(ed);
    async_update(ed->playing ? 2.0 : 6.0); // the finished background work put in place; the steps' slices (async.h)
    navmesh_set_wanted(ed->show_nav);
    navmesh_update(&ed->world, ed->playing, now); // the NavMesh follows the level (its tiles built in the background)
    // The mouse belongs to the camera while flying or possessed (the cursor is captured)
    if (ed->flying || (ed->playing && ed->possessed && !ed->mouse_released)) io->ConfigFlags |= ImGuiConfigFlags_NoMouse;
    else io->ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

    if (ed->show_browser && !ed->playing) { // the Project Browser covers the editor: nothing else drawn nor rendered
        ed->render_view.w = ed->render_view.h = 0;
        project_ui_frame(ed, now);
        services_frame(ed);
        async_ui_draw(now);
        update_title(ed);
        return;
    }
    shortcuts(ed, now);
    if (ed->playing) {
        pause_menu_poll_gamepad(&ed->world); // (a gamepad's Start opens the pause menu, B goes back)
        if (game_quit_requested()) { // Quit Game: the Play stops
            game_quit_clear();
            editor_stop(ed, now);
        }
        // the pause menu closed: the mouse back to the camera when it was the camera's
        static bool s_menu;
        if (s_menu && !pause_menu_is_open() && !ui_mouse_shown() && ed->possessed && ed->mouse_released) {
            ed->mouse_released = false;
            glfwSetInputMode(ed->window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            glfwGetCursorPos(ed->window, &ed->last_cursor[0], &ed->last_cursor[1]);
        }
        s_menu = pause_menu_is_open();
    }
    // the game paused (Set Game Paused, the pause menu: game_settings.h): the world waits; the widgets, the UI sounds and the
    // actors with Tick When Paused go on (bp_tick). The editor's Pause button stops everything, as before
    const bool loading = ed->playing && rtl_blocking(); // (Q16) a Build Lighting (Async) in Loading Screen mode: the world waits
    const bool frozen = ed->playing && (game_paused() || loading);
    if (ed->playing && !ed->paused && bp_debug_paused()) // the Blueprint debugger: the world waits at a breakpoint; Resume / Step go on in bp_tick
        bp_tick(&ed->world, 0.0f);
    else if (ed->playing && !ed->paused) {
        double pz0 = re_time_seconds();
        if (!frozen) ed->play_time += dt;
        TRACE("Online (receive)", online_pre_tick(&ed->world, now)); // the network: what came (states, RPCs, players), then their events
        bp_online_events(&ed->world);
        if (!frozen) TRACE("Systems (ECS)", systems_update(&ed->world, (float)(dt > 0.1 ? 0.1 : dt)));
        pawn_input(ed);
        if (ed->test_step) ed->test_step(ed, dt > 0.1 ? 0.1 : dt);
        if (!frozen) TRACE("AI", ai_update(&ed->world, (float)(dt > 0.1 ? 0.1 : dt))); // the AIs set their pawns' input before they move
        if (!loading) TRACE("Blueprints", bp_tick(&ed->world, (float)(dt > 0.1 ? 0.1 : dt)));
        const bool dbg = bp_debug_paused(); // (a breakpoint stopped the tick halfway: the rest of the world waits with it)
        if (!frozen && !dbg) {
            TRACE("Physics", physics_update(&ed->world, (float)(dt > 0.1 ? 0.1 : dt))); // after the Blueprints: their impulses count this frame
            TRACE("Vehicles", vehicle_update(&ed->world, (float)(dt > 0.1 ? 0.1 : dt))); // the vehicles (after the physics: the Blueprints' inputs count)
        }
        TRACE("Online (send)", online_post_tick(&ed->world, now)); // the others' actors where the server had them, then what this machine sends
        if (!frozen && !dbg) {
            anim_set_view_point(ed->render_view.pos); // (the Look At nodes turn toward the camera)
            TRACE("Animation", anim_update(&ed->world, (float)(dt > 0.1 ? 0.1 : dt), true)); // the poses (after the Characters moved)
            TRACE("Crowds", crowd_update(&ed->world, (float)(dt > 0.1 ? 0.1 : dt), true, ed->render_view.pos)); // the crowds think and move
            panic_tick(ed, dt); // (Q27.1: --scene panicN)
            TRACE("Traffic", traffic_update(&ed->world, (float)(dt > 0.1 ? 0.1 : dt), ed->render_view.pos)); // (Q27.2) the roads' cars
        }
        g_pie_tick_ms = (re_time_seconds() - pz0) * 1000.0;
        level_stream_update(&ed->world, true); // the streamed levels read meanwhile come in (Load / Unload Stream Level)
        level_preload_update(); // (2026-11-23) the preloads' component trees, within the speed's budget
        TRACE("World Partition", wp_update(&ed->world, ed->render_view.pos, true)); // (B1) the World Partition's cells around the camera
        if (level_open_pending()) {
            char go[700];
            snprintf(go, sizeof go, "%s", level_open_pending());
            level_open_clear();
            pie_open_level(ed, go);
        }
    }
    editor_construct_update(ed); // the Blueprints' Construction Scripts on what changed (sugestÃ£o #64: shown, not kept)
    if (!ed->playing) vehicle_editor_update(&ed->world); // (the rails' meshes when their points changed)
    if (!ed->playing) bp_components_update(&ed->world, (float)dt, false); // the Blueprints' components on their actors
    igGetStyle()->FontScaleMain = ed->ui_scale * TMV(TM_FONT_SIZE) / 15.0f; // (Editor Preferences > General; the theme's text size)
    // Auto Save (Editor Preferences > General): the level, when it has a file and changed, every N minutes
    if (ed->autosave_minutes > 0 && !ed->playing && ed->dirty && ed->level_path[0]) {
        if (ed->autosave_at <= 0.0) ed->autosave_at = now + ed->autosave_minutes * 60.0;
        else if (now >= ed->autosave_at) {
            editor_save_level(ed, ed->level_path, now);
            ed->autosave_at = 0.0;
        }
    }
    // Use Less CPU when in Background, and the frame rate limit (Editor Preferences > Performance); not while measuring:
    // a --bench whose window lost the focus to another one slept 30 ms a frame (29 fps, "CPU 35 ms": preview_anim_bp)
    if (ed->measuring) {
    } else if (ed->less_cpu_in_background && !editor_any_window_focused(ed) && !ed->playing) re_sleep_seconds(0.03);
    else if (ed->frame_limit > 0 && dt < 1.0 / ed->frame_limit) re_sleep_seconds(1.0 / ed->frame_limit - dt);
    anim_set_editor_realtime(ed->realtime && !ed->playing);
    if (!ed->playing) anim_update(&ed->world, (float)dt, false); // the skeletal meshes' parts at the bind pose (Realtime: their animations)
    if (!ed->playing) crowd_update(&ed->world, (float)dt, false, ed->render_view.pos); // the crowds where they start
    TRACE("Plugins", plugins_tick(frozen || (ed->playing && ed->paused) ? 0.0f : (float)dt, ed->playing)); // (B14c) the plugins' ticks
    TRACE("Splines", spline_update(&ed->world, frozen || (ed->playing && (ed->paused || bp_debug_paused())) ? 0.0f : (float)dt, ed->playing)); // (Q14) the splines' pieces, the cables
    const bool still = frozen || (ed->playing && ed->paused) || (!ed->playing && !ed->realtime); // (Realtime off: the editor holds still)
    TRACE("Particles", particles_update(&ed->world, still ? 0.0f : (float)dt, ed->playing)); // the particle systems' clocks (the editor plays them too, looping; paused: they wait)
    decals_update(&ed->world, still ? 0.0f : (float)dt, ed->playing); // the decals' life spans (M6)
    landscape_update(&ed->world, ed->vk); // the landscapes' chunks (written again after a sculpt or an import)
    { // the procedural world around the player (playing) or the camera (the editor's preview)
        static Editor* s_pg;
        if (s_pg != ed) s_pg = ed, g_pg_editor = ed, procgen_set_camera_hook(pg_cameras);
        float focus[3];
        Entity pl = ed->playing ? bp_player(&ed->world) : ENTITY_NONE;
        const CTransform* pt = pl ? ecs_get(&ed->world, pl, COMP_TRANSFORM) : NULL;
        if (pt) memcpy(focus, pt->location, 12);
        else memcpy(focus, ed->playing && ed->possessed ? ed->pie_cam.pos : ed->cam.pos, 12);
        procgen_update(&ed->world, focus, ed->playing);
        TRACE("PCG", pcg_update(&ed->world, focus, ed->playing)); // (Q15: the PCG components, generated and streamed)
    }
    foliage_update(&ed->world);           // the painted plants' entities
    proto_update(&ed->world); // the prototyping shapes whose numbers changed

    main_menu(ed, now);
    ImGuiViewport* vp = igGetMainViewport();
    float menu_h = look_menu_bar_height(); // (Q26: taller than a frame, the brand's)
    float x0 = vp->Pos.x, y0 = vp->Pos.y + menu_h, W = vp->Size.x, H = vp->Size.y - menu_h;

    igSetNextWindowPos((ImVec2){ x0, y0 }, ImGuiCond_Always, (ImVec2){ 0, 0 });
    igSetNextWindowSize((ImVec2){ W, H }, ImGuiCond_Always);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){ 0, 0 });
    igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 0.0f);
    // No background: the scene (render.c) is drawn under the UI where the viewport is
    igBegin("##editor", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse |
                              ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground);
    igPopStyleVar(2);

    const float pad = 6.0f;
    float bar_h = igGetFrameHeight() + pad * 2.0f;
    float tb_h = igGetFontSize() + 10.0f + pad * 2.0f; // (the toolbar's taller buttons: FramePadding 5, toolbar())
    ImGuiChildFlags pad_child = ImGuiChildFlags_AlwaysUseWindowPadding;

    igSetCursorScreenPos((ImVec2){ x0, y0 });
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){ pad, pad });
    igPushStyleColor_Vec4(ImGuiCol_ChildBg, THV(TK_BG_APP));
    if (igBeginChild_Str("toolbar", (ImVec2){ W, tb_h }, pad_child, ImGuiWindowFlags_NoScrollbar)) toolbar(ed, now);
    igEndChild();
    igPopStyleColor(1);
    igPopStyleVar(1);

    // Q2: the panels are windows docked around the viewport (editor_dock.c): dragged anywhere, another monitor too;
    // the layout's default, or the one saved (Window > Layout), the last one back on open
    float top = y0 + tb_h, bottom = y0 + H - bar_h, vh = bottom - top;
    float gap = TMV(TM_SPACE_CARD);
    dock_host(ed, x0 + gap, top + gap, W - 2.0f * gap, vh - 2.0f * gap);

    igSetCursorScreenPos((ImVec2){ x0, bottom });
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){ pad, pad });
    igPushStyleColor_Vec4(ImGuiCol_ChildBg, THV(TK_BG_APP));
    if (igBeginChild_Str("status", (ImVec2){ W, bar_h }, pad_child, ImGuiWindowFlags_NoScrollbar)) status_bar(ed, now);
    igEndChild();
    igPopStyleColor(1);
    igPopStyleVar(1);
    igEnd();

    // the viewport: in the middle, the scene drawn under it (render.c) where its window is
    float xv = x0, vw = W, vph = vh;
    if (dock_panel_begin(DP_VIEWPORT, TR("Viewport"), NULL, 0)) {
        ImVec2 vp0 = igGetCursorScreenPos(), vs = igGetContentRegionAvail();
        xv = vp0.x, vw = fmaxf(vs.x, 1.0f), vph = fmaxf(vs.y, 1.0f);
        viewport_draw(ed, xv, vp0.y, vw, vph, dt);
        if (!(ed->playing && ed->possessed)) viewport_toolbar(ed, xv, vp0.y, vw);
    }
    dock_panel_end();
    // (Q26) the left: Modes (Selection, Landscape, Foliage and the mode's tools) beside Place Actors; entering a mode
    // brings Modes to the front (Unreal's Landscape and Foliage modes; it opens when it was closed)
    bool mode_on = ed->land_tool || ed->foliage_mode;
    static bool s_mode_was;
    if (mode_on && !s_mode_was) *look_modes_flag() = true, igSetWindowFocus_Str(dock_panel_id(DP_MODES));
    s_mode_was = mode_on;
    if (*look_modes_flag()) {
        if (dock_panel_begin(DP_MODES, TR("Modes"), look_modes_flag(), 0)) modes_panel(ed, now);
        dock_panel_end();
    }
    if (ed->show_place) {
        if (dock_panel_begin(DP_PLACE, TR("Place Actors"), &ed->show_place, ImGuiWindowFlags_NoScrollbar)) place_actors(ed, now);
        dock_panel_end();
    }

    if (ed->show_outliner) {
        if (dock_panel_begin(DP_OUTLINER, TR("Outliner"), &ed->show_outliner, ImGuiWindowFlags_NoScrollbar)) outliner(ed, now);
        dock_panel_end();
    }
    if (ed->show_details) {
        if (dock_panel_begin(DP_DETAILS, TR("Details"), &ed->show_details, 0)) details(ed);
        dock_panel_end();
    }

    // Content Browser: docked (a panel of the layout), or the drawer over the bottom (focused when it opens, hidden when
    // the focus goes elsewhere, like Unreal's)
    bool toggle_dock = false;
    if (ed->content_docked) {
        if (!content_draw(0, 0, -1.0f, -1.0f, true, &toggle_dock)) ed->content_docked = false;
    } else if (ed->drawer_open) {
        float dh = floorf(vh * ed->drawer_frac);
        if (!s_drawer_shown) igSetNextWindowFocus();
        ed->drawer_open = content_draw(x0, bottom - dh, W, dh, false, &toggle_dock);
        ed->drawer_frac = fminf(fmaxf(ed->drawer_frac - content_drawer_drag() / fmaxf(vh, 1.0f), 0.15f), 0.85f); // (its top edge dragged)
        if (s_drawer_shown && !content_focused() && !igIsMouseDragging(ImGuiMouseButton_Left, -1.0f)) ed->drawer_open = false;
    }
    s_drawer_shown = ed->drawer_open && !ed->content_docked;
    if (toggle_dock) {
        ed->content_docked = !ed->content_docked;
        ed->drawer_open = false;
    }
    content_import_options(); // (a drop or Import: its options, wherever the drawer is)
    asset_picker_draw();      // (Q6) the asset picker, when a field or a creation opened it
    output_log(ed);
    if (*look_platforms_flag()) { // (Q26) Platform Support: each target's readiness, the devices
        if (dock_panel_begin(DP_PLATFORMS, TR("Platform Support"), look_platforms_flag(), 0)) look_platforms_panel(ed, now);
        dock_panel_end();
    }
    look_frame(ed, now); // (the title bar's color, the caches' readouts, the new look's offer)
    (void)xv, (void)vph;
    view2_window(ed, dt); // Window > Viewport 2
    if (g_prefs_dirty) {
        editor_save_prefs(ed);
        g_prefs_dirty = false;
    }
    services_frame(ed);
    if (ed->playing) game_bake_tick(ed); // (Q16) a Build Lighting a Blueprint asked for while playing
    bake_tick(ed, now);
    bp_editor_draw(ed, now);
    mat_editor_draw(ed, now);
    particle_editor_draw(ed);
    cloth_panel(ed);
    levels_frame(ed, now); // Window > Levels (after bake_tick: a cancelled bake puts the lights back)
    cinematics_window(ed, &g_show_cine); // (B9)
    sc_window(&g_show_submit); // (B14: File > Source Control > Submit Content)
    plugins_window(&g_show_plugins); // (B14c: Edit > Plugins)
    { // (B14b) Tools > Trace: the recordings in <project>/Saved/Profiling
        static char dir[900];
        if (!dir[0]) {
            char pr[800];
            project_root(pr, sizeof pr);
            snprintf(dir, sizeof dir, "%s/Saved/Profiling", pr);
        }
        trace_window(&g_show_trace, dir);
    }
    imprep_window(); // (Q19) Window > Import Report
    { // (B14) the Content Browser's Git states: read again every 5 s (a thread: never a wait)
        static double last;
        if (sc_active() && now - last > 5.0) last = now, sc_refresh();
    }
    if (ed->playing) take_record_tick(ed, dt); // (a take records while playing)
    ikrig_editor_draw(ed);
    vehicle_editor_draw(ed);
    dlg_editor_draw(ed); // (dialogue_editor.c)
    quest_editor_draw(ed); // (quest_editor.c)
    physmat_editor_draw(); // (Q8, physmat_editor.c)
    camshake_editor_draw(); // (Q12, camshake_editor.c)
    pcg_editor_draw(); // (Q15, pcg_editor.c)
    asset_viewer_draw();
    anim_editor_draw(ed, now);
    ai_editor_draw(ed, now);
    seq_editor_draw(ed, now, (float)dt); // the Sequencer (a Level Sequence opened)
    input_editor_draw();
    audio_editor_draw(ed, now, (float)dt); // a sound asset opened
    script_hot_reload(ed->playing); // a script saved (in the IDE or another editor): compiled again
    script_ide_draw(ed, now, (float)dt); // the Code Editor, the API reference, the Editor Utilities
    assistant_ui_draw(); // Help > Assistant (and its "Show me where" outlines)
    mcp_ui_draw(ed); // Tools > AI Connection (MCP)
    // the listener: the view (the player's camera while playing); the game's sounds paused with it
    audio_pause_game(ed->playing && (ed->paused || game_paused()));
    TRACE("Audio", audio_world_update(&ed->world, (float)dt, ed->playing, ed->render_view.pos, ed->render_view.fwd, ed->render_view.up));
    project_ui_frame(ed, now); // Project Settings, Editor Preferences
    // the Debug Viewer (F10): over everything; it measures only while open (or while the object cost view is on)
    DvContext dc = { &ed->world, true, ed->playing, ed->stats.frame_ms, ed->stats.cpu_ms, ed->stats.gpu_ms,
                     { 0 }, { 0 }, ed->render_view.fov_deg, ed->render_view.w, ed->render_view.h };
    memcpy(dc.camera, ed->render_view.pos, sizeof dc.camera);
    memcpy(dc.fwd, ed->render_view.fwd, sizeof dc.fwd);
    editor_cost_frame(ed, &dc); // View Mode > Object Cost: the colors' values and the panel (sugestÃ£o #65)
    if (dv_open()) {
        Entity picked = dv_frame(&dc);
        if (picked != ENTITY_NONE && ecs_alive(&ed->world, picked)) editor_select(ed, picked, SEL_REPLACE);
    } else {
        dv_frame(NULL); // closing it turns its measures off
    }
    dock_layout_popups(ed); // (Q2: Window > Layout > Save Layout As)
    dock_frame_end(ed); // (Q2: the docking targets while a window is dragged, the gaps between panels)
    async_ui_draw(now); // the background tasks' cards (bottom right, over the panels)
    about_popup(ed);
    open_asset_dialog(ed);
    quit_popup(ed, now);
    update_title(ed);
}

// (2026-11-24, the MCP server: mcp_tools.c) the Add menus' catalog and the Outliner's class names by code
int editor_place_count(void) {
    place_catalog();
    return g_nplace;
}
const char* editor_place_name(int i, const char** category) {
    place_catalog();
    if (i < 0 || i >= g_nplace) return "";
    if (category) *category = g_place[i].group ? g_place[i].group : g_place_cat_names[g_place[i].cat];
    return g_place[i].name;
}
bool editor_place_is_preset(int i) { // (a Sky Preset changes the sky's actors: nothing new to return)
    place_catalog();
    return i >= 0 && i < g_nplace && g_place[i].spawn == PX_SKY;
}
Entity editor_place_spawn(Editor* ed, int i, double now) {
    place_catalog();
    if (i < 0 || i >= g_nplace || ed->playing) return ENTITY_NONE;
    Entity before = ed->selected;
    place_item(ed, i, now);
    return ed->selected != before ? ed->selected : ENTITY_NONE;
}
const char* editor_actor_type(World* w, Entity e) { return type_label(w, e); }
