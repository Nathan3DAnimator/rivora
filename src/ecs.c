#include "particles.h"
#include "ecs.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "spline.h" // (Q14: the Spline Component's defaults)

// The particle components' defaults (particles.h; here so re_core links alone)
void particles_system_defaults(CParticleSystem* c) {
    memset(c, 0, sizeof *c);
    c->auto_activate = 1, c->active = 1;
    c->time_scale = c->spawn_scale = c->size_scale = 1.0f;
    c->color[0] = c->color[1] = c->color[2] = c->color[3] = 1.0f;
}

void particles_tl_defaults(CParticleTl* c) { // Blender's defaults (the Tlender's)
    memset(c, 0, sizeof *c);
    c->count = 1000, c->seed = 0;
    c->frame_start = 1, c->frame_end = 200, c->fps = 24;
    c->lifetime = 50, c->lifetime_random = 0;
    c->emit_from = PTL_FROM_FACES, c->render_as = PTL_RENDER_HALO, c->physics = PTL_PHYS_NEWTON, c->loop = 1;
    c->normal_factor = 1, c->random_factor = 0;
    c->gravity = 1, c->drag = 0, c->brownian = 0;
    c->size = 0.05f, c->size_random = 0;
    c->color[0] = c->color[1] = c->color[2] = 0.8f;
    c->emissive = 0;
    c->receive_shadows = 1;
}

const ComponentInfo g_components[COMP_COUNT] = {
    [COMP_NAME] = { "Name", sizeof(CName), 0 },
    [COMP_TRANSFORM] = { "Transform", sizeof(CTransform), 0 },
    [COMP_ROTATING] = { "Rotating Movement", sizeof(CRotating), 1 },
    [COMP_STATIC_MESH] = { "Static Mesh", sizeof(CStaticMesh), 1 },
    [COMP_LIGHT] = { "Light", sizeof(CLight), 0 },
    [COMP_CAMERA] = { "Camera", sizeof(CCamera), 0 },
    [COMP_PLAYER_START] = { "Player Start", sizeof(CPlayerStart), 0 },
    [COMP_FOG] = { "Exponential Height Fog", sizeof(CFog), 0 },
    [COMP_CLOUD] = { "Volumetric Cloud", sizeof(CCloud), 0 },
    [COMP_BLUEPRINT] = { "Blueprint", sizeof(CBlueprint), 0 },
    [COMP_CHARACTER] = { "Character Movement", sizeof(CCharacter), 0 },
    [COMP_ATTACH] = { "Attachment", sizeof(CAttach), 0 },
    [COMP_SPRING_ARM] = { "Spring Arm", sizeof(CSpringArm), 0 },
    [COMP_COLLISION] = { "Collision", sizeof(CCollision), 0 },
    [COMP_PROJECTILE] = { "Projectile Movement", sizeof(CProjectile), 0 },
    [COMP_FLOATING] = { "Floating Pawn Movement", sizeof(CFloating), 0 },
    [COMP_ARROW] = { "Arrow", sizeof(CArrow), 0 },
    [COMP_RIGID_BODY] = { "Physics", sizeof(CRigidBody), 1 },
    [COMP_SKELETAL_MESH] = { "Skeletal Mesh", sizeof(CSkeletalMesh), 1 },
    [COMP_AI_AGENT] = { "AI", sizeof(CAIAgent), 1 },
    [COMP_FOLDER] = { "Folder", sizeof(CFolder), 0 },
    [COMP_OUTLINER_FOLDER] = { "Outliner Folder", sizeof(COutlinerFolder), 0 },
    [COMP_STREAMED] = { "Streamed", sizeof(CStreamed), 0 },
    [COMP_POST_PROCESS] = { "Post Process Volume", sizeof(CPostProcess), 0 },
    [COMP_CROWD] = { "Crowd", sizeof(CCrowd), 1 },
    [COMP_LANDSCAPE] = { "Landscape", sizeof(CLandscape), 0 },
    [COMP_PROTO] = { "Prototype", sizeof(CProto), 0 },
    [COMP_FOLIAGE] = { "Foliage", sizeof(CFoliage), 0 },
    [COMP_AUDIO] = { "Audio", sizeof(CAudio), 1 },
    [COMP_AUDIO_VOLUME] = { "Audio Volume", sizeof(CAudioVolume), 0 },
    [COMP_PARTICLE_SYSTEM] = { "Particle System", sizeof(CParticleSystem), 1 },
    [COMP_PARTICLE_TL] = { "Particles (Tlender)", sizeof(CParticleTl), 1 },
    [COMP_VEHICLE] = { "Vehicle", sizeof(CVehicle), 1 },
    [COMP_WATER] = { "Water Body", sizeof(CWater), 1 },
    [COMP_RAIL] = { "Spline", sizeof(CRail), 1 }, // (Q14: the Rail grew into the Spline Component, spline.h)
    [COMP_PROC_WORLD] = { "Procedural World", sizeof(CProcWorld), 1 },
    [COMP_RAIN] = { "Rain", sizeof(CRain), 1 },
    [COMP_CLOTH] = { "Cloth", sizeof(CCloth), 1 },
    [COMP_GAMEPLAY_TAGS] = { "Gameplay Tags", sizeof(CGameplayTags), 1 },
    [COMP_CONSTRAINT] = { "Physics Constraint", sizeof(CConstraint), 1 },
    [COMP_NAV] = { "Navigation", sizeof(CNav), 0 },
    [COMP_HEALTH] = { "Health", sizeof(CHealth), 1 },
    [COMP_INVENTORY] = { "Inventory", sizeof(CInventory), 1 },
    [COMP_PICKUP] = { "Pickup", sizeof(CPickup), 1 },
    [COMP_SAVEABLE] = { "Saveable", sizeof(CSaveable), 1 },
    [COMP_CHECKPOINT] = { "Checkpoint", sizeof(CCheckpoint), 0 },
    [COMP_INTERACTABLE] = { "Interactable", sizeof(CInteractable), 1 },
    [COMP_INTERACTOR] = { "Interactor", sizeof(CInteractor), 1 },
    [COMP_DEVICE] = { "Interactive Device", sizeof(CDevice), 1 },
    [COMP_DIALOGUE_SPEAKER] = { "Dialogue Speaker", sizeof(CDialogueSpeaker), 1 },
    [COMP_QUEST_LOG] = { "Quest Log", sizeof(CQuestLog), 1 },
    [COMP_GAME_RULES] = { "Game Rules", sizeof(CGameRules), 1 },
    [COMP_ABILITY_SYSTEM] = { "Ability System", sizeof(CAbilitySystem), 1 },
    [COMP_HUD_LAYOUT] = { "HUD Layout", sizeof(CHudLayout), 1 },
    [COMP_DECAL] = { "Decal", sizeof(CDecal), 1 },
    [COMP_GROOM] = { "Groom", sizeof(CGroom), 1 },
    [COMP_FIRE] = { "Fire", sizeof(CFire), 1 },
    [COMP_RIVER] = { "River", sizeof(CRiver), 1 },
    [COMP_BUOYANCY] = { "Buoyancy", sizeof(CBuoyancy), 1 },
    [COMP_FOG_VOLUME] = { "Local Fog Volume", sizeof(CFogVolume), 0 },
    [COMP_SPLAT] = { "Gaussian Splat", sizeof(CSplat), 1 },
    [COMP_VOLUME] = { "Heterogeneous Volume", sizeof(CVolume), 1 },
    [COMP_WORLD_PARTITION] = { "World Partition", sizeof(CWorldPartition), 0 },
    [COMP_PARTITION] = { "World Partition (Actor)", sizeof(CPartition), 0 },
};

