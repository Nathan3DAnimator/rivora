// The Fighting demo's game mode (sugestão #34; included by demo_play.c). Two Mannequins with ABP_Fighter (the guard,
// the fighting step; the strikes as montages: anim_fight.inc), best of three rounds of 60 s.
//   You: WASD moves (W toward the rival, A / D circle round it), Left Mouse jab -> cross -> hook -> uppercut (the next
//   blow of the chain when pressed while one is on), Right Mouse a roundhouse kick, Space jumps (Left Mouse in the air:
//   a jump kick), Left Shift held blocks (15% of the damage, no stagger), Q sways back out of reach, Left Ctrl rolls the
//   way you move (both untouchable while they last).
//   Each blow: startup, an active window (the hit, once, if the other is within its reach and in front), recovery;
//   a hit staggers (hitstun) and pushes back; blows landing within a second of each other count as a combo.
//   The rival thinks every 0.12-0.25 s (its reaction): closes in, backs off, circles, blocks or dodges what it sees
//   coming, punishes a blow that missed, chains its own combos, kicks from outside a punch's reach.
//   The camera: from the side of the line between the two, far enough to keep both in the picture.

typedef enum { FA_IDLE = 0, FA_ATTACK, FA_BLOCK, FA_DODGE, FA_ROLL, FA_HIT, FA_KO, FA_WIN } FightAct;
typedef struct FightMove {
    const char* montage;
    float dmg, reach, hit_from, hit_to, total, push, stun;
    int next; // the chain's next move (-1: none)
} FightMove;
enum { FM_JAB = 0, FM_CROSS, FM_HOOK, FM_UPPER, FM_KICK, FM_AIR, FM_COUNT };
static const FightMove g_moves[FM_COUNT] = {
    { "AM_Fight_Jab", 6.0f, 125.0f, 0.08f, 0.16f, 0.32f, 40.0f, 0.28f, FM_CROSS },
    { "AM_Fight_Cross", 9.0f, 135.0f, 0.14f, 0.22f, 0.40f, 60.0f, 0.32f, FM_HOOK },
    { "AM_Fight_Hook", 11.0f, 120.0f, 0.2f, 0.28f, 0.47f, 80.0f, 0.36f, FM_UPPER },
    { "AM_Fight_Uppercut", 15.0f, 115.0f, 0.26f, 0.34f, 0.55f, 170.0f, 0.5f, -1 },
    { "AM_Fight_Kick", 14.0f, 175.0f, 0.24f, 0.34f, 0.62f, 150.0f, 0.45f, -1 },
    { "AM_Fight_AirKick", 12.0f, 165.0f, 0.12f, 0.28f, 0.5f, 120.0f, 0.4f, -1 },
};
#define FG_DODGE_T 0.45f
#define FG_ROLL_T 0.95f

typedef struct Fighter {
    Entity e;
    float hp, act_t, stun, push[2], push_t, combo_t, anim_t;
    int act, move, queued, combo, best_combo, wins, hits, blocked, dodged;
    bool hit_done;
    float dir[2]; // a roll's / dodge's direction
} Fighter;

static struct {
    Fighter f[2]; // 0: you, 1: the rival
    Entity hud;
    float round_t, banner_t, between_t, cam_side, think;
    int round, state; // 0: fighting, 1: a round over, 2: the match over
    float cam_pos[3], cam_yaw, cam_pitch;
    bool cam_valid;
    float ai_move[2][2], think_t[2]; // each AI's movement input (its last decision), its next thought
    bool autoplay; // REALENGINE_DEMO_AUTOPLAY: you fight by the same mind (the tests)
    uint32_t ai_attacks, ai_blocks, ai_dodges, montages;
} FG;

static const float FG_START[2][2] = { { -230.0f, 0.0f }, { 230.0f, 0.0f } };

static CTransform* fx(World* w, const Fighter* f) { return f->e ? ecs_get(w, f->e, COMP_TRANSFORM) : NULL; }
static CCharacter* fc(World* w, const Fighter* f) { return f->e ? ecs_get(w, f->e, COMP_CHARACTER) : NULL; }

