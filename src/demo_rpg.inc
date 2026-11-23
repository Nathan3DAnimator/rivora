// The RPG demo's game mode (sugestão #31; included by demo_play.c). Oakvale, a village: four merchants with 50 items
// between them, a bag, gold; three jobs to earn it; the Elder's quest; dialogue and every message on the screen (UMG).
//   - E talks to whoever is in front (their name on the prompt), chops, harvests; in a dialogue E goes on;
//   - a merchant opens the shop (WBP_Shop): its wares on the left (a click buys), your bag on the right (a click sells,
//     at half the price), your gold under them; E or Esc closes it;
//   - jobs: the Lumberjack lends an axe (chop the dead trunks at his camp: three swings a log, 6 gold each to him), the
//     Farmer pays 4 gold a sheaf from his field (they grow back), the Courier gives a parcel for someone in the village
//     (25 gold when it arrives); repeat them as you like;
//   - the quest (the HUD shows the step): the Elder needs a Health Potion for the sick Miller, then an Iron Shield for the
//     village's watch; it ends with a reward.

enum { SH_SMITH = 0, SH_GROCER, SH_ALCHEMIST, SH_TRADER, SH_COUNT };
static const char* const g_shop_names[SH_COUNT] = { "Bram the Smith", "Mira's Groceries", "Old Ysolde, Alchemist", "Tobin the Trader" };
static const char* const g_shop_npc[SH_COUNT] = { "Smith", "Grocer", "Alchemist", "Trader" };
typedef struct RpgItem { const char* name; int price, shop; } RpgItem;
static const RpgItem g_items[] = {
    // the smith: weapons, armor, tools (20)
    { "Iron Dagger", 25, SH_SMITH }, { "Short Sword", 60, SH_SMITH }, { "Longsword", 140, SH_SMITH }, { "War Axe", 120, SH_SMITH },
    { "Spear", 70, SH_SMITH }, { "Hunting Bow", 90, SH_SMITH }, { "Arrows (20)", 10, SH_SMITH }, { "Wooden Shield", 35, SH_SMITH },
    { "Iron Shield", 80, SH_SMITH }, { "Leather Cap", 20, SH_SMITH }, { "Iron Helm", 65, SH_SMITH }, { "Leather Armor", 75, SH_SMITH },
    { "Chainmail", 180, SH_SMITH }, { "Iron Gauntlets", 45, SH_SMITH }, { "Travel Boots", 30, SH_SMITH }, { "Woodcutter's Axe", 40, SH_SMITH },
    { "Pickaxe", 45, SH_SMITH }, { "Fishing Rod", 25, SH_SMITH }, { "Hammer", 15, SH_SMITH }, { "Lockpicks", 18, SH_SMITH },
    // the grocer: food and drink (12)
    { "Bread", 3, SH_GROCER }, { "Apple", 2, SH_GROCER }, { "Cheese Wheel", 8, SH_GROCER }, { "Smoked Ham", 12, SH_GROCER },
    { "Fish Stew", 9, SH_GROCER }, { "Honey Jar", 7, SH_GROCER }, { "Ale", 4, SH_GROCER }, { "Wine", 15, SH_GROCER },
    { "Salt", 3, SH_GROCER }, { "Carrots", 2, SH_GROCER }, { "Potatoes", 2, SH_GROCER }, { "Eggs", 3, SH_GROCER },
    // the alchemist: potions and ingredients (10)
    { "Health Potion", 30, SH_ALCHEMIST }, { "Mana Potion", 30, SH_ALCHEMIST }, { "Stamina Tonic", 25, SH_ALCHEMIST },
    { "Antidote", 20, SH_ALCHEMIST }, { "Elixir of Strength", 85, SH_ALCHEMIST }, { "Night Eye Draught", 40, SH_ALCHEMIST },
    { "Healing Herbs", 6, SH_ALCHEMIST }, { "Mushroom", 4, SH_ALCHEMIST }, { "Spider Silk", 12, SH_ALCHEMIST }, { "Glowcap", 15, SH_ALCHEMIST },
    // the trader: goods and trinkets (8)
    { "Cloth", 6, SH_TRADER }, { "Leather", 10, SH_TRADER }, { "Rope", 5, SH_TRADER }, { "Lantern", 14, SH_TRADER },
    { "Map of the Valley", 25, SH_TRADER }, { "Silver Ring", 60, SH_TRADER }, { "Gold Necklace", 150, SH_TRADER }, { "Candles", 3, SH_TRADER },
    // what the jobs give (not sold in shops)
    { "Log", 6, -1 }, { "Wheat Sheaf", 4, -1 }, { "Parcel", 0, -1 },
};
#define RPG_ITEMS ((int)(sizeof g_items / sizeof g_items[0]))
#define RPG_SHOP_ITEMS 50
enum { IT_POTION = 32, IT_SHIELD = 8, IT_AXE = 15, IT_RING = 47, IT_LOG = 50, IT_WHEAT = 51, IT_PARCEL = 52 };
#define RPG_BUY_ROWS 20
#define RPG_SELL_ROWS 14