const char* const g_shape_names[SHAPE_COUNT] = { "Cube", "Sphere", "Cylinder", "Cone", "Plane" };
const char* const g_collision_shape_names[COLSHAPE_COUNT] = { "Automatic", "Box", "Sphere", "Capsule", "Convex Hull" };
const char* const g_light_names[LIGHT_TYPE_COUNT] = { "Directional Light", "Point Light", "Spot Light", "Rect Light",
                                                      "Sky Light" };

#define GEN_MASK ((1u << (32 - RE_ECS_INDEX_BITS)) - 1)

bool ecs_init(World* w) {
    memset(w, 0, sizeof *w);
    if (!re_vm_reserve(&w->slots, sizeof(EcsSlot) * RE_ECS_MAX_ENTITIES)) return false;
    if (!re_vm_reserve(&w->free_list, sizeof(uint32_t) * RE_ECS_MAX_ENTITIES)) return false;
    if (!re_vm_ensure(&w->slots, sizeof(EcsSlot))) return false;
    ecs_slots(w)[0].archetype = ECS_NO_ARCHETYPE; // slot 0: ENTITY_NONE, never handed out
    w->slot_count = 1;
    return true;
}

void ecs_touch(World* w, Entity e) {
    w->changed[w->changes++ % RE_ECS_CHANGE_RING] = entity_index(e);
}

void ecs_touch_all(World* w) { w->revision++; }

void ecs_shutdown(World* w) {
    re_vm_release(&w->slots);
    re_vm_release(&w->free_list);
    for (uint32_t a = 0; a < w->archetype_count; a++) {
        re_vm_release(&w->archetypes[a].entities);
        for (int c = 0; c < COMP_COUNT; c++) re_vm_release(&w->archetypes[a].columns[c]);
    }
    memset(w, 0, sizeof *w);
}

static int find_archetype(World* w, uint64_t mask) {
    for (uint32_t a = 0; a < w->archetype_count; a++)
        if (w->archetypes[a].mask == mask) return (int)a;
    if (w->archetype_count >= RE_ECS_MAX_ARCHETYPES) return -1;
    EcsArchetype* arch = &w->archetypes[w->archetype_count];
    memset(arch, 0, sizeof *arch);
    arch->mask = mask;
    if (!re_vm_reserve(&arch->entities, sizeof(Entity) * RE_ECS_MAX_ENTITIES)) return -1;
    for (int c = 0; c < COMP_COUNT; c++)
        if ((mask & COMP_BIT(c)) && !re_vm_reserve(&arch->columns[c], (size_t)g_components[c].size * RE_ECS_MAX_ENTITIES))
            return -1;
    return (int)w->archetype_count++;
}

// A new zeroed row at the end of the archetype. -1 without memory.
static int64_t push_row(EcsArchetype* a, Entity e) {
    uint32_t row = a->count;
    if (!re_vm_ensure(&a->entities, sizeof(Entity) * (row + 1))) return -1;
    for (int c = 0; c < COMP_COUNT; c++) {
        if (!(a->mask & COMP_BIT(c))) continue;
        size_t size = g_components[c].size;
        if (!re_vm_ensure(&a->columns[c], size * (row + 1))) return -1;
        memset(a->columns[c].base + size * row, 0, size); // a reused row holds an old entity's data
    }
    ecs_arch_entities(a)[row] = e;
    a->count++;
    return row;
}

// Removes a row by moving the last one into it (the arrays stay packed); fixes the moved entity's slot.
static void remove_row(World* w, EcsArchetype* a, uint32_t row) {
    uint32_t last = a->count - 1;
    if (row != last) {
        for (int c = 0; c < COMP_COUNT; c++) {
            if (!(a->mask & COMP_BIT(c))) continue;
            size_t size = g_components[c].size;
            memcpy(a->columns[c].base + size * row, a->columns[c].base + size * last, size);
        }
        Entity moved = ecs_arch_entities(a)[last];
        ecs_arch_entities(a)[row] = moved;
        ecs_slots(w)[entity_index(moved)].row = row;
    }
    a->count--;
}

