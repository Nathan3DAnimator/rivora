// The component types. Plain data (no pointers, no vtables): a component is a row of one column of its archetype.
// Adding one: a struct here, an id in ComponentId and a line in g_components (ecs.c).
#pragma once
#include <stdint.h>

typedef enum ComponentId {
    COMP_NAME = 0,      // the label in the Outliner
    COMP_TRANSFORM,     // location, rotation, scale, mobility
    COMP_ROTATING,      // turns the transform every frame while playing (Unreal's Rotating Movement)
    COMP_STATIC_MESH,   // a mesh with its material
    COMP_LIGHT,         // directional, point, spot, rect or sky light
    COMP_CAMERA,        // a view the player can take (Camera Actor)
    COMP_PLAYER_START,  // where Play in Editor puts the player
    COMP_FOG,           // Exponential Height Fog (volumetric)
    COMP_CLOUD,         // Volumetric Cloud
    COMP_BLUEPRINT,     // the actor is an instance of a Blueprint Class (blueprint.h)
    COMP_CHARACTER,     // Unreal's Character Movement: a capsule that walks, falls and jumps, possessed by the player
    // A Blueprint's components (its component tree, Unreal's Simple Construction Script) as child entities of the actor
    COMP_ATTACH,        // hangs from a parent entity at a relative transform (bp_components.c)
    COMP_SPRING_ARM,    // Unreal's Spring Arm: what hangs from it sits at the arm's end (camera boom, lag, collision)
    COMP_COLLISION,     // a Box / Sphere / Capsule Collision (overlaps, blocking)
    COMP_PROJECTILE,    // Unreal's Projectile Movement (on the actor)
    COMP_FLOATING,      // Unreal's Floating Pawn Movement (on the actor)
    COMP_ARROW,         // Unreal's Arrow Component (shown in the editor)
    COMP_RIGID_BODY,    // Simulate Physics (physics.c)
    COMP_SKELETAL_MESH, // a Skeletal Mesh and its animation (anim.c)
    COMP_AI_AGENT,      // an AI: its Behavior Tree or Brain, its senses (ai.c)
    COMP_FOLDER,        // the Outliner folder an actor is in (editor only: organizes the list)
    COMP_OUTLINER_FOLDER, // an Outliner folder itself (an entity with a name and this: no transform, nothing in the world)
    COMP_STREAMED,      // an actor a streamed level brought in (level_stream.c): its level's id
    COMP_POST_PROCESS,  // Unreal's Post Process Volume (post.h)
    COMP_CROWD,         // a Crowd Spawner: thousands of animated agents with AI, simulated and drawn as one (crowd.c)
    COMP_LANDSCAPE,     // a Landscape: a height grid, its chunk meshes and plants (landscape.c)
    COMP_PROTO,         // a prototyping shape: stairs, walls, rooms... built from boxes by its numbers (proto.c)
    COMP_FOLIAGE,       // Unreal's Foliage (its Instanced Foliage Actor): the painted plants and their types (foliage.c)
    COMP_AUDIO,         // Unreal's Audio Component (an Ambient Sound actor; a Blueprint's): a sound where it is (audio_world.c)
    COMP_AUDIO_VOLUME,  // Unreal's Audio Volume: a box with its reverb and its inside / outside levels (audio_world.c)
    COMP_PARTICLE_SYSTEM, // Unreal's Niagara Component: a Particle System asset played where the actor is (particles.h)
    COMP_PARTICLE_TL,   // the Tlender's (Blender's) particle system on a mesh actor: emitted from its mesh (particles.h)
    COMP_VEHICLE,       // a vehicle: a Vehicle asset driving the actor (wheels, engine, wings, rotors, floats...) (vehicle.h)
    COMP_WATER,         // a Water Body: a surface boats float on (its height, size, waves, current) (vehicle.h)
    COMP_RAIL,          // (Q14: COMP_SPLINE) a Spline: a Rail trains run on, a Spline (meshes along it) or a Cable (spline.h)
    COMP_PROC_WORLD,    // a Procedural World: terrain or a facility made from a seed around the player (procgen.h)
    COMP_RAIN,          // Rain (the Rain actor): drops, splashes, wet surfaces, puddles and the water that runs (water.h)
    COMP_CLOTH,         // Cloth: a simulated sheet (flag, curtain, cape) on the GPU, colliding, tearing (cloth.h)
    COMP_GAMEPLAY_TAGS, // Unreal's Gameplay Tags (and the actor's Tags): hierarchical names, "Damage.Fire" (gameplay_tags.h)
    COMP_CONSTRAINT,    // Unreal's Physics Constraint: two bodies (or one and the world) held by a joint at this actor (physics.c)
    COMP_NAV,           // navigation: a Nav Mesh Bounds Volume, a Nav Link Proxy or a Nav Modifier Volume (navmesh.h)
    COMP_HEALTH,        // Health: life, shield, regeneration, damage by type (Gameplay Tags), death (health.h)
    COMP_INVENTORY,     // an Inventory: slots of items, weight, equipment (inventory.h)
    COMP_PICKUP,        // a Pickup: an item lying in the world, taken on touch or by interacting (inventory.h)
    COMP_SAVEABLE,      // Saveable: the actor's place, Health, Inventory, SaveGame variables and destruction go in the saves (save_progress.h)
    COMP_CHECKPOINT,    // a Checkpoint: a volume that saves the progress when the player walks in, its respawn point (save_progress.h)
    COMP_INTERACTABLE,  // an Interactable: what the player can use (a prompt, a key, a hold, a condition) (interaction.h)
    COMP_INTERACTOR,    // an Interactor: finds the best Interactable in view and uses it (the player's pawn) (interaction.h)
    COMP_DEVICE,        // a ready-made interactive actor: Door, Chest, Lever, Button (interaction.h)
    COMP_DIALOGUE_SPEAKER, // a Dialogue Speaker: the NPC's conversation, the name shown, the camera (dialogue.h)
    COMP_QUEST_LOG,     // a Quest Log: the actor's quests (the player's or a Game State's), its tracker and markers (quest.h)
    COMP_GAME_RULES,    // Game Rules: the match's phases, rounds, score, win / lose, lives and respawn, clock (game_rules.h)
    COMP_ABILITY_SYSTEM, // an Ability System: abilities, attributes (mana, speed...), Gameplay Effects, cooldowns (ability.h)
    COMP_HUD_LAYOUT,    // a HUD Layout: the game's HUD widget, its scale, safe area, hiding and pieces (hud.h)
    COMP_DECAL,         // Unreal's Decal: a material projected on what is inside its box (dirt, blood, cracks, bullet holes) (decal.h)
    COMP_GROOM,         // Unreal's Groom Component: hair in strands grown from the actor's mesh, drawn and lit as fibers (groom.h)
    COMP_FIRE,          // Fire: a gas burning on a grid of its own on the GPU (Niagara Fluids' Grid 3D Gas), its light flickering (fire.h)
    COMP_RIVER,         // Unreal's Water Body River: a Water Body's spline (points with width, depth, speed): the water runs along it (river.h)
    COMP_BUOYANCY,      // Unreal's Buoyancy Component: a physics body floats in any water (lakes, rivers, the ocean), the current carries it
    COMP_FOG_VOLUME,    // Unreal's Local Fog Volume: fog in a box or a sphere, lit with the volumetric fog (weather.c)
    COMP_SPLAT,         // Gaussian Splatting: a scanned scene's 3D Gaussians (3DGS .ply, .splat) drawn over the scene (splat.h)
    COMP_VOLUME,        // Unreal's Heterogeneous Volume: an OpenVDB file or sequence (smoke, fire, explosions) played (volume.h)
    COMP_WORLD_PARTITION, // the level's World Partition (one: Unreal's World Settings > World Partition): grid, Data Layers, HLOD (world_partition.h)
    COMP_PARTITION,     // an actor's World Partition settings: Is Spatially Loaded, its Data Layers (world_partition.h)
    COMP_COUNT
} ComponentId;
_Static_assert(COMP_COUNT <= 64, "the component masks are 64 bits (COMP_BIT): widen them before a 65th component");
typedef struct CStreamed { uint32_t level; } CStreamed;

// Post processing (post.h: the fields' names and defaults, the blend)
#define PP_FIELDS 24
typedef struct PostSettings {
    float v[PP_FIELDS];
    float color_tint[3]; // Color Grading's Gain as a color (white: none)
} PostSettings;
// (Q13) the Tension Director's channels (terror.h): grain, vignette, saturation, letterbox, FOV, pulse, light flicker,
// a Sound Class's volume and low-pass
#define TENSION_CHANNELS 9
// The volume (on an actor: its box is its transform's, 100 cm per unit of scale each way)
typedef struct CPostProcess {
    PostSettings s;
    uint32_t overrides;   // bit per field: the volume sets it (else the lower ones' or the default)
    float priority;
    float blend_radius;   // cm outside the box over which it fades in
    float blend_weight;   // 0..1
    uint32_t flags;       // PPV_UNBOUND, PPV_ENABLED
    // (new fields at the end: the levels saved before keep loading; ecs.c reads their rows at the old size)
    char lut[120];        // Color Grading LUT: "/Content/..." a .cube file or a PNG strip (N*N x N, Unreal's 256 x 16); "": none
    float lut_intensity;  // 0..1 (Unreal's Color Grading LUT Intensity)
    // (Q13) the horror kit's (terror.h): Details > Horror. Grown at the end: old levels read zeros (none of it)
    uint32_t terror_flags;      // TERROR_PPV_EXPOSURE: the horror exposure; TERROR_PPV_TENSION: these tension curves
    float terror_toe;           // 0..1: the shadows pushed toward black (the tone curve's toe), never lifted
    float terror_ev_floor;      // EV100: the auto exposure never adapts brighter than a scene this dark (the room stays dark)
    float terror_meter;         // 0.5..0.99: the share of the picture under the metered level (the highlights decide; 0: 0.95)
    float tension_curve[TENSION_CHANNELS][4]; // each channel's value at tension 0, 1/3, 2/3, 1 (terror.c g_tension_channels)
    float tension_rise, tension_fall;    // per second: how fast the tension follows Set Tension's target (0: 0.5, 0.25)
    char tension_sound_class[48];        // a Sound Class whose volume and low-pass the tension drives ("": none)
    char tension_param[32];              // a MetaSound input every playing Audio Component gets, the tension 0..1 ("": none)
} CPostProcess;
#define TERROR_PPV_EXPOSURE 1u
#define TERROR_PPV_TENSION 2u
#define PPV_UNBOUND 1u // Infinite Extent (Unbound): everywhere
#define PPV_ENABLED 2u

// Unreal's Outliner folders: ids (not entities) so they survive saving; a folder's color tints its icon
typedef struct CFolder { uint32_t id; } CFolder;
#define FOLDER_COLLAPSED 1u
typedef struct COutlinerFolder { uint32_t id, parent; float color[3]; uint32_t flags; } COutlinerFolder;

#define COMP_BIT(id) (1ull << (id))
// Every component's bit. The masks are 64 bits and COMP_COUNT reached 64 with B1 (World Partition): a shift by 64 is
// undefined in C (x64 takes it as 0: (1 << 64) - 1 = 0, every Blueprint lost its components), hence the & 63. A 65th
// component needs wider masks (two words) first
#define COMP_ALL (COMP_COUNT >= 64 ? ~0ull : (1ull << (COMP_COUNT & 63)) - 1ull)

// Unreal's Audio Component (audio.h): a Sound Wave or Sound Cue played where the entity is, following it
typedef struct CAudio {
    uint32_t sound;           // a Sound Wave's or Sound Cue's hash
    float volume, pitch;      // multipliers
    uint32_t auto_activate;   // plays when the game starts (Unreal's Auto Activate)
    uint32_t attenuation, sound_class, concurrency; // overrides (0: the sound's)
    float fade_in, start_time; // seconds
    uint32_t spatialize;      // 3D where it is (0: 2D, music and UI: Unreal's Allow Spatialization off)
    uint32_t ui;              // plays while the game is paused
    uint32_t handle;          // (the run's: the playing sound, audio.h)
} CAudio;

// Unreal's Audio Volume: its box is its transform's (100 cm per unit of scale each way); inside, its reverb; the
// Interior Settings: what is outside heard quieter and darker from inside (and the other way)
typedef struct CAudioVolume {
    float priority;
    uint32_t enabled, reverb_on;
    float reverb[8];          // the reverb's (audio_internal.h AUFX_REVERB: decay, HF ratio, density, pre-delay ms, wet, dry, early, diffusion)
    float fade_time;          // seconds to blend to it
    float exterior_volume, exterior_lpf; // sounds outside, heard from inside (1, 20000: unchanged)
    float interior_volume, interior_lpf; // sounds inside, heard from outside
} CAudioVolume;

#define RE_NAME_MAX 64
typedef struct CName {
    char text[RE_NAME_MAX]; // UTF-8, NUL terminated
} CName;

// Unreal's mobility: what may change after the light is built (the GI bakes Static, updates Movable every frame)
typedef enum Mobility { MOBILITY_STATIC = 0, MOBILITY_STATIONARY, MOBILITY_MOVABLE } Mobility;
#define TRANSFORM_MOBILITY_MASK 3u
#define TRANSFORM_HIDDEN (1u << 2) // hidden in the editor (the Outliner's eye)
#define TRANSFORM_HIDDEN_GAME (1u << 3) // hidden while playing (a Blueprint's Set Actor Hidden In Game)
#define TRANSFORM_NO_ICON (1u << 4) // its icon not drawn in the viewport (Details > Show Icon, Actor > Visibility)
#define TRANSFORM_LEVEL_HIDDEN (1u << 6) // (sublevels.c) hidden by its level's eye or a bake's light mask, not by itself: shown again with it, never saved
#define TRANSFORM_PCG (1u << 7) // (Q15) made by a PCG component (pcg.h): never saved, made again from its graph
#define TRANSFORM_NO_COLLISION (1u << 5) // its mesh doesn't block Characters, traces and the camera boom (Unreal's NoCollision)
#define TRANSFORM_NO_RENDER (1u << 8) // (Q25b) it collides but is never drawn (mixed reality's room: the real one is seen; never saved)