typedef struct RpgThing { const char* name, *prompt; int kind; } RpgThing; // kind: 0 a person, 1 a trunk, 2 a sheaf
static const RpgThing g_rthings[] = {
    { "Elder", "Talk to the Elder", 0 }, { "Miller", "Talk to the Miller", 0 }, { "Smith", "Trade with Bram (smith)", 0 },
    { "Grocer", "Trade with Mira (groceries)", 0 }, { "Alchemist", "Trade with Ysolde (alchemist)", 0 }, { "Trader", "Trade with Tobin (trader)", 0 },
    { "Lumberjack", "Talk to the Lumberjack", 0 }, { "Farmer", "Talk to the Farmer", 0 }, { "Courier", "Talk to the Courier", 0 },
    { "Villager1", "Talk to Anna", 0 }, { "Villager2", "Talk to Jorin", 0 }, { "Villager3", "Talk to Petra", 0 }, { "Villager4", "Talk to Hal", 0 },
    { "Trunk1", "Chop", 1 }, { "Trunk2", "Chop", 1 }, { "Trunk3", "Chop", 1 }, { "Trunk4", "Chop", 1 }, { "Trunk5", "Chop", 1 },
    { "Wheat1", "Harvest", 2 }, { "Wheat2", "Harvest", 2 }, { "Wheat3", "Harvest", 2 }, { "Wheat4", "Harvest", 2 }, { "Wheat5", "Harvest", 2 },
    { "Wheat6", "Harvest", 2 }, { "Wheat7", "Harvest", 2 }, { "Wheat8", "Harvest", 2 },
};
#define RPG_THINGS ((int)(sizeof g_rthings / sizeof g_rthings[0]))

static struct {
    Entity hud, shop, player;
    Entity things[RPG_THINGS];
    float regrow[RPG_THINGS]; int swings[RPG_THINGS];
    int gold, inv[64], quest, focus, shop_at, parcel_to, delivered, earned;
    int buy_map[RPG_BUY_ROWS], sell_map[RPG_SELL_ROWS];
    bool lumber_job, dialog_open, has_axe_loan;
    char dialog[4][200];
    int dialog_n, dialog_at;
    float toast_t, walk_speed;
    int auto_step;
    float auto_t;
} RP;

static void rpg_toast(const char* text) {
    if (!RP.hud) return;
    ui_set_text(RP.hud, "Toast", text);
    ui_set_visible(RP.hud, "Toast", true);
    RP.toast_t = 3.0f;
}