// The defaults of a fresh component (the rest is zero)
void ecs_component_defaults(ComponentId c, void* p) {
    switch (c) {
    case COMP_TRANSFORM: {
        CTransform* t = p;
        t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
        t->flags = MOBILITY_MOVABLE;
        break;
    }
    case COMP_AUDIO: {
        CAudio* a = p;
        a->volume = a->pitch = 1.0f, a->auto_activate = 1, a->spatialize = 1;
        break;
    }
    case COMP_PARTICLE_SYSTEM: particles_system_defaults(p); break;
    case COMP_PARTICLE_TL: particles_tl_defaults(p); break;
    case COMP_VEHICLE: {
        CVehicle* v = p;
        v->start_engine = 1, v->couple_gap = 1.0f;
        break;
    }
    case COMP_WATER: {
        CWater* wb = p;
        wb->wave_height = 8.0f, wb->wave_length = 900.0f, wb->wave_speed = 250.0f, wb->density = 1000.0f, wb->depth = 1000.0f;
        wb->wind_speed = 9.0f, wb->choppiness = 1.0f, wb->foam = 0.6f, wb->clarity = 800.0f, wb->interaction = 1.0f, wb->swell = 0.4f;
        wb->shore_foam = 0.6f;
        wb->color[0] = 0.01f, wb->color[1] = 0.05f, wb->color[2] = 0.08f;
        wb->scatter[0] = 0.02f, wb->scatter[1] = 0.22f, wb->scatter[2] = 0.18f;
        break;
    }
    case COMP_CLOTH: {
        CCloth* c = p;
        c->width = 200.0f, c->height = 250.0f, c->res_x = 48, c->res_y = 60, c->preset = CLOTH_COTTON, c->pin = CLOTH_PIN_TOP;
        c->density = 150.0f, c->stretch = 6000.0f, c->shear = 600.0f, c->bend = 2.0f, c->damping = 0.02f, c->friction = 0.4f;
        c->thickness = 1.0f, c->gust = 0.4f, c->gravity = 1.0f, c->tear = 0.0f;
        c->color[0] = 0.55f, c->color[1] = 0.08f, c->color[2] = 0.07f, c->roughness = 0.8f, c->sheen = 0.25f, c->translucency = 0.3f;
        c->cast_shadow = 1, c->self_collision = 1, c->self_distance = 0.0f;
        c->mesh = 0, c->mesh_scale[0] = c->mesh_scale[1] = c->mesh_scale[2] = 1.0f, c->pin_band = 6.0f, c->pin_axis[2] = 1.0f;
        c->iterations = 6, c->substeps = 1; // (VBD converges in a few: 1 x 6 at 60+ fps; more for stiffer or faster cloth)
        // (Q9) the soft body's: a sofa cushion until a preset says otherwise (softbody.h)
        c->kind = CLOTH_KIND_SHEET, c->soft_preset = SOFT_CUSHION, c->soft_res = 10, c->soft_density = 40.0f, c->youngs = 12.0f, c->poisson = 0.3f;
        c->soft_damping = 0.05f, c->plasticity = 0.15f, c->recovery = 3.0f, c->yield = 0.25f, c->attach = SOFT_ATTACH_BOTTOM;
        c->attach_falloff = 0.0f, c->attach_stiffness = 2000.0f, c->sleep = 1, c->sleep_distance = 60.0f, c->rigid_push = 1.0f, c->released = 0;
        break;
    }
    case COMP_RAIN: {
        CRain* r = p;
        r->intensity = 20.0f, r->wind_speed = 3.0f, r->wind_dir = 30.0f, r->drop_size = 1.7f, r->drops = 1.0f, r->splashes = 1.0f;
        r->wetness = 1.0f, r->wet_time = 20.0f, r->dry_time = 300.0f, r->puddles = 1.0f, r->water_gain = 40.0f, r->flow = 1.0f;
        r->infiltration = 4.0f, r->evaporation = 1.0f, r->micro_relief = 1.5f, r->streaks = 1.0f, r->ripples = 1.0f;
        r->area = 96.0f, r->resolution = 512, r->start_water = 0.0f;
        r->snow = 0, r->flake_size = 8.0f, r->cover_time = 120.0f, r->cover_angle = 55.0f, r->melt_time = 600.0f, r->start_cover = 0.0f;
        r->drop_amount = 1.0f, r->fall_speed = 1.0f;
        r->preset = RAIN_PRESET_CUSTOM, r->drop_color[0] = r->drop_color[1] = r->drop_color[2] = 1.0f;
        r->streak_length = 1.0f, r->drop_width = 1.0f, r->splash_size = 1.0f, r->gusts = 0.3f, r->far_rain = 1.0f;
        r->actor_wet_time = 60.0f, r->actor_dry_time = 480.0f;
        r->sound_volume = 1.0f, r->sound_style = RAIN_SOUND_OPEN, r->custom_sound = 0, r->muffle = 0.7f;
        r->thunder = 0.0f, r->thunder_volume = 1.0f, r->lightning = 0.6f;
        rain_shading_defaults(r, RAIN_SHADE_REALISTIC);
        r->lens_water = 0.0f, r->lens_drop_size = 1.0f;
        r->lens_gravity = r->lens_stickiness = r->lens_inertia = r->lens_shake_off = r->lens_airflow = 1.0f;
        r->lens_dry_time = 8.0f, r->lens_trail = 1.0f, r->lens_blur = 1.0f, r->lens_refraction = 1.0f;
        r->wet_marks = 1.0f, r->wet_marks_dry_time = 120.0f, r->actor_drips = 1.0f; // (the water on the lens: the dev turns it on)
        r->regional = 0, r->storminess = 0.5f, r->weather_seed = 1u; // (Q27.6)
        break;
    }
    case COMP_PROC_WORLD: {
        CProcWorld* pw = p;
        pw->seed = 1337u, pw->block = 200.0f, pw->step = 50.0f, pw->view_distance = 100000.0f, pw->height = 6000.0f;
        pw->biome_scale = 80000.0f, pw->tree_density = 4.0f, pw->structure_density = 3.0f, pw->creature_density = 1.0f;
        pw->rebase_distance = 200000.0f, pw->rooms = 24, pw->room_size = 1200.0f, pw->room_height = 400.0f, pw->loop_chance = 0.15f;
        break;
    }
    case COMP_RAIL: // (Q14) a Spline Component (Unreal's two points); a Rail sets its kind (rail_spawn), a level from before
        spline_component_defaults(p, SPLINE_KIND_SPLINE); // reads a Rail (component_upgrade)
        break;
    case COMP_HEALTH: {
        CHealth* h = p; // (Unreal's usual: 100 health, no shield; regenerates nothing; dies with its event only)
        h->max_health = h->health = 100.0f;
        h->regen_delay = 3.0f;
        h->destroy_delay = 3.0f;
        break;
    }
    case COMP_SAVEABLE: ((CSaveable*)p)->flags = SAVEABLE_DEFAULT; break; // (save_progress.h: place, Health, Inventory, SaveGame variables, destroyed)
    case COMP_CHECKPOINT: { // (a 4 x 4 x 2 m box; the player back at its feet; saves once, respawns after 2 s)
        CCheckpoint* c = p;
        c->extent[0] = c->extent[1] = 200.0f, c->extent[2] = 100.0f;
        c->spawn_offset[2] = -100.0f;
        c->flags = CHECKPOINT_DEFAULT;
        c->respawn_delay = 2.0f;
        break;
    }
    case COMP_NAV: {
        CNav* n = p; // (a link 2 m long along X, both ways; a modifier: an obstacle to go round)
        n->left[0] = -100.0f, n->right[0] = 100.0f;
        n->snap_radius = 60.0f;
        n->area = 2; // NAV_AREA_OBSTACLE
        break;
    }
    case COMP_INVENTORY: {
        CInventory* inv = p; // (a backpack: 20 slots, 30 kg, takes what it touches, E for the rest)
        inv->slots = 20, inv->max_weight = 30.0f, inv->auto_pickup = 1, inv->pickup_radius = 120.0f, inv->interact_key = 'E';
        break;
    }
    case COMP_INTERACTABLE: {
        CInteractable* c = p; // (a press of the Interactor's key, in its reach; anyone may use it, again and again)
        c->flags = IA_ENABLED | IA_REQ_INVENTORY | IA_REQ_TAGS;
        break;
    }
    case COMP_INTERACTOR: {
        CInteractor* c = p; // (2.5 m, 30 degrees off the view, E; outlined, the prompt on the screen, not through walls)
        c->range = 250.0f, c->angle = 30.0f, c->key = 'E', c->flags = IAR_OUTLINE | IAR_PROMPT | IAR_SIGHT;
        break;
    }
    case COMP_DEVICE: {
        CDevice* d = p; // (a door: 95 degrees in 0.6 s, away from who opens it)
        d->amount = 95.0f, d->time = 0.6f, d->flags = DEV_AWAY;
        break;
    }
    case COMP_PICKUP: {
        CPickup* pk = p;
        pk->count = 1, pk->spin = 1;
        break;
    }
    case COMP_ABILITY_SYSTEM: { // (the player's keys 1-6 for its abilities; a mana pool to start with)
        CAbilitySystem* c = p;
        c->flags = ABIL_KEYS;
        snprintf(c->attributes, sizeof c->attributes, "Mana 100, MaxMana 100");
        break;
    }
    case COMP_DIALOGUE_SPEAKER: {
        CDialogueSpeaker* s = p; // (turns to the player; the camera and the pause off: the Dialogue's)
        s->flags = DLG_SPK_FACE, s->cam_distance = 260.0f, s->cam_height = 60.0f, s->cam_angle = 25.0f;
        break;
    }
    case COMP_QUEST_LOG: {
        ((CQuestLog*)p)->flags = QL_TRACKER | QL_MARKERS | QL_AUTO_TRACK; // (quest.h: the tracker and the markers on)
        break;
    }
    case COMP_GAME_RULES: {
        CGameRules* r = p; // (a single round: 3 s countdown, 5 min counting down that lose, 100 points or the enemies win,
                           // 3 lives, back in 3 s at the Player Start, 2 s invincible, the HUD and the end screen)
        r->flags = GR_AUTO_START | GR_WIN_SCORE | GR_WIN_ELIMINATE | GR_LOSE_LIVES | GR_TIMER_DOWN | GR_RESPAWN | GR_SHOW_HUD | GR_SHOW_END;
        r->time_up = RULES_TIME_UP_LOSE, r->teams = 1, r->rounds = 1;
        r->time_limit = 300.0f, r->countdown = 3.0f, r->round_over_time = 3.0f;
        r->score_target = 100, r->kill_points = 10, r->lives = 3, r->max_deaths = 0;
        r->respawn_delay = 3.0f, r->invincible_time = 2.0f;
        snprintf(r->eliminate_tag, sizeof r->eliminate_tag, "Enemy");
        break;
    }
    case COMP_RIVER: { // (B12c: 40 m along X in four points, 6 m wide, 1.5 m deep, 1.5 m/s; narrower and faster midway)
        CRiver* r = p;
        r->npoints = 4;
        static const float pts[4][3] = { { -2000, 0, 0 }, { -700, 300, 0 }, { 700, -300, 0 }, { 2000, 0, 0 } };
        for (int i = 0; i < 4; i++) {
            for (int k = 0; k < 3; k++) r->points[i][k] = pts[i][k];
            r->width[i] = i == 1 || i == 2 ? 450.0f : 600.0f, r->depth[i] = 150.0f, r->speed[i] = i == 1 || i == 2 ? 220.0f : 150.0f;
        }
        break;
    }
    case COMP_WORLD_PARTITION: { // (B1: Unreal's defaults: 128 m cells loaded within 256 m; HLOD regions of 4 x 4 cells)
        CWorldPartition* c = p;
        c->enabled = 1, c->cell_size = 12800.0f, c->loading_range = 25600.0f, c->budget_ms = 1.0f;
        c->hlod = 1, c->hlod_region = 4, c->hlod_keep = 0.05f, c->hlod_min_size = 100.0f;
        break;
    }
    case COMP_VOLUME: { // (A9.5a: meters, Z up, 24 fps, looping, 64 cells)
        CVolume* v = p;
        v->frames = 1, v->resolution = 64, v->up_axis = 0, v->loop = 1, v->fps = 24.0f, v->unit = 100.0f;
        v->flame_temperature = 1500.0f, v->temperature_scale = 1.0f, v->density = 8.0f, v->brightness = 2500.0f, v->smoke_albedo = 0.35f;
        break;
    }
    case COMP_SPLAT: { // (A9.5d: a 3DGS file in meters, -Y up; lit by the scene)
        CSplat* s = p;
        s->up_axis = 1, s->unit = 100.0f, s->mode = 0, s->brightness = 1000.0f, s->opacity = 1.0f;
        break;
    }
    case COMP_FOG_VOLUME: { // (A9.5b: a white mist ball, as Unreal's default Local Fog Volume)
        CFogVolume* v = p;
        v->shape = 0, v->density = 0.4f, v->edge_falloff = 0.5f, v->height_falloff = 0.5f;
        v->albedo[0] = v->albedo[1] = v->albedo[2] = 0.9f;
        break;
    }
    case COMP_BUOYANCY: { // (B12f: floats as wood)
        CBuoyancy* b = p;
        b->buoyancy = 1.0f, b->water_drag = 1.2f, b->angular_drag = 1.5f, b->pontoons = 5;
        break;
    }
    case COMP_FIRE: { // (A14: a campfire, 1.2 m across, 2 m of room above it)
        CFire* c = p;
        c->preset = 1, c->resolution = 40, c->iterations = 24, c->enabled = 1;
        c->width = 120.0f, c->height = 200.0f, c->emitter_radius = 30.0f;
        c->fuel = 1.0f, c->burn = 3.0f, c->heat = 1200.0f, c->buoyancy = 6.0f, c->vorticity = 6.0f;
        c->cooling = 1.6f, c->smoke = 0.6f, c->dissipation = 0.35f, c->turbulence = 90.0f, c->brightness = 2500.0f;
        c->smoke_density = 0.08f, c->smoke_albedo = 0.3f, c->light_intensity = 4000.0f, c->light_radius = 1200.0f, c->flicker = 0.6f;
        break;
    }
    case COMP_GROOM: { // (A9.3b: medium brown, shoulder length, falling, a little wavy)
        CGroom* g = p;
        g->strands = 20000, g->segments = 14, g->seed = 1, g->preset = GROOM_LONG;
        g->length = 22.0f, g->length_var = 0.3f, g->root_width = 0.08f, g->tip_width = 0.04f;
        g->curl = 0.4f, g->curl_freq = 2.0f, g->gravity = 0.85f, g->clump = 0.45f;
        g->melanin = 0.7f, g->redness = 0.2f, g->roughness = 0.35f, g->scalp = -0.15f, g->hairline = 0.35f;
        g->dye[0] = g->dye[1] = g->dye[2] = 1.0f, g->occlusion = 0.85f, g->lod = 1;
        g->dynamic = 0, g->stiffness = 0.5f, g->damping = 0.06f, g->gusts = 0.3f, g->collide = 1, g->collision_radius = 0.6f; // (2026-11-16)
        g->visible = 1; // (2026-11-25, item 16)
        break;
    }
    case COMP_DECAL: {
        CDecal* d = p; // (everything affected, faded past 75 degrees from facing it, no fade by distance, no life span)
        d->opacity = 1.0f, d->angle_fade = 75.0f, d->fade_out = 1.0f, d->flags = DECAL_COLOR | DECAL_NORMAL | DECAL_ROUGH;
        break;
    }
    case COMP_HUD_LAYOUT: {
        CHudLayout* h = p; // (WBP_GameHUD, every piece but the minimap, UMG's scale, the safe area)
        h->flags = HUD_LAYOUT_VISIBLE | HUD_LAYOUT_HIDE_PHOTO | HUD_LAYOUT_HIDE_SEQUENCE | HUD_LAYOUT_SAFE_AREA | HUD_LAYOUT_SPEED_SPREAD;
        h->pieces = HUD_PIECE_ALL & ~HUD_PIECE_MINIMAP, h->scale = 1.0f;
        break;
    }
    case COMP_AUDIO_VOLUME: {
        CAudioVolume* v = p; // (a medium room's reverb, Unreal's defaults for the levels)
        static const float rv[8] = { 1.8f, 0.6f, 0.5f, 20.0f, 0.35f, 1.0f, 0.6f, 0.6f };
        v->enabled = 1, v->reverb_on = 1, v->fade_time = 0.5f;
        for (int k = 0; k < 8; k++) v->reverb[k] = rv[k];
        v->exterior_volume = v->interior_volume = 1.0f, v->exterior_lpf = v->interior_lpf = 20000.0f;
        break;
    }
    case COMP_STATIC_MESH: {
        CStaticMesh* m = p;
        m->base_color[0] = m->base_color[1] = m->base_color[2] = 0.6f;
        m->roughness = 0.5f;
        m->cast_shadow = 1;
        break;
    }
    case COMP_LIGHT: {
        CLight* l = p;
        l->type = LIGHT_POINT;
        l->color[0] = l->color[1] = l->color[2] = 1.0f;
        l->intensity = 1500.0f; // cd (a bright lamp, ~19 000 lm); the sun's are lux (editor.c)
        l->attenuation_radius = 1000.0f;
        l->inner_cone = 0.0f;
        l->outer_cone = 44.0f;
        l->source_width = l->source_height = 64.0f;
        l->cast_shadows = 1;
        l->temperature = 6500.0f;
        l->affects_world = 1;
        l->indirect_intensity = l->specular_scale = l->volumetric_scattering = 1.0f;
        l->far_shadow = 1, l->far_shadow_strength = 1.0f; // (Q21)
        l->ext = 1;
        break;
    }
    case COMP_PROTO: {
        CProto* pr = p;
        pr->size[0] = 400.0f, pr->size[1] = 400.0f, pr->size[2] = 300.0f, pr->thickness = 20.0f, pr->steps = 12;
        pr->opening[0] = 120.0f, pr->opening[1] = 220.0f;
        pr->color[0] = pr->color[1] = pr->color[2] = 0.8f;
        break;
    }
    case COMP_LANDSCAPE: {
        CLandscape* l = p;
        l->res = 256, l->chunks = 4, l->size = 20000.0f, l->height = 2500.0f, l->roughness = 0.5f, l->seed = 7;
        l->color[0] = 0.18f, l->color[1] = 0.3f, l->color[2] = 0.1f;
        // (its plants are a Foliage actor's: none here; an older level's density, kept, moves there when it opens)
        l->foliage_mesh = SHAPE_CONE, l->foliage_density = 0.0f, l->foliage_scale[0] = 0.5f, l->foliage_scale[1] = 1.6f;
        l->foliage_slope = 30.0f, l->wind = 0.5f;
        l->foliage_color[0] = 0.06f, l->foliage_color[1] = 0.22f, l->foliage_color[2] = 0.05f;
        break;
    }
    case COMP_FOG: ((CFog*)p)->sky_atmosphere_ambient = 1.0f; break; // (Q21: the rest the editor's g_fog_default)
    case COMP_FOLIAGE: {
        CFoliage* f = p;
        f->ntypes = 1, f->seed = 11;
        f->impostors = 1; // (Q21)
        FoliageType* t = &f->types[0]; // a slender green plant (Unreal's starter: a grass-like cone)
        t->mesh = SHAPE_CONE, t->density = 12.0f, t->scale[0] = 0.25f, t->scale[1] = 0.7f, t->stretch = 3.0f;
        t->slope = 30.0f, t->wind = 0.5f, t->paint = 1;
        t->color[0] = 0.06f, t->color[1] = 0.22f, t->color[2] = 0.05f;
        break;
    }
    case COMP_CROWD: {
        CCrowd* c = p;
        c->count = 500, c->radius = 1500.0f, c->walk_speed = 150.0f, c->run_speed = 450.0f;
        c->behavior = CROWD_SURROUND, c->mind = CROWD_HIVE;
        c->sight = 2500.0f, c->memory = 5.0f, c->prediction = 1.0f, c->reaction = 0.4f, c->aggression = 0.35f;
        c->ring_radius = 400.0f, c->separation = 70.0f, c->variety = 0.25f, c->seed = 1, c->looks = 1.0f; // (Q27.9)
        break;
    }
    case COMP_AI_AGENT: {
        CAIAgent* a = p;
        a->sight_radius = 1500.0f, a->sight_angle = 120.0f, a->hearing_radius = 1200.0f, a->lose_sight_time = 6.0f;
        a->team = 1;
        a->prediction = 0.8f, a->reaction = 0.2f, a->coordination = 0.6f, a->share_radius = 2500.0f, a->surround_radius = 350.0f;
        break;
    }
    case COMP_SKELETAL_MESH: {
        CSkeletalMesh* s = p;
        s->play_rate = 1.0f, s->loop = 1, s->physics_blend = 1.0f;
        break;
    }
    case COMP_RIGID_BODY: {
        CRigidBody* b = p;
        b->simulate = 1, b->enable_gravity = 1;
        b->linear_damping = 0.01f, b->angular_damping = 0.05f;
        b->friction = 0.6f, b->restitution = 0.2f;
        b->fracture_speed = 900.0f, b->fracture_pieces = 3;
        break;
    }
    case COMP_ATTACH: {
        CAttach* a = p;
        a->scale[0] = a->scale[1] = a->scale[2] = 1.0f;
        break;
    }
    case COMP_SPRING_ARM: {
        CSpringArm* s = p;
        s->target_arm_length = 300.0f;
        s->do_collision_test = 1;
        s->camera_lag_speed = 10.0f;
        s->use_pawn_control_rotation = 1;
        s->probe_size = 12.0f;
        s->current_length = 300.0f;
        break;
    }
    case COMP_COLLISION: {
        CCollision* c = p;
        c->extent[0] = c->extent[1] = c->extent[2] = 32.0f;
        c->generate_overlap_events = 1;
        break;
    }
    case COMP_PROJECTILE: {
        CProjectile* q = p;
        q->initial_speed = 3000.0f;
        q->gravity_scale = 1.0f;
        q->bounciness = 0.6f;
        q->friction = 0.2f;
        q->rotation_follows_velocity = 1;
        q->active = 1;
        q->velocity[0] = 1.0f;
        break;
    }
    case COMP_FLOATING: {
        CFloating* fl = p;
        fl->max_speed = 1200.0f;
        fl->acceleration = 4000.0f;
        fl->deceleration = 8000.0f;
        break;
    }
    case COMP_ARROW: {
        CArrow* a = p;
        a->color[0] = 1.0f, a->color[1] = 0.25f, a->color[2] = 0.1f;
        a->length = 80.0f;
        break;
    }
    case COMP_CAMERA: {
        CCamera* cam = p;
        cam->fov_deg = 90.0f;
        break;
    }
    default: break;
    }
}

