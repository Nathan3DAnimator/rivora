// The Horror demo's game mode (sugestão #29; included by demo_play.c). An abandoned clinic at night, the power cut;
// the way out is an electric gate. To open it (a chain the player has to think through):
//   1. the generator room needs three fuses: one in the infirmary (on the medical box), one in the storeroom, and one
//      locked in the grandfather clock of the library — a note says she died "at three in the morning... the old clock
//      still remembers": set the clock to 3 (E advances its hands) and its case opens;
//   2. with the fuses in, the four breakers must be thrown in the order the office note gives (II, IV, I, III); a wrong
//      one trips them all back;
//   3. the power comes back: the house lights come on, the gate opens... and what walks in the house hears it all.
// Meanwhile something hunts by sight and by noise (running, Left Shift, is loud): caught, you wake at the last
// checkpoint (what you found is kept). F: the flashlight. E: the thing in front of you (its name on the prompt).

typedef enum { HO_FUSE = 0, HO_CLOCK, HO_NOTE, HO_GENERATOR, HO_BREAKER, HO_EXIT } HorrorKind;
typedef struct HorrorThing { const char* name; int kind, arg; const char* prompt; const char* text; } HorrorThing;
static const HorrorThing g_things[] = {
    { "Fuse1", HO_FUSE, 0, "Take the fuse", NULL },
    { "Fuse2", HO_FUSE, 1, "Take the fuse", NULL },
    { "Fuse3", HO_FUSE, 2, "Take the fuse", NULL },
    { "Clock", HO_CLOCK, 0, "Move the clock's hands", NULL },
    { "NoteFoyer", HO_NOTE, 0, "Read the note",
      "The power is out again. Someone pulled the fuses from the generator - all three.\nThe generator room is at the east end of the corridor.\nDon't run in the halls. It hears you." },
    { "NoteLibrary", HO_NOTE, 1, "Read the diary page",
      "She died at three in the morning. Since then the old clock in this room\nhas stood still, and its case stays locked.\nThe old clock still remembers." },
    { "NoteOffice", HO_NOTE, 2, "Read the maintenance card",
      "BREAKER PANEL - restart after a cut:\nthrow them in this order:  II,  IV,  I,  III\nThe wrong order trips the whole panel." },
    { "Generator", HO_GENERATOR, 0, "Put the fuses in", NULL },
    { "Breaker1", HO_BREAKER, 1, "Throw breaker I", NULL },
    { "Breaker2", HO_BREAKER, 2, "Throw breaker II", NULL },
    { "Breaker3", HO_BREAKER, 3, "Throw breaker III", NULL },
    { "Breaker4", HO_BREAKER, 4, "Throw breaker IV", NULL },
    { "Exit", HO_EXIT, 0, "Leave", NULL },
};
#define HO_THINGS (sizeof g_things / sizeof g_things[0])
static const int g_breaker_order[4] = { 2, 4, 1, 3 };

static struct {
    Entity hud, player, monster, flash, gate;
    Entity things[HO_THINGS];
    Entity power_lights[24];
    float power_int[24];
    uint32_t npower;
    bool fuse[3], fuses_in, power, flash_on, won, note_open;
    int clock_hour, breakers; // breakers: how many thrown in order
    float msg_t, caught_t, auto_t, monster_start[3], checkpoint[3];
    int focus, auto_step, catches;
    float bars_t, scare_cool; // (Q13) the letterbox's time left; the next tension scare not before
    bool scared;
} HO;

// (Q13) an actor and its parts (a character's skeletal parts, its eyes) on these Lighting Channels
static void horror_channels(World* w, Entity e, uint32_t mask) {
    CStaticMesh* m = e ? ecs_get(w, e, COMP_STATIC_MESH) : NULL;
    if (m) m->light_channels = mask, ecs_touch(w, e);
    for (uint32_t ai = 0; e && ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_ATTACH)) || !(a->mask & COMP_BIT(COMP_STATIC_MESH))) continue;
        const CAttach* at = ecs_arch_column(a, COMP_ATTACH);
        CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
        for (uint32_t i = 0; i < a->count; i++)
            if (at[i].owner == e) sm[i].light_channels = mask, ecs_touch(w, ecs_arch_entities(a)[i]);
    }
}

