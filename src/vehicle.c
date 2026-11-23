// Vehicles (vehicle.h, notas-dev/veiculos.md): the assets, their templates, the actors made from them; the simulation
// is in vehicle_sim.inc
#include "vehicle.h"

#include <ctype.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "components.h"
#include "io_json.h"
#include "landscape.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "water.h"
#include "river.h"
#include "spline.h"

float bp_trace_boxes(const float a[3], const float b[3], float radius); // (bp_vm.c: the level's boxes and landscapes)
uint32_t bp_world_boxes(const float mn[3], const float mx[3], float (*out)[6], uint32_t cap);
void bp_set_player(World* w, Entity e);
void bp_get_input(float move[2], float look[2]);
void bp_components_place_actor(World* w, Entity actor);

#define MAX_ASSETS 128

const char* const g_veh_kind_names[VK_KIND_COUNT] = { "Car", "Motorcycle", "Truck", "Tank", "Boat", "Train", "Airplane", "Helicopter" };
const char* const g_veh_input_names[VI_COUNT] = { "None", "Throttle", "Steer", "Brake", "Handbrake", "Pitch (nose up)", "Roll (right)", "Yaw (right)", "Collective",
                                                  "Action 1 (F)", "Action 2 (G)", "Action 3 (H)", "Action 4 (J)" };
const char* const g_veh_template_names[VT_COUNT] = { "Car", "Sports Car", "Truck 6x6", "Motorcycle", "Tank", "Boat", "Locomotive", "Train Wagon",
                                                     "Airplane", "Helicopter" };

static const VehPartInfo g_parts[VP_KIND_COUNT] = {
    [VP_WHEEL] = { "Wheel", "A wheel on a raycast suspension: its spring and damper hold the chassis, its tire grips by a magic formula (slip and slip angle). Drive: its share of the engine; Max Steer: how far it turns.",
                   10, { "Radius (cm)", "Width (cm)", "Spring (N/cm)", "Damper (N s/cm)", "Suspension (cm)", "Max Steer (deg)", "Drive", "Brake (N m)", "Handbrake (N m)", "Grip" },
                   { 35, 22, 300, 30, 25, 0, 0, 1500, 0, 1 }, { 5, 2, 1, 0, 1, -80, 0, 0, 0, 0.1f }, { 300, 200, 20000, 5000, 300, 80, 1, 50000, 50000, 3 } },
    [VP_ENGINE] = { "Engine", "The engine: its torque curve peaks at Peak RPM; the clutch slips below idle so it pulls away; the limiter cuts at Max RPM.",
                    6, { "Max Torque (N m)", "Peak RPM", "Max RPM", "Idle RPM", "Engine Brake", "Inertia" },
                    { 400, 4500, 7000, 900, 0.15f, 0.25f }, { 10, 500, 1000, 100, 0, 0.01f }, { 20000, 20000, 25000, 3000, 1, 10 } },
    [VP_GEARBOX] = { "Gearbox", "The gears (the asset's ratios) and the final drive; automatic: up and down by the engine's RPM.",
                     5, { "Final Drive", "Automatic", "Shift Up RPM", "Shift Down RPM", "Shift Time (s)" },
                     { 3.7f, 1, 6000, 2500, 0.25f }, { 0.5f, 0, 500, 300, 0 }, { 20, 1, 25000, 20000, 2 } },
    [VP_DIFFERENTIAL] = { "Differential", "How the drive splits between the wheels: open (each its own), locked (together), limited slip (between).",
                          2, { "Type (0 open, 1 locked, 2 limited)", "Limited Slip" }, { 2, 0.4f }, { 0, 0 }, { 2, 1 } },
    [VP_HINGE] = { "Hinge", "A part that swings on its axis (a door, a hood, a trunk, a ramp, a gun): opened by its input (F, G, H, J) or a Blueprint (Open Part, Set Part Position).",
                   4, { "Min (deg)", "Max (deg)", "Speed (deg/s)", "Starts Open" }, { 0, 70, 120, 0 }, { -180, -180, 1, 0 }, { 180, 180, 2000, 1 } },
    [VP_SLIDER] = { "Slider", "A part that slides along its axis (a sliding door, a crane's arm, a forklift).",
                    4, { "Min (cm)", "Max (cm)", "Speed (cm/s)", "Starts Open" }, { 0, 100, 50, 0 }, { -2000, -2000, 1, 0 }, { 2000, 2000, 5000, 1 } },
    [VP_PISTON] = { "Piston", "A piston: stretches between its place and a linked part (a shock absorber on a wheel, a hydraulic arm); without a link it extends along its axis by its input.",
                    4, { "Base Length (cm)", "Min (cm)", "Max (cm)", "Speed (cm/s)" }, { 50, 0, 60, 40 }, { 1, -1000, -1000, 1 }, { 1000, 1000, 1000, 5000 } },
    [VP_GEAR] = { "Gear", "A part turning with another: Link 'Engine' (a crankshaft, a fan), 'Steer' (the steering wheel), a wheel or a part's name; or on its own at Speed.",
                  2, { "Ratio", "Speed (deg/s)" }, { 1, 0 }, { -100, -5000 }, { 100, 5000 } },
    [VP_PROPELLER] = { "Propeller", "Thrust along its axis by the throttle (the brake: reverse). In Water: a boat's screw (pushes only under water).",
                       4, { "Max Thrust (N)", "Reverse", "Spin (rpm)", "In Water" }, { 6000, 0.5f, 1500, 1 }, { 0, 0, 0, 0 }, { 1000000, 1, 10000, 1 } },
    [VP_WING] = { "Wing", "Lift and drag from the air (or the water: a rudder, a hydrofoil) by its angle, stalling past Stall; a control surface turns it by its input (Pitch, Roll, Yaw).",
                  7, { "Area (m2)", "Lift Slope", "Stall (deg)", "Drag", "Control (deg)", "Aspect Ratio", "In Water" },
                  { 8, 5, 15, 0.03f, 0, 6, 0 }, { 0.01f, 0, 1, 0, 0, 1, 0 }, { 500, 10, 45, 1, 45, 30, 1 } },
    [VP_ROTOR] = { "Rotor", "A helicopter's rotor: lift by the collective, tilted by the cyclic (pitch, roll); the tail's torque turns it (yaw). Stability holds it level and hovering when let go.",
                   5, { "Max Lift (N)", "Cyclic (deg)", "Tail Torque (N m)", "Stability", "Spin (rpm)" }, { 30000, 8, 6000, 0.6f, 400 }, { 0, 0, 0, 0, 0 }, { 10000000, 30, 1000000, 1, 3000 } },
    [VP_FLOAT] = { "Pontoon", "A floating point: buoyancy by how deep it sits in a Water Body, and the water's drag (a hull's corners, a pontoon).",
                   2, { "Radius (cm)", "Drag" }, { 50, 1 }, { 1, 0 }, { 1000, 20 } },
    [VP_BOGIE] = { "Bogie", "A train's truck: sits on the rail at its place along the car (the car turns between its bogies); its wheels spin.",
                   1, { "Wheel Radius (cm)" }, { 46 }, { 5 }, { 200 } },
};
const VehPartInfo* veh_part_info(int kind) { return kind >= 0 && kind < VP_KIND_COUNT ? &g_parts[kind] : &g_parts[0]; }
static const char* const g_part_ids[VP_KIND_COUNT] = { "wheel", "engine", "gearbox", "differential", "hinge", "slider", "piston", "gear", "propeller", "wing", "rotor", "float", "bogie" };