static void init_defaults(EcsArchetype* a, uint32_t row) {
    for (int c = 0; c < COMP_COUNT; c++)
        if (a->mask & COMP_BIT(c)) ecs_component_defaults((ComponentId)c, a->columns[c].base + (size_t)g_components[c].size * row);
}

Entity ecs_create(World* w, uint64_t mask) {
    int ai = find_archetype(w, mask);
    if (ai < 0) return ENTITY_NONE;
    uint32_t index;
    bool reused = w->free_count > 0;
    if (reused) {
        index = ((uint32_t*)w->free_list.base)[--w->free_count];
    } else {
        if (w->slot_count >= RE_ECS_MAX_ENTITIES || !re_vm_ensure(&w->slots, sizeof(EcsSlot) * (w->slot_count + 1)))
            return ENTITY_NONE;
        index = w->slot_count++;
        ecs_slots(w)[index].gen = 0;
    }
    EcsSlot* s = &ecs_slots(w)[index];
    Entity e = index | (s->gen << RE_ECS_INDEX_BITS);
    EcsArchetype* a = &w->archetypes[ai];
    int64_t row = push_row(a, e);
    if (row < 0) { // the slot goes back where it came from
        s->archetype = ECS_NO_ARCHETYPE;
        if (reused) w->free_count++;
        else w->slot_count--;
        return ENTITY_NONE;
    }
    s->archetype = (uint16_t)ai;
    s->row = (uint32_t)row;
    init_defaults(a, (uint32_t)row);
    w->alive++;
    ecs_touch(w, e);
    return e;
}