// Unreal's units and axes: centimeters, Z up; rotation in degrees (pitch about Y, yaw about Z, roll about X).
typedef struct CTransform {
    float location[3];
    float rotation[3]; // pitch, yaw, roll
    float scale[3];
    uint32_t flags;    // Mobility in the low bits, TRANSFORM_HIDDEN
} CTransform;

typedef struct CRotating {
    float rate[3]; // degrees per second: pitch, yaw, roll
} CRotating;

// The engine's basic shapes (Unreal's /Engine/BasicShapes): 100 cm, pivot at the center (the plane: flat at z 0)
typedef enum BasicShape { SHAPE_CUBE = 0, SHAPE_SPHERE, SHAPE_CYLINDER, SHAPE_CONE, SHAPE_PLANE, SHAPE_COUNT } BasicShape;
extern const char* const g_shape_names[SHAPE_COUNT];

#define CAST_SHADOW_NO_OCCLUDER 2u
#define CAST_SHADOW_WIND 4u // (a landscape's plant: its vertices sway in the wind, mesh.vert)
#define CAST_SHADOW_ROUND_OCCLUDER 8u // (2026-11-14) a skeletal mesh's part or skin: the contact AO sees it round (render.c fill_occluders), never as its box
typedef struct CStaticMesh {
    uint32_t mesh;         // a BasicShape (imported meshes come after them)
    float base_color[3];   // linear
    float metallic;
    float roughness;
    float emissive[3];     // linear, added to the lit color
    uint32_t cast_shadow;  // bit 0; CAST_SHADOW_NO_OCCLUDER: not a contact AO occluder (a skeletal mesh's small parts)
    uint32_t material;     // a material or Material Instance asset (its path's hash, material.h); 0: the colors above
    float uv_scale;        // the material's tiling (0 read as 1)
    // (Q11) the actor's own outline over its material's (toon.h OUTLINE_ACTOR_: the top byte; 0 the material says, 1 on,
    // 2 off), its color (sRGB bytes below) and width (px at 1440p; 0: the material's or 2.5). Old levels read 0
    uint32_t outline;
    float outline_width;
    uint32_t light_channels; // (Q13) Unreal's Lighting Channels, bits 0-2 (0 read as channel 0): the lights it shares one with light it
} CStaticMesh;

typedef enum LightType { LIGHT_DIRECTIONAL = 0, LIGHT_POINT, LIGHT_SPOT, LIGHT_RECT, LIGHT_SKY, LIGHT_TYPE_COUNT } LightType;
extern const char* const g_light_names[LIGHT_TYPE_COUNT];

// Directional: intensity is the irradiance on a surface facing it. Point/Spot/Rect: radiant intensity, falling with the
// square of the distance in meters, cut smoothly at attenuation_radius. Sky: scales the sky's light.
typedef struct CLight {
    uint32_t type;           // LightType
    float color[3];          // linear
    float intensity;
    float attenuation_radius; // cm
    float inner_cone, outer_cone; // degrees (spot)
    float source_width, source_height; // cm (rect)
    float source_radius;     // cm (soft shadows / spheres lights)
    uint32_t cast_shadows;
    // Unreal's light options (a light saved before them reads ext 0: lights.c uses their defaults)
    uint32_t units;            // LightUnits: how Details shows the intensity (stored in cd; lux for a directional one)
    uint32_t use_temperature;  // the color times a black body at temperature K (6500: white)
    float temperature;
    uint32_t affects_world;    // off: the light lights nothing (Unreal's Affects World)
    float indirect_intensity;  // its share in the GI (bounces, the bake), 1 physical
    float specular_scale;      // its highlights, 1 physical
    float volumetric_scattering; // its light in the volumetric fog, 1 physical
    uint32_t ext;              // 1: the fields above are set
    // (2026-11-17, Q4) Unreal's Light Function: a material of the Light Function domain multiplying the light (a spot's
    // cone, a point light's directions, a directional light's world tiled every light_function_scale cm); 0: none.
    // Old levels read 0 here (none)
    uint32_t light_function;
    float light_function_scale;
    // (Q11) the toon: 0 the material says (a Toon material in bands), 1 Force Toon (every material in two bands under
    // this light), 2 Never (a Toon material lit smooth by it: a fill light). Old levels read 0
    uint32_t toon_mode;
    // (Q13) the horror kit (terror.h). Grown at the end: old levels read zeros (none of it)
    uint32_t light_channels;   // Unreal's Lighting Channels, bits 0-2 (0 read as channel 0): it lights only meshes sharing one
    uint32_t flicker;          // LightFlicker: none, candle, faulty bulb, strobe, lightning, pulse
    float flicker_amount;      // 0..1 how deep (0 read as 1 with a pattern)
    float flicker_speed;       // x the pattern's own rate (0 read as 1)
    float flicker_tension;     // 0..1: the Tension Director's Light Flicker curve shakes it this much (a faulty bulb's)
    uint32_t flicker_sound;    // a sound played at each strike, pop or flash (a Sound Wave's or Cue's hash; 0: none)
    float flicker_sound_delay; // s after the flash (the thunder after the lightning)
    uint32_t rim;              // RIM_: a Rim Light kept behind its target, seen from the camera (the silhouette's edge)
    char rim_target[48];       // RIM_ACTOR: the actor's name (the Outliner's)
    float rim_distance, rim_height, rim_side; // cm behind the target (away from the camera; 0: 250), above it, to its side
    uint32_t beam_dust;        // its cone in the volumetric fog shows drifting dust (a flashlight's beam)
    // (Q21) a directional light's far shadow (farview.h): the terrain shading it kilometers past its virtual maps, and how
    // dark (1: the terrain's whole shadow). Grown at the end: old levels read the defaults (on, 1)
    uint32_t far_shadow;
    float far_shadow_strength;
} CLight;
typedef enum LightFlicker { FLICKER_NONE = 0, FLICKER_CANDLE, FLICKER_FAULTY, FLICKER_STROBE, FLICKER_LIGHTNING, FLICKER_PULSE, FLICKER_COUNT } LightFlicker;
#define RIM_PLAYER 1u // behind the player's pawn
#define RIM_ACTOR 2u  // behind rim_target

// The intensity's units in Details (like Unreal, plus Blender's Watts: the Tlender's lights keep their look)
typedef enum LightUnits { LIGHT_UNITS_CANDELAS = 0, LIGHT_UNITS_LUMENS, LIGHT_UNITS_EV, LIGHT_UNITS_WATTS, LIGHT_UNITS_COUNT } LightUnits;

typedef struct CCamera {
    float fov_deg;         // horizontal
    uint32_t auto_activate; // the player's view while playing (Unreal's "Auto Activate for Player 0")
    uint32_t no_camera_blur; // Motion Blur: the camera's own motion doesn't blur the view (the world's moving things still do)
    // Unreal's Camera Component Post Process Settings: what it overrides while it is the player's view (bit per entry of
    // g_camera_post_fields, post.h), how much (0: all of it), the values. Grown at the end: old levels read them as zero
    uint32_t post_overrides;
    float post_weight;
    float post[14];
    // (A9.4) Unreal's Cine Camera: the physical lens and exposure while it is the player's view (post.c). Grown at the end
    uint32_t cine;        // CINE_LENS | CINE_EXPOSURE | CINE_SHUTTER_BLUR (0: a plain camera)
    float focal_mm;       // the lens's focal length (mm); with the sensor, the field of view
    float sensor_mm;      // the filmback's width (mm: 36 full frame, 24.89 Super 35)
    float aperture;       // f-stop (the depth of field; with ISO and shutter, the exposure)
    float focus_cm;       // the focus distance (0: no depth of field)
    float iso, shutter_s; // the sensor's sensitivity, the shutter's time (s: 1/60 = 0.0167)
    uint32_t blades;      // the iris's blades (0: round bokeh; 5 to 9: a polygon)
    float blade_rot;      // their rotation (degrees)
    // (Q12) First Person Camera (fpcam.h): the player's view moved by the steps, the look's inertia, the lean, breathing,
    // the landing, the recoil, the Camera Shakes; autofocus, the effort's vignette, the body seen. Grown at the end:
    // older levels and Blueprints read 0 (off)
    uint32_t fp;              // FP_* (fpcam.h; 0: a plain camera)
    uint32_t fp_preset;       // FPP_* last applied (the Details' menu; the values below are what count)
    float bob_up_cm, bob_side_cm, bob_roll_deg; // the head's bob at Bob Speed (down at a contact, to the stance foot's side)
    float bob_speed;          // cm/s at which the bob is whole (up to 1.5x faster)
    float stride_cm;          // a step when no Footstep notify comes (the phase from the distance walked)
    float look_lag_hz, look_damping; // the look's spring (Hz; 0: none) and its damping (1: no overshoot)
    float lean_turn_deg, lean_strafe_deg; // the roll into a 180 deg/s turn, at a full-speed strafe
    float breath_deg, breath_rate;   // the breathing's pitch at rest, breaths a minute
    float land_cm_per_m, land_max_cm; // the landing's dip per meter fallen, its most
    float recoil_hz;          // how fast a recoil comes back
    float focus_speed, focus_min_cm, focus_max_cm, focus_fstop; // autofocus: its ease (1/s), its range, the f-stop (no Cine lens)
    float vignette_effort;    // the vignette added at full exertion
    char hide_bone[24];       // the body seen: this bone and below hidden ("head")
} CCamera;
#define CINE_LENS 1u         // focal length + sensor: the field of view; aperture + focus: a physical depth of field
#define CINE_EXPOSURE 2u     // manual exposure: EV100 = log2(N^2 / t) - log2(ISO / 100)
#define CINE_SHUTTER_BLUR 4u // the motion blur by the shutter (its time x the post's Target FPS)

typedef struct CPlayerStart {
    uint32_t reserved;
} CPlayerStart;

typedef struct CAttach {
    uint32_t owner;           // the actor (the Blueprint instance) this component belongs to (an Entity)
    uint32_t parent;          // the entity it hangs from: the actor or another of its components
    float location[3], rotation[3], scale[3]; // relative to the parent (cm, degrees)
    uint32_t node;            // its index in the class's component tree (a part: in its Skeletal Mesh)
    uint32_t kind;            // AttachKind
} CAttach;
// What a child entity is: a Blueprint's component (bp_components.c places it), a part of a Skeletal Mesh or the
// instance of its Animation Blueprint (anim.c)
typedef enum AttachKind { ATTACH_SCS = 0, ATTACH_SK_PART, ATTACH_ANIM_BP, ATTACH_LANDSCAPE, ATTACH_PROTO, ATTACH_FOLIAGE, ATTACH_SK_SKIN, ATTACH_PROCGEN,
    ATTACH_BONE, ATTACH_SPLINE, ATTACH_PCG } AttachKind; // (Q10) ATTACH_BONE: hung on a Skeletal Mesh's bone or socket (anim_attach_to_socket: anim.c places it)
// (Q14) ATTACH_SPLINE: a Spline's piece (spline.c places it): node SPLINE_NODE_MESH | i a Spline Mesh piece, SPLINE_NODE_INST | i an
// instance, SPLINE_NODE_CABLE | i a Cable's segment
// (Q15) ATTACH_PCG: a PCG component's mesh (pcg.h)

// Unreal's Skeletal Mesh Component: the mesh (.rskm) and what animates it
typedef struct CSkeletalMesh {
    uint32_t mesh;        // the Skeletal Mesh (the hash of its vpath)
    uint32_t anim_class;  // an Animation Blueprint (its class's hash; 0: the Animation below)
    uint32_t anim;        // an Animation Sequence (Unreal's "Use Animation Asset")
    float play_rate;
    uint32_t loop;
    uint32_t built;       // (the run's: the mesh its parts were made for)
    uint32_t inst;        // (the run's: its animation instance + 1)
    // IK Rig and Physics Asset (ikrig.h): their hashes (0: none), the start (0 off, 1 ragdoll, 2 the bodies below
    // simulate_bone) and the physics' weight over the animation
    uint32_t ik_rig, physics_asset;
    uint32_t simulate;
    char simulate_bone[32];
    float physics_blend;
    // Morph targets (anim.h ANIM_MAX_MORPHS, by the Skeletal Mesh's list): the weights set on this actor (Details, Set
    // Morph Target) and which are set (a bit each; the others: the Skeletal Mesh's default). Levels before: none set
    float morph_weight[64];
    uint32_t morph_set[2];
} CSkeletalMesh;

typedef struct CSpringArm {
    float target_arm_length;  // cm
    float socket_offset[3];   // at the arm's end (its own axes)
    float target_offset[3];   // at its start (world axes)
    uint32_t do_collision_test; // the arm shortens when something stands between the start and the end
    uint32_t enable_camera_lag;
    float camera_lag_speed;   // 1/s
    uint32_t use_pawn_control_rotation; // the arm turns with the Character's control rotation (the mouse), not the actor
    float probe_size;         // cm
    float current_length;     // (the run's: after the collision test)
    float lag_pos[3];         // (the run's: the lagged end)
} CSpringArm;