// --- Assets ---
static struct {
    char root[600];
    bool scanned;
    VehicleAsset* a[MAX_ASSETS];
    uint32_t n;
} VA;

static uint32_t vpath_hash(const char* vpath) {
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) h = (h ^ (uint8_t)tolower((unsigned char)*c)) * 16777619u;
    return h ? h : 1u;
}
static void disk_of(const char* vpath, char* out, size_t cap) {
    if (!strncmp(vpath, "/Content", 8)) snprintf(out, cap, "%s%s", VA.root, vpath + 8);
    else snprintf(out, cap, "%s", vpath);
}
static void vpath_of(const char* disk, char* out, size_t cap) {
    size_t rl = strlen(VA.root);
    if (rl && !strncmp(disk, VA.root, rl)) snprintf(out, cap, "/Content%s", disk + rl);
    else snprintf(out, cap, "%s", disk);
}
void vehicle_set_root(const char* content_root) {
    snprintf(VA.root, sizeof VA.root, "%s", content_root ? content_root : "");
    for (char* c = VA.root; *c; c++)
        if (*c == '\\') *c = '/';
    VA.scanned = false;
}
static void scan_dir(const char* disk, int depth);
static void scan_entry(void* user, const ReDirEntry* e) {
    char path[800], vpath[800];
    snprintf(path, sizeof path, "%s/%s", (const char*)user, e->name);
    if (e->is_dir) {
        scan_dir(path, 1);
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (!dot || strcmp(dot, ".rvehicle")) return;
    vpath_of(path, vpath, sizeof vpath);
    veh_load(vpath);
}
static void scan_dir(const char* disk, int depth) {
    if (depth <= 12) re_dir_walk(disk, scan_entry, (void*)disk);
}
static void scan(void) {
    if (VA.scanned || !VA.root[0]) return;
    VA.scanned = true;
    scan_dir(VA.root, 0);
}
uint32_t veh_count(void) { scan(); return VA.n; }
VehicleAsset* veh_at(uint32_t i) { return i < VA.n ? VA.a[i] : NULL; }
VehicleAsset* veh_find(uint32_t hash) {
    scan();
    for (uint32_t i = 0; i < VA.n; i++)
        if (VA.a[i]->hash == hash) return VA.a[i];
    return NULL;
}

VehicleAsset* veh_load(const char* vpath) {
    uint32_t h = vpath_hash(vpath);
    for (uint32_t i = 0; i < VA.n; i++)
        if (VA.a[i]->hash == h) return VA.a[i];
    if (VA.n >= MAX_ASSETS) return NULL;
    char path[800];
    disk_of(vpath, path, sizeof path);
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = l > 0 ? malloc((size_t)l + 1) : NULL; // (a load)
    bool rd = t && fread(t, 1, (size_t)l, f) == (size_t)l;
    fclose(f);
    if (!rd) return free(t), NULL;
    t[l] = 0;
    JsonDoc doc;
    char err[160];
    VehicleAsset* a = NULL;
    if (json_parse(t, (size_t)l, &doc, err, sizeof err) && (a = calloc(1, sizeof *a))) { // (an asset)
        const JsonValue* r = &doc.root;
        snprintf(a->path, sizeof a->path, "%s", path);
        const char* s = strrchr(path, '/');
        snprintf(a->name, sizeof a->name, "%s", s ? s + 1 : path);
        char* d = strrchr(a->name, '.');
        if (d) *d = 0;
        a->hash = h, a->version = 1;
        const char* kind = json_get_str(r, "kind", "Car");
        for (int k = 0; k < VK_KIND_COUNT; k++)
            if (!strcmp(kind, g_veh_kind_names[k])) a->kind = (uint8_t)k;
        a->mass = (float)json_get_num(r, "mass", 1300);
        json_get_floats(r, "com", a->com, 3), json_get_floats(r, "extent", a->extent, 3);
        a->drag = (float)json_get_num(r, "drag", 0.7), a->downforce = (float)json_get_num(r, "downforce", 0);
        a->ngears = (uint32_t)json_get_floats(r, "gears", a->gears, VEH_MAX_GEARS);
        a->reverse = (float)json_get_num(r, "reverse", 3.2);
        a->cam_distance = (float)json_get_num(r, "cam_distance", 600), a->cam_height = (float)json_get_num(r, "cam_height", 180);
        a->max_speed = (float)json_get_num(r, "max_speed", 0);
        a->body_mesh = (uint32_t)strtoul(json_get_str(r, "body_mesh", "0"), NULL, 16), a->body_material = (uint32_t)strtoul(json_get_str(r, "body_material", "0"), NULL, 16);
        a->body_scale[0] = a->body_scale[1] = a->body_scale[2] = 1.0f;
        json_get_floats(r, "body_loc", a->body_loc, 3), json_get_floats(r, "body_rot", a->body_rot, 3), json_get_floats(r, "body_scale", a->body_scale, 3);
        const JsonValue* ps = json_get(r, "parts");
        for (size_t i = 0; i < json_len(ps) && a->nparts < VEH_MAX_PARTS; i++) {
            const JsonValue* o = json_at(ps, i);
            VehPart* p = &a->parts[a->nparts];
            memset(p, 0, sizeof *p);
            const char* pk = json_get_str(o, "type", "wheel");
            int kk = -1;
            for (int k = 0; k < VP_KIND_COUNT; k++)
                if (!strcmp(pk, g_part_ids[k])) kk = k;
            if (kk < 0) continue;
            p->kind = (uint8_t)kk;
            const char* in = json_get_str(o, "input", "None");
            for (int k = 0; k < VI_COUNT; k++)
                if (!strcmp(in, g_veh_input_names[k])) p->input = (uint8_t)k;
            p->mirror = (uint8_t)json_get_bool(o, "mirror", false);
            snprintf(p->name, VEH_NAME, "%s", json_get_str(o, "name", "")), snprintf(p->attach, VEH_NAME, "%s", json_get_str(o, "attach", ""));
            snprintf(p->link, VEH_NAME, "%s", json_get_str(o, "link", ""));
            json_get_floats(o, "pos", p->pos, 3), json_get_floats(o, "axis", p->axis, 3);
            for (int k = 0; k < VEH_PARAMS; k++) p->p[k] = g_parts[kk].def[k];
            json_get_floats(o, "p", p->p, VEH_PARAMS);
            p->mesh = (uint32_t)strtoul(json_get_str(o, "mesh", "0"), NULL, 16), p->material = (uint32_t)strtoul(json_get_str(o, "material", "0"), NULL, 16);
            p->mscale[0] = p->mscale[1] = p->mscale[2] = 1.0f;
            json_get_floats(o, "mloc", p->mloc, 3), json_get_floats(o, "mrot", p->mrot, 3), json_get_floats(o, "mscale", p->mscale, 3);
            a->nparts++;
        }
        VA.a[VA.n++] = a;
        json_free(&doc);
    }
    free(t);
    return a;
}

bool veh_save(VehicleAsset* a) {
    if (!a->path[0]) return false;
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1), jw_kstr(&w, "kind", g_veh_kind_names[a->kind % VK_KIND_COUNT]);
    jw_kfloat(&w, "mass", a->mass), jw_kfloats(&w, "com", a->com, 3), jw_kfloats(&w, "extent", a->extent, 3);
    jw_kfloat(&w, "drag", a->drag), jw_kfloat(&w, "downforce", a->downforce);
    jw_kfloats(&w, "gears", a->gears, (int)a->ngears), jw_kfloat(&w, "reverse", a->reverse);
    jw_kfloat(&w, "cam_distance", a->cam_distance), jw_kfloat(&w, "cam_height", a->cam_height), jw_kfloat(&w, "max_speed", a->max_speed);
    char hx[16];
    if (a->body_mesh) {
        snprintf(hx, sizeof hx, "%x", a->body_mesh), jw_kstr(&w, "body_mesh", hx);
        if (a->body_material) snprintf(hx, sizeof hx, "%x", a->body_material), jw_kstr(&w, "body_material", hx);
        jw_kfloats(&w, "body_loc", a->body_loc, 3), jw_kfloats(&w, "body_rot", a->body_rot, 3), jw_kfloats(&w, "body_scale", a->body_scale, 3);
    }
    jw_key(&w, "parts");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < a->nparts; i++) {
        const VehPart* p = &a->parts[i];
        jw_obj_begin(&w);
        jw_kstr(&w, "type", g_part_ids[p->kind % VP_KIND_COUNT]), jw_kstr(&w, "name", p->name), jw_kstr(&w, "attach", p->attach);
        if (p->link[0]) jw_kstr(&w, "link", p->link);
        if (p->input) jw_kstr(&w, "input", g_veh_input_names[p->input % VI_COUNT]);
        if (p->mirror) jw_kbool(&w, "mirror", true);
        jw_kfloats(&w, "pos", p->pos, 3), jw_kfloats(&w, "axis", p->axis, 3);
        jw_kfloats(&w, "p", p->p, g_parts[p->kind % VP_KIND_COUNT].nparams);
        if (p->mesh) {
            snprintf(hx, sizeof hx, "%x", p->mesh), jw_kstr(&w, "mesh", hx);
            if (p->material) snprintf(hx, sizeof hx, "%x", p->material), jw_kstr(&w, "material", hx);
            jw_kfloats(&w, "mloc", p->mloc, 3), jw_kfloats(&w, "mrot", p->mrot, 3), jw_kfloats(&w, "mscale", p->mscale, 3);
        }
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    bool ok = !w.fail;
    if (ok) {
        char tmp[700];
        snprintf(tmp, sizeof tmp, "%s.tmp", a->path);
        FILE* f = re_fopen(tmp, "wb");
        ok = f && fwrite(w.buf, 1, w.len, f) == w.len;
        if (f) ok = fclose(f) == 0 && ok;
        ok = ok && re_replace_file(tmp, a->path);
    }
    jw_free(&w);
    if (ok) a->dirty = false;
    return ok;
}