bool ecs_alive(const World* w, Entity e) {
    uint32_t i = entity_index(e);
    if (i == 0 || i >= w->slot_count) return false;
    const EcsSlot* s = &ecs_slots(w)[i];
    return s->archetype != ECS_NO_ARCHETYPE && s->gen == entity_gen(e);
}

void ecs_destroy(World* w, Entity e) {
    if (!ecs_alive(w, e)) return;
    uint32_t i = entity_index(e);
    EcsSlot* s = &ecs_slots(w)[i];
    remove_row(w, &w->archetypes[s->archetype], s->row);
    s->archetype = ECS_NO_ARCHETYPE;
    s->gen = (s->gen + 1) & GEN_MASK;
    ecs_touch(w, e); // its slot, now empty
    if (re_vm_ensure(&w->free_list, sizeof(uint32_t) * (w->free_count + 1)))
        ((uint32_t*)w->free_list.base)[w->free_count++] = i;
    w->alive--;
}

uint64_t ecs_mask(const World* w, Entity e) {
    return ecs_alive(w, e) ? w->archetypes[ecs_slots(w)[entity_index(e)].archetype].mask : 0;
}

void* ecs_get(World* w, Entity e, ComponentId c) {
    if (!ecs_alive(w, e)) return NULL;
    EcsSlot* s = &ecs_slots(w)[entity_index(e)];
    EcsArchetype* a = &w->archetypes[s->archetype];
    if (!(a->mask & COMP_BIT(c))) return NULL;
    return a->columns[c].base + (size_t)g_components[c].size * s->row;
}