static void fight_hud_text(void) {
    if (!FG.hud) return;
    char b[64];
    ui_set_percent(FG.hud, "P1Bar", fmaxf(FG.f[0].hp, 0.0f) / 100.0f);
    ui_set_percent(FG.hud, "P2Bar", fmaxf(FG.f[1].hp, 0.0f) / 100.0f);
    snprintf(b, sizeof b, "%d", (int)ceilf(fmaxf(FG.round_t, 0.0f)));
    ui_set_text(FG.hud, "Timer", b);
    snprintf(b, sizeof b, "ROUND %d", FG.round);
    ui_set_text(FG.hud, "Round", b);
    static const char* const pips[3] = { "", "WIN", "WIN WIN" };
    ui_set_text(FG.hud, "P1Wins", pips[FG.f[0].wins < 3 ? FG.f[0].wins : 2]);
    ui_set_text(FG.hud, "P2Wins", pips[FG.f[1].wins < 3 ? FG.f[1].wins : 2]);
    if (FG.f[0].combo >= 2 && FG.f[0].combo_t > 0.0f) {
        snprintf(b, sizeof b, "%d HIT COMBO!", FG.f[0].combo);
        ui_set_text(FG.hud, "Combo", b);
        ui_set_visible(FG.hud, "Combo", true);
    } else
        ui_set_visible(FG.hud, "Combo", false);
}

static void fight_banner(const char* text, float secs) {
    if (!FG.hud) return;
    ui_set_text(FG.hud, "Banner", text);
    ui_set_visible(FG.hud, "Banner", true);
    FG.banner_t = secs;
}

static void fight_play(World* w, Fighter* f, const char* montage) {
    if (anim_play_montage(w, f->e, montage, 1.0f)) FG.montages++;
}

static void fight_place(World* w) {
    for (int i = 0; i < 2; i++) {
        Fighter* f = &FG.f[i];
        CTransform* t = fx(w, f);
        CCharacter* c = fc(w, f);
        if (!t || !c) continue;
        t->location[0] = FG_START[i][0], t->location[1] = FG_START[i][1];
        t->rotation[1] = i ? 180.0f : 0.0f;
        memset(c->velocity, 0, sizeof c->velocity);
        f->hp = 100.0f, f->act = FA_IDLE, f->act_t = 0.0f, f->stun = 0.0f, f->combo = 0, f->queued = -1, f->push_t = 0.0f;
        anim_use_blueprint(w, f->e);
        ecs_touch(w, f->e);
    }
}

static void fight_begin(World* w) {
    memset(&FG, 0, sizeof FG);
    FG.f[0].e = bp_player(w);
    FG.f[1].e = dp_find(w, "Rival");
    FG.round = 1, FG.round_t = 60.0f, FG.cam_side = 1.0f;
    const char* ap = getenv("REALENGINE_DEMO_AUTOPLAY");
    FG.autoplay = ap && ap[0] == '1';
    for (int i = 0; i < 2; i++) FG.f[i].hp = 100.0f, FG.f[i].queued = -1;
    FG.hud = dp_hud(w, "WBP_FightHUD");
    fight_hud_text();
    fight_banner("ROUND 1 - FIGHT!", 1.6f);
}

static void fight_attack(World* w, Fighter* f, int move) {
    f->act = FA_ATTACK, f->move = move, f->act_t = 0.0f, f->hit_done = false, f->queued = -1;
    fight_play(w, f, g_moves[move].montage);
}

static float fight_dist(World* w, float* dx, float* dy) { // from you to the rival
    const CTransform *a = fx(w, &FG.f[0]), *b = fx(w, &FG.f[1]);
    if (!a || !b) return 1e9f;
    *dx = b->location[0] - a->location[0], *dy = b->location[1] - a->location[1];
    return sqrtf(*dx * *dx + *dy * *dy);
}