static void rpg_hud(void) {
    if (!RP.hud) return;
    char b[160];
    snprintf(b, sizeof b, "%d gold", RP.gold);
    ui_set_text(RP.hud, "Gold", b);
    static const char* const steps[5] = { "Talk to the Elder in the square.", "Buy a Health Potion (Ysolde, the alchemist) and bring it to the Miller.",
                                          "Earn the gold for an Iron Shield (80) at Bram's forge.", "Bring the Iron Shield to the Elder.",
                                          "Done: Oakvale is grateful. (Work, trade, explore.)" };
    int step = RP.quest == 2 && RP.inv[IT_SHIELD] > 0 ? 3 : RP.quest;
    ui_set_text(RP.hud, "QuestStep", steps[step < 5 ? step : 4]);
    snprintf(b, sizeof b, "Logs %d   Wheat %d%s", RP.inv[IT_LOG], RP.inv[IT_WHEAT], RP.inv[IT_PARCEL] ? "   Parcel: deliver it" : "");
    ui_set_text(RP.hud, "Jobs", b);
}

static void rpg_say(const char* who, const char* l0, const char* l1, const char* l2) {
    snprintf(RP.dialog[0], sizeof RP.dialog[0], "%s", l0);
    RP.dialog_n = 1;
    if (l1) snprintf(RP.dialog[RP.dialog_n++], sizeof RP.dialog[0], "%s", l1);
    if (l2) snprintf(RP.dialog[RP.dialog_n++], sizeof RP.dialog[0], "%s", l2);
    RP.dialog_at = 0, RP.dialog_open = true;
    ui_set_text(RP.hud, "DialogName", who);
    ui_set_text(RP.hud, "DialogText", RP.dialog[0]);
    ui_set_visible(RP.hud, "DialogBack", true), ui_set_visible(RP.hud, "DialogName", true), ui_set_visible(RP.hud, "DialogText", true);
}
static void rpg_dialog_next(void) {
    if (++RP.dialog_at < RP.dialog_n) {
        ui_set_text(RP.hud, "DialogText", RP.dialog[RP.dialog_at]);
        return;
    }
    RP.dialog_open = false;
    ui_set_visible(RP.hud, "DialogBack", false), ui_set_visible(RP.hud, "DialogName", false), ui_set_visible(RP.hud, "DialogText", false);
}

// The shop's two lists from the merchant's wares and your bag
static void rpg_shop_fill(void) {
    if (!RP.shop) return;
    char b[96];
    int n = 0;
    for (int i = 0; i < RPG_SHOP_ITEMS && n < RPG_BUY_ROWS; i++)
        if (g_items[i].shop == RP.shop_at) RP.buy_map[n++] = i;
    for (int r = 0; r < RPG_BUY_ROWS; r++) {
        char w[16];
        snprintf(w, sizeof w, "Buy%d", r);
        if (r < n) {
            const RpgItem* it = &g_items[RP.buy_map[r]];
            snprintf(b, sizeof b, "%-22s %4d g", it->name, it->price);
            ui_set_text(RP.shop, w, b);
            ui_set_enabled(RP.shop, w, RP.gold >= it->price);
        } else RP.buy_map[r] = -1;
        ui_set_visible(RP.shop, w, r < n);
    }
    int m = 0;
    for (int i = 0; i < RPG_ITEMS && m < RPG_SELL_ROWS; i++)
        if (RP.inv[i] > 0 && i != IT_PARCEL) RP.sell_map[m++] = i;
    for (int r = 0; r < RPG_SELL_ROWS; r++) {
        char w[16];
        snprintf(w, sizeof w, "Sell%d", r);
        if (r < m) {
            const RpgItem* it = &g_items[RP.sell_map[r]];
            snprintf(b, sizeof b, "%-18s x%-2d %4d g", it->name, RP.inv[RP.sell_map[r]], (it->price + 1) / 2);
            ui_set_text(RP.shop, w, b);
        } else RP.sell_map[r] = -1;
        ui_set_visible(RP.shop, w, r < m);
    }
    ui_set_text(RP.shop, "ShopTitle", g_shop_names[RP.shop_at]);
    snprintf(b, sizeof b, "Your gold: %d", RP.gold);
    ui_set_text(RP.shop, "ShopGold", b);
    ui_set_visible(RP.shop, "BagEmpty", m == 0);
}