static bool move_to(World* w, Entity e, uint64_t new_mask) {
    EcsSlot* s = &ecs_slots(w)[entity_index(e)];
    int to_i = find_archetype(w, new_mask);
    if (to_i < 0) return false;
    EcsArchetype* from = &w->archetypes[s->archetype];
    EcsArchetype* to = &w->archetypes[to_i];
    int64_t row = push_row(to, e);
    if (row < 0) return false;
    init_defaults(to, (uint32_t)row);
    for (int c = 0; c < COMP_COUNT; c++) {
        if (!(from->mask & to->mask & COMP_BIT(c))) continue;
        size_t size = g_components[c].size;
        memcpy(to->columns[c].base + size * (size_t)row, from->columns[c].base + size * s->row, size);
    }
    remove_row(w, from, s->row);
    s->archetype = (uint16_t)to_i;
    s->row = (uint32_t)row;
    ecs_touch(w, e);
    return true;
}

bool ecs_add(World* w, Entity e, ComponentId c) {
    uint64_t m = ecs_mask(w, e);
    if (!ecs_alive(w, e)) return false;
    return (m & COMP_BIT(c)) ? true : move_to(w, e, m | COMP_BIT(c));
}

bool ecs_remove(World* w, Entity e, ComponentId c) {
    uint64_t m = ecs_mask(w, e);
    if (!ecs_alive(w, e)) return false;
    return (m & COMP_BIT(c)) ? move_to(w, e, m & ~COMP_BIT(c)) : true;
}