static void horror_msg(const char* text, float secs) {
    if (!HO.hud) return;
    ui_set_text(HO.hud, "Message", text);
    ui_set_visible(HO.hud, "Message", true);
    HO.msg_t = secs;
}

static void horror_objective(void) {
    if (!HO.hud) return;
    int fuses = HO.fuse[0] + HO.fuse[1] + HO.fuse[2];
    const char* obj = HO.won ? "You escaped."
                    : HO.power ? "The gate is open: get out - east, past the corridor."
                    : HO.fuses_in ? "Throw the four breakers in the right order."
                    : fuses == 3 ? "Take the fuses to the generator room (east)."
                                 : "Find the three fuses for the generator.";
    ui_set_text(HO.hud, "Objective", obj);
    char b[64];
    snprintf(b, sizeof b, "Fuses: %d / 3%s", HO.fuses_in ? 3 : fuses, HO.fuses_in ? " (in the generator)" : "");
    ui_set_text(HO.hud, "Items", b);
}

static void horror_power_on(World* w) {
    HO.power = true;
    for (uint32_t i = 0; i < HO.npower; i++) {
        CLight* l = ecs_get(w, HO.power_lights[i], COMP_LIGHT);
        if (l) l->intensity = HO.power_int[i], ecs_touch(w, HO.power_lights[i]);
    }
    if (HO.gate && ecs_alive(w, HO.gate)) ecs_destroy(w, HO.gate), HO.gate = ENTITY_NONE;
    CCharacter* mc = HO.monster ? ecs_get(w, HO.monster, COMP_CHARACTER) : NULL;
    if (mc) mc->max_walk_speed *= 1.25f; // (it heard the generator)
    if (HO.player) {
        const CTransform* t = ecs_get(w, HO.player, COMP_TRANSFORM);
        if (t) ai_report_noise(w, t->location, 1.0f, HO.player);
    }
    horror_msg("The generator roars. The lights flicker on... and the gate opens. Something is coming.", 5.0f);
    // (Q13) the beat: the bars close, the lens stretches the corridor, the tension jumps
    terror_set_letterbox(2.2f, 0.8f), HO.bars_t = 5.0f;
    terror_scare_rig(TR_DOLLY_ZOOM, 1.0f, 2.5f);
    terror_add_tension(0.4f, 0.5f);
}