typedef enum CollisionShape { COLLISION_BOX = 0, COLLISION_SPHERE, COLLISION_CAPSULE } CollisionShape;
typedef struct CCollision {
    uint32_t shape;           // CollisionShape
    float extent[3];          // box: half extents; sphere: radius in [0]; capsule: radius [0], half height [1] (cm)
    uint32_t generate_overlap_events;
    uint32_t block;           // blocks Characters and physics
} CCollision;

typedef struct CProjectile {
    float initial_speed, max_speed; // cm/s (0: no limit)
    float gravity_scale, bounciness, friction;
    uint32_t should_bounce, rotation_follows_velocity, active;
    float velocity[3];        // local at the start (times initial_speed when it is set), then the world's
    uint32_t started;
} CProjectile;

typedef struct CFloating {
    float max_speed, acceleration, deceleration; // cm/s, cm/s²
    float velocity[3];
    float input[3];           // Add Movement Input this frame (consumed)
} CFloating;

typedef struct CArrow {
    float color[3];
    float length;             // cm
} CArrow;

// Unreal's Physics section: Simulate Physics, the mass, the damping, the material; Fracture: Chaos' Geometry Collection
typedef struct CRigidBody {
    uint32_t simulate;         // Simulate Physics
    uint32_t enable_gravity;
    float mass;                // kg (0: from the volume, 100 kg per cubic meter)
    float linear_damping, angular_damping;
    float friction, restitution;
    uint32_t fracture;         // breaks into pieces on a hard hit
    float fracture_speed;      // cm/s: the hit that breaks it
    uint32_t fracture_pieces;  // per axis (2-4)
    // the run's (zero in the level)
    float velocity[3];         // cm/s
    float angular_velocity[3]; // rad/s (world)
    uint32_t started, sleeping;
    float sleep_time;
    float rest_pos[3], rest_axis[3]; // where it has stayed (a body trembling in place sleeps too)
    // (added after the run's fields: a level from before reads them as 0, Automatic and simple)
    uint32_t collision_shape;   // COLSHAPE_ (Details > Physics > Collision Shape, Unreal's)
    uint32_t complex_collision; // not simulating: its mesh's triangles collide (Unreal's Use Complex Collision As Simple)
    // (Q8, notas-dev/fisica.md "Corpos rígidos profissionais")
    uint32_t ccd;               // continuous collision: swept, it never goes through a wall (Unreal's Use CCD)
    uint32_t phys_material;     // a Physical Material (.rphysmat, its path's hash): friction, bounce, density; 0: the fields above
    uint32_t sleep_group;       // the run's: the island it fell asleep with (they wake together)
    float max_angular_velocity; // deg/s (0: no limit; Unreal's Max Angular Velocity)
} CRigidBody;
// The Collision Shape (physics.c): Automatic is the cube a box, the sphere a sphere, the cylinder a cylinder, the rest
// (the cone, imported meshes) the convex hull of the mesh
enum { COLSHAPE_AUTO = 0, COLSHAPE_BOX, COLSHAPE_SPHERE, COLSHAPE_CAPSULE, COLSHAPE_CONVEX, COLSHAPE_COUNT };
extern const char* const g_collision_shape_names[COLSHAPE_COUNT];

// A Crowd Spawner (crowd.h): its agents, their look and their AI. Toward the player: Wander (roam around the spawner),
// Seek (charge: the flow field to where the player will be), Surround (the aggressive share charges, the rest closes a
// ring), Flee (away along the paths)
enum { CROWD_WANDER = 0, CROWD_SEEK, CROWD_SURROUND, CROWD_FLEE, CROWD_BEHAVIOR_COUNT };
enum { CROWD_HIVE = 0, CROWD_INDIVIDUAL, CROWD_MIND_COUNT };
typedef struct CCrowd {
    uint32_t count;          // agents
    uint32_t mesh;           // the Skeletal Mesh (its hash)
    uint32_t anim[3];        // idle, walk, run (sequences' hashes; 0: the Mannequin's)
    float radius;            // cm: where they start, and roam (around the actor)
    float walk_speed, run_speed; // cm/s (the walk and run clips' own speeds too)
    uint32_t behavior;       // CROWD_ (toward the player)
    uint32_t mind;           // CROWD_HIVE / CROWD_INDIVIDUAL
    float sight;             // cm: seeing the player (Hive: one sees, all know)
    float memory;            // s: an Individual goes on to where it last saw the player
    float prediction;        // s: how far ahead the player is aimed at (0: where it is; more: cut it off)
    float reaction;          // s: between decisions (less: harder)
    float aggression;        // 0-1: the share that charges (Surround: the rest holds the ring)
    float ring_radius;       // cm: Surround's ring
    float separation;        // cm: personal space
    float variety;           // 0-1: speeds and sizes vary
    uint32_t seed;
    // (Q27.9, GTA 6's NPCs that never repeat) how varied their looks are: each agent its skin tone, its top's and its
    // bottom's colors from palettes and its build (wider or slimmer), by the seed; 0 (old levels): all alike
    float looks;
    uint32_t routine;        // (Q27.10) 1: City Routine: they go to the open Smart Objects (shops, kiosks) and leave when they close
} CCrowd;

// A Landscape (landscape.h): its grid, how it was made, its plants. The foliage fields stay together (their hash)
typedef struct CLandscape {
    char name[32];           // its files: Content/Landscapes/<name>.rland and <name>_<chunk>.rmesh
    uint32_t res;            // quads per side (32-1024, a multiple of chunks)
    uint32_t chunks;         // chunk meshes per side (1-16)
    float size;              // cm per side
    float height;            // cm: the noise's range
    float roughness;         // 0-1: hills to crags
    uint32_t seed;
    uint32_t material;       // 0: the color
    float color[3];
    // (until 0.14 the landscape scattered its own plants: these settings become a Foliage actor's first type when an
    // older level opens, foliage_moved then set; the plants are painted in the Foliage mode since)
    uint32_t foliage_mesh;   // a BasicShape or an imported mesh
    float foliage_density;   // plants per 100 m² (0: none)
    float foliage_scale[2];  // the smallest and the largest
    float foliage_slope;     // degrees: none steeper
    float wind;              // 0-1: the sway
    float foliage_color[3];
    uint32_t foliage_material;
    uint32_t foliage_moved;  // its plants are a Foliage actor's (0: an older level's, moved when it opens)
} CLandscape;

// Unreal's Foliage: the level's painted instances (Content/Foliage/<name>.rfol, foliage.c), drawn as its child entities
// (CAttach kind ATTACH_FOLIAGE), of up to FOLIAGE_TYPES kinds of plant
#define FOLIAGE_TYPES 4
typedef struct FoliageType {
    uint32_t mesh, material; // a BasicShape or an imported mesh; a material's hash (0: the color)
    float color[3];
    float density;           // instances per 100 m² (the brush's and Fill's)
    float scale[2];          // the smallest and the largest (uniform)
    float stretch;           // the height over the width (1: as the mesh is; a cone 3: a slender plant)
    float slope;             // degrees: none on a steeper ground
    float wind;              // 0-1: the sway (mesh.vert)
    uint32_t paint;          // the brush paints it (and erases it)
} FoliageType;
typedef struct CFoliage {
    char name[32];           // its file: Content/Foliage/<name>.rfol
    uint32_t ntypes;
    FoliageType types[FOLIAGE_TYPES];
    uint32_t seed;
    uint32_t impostors; // (Q21) far away its plants drawn by their impostors (farview.h). Grown at the end: old levels on
} CFoliage;

// A prototyping shape (proto.h): what it is and its numbers (cm); built is the hash it was built with
enum { PROTO_BOX = 0, PROTO_STAIRS, PROTO_RAMP, PROTO_WALL, PROTO_ROOM, PROTO_CORRIDOR, PROTO_PLATFORM, PROTO_KIND_COUNT };
#define PROTO_CEILING 1u
// (2026-11-10, "opções de mais portas") a Room's doors besides its front one (the same size: CProto.opening), and the
// doors along a Wall / a Corridor's front wall: their count - 1 in bits 8..10 (1 to 8, spread evenly)
#define PROTO_DOOR_BACK 2u
#define PROTO_DOOR_LEFT 4u
#define PROTO_DOOR_RIGHT 8u
#define PROTO_DOOR_FRONT 16u // (a Corridor's front wall: its doors; a Room's front door is always there)
#define PROTO_DOORS_SHIFT 8u
#define PROTO_DOORS(flags) ((((flags) >> PROTO_DOORS_SHIFT) & 7u) + 1u)
// (2026-11-10, "um array para definir quantas portas") each wall's own count - 1: the back, left and right walls
#define PROTO_DOORS_BACK_SHIFT 11u
#define PROTO_DOORS_LEFT_SHIFT 14u
#define PROTO_DOORS_RIGHT_SHIFT 17u
#define PROTO_DOORS_AT(flags, shift) ((((flags) >> (shift)) & 7u) + 1u)
#define PROTO_MAX_POINTS 24
typedef struct CProto {
    uint32_t kind;
    float size[3];           // length (x), width (y), height (z)
    float thickness;         // walls, floors, slabs
    uint32_t steps;          // stairs
    float opening[3];        // a wall's (a room's front wall's) door: width (0: none), height, its middle's offset along it
    uint32_t flags;          // PROTO_CEILING
    uint32_t material;       // 0: the grid
    float color[3];
    uint32_t built;
    // (2026-11-10, "os manipuladores do spline do rio devem estar nas partes de prototipagem") a spline as the river's:
    // 2 or more points (the actor's space, cm; dragged in the viewport as the river's) and the Wall, Corridor, Box, Ramp
    // and Platform follow it (a smooth curve through them) instead of their length; 0: the shape as before
    uint32_t npoints;
    float points[PROTO_MAX_POINTS][3];
} CProto;

// Unreal's AI Controller + AI Perception, as a component of the pawn it drives
enum { AI_MODE_NONE = 0, AI_MODE_TREE, AI_MODE_BRAIN };
typedef struct CAIAgent {
    uint32_t mode;           // AI_MODE_
    uint32_t asset;          // the Behavior Tree (.rbt) or the Brain (.rbrain): its vpath's hash
    float sight_radius;      // cm
    float sight_angle;       // degrees, the whole cone
    float hearing_radius;    // cm (a noise of loudness 1)
    float lose_sight_time;   // s: how long it remembers a target it can't see
    uint32_t team;           // 0: none (it ignores the others); agents of other teams (and the player) are targets
    uint32_t inst;           // (the run's: its instance + 1)
    // the difficulty and the teamwork (notas-dev/ia.md)
    float prediction;        // s: how far ahead it aims at a moving target (the intercept: the shortcut to where it will be)
    float reaction;          // s: seeing before it reacts (less: harder)
    float coordination;      // 0-1: the share of a squad that goes round to cut off (the rest charges straight)
    float share_radius;      // cm: what it sees, its team within this knows (0: keeps it)
    float surround_radius;   // cm: the squad's ring around the target
} CAIAgent;

// Unreal's Niagara Component (particles.h): the Particle System asset played at the actor
typedef struct CParticleSystem {
    uint32_t asset;          // the .rparticle's hash
    uint8_t auto_activate;   // starts with the level / when spawned (Unreal's Auto Activate)
    uint8_t active;          // (the run's) playing; Activate / Deactivate set it
    uint8_t auto_destroy;    // spawned by Spawn System: the actor goes when its particles are done
    uint8_t pad;
    float time_scale;        // its clock (1)
    float spawn_scale;       // the spawn rates times this (1)
    float size_scale;        // the sizes times this (1)
    float color[4];          // a tint (Set Color Parameter): the colors times it (1, 1, 1, 1)
    uint32_t seed;           // added to the emitters' (0)
} CParticleSystem;

// The Tlender's (Blender's) particle system on a mesh actor (Particle Properties): count particles born over
// [frame_start, frame_end] from its faces, vertices or volume, with their velocity along the normal and at random,
// Newtonian physics; rendered as halos (glowing or lit sprites) or an object (a mesh instanced at each one)
typedef struct CParticleTl {
    uint32_t count, seed;
    float frame_start, frame_end, fps;   // frames (Blender's 1, 200), the frames per second (24)
    float lifetime, lifetime_random;     // frames (50), 0..1
    uint8_t emit_from, render_as, physics, loop; // PTL_FROM_*, PTL_RENDER_*, PTL_PHYS_*, born again after the end
    float normal_factor, random_factor;  // m/s along the normal, at random (Blender's Velocity: 1, 0)
    float object_velocity[3];            // m/s in the actor's space (Blender's Object Align)
    float gravity, drag, brownian;       // the gravity's weight (1), drag (Blender's 0..1), brownian (m/s^2)
    float size, size_random;             // m (0.05), 0..1
    uint32_t mesh, material;             // Render As Object: the mesh (0: a cube); a halo's texture (a material)
    float color[3];
    float emissive;                      // a halo's glow (nits; 0: lit by the sun and the sky)
    float rotation_random, angular_velocity; // deg, deg/s
    uint8_t additive, receive_shadows, local, pad2;
} CParticleTl;

// A vehicle (vehicle.h): its Vehicle asset (.rvehicle) simulates the actor; its parts move the actor's components by
// name (wheels, doors, pistons, propellers). Driven by the player (Enter Vehicle / Auto Possess) or by Blueprint inputs
typedef struct CVehicle {
    uint32_t asset;          // the Vehicle asset (its vpath's hash)
    uint32_t auto_possess;   // the player drives it when play starts
    uint32_t start_engine;   // its engine on from the start (else Start Engine / Enter Vehicle)
    char rail[32];           // a train: the Rail actor it runs on (its name)
    char couple_to[32];      // a train car: the vehicle it follows on the rail (its name)
    float rail_start;        // where on the rail it starts (m from the first point)
    float couple_gap;        // m between it and the one it follows
    uint32_t inst;           // (the run's: its state + 1)
} CVehicle;