Entity ecs_clone(World* w, Entity e) {
    uint64_t m = ecs_mask(w, e);
    if (!m) return ENTITY_NONE;
    Entity n = ecs_create(w, m);
    if (n == ENTITY_NONE) return n;
    for (int c = 0; c < COMP_COUNT; c++)
        if (m & COMP_BIT(c)) memcpy(ecs_get(w, n, (ComponentId)c), ecs_get(w, e, (ComponentId)c), g_components[c].size);
    CSkeletalMesh* sk = (m & COMP_BIT(COMP_SKELETAL_MESH)) ? ecs_get(w, n, COMP_SKELETAL_MESH) : NULL;
    if (sk) sk->built = 0, sk->inst = 0; // (the run's fields: the copy makes its own parts and instance)
    return n;
}

Entity ecs_entity_at(const World* w, uint32_t index) {
    if (index == 0 || index >= w->slot_count) return ENTITY_NONE;
    const EcsSlot* s = &ecs_slots(w)[index];
    return s->archetype == ECS_NO_ARCHETYPE ? ENTITY_NONE : (index | (s->gen << RE_ECS_INDEX_BITS));
}

// --- Snapshot ---

static bool copy_into(ReVmBuf* dst, const void* src, size_t bytes, size_t reserve) {
    if (bytes == 0) return true;
    if (!dst->base && !re_vm_reserve(dst, reserve)) return false;
    if (!re_vm_ensure(dst, bytes)) return false;
    memcpy(dst->base, src, bytes);
    return true;
}

bool ecs_snapshot_save(const World* w, WorldSnapshot* s) {
    uint64_t bytes = 0;
    size_t n = sizeof(EcsSlot) * w->slot_count;
    if (!copy_into(&s->slots, w->slots.base, n, sizeof(EcsSlot) * RE_ECS_MAX_ENTITIES)) return false;
    bytes += n;
    n = sizeof(uint32_t) * w->free_count;
    if (!copy_into(&s->free_list, w->free_list.base, n, sizeof(uint32_t) * RE_ECS_MAX_ENTITIES)) return false;
    bytes += n;
    for (uint32_t a = 0; a < w->archetype_count; a++) {
        const EcsArchetype* arch = &w->archetypes[a];
        n = sizeof(Entity) * arch->count;
        if (!copy_into(&s->entities[a], arch->entities.base, n, sizeof(Entity) * RE_ECS_MAX_ENTITIES)) return false;
        bytes += n;
        for (int c = 0; c < COMP_COUNT; c++) {
            if (!(arch->mask & COMP_BIT(c))) continue;
            n = (size_t)g_components[c].size * arch->count;
            if (!copy_into(&s->columns[a][c], arch->columns[c].base, n, (size_t)g_components[c].size * RE_ECS_MAX_ENTITIES))
                return false;
            bytes += n;
        }
        s->counts[a] = arch->count;
    }
    s->slot_count = w->slot_count;
    s->free_count = w->free_count;
    s->alive = w->alive;
    s->archetype_count = w->archetype_count;
    s->bytes = bytes;
    return true;
}

void ecs_snapshot_restore(World* w, const WorldSnapshot* s) {
    // The world only grew its reservations' commits since the save, so every copy back fits.
    memcpy(w->slots.base, s->slots.base, sizeof(EcsSlot) * s->slot_count);
    if (s->free_count) memcpy(w->free_list.base, s->free_list.base, sizeof(uint32_t) * s->free_count);
    for (uint32_t a = 0; a < w->archetype_count; a++) {
        EcsArchetype* arch = &w->archetypes[a];
        uint32_t count = a < s->archetype_count ? s->counts[a] : 0; // archetypes made while playing end empty
        if (count) {
            memcpy(arch->entities.base, s->entities[a].base, sizeof(Entity) * count);
            for (int c = 0; c < COMP_COUNT; c++)
                if (arch->mask & COMP_BIT(c))
                    memcpy(arch->columns[c].base, s->columns[a][c].base, (size_t)g_components[c].size * count);
        }
        arch->count = count;
    }
    w->slot_count = s->slot_count;
    w->free_count = s->free_count;
    w->alive = s->alive;
    ecs_touch_all(w);
}

void ecs_snapshot_free(WorldSnapshot* s) {
    re_vm_release(&s->slots);
    re_vm_release(&s->free_list);
    for (int a = 0; a < RE_ECS_MAX_ARCHETYPES; a++) {
        re_vm_release(&s->entities[a]);
        for (int c = 0; c < COMP_COUNT; c++) re_vm_release(&s->columns[a][c]);
    }
    memset(s, 0, sizeof *s);
}

// --- Compact blob (undo history, level files) ---

typedef struct BlobWriter {
    uint8_t* p;
    size_t n;
} BlobWriter;

static void put(BlobWriter* b, const void* src, size_t bytes) {
    if (b->p && bytes) memcpy(b->p + b->n, src, bytes);
    b->n += bytes;
}
static void put32(BlobWriter* b, uint32_t v) { put(b, &v, 4); }