// A blow's active window: the other fighter within its reach (in front of the attacker) takes it, once
static void fight_resolve(World* w, int i) {
    Fighter *a = &FG.f[i], *d = &FG.f[i ^ 1];
    const FightMove* m = &g_moves[a->move];
    if (a->hit_done || a->act_t < m->hit_from || a->act_t > m->hit_to || d->act == FA_KO) return;
    float dx, dy, dist = fight_dist(w, &dx, &dy);
    if (i) dx = -dx, dy = -dy;
    const CTransform* at = fx(w, a);
    if (!at) return;
    float fwd[2] = { cosf(at->rotation[1] * DP_DEG), sinf(at->rotation[1] * DP_DEG) };
    if (dist > m->reach || (dx * fwd[0] + dy * fwd[1]) < 0.4f * dist) return;
    a->hit_done = true;
    if (d->act == FA_DODGE || d->act == FA_ROLL) { // untouchable while it lasts: a miss
        d->dodged++;
        return;
    }
    float ux = dist > 1.0f ? dx / dist : 1.0f, uy = dist > 1.0f ? dy / dist : 0.0f;
    if (d->act == FA_BLOCK) { // blocked: 15% of it, a short push, no stagger
        d->hp -= m->dmg * 0.15f, d->blocked++;
        d->push[0] = ux * m->push * 1.5f, d->push[1] = uy * m->push * 1.5f, d->push_t = 0.12f;
        return;
    }
    d->hp -= m->dmg, a->hits++;
    a->combo = a->combo_t > 0.0f ? a->combo + 1 : 1, a->combo_t = 1.0f;
    if (a->combo > a->best_combo) a->best_combo = a->combo;
    d->act = FA_HIT, d->act_t = 0.0f, d->stun = m->stun, d->queued = -1;
    d->push[0] = ux * m->push * 2.5f, d->push[1] = uy * m->push * 2.5f, d->push_t = 0.2f;
    if (d->hp > 0.0f) fight_play(w, d, "AM_Fight_Hit");
}

// The rival's mind: what it sees, a reaction time, a choice
static void fight_think(World* w, float dt, int i) {
    Fighter *me = &FG.f[i], *you = &FG.f[i ^ 1];
    FG.think_t[i] -= dt;
    if (FG.think_t[i] > 0.0f) return;
    FG.think_t[i] = 0.12f + 0.13f * dp_rand();
    float dx, dy, d = fight_dist(w, &dx, &dy);
    float sgn = i ? -1.0f : 1.0f; // (fight_dist is from you to the rival)
    float ux = d > 1.0f ? sgn * dx / d : 0.0f, uy = d > 1.0f ? sgn * dy / d : 0.0f; // toward the other
    float* mv = FG.ai_move[i];
    mv[0] = mv[1] = 0.0f;
    if (me->act == FA_HIT || me->act == FA_KO || me->act == FA_ROLL || me->act == FA_DODGE) return;
    bool threat = you->act == FA_ATTACK && you->act_t < g_moves[you->move].hit_to && d < g_moves[you->move].reach + 30.0f;
    if (threat && me->act != FA_ATTACK) { // a blow coming: block it, sway back, roll aside, or trade
        float r = dp_rand();
        if (r < 0.45f) {
            me->act = FA_BLOCK, me->act_t = 0.0f, me->anim_t = 0.0f, FG.ai_blocks++;
            fight_play(w, me, "AM_Fight_Block");
        } else if (r < 0.65f) {
            me->act = FA_DODGE, me->act_t = 0.0f, me->dir[0] = -ux, me->dir[1] = -uy, FG.ai_dodges++;
            fight_play(w, me, "AM_Fight_Dodge");
        } else if (r < 0.78f) {
            float side = dp_rand() < 0.5f ? 1.0f : -1.0f;
            me->act = FA_ROLL, me->act_t = 0.0f, me->dir[0] = -uy * side, me->dir[1] = ux * side, FG.ai_dodges++;
            fight_play(w, me, "AM_Fight_Roll");
        }
        return;
    }
    if (me->act == FA_BLOCK && !threat && me->act_t > 0.3f) me->act = FA_IDLE; // (lets the guard down)
    if (me->act == FA_ATTACK) { // a combo goes on while its blows land (or now and then even when they don't)
        if (me->queued < 0 && g_moves[me->move].next >= 0 && (me->hit_done || dp_rand() < 0.3f) && dp_rand() < 0.7f)
            me->queued = g_moves[me->move].next;
        return;
    }
    bool whiffed = you->act == FA_ATTACK && you->act_t > g_moves[you->move].hit_to && !you->hit_done;
    if (whiffed && d < 160.0f) { // a punish: your blow missed, you are open
        fight_attack(w, me, d < 130.0f ? FM_CROSS : FM_KICK), FG.ai_attacks++;
        return;
    }
    if (d > 150.0f) { // close in (a little sideways: harder to read); a kick from its edge of reach
        float side = dp_rand() < 0.5f ? 0.35f : -0.35f;
        mv[0] = ux - uy * side, mv[1] = uy + ux * side;
        if (d < g_moves[FM_KICK].reach - 10.0f && dp_rand() < 0.3f) fight_attack(w, me, FM_KICK), FG.ai_attacks++;
    } else if (d < 85.0f) { // too close: back off
        mv[0] = -ux, mv[1] = -uy;
    } else {
        float r = dp_rand();
        if (r < 0.45f) fight_attack(w, me, FM_JAB), FG.ai_attacks++;
        else if (r < 0.62f) fight_attack(w, me, d > 115.0f ? FM_KICK : FM_HOOK), FG.ai_attacks++;
        else if (r < 0.72f) fight_attack(w, me, FM_CROSS), FG.ai_attacks++;
        else { // circle
            float side = dp_rand() < 0.5f ? 1.0f : -1.0f;
            mv[0] = -uy * side, mv[1] = ux * side;
        }
    }
}