// Unreal's Water Body (simpler): a flat surface at the actor's height over its size (a plane's 100 cm times the
// scale), with waves; boats and floating parts float on it
typedef struct CWater {
    float wave_height;       // cm (crest to trough / 2)
    float wave_length;       // cm
    float wave_speed;        // cm/s
    float current[2];        // cm/s (x, y)
    float density;           // kg/m3 (1000 water)
    float depth;             // cm below the surface (the bottom: nothing floats deeper)
    // 2026-11-08 (water.h): the surface drawn as water (no more a plane); Ocean: FFT waves from a wind's spectrum,
    // endless, foam where they break, ripples where things move in it
    uint32_t ocean;          // 0: a lake / pool (the waves above), 1: an ocean (the spectrum; endless)
    float wind_speed;        // m/s (the ocean's waves grow with it: 5 calm, 10 fresh, 20 a gale)
    float wind_dir;          // degrees (where the wind blows to; 0: +X)
    float choppiness;        // 0..2: the crests pulled sideways (sharp, choppy waves)
    float foam;              // 0..1: the white water where the waves fold
    float color[3];          // the deep water's color (linear)
    float scatter[3];        // the light through the crests (linear)
    float clarity;           // cm: how far light goes in it (clear 2000, murky 200)
    float interaction;       // 0..1: the ripples and wakes of what moves in it
    float swell;             // 0..1: long waves from far away (the fetch's share)
    float shore_foam;        // 0..1: white water where it is shallow (shores, around what stands in it) (2026-11-08)
    // (2026-11-10, "colocar áreas que a água não pode sobrepor, tipo áreas de terra") Unreal's Water Body Exclusion
    // Volume: on, this body is no water but a hole in the ocean, its rectangle (scale x 100 cm, turned by its yaw): no
    // surface drawn there, no waves felt (boats, buoyancy, swimming, the camera under water)
    uint32_t exclusion;
} CWater;
// Cloth (cloth.h, notas-dev/tecido.md): a sheet of res_x x res_y vertices hanging in the actor's XZ plane from its top
// edge (or pinned otherwise), simulated on the GPU (Vertex Block Descent); it collides with Skeletal Meshes (their bones),
// characters, physics bodies and the level's meshes, and tears past its Tear Strain
typedef enum ClothPreset { CLOTH_CUSTOM = 0, CLOTH_SILK, CLOTH_COTTON, CLOTH_DENIM, CLOTH_LEATHER, CLOTH_CANVAS, CLOTH_FLAG, CLOTH_CAPE, CLOTH_SHIRT, CLOTH_SKIRT, CLOTH_PRESET_COUNT } ClothPreset; // (Q10: Shirt, Skirt - garments worn)
typedef enum ClothPin { CLOTH_PIN_TOP = 0, CLOTH_PIN_TOP_CORNERS, CLOTH_PIN_LEFT, CLOTH_PIN_CORNERS, CLOTH_PIN_NONE, CLOTH_PIN_COUNT } ClothPin;
typedef struct CCloth {
    float width, height;     // cm
    uint32_t res_x, res_y;   // vertices across, down (2..128)
    uint32_t preset;         // ClothPreset (set: its numbers below)
    uint32_t pin;            // ClothPin
    float density;           // g/m2 (silk 80, cotton 150, denim 400)
    float stretch, shear, bend; // N/m: the springs along, across the diagonal, over two cells (bending)
    float damping;           // 0..1
    float friction;          // 0..1 against what it touches
    float thickness;         // cm: its offset from what it touches (never closer: no clipping)
    float wind[3];           // m/s
    float gust;              // 0..1
    float gravity;           // its scale
    float tear;              // tears when a thread stretches past this (1.6: 160%); 0: never
    float color[3];          // linear
    float roughness, sheen, translucency;
    uint32_t iterations, substeps;
    char follow_actor[32];   // follow a Skeletal Mesh actor's bone (a cape): its name and the bone's (empty: stays)
    char follow_bone[32];
    float follow_offset[3];  // cm in the bone's space
    uint32_t reset;          // (set by the editor: start over from the rest pose)
    uint32_t cast_shadow;    // its shadow: the sun's virtual shadow maps and the ray traced shadows (2026-11-08)
    uint32_t self_collision; // against itself and the other cloths that have it (folds don't pass through)
    float self_distance;     // cm the vertices keep apart (0: from its thickness and its grid)
    // A mesh of its own (2026-11-08, cloth_mesh.h): an imported mesh's id (0: the grid above), its scale (0: 1); the
    // vertices pinned (unless Pinned is None): those within pin_band cm of its top along pin_axis (its space; 0: +Z).
    // Following a bone (Follow Actor / Bone) it takes the bone's whole turn: a skirt on the pelvis, a sleeve on an arm
    uint32_t mesh;
    float mesh_scale[3];
    float pin_band;
    float pin_axis[3];
    uint32_t follow_turn; // following a bone: its whole turn (a part's mesh made a cloth: its own space); 0: its yaw (a cape)
    // Soft body (2026-11-18, Q9, softbody.h, notas-dev/soft-body.md): kind CLOTH_KIND_SOFT makes it a volume, not a sheet:
    // the actor's own Static Mesh embedded in a lattice of tetrahedra (6 a cube, the cells the mesh fills), simulated by the
    // same Vertex Block Descent on stable Neo-Hookean elasticity (volume kept by Poisson's ratio), the mesh drawn bent with
    // its own material. Shares with the sheet: friction, thickness, gravity, iterations, substeps, Follow Actor/Bone, Cast
    // Shadow, Self Collision (against the other soft bodies and the cloths), pin_band / pin_axis (the band held)
    uint32_t kind;            // ClothKind
    uint32_t soft_preset;     // SoftPreset (set: its numbers below)
    uint32_t soft_res;        // lattice cells along the mesh's longest side (2..32)
    float soft_density;       // kg/m3 (foam 35, rubber 1100, jelly 1050, flesh 1000)
    float youngs;             // kPa: Young's modulus, the stiffness (jelly 3, cushion 12, foam 30, flesh 20, rubber 600)
    float poisson;            // 0..0.49: how much it keeps its volume when squeezed (0.49 almost incompressible)
    float soft_damping;       // 0..1
    float plasticity;         // 0..1: how much of a dent stays (memory foam); held bodies only
    float recovery;           // s: a dent comes back in about this (0: never)
    float yield;              // a dent starts past this share of a cell pushed in (0..1)
    uint32_t attach;          // SoftAttach: what holds it
    float attach_falloff;     // cm past the band where the hold fades (a soft hold: belly fat on the pelvis)
    float attach_stiffness;   // N/m of the faded hold (a weight 1 vertex is pinned)
    uint32_t sleep;           // sleeps when still (its vertices barely move) or far from the camera
    float sleep_distance;     // m from the camera past which it sleeps (0: never by distance)
    float rigid_push;         // x the reaction its contacts give the physics bodies (1 physical, 0 none)
    uint32_t released;        // (Blueprint Set Soft Body Pinned false: the hold let go until set again)
} CCloth;
typedef enum ClothKind { CLOTH_KIND_SHEET = 0, CLOTH_KIND_SOFT, CLOTH_KIND_COUNT } ClothKind;
typedef enum SoftPreset { SOFT_CUSTOM = 0, SOFT_FOAM, SOFT_RUBBER, SOFT_JELLY, SOFT_CUSHION, SOFT_FLESH, SOFT_PRESET_COUNT } SoftPreset;
// Held: Free (nothing), Bottom (the band at its bottom along -Pin Axis: a cushion on its sofa, a mattress), Band (along
// +Pin Axis), Bone (Follow Actor/Bone: the vertices within Pin Band of the bone held, fading over Falloff: a belly),
// Painted (the mesh's vertex colors: red 1 held, 0 free, as painted in the Tlender)
typedef enum SoftAttach { SOFT_ATTACH_FREE = 0, SOFT_ATTACH_BOTTOM, SOFT_ATTACH_BAND, SOFT_ATTACH_BONE, SOFT_ATTACH_PAINTED, SOFT_ATTACH_COUNT } SoftAttach;
// Rain (water.h, notas-dev/agua.md): one actor makes it rain around the camera; the scene's surfaces get wet where the
// rain reaches them, water gathers in puddles, runs down slopes and, heavy enough, makes streams
typedef struct CRain {
    float intensity;         // mm/h: 1 drizzle, 5 light, 20 moderate, 50 heavy, 100 storm (0: dry)
    float wind_speed;        // m/s (tilts the drops)
    float wind_dir;          // degrees
    float drop_size;         // mm
    float drops;             // 0..1: how visible the falling drops are
    float splashes;          // 0..1
    float wetness;           // 0..1: how wet (dark and glossy) the surfaces get
    float wet_time, dry_time; // s to soak, s to dry once the rain stops or a roof covers
    float puddles;           // 0..1: how much the water shows where it gathers
    float water_gain;        // x: the water the rain leaves on the ground (1 real; more: puddles in a minute, not an hour)
    float flow;              // 0..1: how freely the water runs downhill (0: stays where it falls)
    float infiltration;      // mm/h the ground drinks
    float evaporation;       // mm/h lost to the air
    float micro_relief;      // cm: the small dips flat ground has (where puddles form on it)
    float streaks;           // 0..1: water running down walls and windows
    float ripples;           // 0..1: rings where the drops hit the water
    float area;              // m: the square around the camera where the water is simulated
    uint32_t resolution;     // its cells across (128..1024)
    float start_water;       // mm on the ground when it starts (an area already flooded)
    // snow (2026-11-08, as the Tlender's): flakes falling slowly and fluttering, a cover gathering on what faces up
    uint32_t snow;           // 0 rain, 1 snow
    float flake_size;        // mm
    float cover_time;        // s until the cover is full
    float cover_angle;       // degrees: steeper surfaces get no snow
    float melt_time;         // s for the cover to melt once it stops snowing (0: it stays)
    float start_cover;       // 0..1: the cover when the level starts (a place already snowed on)
    // (2026-11-10, "aumentar a quantidade e velocidade da chuva") the falling drops on their own: how many (x the count
    // the intensity gives) and how fast (x the terminal speed); the water on the ground still follows the intensity
    float drop_amount;       // 0..4
    float fall_speed;        // 0.25..3
    // (2026-11-14, "mais escolhas de design para a chuva", "sobre o som da chuva também ter opções", "objetos molhados secam
    // muito rápido") the look, the sound and how fast what moves gets wet; every field also by Blueprint (Set Rain
    // Property, water.h rain_props)
    uint32_t preset;         // RainPreset: the last one applied (0 Custom)
    float drop_color[3];     // the drops' and splashes' tint (1 1 1: clear water lit by the sky)
    float streak_length;     // x the streak a drop draws (0.25..4)
    float drop_width;        // x how thick a drop is drawn (0.25..4)
    float splash_size;       // x the crowns (0.25..4)
    float gusts;             // 0..1: the wind comes and goes (the drops sway; the water on the ground keeps the mean wind)
    float far_rain;          // 0..1: the rain seen from high up (a camera far over the ground still sees it reach it)
    float actor_wet_time;    // s for something moving (a Character, a vehicle) to soak through in the rain
    float actor_dry_time;    // s for it to dry under a roof or after the rain
    float sound_volume;      // 0..2
    uint32_t sound_style;    // RainSoundStyle: what the rain falls on (open ground, roof, tin roof, leaves, tent, far)
    uint32_t custom_sound;   // a Sound Wave / Cue / MetaSound in place of the engine's own (its path hash; 0: none)
    float muffle;            // 0..1: how muffled it sounds with a roof over the camera
    float thunder;           // 0..1: how often it thunders (1: about every 12 s)
    float thunder_volume;    // 0..2
    float lightning;         // 0..1: how bright the sky flashes with each thunder (0: none)
    // (2026-11-15, "o shading não tá realista, quero algo realmente realista"; "permitir o shading da chuva ser alterado,
    // para o dev escolher o estilo e poder modificar como quiser") how the wet world is shaded: a style (its defaults
    // and, Stylized, its forms) and every knob of it (water.glsl rain_shade, rain.vert/.frag)
    uint32_t shading_style;  // RainShadingStyle: Realistic, Cinematic, Stylized
    float darkening;         // 0..2: how much darker (and more saturated) porous surfaces get wet (1: physical)
    float film;              // 0..2: the water film's reflection over what is wet (a coat with its own normal)
    float film_roughness;    // 0.005..0.5: how sharp the film reflects
    float puddle_color[3];   // what the puddles' water lets through (its tint)
    float puddle_clarity;    // 0..1: how much of the ground the puddles show (0: dark mirrors)
    float puddle_edge;       // 0..1: how irregular the puddles' edges are
    float surface_splashes;  // 0..2: crowns and wet dots where the drops hit the surfaces
    float wall_drops;        // 0..2: drops on the walls and windows (staying, and running down leaving a trail)
    float wall_drop_size;    // cm
    float drop_light;        // 0..4: how much the lights (the sun behind, a street lamp) light the falling drops
    float drop_opacity;      // 0..2: how much the falling drops show (1: the physical coverage)
    float mist;              // 0..3: the haze of the rain in the air (1: as dense as the intensity makes it)
    // (2026-11-15, "águas na tela, a tela reagir igual uma câmera real gravando na chuva com água escorrendo pelo visor")
    // water on the camera's lens: off unless the dev turns it on (composite.frag)
    float lens_water;        // 0..2: how much water gathers on the lens out in the rain (0: none)
    float lens_drop_size;    // x the drops' size on the lens (0.3..3)
    // (2026-11-15, "dependendo do ângulo ... ter inércia, poder mexer a câmera rápido para tirar a chuva; tudo deve poder ser
    // controlado e ajustado pelo dev") how the drops on the lens behave (lens_water.c)
    float lens_gravity;      // 0..3: how fast they run down (by the camera's tilt: looking down they stay)
    float lens_stickiness;   // 0..3: how big a drop must grow to start running
    float lens_inertia;      // 0..3: how much the camera's moves and turns push them
    float lens_shake_off;    // 0..3: how easily a quick turn throws them off (0: never)
    float lens_airflow;      // 0..3: how much moving forward pushes them out to the edges
    float lens_dry_time;     // s for them to dry out of the rain
    float lens_trail;        // 0..2: how long their wet trails last
    float lens_blur;         // 0..2: how out of focus they are
    float lens_refraction;   // 0..2: how much they bend what is behind them
    // (2026-11-15, "quando algo molhado entrar em um lugar seco molhar o chão, onde o personagem toca o chão ou pelas
    // goteiras pingando") the marks wet things leave on dry ground
    float wet_marks;         // 0..2: how wet the footprints and the drips of wet things leave the floor (0: none)
    float wet_marks_dry_time; // s for a mark to dry
    float actor_drips;       // 0..2: how much a wet actor drips where it is dry (under a roof, after the rain)
    // (Q27.6, GTA 6's "sunny where you are while a storm hits nearby") the regional weather: storms over the world
    // (weather_field.h) in place of one intensity everywhere: the rain, its wind and the clouds where the camera is
    uint32_t regional;       // 0: the intensity everywhere; 1: the storms'
    float storminess;        // 0..1: how many storms come by (0: only those Add Storm puts)
    uint32_t weather_seed;
} CRain;
typedef enum RainShadingStyle { RAIN_SHADE_REALISTIC = 0, RAIN_SHADE_CINEMATIC, RAIN_SHADE_STYLIZED, RAIN_SHADE_COUNT } RainShadingStyle;
// A shading style's defaults (the shading fields only; Stylized also changes the forms in the shaders: water.glsl)
static inline void rain_shading_defaults(CRain* r, uint32_t style) {
    if (!r || style >= RAIN_SHADE_COUNT) return;
    // darkening, film, film roughness, puddle color rgb, clarity, edge, surface splashes, wall drops, wall drop size (cm),
    // drop light, drop opacity, mist
    static const float k[RAIN_SHADE_COUNT][14] = {
        { 1.0f, 1.0f, 0.05f, 0.82f, 0.86f, 0.84f, 0.55f, 0.65f, 1.0f, 1.0f, 0.8f, 1.0f, 1.0f, 1.0f },  // Realistic: as measured
        { 1.2f, 1.45f, 0.025f, 0.7f, 0.76f, 0.8f, 0.3f, 0.55f, 1.3f, 1.3f, 1.0f, 2.4f, 1.35f, 1.6f },  // Cinematic: the film's wet streets
        { 0.55f, 1.0f, 0.12f, 0.45f, 0.7f, 1.0f, 0.15f, 0.0f, 1.4f, 0.8f, 1.6f, 0.5f, 1.6f, 0.35f },   // Stylized: clean shapes, flat tones
    };
    const float* v = k[style];
    r->shading_style = style;
    r->darkening = v[0], r->film = v[1], r->film_roughness = v[2];
    r->puddle_color[0] = v[3], r->puddle_color[1] = v[4], r->puddle_color[2] = v[5];
    r->puddle_clarity = v[6], r->puddle_edge = v[7], r->surface_splashes = v[8], r->wall_drops = v[9], r->wall_drop_size = v[10];
    r->drop_light = v[11], r->drop_opacity = v[12], r->mist = v[13];
}
typedef enum RainPreset { RAIN_PRESET_CUSTOM = 0, RAIN_PRESET_DRIZZLE, RAIN_PRESET_LIGHT, RAIN_PRESET_MODERATE, RAIN_PRESET_HEAVY,
                          RAIN_PRESET_STORM, RAIN_PRESET_MONSOON, RAIN_PRESET_STYLIZED, RAIN_PRESET_COUNT } RainPreset;
