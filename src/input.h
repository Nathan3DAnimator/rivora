// Enhanced Input (Unreal's): Input Actions (.rinputaction: what the player does, its value type, triggers and
// modifiers) and Input Mapping Contexts (.rimc: keys, mouse and gamepad axes mapped to actions, each with its own
// modifiers and triggers). Contexts are added while playing (Blueprint: Add Mapping Context, or Project Settings'
// default); every frame the actions' values and events (Started, Ongoing, Triggered, Completed, Canceled) are worked out
// before the Blueprints tick, which run the Enhanced Input Action events (bp_vm.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define IN_NAME 48
#define IN_MAX_MODS 4
#define IN_MAX_TRIGS 3
#define IN_MAX_MAPS 64

typedef enum InValueType { IAV_BOOL = 0, IAV_AXIS1D, IAV_AXIS2D, IAV_AXIS3D, IAV_COUNT } InValueType;
extern const char* const g_in_value_names[IAV_COUNT]; // "Digital (bool)", "Axis1D (float)", ...

typedef enum InModKind { IMOD_NEGATE = 0, IMOD_SWIZZLE, IMOD_DEADZONE, IMOD_SCALAR, IMOD_COUNT } InModKind;
extern const char* const g_in_mod_names[IMOD_COUNT];
typedef struct InMod {
    uint8_t kind;
    uint8_t flags;   // Negate: x 1, y 2, z 4; Swizzle: 0 YXZ, 1 ZYX, 2 XZY, 3 YZX, 4 ZXY
    float v[3];      // Dead Zone: lower, upper; Scalar: the factors
} InMod;

typedef enum InTrigKind { ITRG_DOWN = 0, ITRG_PRESSED, ITRG_RELEASED, ITRG_HOLD, ITRG_HOLD_RELEASE, ITRG_TAP, ITRG_PULSE, ITRG_COUNT } InTrigKind;
extern const char* const g_in_trig_names[ITRG_COUNT];
typedef struct InTrig {
    uint8_t kind;
    bool once;       // Hold: triggers once (else every frame after the time)
    float time;      // Hold / Hold and Release: the hold; Tap: the most it may last; Pulse: the interval
    float threshold; // actuation (0.5 by default)
} InTrig;

typedef struct InputAction {
    char name[IN_NAME], path[512];
    uint32_t hash;
    uint8_t value_type;
    bool consume;    // Consume Input: lower priority contexts don't see its keys
    InTrig trig[IN_MAX_TRIGS];
    uint8_t ntrig;
    InMod mods[IN_MAX_MODS];
    uint8_t nmods;
} InputAction;

typedef struct InputMapping {
    char action[IN_NAME];
    char key[IN_NAME];   // "W", "Space", "Left Mouse Button", "Mouse XY 2D-Axis", "Gamepad Left Thumbstick 2D-Axis", ...
    InMod mods[IN_MAX_MODS];
    uint8_t nmods;
    InTrig trig[IN_MAX_TRIGS];
    uint8_t ntrig;
} InputMapping;

typedef struct InputContext {
    char name[IN_NAME], path[512];
    uint32_t hash;
    InputMapping maps[IN_MAX_MAPS];
    uint32_t nmaps;
} InputContext;

// --- Assets ---
void input_init(const char* content_root, bool make_defaults); // IA_Move, IA_Look, IA_Jump, IMC_Default when missing
InputAction* input_action_load(const char* vpath);
InputContext* input_context_load(const char* vpath);
InputAction* input_action_named(const char* name);   // (the Content folder scanned once)
InputContext* input_context_named(const char* name);
uint32_t input_action_count(void);
InputAction* input_action_at(uint32_t i);
uint32_t input_context_count(void);
InputContext* input_context_at(uint32_t i);
InputAction* input_action_create(const char* folder_vpath, const char* name);
InputContext* input_context_create(const char* folder_vpath, const char* name);
bool input_action_save(const InputAction* a);
bool input_context_save(const InputContext* c);
// The key names a mapping takes (the editors' lists)
uint32_t input_key_count(void);
const char* input_key_name(uint32_t i);

// --- Play ---
void input_begin_play(void);                 // no context, every action at rest
void input_add_context(const char* name, int priority);
void input_remove_context(const char* name);
// The frame's devices: keys and buttons (GLFW codes, 500/501 the mouse buttons), the mouse's move (pixels, y up)
void input_set_devices(const uint8_t* keys, uint32_t nkeys, const float mouse_delta[2], float wheel);
void input_update(float dt);                 // the actions' values and events this frame
// An action's state: its value (x y z), and which events fired this frame (bit per InEvent), how long it has been on
typedef enum InEvent { IEV_TRIGGERED = 0, IEV_STARTED, IEV_ONGOING, IEV_CANCELED, IEV_COMPLETED, IEV_COUNT } InEvent;
bool input_action_state(const char* name, float value[3], uint32_t* events, float* elapsed, float* triggered_s);
// The gamepad was the device used last (a button or a stick; the keyboard or the mouse since: false)
bool input_gamepad_used(void);
// A key, mouse button or gamepad button held now, by its Enhanced Input name ("E", "Gamepad Face Button Left")
bool input_key_held(const char* key);
