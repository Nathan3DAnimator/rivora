// Blueprints' insides shared by blueprint.c (assets, registry) and bp_vm.c (compiler, VM, runtime)
#pragma once
#include "blueprint.h"

// The node kinds, in the registry's order (blueprint.c g_nodes); files store the names, never these numbers
typedef enum BpNodeKind {
    BPK_EVENT_BEGIN_PLAY = 0, BPK_EVENT_TICK, BPK_EVENT_KEY, BPK_EVENT_CUSTOM, BPK_EVENT_INTERFACE, BPK_CALL_CUSTOM,
    BPK_BRANCH, BPK_SEQUENCE, BPK_FOR_LOOP, BPK_DELAY, BPK_DO_ONCE, BPK_PRINT, BPK_VAR_GET, BPK_VAR_SET,
    BPK_ADD, BPK_SUB, BPK_MUL, BPK_DIV, BPK_VADD, BPK_VSUB, BPK_VMULS, BPK_DOT, BPK_VLEN, BPK_NORMALIZE,
    BPK_MAKE_VECTOR, BPK_BREAK_VECTOR, BPK_MAKE_ROTATOR, BPK_BREAK_ROTATOR, BPK_GREATER, BPK_LESS, BPK_EQUAL,
    BPK_AND, BPK_OR, BPK_NOT, BPK_RANDOM, BPK_LERP, BPK_CLAMP, BPK_SIN, BPK_COS, BPK_SELECT,
    BPK_SELF, BPK_GET_PLAYER, BPK_DELTA, BPK_GAME_TIME,
    BPK_GET_LOCATION, BPK_SET_LOCATION, BPK_ADD_OFFSET, BPK_GET_ROTATION, BPK_SET_ROTATION, BPK_ADD_ROTATION,
    BPK_GET_SCALE, BPK_SET_SCALE, BPK_GET_FORWARD, BPK_GET_RIGHT, BPK_SPAWN, BPK_DESTROY, BPK_IS_VALID, BPK_DISTANCE,
    BPK_CAST, BPK_CALL_INTERFACE, BPK_DOES_IMPLEMENT,
    BPK_LIGHT_INTENSITY, BPK_LIGHT_COLOR, BPK_FOG_DENSITY, BPK_FOG_COLOR, BPK_CLOUD_COVERAGE, BPK_SUN_ANGLE,
    BPK_SET_VISIBILITY, BPK_SET_COLOR,
    BPK_MOVE_INPUT, BPK_JUMP, BPK_YAW_INPUT, BPK_PITCH_INPUT, BPK_AXIS, BPK_KEY_DOWN, BPK_CONTROL_FORWARD, BPK_CONTROL_RIGHT,
    BPK_FUNC_ENTRY, BPK_FUNC_RETURN, BPK_CALL_FUNCTION, BPK_MAKE_STRUCT, BPK_BREAK_STRUCT, BPK_SWITCH_ENUM,
    BPK_ENUM_LITERAL, BPK_DATA_GET, BPK_INT_TO_FLOAT, BPK_FLOAT_TO_INT,
    BPK_MAT_SCALAR, BPK_MAT_VECTOR, // Unreal's Set Scalar / Vector Parameter Value on Materials (the name: the node's text)
    BPK_GET_OWNER,  // an Actor Component Blueprint's actor
    BPK_ADD_IMPULSE, BPK_SET_SIMULATE, BPK_RADIAL_IMPULSE, BPK_GET_VELOCITY, // physics.c
    // animation (anim.c): the Anim Blueprints' event and getters, montages (the asset's name: the node's text)
    BPK_EVENT_UPDATE_ANIM, BPK_TRY_GET_PAWN_OWNER, BPK_GET_ACTOR_VELOCITY, BPK_IS_FALLING, BPK_PLAY_MONTAGE,
    BPK_STOP_MONTAGE, BPK_PLAY_ANIMATION,
    // AI (ai.c): the Blackboard by the key in the node's text, moving, a tree, a noise
    BPK_BB_GET_FLOAT, BPK_BB_GET_BOOL, BPK_BB_GET_VECTOR, BPK_BB_GET_ACTOR, BPK_BB_SET_FLOAT, BPK_BB_SET_BOOL,
    BPK_BB_SET_VECTOR, BPK_BB_SET_ACTOR, BPK_AI_MOVE_TO, BPK_RUN_BT, BPK_REPORT_NOISE,
    BPK_GET_CONTROL_ROTATION, // the view's pitch and yaw (a Character's control rotation)
    // math (integers, floats, vectors, rotators), random, strings, colors, transforms, classes
    BPK_IADD, BPK_ISUB, BPK_IMUL, BPK_IDIV, BPK_IMOD, BPK_IGREATER, BPK_ILESS, BPK_IGE, BPK_ILE, BPK_IEQUAL, BPK_INOTEQUAL,
    BPK_IMAX, BPK_IMIN, BPK_IABS, BPK_ICLAMP,
    BPK_FGE, BPK_FLE, BPK_FNOTEQUAL, BPK_FMAX, BPK_FMIN, BPK_FABS, BPK_POW, BPK_SQRT, BPK_FLOOR, BPK_CEIL, BPK_ROUND,
    BPK_FRAC, BPK_FMOD, BPK_TAN, BPK_ATAN2, BPK_MAP_RANGE, BPK_FINTERP, BPK_NEARLY_EQUAL, BPK_XOR,
    BPK_VMUL, BPK_VDIVS, BPK_CROSS, BPK_VDIST, BPK_VLERP, BPK_VINTERP, BPK_ROTATE_VECTOR, BPK_UNROTATE_VECTOR, BPK_VEQUAL,
    BPK_ROT_FORWARD, BPK_ROT_RIGHT, BPK_ROT_UP, BPK_LOOK_AT, BPK_ROT_FROM_X, BPK_COMBINE_ROT, BPK_RLERP, BPK_RINTERP, BPK_DELTA_ROT,
    BPK_RAND_BOOL, BPK_RAND_BOOL_W, BPK_RAND_INT, BPK_RAND_INT_RANGE, BPK_RAND_FLOAT, BPK_RAND_UNIT_VEC, BPK_RAND_CONE,
    BPK_RAND_ROT, BPK_RAND_POINT_BOX, BPK_RAND_INT64, BPK_RAND_ARRAY_ITEM,
    BPK_TO_STRING, BPK_STRING_TO_INT, BPK_STRING_TO_FLOAT, BPK_APPEND, BPK_STR_LEN, BPK_SUBSTRING, BPK_STR_CONTAINS,
    BPK_STR_UPPER, BPK_STR_LOWER, BPK_STR_EQUAL, BPK_STR_REPLACE, BPK_JOIN,
    BPK_MAKE_COLOR, BPK_BREAK_COLOR, BPK_LERP_COLOR, BPK_HSV_TO_RGB,
    BPK_MAKE_TRANSFORM, BPK_BREAK_TRANSFORM, BPK_GET_TRANSFORM, BPK_SET_TRANSFORM, BPK_TRANSFORM_LOC, BPK_INV_TRANSFORM_LOC,
    BPK_TRANSFORM_DIR, BPK_COMPOSE, BPK_INVERT_TRANSFORM,
    BPK_GET_CLASS, BPK_CLASS_EQUAL, BPK_GET_DISPLAY_NAME, BPK_OBJ_EQUAL, BPK_GET_ALL_ACTORS, BPK_GET_ACTOR_OF_CLASS,
    BPK_SELECTW, BPK_INT64_ADD, BPK_INT64_SUB, BPK_INT64_MUL, BPK_INT64_DIV,
    // flow control (compiled like For Loop: no macro expansion needed)
    BPK_FLIPFLOP, BPK_GATE, BPK_DO_N, BPK_WHILE, BPK_FOR_LOOP_BREAK, BPK_FOREACH, BPK_FOREACH_BREAK, BPK_SWITCH_INT,
    BPK_ISVALID_EXEC, BPK_RETRIGGER_DELAY, BPK_SET_TIMER, BPK_CLEAR_TIMER, BPK_EVENT_END_PLAY,
    // containers (wildcards: their type is the first wire's)
    BPK_ARR_ADD, BPK_ARR_ADD_UNIQUE, BPK_ARR_INSERT, BPK_ARR_REMOVE_INDEX, BPK_ARR_REMOVE, BPK_ARR_CLEAR, BPK_ARR_SET,
    BPK_ARR_GET, BPK_ARR_LEN, BPK_ARR_LAST, BPK_ARR_VALID_INDEX, BPK_ARR_CONTAINS, BPK_ARR_FIND, BPK_ARR_MAKE,
    BPK_ARR_SHUFFLE, BPK_ARR_APPEND, BPK_ARR_RESIZE, BPK_ARR_IS_EMPTY,
    BPK_SET_ADD, BPK_SET_REMOVE, BPK_SET_CONTAINS, BPK_SET_LEN, BPK_SET_CLEAR, BPK_SET_TO_ARRAY,
    BPK_MAP_ADD, BPK_MAP_FIND, BPK_MAP_REMOVE, BPK_MAP_CONTAINS, BPK_MAP_LEN, BPK_MAP_CLEAR, BPK_MAP_KEYS, BPK_MAP_VALUES,
    // levels (level.c's streaming) and spawning
    BPK_OPEN_LEVEL, BPK_LOAD_STREAM, BPK_UNLOAD_STREAM, BPK_GET_LEVEL_NAME, BPK_SPAWN_ASYNC,
    // macros: an instance of a macro graph (expanded when compiling) and the macro's tunnels
    BPK_MACRO, BPK_TUNNEL_IN, BPK_TUNNEL_OUT,
    // Enhanced Input (input.c): an action's event (its five outputs), the contexts, an action's value
    BPK_EVENT_INPUT_ACTION, BPK_ADD_MAPPING_CONTEXT, BPK_REMOVE_MAPPING_CONTEXT, BPK_GET_ACTION_VALUE,
    // Post Process Volumes (post.c): a setting by its name (the node's text), the blend weight, the color tint
    BPK_PP_SET_FLOAT, BPK_PP_SET_WEIGHT, BPK_PP_SET_TINT,
    // UMG (ui_widget.c): widgets made and shown; a widget of the tree by its name (the node's text)
    BPK_CREATE_WIDGET, BPK_ADD_TO_VIEWPORT, BPK_REMOVE_FROM_PARENT, BPK_WIDGET_SET_TEXT, BPK_WIDGET_SET_PERCENT,
    BPK_WIDGET_SET_VISIBLE, BPK_WIDGET_SET_COLOR, BPK_SHOW_MOUSE,
    BPK_REROUTE, // Unreal's Reroute Node: a knot on a wire (any type, exec too); the compiler joins its wires
    BPK_EVENT_BEGIN_OVERLAP, BPK_EVENT_END_OVERLAP, BPK_EVENT_OVERLAPPING, BPK_IS_OVERLAPPING, // overlaps (triggers)
    BPK_DISPATCH_CALL, BPK_DISPATCH_BIND, BPK_DISPATCH_UNBIND, BPK_DISPATCH_UNBIND_ALL, BPK_EVENT_DISPATCH, // Event Dispatchers
    // UMG 5.8 (sugestão #59): a widget's value (Slider, Spin Box, Check Box, Combo Box index), its text, Is Enabled; the
    // Widget Animations (the node's text: the widget, or the animation)
    BPK_WIDGET_GET_VALUE, BPK_WIDGET_SET_VALUE, BPK_WIDGET_GET_TEXT, BPK_WIDGET_SET_ENABLED, BPK_PLAY_WIDGET_ANIM, BPK_STOP_WIDGET_ANIM,
    // Sequencer (sugestão #45): a Level Sequence played, stopped, asked (the node's text: its name, sequencer.h seq_load)
    BPK_PLAY_SEQUENCE, BPK_STOP_SEQUENCE, BPK_IS_SEQUENCE_PLAYING,
    // Online (online.h): sessions, players, replication, RPCs (the event: the node's text), the network emulation; their
    // events; Steam (steam.h): the user, achievements, stats, leaderboards, the overlay, presence, DLC, friends, the cloud;
    // the saves (online.c: the actor's variables in a slot, and Steam Cloud)
    BPK_NET_CREATE_SESSION, BPK_NET_FIND_SESSIONS, BPK_NET_SESSION_COUNT, BPK_NET_SESSION_NAME, BPK_NET_SESSION_PLAYERS,
    BPK_NET_JOIN_SESSION, BPK_NET_JOIN_ADDRESS, BPK_NET_DESTROY_SESSION, BPK_NET_IS_SERVER, BPK_NET_IS_CLIENT,
    BPK_NET_IN_SESSION, BPK_NET_HAS_AUTHORITY, BPK_NET_LOCALLY_CONTROLLED, BPK_NET_PLAYER_COUNT, BPK_NET_PLAYER_NAME,
    BPK_NET_PLAYER_PAWN, BPK_NET_PING, BPK_NET_PACKET_LOSS, BPK_NET_LOCAL_PLAYER, BPK_NET_KICK, BPK_NET_CHAT,
    BPK_NET_SET_REPLICATES, BPK_NET_RPC_SERVER, BPK_NET_RPC_MULTICAST, BPK_NET_RPC_OWNER, BPK_NET_RPC_FLOAT,
    BPK_NET_RPC_VECTOR, BPK_NET_RPC_TEXT, BPK_NET_RPC_SENDER, BPK_NET_REPLICATE_VAR, BPK_NET_SPAWN, BPK_NET_DESTROY,
    BPK_NET_EMULATION,
    BPK_EVENT_NET_PLAYER_JOINED, BPK_EVENT_NET_PLAYER_LEFT, BPK_EVENT_NET_SESSION_CREATED, BPK_EVENT_NET_SESSIONS_FOUND,
    BPK_EVENT_NET_JOINED, BPK_EVENT_NET_ERROR, BPK_EVENT_NET_CHAT,
    BPK_STEAM_AVAILABLE, BPK_STEAM_NAME, BPK_STEAM_ID, BPK_STEAM_UNLOCK, BPK_STEAM_CLEAR, BPK_STEAM_IS_UNLOCKED,
    BPK_STEAM_PROGRESS, BPK_STEAM_SET_STAT, BPK_STEAM_GET_STAT, BPK_STEAM_STORE_STATS, BPK_STEAM_UPLOAD_SCORE,
    BPK_STEAM_DOWNLOAD_SCORES, BPK_STEAM_SCORE_COUNT, BPK_STEAM_SCORE_NAME, BPK_STEAM_SCORE_VALUE, BPK_STEAM_SCORE_RANK,
    BPK_STEAM_OVERLAY, BPK_STEAM_INVITE, BPK_STEAM_PRESENCE, BPK_STEAM_DLC, BPK_STEAM_LANGUAGE, BPK_STEAM_FRIEND_COUNT,
    BPK_STEAM_FRIEND_NAME, BPK_STEAM_CLOUD_ENABLED, BPK_EVENT_STEAM_LEADERBOARD, BPK_EVENT_STEAM_OVERLAY,
    BPK_SAVE_GAME, BPK_LOAD_GAME, BPK_SAVE_EXISTS, BPK_DELETE_SAVE,
    // Audio (audio.h, audio_world.h): sounds played (the sound, a mix, a class: the node's text, a vpath), the playing
    // sounds by their handle (an int), an actor's Audio Component, the mixes
    BPK_PLAY_SOUND_2D, BPK_PLAY_SOUND_AT, BPK_SPAWN_SOUND_2D, BPK_SPAWN_SOUND_AT, BPK_SPAWN_SOUND_ATTACHED,
    BPK_STOP_SOUND, BPK_SET_SOUND_PAUSED, BPK_IS_SOUND_PLAYING, BPK_SET_SOUND_VOLUME, BPK_SET_SOUND_PITCH,
    BPK_AUDIO_PLAY, BPK_AUDIO_STOP, BPK_AUDIO_FADE_IN, BPK_AUDIO_FADE_OUT, BPK_AUDIO_SET_PAUSED, BPK_AUDIO_SET_VOLUME,
    BPK_AUDIO_SET_PITCH, BPK_AUDIO_IS_PLAYING, BPK_AUDIO_SET_SOUND,
    BPK_PUSH_SOUND_MIX, BPK_POP_SOUND_MIX, BPK_CLEAR_SOUND_MIXES, BPK_SET_BASE_SOUND_MIX, BPK_SET_CLASS_VOLUME,
    // Editor utilities (bp_run_editor_event): the editor's selection
    BPK_ED_SELECTED_COUNT, BPK_ED_SELECTED_AT, BPK_ED_SELECT,
    BPK_LAUNCH_CHARACTER, // (Unreal's: the double jump, a jump pad, a knock back)
    // Advanced sessions (online.h; Unreal's Advanced Sessions plugin): every setting when creating, searching and joining,
    // the session's own properties and filters, its state, bans, friends and invites; their events
    BPK_NET_CREATE_ADV, BPK_NET_FIND_ADV, BPK_NET_ADD_FILTER, BPK_NET_CLEAR_FILTERS, BPK_NET_SET_PROP, BPK_NET_REMOVE_PROP,
    BPK_NET_GET_PROP, BPK_NET_RESULT_PROP, BPK_NET_RESULT_INFO, BPK_NET_JOIN_ADV, BPK_NET_JOIN_ADDR_ADV, BPK_NET_UPDATE_SESSION,
    BPK_NET_START_SESSION, BPK_NET_END_SESSION, BPK_NET_SET_JOINABLE, BPK_NET_SESSION_STATE, BPK_NET_SESSION_SETTINGS,
    BPK_NET_BAN, BPK_NET_UNBAN, BPK_NET_UNBAN_ALL, BPK_NET_BAN_COUNT, BPK_NET_BAN_NAME, BPK_NET_PLAYER_ID, BPK_NET_LAST_ERROR,
    BPK_NET_FRIEND_IN_SESSION, BPK_NET_JOIN_FRIEND, BPK_NET_INVITE_FRIEND, BPK_NET_AUTO_INVITES, BPK_NET_ACCEPT_INVITE,
    BPK_NET_IS_FINDING,
    BPK_EVENT_NET_SESSION_UPDATED, BPK_EVENT_NET_SESSION_STATE, BPK_EVENT_NET_INVITE, BPK_EVENT_NET_JOIN_REFUSED,
    // Particles (particles.h; Unreal's Niagara functions): spawn a system, activate, deactivate, tint, its rate, active
    BPK_PS_SPAWN, BPK_PS_ACTIVATE, BPK_PS_DEACTIVATE, BPK_PS_SET_COLOR, BPK_PS_SET_SPAWN, BPK_PS_IS_ACTIVE,
    // IK Rig and Physics Asset (ikrig.h; Unreal's Skeletal Mesh Component functions): ragdoll, bodies below, blend,
    // impulse, simulating, IK goals
    BPK_PHYS_SET_SIMULATE, BPK_PHYS_SET_BELOW, BPK_PHYS_SET_BLEND, BPK_PHYS_IMPULSE, BPK_PHYS_IS_SIMULATING, BPK_IK_SET_GOAL,
    BPK_IK_CLEAR_GOAL,
    // Vehicles (vehicle.h; Unreal's Chaos Vehicle Movement functions): inputs, gears, engine, parts, enter / exit, state
    BPK_VEH_THROTTLE, BPK_VEH_STEER, BPK_VEH_BRAKE, BPK_VEH_HANDBRAKE, BPK_VEH_FLIGHT, BPK_VEH_GEAR, BPK_VEH_ENGINE,
    BPK_VEH_PART_SET, BPK_VEH_PART_TOGGLE, BPK_VEH_PART_GET, BPK_VEH_ENTER, BPK_VEH_EXIT, BPK_VEH_SPEED, BPK_VEH_RPM,
    BPK_VEH_GET_GEAR,
    BPK_NET_LAG_TRACE,
    // Cloth and rain (cloth.h, water.h)
    BPK_CLOTH_TEAR, BPK_RAIN_SET, BPK_RAIN_GET,
    // VR (xr.h)
    BPK_VR_ACTIVE, BPK_VR_HEAD, BPK_VR_HAND, BPK_VR_HAND_INPUT, BPK_VR_HAPTIC,
    // AR (ar_arcore.h)
    BPK_AR_TRACKING, BPK_AR_HIT, BPK_AR_PLANES,
    // the touch screen and the mouse (bp_touch_update)
    BPK_EVENT_TOUCH, BPK_GET_MOUSE_POS,
    BPK_TAG_ADD, BPK_TAG_REMOVE, BPK_TAG_HAS, BPK_TAG_HAS_EXACT, BPK_TAG_MATCHES, // Gameplay Tags (gameplay_tags.h)
    BPK_FORMAT_TEXT,
    BPK_CONSTRAINT_BREAK, BPK_CONSTRAINT_MOTOR, // Physics Constraint (physics.c)
    BPK_NAV_PROJECT, BPK_NAV_RANDOM_REACHABLE, BPK_NAV_PATH_LENGTH, // the NavMesh (navmesh.h)
    BPK_MORPH_SET, BPK_MORPH_GET, // Morph targets (anim.h anim_morph_set)
    // Health and damage (health.h): the actions, the getters, the actor's events
    BPK_APPLY_DAMAGE, BPK_APPLY_RADIAL_DAMAGE, BPK_HEAL, BPK_GET_HEALTH, BPK_GET_HEALTH_PERCENT, BPK_GET_MAX_HEALTH, BPK_GET_SHIELD,
    BPK_SET_MAX_HEALTH, BPK_SET_SHIELD, BPK_IS_DEAD, BPK_REVIVE, BPK_SET_INVINCIBLE, BPK_SET_DAMAGE_MULT,
    BPK_EVENT_ANY_DAMAGE, BPK_EVENT_HEALED, BPK_EVENT_DEATH, BPK_EVENT_SHIELD_BROKEN,
    // Inventory and Items (inventory.h): an item is its Item Definition's name; Target unconnected is self
    BPK_INV_ADD, BPK_INV_REMOVE, BPK_INV_HAS, BPK_INV_COUNT, BPK_INV_USE, BPK_INV_EQUIP, BPK_INV_UNEQUIP, BPK_INV_DROP,
    BPK_INV_GET_SLOT, BPK_INV_MOVE, BPK_INV_FIND_TAG, BPK_INV_SPLIT, BPK_INV_SORT, BPK_INV_GET_EQUIPPED, BPK_INV_WEIGHT,
    BPK_INV_ITEM_INFO, BPK_INV_PICK_UP, BPK_INV_TRANSFER, BPK_INV_SLOT_COUNT,
    BPK_EVENT_INV_CHANGED, BPK_EVENT_ITEM_USED, BPK_EVENT_ITEM_EQUIPPED, BPK_EVENT_ITEM_UNEQUIPPED,
    // Pause and options (game_settings.h): Unreal's Set Game Paused, GameUserSettings, the pause menu, Quit Game
    BPK_SET_GAME_PAUSED, BPK_IS_GAME_PAUSED, BPK_SET_TICK_PAUSED, BPK_GET_GUS, BPK_SET_GUS, BPK_APPLY_GUS, BPK_LOAD_GUS,
    BPK_DEFAULTS_GUS, BPK_REMAP_KEY, BPK_GET_ACTION_KEY, BPK_OPEN_PAUSE_MENU, BPK_CLOSE_PAUSE_MENU, BPK_MAIN_MENU, BPK_QUIT_GAME,
    // Save and load the progress (save_progress.h): the Save Game object, the progress, slots, auto save, checkpoints
    BPK_CREATE_SAVE_OBJECT, BPK_GET_PROGRESS_SAVE, BPK_SET_PROGRESS_SAVE, BPK_SAVE_PROGRESS, BPK_LOAD_PROGRESS, BPK_NEW_GAME,
    BPK_AUTO_SAVE, BPK_LIST_SAVE_SLOTS, BPK_GET_SLOT_INFO, BPK_GET_SLOT_NAME, BPK_GET_LATEST_SLOT, BPK_GET_PLAY_TIME,
    BPK_GET_CURRENT_SLOT, BPK_SET_AUTO_SAVE_INTERVAL, BPK_RESPAWN_CHECKPOINT, BPK_GET_CHECKPOINT, BPK_SET_CHECKPOINT,
    BPK_OPEN_SAVE_MENU, BPK_EVENT_GAME_SAVED, BPK_EVENT_GAME_LOADED, BPK_EVENT_CHECKPOINT,
    // Interaction (interaction.h): the Interactor's and the Interactable's nodes, the devices, the Interactable's events
    BPK_IA_INTERACT, BPK_IA_GET_FOCUSED, BPK_IA_SET_ENABLED, BPK_IA_SET_PROMPT, BPK_IA_GET_PROMPT, BPK_IA_DEVICE_SET, BPK_IA_DEVICE_IS,
    BPK_EVENT_IA_INTERACT, BPK_EVENT_IA_FOCUS_BEGIN, BPK_EVENT_IA_FOCUS_END, BPK_EVENT_IA_HOLD,
    // Dialogue (dialogue.h): the conversation with a Dialogue Speaker, its events (to every Blueprint that has them)
    BPK_DLG_START, BPK_DLG_CHOOSE, BPK_DLG_SKIP, BPK_DLG_END, BPK_DLG_IS_IN, BPK_DLG_SET_LANGUAGE,
    BPK_EVENT_DLG_LINE, BPK_EVENT_DLG_CHOICE, BPK_EVENT_DLG_END,
    // Quests (quest.h): a quest by its asset's name, the objective by its id (or index); Quest Log unconnected: the player's
    BPK_QST_START, BPK_QST_COMPLETE, BPK_QST_FAIL, BPK_QST_ADVANCE, BPK_QST_SET_PROGRESS, BPK_QST_IS_ACTIVE, BPK_QST_IS_COMPLETED,
    BPK_QST_GET_STATE, BPK_QST_GET_PROGRESS, BPK_QST_TRACK, BPK_QST_GET_TRACKED,
    BPK_EVENT_QST_STARTED, BPK_EVENT_QST_OBJ_UPDATED, BPK_EVENT_QST_OBJ_COMPLETED, BPK_EVENT_QST_COMPLETED, BPK_EVENT_QST_FAILED,
    // Game Rules (game_rules.h): the match's phases, score, clock, lives; its events (to every Blueprint that has them)
    BPK_GR_GET_PHASE, BPK_GR_SET_PHASE, BPK_GR_START, BPK_GR_END, BPK_GR_ADD_SCORE, BPK_GR_GET_SCORE, BPK_GR_TEAM_SCORE, BPK_GR_TIME,
    BPK_GR_SET_TIMER, BPK_GR_GET_LIVES, BPK_GR_ADD_LIVES, BPK_GR_RESPAWN, BPK_GR_RESTART, BPK_GR_RESULT,
    BPK_EVENT_MATCH_PHASE, BPK_EVENT_SCORE_CHANGED, BPK_EVENT_MATCH_WON, BPK_EVENT_MATCH_LOST, BPK_EVENT_ROUND_STARTED,
    BPK_EVENT_ROUND_ENDED, BPK_EVENT_PLAYER_RESPAWNED, BPK_EVENT_TIMER_EXPIRED,
    // Abilities and effects (ability.h): Unreal's Gameplay Ability System, the light version; Target unconnected is self
    BPK_ABIL_GIVE, BPK_ABIL_REMOVE, BPK_ABIL_TRY, BPK_ABIL_TRY_TAG, BPK_ABIL_CANCEL, BPK_ABIL_END, BPK_ABIL_OWNER,
    BPK_ABIL_ON_COOLDOWN, BPK_ABIL_COOLDOWN, BPK_ABIL_IS_ACTIVE, BPK_GE_APPLY, BPK_GE_REMOVE, BPK_GE_HAS, BPK_ATTR_GET,
    BPK_ATTR_SET, BPK_EVENT_ABIL_ACTIVATE, BPK_EVENT_ABIL_ACTIVATED, BPK_EVENT_ABIL_ENDED, BPK_EVENT_GE_APPLIED,
    BPK_EVENT_GE_REMOVED, BPK_EVENT_ATTR_CHANGED,
    // The HUD (hud.h): the feed, the markers, the damage numbers, the visibility, the bars, the crosshair, the photo mode
    BPK_HUD_MESSAGE, BPK_HUD_ADD_COMPASS, BPK_HUD_REMOVE_COMPASS, BPK_HUD_ADD_SCREEN, BPK_HUD_REMOVE_SCREEN, BPK_HUD_DAMAGE_NUMBER,
    BPK_HUD_SET_VISIBLE, BPK_HUD_BIND_BAR, BPK_HUD_SET_SPREAD, BPK_HUD_PHOTO_MODE, BPK_HUD_IS_VISIBLE,
    BPK_GET_ALL_WITH_TAG, BPK_GET_ALL_WITH_GTAG, // the actors with a Gameplay Tag (gameplay_tags.h gt_find_actors)
    BPK_SET_RENDER_STYLE, BPK_GET_RENDER_STYLE, // the render style (render.h, A9.0)
    BPK_SPAWN_DECAL, // Unreal's Spawn Decal at Location (decal.h, M6)
    BPK_WP_SET_LAYER, BPK_WP_GET_LAYER, BPK_WP_LOCATION_LOADED, // World Partition (world_partition.h, B1)
    BPK_AUDIO_SET_PARAM, // Set Float Parameter: a MetaSound's input on an Audio Component (metasound.h, B8)
    BPK_PLUGIN_PURE, BPK_PLUGIN_CALL, // a plugin's node (plugins.h, B14c): text = its function's name; pins from the plugin
    BPK_GET_COMPONENT, // (2026-11-10) one of self's components by its name (dragged from Components): text = the name
    // (2026-11-14, "toda a chuva tem que ser modificável por blueprints") every Rain field by name, its presets, the drops'
    // color, what moves' own wetness (water.h)
    BPK_RAIN_PROP_SET, BPK_RAIN_PROP_GET, BPK_RAIN_PRESET, BPK_RAIN_DROP_COLOR, BPK_ACTOR_WETNESS, BPK_SET_ACTOR_WETNESS,
    BPK_RAIN_SHADING, BPK_RAIN_PUDDLE_COLOR, // (2026-11-15) the rain's shading style, the puddles' tint
    BPK_SET_LIGHTING_LEVEL, BPK_GET_LIGHTING_LEVEL, // (2026-11-16) a Lighting Level on or off, any mix at once (sublevels.h)
    BPK_SET_LIGHTING_BLEND, BPK_GET_LIGHTING_BLEND, // (2026-11-16) ... at a weight (a timeline fades or flickers it)
    BPK_TIMELINE, // (2026-11-16) Unreal's Timeline: text = the class's timeline (bp_timeline.h)
    // (2026-11-16) the active ragdoll (ikrig.h PHYS_ACTIVE)
    BPK_ACR_SET, BPK_ACR_STRENGTH, BPK_ACR_PROFILE, BPK_ACR_HIT, BPK_ACR_BALANCE, BPK_ACR_IS_BALANCED, BPK_ACR_GET_UP, BPK_ACR_AUTO_GET_UP,
    // (Q8) the rigid bodies' (physics.h: forces, torques, velocities, mass, sleep, gravity, material, CCD) and the
    // constraints' (drives, limits, an axis' motion, the force it holds)
    BPK_PHYS_ADD_FORCE, BPK_PHYS_ADD_TORQUE, BPK_PHYS_ANG_IMPULSE, BPK_PHYS_IMPULSE_AT, BPK_PHYS_SET_LIN_VEL, BPK_PHYS_SET_ANG_VEL,
    BPK_PHYS_GET_ANG_VEL, BPK_PHYS_SET_MASS, BPK_PHYS_GET_MASS, BPK_PHYS_WAKE, BPK_PHYS_SLEEP, BPK_PHYS_IS_AWAKE, BPK_PHYS_SET_GRAVITY,
    BPK_PHYS_SET_MATERIAL, BPK_PHYS_SET_CCD, BPK_CONSTRAINT_LIN_DRIVE, BPK_CONSTRAINT_ANG_DRIVE, BPK_CONSTRAINT_LIMITS,
    BPK_CONSTRAINT_MOTION, BPK_CONSTRAINT_FORCE,
    // (Q9) the soft bodies (cloth.h): an impulse, the stiffness, held or let go, any field by name
    BPK_SOFT_IMPULSE, BPK_SOFT_STIFFNESS, BPK_SOFT_PINNED, BPK_SOFT_PROP_SET,
    // (Q10, 2026-11-18) the ragdoll's blend by bone, lying face down; the dynamic bones; things on sockets in the game
    BPK_PHYS_BLEND_BELOW, BPK_ACR_FACE_DOWN, BPK_DYN_SET, BPK_DYN_WIND, BPK_DYN_RESET, BPK_ATTACH_SOCKET, BPK_DETACH_SOCKET,
    BPK_SOCKET_LOCATION, BPK_SOCKET_ROTATION,
    // (Q11) the toon and the outline in the game (toon_actor.h): a field of the actor's toon by name, a color, read one
    // back; the actor's own outline; a light's toon mode
    BPK_TOON_SET_PARAM, BPK_TOON_SET_COLOR, BPK_TOON_GET_PARAM, BPK_SET_ACTOR_OUTLINE, BPK_SET_LIGHT_TOON, // (Q11)
    // (Q12) the First Person Camera (fpcam.h: exertion, fear, recoil, focus, preset), Camera Shakes (camshake.h), Hide Bone by Name
    BPK_FP_SET_EXERTION, BPK_FP_SET_FEAR, BPK_FP_GET_EXERTION, BPK_FP_GET_FEAR, BPK_FP_RECOIL, BPK_FP_FOCUS_TARGET, BPK_FP_FOCUS_DISTANCE,
    BPK_FP_PRESET, BPK_SHAKE_START, BPK_SHAKE_STOP, BPK_SHAKE_STOP_ALL, BPK_SHAKE_WORLD, BPK_HIDE_BONE,
    // (Q13) the horror kit (terror.h): the Tension Director, the filter stack, the letterbox, the dips, the scare rigs, the
    // lights' flicker, Lighting Channels and Rim Lights
    BPK_SET_TENSION, BPK_ADD_TENSION, BPK_GET_TENSION, BPK_TRIGGER_FILTER, BPK_SET_FILTER, BPK_CLEAR_FILTERS, BPK_GET_FILTER,
    BPK_SET_LETTERBOX, BPK_DIP_BLACK, BPK_DIP_WHITE, BPK_SCARE_RIG, BPK_SET_LIGHT_FLICKER, BPK_SET_LIGHT_CHANNELS, BPK_SET_RIM_TARGET,
    // (Q14) splines and cables (spline.h): Unreal's Spline Component queries by distance and input key, its points edited,
    // the Cable's ends, length and a push
    BPK_SPLINE_LENGTH, BPK_SPLINE_LOC_AT_DIST, BPK_SPLINE_DIR_AT_DIST, BPK_SPLINE_ROT_AT_DIST, BPK_SPLINE_XFORM_AT_DIST, BPK_SPLINE_LOC_AT_KEY,
    BPK_SPLINE_DIST_AT_KEY, BPK_SPLINE_CLOSEST_KEY, BPK_SPLINE_CLOSEST_LOC, BPK_SPLINE_NUM_POINTS, BPK_SPLINE_POINT_LOC, BPK_SPLINE_ADD_POINT,
    BPK_SPLINE_SET_POINT, BPK_SPLINE_REMOVE_POINT, BPK_SPLINE_CLEAR, BPK_SPLINE_SET_CLOSED, BPK_SPLINE_SET_TYPE, BPK_CABLE_ATTACH_END,
    BPK_CABLE_ATTACH_START, BPK_CABLE_DETACH, BPK_CABLE_SET_LENGTH, BPK_CABLE_GET_LENGTH, BPK_CABLE_IMPULSE,
    // (Q15) PCG (pcg.h): a PCG component's generation from the Blueprints
    BPK_PCG_GENERATE, BPK_PCG_CLEANUP, BPK_PCG_SET_SEED, BPK_PCG_SET_PARAM, BPK_PCG_IS_GENERATED,
    // (Q16) everything editable in the game (notas-dev/em-jogo.md): Build Lighting while playing (runtime_lighting.h: the
    // latent node, its progress, cancel, the bake saved / loaded by a name, its event); the Details' properties by name
    // (props.h); the console variables (cvars.h)
    BPK_BUILD_LIGHTING, BPK_LIGHTING_PROGRESS, BPK_CANCEL_LIGHTING, BPK_SAVE_LIGHTING, BPK_LOAD_LIGHTING, BPK_EVENT_LIGHTING_BUILT,
    BPK_SET_PROPERTY, BPK_GET_PROPERTY, BPK_SET_CVAR, BPK_GET_CVAR, BPK_EXEC_CONSOLE, BPK_LIST_PROPERTIES,
    // (Q25) VR (vr_pawn.h): the VR Template's pawn from the Blueprints; its events on a grabbed actor and on the pawn
    BPK_VR_TELEPORT, BPK_VR_SET_LOCOMOTION, BPK_VR_SET_VIGNETTE, BPK_VR_GRAB, BPK_VR_RELEASE, BPK_VR_GET_HELD, BPK_VR_LASER,
    BPK_VR_HAND_TRACKING, BPK_VR_SET_REFRESH, BPK_EVENT_VR_GRABBED, BPK_EVENT_VR_RELEASED, BPK_EVENT_VR_TELEPORTED,
    BPK_VR_FINGER_CURLS, BPK_VR_HAND_GESTURE, BPK_VR_FINGER_TOUCH, BPK_VR_SET_HAND_COLLISION,
    // (Q25b) mixed reality (xr_mr.h): the passthrough, the room (Space Setup), the real world's traces; its event
    BPK_MR_SET_PASSTHROUGH, BPK_MR_IS_PASSTHROUGH, BPK_MR_SET_OPACITY, BPK_MR_SET_STYLE, BPK_MR_SET_EDGE, BPK_MR_SET_BACKGROUND,
    BPK_MR_SET_BLEND, BPK_MR_SET_WINDOW, BPK_MR_SET_OCCLUSION, BPK_MR_LOAD_ROOM, BPK_MR_ROOM_SETUP, BPK_MR_ROOM_PLANES,
    BPK_MR_PLANE_INFO, BPK_MR_TRACE, BPK_EVENT_MR_ROOM_LOADED,
    // (2026-11-19) the traces and the debug drawing (scene_query.h, debug_draw.h; notas-dev/traces.md)
    BPK_LINE_TRACE, BPK_SPHERE_TRACE, BPK_BOX_TRACE, BPK_CAPSULE_TRACE,
    BPK_MULTI_LINE_TRACE, BPK_MULTI_SPHERE_TRACE, BPK_MULTI_BOX_TRACE, BPK_MULTI_CAPSULE_TRACE,
    BPK_LINE_TRACE_BATCH, BPK_SPHERE_TRACE_BATCH,
    BPK_DRAW_DEBUG_LINE, BPK_DRAW_DEBUG_POINT, BPK_DRAW_DEBUG_BOX, BPK_DRAW_DEBUG_SPHERE, BPK_DRAW_DEBUG_CAPSULE,
    BPK_FLUSH_DEBUG_LINES, BPK_SET_TRACE_COLORS,
    BPK_ASYNC_LINE_TRACE_BATCH, BPK_ASYNC_SPHERE_TRACE_BATCH, BPK_GET_ASYNC_TRACE,
    // (Q27.1) the crowds (crowd.h): an alarm the agents near it panic at
    BPK_CROWD_ALARM,
    // (Q27.5) the active ragdoll's wounds (ikrig.h): a hand on it, a limp, writhing
    BPK_ACR_WOUND, BPK_ACR_GET_LIMP, BPK_ACR_IS_WRITHING,
    // (Q27.6) the regional weather (weather_field.h)
    BPK_ADD_STORM, BPK_CLEAR_STORMS, BPK_GET_RAIN_AT, BPK_GET_WIND_AT, BPK_GET_CLOUDS_AT,
    // (Q27.10) the city (city.h): the clock, the Smart Objects, a crime and the police
    BPK_GET_TIME_OF_DAY, BPK_SET_TIME_OF_DAY, BPK_REPORT_CRIME, BPK_GET_POLICE_CALLS, BPK_IS_OPEN_NOW, BPK_GET_VISITORS, BPK_SET_SMART_OBJECT,
    // (Q27.11) the locomotion styles (anim.h)
    BPK_SET_LOCO_STYLE, BPK_GET_LOCO_STYLE,
    // (2026-11-23) Motion Warping and paired montages (anim.h, anim_warp.inc)
    BPK_ADD_WARP_TARGET, BPK_REMOVE_WARP_TARGET, BPK_PLAY_PAIRED_MONTAGE, BPK_STOP_PAIRED_MONTAGE,
    BPK_TRY_TRAVERSAL, // (2026-11-23, anim_traversal.inc)
    BPK_FIND_PAIRED_MONTAGE, BPK_CAN_PLAY_PAIRED, // (2026-11-23)
    BPK_MONTAGE_COMBO_INPUT, BPK_IN_MONTAGE_WINDOW, BPK_EVENT_MONTAGE_WINDOW, BPK_EVENT_MONTAGE_HIT, // (2026-11-23, anim_combo.inc)
    BPK_PLAY_HIT_REACTION, // (2026-11-23)
    BPK_ENTER_COVER, BPK_EXIT_COVER, BPK_COVER_PEEK, BPK_IS_IN_COVER, // (2026-11-23, anim_cover.inc)
    BPK_PRELOAD_LEVEL, BPK_SET_LEVEL_LOAD_SPEED, BPK_SET_ASYNC_LOAD_SPEED, BPK_GET_PRELOAD_PROGRESS, BPK_CANCEL_PRELOAD, // (2026-11-23, level_preload.h)
    BPN_KIND_COUNT
} BpNodeKind;