typedef enum RainSoundStyle { RAIN_SOUND_OPEN = 0, RAIN_SOUND_ROOF, RAIN_SOUND_TIN, RAIN_SOUND_LEAVES, RAIN_SOUND_TENT, RAIN_SOUND_DISTANT,
                              RAIN_SOUND_COUNT } RainSoundStyle;

// A Rail (Unreal's Spline Component for a track): up to 64 points (the actor's space, cm) joined by a smooth curve;
// trains (CVehicle.rail) run on it. Its rails are drawn as components
// A Procedural World (procgen.h): what it makes, from its seed, around the player
typedef struct CProcWorld {
    uint32_t mode;            // PROCGEN_TERRAIN, PROCGEN_FACILITY
    uint32_t seed;
    float block;              // cm: the nearest columns' width (terrain)
    float step;               // cm: the heights rounded to it (100: Minecraft's blocks; 25: gentle terraces)
    float view_distance;      // cm (terrain)
    float height;             // cm: the mountains' height
    float water_level;        // cm above the actor
    float biome_scale;        // cm: about a biome's size
    float tree_density;       // trees per 1000 m2 of forest's share
    float structure_density;  // ruins and towers per km2
    float creature_density;   // creatures per 10 000 m2 near the player (a facility: its deep rooms' share)
    float rebase_distance;    // cm from the level's 0 before the world moves back (0: never)
    uint32_t creature_class;  // a Blueprint class spawned as the creatures (0: the Mannequin with BT_Chase)
    uint32_t structure_class; // a Blueprint class placed as the structures (0: ruins and towers of stone)
    uint32_t rooms;           // a facility's rooms
    float room_size;          // cm: a room's side
    float room_height;        // cm
    float loop_chance;        // 0..1: extra doors between neighbors (loops)
    uint32_t built;           // (the run's)
    // (Q15) the PCG component (pcg.h): an actor in mode PROCGEN_PCG runs a PCG Graph inside its box. Here and not a
    // component of its own: the masks are full (64). Older levels read zeros (the mode is never PCG for them)
    struct {
        uint32_t graph;       // the PCG Graph (its path's hash)
        uint32_t seed;
        float extent[3];      // cm: the box's half size (times the actor's scale)
        uint32_t flags;       // PCGC_ (pcg.h)
        uint32_t trigger;     // PCG_TRIGGER_
        float cell_size;      // cm: a partition cell (0: the World Partition's, else 128 m)
        float load_range;     // cm: the cells kept around the player (0: the World Partition's, else 256 m)
        float spawn_budget;   // ms a frame for the entities while playing (0: 1)
        uint32_t noverrides;  // the graph's parameters set on this actor
        char override_name[8][24];
        float override_value[8];
    } pcg;
} CProcWorld;

#define RAIL_MAX_POINTS 64
// (Q14) the Spline Component (spline.h, notas-dev/splines.md): the Rail grew into the engine's one curve. Its kind says
// what it is: a Rail (trains, its rails drawn), a Spline (Unreal's Spline Component: points with tangents, roll and scale;
// a Spline Mesh bent along it, instances along it) or a Cable (Unreal's Cable Component: a rope simulated by XPBD between
// two actors). COMP_SPLINE is COMP_RAIL (the 64 component ids are all taken). The fields after `built` grew at the end:
// a level saved before reads a Rail (ecs.c's upgrade), its other fields the defaults
#define SPLINE_MAX_POINTS RAIL_MAX_POINTS
#define COMP_SPLINE COMP_RAIL
typedef struct CRail {
    float points[RAIL_MAX_POINTS][3];
    uint32_t npoints;
    uint32_t closed;         // a loop
    float gauge;             // cm between the rails
    uint32_t built;          // (its rails' / meshes' version: rebuilt when the points change)
    // --- (Q14) the Spline Component ---
    uint32_t kind;           // SplineKind (spline.h): SPLINE_KIND_RAIL, _SPLINE, _CABLE
    uint32_t flags;          // SPLINE_F_ (spline.h): drawn in the game, the mesh's collision...
    float arrive[RAIL_MAX_POINTS][3], leave[RAIL_MAX_POINTS][3]; // a Curve Custom Tangent point's tangents (cm, the actor's space)
    float roll[RAIL_MAX_POINTS];      // degrees about the curve
    float pscale[RAIL_MAX_POINTS][2]; // a point's scale across the curve (the Spline Mesh's width and height)
    uint8_t ptype[RAIL_MAX_POINTS];   // SplinePointType: Curve (auto tangents), Curve Custom Tangent, Linear, Constant
    // the Spline Mesh: a mesh bent along each segment (or each Mesh Length of the curve) on the GPU (skin.comp)
    uint32_t mesh_on;
    uint32_t mesh;           // BasicShape or an imported mesh's id
    uint32_t mesh_material;  // 0: the color
    uint32_t mesh_axis;      // the mesh's forward axis: 0 X, 1 Y, 2 Z
    float mesh_length;       // cm of curve per mesh (0: one per segment)
    float mesh_color[3], mesh_roughness, mesh_metallic;
    // instances along the curve (fence posts, lamps, trees by a road)
    uint32_t inst_on;
    uint32_t inst_mesh, inst_material;
    float inst_spacing;      // cm between them
    float inst_start;        // cm from the start before the first
    uint32_t inst_seed;
    float inst_side;         // cm across the curve (+: its right)
    float inst_jitter;       // cm of random offset across
    float inst_yaw_jitter;   // degrees of random turn
    float inst_scale_min, inst_scale_max;
    uint32_t inst_align;     // SPLINE_ALIGN_: upright (yaw along), full (the curve's frame), none (the actor's)
    float inst_color[3];
    float inst_scale3[3];    // the instance's own scale (times the random Scale Min..Max): a thin, tall post from a cylinder
    float inst_up;           // cm up the curve's frame (a post's half height: standing on the curve, not half buried)
    // the Cable: its ends (an actor by name, and a socket or bone of its Skeletal Mesh; none: this actor / a free end)
    char cable_start_actor[32], cable_start_socket[32], cable_end_actor[32], cable_end_socket[32];
    float cable_start_offset[3], cable_end_offset[3]; // cm in the end's space (a free end: from this actor)
    uint32_t cable_end_free; // the end hangs free (Unreal's Attach End off)
    float cable_length;      // cm (0: the ends' distance at the start)
    uint32_t cable_segments; // particles - 1 (Unreal's Num Segments)
    float cable_stiffness;   // 0..1: how little it stretches (the XPBD distance compliance)
    float cable_bend;        // 0..1: how much it resists bending (a wire stiff, a rope soft)
    uint32_t cable_iterations; // solver iterations per substep (Unreal's Solver Iterations)
    uint32_t cable_substeps;
    float cable_damping;     // 0..1 per second
    float cable_gravity;     // scale of the gravity
    float cable_width;       // cm
    uint32_t cable_render;   // CABLE_DRAW_TUBE, _RIBBON, _HIDDEN
    uint32_t cable_collide;  // SPLINE_COLLIDE_WORLD | _BODIES (the landscape, the level's boxes, rigid bodies, Characters)
    float cable_friction;    // 0..1
    float cable_wind;        // m/s of wind on it
    float cable_wind_dir;    // degrees (where it blows to; 0: +X)
    float cable_gusts;       // 0..1
    uint32_t cable_material;
    float cable_color[3], cable_roughness;
    // (Q27.2) a road (road_lanes > 0): the traffic's cars drive it (traffic.h). Old levels read 0: not a road
    uint32_t road_lanes;     // lanes each way (0: not a road)
    uint32_t road_oneway;    // 1: one way, along its points
    float road_speed;        // the speed limit, km/h (0: 50)
    float road_density;      // cars per 100 m of lane at the start (0: 4)
    float road_lane_width;   // cm (0: 350)
    float road_signal;       // s of green per approach at its junctions of three roads or more (0: none)
} CRail;
typedef CRail CSpline;

typedef struct ComponentInfo {
    const char* name;  // the Details panel's header
    uint32_t size;
    uint32_t user_addable; // offered by Details > Add
} ComponentInfo;

extern const ComponentInfo g_components[COMP_COUNT];

// Unreal's Exponential Height Fog: the fog's density falls exponentially with the height above the actor. Volumetric:
// lit per froxel by the sun (with its shadow: shafts of light), the sky and the local lights (weather.c). Every field
// is what a Blueprint may set (the fog changes the same frame).
typedef struct CFog {
    float density;          // extinction at the actor's height (1/m): 0.002 haze, 0.02 fog, 0.08 thick fog
    float height_falloff;   // 1/m: the density divides by e every 1/falloff meters up
    float albedo[3];        // the scattered color (linear)
    float anisotropy;       // -0.9..0.9: > 0 glows toward the sun (Henyey-Greenstein g)
    float start_distance;   // cm: no fog nearer than this
    float max_opacity;      // 0..1
    float emissive[3];      // its own light (nits), e.g. a polluted night sky
    uint32_t volumetric;    // lit per froxel with shadows (else the analytic fog only)
    float volumetric_distance; // cm: the froxels' reach (beyond: analytic)
    // (Q21) Unreal's Sky Atmosphere lighting the height fog: the far fog's ambient light is the sky's color over the
    // horizon ahead (0: its own gray from the sky light; 1: the sky's). Grown at the end: old levels read 1
    float sky_atmosphere_ambient;
} CFog;