// Your input: the actions (the movement is the Character's: WASD toward / around the rival)
static void fight_input(World* w) {
    Fighter* f = &FG.f[0];
    const CCharacter* c = fc(w, f);
    bool lmb = dp_pressed(500), rmb = dp_pressed(501), dodge = dp_pressed(81 /* Q */), roll = dp_pressed(341 /* Left Ctrl */);
    bool block = bp_key_held(340 /* Left Shift */);
    if (f->act == FA_HIT || f->act == FA_KO || f->act == FA_WIN) return;
    if (f->act == FA_ATTACK) { // the chain: the next blow queued while this one is on
        if (lmb && g_moves[f->move].next >= 0) f->queued = g_moves[f->move].next;
        return;
    }
    if (f->act == FA_DODGE || f->act == FA_ROLL) return;
    if (dodge || roll) {
        float dx, dy, d = fight_dist(w, &dx, &dy);
        float ux = d > 1.0f ? dx / d : 1.0f, uy = d > 1.0f ? dy / d : 0.0f;
        if (roll) { // the way the stick points (else to the side)
            float ix = c ? c->input[0] : 0.0f, iy = c ? c->input[1] : 0.0f, il = sqrtf(ix * ix + iy * iy);
            f->dir[0] = il > 0.1f ? ix / il : -uy, f->dir[1] = il > 0.1f ? iy / il : ux;
        } else f->dir[0] = -ux, f->dir[1] = -uy;
        f->act = roll ? FA_ROLL : FA_DODGE, f->act_t = 0.0f;
        fight_play(w, f, roll ? "AM_Fight_Roll" : "AM_Fight_Dodge");
        return;
    }
    if (lmb || rmb) {
        bool air = c && !c->on_ground;
        fight_attack(w, f, air ? FM_AIR : rmb ? FM_KICK : FM_JAB);
        return;
    }
    if (block && f->act != FA_BLOCK) f->act = FA_BLOCK, f->act_t = 0.0f, f->anim_t = 0.0f, fight_play(w, f, "AM_Fight_Block");
    else if (!block && f->act == FA_BLOCK) f->act = FA_IDLE, anim_stop_montage(w, f->e, 0.1f);
}