// --- Templates (what Add > Vehicles makes: each drives as it is) ---
static VehPart* add_part(VehicleAsset* a, int kind, const char* name, const char* attach, float x, float y, float z) {
    if (a->nparts >= VEH_MAX_PARTS) return &a->parts[VEH_MAX_PARTS - 1];
    VehPart* p = &a->parts[a->nparts++];
    memset(p, 0, sizeof *p);
    p->kind = (uint8_t)kind;
    snprintf(p->name, VEH_NAME, "%s", name), snprintf(p->attach, VEH_NAME, "%s", attach ? attach : "");
    p->pos[0] = x, p->pos[1] = y, p->pos[2] = z;
    p->axis[2] = 1.0f;
    p->mscale[0] = p->mscale[1] = p->mscale[2] = 1.0f;
    for (int k = 0; k < VEH_PARAMS; k++) p->p[k] = g_parts[kind].def[k];
    return p;
}
static VehPart* wheel(VehicleAsset* a, const char* name, float x, float y, float z, float r, float steer, float drive, float spring, float damper) {
    char at[VEH_NAME];
    snprintf(at, sizeof at, "%s", name);
    VehPart* p = add_part(a, VP_WHEEL, name, at, x, y, z);
    p->axis[0] = 0, p->axis[1] = 1, p->axis[2] = 0;
    p->p[0] = r, p->p[5] = steer, p->p[6] = drive, p->p[2] = spring, p->p[3] = damper;
    return p;
}
static void drivetrain(VehicleAsset* a, float torque, float peak, float maxrpm, float final, float diff) {
    VehPart* e = add_part(a, VP_ENGINE, "Engine", NULL, 0, 0, 0);
    e->p[0] = torque, e->p[1] = peak, e->p[2] = maxrpm, e->p[5] = 0.25f;
    VehPart* g = add_part(a, VP_GEARBOX, "Gearbox", NULL, 0, 0, 0);
    g->p[0] = final, g->p[2] = maxrpm * 0.86f, g->p[3] = peak * 0.5f;
    VehPart* d = add_part(a, VP_DIFFERENTIAL, "Differential", NULL, 0, 0, 0);
    d->p[0] = diff;
}