// Unreal's Volumetric Cloud: a layer of clouds between two altitudes, moved by the wind, lit by the sun and the sky,
// casting their shadows on the ground
typedef struct CCloud {
    float coverage;         // 0..1: clear sky .. overcast
    float density;          // extinction inside the clouds (1/m)
    float bottom, top;      // the layer's altitudes (cm above the ground)
    float wind[2];          // m/s along x and y
    float albedo[3];        // linear
    float detail;           // 0..1: the edges eroded (wispy) .. round (cumulus)
    float shadow_strength;  // 0..1: their shadows on the ground
    float cirrus;           // 0..1: the high ice layer (~9 km, streaks along the wind); grown at the end: old levels 0
} CCloud;

// An instance of a Blueprint Class: the asset (the hash of its path, blueprint.c) and, while playing, its instance
// An Instance Editable variable's value on this actor (Unreal's per-instance value): the variable by the hash of its name
// (bp_name_hash), its value (one register) or its text. Grown at the end: levels before it read none
#define BP_MAX_OVERRIDES 12
typedef struct CBpOverride {
    uint32_t var;          // bp_name_hash of the variable's name (0: unused)
    uint32_t pad;
    float v[4];
    char text[56];         // a String / Name / Text variable's value
} CBpOverride;
enum { BP_FLAG_TICK_WHEN_PAUSED = 1u }; // the actor ticks while the game is paused (Unreal's Tick Even When Paused)
typedef struct CBlueprint {
    uint32_t asset_hash;
    uint32_t inst;         // while playing: the VM's instance + 1 (0: none)
    uint32_t noverrides, flags; // flags: BP_FLAG_* (zero in the levels from before)
    CBpOverride ov[BP_MAX_OVERRIDES]; // the Instance Editable values set on this actor (Details)
} CBlueprint;

// Unreal's Physics Constraint (physics.c, notas-dev/fisica.md "Physics Constraint"): the joint is at this actor's location,
// its X axis the hinge's / the slide's. Actor 2 none: Actor 1 is held to the world. Grown at the end: zero = old levels
// (Q8) Cone: a ball joint held inside a cone (Swing 1 x Swing 2) with a twist limit (ragdolls' shoulders, a chain);
// 6DOF: Unreal's Physics Constraint whole, each axis Free / Limited / Locked, soft limits, drives
typedef enum ConstraintType { CONSTRAINT_FIXED = 0, CONSTRAINT_HINGE, CONSTRAINT_BALL, CONSTRAINT_PRISMATIC, CONSTRAINT_CONE, CONSTRAINT_6DOF,
                              CONSTRAINT_TYPE_COUNT } ConstraintType;
enum { CMOTION_FREE = 0, CMOTION_LIMITED, CMOTION_LOCKED, CMOTION_COUNT }; // an axis of a 6DOF (Unreal's Free, Limited, Locked)
enum { CDRIVE_POSITION = 1, CDRIVE_VELOCITY = 2 };                         // a drive's bits (toward a pose, a speed)
typedef struct CConstraint {
    uint32_t actor1, actor2;    // the bodies (Simulate Physics; Entity handles); actor2 0: the world
    uint32_t type;              // ConstraintType
    float limit;                // a hinge: +- degrees around X; a slide: +- cm along X (0: free)
    uint32_t motor;             // drives around / along X
    float motor_speed;          // deg/s (hinge) or cm/s (slide)
    float motor_strength;       // the most it pushes, kg cm/s2 (0: as strong as needed)
    float break_force;          // kg cm/s2 that breaks it (0: never)
    uint32_t keep_collision;    // the two bodies still collide (Unreal's Disable Collision off)
    // the run's (zero in the level)
    uint32_t started, broken;
    float la[3], lb[3];         // the anchor in body 1's space, in body 2's (or the world point)
    float axa[3], axb[3];       // X in body 1's space, in body 2's (or the world)
    float q0[4];                // the bodies' relative rotation when it started
    // (Q8, notas-dev/fisica.md "Juntas"; a level from before reads these as 0: its four types as they were)
    uint32_t lin_motion[3];     // 6DOF: X, Y, Z of the constraint's frame (CMOTION_)
    uint32_t ang_motion[3];     // 6DOF: Twist (around X), Swing 1 (around Z), Swing 2 (around Y)
    float lin_limit;            // cm: the limited linear axes' +- range (6DOF)
    float twist_limit, swing1_limit, swing2_limit; // degrees (+-): the twist; the cone's two half angles (Cone, 6DOF)
    uint32_t soft_limits;       // bit 0 linear, bit 1 angular: the limits are springs (Unreal's Soft Constraint)
    float limit_hz, limit_damping;  // the soft limits' spring: frequency (Hz) and damping ratio (1: no bounce)
    uint32_t lin_drive, ang_drive;  // CDRIVE_ bits (Unreal's Linear / Angular Drive; the angular one a SLERP drive)
    float lin_target[3], lin_target_vel[3]; // cm, cm/s in the frame
    float ang_target[3], ang_target_vel[3]; // degrees (pitch, yaw, roll as a rotator), deg/s
    float drive_hz, drive_damping;          // the drives' spring (mass-free: Unreal's Acceleration mode); 0 Hz: rigid
    float lin_drive_max, ang_drive_max;     // the most a drive pushes: kg cm/s2, kg cm2/s2 (0: no limit)
    float break_torque;         // kg cm2/s2 that breaks it (0: never; Unreal's Angular Break Threshold)
    // the run's
    float fa[4], fb[4];         // the constraint's frame in body 1's / body 2's space (or the world's), quaternions
    float acc[12];              // its rows' impulses (the next step starts from them)
    float force, torque;        // the last step's pull (kg cm/s2) and twist (kg cm2/s2), the Details while playing
} CConstraint;

// Navigation (navmesh.h): Unreal's Nav Mesh Bounds Volume (where the NavMesh is made), Nav Link Proxy (two points the AI
// crosses: a gap it jumps, an edge it drops from) and Nav Modifier Volume (an area's cost, or none). A volume's box is its
// transform's (100 cm per unit of scale each way, as the Post Process Volume's)
typedef enum NavActorKind { NAV_BOUNDS_VOLUME = 0, NAV_LINK_PROXY, NAV_MODIFIER_VOLUME, NAV_KIND_COUNT } NavActorKind;
enum { NAV_LINK_BOTH_WAYS = 0, NAV_LINK_LEFT_TO_RIGHT, NAV_LINK_RIGHT_TO_LEFT };
typedef struct CNav {
    uint32_t kind;              // NavActorKind
    float left[3], right[3];    // a link's two points, in the actor's space (cm)
    uint32_t direction;         // a link: NAV_LINK_BOTH_WAYS, LEFT_TO_RIGHT, RIGHT_TO_LEFT
    float snap_radius;          // a link: how far its points reach for the NavMesh (cm)
    uint32_t area;              // a modifier: NAV_AREA_ (navmesh.h): Null, Default, Obstacle, Shallow Water
} CNav;

// Health (health.h, notas-dev/saude-dano.md): the Health Components of Unreal's games. The shield takes the damage first;
// each damage type (a Gameplay Tag, "Damage.Fire") is scaled by the most specific multiplier that matches it ("Damage"
// covers "Damage.Fire.Burning"); regeneration after a delay; death: only the event, the actor destroyed after a delay, or
// its Skeletal Mesh's ragdoll. 120 bytes: it fits a Blueprint class's defaults (BpAsset.comp_data) and the Details' undo
#define HEALTH_MAX_MULT 3
#define HEALTH_TAG_LEN 20
enum { HEALTH_DEATH_EVENT = 0, HEALTH_DEATH_DESTROY, HEALTH_DEATH_RAGDOLL, HEALTH_DEATH_COUNT };
#define HEALTH_INVINCIBLE 1u   // no damage (the events don't run either)
#define HEALTH_NUMBERS 2u      // floating damage numbers over the actor
#define HEALTH_SHOW_BAR 4u     // the player's pawn: WBP_HealthBar on the screen when play starts
#define HEALTH_DEAD 0x100u     // (the run's)
#define HEALTH_DESTROYING 0x200u // (the run's: its destroy is due)
typedef struct CHealthMult {
    char tag[HEALTH_TAG_LEN]; // a damage type ("Damage.Fire"; "": unused)
    float scale;              // 0: immune, 0.5: half, 2: double
} CHealthMult;
typedef struct CHealth {
    float max_health, health;   // health: the current (the level's: where it starts)
    float max_shield, shield;   // 0: no shield
    float regen_delay;          // seconds after the last damage before it regenerates
    float regen_rate;           // health a second (0: none)
    float shield_regen_rate;    // shield a second (0: none)
    float destroy_delay;        // seconds after death: Destroy (and Ragdoll when > 0) removes the actor
    uint32_t death;             // HEALTH_DEATH_*
    uint32_t flags;             // HEALTH_INVINCIBLE, HEALTH_NUMBERS, HEALTH_SHOW_BAR; the run's HEALTH_DEAD
    float since_damage;         // (the run's) seconds since the last damage (dead: since the death)
    uint32_t last_instigator;   // (the run's) the last damage's instigator (Entity)
    CHealthMult mult[HEALTH_MAX_MULT];
} CHealth;

// Gameplay Tags on an actor (gameplay_tags.h): up to GT_MAX_TAGS names, "Parent.Child"
#define GT_MAX_TAGS 16
#define GT_TAG_LEN 40
typedef struct CGameplayTags {
    uint32_t count, pad;
    char tags[GT_MAX_TAGS][GT_TAG_LEN];
} CGameplayTags;

// Unreal's Character Movement Component, simplified: a vertical capsule (radius, half height) with gravity, walking
// toward the input, jumping; the controller's yaw and pitch (the camera boom's) live here too
typedef struct CCharacter {
    float max_walk_speed;  // cm/s
    float jump_z_velocity; // cm/s
    float gravity_scale;
    float radius, half_height; // cm
    float velocity[3];
    float input[3];        // this frame's Add Movement Input (consumed by the movement)
    float control_yaw, control_pitch; // degrees
    float camera_distance; // cm behind (0: first person)
    uint32_t on_ground;
    uint32_t auto_possess; // the player takes it when play starts
    float anim_accel, anim_decel; // (the run's) its body's limits from Motion Matching's made starts and stops, cm/s2 (0: none)
    float desired[2];             // (the run's) the velocity asked this frame (the input times the speed): Motion Matching's trajectory
    float anim_yaw_rate;          // (the run's) while anim_turning: a Motion Matching clip turns it (degrees a second), not its velocity
    uint32_t anim_turning;
    uint32_t ground_box;          // (the run's) the collision box it stands on + 1 (0: none / the landscape): a platform carries it
    // (B12f, added: a level from before reads 0) Unreal's Swimming: in water over its chest it swims (no gravity, its head
    // kept above the surface, the current carrying it, Jump swims up); wading slower
    uint32_t swimming;            // (the run's) 1 while swimming
    float swim_speed;             // cm/s (0: 55% of Max Walk Speed)
    uint32_t jumps, jumps_seen;   // (B13, the run's) Jumps done, and as the tick last saw: a move's jump for the network's prediction
    // (Q12, added: a level from before reads 0) Unreal's Use Controller Rotation Yaw: the body faces the control yaw (a
    // first-person body seen from its own eyes), not its movement (Orient Rotation to Movement)
    uint32_t use_controller_yaw;
    // (the run's) a jump in a Motion Matching clip (its vertical root motion): while anim_vertical the clip's vertical
    // velocity (anim_vz, cm/s) carries the capsule instead of gravity; anim_jump_ok: its database has jumps, so Jump asks
    // the animation for one (jump_wait s, then the physical jump if none came)
    float anim_vz;
    uint16_t anim_vertical, anim_jump_ok;
    float jump_wait;
} CCharacter;
_Static_assert(sizeof(CCharacter) <= 128, "a Blueprint class keeps a component in 128 bytes (BpAsset.comp_data)");

// Inventory (inventory.h, notas-dev/inventario.md): what the actor carries. The component holds its settings (Details);
// the items themselves live in inventory.c while playing (handle), made from the starting items when play starts.
// Small enough for a Blueprint class's components (96 bytes)
#define INV_START_ITEMS 6
typedef struct CInventory {
    uint32_t slots;            // how many (1..64)
    float max_weight;          // kg it can carry (0: no limit)
    uint32_t auto_pickup;      // takes the Pickups it touches (theirs set to On Touch)
    float pickup_radius;       // cm: how near a Pickup counts as touched or in reach of the interact key
    uint32_t start_item[INV_START_ITEMS];  // Starting Items: Item Definitions' hashes (0: none)
    uint32_t start_count[INV_START_ITEMS];
    uint32_t interact_key;     // a GLFW key: the possessed player takes the nearest On Interact Pickup (0: none; 'E')
    uint32_t handle;           // (the run's) its contents in inventory.c + 1
} CInventory;

// A Pickup (inventory.h): an item in the world (its mesh: the actor's Static Mesh), taken by an actor with an Inventory
enum { PICKUP_ON_TOUCH = 0, PICKUP_ON_INTERACT };
typedef struct CPickup {
    uint32_t item;             // an Item Definition's hash
    uint32_t count;            // how many (0 read as 1)
    uint32_t mode;             // PICKUP_ON_TOUCH, PICKUP_ON_INTERACT
    float radius;              // cm (0: the taker's Pickup Radius)
    uint32_t spin;             // turns and bobs while it waits
    float respawn_time;        // seconds before it comes back once taken (0: gone for good)
    uint32_t ignore;           // (the run's) the actor that dropped it: not taken by it until it walks away
    float timer, base_z, phase; // (the run's) the respawn's countdown, its height and its bob
    uint32_t full;             // (the run's) its count when play started (what a respawn brings back)
} CPickup;