static void rpg_freeze(World* w, bool frozen) { // (a shop or a dialogue open: the player stands still)
    CCharacter* c = RP.player ? ecs_get(w, RP.player, COMP_CHARACTER) : NULL;
    if (!c) return;
    if (frozen && c->max_walk_speed > 0.0f) RP.walk_speed = c->max_walk_speed, c->max_walk_speed = 0.0f;
    else if (!frozen && c->max_walk_speed == 0.0f) c->max_walk_speed = RP.walk_speed > 0.0f ? RP.walk_speed : 450.0f;
}

static void rpg_shop_open(World* w, int shop) {
    if (!RP.shop) {
        BpAsset* c = (BpAsset*)bp_find_name(BPA_CLASS, "WBP_Shop");
        RP.shop = c ? ui_create_widget(w, c) : ENTITY_NONE;
    }
    if (!RP.shop) return;
    RP.shop_at = shop;
    ui_add_to_viewport(RP.shop, 10);
    ui_show_mouse(true);
    rpg_shop_fill();
}
static void rpg_shop_close(void) {
    if (RP.shop_at < 0) return;
    RP.shop_at = -1;
    if (RP.shop) ui_remove_from_parent(RP.shop);
    ui_show_mouse(false);
}

static bool rpg_buy(int item) {
    if (item < 0 || RP.gold < g_items[item].price) return false;
    RP.gold -= g_items[item].price, RP.inv[item]++;
    char b[96];
    snprintf(b, sizeof b, "Bought: %s (-%d gold)", g_items[item].name, g_items[item].price);
    rpg_toast(b);
    return true;
}
static bool rpg_sell(int item) {
    if (item < 0 || RP.inv[item] <= 0) return false;
    int p = (g_items[item].price + 1) / 2;
    RP.inv[item]--, RP.gold += p, RP.earned += p;
    char b[96];
    snprintf(b, sizeof b, "Sold: %s (+%d gold)", g_items[item].name, p);
    rpg_toast(b);
    return true;
}

// The shop's clicks (ui_set_native_events): Buy<row>, Sell<row>, Close
static void rpg_ui_event(World* w, Entity e, const char* event, const char* widget) {
    (void)w;
    if (e != RP.shop || strcmp(event, "OnClicked") || RP.shop_at < 0) return;
    if (!strncmp(widget, "Buy", 3)) {
        int r = atoi(widget + 3);
        if (r >= 0 && r < RPG_BUY_ROWS) rpg_buy(RP.buy_map[r]);
    } else if (!strncmp(widget, "Sell", 4)) {
        int r = atoi(widget + 4);
        if (r >= 0 && r < RPG_SELL_ROWS) rpg_sell(RP.sell_map[r]);
    } else if (!strcmp(widget, "Close")) {
        rpg_shop_close();
        return;
    }
    rpg_shop_fill();
    rpg_hud();
}

static void rpg_pay(int gold, const char* why) {
    RP.gold += gold, RP.earned += gold;
    char b[96];
    snprintf(b, sizeof b, "+%d gold - %s", gold, why);
    rpg_toast(b);
}