void veh_template(VehicleAsset* a, int t) {
    a->nparts = 0, a->ngears = 0, a->max_speed = 0, a->downforce = 0;
    a->body_mesh = a->body_material = 0;
    memset(a->body_loc, 0, sizeof a->body_loc), memset(a->body_rot, 0, sizeof a->body_rot);
    a->body_scale[0] = a->body_scale[1] = a->body_scale[2] = 1.0f;
    memset(a->com, 0, sizeof a->com);
    a->cam_distance = 650, a->cam_height = 200, a->reverse = 3.2f;
    static const float car_gears[6] = { 3.5f, 2.1f, 1.45f, 1.1f, 0.88f, 0.72f };
    switch (t) {
    case VT_CAR: case VT_SPORTS: {
        bool sp = t == VT_SPORTS;
        a->kind = VK_CAR, a->mass = sp ? 1350 : 1300, a->drag = sp ? 0.62f : 0.75f, a->downforce = sp ? 1.2f : 0;
        a->extent[0] = 220, a->extent[1] = 90, a->extent[2] = sp ? 32 : 40, a->com[2] = -12;
        memcpy(a->gears, car_gears, sizeof car_gears), a->ngears = 6;
        float r = sp ? 34 : 33, z = sp ? -12 : -10;
        float spring = sp ? 450 : 320, damp = sp ? 42 : 30;
        wheel(a, "Wheel_FL", 135, -80, z, r, 35, sp ? 0 : 0.5f, spring, damp), wheel(a, "Wheel_FR", 135, 80, z, r, 35, sp ? 0 : 0.5f, spring, damp);
        wheel(a, "Wheel_RL", -130, -80, z, r, 0, sp ? 0.5f : 0.5f, spring, damp), wheel(a, "Wheel_RR", -130, 80, z, r, 0, sp ? 0.5f : 0.5f, spring, damp);
        if (!sp) a->parts[0].p[6] = a->parts[1].p[6] = 0.25f, a->parts[2].p[6] = a->parts[3].p[6] = 0.25f; // (AWD, more on the front)
        a->parts[2].p[8] = a->parts[3].p[8] = 4000; // (the handbrake on the rear)
        drivetrain(a, sp ? 620 : 380, sp ? 5500 : 4200, sp ? 8000 : 6500, sp ? 3.9f : 3.7f, 2);
        // the doors (on their front edge, swinging out), the hood, the steering wheel, the shocks
        VehPart* d = add_part(a, VP_HINGE, "Door_L", "Door_L", 50, -92, 20);
        d->input = VI_ACTION1, d->p[1] = 70;
        d = add_part(a, VP_HINGE, "Door_R", "Door_R", 50, 92, 20);
        d->input = VI_ACTION1, d->p[0] = -70, d->p[1] = 0;
        d = add_part(a, VP_HINGE, "Hood", "Hood", 95, 0, sp ? 32 : 40);
        d->axis[0] = 0, d->axis[1] = 1, d->axis[2] = 0, d->input = VI_ACTION2, d->p[0] = -55, d->p[1] = 0;
        VehPart* g = add_part(a, VP_GEAR, "SteeringWheel", "SteeringWheel", 30, -40, 45);
        snprintf(g->link, VEH_NAME, "Steer"), g->axis[0] = -0.8f, g->axis[1] = 0, g->axis[2] = 0.6f, g->p[0] = 270;
        static const char* const shocks[4] = { "Shock_FL", "Shock_FR", "Shock_RL", "Shock_RR" };
        for (int i = 0; i < 4; i++) {
            VehPart* s = add_part(a, VP_PISTON, shocks[i], shocks[i], a->parts[i].pos[0], a->parts[i].pos[1] * 0.75f, z + 22);
            snprintf(s->link, VEH_NAME, "%s", a->parts[i].name), s->p[0] = 40;
        }
        break;
    }
    case VT_TRUCK: {
        a->kind = VK_TRUCK, a->mass = 9000, a->drag = 1.6f, a->extent[0] = 380, a->extent[1] = 125, a->extent[2] = 95, a->com[2] = -30;
        static const float tg[8] = { 6.0f, 4.2f, 3.0f, 2.2f, 1.6f, 1.2f, 1.0f, 0.82f };
        memcpy(a->gears, tg, sizeof tg), a->ngears = 8, a->reverse = 6.0f;
        a->cam_distance = 1100, a->cam_height = 380;
        static const float xs[3] = { 270, -110, -260 };
        static const char* const nm[6] = { "Wheel_1L", "Wheel_1R", "Wheel_2L", "Wheel_2R", "Wheel_3L", "Wheel_3R" };
        for (int i = 0; i < 6; i++) {
            VehPart* p = wheel(a, nm[i], xs[i / 2], i & 1 ? 112 : -112, -60, 55, i < 2 ? 32 : 0, 1.0f / 6.0f, 1600, 140);
            p->p[1] = 40, p->p[4] = 32, p->p[7] = 9000;
            if (i >= 4) p->p[8] = 20000;
        }
        drivetrain(a, 2400, 1600, 2600, 4.1f, 1);
        VehPart* bed = add_part(a, VP_HINGE, "Bed", "Bed", -380, 0, 40); // (the dump bed: tips back by its piston)
        bed->axis[0] = 0, bed->axis[1] = 1, bed->axis[2] = 0, bed->input = VI_ACTION2, bed->p[0] = -45, bed->p[1] = 0, bed->p[2] = 25;
        VehPart* hp = add_part(a, VP_PISTON, "BedPiston", "BedPiston", -80, 0, 40);
        snprintf(hp->link, VEH_NAME, "Bed"), hp->p[0] = 150;
        break;
    }
    case VT_MOTORCYCLE: {
        a->kind = VK_MOTORCYCLE, a->mass = 230, a->drag = 0.45f, a->extent[0] = 95, a->extent[1] = 22, a->extent[2] = 40, a->com[2] = 5;
        static const float mg[6] = { 2.8f, 2.0f, 1.6f, 1.3f, 1.1f, 0.95f };
        memcpy(a->gears, mg, sizeof mg), a->ngears = 6;
        a->cam_distance = 450, a->cam_height = 160;
        VehPart* f = wheel(a, "Wheel_F", 72, 0, -18, 31, 28, 0, 180, 18);
        VehPart* r = wheel(a, "Wheel_R", -70, 0, -18, 31, 0, 1, 220, 22);
        f->p[1] = r->p[1] = 14, r->p[8] = 600;
        drivetrain(a, 110, 8500, 11000, 2.6f, 0);
        VehPart* g = add_part(a, VP_GEAR, "Handlebar", "Handlebar", 62, 0, 45);
        snprintf(g->link, VEH_NAME, "Steer"), g->p[0] = 28;
        VehPart* s = add_part(a, VP_PISTON, "Fork", "Fork", 58, 0, 30);
        snprintf(s->link, VEH_NAME, "Wheel_F"), s->p[0] = 45;
        break;
    }
    case VT_TANK: {
        a->kind = VK_TANK, a->mass = 40000, a->drag = 3.0f, a->extent[0] = 400, a->extent[1] = 170, a->extent[2] = 75, a->com[2] = -25;
        static const float tg[4] = { 4.5f, 2.6f, 1.6f, 1.0f };
        memcpy(a->gears, tg, sizeof tg), a->ngears = 4, a->reverse = 4.5f, a->max_speed = 60;
        a->cam_distance = 1300, a->cam_height = 450;
        for (int i = 0; i < 12; i++) { // road wheels: 6 a side (the tracks: skid steering)
            char nm[VEH_NAME];
            snprintf(nm, sizeof nm, "Road_%d%c", i / 2, i & 1 ? 'R' : 'L');
            VehPart* p = wheel(a, nm, 300.0f - (float)(i / 2) * 120.0f, i & 1 ? 150 : -150, -55, 40, 0, 1.0f / 12.0f, 9000, 900);
            p->p[1] = 60, p->p[4] = 25, p->p[7] = 25000, p->p[9] = 1.4f;
        }
        drivetrain(a, 9000, 2000, 3000, 6.0f, 1);
        VehPart* tur = add_part(a, VP_GEAR, "Turret", "Turret", 0, 0, 80);
        snprintf(tur->link, VEH_NAME, "Steer"), tur->p[0] = 0, tur->p[1] = 0;
        VehPart* gun = add_part(a, VP_HINGE, "Gun", "Gun", 120, 0, 110);
        gun->axis[0] = 0, gun->axis[1] = 1, gun->axis[2] = 0, gun->input = VI_ACTION2, gun->p[0] = -15, gun->p[1] = 8, gun->p[2] = 15;
        break;
    }
    case VT_BOAT: {
        a->kind = VK_BOAT, a->mass = 900, a->drag = 0.3f, a->extent[0] = 300, a->extent[1] = 110, a->extent[2] = 45, a->com[2] = -25;
        a->cam_distance = 900, a->cam_height = 300;
        for (int i = 0; i < 8; i++) { // the hull's corners and middles float
            char nm[VEH_NAME];
            snprintf(nm, sizeof nm, "Float%d", i);
            VehPart* f = add_part(a, VP_FLOAT, nm, NULL, i < 4 ? (i < 2 ? 240.0f : -240.0f) : (i < 6 ? 80.0f : -80.0f), (i & 1) ? -75.0f : 75.0f, -35);
            f->p[0] = 35, f->p[1] = 1.2f;
        }
        VehPart* pr = add_part(a, VP_PROPELLER, "Screw", "Screw", -300, 0, -45);
        pr->axis[0] = 1, pr->axis[1] = 0, pr->axis[2] = 0, pr->p[0] = 9000;
        VehPart* rd = add_part(a, VP_WING, "Rudder", "Rudder", -295, 0, -40);
        rd->axis[0] = 0, rd->axis[1] = 1, rd->axis[2] = 0, rd->input = VI_STEER, rd->p[0] = 0.25f, rd->p[4] = 30, rd->p[6] = 1;
        VehPart* keel = add_part(a, VP_WING, "Keel", NULL, 0, 0, -45); // (the hull's grip on the water: it goes where it points)
        keel->axis[0] = 0, keel->axis[1] = 1, keel->axis[2] = 0, keel->p[0] = 1.2f, keel->p[6] = 1, keel->p[2] = 30;
        break;
    }
    case VT_LOCOMOTIVE: case VT_WAGON: {
        bool loco = t == VT_LOCOMOTIVE;
        a->kind = VK_TRAIN, a->mass = loco ? 80000 : 30000, a->drag = loco ? 8 : 4, a->extent[0] = 900, a->extent[1] = 150, a->extent[2] = 180;
        a->cam_distance = 2400, a->cam_height = 700, a->max_speed = 140;
        VehPart* b1 = add_part(a, VP_BOGIE, "Bogie_F", "Bogie_F", 620, 0, -200);
        VehPart* b2 = add_part(a, VP_BOGIE, "Bogie_R", "Bogie_R", -620, 0, -200);
        (void)b1, (void)b2;
        if (loco) {
            static const float lg[1] = { 1.0f };
            memcpy(a->gears, lg, sizeof lg), a->ngears = 1;
            drivetrain(a, 60000, 900, 1500, 1.0f, 1);
            VehPart* w = wheel(a, "Driver", 0, 0, -200, 60, 0, 1, 1, 0); // (the traction's wheel: the rail's grip)
            w->attach[0] = 0;
        }
        break;
    }
    case VT_AIRPLANE: {
        a->kind = VK_AIRPLANE, a->mass = 1100, a->drag = 0.35f, a->extent[0] = 380, a->extent[1] = 60, a->extent[2] = 60, a->com[0] = 20;
        a->cam_distance = 1300, a->cam_height = 300;
        // the landing gear: a steering nose wheel, two mains with brakes
        VehPart* n = wheel(a, "Gear_N", 250, 0, -55, 22, 25, 0, 250, 30);
        VehPart* l = wheel(a, "Gear_L", -10, -150, -55, 28, 0, 0, 400, 40);
        VehPart* r = wheel(a, "Gear_R", -10, 150, -55, 28, 0, 0, 400, 40);
        n->p[7] = 0, l->p[7] = r->p[7] = 2500;
        VehPart* pr = add_part(a, VP_PROPELLER, "Propeller", "Propeller", 390, 0, 0);
        pr->axis[0] = 1, pr->axis[1] = 0, pr->axis[2] = 0, pr->p[0] = 5200, pr->p[1] = 0, pr->p[2] = 2400, pr->p[3] = 0;
        // the wings (a little dihedral and incidence), the ailerons, the tail (elevator, rudder)
        VehPart* wl = add_part(a, VP_WING, "Wing_L", "Wing_L", 30, -270, 20);
        VehPart* wr = add_part(a, VP_WING, "Wing_R", "Wing_R", 30, 270, 20);
        wl->axis[0] = -0.05f, wl->axis[1] = 0.05f, wl->axis[2] = 1, wr->axis[0] = -0.05f, wr->axis[1] = -0.05f, wr->axis[2] = 1; // (3 degrees of incidence, a little dihedral)
        wl->p[0] = wr->p[0] = 7.5f, wl->p[4] = wr->p[4] = 15, wl->input = wr->input = VI_ROLL, wl->mirror = 1;
        VehPart* hs = add_part(a, VP_WING, "Elevator", "Elevator", -340, 0, 15);
        hs->p[0] = 2.4f, hs->p[4] = 20, hs->input = VI_PITCH, hs->axis[0] = 0.02f; // (less incidence than the wing: stable)
        VehPart* vs = add_part(a, VP_WING, "Fin", "Fin", -350, 0, 70);
        vs->axis[0] = 0, vs->axis[1] = 1, vs->axis[2] = 0, vs->p[0] = 1.3f, vs->p[4] = 20, vs->input = VI_YAW;
        break;
    }
    case VT_HELICOPTER: {
        a->kind = VK_HELICOPTER, a->mass = 1500, a->drag = 0.9f, a->extent[0] = 240, a->extent[1] = 80, a->extent[2] = 90, a->com[2] = -10;
        a->cam_distance = 1200, a->cam_height = 350;
        VehPart* sk1 = wheel(a, "Skid_FL", 100, -90, -90, 10, 0, 0, 900, 120);
        VehPart* sk2 = wheel(a, "Skid_FR", 100, 90, -90, 10, 0, 0, 900, 120);
        VehPart* sk3 = wheel(a, "Skid_RL", -90, -90, -90, 10, 0, 0, 900, 120);
        VehPart* sk4 = wheel(a, "Skid_RR", -90, 90, -90, 10, 0, 0, 900, 120);
        sk1->attach[0] = sk2->attach[0] = sk3->attach[0] = sk4->attach[0] = 0;
        sk1->p[7] = sk2->p[7] = sk3->p[7] = sk4->p[7] = 8000, sk1->p[4] = sk2->p[4] = sk3->p[4] = sk4->p[4] = 10;
        VehPart* ro = add_part(a, VP_ROTOR, "MainRotor", "MainRotor", 0, 0, 120);
        ro->p[0] = 30000;
        VehPart* tr = add_part(a, VP_GEAR, "TailRotor", "TailRotor", -480, 25, 60);
        snprintf(tr->link, VEH_NAME, "MainRotor"), tr->axis[0] = 0, tr->axis[1] = 1, tr->axis[2] = 0, tr->p[0] = 5;
        break;
    }
    default: break;
    }
    a->version++;
}