static void fight_round_over(World* w, int winner) {
    Fighter* win = &FG.f[winner];
    Fighter* lose = &FG.f[winner ^ 1];
    win->wins++;
    lose->act = FA_KO;
    anim_play(w, lose->e, "MM_Fight_KO", false);
    FG.state = 1, FG.between_t = 3.0f;
    if (win->wins >= 2) {
        FG.state = 2;
        win->act = FA_WIN;
        anim_play(w, win->e, "MM_Fight_Win", true);
        fight_banner(winner ? "K.O.  -  THE RIVAL WINS THE MATCH" : "K.O.  -  YOU WIN THE MATCH!", 1e9f);
    } else
        fight_banner(winner ? "K.O.  -  RIVAL WINS THE ROUND" : "K.O.  -  YOU WIN THE ROUND", 3.0f);
}

static void fight_tick(World* w, float dt) {
    Fighter* f = FG.f;
    if (!f[0].e || !f[1].e || !fx(w, &f[0]) || !fx(w, &f[1])) {
        if (!f[0].e) f[0].e = bp_player(w); // (the pawn is possessed after begin play)
        return;
    }
    if (FG.banner_t > 0.0f && (FG.banner_t -= dt) <= 0.0f) ui_set_visible(FG.hud, "Banner", false);
    if (FG.state == 2) {
        fight_hud_text();
        return;
    }
    if (FG.state == 1) { // between rounds
        if ((FG.between_t -= dt) <= 0.0f) {
            FG.state = 0, FG.round++, FG.round_t = 60.0f;
            fight_place(w);
            char b[48];
            snprintf(b, sizeof b, "ROUND %d - FIGHT!", FG.round);
            fight_banner(b, 1.6f);
        }
        fight_hud_text();
        return;
    }
    FG.round_t -= dt;
    if (FG.autoplay) fight_think(w, dt, 0); // (the tests: you by the same mind)
    else fight_input(w);
    fight_think(w, dt, 1);
    for (int i = 0; i < 2; i++) {
        Fighter* p = &f[i];
        CTransform* t = fx(w, p);
        CCharacter* c = fc(w, p);
        p->act_t += dt;
        if (p->combo_t > 0.0f) p->combo_t -= dt;
        else p->combo = 0;
        switch (p->act) {
        case FA_ATTACK:
            fight_resolve(w, i);
            if (p->act_t >= g_moves[p->move].total) {
                if (p->queued >= 0) fight_attack(w, p, p->queued); // the chain's next blow
                else p->act = FA_IDLE;
            }
            break;
        case FA_HIT:
            if (p->act_t >= p->stun) p->act = FA_IDLE;
            break;
        case FA_BLOCK: // (the block's clip loops by being played again)
            if ((p->anim_t += dt) > 0.9f) p->anim_t = 0.0f, fight_play(w, p, "AM_Fight_Block");
            if (i == 1 && p->act_t > 1.2f) p->act = FA_IDLE;
            break;
        case FA_DODGE:
        case FA_ROLL: {
            float len = p->act == FA_ROLL ? FG_ROLL_T : FG_DODGE_T, speed = p->act == FA_ROLL ? 430.0f : 300.0f;
            float k = sinf(3.14159f * fminf(p->act_t / len, 1.0f)); // (eased in and out)
            if (t) t->location[0] += p->dir[0] * speed * k * dt * 1.6f, t->location[1] += p->dir[1] * speed * k * dt * 1.6f;
            if (p->act_t >= len) p->act = FA_IDLE;
            break;
        }
        default: break;
        }
        if (p->push_t > 0.0f && t) { // knockback (the ring's ropes stop it)
            t->location[0] += p->push[0] * dt, t->location[1] += p->push[1] * dt;
            p->push_t -= dt;
        }
        if (t) { // the ring: 5.5 m each way
            t->location[0] = fminf(fmaxf(t->location[0], -540.0f), 540.0f);
            t->location[1] = fminf(fmaxf(t->location[1], -540.0f), 540.0f);
        }
        // facing each other (not while rolling); your WASD relative to the rival (control yaw), the rival's input set
        float dx, dy;
        fight_dist(w, &dx, &dy);
        if (i) dx = -dx, dy = -dy;
        float yaw = atan2f(dy, dx) / DP_DEG;
        if (t && p->act != FA_ROLL && p->act != FA_KO) t->rotation[1] = yaw;
        if (c) {
            if (i == 0) c->control_yaw = yaw;
            if (i == 1 || FG.autoplay) {
                bool can_move = p->act == FA_IDLE || p->act == FA_BLOCK;
                c->input[0] = can_move ? FG.ai_move[i][0] * (p->act == FA_BLOCK ? 0.4f : 1.0f) : 0.0f;
                c->input[1] = can_move ? FG.ai_move[i][1] * (p->act == FA_BLOCK ? 0.4f : 1.0f) : 0.0f;
            }
            if (i == 0 && p->act != FA_IDLE && p->act != FA_BLOCK) c->input[0] = c->input[1] = 0.0f; // (no walking mid-blow)
        }
        ecs_touch(w, p->e);
    }
    { // the bodies never pass through each other (75 cm between the two: the Characters don't collide with each other)
        CTransform *ta = fx(w, &f[0]), *tb = fx(w, &f[1]);
        float dx = tb->location[0] - ta->location[0], dy = tb->location[1] - ta->location[1], d = sqrtf(dx * dx + dy * dy);
        if (d < 75.0f) {
            float ux = d > 1.0f ? dx / d : 1.0f, uy = d > 1.0f ? dy / d : 0.0f, k = (75.0f - d) * 0.5f;
            ta->location[0] -= ux * k, ta->location[1] -= uy * k, tb->location[0] += ux * k, tb->location[1] += uy * k;
        }
    }
    for (int i = 0; i < 2; i++)
        if (f[i].hp <= 0.0f) {
            fight_round_over(w, i ^ 1);
            break;
        }
    if (FG.state == 0 && FG.round_t <= 0.0f) fight_round_over(w, f[0].hp >= f[1].hp ? 0 : 1); // time: the healthier wins
    fight_hud_text();
}