static void rpg_talk(World* w, int i) {
    const char* n = g_rthings[i].name;
    char b[200];
    if (!strcmp(n, "Elder")) {
        if (RP.quest == 0) {
            RP.quest = 1;
            rpg_say("Elder Rowan", "Welcome to Oakvale, traveler. You come in a hard season.",
                    "Our miller has the marsh fever. Ysolde, the alchemist, sells Health Potions - bring him one, will you?",
                    "Gold you can earn here: the Lumberjack, the Farmer and the Courier always need hands.");
        } else if (RP.quest == 3 || (RP.quest == 2 && RP.inv[IT_SHIELD] > 0)) {
            RP.inv[IT_SHIELD]--, RP.inv[IT_RING]++, RP.quest = 4;
            rpg_pay(50, "the Elder's thanks");
            rpg_say("Elder Rowan", "An Iron Shield! Our watch will stand at the bridge tonight.",
                    "Take this silver ring, and these coins. Oakvale won't forget you.", NULL);
        } else if (RP.quest >= 4) rpg_say("Elder Rowan", "The valley is quieter thanks to you. Rest here as long as you like.", NULL, NULL);
        else rpg_say("Elder Rowan", RP.quest == 1 ? "The Miller's house is by the waterwheel, west of the square. Hurry." : "An Iron Shield, from Bram's forge. Eighty gold - work, trade, you'll get there.", NULL, NULL);
    } else if (!strcmp(n, "Miller")) {
        if (RP.quest == 1 && RP.inv[IT_POTION] > 0) {
            RP.inv[IT_POTION]--, RP.quest = 2;
            rpg_pay(40, "the Miller's gratitude");
            rpg_say("Miller Oswin", "*cough* ... a Health Potion? Bless you.", "The Elder will want to see you again - something about the bridge.", NULL);
        } else rpg_say("Miller Oswin", RP.quest == 1 ? "*cough* ... the fever... Ysolde's potions... thirty gold, I can't..." : "Back on my feet and grinding again. Thank you, friend.", NULL, NULL);
    } else if (!strcmp(n, "Lumberjack")) {
        if (!RP.lumber_job) {
            RP.lumber_job = true;
            if (RP.inv[IT_AXE] <= 0) RP.inv[IT_AXE]++, RP.has_axe_loan = true;
            rpg_say("Gareth, lumberjack", "Looking for work? The dead trunks around my camp need clearing.",
                    "Here, take an axe. Three good swings a log; I pay six gold a log.", NULL);
        } else if (RP.inv[IT_LOG] > 0) {
            int n_logs = RP.inv[IT_LOG];
            RP.inv[IT_LOG] = 0;
            snprintf(b, sizeof b, "%d logs", n_logs);
            rpg_pay(n_logs * 6, b);
            rpg_say("Gareth, lumberjack", "Good, clean cuts. Here's your pay. More trunks over there whenever you want.", NULL, NULL);
        } else rpg_say("Gareth, lumberjack", "The trunks are by the camp. Bring me logs.", NULL, NULL);
    } else if (!strcmp(n, "Farmer")) {
        if (RP.inv[IT_WHEAT] > 0) {
            int k = RP.inv[IT_WHEAT];
            RP.inv[IT_WHEAT] = 0;
            snprintf(b, sizeof b, "%d sheaves", k);
            rpg_pay(k * 4, b);
            rpg_say("Wenna, farmer", "Lovely sheaves! Four gold apiece, as promised.", NULL, NULL);
        } else rpg_say("Wenna, farmer", "Harvest the sheaves in my field (E) and bring them here - four gold each. They grow back.", NULL, NULL);
    } else if (!strcmp(n, "Courier")) {
        if (RP.inv[IT_PARCEL] > 0) rpg_say("Pip, courier", "You still have that parcel! Off you go.", NULL, NULL);
        else {
            static const char* const who[4] = { "Anna", "Jorin", "Petra", "Hal" };
            RP.parcel_to = (RP.delivered * 3 + 1) % 4, RP.inv[IT_PARCEL] = 1;
            snprintf(b, sizeof b, "This parcel is for %s. Twenty-five gold when it's in their hands.", who[RP.parcel_to]);
            rpg_say("Pip, courier", "Got legs? Good.", b, NULL);
        }
    } else if (!strncmp(n, "Villager", 8)) {
        int v = n[8] - '1';
        static const char* const names[4] = { "Anna", "Jorin", "Petra", "Hal" };
        if (RP.inv[IT_PARCEL] > 0 && v == RP.parcel_to) {
            RP.inv[IT_PARCEL] = 0, RP.delivered++;
            rpg_pay(25, "a parcel delivered");
            rpg_say(names[v], "For me? Oh, the seeds I ordered! Thank you!", NULL, NULL);
        } else {
            static const char* const chat[4] = { "Nice weather for the market, isn't it?", "Bram's swords are the best this side of the river.",
                                                 "They say bandits camp past the old bridge...", "Mira's cheese is worth every coin." };
            rpg_say(names[v], chat[v], NULL, NULL);
        }
    } else
        for (int s = 0; s < SH_COUNT; s++)
            if (!strcmp(n, g_shop_npc[s])) rpg_shop_open(w, s);
    rpg_hud();
}