static void horror_interact(World* w, int i) {
    const HorrorThing* th = &g_things[i];
    Entity e = HO.things[i];
    char b[256];
    switch (th->kind) {
    case HO_FUSE:
        HO.fuse[th->arg] = true;
        if (e && ecs_alive(w, e)) ecs_destroy(w, e);
        HO.things[i] = ENTITY_NONE;
        snprintf(b, sizeof b, "A fuse. (%d of 3)", HO.fuse[0] + HO.fuse[1] + HO.fuse[2]);
        horror_msg(b, 2.5f);
        break;
    case HO_CLOCK:
        HO.clock_hour = HO.clock_hour % 12 + 1;
        if (HO.clock_hour == 3) {
            Entity f3 = dp_find(w, "Fuse3");
            CTransform* ft = f3 ? ecs_get(w, f3, COMP_TRANSFORM) : NULL;
            if (ft && (ft->flags & TRANSFORM_HIDDEN_GAME)) {
                ft->flags &= ~TRANSFORM_HIDDEN_GAME, ecs_touch(w, f3);
                horror_msg("The hands stop at three. Something clicks inside the case: it swings open. A fuse.", 4.0f);
                terror_filter_trigger(TF_BW_BLOWN, 1.0f, 0.04f, 0.8f, 0.6f); // (Q13) the snap to a blown-out black and white: a memory
                break;
            }
        }
        snprintf(b, sizeof b, "The hands move. The clock shows %d o'clock.", HO.clock_hour);
        horror_msg(b, 2.0f);
        break;
    case HO_NOTE:
        HO.note_open = true;
        ui_set_text(HO.hud, "NoteText", th->text);
        ui_set_visible(HO.hud, "NoteBack", true), ui_set_visible(HO.hud, "NoteText", true);
        break;
    case HO_GENERATOR:
        if (HO.fuses_in) horror_msg(HO.power ? "It runs." : "The fuses are in. It needs the breakers.", 2.5f);
        else if (HO.fuse[0] && HO.fuse[1] && HO.fuse[2]) HO.fuses_in = true, horror_msg("Three fuses, in. Now the breaker panel.", 3.0f);
        else horror_msg("Three fuse slots, empty. You don't have them all.", 2.5f);
        break;
    case HO_BREAKER:
        if (HO.power) break;
        if (!HO.fuses_in) {
            horror_msg("Clack. Nothing - the generator has no fuses.", 2.0f);
            break;
        }
        if (th->arg == g_breaker_order[HO.breakers]) {
            HO.breakers++;
            snprintf(b, sizeof b, "Clack. (%d of 4)", HO.breakers);
            horror_msg(b, 1.5f);
            if (HO.breakers == 4) horror_power_on(w);
        } else {
            HO.breakers = 0;
            horror_msg("CLACK - the whole panel trips back. Wrong order.", 2.5f);
            if (HO.player) {
                const CTransform* t = ecs_get(w, HO.player, COMP_TRANSFORM);
                if (t) ai_report_noise(w, t->location, 0.6f, HO.player);
            }
        }
        break;
    case HO_EXIT:
        if (!HO.power) {
            horror_msg("The gate is shut - an electric lock. No power.", 2.5f);
            break;
        }
        HO.won = true;
        terror_set_tension(0.0f, 2.0f), terror_dip((const float[3]){ 1, 1, 1 }, 1.5f, 2.0f, 0.0f); // (Q13: out into the light)
        ui_set_text(HO.hud, "Banner", "YOU ESCAPED");
        ui_set_visible(HO.hud, "Banner", true);
        break;
    }
    horror_objective();
}

static void horror_begin(World* w) {
    memset(&HO, 0, sizeof HO);
    HO.player = bp_player(w);
    HO.monster = dp_find(w, "Monster");
    HO.flash = dp_find(w, "Flashlight");
    HO.gate = dp_find(w, "ExitGate");
    HO.flash_on = true, HO.focus = -1, HO.clock_hour = 9;
    for (size_t i = 0; i < HO_THINGS; i++) HO.things[i] = dp_find(w, g_things[i].name);
    if (HO.monster) memcpy(HO.monster_start, ((CTransform*)ecs_get(w, HO.monster, COMP_TRANSFORM))->location, 12);
    // the house's lights wait for the power (their intensity kept, 0 until then)
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_NAME)) || !(a->mask & COMP_BIT(COMP_LIGHT))) continue;
        const CName* n = ecs_arch_column(a, COMP_NAME);
        CLight* l = ecs_arch_column(a, COMP_LIGHT);
        for (uint32_t r = 0; r < a->count && HO.npower < 24; r++)
            if (!strncmp(n[r].text, "PowerLight", 10)) {
                HO.power_lights[HO.npower] = ecs_arch_entities(a)[r];
                HO.power_int[HO.npower++] = l[r].intensity;
                l[r].intensity = 0.0f;
            }
    }
    HO.hud = dp_hud(w, "WBP_HorrorHUD");
    // (Q13) the horror kit: the creature on Lighting Channels 0 and 1 (its Rim Light is channel 1 alone), its eyes too; the
    // story opens from black between the bars
    if (HO.monster) horror_channels(w, HO.monster, 3u);
    horror_channels(w, dp_find(w, "MonsterEyeL"), 3u), horror_channels(w, dp_find(w, "MonsterEyeR"), 3u);
    terror_dip((const float[3]){ 0, 0, 0 }, 0.0f, 0.6f, 1.6f);
    terror_set_letterbox(2.2f, 0.0f), HO.bars_t = 4.0f;
    horror_objective();
    horror_msg("The power is out. Find a way out of the clinic. (F: flashlight, E: use, Shift: run - it hears you)", 6.0f);
    const char* ap = getenv("REALENGINE_DEMO_AUTOPLAY");
    HO.auto_step = ap && ap[0] == '1' ? 0 : -1;
}