// The fight camera: from the side of the line between the two, pulled back to keep both in the picture
static bool fight_view(World* w, float pos[3], float* yaw, float* pitch) {
    const CTransform *a = fx(w, &FG.f[0]), *b = fx(w, &FG.f[1]);
    if (!a || !b) return false;
    float mx = (a->location[0] + b->location[0]) * 0.5f, my = (a->location[1] + b->location[1]) * 0.5f;
    float dx = b->location[0] - a->location[0], dy = b->location[1] - a->location[1], d = sqrtf(dx * dx + dy * dy);
    float ux = d > 1.0f ? dx / d : 1.0f, uy = d > 1.0f ? dy / d : 0.0f;
    float nx = -uy * FG.cam_side, ny = ux * FG.cam_side; // (the side kept: no flip when they circle)
    float back = 330.0f + d * 0.85f;
    float want[3] = { mx + nx * back - ux * 60.0f, my + ny * back - uy * 60.0f, a->location[2] + 95.0f + d * 0.08f };
    if (!FG.cam_valid) memcpy(FG.cam_pos, want, sizeof want), FG.cam_valid = true;
    for (int k = 0; k < 3; k++) FG.cam_pos[k] += (want[k] - FG.cam_pos[k]) * 0.12f;
    memcpy(pos, FG.cam_pos, sizeof FG.cam_pos);
    float lx = mx - pos[0], ly = my - pos[1], lz = a->location[2] + 40.0f - pos[2];
    *yaw = atan2f(ly, lx) / DP_DEG;
    *pitch = atan2f(lz, sqrtf(lx * lx + ly * ly)) / DP_DEG;
    return true;
}

static void fight_report(World* w, char* out, size_t cap) {
    (void)w;
    snprintf(out, cap, "Fight: round %d, you %.0f hp (%d hits, best combo %d, %d blocked), rival %.0f hp (%d hits, %u attacks, %u blocks, %u dodges), %u montages",
             FG.round, FG.f[0].hp, FG.f[0].hits, FG.f[0].best_combo, FG.f[0].blocked, FG.f[1].hp, FG.f[1].hits, FG.ai_attacks, FG.ai_blocks,
             FG.ai_dodges, FG.montages);
}