static void rpg_use(World* w, int i) {
    const RpgThing* th = &g_rthings[i];
    if (th->kind == 0) {
        rpg_talk(w, i);
        return;
    }
    if (th->kind == 1) { // a dead trunk: three swings a log (an axe needed)
        if (RP.inv[IT_AXE] <= 0) {
            rpg_toast("You need an axe (the Lumberjack lends one; Bram sells them).");
            return;
        }
        if (++RP.swings[i] < 3) {
            rpg_toast("Thunk!");
            return;
        }
        RP.swings[i] = 0, RP.inv[IT_LOG]++, RP.regrow[i] = 25.0f;
        rpg_toast("A log. (Sell logs to the Lumberjack.)");
    } else { // a sheaf: in the bag, grows back later
        RP.inv[IT_WHEAT]++, RP.regrow[i] = 30.0f;
        rpg_toast("A wheat sheaf. (The Farmer pays for them.)");
    }
    CTransform* t = RP.things[i] ? ecs_get(w, RP.things[i], COMP_TRANSFORM) : NULL;
    if (t) t->flags |= TRANSFORM_HIDDEN_GAME, ecs_touch(w, RP.things[i]);
    rpg_hud();
}

static void rpg_begin(World* w) {
    memset(&RP, 0, sizeof RP);
    RP.player = bp_player(w);
    RP.gold = 50, RP.focus = -1, RP.shop_at = -1;
    for (int i = 0; i < RPG_THINGS; i++) RP.things[i] = dp_find(w, g_rthings[i].name);
    RP.hud = dp_hud(w, "WBP_RPGHUD");
    ui_set_native_events(rpg_ui_event);
    rpg_hud();
    rpg_toast("Oakvale. E: talk / use. The Elder waits in the square.");
    const char* ap = getenv("REALENGINE_DEMO_AUTOPLAY");
    RP.auto_step = ap && ap[0] == '1' ? 0 : -1;
}

// The tests: the quest and the three jobs by the same calls, one step every 0.2 s
static void rpg_autoplay(World* w, float dt) {
    static const char* const steps[] = { "Elder", "!buy:Health Potion", "Miller", "Lumberjack", "Trunk1", "Trunk1", "Trunk1", "Trunk2", "Trunk2",
                                         "Trunk2", "Trunk3", "Trunk3", "Trunk3", "Lumberjack", "Wheat1", "Wheat2", "Wheat3", "Wheat4", "Wheat5",
                                         "Farmer", "Courier", "!deliver", "!buy:Iron Shield", "Elder" };
    if (RP.auto_step < 0 || RP.auto_step >= (int)(sizeof steps / sizeof steps[0]) || (RP.auto_t -= dt) > 0.0f) return;
    RP.auto_t = 0.2f;
    const char* s = steps[RP.auto_step++];
    RP.dialog_open = false;
    if (!strncmp(s, "!buy:", 5) || !strncmp(s, "!sell:", 6)) {
        bool buy = s[1] == 'b';
        for (int i = 0; i < RPG_ITEMS; i++)
            if (!strcmp(g_items[i].name, s + (buy ? 5 : 6))) {
                if (buy) rpg_shop_open(w, g_items[i].shop), rpg_buy(i);
                else rpg_shop_open(w, SH_SMITH), rpg_sell(i);
                rpg_shop_fill(), rpg_shop_close();
            }
    } else if (!strcmp(s, "!deliver")) {
        char nm[16];
        snprintf(nm, sizeof nm, "Villager%d", RP.parcel_to + 1);
        for (int i = 0; i < RPG_THINGS; i++)
            if (!strcmp(g_rthings[i].name, nm)) rpg_talk(w, i);
    } else
        for (int i = 0; i < RPG_THINGS; i++)
            if (!strcmp(g_rthings[i].name, s)) rpg_use(w, i);
    rpg_hud();
}