uint32_t veh_create(const char* folder_vpath, const char* name, int tmpl) {
    if (VA.n >= MAX_ASSETS) return 0;
    VehicleAsset* a = calloc(1, sizeof *a); // (an asset)
    if (!a) return 0;
    char vp[700];
    snprintf(vp, sizeof vp, "%s/%s.rvehicle", folder_vpath, name);
    disk_of(vp, a->path, sizeof a->path);
    snprintf(a->name, sizeof a->name, "%s", name);
    a->hash = vpath_hash(vp);
    veh_template(a, tmpl);
    if (!veh_save(a)) return free(a), 0u;
    VA.a[VA.n++] = a;
    return a->hash;
}
VehicleAsset* veh_temp(const char* name, int tmpl) {
    if (VA.n >= MAX_ASSETS) return NULL;
    VehicleAsset* a = calloc(1, sizeof *a); // (an asset)
    if (!a) return NULL;
    char vp[200];
    snprintf(vp, sizeof vp, "/Temp/%s.rvehicle", name);
    snprintf(a->name, sizeof a->name, "%s", name);
    a->hash = vpath_hash(vp);
    veh_template(a, tmpl);
    VA.a[VA.n++] = a;
    return a;
}

// --- The actors ---
static Entity component(World* w, Entity owner, uint32_t node, const char* name, int shape, const float loc[3], const float rot[3],
                        const float scale[3], const float color[3], float metal, float rough) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_ATTACH));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CAttach* at = ecs_get(w, e, COMP_ATTACH);
    at->owner = at->parent = owner, at->node = 1, at->kind = ATTACH_SCS; (void)node; // (all on the actor: one level)
    memcpy(at->location, loc, 12), memcpy(at->rotation, rot, 12), memcpy(at->scale, scale, 12);
    CStaticMesh* m = ecs_get(w, e, COMP_STATIC_MESH);
    m->mesh = (uint32_t)shape, memcpy(m->base_color, color, 12), m->metallic = metal, m->roughness = rough, m->cast_shadow = 1;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->flags = MOBILITY_MOVABLE;
    memcpy(t->scale, scale, 12);
    return e;
}