// Saveable (save_progress.h, notas-dev/salvar-progresso.md): what of this actor goes into the progress's saves. Its key
// in the files: id (given by the editor when the component is added) or, 0, its name's hash; two alike: the later one
// gets another, the same every time for the same level
#define SAVEABLE_TRANSFORM 1u      // its place (location, rotation, scale)
#define SAVEABLE_HEALTH 2u         // its Health (health, shield, maximums, dead)
#define SAVEABLE_INVENTORY 4u      // its Inventory and equipment
#define SAVEABLE_VARIABLES 8u      // its Blueprint variables marked SaveGame
#define SAVEABLE_ALL_VARIABLES 16u // every plain variable of its Blueprint (not only the SaveGame ones)
#define SAVEABLE_DESTROYED 32u     // destroyed stays destroyed (a chest opened, an enemy killed)
#define SAVEABLE_HIDDEN 64u        // its visibility (Set Actor Hidden In Game)
#define SAVEABLE_DEFAULT (SAVEABLE_TRANSFORM | SAVEABLE_HEALTH | SAVEABLE_INVENTORY | SAVEABLE_VARIABLES | SAVEABLE_DESTROYED)
typedef struct CSaveable {
    uint32_t id;     // its key (0: its name's hash)
    uint32_t flags;  // SAVEABLE_*
    uint32_t key;    // (the run's) the key in use
    uint32_t spawned; // (the run's) 1: spawned while playing (saved with its class, spawned again by a load)
} CSaveable;

// A Checkpoint (save_progress.h): a box volume (its extent times the actor's scale, turned with its yaw); the player's
// pawn walking in makes it the respawn point and, with Save, saves the progress into its slot
#define CHECKPOINT_SAVE 1u             // saves when reached
#define CHECKPOINT_ONCE 2u             // only the first time (else each time the player comes in again)
#define CHECKPOINT_RESPAWN_ON_DEATH 4u // the player's Health dying: back here after Respawn Delay (its save reloaded when it has one)
#define CHECKPOINT_DEFAULT (CHECKPOINT_SAVE | CHECKPOINT_ONCE | CHECKPOINT_RESPAWN_ON_DEATH)
typedef struct CCheckpoint {
    float extent[3];        // cm, half the box (times the actor's scale)
    float spawn_offset[3];  // where the player comes back, from the actor (cm, its axes; z: the feet)
    uint32_t flags;         // CHECKPOINT_*
    float respawn_delay;    // seconds after the death
    char slot[32];          // the save's slot ("": "Checkpoint")
    uint32_t reached;       // (the run's) the player came in
} CCheckpoint;
// Interaction (interaction.h, notas-dev/interacao.md; R7 módulo 5): an Interactable (what can be used: its prompt, key,
// hold, condition), the Interactor (who uses: the player's pawn gets one when play starts) and the ready-made Devices
// (Door, Chest, Lever, Button). Each at most 128 bytes: they fit a Blueprint class's defaults (BpAsset.comp_data)
#define IA_TEXT 32
#define IA_NAME 20
#define IA_TAG 28
#define IA_ACTION 20
#define IA_ENABLED 1u        // it can be focused and used (Set Interaction Enabled)
#define IA_ONCE 2u           // used once: then it no longer interacts
#define IA_CONSUME 4u        // the condition's item is spent (a key used up)
#define IA_REQ_INVENTORY 8u  // the condition: an item with the tag in the instigator's Inventory
#define IA_REQ_TAGS 16u      // the condition: the tag on the instigator (its Gameplay Tags)
#define IA_GRAB 32u          // (Q25, vr_pawn.h) Grabbable: a VR hand's grip takes it, it follows the hand, thrown with the hand's speed
#define IA_GRAB_SNAP 64u     // (Q25) held snapped to the hand's pose (as made: a gun by its grip), not where it was taken
#define IA_GRAB_SOCKET 128u  // (Q25) a Grab Socket: a grabbed actor released near it snaps into its place (Required Tag: what it takes)
#define IA_USED 0x100u       // (the run's) used (with IA_ONCE: no more)
#define IA_UNLOCKED 0x200u   // (the run's) the condition met once (a door unlocked stays so)
typedef struct CInteractable {
    char prompt[IA_TEXT];    // "Open {0}" ({0}: Name); "": the device's verb (Open, Close, Pull, Press) or "Interact"
    char name[IA_NAME];      // {0} in the prompt ("": the actor's name)
    char require[IA_TAG];    // a Gameplay Tag the instigator needs ("Item.Key.Red"; "": none)
    char action[IA_ACTION];  // an Enhanced Input action ("IA_Interact"; "": the key)
    uint16_t key;            // a GLFW key (0: the Interactor's)
    uint16_t flags;          // IA_*
    float range;             // cm (0: the Interactor's)
    float angle;             // the most degrees off the view it is focused at (0: the Interactor's)
    float hold_time;         // seconds the key is held (0: a press)
    int32_t priority;        // the higher wins when two are in view
    // (Q27.10) a Smart Object for the city's crowds (city.h): how many visit it at once (0: not one), its opening hours
    // in quarter hours (the clock's; both 0: always open; 22 to 6 wraps past midnight). Old levels read 0. (Packed: a
    // Blueprint class's defaults hold 128 bytes)
    uint8_t so_slots, open_q, close_q, so_pad;
} CInteractable;

#define IAR_OUTLINE 1u       // the focused actor outlined
#define IAR_GLOW 2u          // the focused actor glows
#define IAR_PROMPT 4u        // WBP_InteractPrompt on the screen (the player)
#define IAR_SIGHT 8u         // nothing focused through a wall (a trace to it)
typedef struct CInteractor {
    float range;             // cm from the pawn
    float angle;             // the view cone's half angle, degrees
    uint16_t key, flags;     // a GLFW key ('E'; a gamepad's X too when no action); IAR_*
    char action[IA_ACTION];  // an Enhanced Input action ("": the key)
    uint32_t focused;        // (the run's) the Interactable in focus (Entity)
    float hold;              // (the run's) seconds held on it
    float progress;          // (the run's) the hold, 0-1
    uint32_t held;           // (the run's) the key down last frame
    uint32_t hold_on;        // (the run's) what the hold started on
} CInteractor;

enum { DEV_DOOR = 0, DEV_CHEST, DEV_LEVER, DEV_BUTTON, DEV_KIND_COUNT };
enum { DEV_SWING = 0, DEV_SLIDE, DEV_TILT, DEV_MOTION_COUNT }; // about Z at an edge, along X, about Y at the back / bottom
#define DEV_HINGE_RIGHT 1u   // the hinge at the +X edge (else -X)
#define DEV_START_ACTIVE 2u  // starts open / on
#define DEV_AWAY 4u          // a door swings away from who opens it
#define DEV_STARTED 0x100u   // (the run's) its closed transform read
#define DEV_ACTIVE 0x200u    // (the run's) open / on
#define DEV_GIVEN 0x400u     // (the run's) a chest gave its items
#define DEV_ITEMS 3
typedef struct CDevice {
    uint32_t kind, motion, flags; // DEV_DOOR..., DEV_SWING..., DEV_* flags
    float amount;            // degrees (swing, tilt) or cm (slide)
    float time;              // seconds to open (or close)
    float auto_close;        // seconds open before it closes by itself (0: stays); a Button: how long it stays in
    uint32_t part;           // the actor that moves (Entity; 0: this one): a chest's lid
    uint32_t target;         // a Lever's / Button's: the actor it switches (Entity)
    char event[IA_ACTION];   // a Custom Event called on the target (its bool input: On) instead of the switch
    uint32_t item[DEV_ITEMS], count[DEV_ITEMS]; // a Chest's: the Item Definitions it gives (hashes) and how many
    float t;                 // (the run's) 0 closed .. 1 open
    float sign;              // (the run's) the swing's way this time
    float timer;             // (the run's) the auto close's
    float loc0[3], rot0[3];  // (the run's) the moving part's closed transform
} CDevice;
// A Dialogue Speaker (dialogue.h, notas-dev/dialogo.md): the NPC one talks to. Start Dialogue on it runs its Dialogue
// (or another by name) with these settings. Small enough for a Blueprint class's components (80 bytes)
#define DLG_SPK_CAMERA 1u   // the conversation camera: beside the player, looking at the speaker
#define DLG_SPK_FACE 2u     // the speaker turns to the player when it starts
enum { DLG_PAUSE_DIALOGUE = 0, DLG_PAUSE_YES, DLG_PAUSE_NO }; // the Dialogue's own setting, or always / never
typedef struct CDialogueSpeaker {
    uint32_t dialogue;         // the Dialogue asset's hash (0: none)
    char display_name[48];     // the name shown ("": the actor's)
    uint32_t flags;            // DLG_SPK_*
    uint32_t pause;            // DLG_PAUSE_*: the game pauses while it runs
    float cam_distance;        // cm from the speaker's face (0: 260)
    float cam_height;          // cm over the speaker's origin it looks at (0: 60, a Character's head over its capsule's middle)
    float cam_angle;           // degrees around the speaker from the player's side (0: 25, over the player's shoulder)
    uint32_t widget;           // a Widget Blueprint's hash (0: WBP_Dialogue)
} CDialogueSpeaker;
// A Quest Log (quest.h, notas-dev/missoes.md): the quests of its actor (the player's pawn, or a Game State actor). The
// quests themselves (active, completed, failed, the objectives' progress) live in quest.c while playing; this is what the
// editor sets. 28 bytes (a Blueprint class's components)
#define QL_TRACKER 1u        // WBP_QuestTracker on the screen: the tracked quest and its objectives
#define QL_MARKERS 2u        // the tracked quest's objectives marked over the view (and at its edge)
#define QL_AUTO_TRACK 4u     // a quest started while none is tracked becomes the tracked one
#define QL_START_QUESTS 4
typedef struct CQuestLog {
    uint32_t flags;          // QL_*
    uint32_t start[QL_START_QUESTS]; // Quests (their assets' hashes) started when play begins (0: none)
    uint32_t widget;         // a Widget Blueprint's hash (0: WBP_QuestTracker)
    float marker_distance;   // cm: the markers farther than this are hidden (0: always shown)
} CQuestLog;

// Game Rules (game_rules.h, notas-dev/regras-de-jogo.md; R7 módulo 8): the match's configuration (its state lives in
// game_rules.c). 128 bytes: it fits a Blueprint class's defaults (BpAsset.comp_data)
#define GR_TAG 24
#define GR_SCORE_TAGS 2
#define GR_AUTO_START 1u       // Waiting -> Countdown by itself after Wait Time (else: Start Match)
#define GR_WIN_SCORE 2u        // the Score Target reached wins (a player's; with teams, a team's)
#define GR_WIN_ELIMINATE 4u    // every actor with the Eliminate Tag dead wins
#define GR_LOSE_DEATHS 8u      // the player dead Max Deaths times loses
#define GR_LOSE_LIVES 16u      // out of lives loses (Lives above 0)
#define GR_TIMER_DOWN 32u      // the clock counts down from the Time Limit (else up)
#define GR_RESPAWN 64u         // the dead players come back after the Respawn Delay
#define GR_AT_CHECKPOINT 128u  // ...at the last Checkpoint (none reached: the Player Start)
#define GR_SHOW_HUD 256u       // WBP_MatchHUD on the screen while playing
#define GR_SHOW_END 512u       // WBP_MatchEnd at Game Over (the cursor shown)
#define GR_RESET_SCORE 1024u   // the scores back to 0 at each round
enum { RULES_TIME_UP_NOTHING = 0, RULES_TIME_UP_LOSE, RULES_TIME_UP_WIN, RULES_TIME_UP_SCORE, RULES_TIME_UP_COUNT };
typedef struct CGameRules {
    uint32_t flags;            // GR_*
    uint8_t time_up;           // RULES_TIME_UP_*: what the clock reaching its limit does
    uint8_t teams;             // 0: each for themself; 1: one team (co-op: the points add up); 2-4 teams
    uint8_t rounds;            // 1: a single round; N: best of N
    uint8_t pad;
    float time_limit;          // seconds (0: none, counting up)
    float wait_time, countdown, round_over_time; // the phases' seconds (Waiting with Auto Start; 3, 2, 1; the round's end)
    int32_t score_target;      // points (GR_WIN_SCORE)
    int32_t kill_points;       // a kill's points (the Health's On Death, its Killer)
    uint16_t lives, max_deaths; // lives (0: infinite); deaths that lose (GR_LOSE_DEATHS)
    float respawn_delay;       // seconds dead before the respawn
    float invincible_time;     // seconds invincible after it
    char eliminate_tag[GR_TAG]; // the Gameplay Tag of the enemies to kill (GR_WIN_ELIMINATE: "Enemy")
    char score_tag[GR_SCORE_TAGS][GR_TAG]; // a victim with this tag gives its own points instead ("Enemy.Boss")
    int32_t score_tag_points[GR_SCORE_TAGS];
    uint32_t reserved;
} CGameRules;