static size_t write_blob(const World* w, uint8_t* dst) {
    BlobWriter b = { dst, 0 };
    put32(&b, ECS_BLOB_MAGIC);
    put32(&b, ECS_BLOB_VERSION);
    put32(&b, COMP_COUNT);
    for (int c = 0; c < COMP_COUNT; c++) put32(&b, g_components[c].size);
    put32(&b, w->slot_count);
    put32(&b, w->free_count);
    put32(&b, w->alive);
    uint32_t live = 0; // archetypes with entities
    for (uint32_t a = 0; a < w->archetype_count; a++) live += w->archetypes[a].count > 0;
    put32(&b, live);
    put(&b, w->slots.base, sizeof(EcsSlot) * w->slot_count);
    put(&b, w->free_list.base, sizeof(uint32_t) * w->free_count);
    for (uint32_t a = 0; a < w->archetype_count; a++) {
        const EcsArchetype* arch = &w->archetypes[a];
        if (!arch->count) continue;
        put32(&b, a); // the index the slots refer to
        put(&b, &arch->mask, 8);
        put32(&b, arch->count);
        put(&b, arch->entities.base, sizeof(Entity) * arch->count);
        for (int c = 0; c < COMP_COUNT; c++)
            if (arch->mask & COMP_BIT(c)) put(&b, arch->columns[c].base, (size_t)g_components[c].size * arch->count);
    }
    return b.n;
}

size_t ecs_serialized_size(const World* w) { return write_blob(w, NULL); }
size_t ecs_serialize(const World* w, uint8_t* dst) { return write_blob(w, dst); }

typedef struct BlobReader {
    const uint8_t* p;
    size_t n, at;
    bool ok;
} BlobReader;

static const void* take(BlobReader* r, size_t bytes) {
    if (!r->ok || r->at + bytes > r->n) {
        r->ok = false;
        return NULL;
    }
    const void* p = r->p + r->at;
    r->at += bytes;
    return p;
}
static uint32_t take32(BlobReader* r) {
    const void* p = take(r, 4);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

// A row read at an older (smaller) size: the fields it had keep their meaning where a newer field's default would change it
static void component_upgrade(ComponentId c, void* p, uint32_t old_size) {
    if (c == COMP_RAIL && old_size <= offsetof(CRail, kind)) ((CRail*)p)->kind = SPLINE_KIND_RAIL; // (Q14) a Rail from before the Spline
}

bool ecs_deserialize(World* w, const uint8_t* src, size_t size) {
    // A first pass checks everything before the world is touched
    for (int pass = 0; pass < 2; pass++) {
        BlobReader r = { src, size, 0, true };
        // a blob from before newer component types (they are added at the end): its first ncomp sizes must match
        if (take32(&r) != ECS_BLOB_MAGIC || take32(&r) != ECS_BLOB_VERSION) return false;
        uint32_t ncomp = take32(&r);
        if (ncomp == 0 || ncomp > COMP_COUNT) return false;
        // a component that grew since (new fields at its end): its rows are read at the old size, the rest gets the
        // defaults (init_component); a smaller one than the blob's is a newer engine's: refused
        uint32_t sizes[COMP_COUNT];
        for (uint32_t c = 0; c < COMP_COUNT; c++) sizes[c] = g_components[c].size;
        for (uint32_t c = 0; c < ncomp; c++) {
            sizes[c] = take32(&r);
            if (sizes[c] == 0 || sizes[c] > g_components[c].size) return false;
        }
        uint32_t slot_count = take32(&r), free_count = take32(&r), alive = take32(&r), narch = take32(&r);
        if (slot_count == 0 || slot_count > RE_ECS_MAX_ENTITIES || free_count > slot_count) return false;
        const EcsSlot* slots = take(&r, sizeof(EcsSlot) * slot_count);
        const uint32_t* free_list = take(&r, sizeof(uint32_t) * free_count);
        if (!r.ok) return false;
        uint16_t remap[RE_ECS_MAX_ARCHETYPES];
        for (int i = 0; i < RE_ECS_MAX_ARCHETYPES; i++) remap[i] = ECS_NO_ARCHETYPE;
        if (pass == 1) {
            for (uint32_t a = 0; a < w->archetype_count; a++) w->archetypes[a].count = 0;
            if (!re_vm_ensure(&w->slots, sizeof(EcsSlot) * slot_count) ||
                !re_vm_ensure(&w->free_list, sizeof(uint32_t) * (free_count + 1)))
                return false;
        }
        for (uint32_t i = 0; i < narch; i++) {
            uint32_t index = take32(&r);
            const void* mp = take(&r, 8);
            uint32_t count = take32(&r);
            uint64_t mask = 0;
            if (mp) memcpy(&mask, mp, 8);
            if (!r.ok || index >= RE_ECS_MAX_ARCHETYPES || count > RE_ECS_MAX_ENTITIES || (ncomp < 64 && (mask >> ncomp))) return false; // (a shift by 64: undefined)
            const Entity* ents = take(&r, sizeof(Entity) * count);
            const uint8_t* cols[COMP_COUNT] = { 0 };
            for (int c = 0; c < COMP_COUNT; c++)
                if (mask & COMP_BIT(c)) cols[c] = take(&r, (size_t)sizes[c] * count);
            if (!r.ok) return false;
            if (pass == 0) continue;
            int ai = find_archetype(w, mask);
            if (ai < 0) return false;
            remap[index] = (uint16_t)ai;
            EcsArchetype* arch = &w->archetypes[ai];
            if (!re_vm_ensure(&arch->entities, sizeof(Entity) * count)) return false;
            memcpy(arch->entities.base, ents, sizeof(Entity) * count);
            for (int c = 0; c < COMP_COUNT; c++) {
                if (!(mask & COMP_BIT(c))) continue;
                size_t size = g_components[c].size, bytes = size * count;
                if (!re_vm_ensure(&arch->columns[c], bytes)) return false;
                if (sizes[c] == size) {
                    memcpy(arch->columns[c].base, cols[c], bytes);
                } else { // grown: the defaults, then the old fields over them
                    for (uint32_t row = 0; row < count; row++) {
                        uint8_t* dst = arch->columns[c].base + size * row;
                        memset(dst, 0, size);
                        ecs_component_defaults((ComponentId)c, dst);
                        memcpy(dst, cols[c] + (size_t)sizes[c] * row, sizes[c]);
                        component_upgrade((ComponentId)c, dst, sizes[c]);
                    }
                }
            }
            arch->count = count;
        }
        if (pass == 1) {
            EcsSlot* ws = ecs_slots(w);
            memcpy(ws, slots, sizeof(EcsSlot) * slot_count);
            for (uint32_t i = 0; i < slot_count; i++)
                if (ws[i].archetype != ECS_NO_ARCHETYPE) ws[i].archetype = remap[ws[i].archetype < RE_ECS_MAX_ARCHETYPES ? ws[i].archetype : 0];
            if (free_count) memcpy(w->free_list.base, free_list, sizeof(uint32_t) * free_count);
            w->slot_count = slot_count;
            w->free_count = free_count;
            w->alive = alive;
            ecs_touch_all(w);
        }
    }
    return true;
}