static const float* body_color(int kind) {
    static const float c[VK_KIND_COUNT][3] = { { 0.55f, 0.06f, 0.05f }, { 0.08f, 0.1f, 0.12f }, { 0.75f, 0.5f, 0.08f }, { 0.22f, 0.27f, 0.15f },
                                               { 0.85f, 0.85f, 0.82f }, { 0.12f, 0.22f, 0.45f }, { 0.85f, 0.85f, 0.88f }, { 0.1f, 0.3f, 0.18f } };
    return c[kind % VK_KIND_COUNT];
}

Entity vehicle_spawn(World* w, uint32_t asset, const float loc[3], float yaw, const char* name) {
    const VehicleAsset* a = veh_find(asset);
    if (!a) return ENTITY_NONE;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_VEHICLE));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name && name[0] ? name : a->name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, loc, 12), t->rotation[1] = yaw, t->flags = MOBILITY_MOVABLE;
    ((CVehicle*)ecs_get(w, e, COMP_VEHICLE))->asset = asset;
    uint32_t node = 1;
    const float* col = body_color(a->kind);
    const float dark[3] = { 0.03f, 0.03f, 0.035f }, metal[3] = { 0.6f, 0.6f, 0.62f }, glass[3] = { 0.05f, 0.08f, 0.1f };
    float z0[3] = { 0, 0, 0 };
    // the body: the chassis box (an airplane's fuselage, a boat's hull, a wagon's box), a cabin on cars and trucks
    float bs[3] = { a->extent[0] / 50.0f, a->extent[1] / 50.0f, a->extent[2] / 50.0f };
    if (a->kind == VK_AIRPLANE) bs[1] = bs[2] = a->extent[1] / 50.0f;
    if (a->body_mesh) { // the user's model: its body, then only the parts with a model of their own
        Entity b = component(w, e, node++, "Body", (int)a->body_mesh, a->body_loc, a->body_rot, a->body_scale, (const float[3]){ 0.8f, 0.8f, 0.8f }, 0.0f, 0.5f);
        CStaticMesh* bm = b ? ecs_get(w, b, COMP_STATIC_MESH) : NULL;
        if (bm) bm->material = a->body_material;
        for (uint32_t i = 0; i < a->nparts; i++) {
            const VehPart* p = &a->parts[i];
            if (!p->mesh || !p->attach[0]) continue;
            float l[3] = { p->mloc[0], p->mloc[1], p->mloc[2] };
            if (p->kind == VP_WHEEL) l[0] = p->pos[0], l[1] = p->pos[1], l[2] = p->pos[2] - p->p[4] * 0.6f; // (where it hangs at rest)
            Entity pe = component(w, e, node++, p->attach, (int)p->mesh, l, p->mrot, p->mscale, (const float[3]){ 0.8f, 0.8f, 0.8f }, 0.0f, 0.5f);
            CStaticMesh* pm = pe ? ecs_get(w, pe, COMP_STATIC_MESH) : NULL;
            if (pm) pm->material = p->material;
        }
        return e;
    }
    component(w, e, node++, "Body", a->kind == VK_HELICOPTER ? SHAPE_SPHERE : SHAPE_CUBE, z0, z0, bs, col, 0.3f, 0.35f);
    if (a->kind == VK_CAR || a->kind == VK_TRUCK) {
        float cl[3] = { a->kind == VK_TRUCK ? a->extent[0] * 0.55f : -a->extent[0] * 0.1f, 0, a->extent[2] + (a->kind == VK_TRUCK ? 60.0f : 25.0f) };
        float cs[3] = { a->kind == VK_TRUCK ? 1.6f : 2.2f, a->extent[1] / 52.0f, a->kind == VK_TRUCK ? 1.2f : 0.5f };
        component(w, e, node++, "Cabin", SHAPE_CUBE, cl, z0, cs, a->kind == VK_TRUCK ? col : glass, 0.2f, a->kind == VK_TRUCK ? 0.35f : 0.05f);
    }
    if (a->kind == VK_TANK) {
        float tl[3] = { 0, 0, a->extent[2] + 40 }, ts[3] = { 3.2f, 2.6f, 0.8f };
        (void)tl, (void)ts;
        for (int s = -1; s <= 1; s += 2) { // the tracks
            float l[3] = { 0, (float)s * 150.0f, -50 }, sc[3] = { 8.6f, 0.62f, 0.95f };
            component(w, e, node++, s < 0 ? "Track_L" : "Track_R", SHAPE_CUBE, l, z0, sc, dark, 0.2f, 0.9f);
        }
    }
    if (a->kind == VK_HELICOPTER) {
        float tb[3] = { -300, 0, 40 }, ts[3] = { 4.0f, 0.3f, 0.35f };
        component(w, e, node++, "TailBoom", SHAPE_CUBE, tb, z0, ts, col, 0.3f, 0.35f);
    }
    // the parts' meshes
    for (uint32_t i = 0; i < a->nparts; i++) {
        const VehPart* p = &a->parts[i];
        if (!p->attach[0]) continue;
        float l[3] = { p->pos[0], p->pos[1], p->pos[2] }, r[3] = { 0, 0, 0 }, s[3] = { 1, 1, 1 };
        int shape = SHAPE_CUBE;
        const float* c = metal;
        float rough = 0.4f, met = 0.8f;
        switch (p->kind) {
        case VP_WHEEL: {
            float rad = p->p[0], wid = p->p[1];
            l[2] -= p->p[4] * 0.6f; // (where it hangs at rest; vehicle_update puts it on its spring)
            r[2] = 90.0f, s[0] = s[1] = rad / 50.0f, s[2] = wid / 100.0f;
            shape = SHAPE_CYLINDER, c = dark, rough = 0.85f, met = 0.0f;
            break;
        }
        case VP_HINGE: { // a door / hood / bed / gun: a panel from the hinge
            if (!strncmp(p->name, "Door", 4)) l[0] -= 55, l[2] -= 5, s[0] = 1.1f, s[1] = 0.06f, s[2] = 0.6f, c = col, rough = 0.35f, met = 0.3f;
            else if (!strcmp(p->name, "Hood")) l[0] += 55, s[0] = 1.1f, s[1] = 1.7f, s[2] = 0.06f, c = col, rough = 0.35f, met = 0.3f;
            else if (!strcmp(p->name, "Bed")) l[0] += 150, l[2] += 50, s[0] = 3.0f, s[1] = 2.3f, s[2] = 1.0f, c = metal;
            else if (!strcmp(p->name, "Gun")) l[0] += 200, s[0] = 4.0f, s[1] = s[2] = 0.22f, c = col;
            else s[0] = s[1] = s[2] = 0.5f;
            break;
        }
        case VP_PISTON: s[0] = s[1] = 0.08f, s[2] = p->p[0] / 100.0f, shape = SHAPE_CYLINDER; break;
        case VP_GEAR:
            if (!strcmp(p->link, "Steer") && strcmp(p->name, "Turret")) s[0] = s[1] = 0.38f, s[2] = 0.04f, shape = SHAPE_CYLINDER, c = dark, rough = 0.6f, met = 0.0f;
            else if (!strcmp(p->name, "Turret")) s[0] = 3.2f, s[1] = 2.6f, s[2] = 0.8f, c = col, rough = 0.5f, met = 0.3f;
            else if (!strcmp(p->name, "TailRotor")) s[0] = 1.4f, s[1] = 0.05f, s[2] = 0.18f, c = dark;
            else s[0] = s[1] = 0.3f, s[2] = 0.1f, shape = SHAPE_CYLINDER;
            break;
        case VP_PROPELLER: s[0] = 0.08f, s[1] = p->p[3] > 0.5f ? 0.7f : 1.9f, s[2] = 0.18f, c = dark; break;
        case VP_ROTOR: s[0] = 10.0f, s[1] = 0.32f, s[2] = 0.05f, c = dark; break;
        case VP_WING: {
            float area = p->p[0];
            if (fabsf(p->axis[2]) > 0.5f) s[0] = 1.5f, s[1] = area / 1.5f, s[2] = 0.08f, l[1] = p->pos[1] * 1.0f, s[1] = fminf(s[1], fabsf(p->pos[1]) / 50.0f * 0.9f + 1.0f);
            else s[0] = 1.0f, s[1] = 0.06f, s[2] = area / 1.0f;
            if (p->p[6] > 0.5f) s[0] = 0.6f, s[2] = 0.7f; // (a rudder)
            if (!strcmp(p->name, "Elevator")) s[0] = 1.0f, s[1] = 4.8f, s[2] = 0.06f;
            c = col, rough = 0.35f, met = 0.3f;
            break;
        }
        case VP_BOGIE: s[0] = 2.6f, s[1] = 1.6f, s[2] = 0.7f, c = dark, rough = 0.7f, met = 0.5f, l[2] += 25; break;
        case VP_SLIDER: s[0] = s[1] = s[2] = 0.5f; break;
        default: s[0] = s[1] = s[2] = 0.3f; break;
        }
        component(w, e, node++, p->attach, shape, l, r, s, c, met, rough);
    }
    bp_components_place_actor(w, e);
    return e;
}