static void rpg_tick(World* w, float dt) {
    if (!RP.player || !ecs_alive(w, RP.player)) RP.player = bp_player(w);
    CTransform* pt = RP.player ? ecs_get(w, RP.player, COMP_TRANSFORM) : NULL;
    const CCharacter* pc = RP.player ? ecs_get(w, RP.player, COMP_CHARACTER) : NULL;
    if (!pt || !pc) return;
    if (RP.toast_t > 0.0f && (RP.toast_t -= dt) <= 0.0f) ui_set_visible(RP.hud, "Toast", false);
    for (int i = 0; i < RPG_THINGS; i++) // the trunks and the sheaves come back
        if (RP.regrow[i] > 0.0f && (RP.regrow[i] -= dt) <= 0.0f) {
            CTransform* t = RP.things[i] ? ecs_get(w, RP.things[i], COMP_TRANSFORM) : NULL;
            if (t) t->flags &= ~TRANSFORM_HIDDEN_GAME, ecs_touch(w, RP.things[i]);
        }
    rpg_autoplay(w, dt);
    rpg_freeze(w, RP.shop_at >= 0 || RP.dialog_open);
    if (RP.shop_at >= 0) {
        if (dp_pressed(69 /* E */) || dp_pressed(256 /* Esc */)) rpg_shop_close();
        return;
    }
    if (RP.dialog_open) {
        if (dp_pressed(69 /* E */)) rpg_dialog_next();
        return;
    }
    float fwd[2] = { cosf(pc->control_yaw * DP_DEG), sinf(pc->control_yaw * DP_DEG) }, best = 1e9f;
    int focus = -1;
    for (int i = 0; i < RPG_THINGS; i++) {
        Entity e = RP.things[i];
        const CTransform* t = e && ecs_alive(w, e) ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (!t || (t->flags & TRANSFORM_HIDDEN_GAME)) continue;
        float dx = t->location[0] - pt->location[0], dy = t->location[1] - pt->location[1], d = sqrtf(dx * dx + dy * dy);
        if (d > 230.0f || (d > 80.0f && (dx * fwd[0] + dy * fwd[1]) < 0.4f * d)) continue;
        if (d < best) best = d, focus = i;
    }
    if (focus != RP.focus) {
        RP.focus = focus;
        char b[96];
        snprintf(b, sizeof b, "E  -  %s", focus >= 0 ? g_rthings[focus].prompt : "");
        ui_set_text(RP.hud, "Prompt", b);
        ui_set_visible(RP.hud, "Prompt", focus >= 0);
    }
    if (focus >= 0 && dp_pressed(69 /* E */)) rpg_use(w, focus);
}

static void rpg_report(World* w, char* out, size_t cap) {
    (void)w;
    int items = 0;
    for (int i = 0; i < RPG_ITEMS; i++) items += RP.inv[i];
    snprintf(out, cap, "RPG: quest step %d, %d gold (%d earned), %d items carried, %d parcels delivered, %d items in the shops",
             RP.quest, RP.gold, RP.earned, items, RP.delivered, RPG_SHOP_ITEMS);
}

static bool rpg_check(World* w) {
    (void)w;
    if (RP.auto_step < 0) return true;
    bool ok = RP.quest == 4 && RP.inv[IT_RING] == 1 && RP.delivered == 1 && RP.earned > 0;
    if (!ok) printf("[RealEngine] FAIL: the RPG demo: the quest, the jobs or the shop\n");
    return ok;
}