// The tests: the whole chain solved by the same interactions, one step every 0.3 s (the player teleported to each)
static void horror_autoplay(World* w, float dt) {
    static const char* const steps[] = { "NoteFoyer", "Fuse1", "Fuse2", "NoteLibrary", "Clock", "Clock", "Clock", "Clock", "Clock", "Clock",
                                         "Fuse3", "NoteOffice", "Generator", "Breaker2", "Breaker4", "Breaker1", "Breaker3", "Exit" };
    if (HO.auto_step < 0 || HO.auto_step >= (int)(sizeof steps / sizeof steps[0]) || (HO.auto_t -= dt) > 0.0f) return;
    HO.auto_t = 0.3f;
    for (size_t i = 0; i < HO_THINGS; i++)
        if (!strcmp(g_things[i].name, steps[HO.auto_step])) {
            if (g_things[i].kind == HO_FUSE && i == 2) HO.things[2] = dp_find(w, "Fuse3");
            if (g_things[i].kind == HO_NOTE) HO.note_open = false;
            horror_interact(w, (int)i);
        }
    HO.auto_step++;
}

static void horror_tick(World* w, float dt) {
    if (!HO.player || !ecs_alive(w, HO.player)) HO.player = bp_player(w);
    CTransform* pt = HO.player ? ecs_get(w, HO.player, COMP_TRANSFORM) : NULL;
    const CCharacter* pc = HO.player ? ecs_get(w, HO.player, COMP_CHARACTER) : NULL;
    if (!pt || !pc) return;
    if (HO.msg_t > 0.0f && (HO.msg_t -= dt) <= 0.0f) ui_set_visible(HO.hud, "Message", false);
    horror_autoplay(w, dt);
    // (Q13) the Tension Director: how near the creature is (a floor of fear once the power is on), the bars opening after
    // a beat, a whip and a fringe when it closes in
    if (HO.bars_t > 0.0f && (HO.bars_t -= dt) <= 0.0f) terror_set_letterbox(0.0f, 1.2f);
    const CTransform* mt0 = HO.monster ? ecs_get(w, HO.monster, COMP_TRANSFORM) : NULL;
    if (mt0 && !HO.won) {
        float dx = mt0->location[0] - pt->location[0], dy = mt0->location[1] - pt->location[1], d = sqrtf(dx * dx + dy * dy);
        float t = fminf(fmaxf((1500.0f - d) / 1200.0f, 0.0f), 1.0f);
        terror_set_tension(fmaxf(t, HO.power ? 0.3f : 0.0f), -1.0f);
        HO.scare_cool -= dt;
        if (terror_tension() > 0.75f && !HO.scared && HO.scare_cool <= 0.0f) {
            HO.scared = true, HO.scare_cool = 10.0f;
            terror_scare_rig(TR_WHIP_PAN, 0.6f, 0.0f);
            terror_filter_trigger(TF_CHROMATIC, 0.8f, 0.0f, 0.2f, 0.6f);
        } else if (terror_tension() < 0.5f)
            HO.scared = false;
    }
    // the flashlight at the eyes, along the view; F turns it on and off
    if (dp_pressed(70 /* F */)) HO.flash_on = !HO.flash_on;
    CTransform* ft = HO.flash ? ecs_get(w, HO.flash, COMP_TRANSFORM) : NULL;
    CLight* fl = HO.flash ? ecs_get(w, HO.flash, COMP_LIGHT) : NULL;
    if (ft && fl) {
        ft->location[0] = pt->location[0], ft->location[1] = pt->location[1], ft->location[2] = pt->location[2] + 60.0f;
        ft->rotation[0] = pc->control_pitch, ft->rotation[1] = pc->control_yaw;
        fl->intensity = HO.flash_on ? 900.0f : 0.0f; // (the exposure is held for the night: brighter blew the cone out)
        // (no ecs_touch: a Movable light is compared each frame by the renderer; a touch made the static TLAS refit every frame)
    }
    // running is loud
    if (bp_key_held(340 /* Left Shift */) && (fabsf(pc->velocity[0]) + fabsf(pc->velocity[1])) > 300.0f) ai_report_noise(w, pt->location, 0.8f, HO.player);
    // a note open: E or Esc closes it
    if (HO.note_open) {
        if (dp_pressed(69 /* E */) || dp_pressed(256 /* Esc */)) {
            HO.note_open = false;
            ui_set_visible(HO.hud, "NoteBack", false), ui_set_visible(HO.hud, "NoteText", false);
        }
        return;
    }
    // what is in front: the nearest thing within 2 m, toward the view
    float fwd[2] = { cosf(pc->control_yaw * DP_DEG), sinf(pc->control_yaw * DP_DEG) }, best = 1e9f;
    int focus = -1;
    for (size_t i = 0; i < HO_THINGS; i++) {
        Entity e = HO.things[i];
        const CTransform* t = e && ecs_alive(w, e) ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (!t || (t->flags & TRANSFORM_HIDDEN_GAME)) continue;
        float dx = t->location[0] - pt->location[0], dy = t->location[1] - pt->location[1], d = sqrtf(dx * dx + dy * dy);
        if (d > 200.0f || (d > 60.0f && (dx * fwd[0] + dy * fwd[1]) < 0.5f * d)) continue;
        if (d < best) best = d, focus = (int)i;
    }
    if (focus != HO.focus) {
        HO.focus = focus;
        char b[96];
        snprintf(b, sizeof b, "E  -  %s", focus >= 0 ? g_things[focus].prompt : "");
        ui_set_text(HO.hud, "Prompt", b);
        ui_set_visible(HO.hud, "Prompt", focus >= 0);
    }
    if (focus >= 0 && dp_pressed(69 /* E */)) horror_interact(w, focus);
    // caught: you wake at the last checkpoint (the foyer, or the generator room once the power is on)
    const CTransform* mt = HO.monster ? ecs_get(w, HO.monster, COMP_TRANSFORM) : NULL;
    if (mt && !HO.won && HO.auto_step < 0) {
        float dx = mt->location[0] - pt->location[0], dy = mt->location[1] - pt->location[1];
        if (dx * dx + dy * dy < 85.0f * 85.0f && fabsf(mt->location[2] - pt->location[2]) < 150.0f) {
            HO.catches++;
            const float start[3] = { -300.0f, -200.0f, 100.0f }, gen[3] = { 2100.0f, 600.0f, 100.0f };
            memcpy(pt->location, HO.power ? gen : start, 12);
            CTransform* m = ecs_get(w, HO.monster, COMP_TRANSFORM);
            memcpy(m->location, HO.monster_start, 12);
            ecs_touch(w, HO.player), ecs_touch(w, HO.monster);
            horror_msg("It caught you. ... You wake up, your heart pounding.", 4.0f);
            // (Q13) the shock: a white frame, the hallucination, the horizon tipping, then black and waking
            terror_filter_trigger(TF_WHITE_FLASH, 1.0f, 0.0f, 0.04f, 0.12f);
            terror_filter_trigger(TF_HALLUCINATION, 1.0f, 0.05f, 0.5f, 0.8f);
            terror_scare_rig(TR_DUTCH, 1.0f, 2.5f);
            terror_dip((const float[3]){ 0, 0, 0 }, 0.25f, 0.8f, 1.2f);
        }
    }
}

static void horror_report(World* w, char* out, size_t cap) {
    (void)w;
    snprintf(out, cap, "Horror: fuses %d/3%s, clock %d, breakers %d/4, power %s, %s, caught %d times",
             HO.fuse[0] + HO.fuse[1] + HO.fuse[2], HO.fuses_in ? " (in)" : "", HO.clock_hour, HO.breakers, HO.power ? "on" : "off",
             HO.won ? "escaped" : "inside", HO.catches);
}

static bool horror_check(World* w) {
    (void)w;
    if (HO.auto_step < 0) return true; // (a person plays: nothing to check)
    bool ok = HO.won && HO.power && HO.fuses_in;
    if (!ok) printf("[RealEngine] FAIL: the horror demo's chain: the fuses, the clock, the breakers, the gate\n");
    return ok;
}