float veh_rest_height(const VehicleAsset* a) {
    float h = 0.0f;
    bool wheels = false;
    for (uint32_t i = 0; i < a->nparts; i++)
        if (a->parts[i].kind == VP_WHEEL) h = fmaxf(h, -a->parts[i].pos[2] + a->parts[i].p[4] * 0.7f + a->parts[i].p[0]), wheels = true;
    if (a->kind == VK_TRAIN) return 0.0f;
    return wheels ? h : a->extent[2] + 5.0f;
}

Entity water_spawn(World* w, const float center[3], float size_x, float size_y) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_WATER));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "WaterBody");
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, center, 12), t->scale[0] = size_x / 100.0f, t->scale[1] = size_y / 100.0f, t->scale[2] = 1.0f;
    t->flags = MOBILITY_STATIC;
    CStaticMesh* m = ecs_get(w, e, COMP_STATIC_MESH);
    m->mesh = SHAPE_PLANE, m->base_color[0] = 0.015f, m->base_color[1] = 0.06f, m->base_color[2] = 0.09f, m->roughness = 0.04f, m->metallic = 0.0f;
    return e;
}

Entity rail_spawn(World* w, const float (*points)[3], uint32_t n, bool closed, const char* name) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_RAIL));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name && name[0] ? name : "Rail");
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->flags = MOBILITY_STATIC;
    CRail* r = ecs_get(w, e, COMP_RAIL);
    spline_component_defaults(r, SPLINE_KIND_RAIL); // (Q14: a Spline of the Rail kind)
    r->npoints = n < RAIL_MAX_POINTS ? n : RAIL_MAX_POINTS, r->closed = closed;
    memcpy(r->points, points, sizeof(float) * 3 * r->npoints);
    r->built = 0;
    vehicle_editor_update(w);
    return e;
}

#include "vehicle_build.inc"
#include "vehicle_sim.inc"
#include "vehicle_play.inc"