// Play's collision boxes (bp_vm.c): the fraction of the segment a -> b a sphere of radius r travels before it touches
// one (1: nothing in the way). Nothing while not playing.
float bp_trace_boxes(const float a[3], const float b[3], float radius);
uint32_t bp_world_boxes(const float mn[3], const float mx[3], float (*out)[6], uint32_t cap); // (the ragdolls' contacts)

// The VM's ops. Operands: a register of the instance (< 0x8000) or a constant (BP_K | index). A native node's op is
// OP_NATIVE + its kind, with its inputs in a, b, c and its first output register in d.
enum {
    OP_END = 0,
    OP_MOVN,    // a <- b, c values
    OP_JMP,     // to a
    OP_JZ,      // a == 0: to b
    OP_JNZ,     // a != 0: to b
    OP_JGT,     // a > b: to c
    OP_JEQ,     // a == b: to c
    OP_PUSHRET, // a: where the next OP_RET goes
    OP_RET,     // back to the last OP_PUSHRET, or the end
    OP_DELTA,   // a <- the frame's seconds
    OP_INC,     // a += 1
    OP_LATENT,  // Delay: latent a, b seconds, resume at c (then returns)
    OP_PRINT,   // a (of type b)
    OP_CAST,    // c <- a when its class is b (a string), else none and to d
    OP_CALLIFACE, // the interface function b (a string) on actor a
    OP_DISPATCH,  // Event Dispatcher b (a string) of actor a called: the bound events run (the staged arguments)
    OP_CALLEV,    // Custom Event b (a string) run on actor a's instance (the staged arguments)
    OP_DBIND,     // this instance's event of dispatcher b bound to actor a's (c: 0 bind, 1 unbind, 2 unbind all)
    OP_SPAWN,   // c <- an actor of class b (a class value) at the transform a (3 registers)
    OP_COMP,    // a <- b's component c (a split vector / rotator / color)
    OP_SETCOMP, // a's component c <- b
    OP_TOSTR,   // a <- b (of type c) as a string
    OP_ACOPY,   // the container a <- a copy of the container b
    OP_NPARAM,  // a native's input a (from the 4th) <- b
    OP_ARG,     // an interface call's argument at a <- b, c values (d: strings made global)
    OP_GETARGS, // a <- the arguments, b values
    OP_PUTRES,  // the results <- a, b values
    OP_TAKERES, // a <- the results, b values
    OP_ALEN,    // a <- container b's length
    OP_AGET,    // a <- container b's element c (its index register)
    OP_JGE,     // a >= b: to c
    OP_LATENT2, // a latent action (d: its kind, LAT_*): its slot a, its input b, resume at c
    OP_CONV,    // a <- b converted from type c & 255 to type c >> 8 (Integer64, Byte, Color, Bool)
    OP_SPAWNARG, // (Expose on Spawn) the next OP_SPAWN's variable (name hash in register b) <- a (d: a string)
    OP_TIMELINE, // (2026-11-16) a Timeline node's input b (Play, Play from Start, Stop, Reverse, Reverse from End, Set New Time: c's time) on timeline a
    OP_BREAK = 63, // the debugger's (bp_debug.h): a breakpoint, a step or a trace at a node's start; a: its patch (the op it replaced)
    OP_NATIVE = 64,
};
#define BP_MAX_NPARAM 8 // a native's inputs past the third (OP_NPARAM)
enum { LAT_RETRIGGER = 1, LAT_SPAWN, LAT_STREAM_LOAD, LAT_STREAM_UNLOAD, LAT_LIGHTING /* (Q16) Build Lighting (Async)'s Completed */ };

#define BP_K 0x8000u
#define BP_MAX_CALL 32  // the return stack
#define BP_OP_BUDGET 1000000u // per run: an infinite loop stops here (an error on the screen, like Unreal's)

// GLFW key codes of the names a Key Event / Is Key Down takes ("Space", "E", "F1", "Left Mouse Button", ...)
int bp_key_code(const char* name);
// the axes: 0 MoveForward, 1 MoveRight, 2 Turn, 3 LookUp (-1 unknown)
int bp_axis_index(const char* name);
