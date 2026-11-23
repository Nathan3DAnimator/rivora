// (2026-11-23) Emissive materials light the scene (lights.c lights_emissive_light, notas-dev/luz.md "Material emissivo
// ilumina"): --scene emit: a closed room of 8 x 8 x 3 m with no light at all and a sphere of 40 cm whose material glows
// orange (Emission Strength 20: 2000 nits); --scene emit_off: the same with Emissive Light 0. The shot (--shot) is read
// back: the floor around the sphere has to be lit (before: black, a material's glow lit nothing)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "emissive_test.h"
#include "lights.h"
#include "material.h"
#include "platform.h"
#include "content.h"
#include "platform_files.h"
#include "render.h"

uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h); // mat_image.c

static int g_scene; // 1 emit, 2 emit_off

static int sock_in(int type, const char* name) {
    const MatNodeDef* d = mat_node_def(type);
    for (int i = 0; d && i < d->nin; i++)
        if (!strcmp(d->in[i].name, name)) return i;
    return -1;
}

static Entity box(World* w, const char* name, int shape, float x, float y, float z, float sx, float sy, float sz, uint32_t mat) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    if (!e) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = z, t->flags = MOBILITY_STATIC;
    t->scale[0] = sx, t->scale[1] = sy, t->scale[2] = sz;
    CStaticMesh* m = ecs_get(w, e, COMP_STATIC_MESH);
    m->mesh = (uint32_t)shape, m->material = mat, m->uv_scale = 1.0f, m->roughness = 0.6f, m->cast_shadow = 1;
    m->base_color[0] = m->base_color[1] = m->base_color[2] = 0.7f;
    return e;
}

bool emissive_test_scene(Editor* ed, const char* name) {
    int sc = !strcmp(name, "emit") ? 1 : !strcmp(name, "emit_off") ? 2 : 0;
    if (!sc) return false;
    g_scene = sc;
    char dir[700], path[800];
    snprintf(dir, sizeof dir, "%s\\EmitTest", content_root());
    re_make_dirs(dir);
    snprintf(path, sizeof path, "%s\\M_EmitTest%s.rmat", dir, sc == 2 ? "_Off" : "");
    Material* m = mat_load(path);
    if (!m) m = mat_create(path, false, NULL);
    if (m && m->graph) {
        for (int i = 0; i < m->graph->node_count; i++) {
            MatNode* n = &m->graph->nodes[i];
            if (n->type != NODE_BSDF_PRINCIPLED) continue;
            int bc = sock_in(n->type, "Base Color"), ec = sock_in(n->type, "Emission Color"), es = sock_in(n->type, "Emission Strength");
            if (bc >= 0) n->in[bc][0] = n->in[bc][1] = n->in[bc][2] = 0.05f;
            if (ec >= 0) n->in[ec][0] = 1.0f, n->in[ec][1] = 0.55f, n->in[ec][2] = 0.2f;
            if (es >= 0) n->in[es][0] = 20.0f;
        }
        m->emissive_light = sc == 2 ? 0.0f : 1.0f;
        m->emissive_shadows = sc == 1; // (the shadowed path: by ray, the many lights' sampling)
        mat_save(m);
        mat_mark_dirty(m);
    }
    mat_bake_pending(NULL);
    mat_update(ed->vk, VK_NULL_HANDLE);
    World* w = &ed->world;
    ecs_shutdown(w);
    ecs_init(w);
    ed->selected = ENTITY_NONE;
    ed->game_view = true; // (no icons or grid over the shot)
    // the room (cubes of 100 cm scaled): floor, ceiling, four walls; nothing lights it but the sphere
    box(w, "Floor", SHAPE_CUBE, 0, 0, -5, 8, 8, 0.1f, 0);
    box(w, "Ceiling", SHAPE_CUBE, 0, 0, 305, 8, 8, 0.1f, 0);
    box(w, "WallN", SHAPE_CUBE, 0, 405, 150, 8.2f, 0.1f, 3.2f, 0);
    box(w, "WallS", SHAPE_CUBE, 0, -405, 150, 8.2f, 0.1f, 3.2f, 0);
    box(w, "WallE", SHAPE_CUBE, 405, 0, 150, 0.1f, 8.2f, 3.2f, 0);
    box(w, "WallW", SHAPE_CUBE, -405, 0, 150, 0.1f, 8.2f, 3.2f, 0);
    Entity s = box(w, "GlowSphere", SHAPE_SPHERE, 0, 0, 60, 0.4f, 0.4f, 0.4f, m ? m->hash : 0);
    (void)s;
    ed->cam.pos[0] = -330.0f, ed->cam.pos[1] = -330.0f, ed->cam.pos[2] = 230.0f, ed->cam.yaw = 45.0f, ed->cam.pitch = -28.0f;
    printf("[RealEngine] emit scene %s: the sphere's material %s (Emissive Light %.0f)\n", name, m ? "ready" : "FAIL: not made",
           m ? m->emissive_light : 0.0f);
    return true;
}

// The shot's floor (the viewport's lower half, its mean luma) and the emissive lights sent
bool emissive_test_check(Editor* ed, const char* shot) {
    if (!g_scene) return true;
    uint32_t w = 0, h = 0;
    uint8_t* px = shot ? mat_load_image_rgba(shot, &w, &h) : NULL;
    if (!px) {
        printf("[RealEngine] FAIL: emit: no shot to read (--shot)\n");
        return false;
    }
    const RenderView* rv = &ed->render_view;
    uint32_t x0 = (uint32_t)rv->x, y0 = (uint32_t)rv->y, vw = (uint32_t)rv->w, vh = (uint32_t)rv->h;
    if (!vw || !vh || x0 + vw > w || y0 + vh > h) x0 = y0 = 0, vw = w, vh = h;
    double sum = 0.0;
    uint64_t n = 0;
    for (uint32_t y = y0 + vh * 6 / 10; y < y0 + vh * 95 / 100; y++)
        for (uint32_t x = x0 + vw / 10; x < x0 + vw * 9 / 10; x++) {
            const uint8_t* a = &px[((size_t)y * w + x) * 4];
            sum += 0.2126 * a[0] + 0.7152 * a[1] + 0.0722 * a[2], n++;
        }
    free(px);
    double floor = n ? sum / (double)n : 0.0;
    uint32_t locals = lights_local_count();
    printf("[RealEngine] emit: the floor's mean luma %.1f (0-255), %u local light(s) (the sphere's: %s)\n", floor, locals,
           g_scene == 1 ? "1 expected" : "none expected");
    bool ok = true;
    if (g_scene == 1) {
        if (locals != 1) printf("[RealEngine] FAIL: emit: the glowing sphere didn't become a light\n"), ok = false;
        if (floor < 12.0) printf("[RealEngine] FAIL: emit: the floor around the glowing sphere is dark (%.1f)\n", floor), ok = false;
    } else if (locals != 0) printf("[RealEngine] FAIL: emit_off: Emissive Light 0 still made a light\n"), ok = false;
    printf("[RealEngine] emit: %s\n", ok ? "ok" : "FAILED");
    return ok;
}