// An Ability System (ability.h, notas-dev/habilidades.md; R7 módulo 9): Unreal's Ability System Component, the light
// version. The component holds its settings (Details); the run's state (attributes, active effects, cooldowns) lives in
// ability.c (handle). Small enough for a Blueprint class's components (120 bytes)
#define ABIL_START_ABILITIES 6
#define ABIL_START_EFFECTS 4
#define ABIL_ATTR_TEXT 72
#define ABIL_KEYS 1u      // the possessed player's keys 1..6 activate the abilities in that order
#define ABIL_SHOW_BAR 2u  // the player's pawn: WBP_AbilityBar (and the active effects) on the screen when play starts
typedef struct CAbilitySystem {
    uint32_t ability[ABIL_START_ABILITIES]; // Starting Abilities: Gameplay Ability classes' hashes (0: none)
    uint32_t effect[ABIL_START_EFFECTS];    // Starting Effects: Gameplay Effects' hashes (a mana regeneration, a passive)
    char attributes[ABIL_ATTR_TEXT];        // Starting Attributes: "Mana 100, MaxMana 100, Stamina 50"
    uint32_t flags;                         // ABIL_KEYS, ABIL_SHOW_BAR
    uint32_t handle;                        // (the run's) its state in ability.c + 1
} CAbilitySystem;

// A HUD Layout (hud.h, notas-dev/hud.md): the game's HUD made of the HUD pieces. In play its widget (WBP_GameHUD unless
// another) is made and shown; the layout scales it by the resolution, keeps it in the safe area, hides it in the photo
// mode and while a Sequence has the camera, and shows only the pieces checked. 56 bytes (a Blueprint class's components)
enum { HUD_SCALE_HEIGHT = 0, HUD_SCALE_SHORT_SIDE, HUD_SCALE_FIT, HUD_SCALE_FIXED, HUD_SCALE_COUNT };
#define HUD_LAYOUT_VISIBLE 1u       // made and shown when play starts
#define HUD_LAYOUT_HIDE_PHOTO 2u    // hidden in the photo mode (Set Photo Mode)
#define HUD_LAYOUT_HIDE_SEQUENCE 4u // hidden while a Level Sequence's Camera Cuts have the view
#define HUD_LAYOUT_SAFE_AREA 8u     // inside the device's safe area (Android: the notch, the rounded corners)
#define HUD_LAYOUT_SPEED_SPREAD 16u // the crosshair opens with the player's speed
#define HUD_PIECE_BARS 1u
#define HUD_PIECE_COMPASS 2u
#define HUD_PIECE_MESSAGES 4u
#define HUD_PIECE_NUMBERS 8u
#define HUD_PIECE_CROSSHAIR 16u
#define HUD_PIECE_MARKERS 32u
#define HUD_PIECE_DAMAGE_DIR 64u
#define HUD_PIECE_MINIMAP 128u
#define HUD_PIECE_ALL 255u
typedef struct CHudLayout {
    uint32_t widget;          // a Widget Blueprint's hash (0: WBP_GameHUD)
    uint32_t flags;           // HUD_LAYOUT_*
    uint32_t pieces;          // HUD_PIECE_*: the kinds of piece shown (the others hidden)
    uint32_t scale_mode;      // HUD_SCALE_*: UMG's (the screen's height over 1080), the shortest side, fit 1920 x 1080, pixels
    float scale;              // times that
    float min_scale, max_scale; // the scale kept within (0: no limit)
    float safe[4];            // margins in % of the screen: left, top, right, bottom (the TV's overscan, a frame)
    int32_t zorder;           // its Add to Viewport's
    uint32_t widget_entity;   // (the run's) the widget made
} CHudLayout;

// Unreal's Decal Component (M6, decal.h): a material projected along the actor's X onto the surfaces inside its box
// (100 cm * scale each way, as the volumes); its baked base color and alpha, normal, roughness and metallic laid over theirs
#define DECAL_COLOR 1u   // affects the base color (and metallic)
#define DECAL_NORMAL 2u  // its normal map over the surface's
#define DECAL_ROUGH 4u   // its roughness
typedef struct CDecal {
    uint32_t material;   // its material (a hash, as a Static Mesh's; 0: none, nothing drawn)
    float opacity;       // 0-1 (a Blueprint fades it)
    float angle_fade;    // degrees: surfaces turned further than this from facing it fade out (Unreal's fade by angle)
    float fade_distance; // cm: it fades out from 80% to 100% of it from the camera (0: never)
    float life_span;     // seconds (0: forever): it fades out over its last fade_out and the actor goes (a bullet hole)
    float fade_out;      // seconds
    float age;           // (its time so far)
    uint32_t flags;      // DECAL_
    int32_t sort_order;  // the higher on top
} CDecal;

// Unreal's Groom Component (A9.3b, groom.c): hair in strands grown over the top of its actor's mesh (its bounds as an
// ellipsoid: a head), each a chain bent by gravity, curled, pulled into clumps; drawn as fibers (Marschner's lobes, the
// deep shadow of its density). Its numbers change the strands (made again); its look (melanin, redness, roughness) not
typedef enum GroomPreset { GROOM_CUSTOM = 0, GROOM_SHORT, GROOM_LONG, GROOM_WAVY, GROOM_CURLY, GROOM_PRESET_COUNT } GroomPreset;
typedef struct CGroom {
    uint32_t strands, segments, seed, preset; // strands (100..200000), points a strand - 1 (2..32), GroomPreset
    float length, length_var;  // cm; 0..1: how much shorter some are
    float root_width, tip_width; // mm (a real hair ~0.07; thicker draws fewer as more)
    float curl, curl_freq;     // the curl's radius (cm), its turns along a strand
    float gravity, clump;      // 0..1: how much it falls; how much the strands gather into locks
    float melanin, redness;    // Unreal's hair color: 0 white / blond .. 1 black; the red (pheomelanin) share
    float roughness;           // the fiber's (0.1..0.9: the highlights' width)
    float scalp;               // how far down the head it grows (-1 all of it .. 1 the crown only; the ellipsoid's z)
    float hairline;            // the forehead (the actor's +X: no roots in front past it, below the crown; 1: none cut)
    float dye[3];              // a tint over the fiber's color (white: none)
    float occlusion;           // how much the hair darkens its own ambient (0..1)
    uint32_t lod;              // 1: fewer strands far away (the rest wider), 0: always all
    // (2026-11-16) dynamic hair (groom.c, hair_sim.comp): simulated on the GPU, swinging with the head, colliding
    uint32_t dynamic;          // 1: simulated (falls, swings, the wind moves it); 0: as grown
    float stiffness;           // 0..1: how much it keeps its combed shape toward the tips (the roots always firm)
    float damping;             // 0..1: the air's drag
    float wind[3];             // cm/s^2 the wind pushes it with (world)
    float gusts;               // 0..1: how much the wind comes and goes
    uint32_t collide;          // 1: out of the head, the actor's Skeletal Mesh (its bones) and the scene's boxes
    float collision_radius;    // cm the strands keep from what they touch
    uint32_t guides;           // (2026-11-16) a Groom asset's guides (.rgroom: the Tlender's hair, OBJ curves): grown from them; 0: on the head's ellipsoid
    uint32_t visible;          // (2026-11-25, item 16) 0: not drawn nor simulated (Set Property "Visible"; a hidden actor's neither); a level from before reads 1
} CGroom;

// Fire (A14, fire.c): a box of gas (width x width x height cm, its base at the actor) simulated on the GPU: fuel at its
// base burnt into heat and smoke, rising by buoyancy, curling (vorticity), drawn by its temperature (a black body) and
// its smoke; the actor's Light (a point light, added with it) flickering with it
typedef struct CFire {
    uint32_t preset, resolution, iterations, enabled; // fire.c's presets; cells across (12..96); the pressure's Jacobi steps
    float width, height;       // cm: the grid's box
    float emitter_radius;      // cm: the burning base's
    float fuel, burn, heat;    // the fuel kept at the base (0: out), burnt a second, K a unit of it raises
    float buoyancy;            // m/s^2 per 1000 K over the air (heat rises)
    float vorticity;           // the curls kept alive (vorticity confinement)
    float cooling, smoke, dissipation; // 1/s; the smoke a unit of fuel leaves; the smoke's fading (1/s)
    float turbulence;          // cm/s of random push at the base (the flicker's shape)
    float brightness;          // nits of a 10 cm flame at 1500 K
    float smoke_density, smoke_albedo; // 1/cm of a unit of smoke; how much of the light it scatters (0 soot .. 1 steam)
    float light_intensity, light_radius, flicker; // the actor's light (cd), its reach (cm), how much it flickers (0..1)
} CFire;

// Unreal's Water Body River (B12c, river.c): on a Water Body (its look: color, clarity, foam), the river's spline in the
// actor's space, each point its width, depth and speed; the surface a ribbon along it, its waves dragged by the flow,
// what floats carried downstream
#define RIVER_MAX_POINTS 32
typedef struct CRiver {
    uint32_t npoints;
    float points[RIVER_MAX_POINTS][3]; // cm, the actor's space (the water's surface)
    float width[RIVER_MAX_POINTS];     // cm
    float depth[RIVER_MAX_POINTS];     // cm under the surface
    float speed[RIVER_MAX_POINTS];     // cm/s along the spline
} CRiver;

// Unreal's Buoyancy Component (B12f, physics.c): a Simulate Physics body floats in water (any Water Body: a lake, a river,
// the ocean): its bottom's corners and middle (pontoons) lifted by the water they are under, the current pulling it
typedef struct CBuoyancy {
    float buoyancy;      // the lift (1: as wood, about half under; 2: rides high; under 0.5 it sinks)
    float water_drag;    // 1/s: how fast the water takes its velocity (the current's pull, the water's brake)
    float angular_drag;  // 1/s: its spin slowed in water
    uint32_t pontoons;   // 1 (its middle) or 5 (the bottom's corners and middle)
} CBuoyancy;

// Unreal's Local Fog Volume (A9.5b, weather.c / weather.comp): fog inside the actor's box or sphere (100 cm per unit of
// scale each way), denser in the middle, thinning toward the edges and with height; added to the volumetric fog's
// froxels, so the sun, the sky and the lamps light it with their shadows (needs the volumetric fog: Medium and up)
typedef struct CFogVolume {
    uint32_t shape;          // 0: a sphere, 1: a box
    float density;           // extinction in the middle (1/m): 0.05 a haze, 0.5 a thick mist, 2 a cloud you can't see through
    float edge_falloff;      // 0..1: the share of its size over which it fades out at the edges
    float height_falloff;    // 0..: thinner upward inside it (1: e^-1 at its top); 0: even
    float albedo[3];         // the scattered color (linear)
    float emissive[3];       // its own light (nits)
} CFogVolume;

// Gaussian Splatting (A9.5d, splat.c): a scene scanned (a phone's capture trained into 3D Gaussians: 3DGS .ply, or
// .splat) shown where the actor is; its colors lit by the scene's sky and sun (as an albedo) or as their own light
typedef struct CSplat {
    char path[96];           // the file (.ply, .splat) under Content (Content/Splats/...), or a full path
    uint32_t up_axis;        // the file's up: 0 Z, 1 -Y (3DGS / COLMAP: most trained scenes), 2 Y
    float unit;              // cm per unit of the file (100: meters)
    uint32_t mode;           // 0 lit by the scene, 1 its own light (Brightness nits)
    float brightness;        // nits (mode 1)
    float opacity;           // a scale on the Gaussians' opacity (1)
} CSplat;

// Unreal's Heterogeneous Volume (A9.5a, volume.c, fire.c): an OpenVDB file (.vdb) or its sequence (the frame's digits:
// Blender's fire_0001.vdb) played where the actor is: the density as smoke, the temperature (or the flame) as the
// fire's light, drawn as the Fire actor's gas is
typedef struct CVolume {
    char path[80];           // the first file, under Content (Content/Volumes/...) or a full path
    uint32_t frames;         // the sequence's length (1: one file)
    uint32_t resolution;     // cells along its longest side (16..128)
    uint32_t up_axis;        // 0 Z (Blender, the Tlender), 1 Y (Houdini, EmberGen)
    uint32_t loop;           // 1: plays again from the start
    float fps;               // the sequence's speed
    float unit;              // cm per unit of the file (100: meters)
    float flame_temperature; // K a flame (or a 0..1 temperature) of 1 means
    float temperature_scale; // a temperature grid's values times this (1: kelvin)
    float density;           // extinction (1/m) at density 1
    float brightness;        // nits of 10 cm of flame at 1500 K
    float smoke_albedo;      // 0 soot .. 1 steam
} CVolume;

// World Partition (B1, world_partition.h; notas-dev/mundos-grandes.md). The level's: its grid, its HLOD and its Data
// Layers (each a name, its state when play starts - WP_LAYER_ -, hidden in the editor's viewport)
#define WP_MAX_LAYERS 16
typedef struct CWpLayer { char name[32]; uint32_t initial, editor_hidden; } CWpLayer;
typedef struct CWorldPartition {
    uint32_t enabled;      // the grid streams while playing
    float cell_size;       // cm (Unreal's 12800: 128 m)
    float loading_range;   // cm: the cells this near the streaming source are in (Unreal's 25600)
    float budget_ms;       // the streaming's main-thread time a frame (loads and unloads spread over the frames)
    uint32_t hlod;         // the HLOD past the range: proxies near, the regions' merged meshes far
    uint32_t hlod_region;  // cells a side of a merged HLOD mesh
    float hlod_keep;       // the fraction of the triangles a merged mesh keeps
    float hlod_min_size;   // cm: smaller meshes left out of the HLOD (pebbles, grass)
    uint32_t nlayers;
    CWpLayer layers[WP_MAX_LAYERS];
} CWorldPartition;
// An actor's (WP_SPATIAL_: Auto, Yes, No; layers: a bit per Data Layer of the level's World Partition)
typedef struct CPartition { uint32_t spatial, layers; } CPartition;
