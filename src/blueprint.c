// Blueprints: the assets, the node registry, graph editing and the files (the compiler and the VM: bp_vm.c)
#include "blueprint.h"
#include "bp_debug.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bp_internal.h"
#include "plugins.h"
#include "inventory.h"
#include "ability.h"
#include "bp_jit.h"
#include "ui_widget.h"
#include "anim.h"
#include "input.h"
#include "components.h"
#include "io_json.h"
#include "bp_timeline.h"
#include "platform.h"
#include "platform_files.h"

const char* const g_bp_type_names[BPT_COUNT] = { "Exec", "Boolean", "Integer", "Float", "Vector", "Rotator", "String", "Actor",
                                                  "Structure", "Enum", "Byte", "Integer64", "Name", "Text", "Linear Color",
                                                  "Transform", "Class Reference", "Object Reference", "Wildcard" };
const char* const g_bp_asset_ext[BPA_COUNT] = { "rbp", "rstruct", "renum", "rinterface", "rdata", "rmacros" };
const char* const g_bp_asset_names[BPA_COUNT] = { "Blueprint Class", "Structure", "Enumeration", "Blueprint Interface", "Data Asset",
                                                  "Blueprint Macro Library" };
const char* const g_bp_native_names[BPN_COUNT] = { "Actor", "Pawn", "Character", "Game Mode", "Player Controller", "AI Controller",
                                                   "Game State", "Player State", "HUD", "Game Instance", "Actor Component",
                                                   "Scene Component", "Save Game", "Anim Instance", "Level Script Actor",
                                                   "User Widget", "Gameplay Ability" };

// --- The node registry: name, category, tooltip, pure, and the pins ("i:" inputs, "o:" outputs, then the type and the
// name; "var" is the named variable's type, "field" the struct's fields, "entry" the enum's entries) ---
typedef struct NodeSpec {
    BpNodeDesc d;
    const char* pins;
} NodeSpec;

static const NodeSpec g_nodes[BPN_KIND_COUNT] = {
    [BPK_EVENT_BEGIN_PLAY] = { { "Event BeginPlay", "Events", "Runs once when play starts (or the actor is spawned).", false }, "o:exec:" },
    [BPK_EVENT_TICK] = { { "Event Tick", "Events", "Runs every frame. Delta Seconds: the frame's time.", false }, "o:exec:,o:float:Delta Seconds" },
    [BPK_EVENT_KEY] = { { "Key Event", "Input", "Runs when the key is pressed / released (the node's key).", false }, "o:exec:Pressed,o:exec:Released" },
    [BPK_EVENT_CUSTOM] = { { "Custom Event", "Events", "An event of your own, with inputs if you want (Details: Inputs): call it with \"Call <Event>\", here or on another actor.", false }, "o:exec:" },
    [BPK_EVENT_INTERFACE] = { { "Interface Event", "Interfaces", "Runs when an interface function is called on this actor.", false }, "o:exec:" },
    [BPK_CALL_CUSTOM] = { { "Call Custom Event", "Events", "Runs a Custom Event with its inputs: one of this Blueprint's, or another class's on the Target actor (Details: Target Class).", false }, "i:exec:,o:exec:" },
    [BPK_BRANCH] = { { "Branch", "Flow Control", "If: True or False (B + click in the graph).", false }, "i:exec:,i:bool:Condition,o:exec:True,o:exec:False" },
    [BPK_SEQUENCE] = { { "Sequence", "Flow Control", "Runs its outputs one after the other (Add pin: one more).", false }, "i:exec:,o:exec*2:Then " },
    [BPK_FOR_LOOP] = { { "For Loop", "Flow Control", "Loop Body for each index from First to Last, then Completed.", false },
                       "i:exec:,i:int:First Index,i:int:Last Index,o:exec:Loop Body,o:int:Index,o:exec:Completed" },
    [BPK_DELAY] = { { "Delay", "Flow Control", "Waits Duration seconds, then Completed (latent: the rest of the frame goes on).", false },
                    "i:exec:,i:float:Duration,o:exec:Completed" },
    [BPK_DO_ONCE] = { { "Do Once", "Flow Control", "Lets the execution through the first time only (until Reset).", false },
                      "i:exec:,o:exec:Completed,i:exec:Reset,i:bool:Start Closed" },
    [BPK_PRINT] = { { "Print String", "Utilities", "Shows a message on the screen (and the Output Log). Connect any value.", false },
                    "i:exec:,i:string:In String,o:exec:" },
    [BPK_VAR_GET] = { { "Get", "Variables", "A variable's value (drag it from My Blueprint).", true }, "o:var:" },
    [BPK_VAR_SET] = { { "Set", "Variables", "Changes a variable.", false }, "i:exec:,i:var:,o:exec:,o:var:" },
    [BPK_ADD] = { { "Add (Float)", "Math|Float", "A + B", true }, "i:float:A,i:float:B,o:float:" },
    [BPK_SUB] = { { "Subtract (Float)", "Math|Float", "A - B", true }, "i:float:A,i:float:B,o:float:" },
    [BPK_MUL] = { { "Multiply (Float)", "Math|Float", "A * B", true }, "i:float:A,i:float:B,o:float:" },
    [BPK_DIV] = { { "Divide (Float)", "Math|Float", "A / B (0 when B is 0)", true }, "i:float:A,i:float:B,o:float:" },
    [BPK_VADD] = { { "Add (Vector)", "Math|Vector", "A + B", true }, "i:vector:A,i:vector:B,o:vector:" },
    [BPK_VSUB] = { { "Subtract (Vector)", "Math|Vector", "A - B", true }, "i:vector:A,i:vector:B,o:vector:" },
    [BPK_VMULS] = { { "Multiply (Vector * Float)", "Math|Vector", "A * B", true }, "i:vector:A,i:float:B,o:vector:" },
    [BPK_DOT] = { { "Dot Product", "Math|Vector", "A . B", true }, "i:vector:A,i:vector:B,o:float:" },
    [BPK_VLEN] = { { "Vector Length", "Math|Vector", "|A|", true }, "i:vector:A,o:float:" },
    [BPK_NORMALIZE] = { { "Normalize", "Math|Vector", "A / |A|", true }, "i:vector:A,o:vector:" },
    [BPK_MAKE_VECTOR] = { { "Make Vector", "Math|Vector", "A vector from X, Y, Z.", true }, "i:float:X,i:float:Y,i:float:Z,o:vector:" },
    [BPK_BREAK_VECTOR] = { { "Break Vector", "Math|Vector", "X, Y, Z of a vector.", true }, "i:vector:,o:float:X,o:float:Y,o:float:Z" },
    [BPK_MAKE_ROTATOR] = { { "Make Rotator", "Math|Rotator", "Pitch, Yaw, Roll (degrees).", true }, "i:float:Pitch,i:float:Yaw,i:float:Roll,o:rotator:" },
    [BPK_BREAK_ROTATOR] = { { "Break Rotator", "Math|Rotator", "Pitch, Yaw, Roll of a rotator.", true }, "i:rotator:,o:float:Pitch,o:float:Yaw,o:float:Roll" },
    [BPK_GREATER] = { { "Greater (Float)", "Math|Compare", "A > B", true }, "i:float:A,i:float:B,o:bool:" },
    [BPK_LESS] = { { "Less (Float)", "Math|Compare", "A < B", true }, "i:float:A,i:float:B,o:bool:" },
    [BPK_EQUAL] = { { "Equal (Float)", "Math|Compare", "A == B (within 0.0001)", true }, "i:float:A,i:float:B,o:bool:" },
    [BPK_AND] = { { "AND", "Math|Boolean", "A and B", true }, "i:bool:A,i:bool:B,o:bool:" },
    [BPK_OR] = { { "OR", "Math|Boolean", "A or B", true }, "i:bool:A,i:bool:B,o:bool:" },
    [BPK_NOT] = { { "NOT", "Math|Boolean", "not A", true }, "i:bool:A,o:bool:" },
    [BPK_RANDOM] = { { "Random Float in Range", "Math|Random", "A random number between Min and Max.", true }, "i:float:Min,i:float:Max,o:float:" },
    [BPK_LERP] = { { "Lerp", "Math|Float", "A + (B - A) * Alpha", true }, "i:float:A,i:float:B,i:float:Alpha,o:float:" },
    [BPK_CLAMP] = { { "Clamp (Float)", "Math|Float", "Value kept between Min and Max.", true }, "i:float:Value,i:float:Min,i:float:Max,o:float:" },
    [BPK_SIN] = { { "Sin (Degrees)", "Math|Trig", "sin(A degrees)", true }, "i:float:A,o:float:" },
    [BPK_COS] = { { "Cos (Degrees)", "Math|Trig", "cos(A degrees)", true }, "i:float:A,o:float:" },
    [BPK_SELECT] = { { "Select Float", "Utilities", "A when Pick A, else B.", true }, "i:bool:Pick A,i:float:A,i:float:B,o:float:" },
    [BPK_SELF] = { { "Get a reference to self", "Utilities", "This actor.", true }, "o:actor:Self" },
    [BPK_GET_COMPONENT] = { { "Get Component", "Components", "One of this actor's components by its name (drag it from the Components panel): move it, hide it, read where it is.", true }, "o:actor:Component" },
    [BPK_GET_PLAYER] = { { "Get Player Character", "Game", "The pawn the player possesses.", true }, "o:actor:Player" },
    [BPK_DELTA] = { { "Get World Delta Seconds", "Utilities|Time", "The frame's time (s).", true }, "o:float:" },
    [BPK_GAME_TIME] = { { "Get Game Time in Seconds", "Utilities|Time", "Seconds since play started.", true }, "o:float:" },
    [BPK_GET_LOCATION] = { { "Get Actor Location", "Transformation", "Where the actor is (cm).", true }, "i:actor:Target,o:vector:" },
    [BPK_SET_LOCATION] = { { "Set Actor Location", "Transformation", "Moves the actor there.", false }, "i:exec:,i:actor:Target,i:vector:New Location,o:exec:" },
    [BPK_ADD_OFFSET] = { { "Add Actor World Offset", "Transformation", "Moves the actor by Delta (cm).", false }, "i:exec:,i:actor:Target,i:vector:Delta Location,o:exec:" },
    [BPK_GET_ROTATION] = { { "Get Actor Rotation", "Transformation", "Its rotation (degrees).", true }, "i:actor:Target,o:rotator:" },
    [BPK_SET_ROTATION] = { { "Set Actor Rotation", "Transformation", "Sets its rotation.", false }, "i:exec:,i:actor:Target,i:rotator:New Rotation,o:exec:" },
    [BPK_ADD_ROTATION] = { { "Add Actor Local Rotation", "Transformation", "Turns it by Delta (degrees).", false }, "i:exec:,i:actor:Target,i:rotator:Delta Rotation,o:exec:" },
    [BPK_GET_SCALE] = { { "Get Actor Scale 3D", "Transformation", "Its scale.", true }, "i:actor:Target,o:vector:" },
    [BPK_SET_SCALE] = { { "Set Actor Scale 3D", "Transformation", "Sets its scale.", false }, "i:exec:,i:actor:Target,i:vector:New Scale 3D,o:exec:" },
    [BPK_GET_FORWARD] = { { "Get Actor Forward Vector", "Transformation", "Where it faces (X).", true }, "i:actor:Target,o:vector:" },
    [BPK_GET_RIGHT] = { { "Get Actor Right Vector", "Transformation", "Its right (Y).", true }, "i:actor:Target,o:vector:" },
    [BPK_SPAWN] = { { "Spawn Actor from Class", "Game", "A new actor of Class at Spawn Transform (right click the pin: Split).", false },
                    "i:exec:,i:class:Class,i:transform:Spawn Transform,o:exec:,o:actor:Return Value" },
    [BPK_DESTROY] = { { "Destroy Actor", "Game", "Removes the actor.", false }, "i:exec:,i:actor:Target,o:exec:" },
    [BPK_IS_VALID] = { { "Is Valid", "Utilities", "The actor exists.", true }, "i:actor:Object,o:bool:" },
    [BPK_DISTANCE] = { { "Get Distance To", "Transformation", "From Target to Other (cm).", true }, "i:actor:Target,i:actor:Other Actor,o:float:" },
    [BPK_CAST] = { { "Cast To", "Utilities|Casting", "Whether Object is of the node's class (or a child of it).", false },
                   "i:exec:,i:actor:Object,o:exec:,o:exec:Cast Failed,o:actor:As" },
    [BPK_CALL_INTERFACE] = { { "Call Interface Function", "Interfaces", "Runs the function on Target if its class implements the interface.", false },
                             "i:exec:,i:actor:Target,o:exec:" },
    [BPK_DOES_IMPLEMENT] = { { "Does Implement Interface", "Interfaces", "Whether Target's class implements the interface.", true }, "i:actor:Target,o:bool:" },
    [BPK_LIGHT_INTENSITY] = { { "Set Intensity", "Rendering|Light", "A light's intensity (lux, candela).", false }, "i:exec:,i:actor:Target,i:float:New Intensity,o:exec:" },
    [BPK_LIGHT_COLOR] = { { "Set Light Color", "Rendering|Light", "A light's color (0-1 per channel).", false }, "i:exec:,i:actor:Target,i:vector:New Light Color,o:exec:" },
    [BPK_FOG_DENSITY] = { { "Set Fog Density", "Rendering|Fog", "The level's Exponential Height Fog (1/m).", false }, "i:exec:,i:float:Density,o:exec:" },
    [BPK_FOG_COLOR] = { { "Set Fog Inscattering Color", "Rendering|Fog", "The fog's color.", false }, "i:exec:,i:vector:Color,o:exec:" },
    [BPK_CLOUD_COVERAGE] = { { "Set Cloud Coverage", "Rendering|Sky", "The Volumetric Cloud's coverage (0-1).", false }, "i:exec:,i:float:Coverage,o:exec:" },
    [BPK_SUN_ANGLE] = { { "Set Sun Angle", "Rendering|Sky", "The sun's pitch and yaw (degrees): the time of day.", false }, "i:exec:,i:float:Pitch,i:float:Yaw,o:exec:" },
    [BPK_SET_VISIBILITY] = { { "Set Actor Hidden In Game", "Rendering", "Hides or shows the actor.", false }, "i:exec:,i:actor:Target,i:bool:New Hidden,o:exec:" },
    [BPK_SET_COLOR] = { { "Set Base Color", "Rendering|Material", "The mesh's base color.", false }, "i:exec:,i:actor:Target,i:vector:Color,o:exec:" },
    [BPK_MAT_SCALAR] = { { "Set Scalar Parameter Value on Materials", "Rendering|Material",
                           "A parameter of the actor's material (a named Value node): the actor gets its own instance.", false },
                         "i:exec:,i:actor:Target,i:float:Parameter Value,o:exec:" },
    [BPK_MAT_VECTOR] = { { "Set Vector Parameter Value on Materials", "Rendering|Material",
                           "A color parameter of the actor's material (a named RGB node): the actor gets its own instance.", false },
                         "i:exec:,i:actor:Target,i:vector:Parameter Value,o:exec:" },
    [BPK_ADD_IMPULSE] = { { "Add Impulse", "Physics", "Pushes a simulating actor at once (kg cm/s; Vel Change: the velocity added, whatever the mass).", false },
                          "i:exec:,i:actor:Target,i:vector:Impulse,i:bool:Vel Change,o:exec:" },
    [BPK_SET_SIMULATE] = { { "Set Simulate Physics", "Physics", "Turns the actor into a rigid body (or back).", false },
                           "i:exec:,i:actor:Target,i:bool:Simulate,o:exec:" },
    [BPK_RADIAL_IMPULSE] = { { "Add Radial Impulse", "Physics", "An explosion: the simulating actors within Radius pushed away (cm/s at the middle, none at the edge).", false },
                             "i:exec:,i:vector:Origin,i:float:Radius,i:float:Strength,o:exec:" },
    [BPK_GET_VELOCITY] = { { "Get Physics Linear Velocity", "Physics", "A simulating actor's velocity (cm/s).", true }, "i:actor:Target,o:vector:Return Value" },
    [BPK_EVENT_UPDATE_ANIM] = { { "Event Blueprint Update Animation", "Animation",
                                  "An Animation Blueprint's frame: set the variables the Anim Graph reads.", false },
                                "o:exec:,o:float:Delta Time X" },
    [BPK_TRY_GET_PAWN_OWNER] = { { "Try Get Pawn Owner", "Animation", "The pawn (actor) this animation plays on.", true }, "o:actor:Return Value" },
    [BPK_GET_ACTOR_VELOCITY] = { { "Get Velocity", "Utilities|Transformation", "The actor's velocity (cm/s): a Character's, a body's, a flying pawn's.", true },
                                 "i:actor:Target,o:vector:Return Value" },
    [BPK_IS_FALLING] = { { "Is Falling", "Character", "The Character is in the air (jumping or falling).", true }, "i:actor:Target,o:bool:Return Value" },
    [BPK_PLAY_MONTAGE] = { { "Play Montage", "Animation|Montage", "Plays an Anim Montage on the actor's Skeletal Mesh (the montage: in Details).", false },
                           "i:exec:,i:actor:Target,i:float:Play Rate,o:exec:" },
    [BPK_STOP_MONTAGE] = { { "Stop Montage", "Animation|Montage", "Blends the playing montage out.", false }, "i:exec:,i:actor:Target,i:float:Blend Out,o:exec:" },
    [BPK_PLAY_ANIMATION] = { { "Play Animation", "Animation", "Plays a sequence on the actor's Skeletal Mesh, leaving its Anim Blueprint (the sequence: in Details).", false },
                             "i:exec:,i:actor:Target,i:bool:Looping,o:exec:" },
    [BPK_BB_GET_FLOAT] = { { "Get Blackboard Value as Float", "AI|Blackboard", "An AI pawn's Blackboard key (the key: in Details).", true }, "i:actor:Target,o:float:Return Value" },
    [BPK_BB_GET_BOOL] = { { "Get Blackboard Value as Bool", "AI|Blackboard", "An AI pawn's Blackboard key (the key: in Details).", true }, "i:actor:Target,o:bool:Return Value" },
    [BPK_BB_GET_VECTOR] = { { "Get Blackboard Value as Vector", "AI|Blackboard", "An AI pawn's Blackboard key (the key: in Details).", true }, "i:actor:Target,o:vector:Return Value" },
    [BPK_BB_GET_ACTOR] = { { "Get Blackboard Value as Actor", "AI|Blackboard", "An AI pawn's Blackboard key (the key: in Details).", true }, "i:actor:Target,o:actor:Return Value" },
    [BPK_BB_SET_FLOAT] = { { "Set Blackboard Value as Float", "AI|Blackboard", "Sets an AI pawn's Blackboard key.", false }, "i:exec:,i:actor:Target,i:float:Value,o:exec:" },
    [BPK_BB_SET_BOOL] = { { "Set Blackboard Value as Bool", "AI|Blackboard", "Sets an AI pawn's Blackboard key.", false }, "i:exec:,i:actor:Target,i:bool:Value,o:exec:" },
    [BPK_BB_SET_VECTOR] = { { "Set Blackboard Value as Vector", "AI|Blackboard", "Sets an AI pawn's Blackboard key.", false }, "i:exec:,i:actor:Target,i:vector:Value,o:exec:" },
    [BPK_BB_SET_ACTOR] = { { "Set Blackboard Value as Actor", "AI|Blackboard", "Sets an AI pawn's Blackboard key.", false }, "i:exec:,i:actor:Target,i:actor:Value,o:exec:" },
    [BPK_AI_MOVE_TO] = { { "AI Move To", "AI|Navigation", "The pawn walks to the place along the NavMesh (round walls, up stairs and ramps, over Nav Links; the grid while the NavMesh is built).", false }, "i:exec:,i:actor:Pawn,i:vector:Destination,i:float:Acceptance Radius,o:exec:" },
    [BPK_RUN_BT] = { { "Run Behavior Tree", "AI", "The pawn's AI runs a Behavior Tree (in Details).", false }, "i:exec:,i:actor:Pawn,o:exec:" },
    [BPK_REPORT_NOISE] = { { "Report Noise Event", "AI|Perception", "A noise the AIs hear (Loudness 1: their hearing radius).", false }, "i:exec:,i:vector:Noise Location,i:float:Loudness,o:exec:" },
    [BPK_GET_CONTROL_ROTATION] = { { "Get Control Rotation", "Pawn", "Where the view looks: pitch and yaw (the mouse's).", true }, "o:rotator:Return Value" },
    [BPK_GET_OWNER] = { { "Get Owner", "Utilities", "The actor this component belongs to (self when it is an actor).", true }, "o:actor:Owner" },
    [BPK_MOVE_INPUT] = { { "Add Movement Input", "Pawn|Input", "The Character walks along World Direction (times Scale).", false },
                         "i:exec:,i:vector:World Direction,i:float:Scale Value,o:exec:" },
    [BPK_JUMP] = { { "Jump", "Character", "Jumps when on the ground.", false }, "i:exec:,o:exec:" },
    [BPK_LAUNCH_CHARACTER] = { { "Launch Character", "Character", "Throws the Character (in the air too: a double jump, a jump pad): Launch Velocity added to its velocity, or put in place of it with XY / Z Override (cm/s).", false },
                               "i:exec:,i:vector:Launch Velocity,i:bool:XY Override,i:bool:Z Override,o:exec:" },
    [BPK_YAW_INPUT] = { { "Add Controller Yaw Input", "Pawn|Input", "Turns the view (degrees).", false }, "i:exec:,i:float:Val,o:exec:" },
    [BPK_PITCH_INPUT] = { { "Add Controller Pitch Input", "Pawn|Input", "Tilts the view (degrees).", false }, "i:exec:,i:float:Val,o:exec:" },
    [BPK_AXIS] = { { "Get Input Axis", "Input", "MoveForward (W/S), MoveRight (D/A), Turn and LookUp (the mouse): the axis in Details; a script names it: GetInputAxis(\"MoveForward\").", true }, "o:float:" },
    [BPK_KEY_DOWN] = { { "Is Key Down", "Input", "Whether the key is held (the key: in Details; a script names it: IsKeyDown(\"Space\")).", true }, "o:bool:" },
    [BPK_CONTROL_FORWARD] = { { "Get Control Forward Vector", "Pawn", "The view's forward on the ground (yaw only).", true }, "o:vector:" },
    [BPK_CONTROL_RIGHT] = { { "Get Control Right Vector", "Pawn", "The view's right on the ground.", true }, "o:vector:" },
    [BPK_FUNC_ENTRY] = { { "Function Entry", "Functions", "Where a function starts (its inputs).", false }, "o:exec:" },
    [BPK_FUNC_RETURN] = { { "Return Node", "Functions", "Where a function ends (its outputs).", false }, "i:exec:" },
    [BPK_CALL_FUNCTION] = { { "Call Function", "Functions", "Runs one of this Blueprint's functions.", false }, "i:exec:,o:exec:" },
    [BPK_MAKE_STRUCT] = { { "Make Struct", "Structures", "A structure from its fields.", true }, "i:field:,o:struct:" },
    [BPK_BREAK_STRUCT] = { { "Break Struct", "Structures", "A structure's fields.", true }, "i:struct:,o:field:" },
    [BPK_SWITCH_ENUM] = { { "Switch on Enum", "Flow Control", "One output per entry.", false }, "i:exec:,i:enum:Selection,o:entry:" },
    [BPK_ENUM_LITERAL] = { { "Literal Enum", "Utilities", "One of an enumeration's entries.", true }, "o:enum:" },
    [BPK_DATA_GET] = { { "Get Data Asset Value", "Data", "A value of a Data Asset.", true }, "o:data:" },
    [BPK_INT_TO_FLOAT] = { { "To Float (Integer)", "Math|Conversions", "An integer as a float.", true }, "i:int:,o:float:" },
    [BPK_FLOAT_TO_INT] = { { "Truncate", "Math|Conversions", "A float as an integer (toward 0).", true }, "i:float:,o:int:" },
    // --- integers ---
    [BPK_IADD] = { { "Add (Integer)", "Math|Integer", "A + B", true }, "i:int:A,i:int:B,o:int:" },
    [BPK_ISUB] = { { "Subtract (Integer)", "Math|Integer", "A - B", true }, "i:int:A,i:int:B,o:int:" },
    [BPK_IMUL] = { { "Multiply (Integer)", "Math|Integer", "A * B", true }, "i:int:A,i:int:B,o:int:" },
    [BPK_IDIV] = { { "Divide (Integer)", "Math|Integer", "A / B, toward 0 (0 when B is 0)", true }, "i:int:A,i:int:B,o:int:" },
    [BPK_IMOD] = { { "% (Integer)", "Math|Integer", "The remainder of A / B", true }, "i:int:A,i:int:B,o:int:" },
    [BPK_IGREATER] = { { "Greater (Integer)", "Math|Integer", "A > B", true }, "i:int:A,i:int:B,o:bool:" },
    [BPK_ILESS] = { { "Less (Integer)", "Math|Integer", "A < B", true }, "i:int:A,i:int:B,o:bool:" },
    [BPK_IGE] = { { "Greater Equal (Integer)", "Math|Integer", "A >= B", true }, "i:int:A,i:int:B,o:bool:" },
    [BPK_ILE] = { { "Less Equal (Integer)", "Math|Integer", "A <= B", true }, "i:int:A,i:int:B,o:bool:" },
    [BPK_IEQUAL] = { { "Equal (Integer)", "Math|Integer", "A == B", true }, "i:int:A,i:int:B,o:bool:" },
    [BPK_INOTEQUAL] = { { "Not Equal (Integer)", "Math|Integer", "A != B", true }, "i:int:A,i:int:B,o:bool:" },
    [BPK_IMAX] = { { "Max (Integer)", "Math|Integer", "The larger of A and B", true }, "i:int:A,i:int:B,o:int:" },
    [BPK_IMIN] = { { "Min (Integer)", "Math|Integer", "The smaller of A and B", true }, "i:int:A,i:int:B,o:int:" },
    [BPK_IABS] = { { "Absolute (Integer)", "Math|Integer", "|A|", true }, "i:int:A,o:int:" },
    [BPK_ICLAMP] = { { "Clamp (Integer)", "Math|Integer", "Value kept between Min and Max.", true }, "i:int:Value,i:int:Min,i:int:Max,o:int:" },
    // --- floats ---
    [BPK_FGE] = { { "Greater Equal (Float)", "Math|Float", "A >= B", true }, "i:float:A,i:float:B,o:bool:" },
    [BPK_FLE] = { { "Less Equal (Float)", "Math|Float", "A <= B", true }, "i:float:A,i:float:B,o:bool:" },
    [BPK_FNOTEQUAL] = { { "Not Equal (Float)", "Math|Float", "A != B", true }, "i:float:A,i:float:B,o:bool:" },
    [BPK_FMAX] = { { "Max (Float)", "Math|Float", "The larger of A and B", true }, "i:float:A,i:float:B,o:float:" },
    [BPK_FMIN] = { { "Min (Float)", "Math|Float", "The smaller of A and B", true }, "i:float:A,i:float:B,o:float:" },
    [BPK_FABS] = { { "Absolute (Float)", "Math|Float", "|A|", true }, "i:float:A,o:float:" },
    [BPK_POW] = { { "Power", "Math|Float", "Base ^ Exp", true }, "i:float:Base,i:float:Exp,o:float:" },
    [BPK_SQRT] = { { "Square Root", "Math|Float", "sqrt(A) (0 below 0)", true }, "i:float:A,o:float:" },
    [BPK_FLOOR] = { { "Floor", "Math|Float", "The integer at or below A", true }, "i:float:A,o:int:" },
    [BPK_CEIL] = { { "Ceil", "Math|Float", "The integer at or above A", true }, "i:float:A,o:int:" },
    [BPK_ROUND] = { { "Round", "Math|Float", "The nearest integer", true }, "i:float:A,o:int:" },
    [BPK_FRAC] = { { "Fraction", "Math|Float", "A - Floor(A)", true }, "i:float:A,o:float:" },
    [BPK_FMOD] = { { "% (Float)", "Math|Float", "The remainder of A / B", true }, "i:float:A,i:float:B,o:float:" },
    [BPK_TAN] = { { "Tan (Degrees)", "Math|Trig", "tan(A degrees)", true }, "i:float:A,o:float:" },
    [BPK_ATAN2] = { { "Atan2 (Degrees)", "Math|Trig", "The angle of (X, Y) in degrees", true }, "i:float:Y,i:float:X,o:float:" },
    [BPK_MAP_RANGE] = { { "Map Range Clamped", "Math|Float", "Value from [In Range A, In Range B] to [Out Range A, Out Range B], clamped.", true },
                        "i:float:Value,i:float:In Range A,i:float:In Range B,i:float:Out Range A,i:float:Out Range B,o:float:" },
    [BPK_FINTERP] = { { "FInterp To", "Math|Interpolation", "Current moved toward Target (Interp Speed 0: at once).", true },
                      "i:float:Current,i:float:Target,i:float:Delta Time,i:float:Interp Speed,o:float:" },
    [BPK_NEARLY_EQUAL] = { { "Nearly Equal (Float)", "Math|Float", "|A - B| <= Error Tolerance", true }, "i:float:A,i:float:B,i:float:Error Tolerance,o:bool:" },
    [BPK_XOR] = { { "XOR", "Math|Boolean", "A or B, not both", true }, "i:bool:A,i:bool:B,o:bool:" },
    // --- vectors and rotators ---
    [BPK_VMUL] = { { "Multiply (Vector * Vector)", "Math|Vector", "A * B per component", true }, "i:vector:A,i:vector:B,o:vector:" },
    [BPK_VDIVS] = { { "Divide (Vector / Float)", "Math|Vector", "A / B (0 when B is 0)", true }, "i:vector:A,i:float:B,o:vector:" },
    [BPK_CROSS] = { { "Cross Product", "Math|Vector", "A x B", true }, "i:vector:A,i:vector:B,o:vector:" },
    [BPK_VDIST] = { { "Distance (Vector)", "Math|Vector", "|V1 - V2|", true }, "i:vector:V1,i:vector:V2,o:float:" },
    [BPK_VLERP] = { { "Lerp (Vector)", "Math|Vector", "A + (B - A) * Alpha", true }, "i:vector:A,i:vector:B,i:float:Alpha,o:vector:" },
    [BPK_VINTERP] = { { "VInterp To", "Math|Interpolation", "Current moved toward Target.", true },
                      "i:vector:Current,i:vector:Target,i:float:Delta Time,i:float:Interp Speed,o:vector:" },
    [BPK_ROTATE_VECTOR] = { { "Rotate Vector", "Math|Vector", "A turned by B", true }, "i:vector:A,i:rotator:B,o:vector:" },
    [BPK_UNROTATE_VECTOR] = { { "Unrotate Vector", "Math|Vector", "A turned back by B", true }, "i:vector:A,i:rotator:B,o:vector:" },
    [BPK_VEQUAL] = { { "Equal (Vector)", "Math|Vector", "A == B within Error Tolerance", true }, "i:vector:A,i:vector:B,i:float:Error Tolerance,o:bool:" },
    [BPK_ROT_FORWARD] = { { "Get Forward Vector", "Math|Rotator", "The rotator's X axis.", true }, "i:rotator:In Rot,o:vector:" },
    [BPK_ROT_RIGHT] = { { "Get Right Vector", "Math|Rotator", "The rotator's Y axis.", true }, "i:rotator:In Rot,o:vector:" },
    [BPK_ROT_UP] = { { "Get Up Vector", "Math|Rotator", "The rotator's Z axis.", true }, "i:rotator:In Rot,o:vector:" },
    [BPK_LOOK_AT] = { { "Find Look at Rotation", "Math|Rotator", "The rotation from Start looking at Target.", true }, "i:vector:Start,i:vector:Target,o:rotator:" },
    [BPK_ROT_FROM_X] = { { "Make Rot from X", "Math|Rotator", "The rotation whose forward is X.", true }, "i:vector:X,o:rotator:" },
    [BPK_COMBINE_ROT] = { { "Combine Rotators", "Math|Rotator", "A then B", true }, "i:rotator:A,i:rotator:B,o:rotator:" },
    [BPK_RLERP] = { { "Lerp (Rotator)", "Math|Rotator", "A toward B by Alpha (the short way)", true }, "i:rotator:A,i:rotator:B,i:float:Alpha,o:rotator:" },
    [BPK_RINTERP] = { { "RInterp To", "Math|Interpolation", "Current turned toward Target.", true },
                      "i:rotator:Current,i:rotator:Target,i:float:Delta Time,i:float:Interp Speed,o:rotator:" },
    [BPK_DELTA_ROT] = { { "Delta (Rotator)", "Math|Rotator", "A - B, each angle in -180..180", true }, "i:rotator:A,i:rotator:B,o:rotator:" },
    // --- random (Unreal's Math|Random) ---
    [BPK_RAND_BOOL] = { { "Random Bool", "Math|Random", "True or false, half the time each.", true }, "o:bool:" },
    [BPK_RAND_BOOL_W] = { { "Random Bool with Weight", "Math|Random", "True with the probability Weight (0-1).", true }, "i:float:Weight,o:bool:" },
    [BPK_RAND_INT] = { { "Random Integer", "Math|Random", "0 .. Max - 1", true }, "i:int:Max,o:int:" },
    [BPK_RAND_INT_RANGE] = { { "Random Integer in Range", "Math|Random", "Min .. Max (both included)", true }, "i:int:Min,i:int:Max,o:int:" },
    [BPK_RAND_FLOAT] = { { "Random Float", "Math|Random", "0 .. 1", true }, "o:float:" },
    [BPK_RAND_UNIT_VEC] = { { "Random Unit Vector", "Math|Random", "A direction, any way, of length 1.", true }, "o:vector:" },
    [BPK_RAND_CONE] = { { "Random Unit Vector in Cone in Degrees", "Math|Random", "A direction within Cone Half Angle of Cone Dir.", true },
                        "i:vector:Cone Dir,i:float:Cone Half Angle in Degrees,o:vector:" },
    [BPK_RAND_ROT] = { { "Random Rotator", "Math|Random", "A random yaw and pitch (and roll when Roll).", true }, "i:bool:Roll,o:rotator:" },
    [BPK_RAND_POINT_BOX] = { { "Random Point in Bounding Box", "Math|Random", "A point inside the box (Center, Half Size).", true },
                             "i:vector:Center,i:vector:Half Size,o:vector:" },
    [BPK_RAND_INT64] = { { "Random Integer64 in Range", "Math|Random", "Min .. Max (both included)", true }, "i:int64:Min,i:int64:Max,o:int64:" },
    [BPK_RAND_ARRAY_ITEM] = { { "Random Array Item", "Utilities|Array", "An element picked at random and its index.", true },
                              "i:wild[]:Target Array,o:wild:Out Item,o:int:Out Index" },
    // --- strings ---
    [BPK_TO_STRING] = { { "To String", "Utilities|String", "Any value as text (wires do it on their own too).", true }, "i:wild:In,o:string:" },
    [BPK_STRING_TO_INT] = { { "String to Int", "Utilities|String", "The number written in the text (0: none).", true }, "i:string:In String,o:int:" },
    [BPK_STRING_TO_FLOAT] = { { "String to Float", "Utilities|String", "The number written in the text (0: none).", true }, "i:string:In String,o:float:" },
    [BPK_APPEND] = { { "Append", "Utilities|String", "A then B (Add pin: one more).", true }, "i:string*2:#,o:string:Return Value" },
    [BPK_STR_LEN] = { { "Len", "Utilities|String", "How many characters.", true }, "i:string:S,o:int:" },
    [BPK_SUBSTRING] = { { "Get Substring", "Utilities|String", "Length characters from Start Index.", true }, "i:string:Source String,i:int:Start Index,i:int:Length,o:string:" },
    [BPK_STR_CONTAINS] = { { "Contains", "Utilities|String", "Whether Search In has Substring (ignoring case).", true }, "i:string:Search In,i:string:Substring,o:bool:" },
    [BPK_STR_UPPER] = { { "To Upper", "Utilities|String", "In capitals.", true }, "i:string:Source String,o:string:" },
    [BPK_STR_LOWER] = { { "To Lower", "Utilities|String", "In small letters.", true }, "i:string:Source String,o:string:" },
    [BPK_STR_EQUAL] = { { "Equal (String)", "Utilities|String", "A == B (Name, Text and String alike).", true }, "i:string:A,i:string:B,o:bool:" },
    [BPK_STR_REPLACE] = { { "Replace", "Utilities|String", "Every From in Source String turned into To.", true }, "i:string:Source String,i:string:From,i:string:To,o:string:" },
    [BPK_JOIN] = { { "Join String Array", "Utilities|String", "The strings one after the other with Separator.", true }, "i:string[]:Source Array,i:string:Separator,o:string:" },
    // --- colors ---
    [BPK_MAKE_COLOR] = { { "Make Linear Color", "Math|Color", "A color from R, G, B, A (0-1, more is brighter).", true }, "i:float:R,i:float:G,i:float:B,i:float:A,o:color:" },
    [BPK_BREAK_COLOR] = { { "Break Linear Color", "Math|Color", "R, G, B, A of a color.", true }, "i:color:,o:float:R,o:float:G,o:float:B,o:float:A" },
    [BPK_LERP_COLOR] = { { "Lerp (Linear Color)", "Math|Color", "A + (B - A) * Alpha", true }, "i:color:A,i:color:B,i:float:Alpha,o:color:" },
    [BPK_HSV_TO_RGB] = { { "HSV to RGB", "Math|Color", "A color from hue (0-360), saturation and value.", true }, "i:float:H,i:float:S,i:float:V,i:float:A,o:color:" },
    // --- transforms ---
    [BPK_MAKE_TRANSFORM] = { { "Make Transform", "Math|Transform", "A transform from its location, rotation and scale.", true },
                             "i:vector:Location,i:rotator:Rotation,i:vector:Scale,o:transform:" },
    [BPK_BREAK_TRANSFORM] = { { "Break Transform", "Math|Transform", "A transform's location, rotation and scale.", true },
                              "i:transform:In Transform,o:vector:Location,o:rotator:Rotation,o:vector:Scale" },
    [BPK_GET_TRANSFORM] = { { "Get Actor Transform", "Transformation", "Where the actor is, how it turns and its size.", true }, "i:actor:Target,o:transform:" },
    [BPK_SET_TRANSFORM] = { { "Set Actor Transform", "Transformation", "Moves, turns and sizes the actor at once.", false },
                            "i:exec:,i:actor:Target,i:transform:New Transform,o:exec:" },
    [BPK_TRANSFORM_LOC] = { { "Transform Location", "Math|Transform", "A point of the transform's space in the world.", true },
                            "i:transform:T,i:vector:Location,o:vector:" },
    [BPK_INV_TRANSFORM_LOC] = { { "Inverse Transform Location", "Math|Transform", "A world point in the transform's space.", true },
                                "i:transform:T,i:vector:Location,o:vector:" },
    [BPK_TRANSFORM_DIR] = { { "Transform Direction", "Math|Transform", "A direction of the transform's space in the world.", true },
                            "i:transform:T,i:vector:Direction,o:vector:" },
    [BPK_COMPOSE] = { { "Compose Transforms", "Math|Transform", "A then B", true }, "i:transform:A,i:transform:B,o:transform:" },
    [BPK_INVERT_TRANSFORM] = { { "Invert Transform", "Math|Transform", "The transform that undoes T", true }, "i:transform:T,o:transform:" },
    // --- classes and objects ---
    [BPK_GET_CLASS] = { { "Get Class", "Utilities", "The actor's class.", true }, "i:actor:Object,o:class:" },
    [BPK_CLASS_EQUAL] = { { "Equal (Class)", "Utilities", "A and B are the same class.", true }, "i:class:A,i:class:B,o:bool:" },
    [BPK_GET_DISPLAY_NAME] = { { "Get Display Name", "Utilities", "The actor's name (the Outliner's).", true }, "i:actor:Object,o:string:" },
    [BPK_OBJ_EQUAL] = { { "Equal (Object)", "Utilities", "A and B are the same actor.", true }, "i:actor:A,i:actor:B,o:bool:" },
    [BPK_GET_ALL_ACTORS] = { { "Get All Actors Of Class", "Utilities", "Every actor of Actor Class (or a child of it).", false },
                             "i:exec:,i:class:Actor Class,o:exec:,o:actor[]:Out Actors" },
    [BPK_GET_ACTOR_OF_CLASS] = { { "Get Actor Of Class", "Utilities", "The first actor of Actor Class.", true }, "i:class:Actor Class,o:actor:Return Value" },
    [BPK_SELECTW] = { { "Select", "Utilities", "A when Pick A, else B (any type).", true }, "i:bool:Pick A,i:wild:A,i:wild:B,o:wild:Return Value" },
    [BPK_INT64_ADD] = { { "Add (Integer64)", "Math|Integer64", "A + B", true }, "i:int64:A,i:int64:B,o:int64:" },
    [BPK_INT64_SUB] = { { "Subtract (Integer64)", "Math|Integer64", "A - B", true }, "i:int64:A,i:int64:B,o:int64:" },
    [BPK_INT64_MUL] = { { "Multiply (Integer64)", "Math|Integer64", "A * B", true }, "i:int64:A,i:int64:B,o:int64:" },
    [BPK_INT64_DIV] = { { "Divide (Integer64)", "Math|Integer64", "A / B (0 when B is 0)", true }, "i:int64:A,i:int64:B,o:int64:" },
    // --- flow control (Unreal's Standard Macros) ---
    [BPK_FLIPFLOP] = { { "FlipFlop", "Flow Control", "A, then B, then A... (Is A: which one ran).", false }, "i:exec:,o:exec:A,o:exec:B,o:bool:Is A" },
    [BPK_GATE] = { { "Gate", "Flow Control", "Enter passes to Exit while the gate is open (Open, Close, Toggle).", false },
                   "i:exec:Enter,i:exec:Open,i:exec:Close,i:exec:Toggle,i:bool:Start Closed,o:exec:Exit" },
    [BPK_DO_N] = { { "Do N", "Flow Control", "Lets the execution through N times (Reset: again).", false },
                   "i:exec:Enter,i:int:N,i:exec:Reset,o:exec:Exit,o:int:Counter" },
    [BPK_WHILE] = { { "While Loop", "Flow Control", "Loop Body while Condition is true, then Completed.", false },
                    "i:exec:,i:bool:Condition,o:exec:Loop Body,o:exec:Completed" },
    [BPK_FOR_LOOP_BREAK] = { { "For Loop with Break", "Flow Control", "For Loop that Break stops.", false },
                             "i:exec:,i:int:First Index,i:int:Last Index,i:exec:Break,o:exec:Loop Body,o:int:Index,o:exec:Completed" },
    [BPK_FOREACH] = { { "For Each Loop", "Utilities|Array", "Loop Body for each element of Array, then Completed.", false },
                      "i:exec:,i:wild[]:Array,o:exec:Loop Body,o:wild:Array Element,o:int:Array Index,o:exec:Completed" },
    [BPK_FOREACH_BREAK] = { { "For Each Loop with Break", "Utilities|Array", "For Each Loop that Break stops.", false },
                            "i:exec:,i:wild[]:Array,i:exec:Break,o:exec:Loop Body,o:wild:Array Element,o:int:Array Index,o:exec:Completed" },
    [BPK_SWITCH_INT] = { { "Switch on Int", "Flow Control", "The output of Selection's number, else Default (Add pin: one more).", false },
                         "i:exec:,i:int:Selection,o:exec*2:,o:exec:Default" },
    [BPK_ISVALID_EXEC] = { { "Is Valid (Exec)", "Utilities", "Is Valid or Is Not Valid, by Input Object.", false },
                           "i:exec:,i:actor:Input Object,o:exec:Is Valid,o:exec:Is Not Valid" },
    [BPK_RETRIGGER_DELAY] = { { "Retriggerable Delay", "Flow Control", "Delay that starts over each time it runs.", false }, "i:exec:,i:float:Duration,o:exec:Completed" },
    [BPK_SET_TIMER] = { { "Set Timer by Event", "Utilities|Time", "Calls the Custom Event (the node's) after Time (again and again when Looping).", false },
                        "i:exec:,i:float:Time,i:bool:Looping,o:exec:" },
    [BPK_CLEAR_TIMER] = { { "Clear Timer by Event", "Utilities|Time", "Stops the Custom Event's timer.", false }, "i:exec:,o:exec:" },
    [BPK_EVENT_END_PLAY] = { { "Event End Play", "Events", "Runs when play stops or the actor is destroyed.", false }, "o:exec:" },
    // --- arrays ---
    [BPK_ARR_ADD] = { { "Add", "Utilities|Array", "Adds the item at the end; its index.", false }, "i:exec:,i:wild[]:Target Array,i:wild:New Item,o:exec:,o:int:" },
    [BPK_ARR_ADD_UNIQUE] = { { "Add Unique", "Utilities|Array", "Adds the item when it isn't there yet; its index (-1: it was).", false },
                             "i:exec:,i:wild[]:Target Array,i:wild:New Item,o:exec:,o:int:" },
    [BPK_ARR_INSERT] = { { "Insert", "Utilities|Array", "Puts the item at Index (the rest move up).", false }, "i:exec:,i:wild[]:Target Array,i:wild:New Item,i:int:Index,o:exec:" },
    [BPK_ARR_REMOVE_INDEX] = { { "Remove Index", "Utilities|Array", "Takes out the element at Index.", false }, "i:exec:,i:wild[]:Target Array,i:int:Index To Remove,o:exec:" },
    [BPK_ARR_REMOVE] = { { "Remove Item", "Utilities|Array", "Takes out every element equal to Item; whether any was.", false },
                         "i:exec:,i:wild[]:Target Array,i:wild:Item,o:exec:,o:bool:" },
    [BPK_ARR_CLEAR] = { { "Clear", "Utilities|Array", "Empties the array.", false }, "i:exec:,i:wild[]:Target Array,o:exec:" },
    [BPK_ARR_SET] = { { "Set Array Elem", "Utilities|Array", "The element at Index becomes Item (Size to Fit: grows when needed).", false },
                      "i:exec:,i:wild[]:Target Array,i:int:Index,i:wild:Item,i:bool:Size to Fit,o:exec:" },
    [BPK_ARR_GET] = { { "Get (a copy)", "Utilities|Array", "The element at Index.", true }, "i:wild[]:Target Array,i:int:Index,o:wild:" },
    [BPK_ARR_LEN] = { { "Length", "Utilities|Array", "How many elements.", true }, "i:wild[]:Target Array,o:int:" },
    [BPK_ARR_LAST] = { { "Last Index", "Utilities|Array", "Length - 1", true }, "i:wild[]:Target Array,o:int:" },
    [BPK_ARR_VALID_INDEX] = { { "Is Valid Index", "Utilities|Array", "0 <= Index < Length", true }, "i:wild[]:Target Array,i:int:Index to Test,o:bool:" },
    [BPK_ARR_CONTAINS] = { { "Contains Item", "Utilities|Array", "Whether an element equals Item.", true }, "i:wild[]:Target Array,i:wild:Item to Find,o:bool:" },
    [BPK_ARR_FIND] = { { "Find Item", "Utilities|Array", "The first index of Item (-1: none).", true }, "i:wild[]:Target Array,i:wild:Item to Find,o:int:" },
    [BPK_ARR_MAKE] = { { "Make Array", "Utilities|Array", "An array of its inputs (Add pin: one more).", true }, "i:wild*2:,o:wild[]:Array" },
    [BPK_ARR_SHUFFLE] = { { "Shuffle", "Utilities|Array", "The elements in a random order.", false }, "i:exec:,i:wild[]:Target Array,o:exec:" },
    [BPK_ARR_APPEND] = { { "Append Array", "Utilities|Array", "Source Array's elements added at the end.", false }, "i:exec:,i:wild[]:Target Array,i:wild[]:Source Array,o:exec:" },
    [BPK_ARR_RESIZE] = { { "Resize", "Utilities|Array", "Length becomes Size (new elements: zero).", false }, "i:exec:,i:wild[]:Target Array,i:int:Size,o:exec:" },
    [BPK_ARR_IS_EMPTY] = { { "Is Empty", "Utilities|Array", "No elements.", true }, "i:wild[]:Target Array,o:bool:" },
    // --- sets ---
    [BPK_SET_ADD] = { { "Add (Set)", "Utilities|Set", "Adds the item (a set keeps each once).", false }, "i:exec:,i:wild{}:Target Set,i:wild:New Item,o:exec:" },
    [BPK_SET_REMOVE] = { { "Remove (Set)", "Utilities|Set", "Takes the item out; whether it was there.", false }, "i:exec:,i:wild{}:Target Set,i:wild:Item,o:exec:,o:bool:" },
    [BPK_SET_CONTAINS] = { { "Contains (Set)", "Utilities|Set", "Whether the item is in it.", true }, "i:wild{}:Target Set,i:wild:Item to Find,o:bool:" },
    [BPK_SET_LEN] = { { "Length (Set)", "Utilities|Set", "How many items.", true }, "i:wild{}:Target Set,o:int:" },
    [BPK_SET_CLEAR] = { { "Clear (Set)", "Utilities|Set", "Empties the set.", false }, "i:exec:,i:wild{}:Target Set,o:exec:" },
    [BPK_SET_TO_ARRAY] = { { "To Array (Set)", "Utilities|Set", "The items as an array.", true }, "i:wild{}:A,o:wild[]:Result" },
    // --- maps ---
    [BPK_MAP_ADD] = { { "Add (Map)", "Utilities|Map", "Key -> Value (replaces Key's value).", false }, "i:exec:,i:wild<>:Target Map,i:wild:Key,i:wild2:Value,o:exec:" },
    [BPK_MAP_FIND] = { { "Find (Map)", "Utilities|Map", "Key's value; whether Key is there.", true }, "i:wild<>:Target Map,i:wild:Key,o:wild2:Value,o:bool:Found" },
    [BPK_MAP_REMOVE] = { { "Remove (Map)", "Utilities|Map", "Takes Key out; whether it was there.", false }, "i:exec:,i:wild<>:Target Map,i:wild:Key,o:exec:,o:bool:" },
    [BPK_MAP_CONTAINS] = { { "Contains (Map)", "Utilities|Map", "Whether Key is there.", true }, "i:wild<>:Target Map,i:wild:Key,o:bool:" },
    [BPK_MAP_LEN] = { { "Length (Map)", "Utilities|Map", "How many keys.", true }, "i:wild<>:Target Map,o:int:" },
    [BPK_MAP_CLEAR] = { { "Clear (Map)", "Utilities|Map", "Empties the map.", false }, "i:exec:,i:wild<>:Target Map,o:exec:" },
    [BPK_MAP_KEYS] = { { "Keys", "Utilities|Map", "Every key, as an array.", true }, "i:wild<>:Target Map,o:wild[]:Keys" },
    [BPK_MAP_VALUES] = { { "Values", "Utilities|Map", "Every value, as an array.", true }, "i:wild<>:Target Map,o:wild2[]:Values" },
    // --- levels ---
    [BPK_OPEN_LEVEL] = { { "Open Level (by Name)", "Game", "Leaves this level for another of the project (Content/.../<Level Name>.rlevel).", false },
                         "i:exec:,i:name:Level Name,o:exec:" },
    [BPK_LOAD_STREAM] = { { "Load Stream Level", "Game|Level Streaming", "Brings a level's actors into this one, read in the background; Completed when they are in.", false },
                          "i:exec:,i:name:Level Name,i:bool:Make Visible After Load,i:bool:Should Block on Load,o:exec:Completed" },
    [BPK_UNLOAD_STREAM] = { { "Unload Stream Level", "Game|Level Streaming", "Takes a streamed level's actors out.", false },
                            "i:exec:,i:name:Level Name,o:exec:Completed" },
    [BPK_GET_LEVEL_NAME] = { { "Get Current Level Name", "Game", "The persistent level's name.", true }, "o:string:" },
    [BPK_SPAWN_ASYNC] = { { "Spawn Actor from Class (Async)", "Game", "Loads Class in the background, then spawns it at Spawn Transform: Completed.", false },
                          "i:exec:,i:class:Class,i:transform:Spawn Transform,o:exec:,o:exec:Completed,o:actor:Return Value" },
    // --- macros ---
    [BPK_MACRO] = { { "Macro", "Macros", "A macro of this Blueprint or of a Macro Library (its nodes in place of this one).", false }, "" },
    [BPK_TUNNEL_IN] = { { "Inputs", "Macros", "The macro's inputs.", false }, "" },
    [BPK_TUNNEL_OUT] = { { "Outputs", "Macros", "The macro's outputs.", false }, "" },
    // --- Enhanced Input ---
    [BPK_EVENT_INPUT_ACTION] = { { "Enhanced Input Action", "Input|Enhanced Action Events",
                                   "An Input Action's events (the action: the node's): Triggered, Started, Ongoing, Canceled, Completed.", false },
                                 "o:exec:Triggered,o:exec:Started,o:exec:Ongoing,o:exec:Canceled,o:exec:Completed,o:ivalue:Action Value,"
                                 "o:float:Elapsed Seconds,o:float:Triggered Seconds" },
    [BPK_ADD_MAPPING_CONTEXT] = { { "Add Mapping Context", "Input|Enhanced Input",
                                    "Turns an Input Mapping Context on (higher Priority first).", false }, "i:exec:,i:int:Priority,o:exec:" },
    [BPK_REMOVE_MAPPING_CONTEXT] = { { "Remove Mapping Context", "Input|Enhanced Input", "Turns an Input Mapping Context off.", false }, "i:exec:,o:exec:" },
    [BPK_GET_ACTION_VALUE] = { { "Get Action Value", "Input|Enhanced Input", "An Input Action's value now.", true }, "o:ivalue:Action Value" },
    [BPK_PP_SET_FLOAT] = { { "Set Post Process Setting", "Rendering|Post Process",
                             "A Post Process Volume's setting (chosen in Details), turned on (overridden). No Target: the level's first volume.", false },
                           "i:exec:,i:actor:Target,i:float:Value,o:exec:" },
    [BPK_PP_SET_WEIGHT] = { { "Set Post Process Blend Weight", "Rendering|Post Process",
                              "How much the volume counts (0: off, 1: fully). No Target: the level's first volume.", false },
                            "i:exec:,i:actor:Target,i:float:Blend Weight,o:exec:" },
    [BPK_PP_SET_TINT] = { { "Set Post Process Color Tint", "Rendering|Post Process",
                            "The volume's color grading tint (Gain per channel, 1: neutral). No Target: the level's first volume.", false },
                          "i:exec:,i:actor:Target,i:color:Tint,o:exec:" },
    [BPK_CREATE_WIDGET] = { { "Create Widget", "User Interface", "Makes a widget of a Widget Blueprint class (its BeginPlay). Add to Viewport shows it.", false },
                            "i:exec:,i:class:Class,o:exec:,o:actor:Return Value" },
    [BPK_ADD_TO_VIEWPORT] = { { "Add to Viewport", "User Interface", "Shows the widget over the game (a higher ZOrder: on top).", false },
                              "i:exec:,i:actor:Target,i:int:ZOrder,o:exec:" },
    [BPK_REMOVE_FROM_PARENT] = { { "Remove from Parent", "User Interface", "Takes the widget off the screen (Add to Viewport shows it again).", false },
                                 "i:exec:,i:actor:Target,o:exec:" },
    [BPK_WIDGET_SET_TEXT] = { { "Set Text (Widget)", "User Interface", "A Text's (or a Button's label) text. The widget: in Details; Target: the widget instance (none: self).", false },
                              "i:exec:,i:actor:Target,i:string:In Text,o:exec:" },
    [BPK_WIDGET_SET_PERCENT] = { { "Set Percent", "User Interface", "A Progress Bar's fill (0-1). The widget: in Details.", false },
                                 "i:exec:,i:actor:Target,i:float:In Percent,o:exec:" },
    [BPK_WIDGET_SET_VISIBLE] = { { "Set Widget Visible", "User Interface", "Shows or hides a widget of the tree (and its children). The widget: in Details.", false },
                                 "i:exec:,i:actor:Target,i:bool:Visible,o:exec:" },
    [BPK_WIDGET_SET_COLOR] = { { "Set Color and Opacity", "User Interface", "A widget's color (a Text's, a bar's fill, an Image's tint). The widget: in Details.", false },
                               "i:exec:,i:actor:Target,i:color:In Color,o:exec:" },
    [BPK_WIDGET_GET_VALUE] = { { "Get Value (Widget)", "User Interface", "A Slider's or Spin Box's value, a Check Box's state (1 checked), a Combo Box's selected index, a Progress Bar's percent. The widget: in Details.", true },
                               "i:actor:Target,o:float:Return Value" },
    [BPK_WIDGET_SET_VALUE] = { { "Set Value (Widget)", "User Interface", "Sets a Slider's or Spin Box's value, a Check Box's state, a Combo Box's selected index, a Progress Bar's percent. The widget: in Details.", false },
                               "i:exec:,i:actor:Target,i:float:In Value,o:exec:" },
    [BPK_WIDGET_GET_TEXT] = { { "Get Text (Widget)", "User Interface", "A Text's or Editable Text's text, a Combo Box's selected option. The widget: in Details.", true },
                              "i:actor:Target,o:string:Return Value" },
    [BPK_WIDGET_SET_ENABLED] = { { "Set Is Enabled", "User Interface", "Enables or disables a widget (grayed, no input; its children too). The widget: in Details.", false },
                                 "i:exec:,i:actor:Target,i:bool:In Is Enabled,o:exec:" },
    [BPK_PLAY_WIDGET_ANIM] = { { "Play Widget Animation", "User Interface|Animation", "Plays a Widget Animation of the widget (the Designer's timeline). Num Loops 0: forever. The animation: in Details.", false },
                               "i:exec:,i:actor:Target,i:int:Num Loops To Play,i:float:Playback Speed,o:exec:" },
    [BPK_STOP_WIDGET_ANIM] = { { "Stop Widget Animation", "User Interface|Animation", "Stops the Widget Animation (it stays where it got to). The animation: in Details.", false },
                               "i:exec:,i:actor:Target,o:exec:" },
    [BPK_PLAY_SOUND_2D] = { { "Play Sound 2D", "Audio", "Plays a sound (a Sound Wave or Sound Cue) without a place: music, UI, voices. Fire and forget. The sound: in Details.", false },
                            "i:exec:,i:float:Volume Multiplier,i:float:Pitch Multiplier,i:float:Start Time,o:exec:" },
    [BPK_PLAY_SOUND_AT] = { { "Play Sound at Location", "Audio", "Plays a sound at a place in the world (3D: it fades with the distance, comes from its side). Fire and forget. The sound: in Details.", false },
                            "i:exec:,i:vector:Location,i:float:Volume Multiplier,i:float:Pitch Multiplier,i:float:Start Time,o:exec:" },
    [BPK_SPAWN_SOUND_2D] = { { "Spawn Sound 2D", "Audio", "Plays a sound without a place and returns it (Stop Sound, Set Sound Paused, Is Sound Playing...). The sound: in Details.", false },
                             "i:exec:,i:float:Volume Multiplier,i:float:Pitch Multiplier,i:float:Start Time,o:exec:,o:int:Sound" },
    [BPK_SPAWN_SOUND_AT] = { { "Spawn Sound at Location", "Audio", "Plays a sound at a place and returns it (to stop or change it later). The sound: in Details.", false },
                             "i:exec:,i:vector:Location,i:float:Volume Multiplier,i:float:Pitch Multiplier,i:float:Start Time,o:exec:,o:int:Sound" },
    [BPK_SPAWN_SOUND_ATTACHED] = { { "Spawn Sound Attached", "Audio", "Plays a sound that moves with an actor (or one of its bones: Attach Point Name in Details) and returns it. The sound: in Details.", false },
                                   "i:exec:,i:actor:Attach To Component,i:float:Volume Multiplier,i:float:Pitch Multiplier,i:float:Start Time,o:exec:,o:int:Sound" },
    [BPK_STOP_SOUND] = { { "Stop Sound", "Audio", "Stops a sound a Spawn Sound node returned (faded out over the duration; 0: at once).", false },
                         "i:exec:,i:int:Sound,i:float:Fade Out Duration,o:exec:" },
    [BPK_SET_SOUND_PAUSED] = { { "Set Sound Paused", "Audio", "Pauses or resumes a sound a Spawn Sound node returned.", false },
                               "i:exec:,i:int:Sound,i:bool:Paused,o:exec:" },
    [BPK_IS_SOUND_PLAYING] = { { "Is Sound Playing", "Audio", "True while the sound a Spawn Sound node returned plays (paused counts).", true },
                               "i:int:Sound,o:bool:Return Value" },
    [BPK_SET_SOUND_VOLUME] = { { "Set Sound Volume", "Audio", "Sets the volume multiplier of a sound a Spawn Sound node returned.", false },
                               "i:exec:,i:int:Sound,i:float:Volume Multiplier,o:exec:" },
    [BPK_SET_SOUND_PITCH] = { { "Set Sound Pitch", "Audio", "Sets the pitch multiplier of a sound a Spawn Sound node returned.", false },
                              "i:exec:,i:int:Sound,i:float:Pitch Multiplier,o:exec:" },
    [BPK_AUDIO_PLAY] = { { "Play (Audio)", "Audio|Components", "Plays the actor's Audio Component (its sound, from Start Time). Target: an actor (its first Audio Component) or the component.", false },
                         "i:exec:,i:actor:Target,i:float:Start Time,o:exec:" },
    [BPK_AUDIO_STOP] = { { "Stop (Audio)", "Audio|Components", "Stops the actor's Audio Component.", false },
                         "i:exec:,i:actor:Target,o:exec:" },
    [BPK_AUDIO_FADE_IN] = { { "Fade In", "Audio|Components", "Plays the actor's Audio Component rising from silence to the volume level over the duration.", false },
                            "i:exec:,i:actor:Target,i:float:Fade In Duration,i:float:Fade Volume Level,i:float:Start Time,o:exec:" },
    [BPK_AUDIO_FADE_OUT] = { { "Fade Out", "Audio|Components", "Fades the actor's Audio Component to the volume level over the duration (0: it stops at the end).", false },
                             "i:exec:,i:actor:Target,i:float:Fade Out Duration,i:float:Fade Volume Level,o:exec:" },
    [BPK_AUDIO_SET_PAUSED] = { { "Set Paused (Audio)", "Audio|Components", "Pauses or resumes the actor's Audio Component.", false },
                               "i:exec:,i:actor:Target,i:bool:Paused,o:exec:" },
    [BPK_AUDIO_SET_VOLUME] = { { "Set Volume Multiplier", "Audio|Components", "The actor's Audio Component's volume multiplier (the sound playing too).", false },
                               "i:exec:,i:actor:Target,i:float:New Volume Multiplier,o:exec:" },
    [BPK_AUDIO_SET_PITCH] = { { "Set Pitch Multiplier", "Audio|Components", "The actor's Audio Component's pitch multiplier (the sound playing too).", false },
                              "i:exec:,i:actor:Target,i:float:New Pitch Multiplier,o:exec:" },
    [BPK_AUDIO_IS_PLAYING] = { { "Is Playing (Audio)", "Audio|Components", "True while the actor's Audio Component plays.", true },
                               "i:actor:Target,o:bool:Return Value" },
    [BPK_AUDIO_SET_SOUND] = { { "Set Sound", "Audio|Components", "Gives the actor's Audio Component another sound (playing: the new one plays). The sound: in Details.", false },
                              "i:exec:,i:actor:Target,o:exec:" },
    [BPK_PUSH_SOUND_MIX] = { { "Push Sound Mix Modifier", "Audio|Mix", "Applies a Sound Mix (its classes' volumes and pitches, faded in): menus, slow motion, ducking. The mix: in Details.", false },
                             "i:exec:,o:exec:" },
    [BPK_POP_SOUND_MIX] = { { "Pop Sound Mix Modifier", "Audio|Mix", "Removes a Sound Mix pushed before (faded out). The mix: in Details.", false },
                            "i:exec:,o:exec:" },
    [BPK_CLEAR_SOUND_MIXES] = { { "Clear Sound Mix Modifiers", "Audio|Mix", "Removes every Sound Mix pushed (the base mix stays).", false },
                                "i:exec:,o:exec:" },
    [BPK_SET_BASE_SOUND_MIX] = { { "Set Base Sound Mix", "Audio|Mix", "The Sound Mix always applied under the pushed ones. The mix: in Details.", false },
                                 "i:exec:,o:exec:" },
    [BPK_SET_CLASS_VOLUME] = { { "Set Sound Class Volume", "Audio|Mix", "Sets a Sound Class's volume and pitch (its children too): the game's volume options (Master, Music, SFX, Voice, UI). The class: in Details.", false },
                               "i:exec:,i:float:Volume,i:float:Pitch,o:exec:" },
    [BPK_ED_SELECTED_COUNT] = { { "Get Selected Actor Count", "Editor Scripting", "How many actors are selected in the editor (Editor Utilities).", true },
                                "o:int:Return Value" },
    [BPK_ED_SELECTED_AT] = { { "Get Selected Actor", "Editor Scripting", "The editor's selected actor at Index (0: the first).", true },
                             "i:int:Index,o:actor:Return Value" },
    [BPK_ED_SELECT] = { { "Select Actor", "Editor Scripting", "Selects the actor in the editor (Add To Selection: kept with the others).", false },
                        "i:exec:,i:actor:Actor,i:bool:Add To Selection,o:exec:" },
    [BPK_PLAY_SEQUENCE] = { { "Play Sequence", "Cinematic", "Plays a Level Sequence (the Sequencer): its tracks move the actors, its Camera Cuts give the view while it plays. The sequence: in Details.", false },
                            "i:exec:,i:bool:Loop,i:float:Play Rate,o:exec:" },
    [BPK_STOP_SEQUENCE] = { { "Stop Sequence", "Cinematic", "Stops a Level Sequence (its actors stay where it left them). None chosen: every sequence playing.", false },
                            "i:exec:,o:exec:" },
    [BPK_IS_SEQUENCE_PLAYING] = { { "Is Sequence Playing", "Cinematic", "True while the Level Sequence plays.", true },
                                  "o:bool:Return Value" },
    // Online (online.h): Unreal's names where it has them
    [BPK_NET_CREATE_SESSION] = { { "Create Session", "Online|Session", "Hosts a game others can join: a listen server (you play too). Use LAN: found on the local network; off: a Steam lobby when Steam is up (friends, invites), else LAN. Event On Session Created tells how it went.", false },
                                 "i:exec:,i:int:Max Players,i:bool:Use LAN,i:string:Session Name,o:exec:" },
    [BPK_NET_FIND_SESSIONS] = { { "Find Sessions", "Online|Session", "Looks for games to join (on the LAN, or Steam's lobbies). Event On Sessions Found gives the count; Get Session Name / Players read each.", false },
                                "i:exec:,i:bool:Use LAN,o:exec:" },
    [BPK_NET_SESSION_COUNT] = { { "Get Session Count", "Online|Session", "How many sessions the last Find Sessions found.", true }, "o:int:Return Value" },
    [BPK_NET_SESSION_NAME] = { { "Get Session Name", "Online|Session", "A found session's name.", true }, "i:int:Index,o:string:Return Value" },
    [BPK_NET_SESSION_PLAYERS] = { { "Get Session Players", "Online|Session", "How many players a found session has.", true }, "i:int:Index,o:int:Return Value" },
    [BPK_NET_JOIN_SESSION] = { { "Join Session", "Online|Session", "Joins a found session. Event On Join Session Complete tells how it went.", false }, "i:exec:,i:int:Index,o:exec:" },
    [BPK_NET_JOIN_ADDRESS] = { { "Join Address", "Online|Session", "Joins a server by its address (\"192.168.0.10:7777\", \"localhost\").", false }, "i:exec:,i:string:Address,o:exec:" },
    [BPK_NET_DESTROY_SESSION] = { { "Destroy Session", "Online|Session", "Leaves the session (a host closes it for everyone).", false }, "i:exec:,o:exec:" },
    [BPK_NET_IS_SERVER] = { { "Is Server", "Online|Session", "True on the server (and when not online): the machine with the authority.", true }, "o:bool:Return Value" },
    [BPK_NET_IS_CLIENT] = { { "Is Client", "Online|Session", "True on a client connected to a server.", true }, "o:bool:Return Value" },
    [BPK_NET_IN_SESSION] = { { "Is In Session", "Online|Session", "True while hosting or connected.", true }, "o:bool:Return Value" },
    [BPK_NET_HAS_AUTHORITY] = { { "Has Authority", "Online|Replication", "True where this actor's state is decided (the server; a client for its own local, unreplicated actors). Use it before changing the game.", true },
                                "i:actor:Target,o:bool:Return Value" },
    [BPK_NET_LOCALLY_CONTROLLED] = { { "Is Locally Controlled", "Online|Replication", "True when this pawn is this machine's player's.", true }, "i:actor:Target,o:bool:Return Value" },
    [BPK_NET_PLAYER_COUNT] = { { "Get Player Count", "Online|Players", "The players in the session (1 when not online).", true }, "o:int:Return Value" },
    [BPK_NET_PLAYER_NAME] = { { "Get Player Name", "Online|Players", "A player's name (Steam's when Steam is up).", true }, "i:int:Player,o:string:Return Value" },
    [BPK_NET_PLAYER_PAWN] = { { "Get Player Pawn", "Online|Players", "A player's pawn (their Character).", true }, "i:int:Player,o:actor:Return Value" },
    [BPK_NET_PING] = { { "Get Ping", "Online|Players", "The round trip to a player (on a client: to the server), in milliseconds.", true }, "i:int:Player,o:float:Return Value" },
    [BPK_NET_PACKET_LOSS] = { { "Get Packet Loss", "Online|Players", "The share of packets lost to a player over the last seconds, in percent.", true }, "i:int:Player,o:float:Return Value" },
    [BPK_NET_LOCAL_PLAYER] = { { "Get Local Player Index", "Online|Players", "This machine's player number.", true }, "o:int:Return Value" },
    [BPK_NET_KICK] = { { "Kick Player", "Online|Players", "The server sends a player away (they get Event On Network Error with the reason).", false }, "i:exec:,i:int:Player,i:string:Reason,o:exec:" },
    [BPK_NET_CHAT] = { { "Send Chat Message", "Online|Players", "A text to every player (Event On Chat Message on each).", false }, "i:exec:,i:string:Message,o:exec:" },
    [BPK_NET_SET_REPLICATES] = { { "Set Replicates", "Online|Replication", "The server sends this actor's movement to the clients (Characters and Simulate Physics bodies do by themselves).", false },
                                 "i:exec:,i:actor:Target,i:bool:Replicates,o:exec:" },
    [BPK_NET_RPC_SERVER] = { { "Run on Server", "Online|Replication", "Runs the Custom Event (Details) on the server: a client asks for an action (fire, open, buy). Its values: Get RPC Float / Vector / Text / Sender.", false },
                             "i:exec:,i:actor:Target,i:float:Float,i:vector:Vector,i:string:Text,i:bool:Reliable,o:exec:" },
    [BPK_NET_RPC_MULTICAST] = { { "Multicast", "Online|Replication", "The server runs the Custom Event (Details) on every machine: effects, sounds, what all must see. Unreliable for what repeats often.", false },
                                "i:exec:,i:actor:Target,i:float:Float,i:vector:Vector,i:string:Text,i:bool:Reliable,o:exec:" },
    [BPK_NET_RPC_OWNER] = { { "Run on Owning Client", "Online|Replication", "The server runs the Custom Event (Details) on the machine of the actor's owner only (its HUD, its camera).", false },
                            "i:exec:,i:actor:Target,i:float:Float,i:vector:Vector,i:string:Text,i:bool:Reliable,o:exec:" },
    [BPK_NET_RPC_FLOAT] = { { "Get RPC Float", "Online|Replication", "The Float the running RPC came with.", true }, "o:float:Return Value" },
    [BPK_NET_RPC_VECTOR] = { { "Get RPC Vector", "Online|Replication", "The Vector the running RPC came with.", true }, "o:vector:Return Value" },
    [BPK_NET_RPC_TEXT] = { { "Get RPC Text", "Online|Replication", "The Text the running RPC came with.", true }, "o:string:Return Value" },
    [BPK_NET_RPC_SENDER] = { { "Get RPC Sender", "Online|Replication", "The player who sent the running RPC.", true }, "o:int:Return Value" },
    [BPK_NET_REPLICATE_VAR] = { { "Replicate Variable", "Online|Replication", "The server sends the variable (Details) to every client; each then runs OnRep_<Name> if the Blueprint has that Custom Event.", false },
                                "i:exec:,i:actor:Target,o:exec:" },
    [BPK_NET_SPAWN] = { { "Spawn Actor (Replicated)", "Online|Replication", "On the server: an actor of Class that every client gets too, owned by a player (-1: the server).", false },
                        "i:exec:,i:class:Class,i:vector:Location,i:rotator:Rotation,i:int:Owner Player,o:exec:,o:actor:Return Value" },
    [BPK_NET_DESTROY] = { { "Destroy Actor (Replicated)", "Online|Replication", "On the server: the actor goes on every machine.", false }, "i:exec:,i:actor:Target,o:exec:" },
    [BPK_NET_EMULATION] = { { "Set Network Emulation", "Online|Debug", "Makes this machine's network worse on purpose to test the game: added latency and jitter (ms), packet loss (%). Zeros: off.", false },
                            "i:exec:,i:float:Latency,i:float:Jitter,i:float:Packet Loss,o:exec:" },
    [BPK_EVENT_NET_PLAYER_JOINED] = { { "Event On Player Joined", "Online|Events", "Runs when a player enters the session (the server and every client).", false }, "o:exec:,o:int:Player,o:string:Name" },
    [BPK_EVENT_NET_PLAYER_LEFT] = { { "Event On Player Left", "Online|Events", "Runs when a player leaves the session.", false }, "o:exec:,o:int:Player,o:string:Name" },
    [BPK_EVENT_NET_SESSION_CREATED] = { { "Event On Session Created", "Online|Events", "Runs when Create Session finished.", false }, "o:exec:,o:bool:Success" },
    [BPK_EVENT_NET_SESSIONS_FOUND] = { { "Event On Sessions Found", "Online|Events", "Runs when Find Sessions finished.", false }, "o:exec:,o:int:Count" },
    [BPK_EVENT_NET_JOINED] = { { "Event On Join Session Complete", "Online|Events", "Runs when joining finished (Error says why it failed).", false }, "o:exec:,o:bool:Success,o:string:Error" },
    [BPK_EVENT_NET_ERROR] = { { "Event On Network Error", "Online|Events", "Runs when the connection was lost or the server sent this machine away.", false }, "o:exec:,o:string:Message" },
    [BPK_EVENT_NET_CHAT] = { { "Event On Chat Message", "Online|Events", "Runs for every chat message (yours too).", false }, "o:exec:,o:int:Player,o:string:Message" },
    // Advanced sessions (online.h): Unreal's Advanced Sessions plugin, its names where it has them
    [BPK_NET_CREATE_ADV] = { { "Create Advanced Session", "Online|Advanced Sessions", "Hosts a game with every setting (Advanced Sessions): public slots (Max Players) and Private Slots (only for who comes by an invite, a friend's session or the address); Use LAN; Visibility 0 public, 1 friends only, 2 invite only; a Password (empty: none); Allow Join In Progress (after Start Session); Allow Invites; Should Advertise (off: Find Sessions doesn't see it); Map Name and Game Mode shown in the searches. Set Session Property before it adds your own keys. Event On Session Created tells how it went.", false },
        "i:exec:,i:string:Session Name,i:int:Max Players,i:int:Private Slots,i:bool:Use LAN,i:int:Visibility,i:string:Password,i:bool:Allow Join In Progress,i:bool:Allow Invites,i:bool:Should Advertise,i:string:Map Name,i:string:Game Mode,o:exec:" },
    [BPK_NET_FIND_ADV] = { { "Find Sessions Advanced", "Online|Advanced Sessions", "Looks for games with filters (Advanced Sessions): Use LAN or Steam; Max Results; Server Type 0 all, 1 listen (a player hosts), 2 dedicated; Min Open Slots; Exclude Full; Empty Only / Non Empty Only (players besides a listen server's host); Hide Passworded; Hide In Progress; Max Ping in ms (0: any; Steam's lobbies have no ping); Same Build Only (the same project version). Add Session Search Filter before it filters by your own properties. The results come nearest first; Event On Sessions Found gives the count, Get Session Details reads each.", false },
        "i:exec:,i:bool:Use LAN,i:int:Max Results,i:int:Server Type,i:int:Min Open Slots,i:bool:Exclude Full,i:bool:Empty Only,i:bool:Non Empty Only,i:bool:Hide Passworded,i:bool:Hide In Progress,i:float:Max Ping,i:bool:Same Build Only,o:exec:" },
    [BPK_NET_ADD_FILTER] = { { "Add Session Search Filter", "Online|Advanced Sessions", "A filter for the next Find Sessions Advanced: the session's property Key compared with Value. Comparison 0 equal, 1 not equal, 2 less, 3 less or equal, 4 greater, 5 greater or equal, 6 contains. Numbers compare as numbers (\"10\" > \"9\"); keys name, map, game_mode and host read the session's own fields. Up to 8; they stay until Clear Session Search Filters.", false },
        "i:exec:,i:string:Key,i:string:Value,i:int:Comparison,o:exec:,o:bool:Success" },
    [BPK_NET_CLEAR_FILTERS] = { { "Clear Session Search Filters", "Online|Advanced Sessions", "Removes every filter Add Session Search Filter added.", false },
        "i:exec:,o:exec:" },
    [BPK_NET_SET_PROP] = { { "Set Session Property", "Online|Advanced Sessions", "A key and value of the session's own (Advanced Sessions' Extra Settings: region, difficulty, round...). In a session, on the host, it changes now: the players and the searches see it (Event On Session Updated). Out of one it waits for the next Create Advanced Session. Up to 16.", false },
        "i:exec:,i:string:Key,i:string:Value,o:exec:,o:bool:Success" },
    [BPK_NET_REMOVE_PROP] = { { "Remove Session Property", "Online|Advanced Sessions", "Removes a property Set Session Property made (the host, or before creating).", false },
        "i:exec:,i:string:Key,o:exec:,o:bool:Success" },
    [BPK_NET_GET_PROP] = { { "Get Current Session Property", "Online|Advanced Sessions", "A property of the session you are in (the clients have the host's copy); out of one, the value waiting for the next Create Advanced Session.", true },
        "i:string:Key,o:string:Value,o:bool:Found" },
    [BPK_NET_RESULT_PROP] = { { "Get Session Property", "Online|Advanced Sessions", "A property of a found session (Find Sessions Advanced). Keys name, map, game_mode and host read its own fields.", true },
        "i:int:Index,i:string:Key,o:string:Value,o:bool:Found" },
    [BPK_NET_LAG_TRACE] = { { "Lag Compensated Trace", "Online|Replication", "A shot tested on the server against where the shooter saw the other players (their round trip and their view's delay rewound): fair hits under lag. Shooter: the player who fired (Get Local Player Index on their side, sent with Run on Server). Radius 0: a line. Characters are their capsules.", true },
        "i:vector:Start,i:vector:End,i:int:Shooter,i:float:Radius,o:bool:Hit,o:actor:Hit Actor,o:vector:Hit Location" },
    [BPK_NET_RESULT_INFO] = { { "Get Session Details", "Online|Advanced Sessions", "Everything about a found session: its name, the host's name, players, max players, open slots, ping (ms; -1 on Steam), map, game mode, whether it is a dedicated server, has a password, already started, and is on the LAN.", true },
        "i:int:Index,o:string:Session Name,o:string:Host Name,o:int:Players,o:int:Max Players,o:int:Open Slots,o:float:Ping,o:string:Map Name,o:string:Game Mode,o:bool:Is Dedicated,o:bool:Has Password,o:bool:In Progress,o:bool:Is LAN" },
    [BPK_NET_JOIN_ADV] = { { "Join Session Advanced", "Online|Advanced Sessions", "Joins a found session with its password (empty when it has none). Refused (full, locked, started, wrong password, banned, another version): Event On Join Session Complete with the reason, and Get Last Session Error.", false },
        "i:exec:,i:int:Index,i:string:Password,o:exec:" },
    [BPK_NET_JOIN_ADDR_ADV] = { { "Join Address Advanced", "Online|Advanced Sessions", "Joins a server by its address with a password; who comes by the address may take a private slot.", false },
        "i:exec:,i:string:Address,i:string:Password,o:exec:" },
    [BPK_NET_UPDATE_SESSION] = { { "Update Session", "Online|Advanced Sessions", "The host changes the session while it runs: name, public and private slots, password, Should Advertise, Allow Join In Progress, map and game mode. The players and the searches see it (Event On Session Updated). The properties stay (Set Session Property changes them).", false },
        "i:exec:,i:string:Session Name,i:int:Max Players,i:int:Private Slots,i:string:Password,i:bool:Should Advertise,i:bool:Allow Join In Progress,i:string:Map Name,i:string:Game Mode,o:exec:,o:bool:Success" },
    [BPK_NET_START_SESSION] = { { "Start Session", "Online|Advanced Sessions", "The host says the match began (state In Progress): without Allow Join In Progress nobody else comes in, and the searches can hide it. Event On Session State Changed on every machine.", false },
        "i:exec:,o:exec:,o:bool:Success" },
    [BPK_NET_END_SESSION] = { { "End Session", "Online|Advanced Sessions", "The host ends the match (state Ended): players may come in again. Start Session begins the next.", false },
        "i:exec:,o:exec:,o:bool:Success" },
    [BPK_NET_SET_JOINABLE] = { { "Set Session Joinable", "Online|Advanced Sessions", "The host locks (off) or opens (on) the session: locked, nobody else comes in and the searches don't list it.", false },
        "i:exec:,i:bool:Joinable,o:exec:,o:bool:Success" },
    [BPK_NET_SESSION_STATE] = { { "Get Session State", "Online|Advanced Sessions", "The session's state: 0 none, 1 creating, 2 pending (the lobby), 3 in progress (after Start Session), 4 ended.", true },
        "o:int:Return Value" },
    [BPK_NET_SESSION_SETTINGS] = { { "Get Current Session Settings", "Online|Advanced Sessions", "The settings of the session you are in (the host's; a client's copy of them): name, slots, players, LAN, password (only whether it has one), joinable, Should Advertise, Allow Join In Progress, Allow Invites, map and game mode.", true },
        "o:string:Session Name,o:int:Max Players,o:int:Private Slots,o:int:Players,o:bool:Is LAN,o:bool:Has Password,o:bool:Joinable,o:bool:Should Advertise,o:bool:Allow Join In Progress,o:bool:Allow Invites,o:string:Map Name,o:string:Game Mode" },
    [BPK_NET_BAN] = { { "Ban Player", "Online|Advanced Sessions", "The host sends a player away and refuses them when they come back (their Steam ID, else their address) until Unban.", false },
        "i:exec:,i:int:Player,i:string:Reason,o:exec:,o:bool:Success" },
    [BPK_NET_UNBAN] = { { "Unban Player", "Online|Advanced Sessions", "Takes a ban off (Index: 0 .. Get Banned Player Count - 1).", false },
        "i:exec:,i:int:Index,o:exec:" },
    [BPK_NET_UNBAN_ALL] = { { "Unban All Players", "Online|Advanced Sessions", "Takes every ban off.", false },
        "i:exec:,o:exec:" },
    [BPK_NET_BAN_COUNT] = { { "Get Banned Player Count", "Online|Advanced Sessions", "How many players the host banned.", true },
        "o:int:Return Value" },
    [BPK_NET_BAN_NAME] = { { "Get Banned Player Name", "Online|Advanced Sessions", "A banned player's name.", true },
        "i:int:Index,o:string:Return Value" },
    [BPK_NET_PLAYER_ID] = { { "Get Player Unique ID", "Online|Advanced Sessions", "A player's id that stays between sessions: \"steam:<SteamID>\", or \"lan:<name>\" without Steam.", true },
        "i:int:Player,o:string:Return Value" },
    [BPK_NET_LAST_ERROR] = { { "Get Last Session Error", "Online|Advanced Sessions", "Why the last create, search or join failed (empty when it didn't): for the menu's message.", true },
        "o:string:Return Value" },
    [BPK_NET_FRIEND_IN_SESSION] = { { "Is Friend In Session", "Online|Advanced Sessions", "True when the Steam friend (Get Friend Count's index) is playing this game in a session you can join.", true },
        "i:int:Friend,o:bool:Return Value" },
    [BPK_NET_JOIN_FRIEND] = { { "Join Friend Session", "Online|Advanced Sessions", "Joins the session a Steam friend is in (Password: when it has one). Event On Join Session Complete tells how it went.", false },
        "i:exec:,i:int:Friend,i:string:Password,o:exec:,o:bool:Success" },
    [BPK_NET_INVITE_FRIEND] = { { "Invite Friend To Session", "Online|Advanced Sessions", "Sends a Steam friend an invite to your session (when Allow Invites is on).", false },
        "i:exec:,i:int:Friend,o:exec:,o:bool:Success" },
    [BPK_NET_AUTO_INVITES] = { { "Set Auto Accept Invites", "Online|Advanced Sessions", "On (the default): an invite accepted in the Steam overlay joins at once. Off: only Event On Session Invite Accepted runs, and Accept Session Invite joins when the game is ready.", false },
        "i:exec:,i:bool:Accept,o:exec:" },
    [BPK_NET_ACCEPT_INVITE] = { { "Accept Session Invite", "Online|Advanced Sessions", "Joins the session of the last invite accepted in the Steam overlay (with Set Auto Accept Invites off).", false },
        "i:exec:,o:exec:,o:bool:Success" },
    [BPK_NET_IS_FINDING] = { { "Is Finding Sessions", "Online|Advanced Sessions", "True while a search runs (show a spinner).", true },
        "o:bool:Return Value" },
    [BPK_EVENT_NET_SESSION_UPDATED] = { { "Event On Session Updated", "Online|Events", "Runs when the session's settings or properties changed (the host; every client, also when it arrives).", false },
        "o:exec:,o:string:Session Name" },
    [BPK_EVENT_NET_SESSION_STATE] = { { "Event On Session State Changed", "Online|Events", "Runs on every machine when Start / End Session changed the state (3 in progress, 4 ended).", false },
        "o:exec:,o:int:State" },
    [BPK_EVENT_NET_INVITE] = { { "Event On Session Invite Accepted", "Online|Events", "Runs when the player accepted a friend's invite in the Steam overlay: From is the friend; Joining is false with Set Auto Accept Invites off (Accept Session Invite joins).", false },
        "o:exec:,o:string:From,o:bool:Joining" },
    [BPK_EVENT_NET_JOIN_REFUSED] = { { "Event On Join Refused", "Online|Events", "Runs on the host when someone couldn't come in: their name and why (wrong password, full, banned, locked, started).", false },
        "o:exec:,o:string:Name,o:string:Reason" },
    [BPK_PS_SPAWN] = { { "Spawn System at Location", "Effects|Particles", "Plays a Particle System (Details) at a place, turned by Rotation; returns its actor. Auto Destroy: the actor goes when its particles are done.", false },
        "i:exec:,i:vector:Location,i:rotator:Rotation,i:bool:Auto Destroy,o:exec:,o:actor:Return Value" },
    [BPK_PS_ACTIVATE] = { { "Activate System", "Effects|Particles", "Starts a Particle System actor's emitters again (Reset: from the start, the old particles gone).", false },
        "i:exec:,i:actor:Target,i:bool:Reset,o:exec:" },
    [BPK_PS_DEACTIVATE] = { { "Deactivate System", "Effects|Particles", "Stops the births of a Particle System actor; its particles live out their lives.", false },
        "i:exec:,i:actor:Target,o:exec:" },
    [BPK_PS_SET_COLOR] = { { "Set Particle Color", "Effects|Particles", "Tints a Particle System actor's particles (times their own colors).", false },
        "i:exec:,i:actor:Target,i:color:Color,o:exec:" },
    [BPK_PS_SET_SPAWN] = { { "Set Spawn Rate Scale", "Effects|Particles", "Multiplies the spawn rates and bursts of a Particle System actor (0 stops them).", false },
        "i:exec:,i:actor:Target,i:float:Scale,o:exec:" },
    [BPK_PHYS_SET_SIMULATE] = { { "Set Simulate Physics (Ragdoll)", "Animation|Physics", "Turns a Skeletal Mesh's Physics Asset bodies into a ragdoll (on) or gives the pose back to the animation (off). Target: the actor or its Skeletal Mesh.", false },
        "i:exec:,i:actor:Target,i:bool:Simulate,o:exec:" },
    [BPK_PHYS_SET_BELOW] = { { "Set All Bodies Below Simulate Physics", "Animation|Physics", "Simulates only the bodies from a bone down (a tail, hair, a hanging arm, a hit limb); the rest keeps the animation.", false },
        "i:exec:,i:actor:Target,i:string:Bone Name,i:bool:Simulate,o:exec:" },
    [BPK_PHYS_SET_BLEND] = { { "Set Physics Blend Weight", "Animation|Physics", "How much of the pose the simulated bodies give: 0 the animation, 1 the physics (blend it down to stand back up).", false },
        "i:exec:,i:actor:Target,i:float:Weight,o:exec:" },
    [BPK_PHYS_IMPULSE] = { { "Add Impulse to Bone", "Animation|Physics", "Pushes a simulated body (cm/s added to its speed); no bone name: every simulated body. A hit, a shot, an explosion.", false },
        "i:exec:,i:actor:Target,i:vector:Impulse,i:string:Bone Name,o:exec:" },
    [BPK_PHYS_IS_SIMULATING] = { { "Is Simulating Physics", "Animation|Physics", "True while the Skeletal Mesh's bodies simulate (ragdoll or below a bone).", true },
        "i:actor:Target,o:bool:Return Value" },
    [BPK_IK_SET_GOAL] = { { "Set IK Goal", "Animation|IK Rig", "Moves an IK Rig goal (its name in the rig) to a place in the world; Alpha 0..1 how far the bones reach it. Hands on a wall, feet on steps, a head looking.", false },
        "i:exec:,i:actor:Target,i:string:Goal,i:vector:Location,i:float:Alpha,o:exec:" },
    [BPK_IK_CLEAR_GOAL] = { { "Clear IK Goal", "Animation|IK Rig", "Releases an IK Rig goal (no name: all of them): the bones go back to the animation.", false },
        "i:exec:,i:actor:Target,i:string:Goal,o:exec:" },
    [BPK_VEH_THROTTLE] = { { "Set Throttle Input", "Vehicles", "The vehicle's throttle, -1..1 (negative: brakes, then backs up once stopped). An airplane's engine lever.", false },
        "i:exec:,i:actor:Target,i:float:Throttle,o:exec:" },
    [BPK_VEH_STEER] = { { "Set Steering Input", "Vehicles", "The vehicle's steering, -1 (left) .. 1 (right): its wheels, a tank's tracks, a boat's rudder.", false },
        "i:exec:,i:actor:Target,i:float:Steering,o:exec:" },
    [BPK_VEH_BRAKE] = { { "Set Brake Input", "Vehicles", "The vehicle's brakes, 0..1 (only brakes: Set Throttle Input -1 backs up).", false },
        "i:exec:,i:actor:Target,i:float:Brake,o:exec:" },
    [BPK_VEH_HANDBRAKE] = { { "Set Handbrake Input", "Vehicles", "The handbrake (the wheels with a Handbrake torque: the rear ones).", false },
        "i:exec:,i:actor:Target,i:bool:Handbrake,o:exec:" },
    [BPK_VEH_FLIGHT] = { { "Set Flight Input", "Vehicles", "An aircraft's stick and pedals, -1..1: Pitch (nose up +), Roll (right +), Yaw (right +); Collective: a helicopter's lift (0 hovers when its rotor has Stability).", false },
        "i:exec:,i:actor:Target,i:float:Pitch,i:float:Roll,i:float:Yaw,i:float:Collective,o:exec:" },
    [BPK_VEH_GEAR] = { { "Set Gear", "Vehicles", "A gear (-1 reverse, 0 neutral, 1..): the gearbox becomes manual.", false },
        "i:exec:,i:actor:Target,i:int:Gear,o:exec:" },
    [BPK_VEH_ENGINE] = { { "Set Engine On", "Vehicles", "Starts or stops the vehicle's engine.", false },
        "i:exec:,i:actor:Target,i:bool:On,o:exec:" },
    [BPK_VEH_PART_SET] = { { "Set Part Position", "Vehicles", "Moves a part (a door, a hood, a ramp, a slider, a piston) toward 0 (closed) .. 1 (open) at its speed.", false },
        "i:exec:,i:actor:Target,i:string:Part,i:float:Position,o:exec:" },
    [BPK_VEH_PART_TOGGLE] = { { "Toggle Part", "Vehicles", "Opens a closed part or closes an open one (a door).", false },
        "i:exec:,i:actor:Target,i:string:Part,o:exec:" },
    [BPK_VEH_PART_GET] = { { "Get Part Position", "Vehicles", "Where a part is now, 0 (closed) .. 1 (open).", true },
        "i:actor:Target,i:string:Part,o:float:Return Value" },
    [BPK_VR_ACTIVE] = { { "Is VR Active", "VR", "True while the game runs on a VR headset (or its simulated mode).", true }, "o:bool:Return Value" },
    [BPK_VR_HEAD] = { { "Get HMD Pose", "VR", "The headset's place (cm, from the play space's origin under the player) and where it looks.", true },
        "o:vector:Location,o:vector:Forward,o:vector:Up" },
    [BPK_VR_HAND] = { { "Get Motion Controller Pose", "VR", "A controller's place and direction (Hand: 0 left, 1 right); Tracked false when it is off or out of sight.", true },
        "i:int:Hand,o:vector:Location,o:vector:Forward,o:bool:Tracked" },
    [BPK_VR_HAND_INPUT] = { { "Get Motion Controller Input", "VR", "A controller's trigger and grip (0..1), thumbstick (-1..1) and buttons (A or X, B or Y).", true },
        "i:int:Hand,o:float:Trigger,o:float:Grip,o:vector:Thumbstick,o:bool:Primary,o:bool:Secondary" },
    [BPK_VR_HAPTIC] = { { "Play Haptic Effect", "VR", "Vibrates a controller (Hand: 0 left, 1 right) at Amplitude (0..1) for Duration seconds.", false },
        "i:exec:,i:int:Hand,i:float:Amplitude,i:float:Duration,o:exec:" },
    [BPK_AR_TRACKING] = { { "Is AR Tracking", "AR", "True while ARCore follows the phone in the real world (the camera is the phone's; false while it searches, without ARCore, or on a PC).", true }, "o:bool:Return Value" },
    [BPK_AR_HIT] = { { "AR Line Trace", "AR", "A point of the screen (pixels) onto the real world: the nearest plane ARCore found under it (a floor, a table, a wall), its place (cm) and normal. To place something where the player taps.", true },
        "i:float:Screen X,i:float:Screen Y,o:bool:Hit,o:vector:Location,o:vector:Normal" },
    [BPK_EVENT_TOUCH] = { { "Event Touch", "Input|Touch", "Runs when a finger touches the screen (Pressed) and when it leaves (Released): where (Location: pixels, x right, y down, z 0) and which finger (0 the first). Not for the fingers a touch widget took (a joystick, a touch button). On a PC the left mouse button is a finger: testable without a phone.", false },
        "o:exec:Pressed,o:exec:Released,o:vector:Location,o:int:Finger Index" },
    [BPK_TAG_ADD] = { { "Add Gameplay Tag", "Gameplay Tags", "Gives the actor a Gameplay Tag (\"State.Stunned\", \"Team.Red\"; the dots make a hierarchy). Its Gameplay Tags component is made when missing; a tag it has stays once.", false },
                      "i:exec:,i:actor:Target,i:name:Tag,o:exec:" },
    [BPK_TAG_REMOVE] = { { "Remove Gameplay Tag", "Gameplay Tags", "Takes the tag (exactly that name) away from the actor.", false }, "i:exec:,i:actor:Target,i:name:Tag,o:exec:" },
    [BPK_TAG_HAS] = { { "Has Matching Gameplay Tag", "Gameplay Tags", "Whether the actor has the tag or one under it: tagged \"Damage.Fire\" it has \"Damage\".", true },
                      "i:actor:Target,i:name:Tag,o:bool:" },
    [BPK_TAG_HAS_EXACT] = { { "Has Gameplay Tag (Exact)", "Gameplay Tags", "Whether the actor has exactly this tag (\"Damage\" isn't \"Damage.Fire\").", true }, "i:actor:Target,i:name:Tag,o:bool:" },
    [BPK_TAG_MATCHES] = { { "Matches Tag", "Gameplay Tags", "Whether Tag A is Tag B or under it (\"Damage.Fire\" matches \"Damage\"); Exact Match: only the same name.", true },
                          "i:name:Tag A,i:name:Tag B,i:bool:Exact Match,o:bool:" },
    [BPK_FORMAT_TEXT] = { { "Format Text", "Utilities|Text", "The Format with {0} {1} {2} {3} replaced by the inputs (any value: a number or a vector wired in turns to text). \"Score: {0} / {1}\". {{ writes a brace.", true },
                          "i:text:Format,i:string:{0},i:string:{1},i:string:{2},i:string:{3},o:text:" },
    [BPK_CONSTRAINT_BREAK] = { { "Break Constraint", "Physics|Constraint", "Breaks the Physics Constraint on the Target actor: its bodies go free.", false }, "i:exec:,i:actor:Target,o:exec:" },
    [BPK_PHYS_ADD_FORCE] = { { "Add Force", "Physics", "Pushes a simulating actor over this frame (kg cm/s2; Accel Change: cm/s2 whatever its mass). Every frame in a Tick: a steady push, a thruster, wind.", false },
                             "i:exec:,i:actor:Target,i:vector:Force,i:bool:Accel Change,o:exec:" },
    [BPK_PHYS_ADD_TORQUE] = { { "Add Torque in Degrees", "Physics", "Turns a simulating actor over this frame around the world's X, Y, Z (kg cm2/s2; Accel Change: deg/s2 whatever its mass).", false },
                              "i:exec:,i:actor:Target,i:vector:Torque,i:bool:Accel Change,o:exec:" },
    [BPK_PHYS_ANG_IMPULSE] = { { "Add Angular Impulse in Degrees", "Physics", "Spins a simulating actor at once (Vel Change: degrees a second added around X, Y, Z, whatever its mass).", false },
                               "i:exec:,i:actor:Target,i:vector:Impulse,i:bool:Vel Change,o:exec:" },
    [BPK_PHYS_IMPULSE_AT] = { { "Add Impulse at Location", "Physics", "Pushes a simulating actor at a point of the world (kg cm/s): off its middle it spins too. A shot, a kick on a corner.", false },
                              "i:exec:,i:actor:Target,i:vector:Impulse,i:vector:Location,o:exec:" },
    [BPK_PHYS_SET_LIN_VEL] = { { "Set Physics Linear Velocity", "Physics", "A simulating actor's velocity (cm/s), or added to it (Add to Current).", false },
                               "i:exec:,i:actor:Target,i:vector:New Vel,i:bool:Add to Current,o:exec:" },
    [BPK_PHYS_SET_ANG_VEL] = { { "Set Physics Angular Velocity in Degrees", "Physics", "A simulating actor's spin (degrees a second around X, Y, Z), or added to it.", false },
                               "i:exec:,i:actor:Target,i:vector:New Ang Vel,i:bool:Add to Current,o:exec:" },
    [BPK_PHYS_GET_ANG_VEL] = { { "Get Physics Angular Velocity in Degrees", "Physics", "A simulating actor's spin (degrees a second around X, Y, Z).", true }, "i:actor:Target,o:vector:Return Value" },
    [BPK_PHYS_SET_MASS] = { { "Set Mass Override in Kg", "Physics", "A simulating actor's mass (kg; 0: from its volume and Physical Material).", false },
                            "i:exec:,i:actor:Target,i:float:Mass in Kg,o:exec:" },
    [BPK_PHYS_GET_MASS] = { { "Get Mass", "Physics", "A simulating actor's mass (kg), set or from its volume and material.", true }, "i:actor:Target,o:float:Return Value" },
    [BPK_PHYS_WAKE] = { { "Wake Rigid Body", "Physics", "Wakes a sleeping body (and the pile it sleeps with).", false }, "i:exec:,i:actor:Target,o:exec:" },
    [BPK_PHYS_SLEEP] = { { "Put Rigid Body to Sleep", "Physics", "Stops a body where it is until something touches or pushes it.", false }, "i:exec:,i:actor:Target,o:exec:" },
    [BPK_PHYS_IS_AWAKE] = { { "Rigid Body Is Awake", "Physics", "Whether a simulating actor moves (asleep: it costs nothing until touched).", true }, "i:actor:Target,o:bool:Return Value" },
    [BPK_PHYS_SET_GRAVITY] = { { "Set Enable Gravity", "Physics", "Whether gravity pulls the simulating actor (off: it floats, a pushed ball in space).", false },
                               "i:exec:,i:actor:Target,i:bool:Gravity Enabled,o:exec:" },
    [BPK_PHYS_SET_MATERIAL] = { { "Set Physical Material", "Physics", "The actor's Physical Material (/Content/.../Ice.rphysmat: friction, bounce, density; empty: its own values).", false },
                                "i:exec:,i:actor:Target,i:string:Material,o:exec:" },
    [BPK_PHYS_SET_CCD] = { { "Set Use CCD", "Physics", "Continuous collision: the body is swept each step and never goes through a thin wall (bullets, fast balls).", false },
                           "i:exec:,i:actor:Target,i:bool:Enable,o:exec:" },
    [BPK_CONSTRAINT_LIN_DRIVE] = { { "Set Linear Drive", "Physics|Constraint", "The constraint's linear drive: body 1 pulled toward Position Target and Velocity Target (cm, cm/s in the constraint's frame) by a spring of Frequency (Hz) and Damping (1: no bounce); Max Force 0: no limit.", false },
                                   "i:exec:,i:actor:Target,i:vector:Position Target,i:vector:Velocity Target,i:bool:Position Drive,i:bool:Velocity Drive,i:float:Frequency,i:float:Damping,i:float:Max Force,o:exec:" },
    [BPK_CONSTRAINT_ANG_DRIVE] = { { "Set Angular Drive", "Physics|Constraint", "The constraint's angular (SLERP) drive: body 1 turned toward Orientation Target (pitch, yaw, roll) and Angular Velocity Target (deg/s around X, Y, Z) by a spring of Frequency and Damping; Max Torque 0: no limit.", false },
                                   "i:exec:,i:actor:Target,i:vector:Orientation Target,i:vector:Angular Velocity Target,i:bool:Orientation Drive,i:bool:Velocity Drive,i:float:Frequency,i:float:Damping,i:float:Max Torque,o:exec:" },
    [BPK_CONSTRAINT_LIMITS] = { { "Set Constraint Limits", "Physics|Constraint", "The constraint's limits: Linear (cm, the 6DOF's limited axes and a slide's), Twist (around X), Swing 1 and Swing 2 (the cone; a hinge's limit is its Twist), in degrees.", false },
                                "i:exec:,i:actor:Target,i:float:Linear Limit,i:float:Twist Limit,i:float:Swing 1 Limit,i:float:Swing 2 Limit,o:exec:" },
    [BPK_CONSTRAINT_MOTION] = { { "Set Constraint Axis Motion", "Physics|Constraint", "A 6DOF constraint's axis (0-2: linear X, Y, Z; 3: twist; 4: swing 1; 5: swing 2) Free (0), Limited (1) or Locked (2).", false },
                                "i:exec:,i:actor:Target,i:int:Axis,i:int:Motion,o:exec:" },
    [BPK_CONSTRAINT_FORCE] = { { "Get Constraint Force", "Physics|Constraint", "What the constraint held last step: its pull (kg cm/s2) and twist (kg cm2/s2), and whether it broke.", true },
                               "i:actor:Target,o:float:Force,o:float:Torque,o:bool:Broken" },
    [BPK_CONSTRAINT_MOTOR] = { { "Set Constraint Motor", "Physics|Constraint", "Turns the constraint's motor on or off and sets its speed (a hinge: degrees a second around its X; a slide: cm a second along it).", false },
                               "i:exec:,i:actor:Target,i:bool:Enable,i:float:Speed,o:exec:" },
    [BPK_NAV_PROJECT] = { { "Project Point to Navigation", "AI|Navigation", "The NavMesh's nearest point to Point, looked for within Query Extent each way (0: 150 cm across, 300 cm up and down). Return Value false: nothing walkable that near (Projected Location is then Point).", true },
                          "i:vector:Point,i:vector:Query Extent,o:vector:Projected Location,o:bool:Return Value" },
    [BPK_NAV_RANDOM_REACHABLE] = { { "Get Random Reachable Point in Radius", "AI|Navigation", "A random point of the NavMesh an AI can walk to from Origin, within Radius (cm) along the mesh: never behind a wall it can't go round. Return Value false: no navigation there.", true },
                                   "i:vector:Origin,i:float:Radius,o:vector:Random Location,o:bool:Return Value" },
    [BPK_NAV_PATH_LENGTH] = { { "Get Path Length", "AI|Navigation", "How far the walk from Path Start to Path End is along the NavMesh (cm: round the walls, over the Nav Links). Return Value false: no way there (Path Length -1).", true },
                              "i:vector:Path Start,i:vector:Path End,o:float:Path Length,o:bool:Return Value" },
    [BPK_MORPH_SET] = { { "Set Morph Target", "Components|Skeletal Mesh", "Sets a morph target (blend shape) of the Target's Skeletal Mesh by its name: 0 none, 1 the whole shape (a smile, a blink, a muscle). Bent on the GPU the next frame.", false },
                        "i:exec:,i:actor:Target,i:name:Morph Target Name,i:float:Value,o:exec:" },
    [BPK_MORPH_GET] = { { "Get Morph Target", "Components|Skeletal Mesh", "The weight of a morph target of the Target's Skeletal Mesh by its name (its default when not set; 0 when there is no such target).", true },
                        "i:actor:Target,i:name:Morph Target Name,o:float:Return Value" },
    // Health and damage (health.h, notas-dev/saude-dano.md): the actor's Health component (Details > Add > Health)
    [BPK_APPLY_DAMAGE] = { { "Apply Damage", "Game|Damage", "Damages the Target's Health (none: self): Amount times its multiplier for the Damage Type (a Gameplay Tag, \"Damage.Fire\"), the shield first. Its Event AnyDamage runs with the Instigator and the Damage Causer. Return Value: the damage it took (0: no Health, dead, invincible, immune).", false },
                           "i:exec:,i:actor:Target,i:float:Amount,i:name:Damage Type,i:actor:Instigator,i:actor:Damage Causer,o:exec:,o:float:Return Value" },
    [BPK_APPLY_RADIAL_DAMAGE] = { { "Apply Radial Damage", "Game|Damage", "An explosion: every Health within Damage Radius (cm) of Origin takes Minimum Damage + (Base Damage - Minimum Damage) x (1 - distance / radius) ^ Falloff (1 linear, 0 the full damage everywhere). Return Value: someone was damaged; Hit Count: how many.", false },
                                  "i:exec:,i:float:Base Damage,i:vector:Origin,i:float:Damage Radius,i:name:Damage Type,i:actor:Instigator,i:actor:Damage Causer,i:float:Falloff,i:float:Minimum Damage,o:exec:,o:bool:Return Value,o:int:Hit Count" },
    [BPK_HEAL] = { { "Heal", "Game|Damage", "Gives the Target health (none: self), up to its Max Health; its On Healed event runs. The dead aren't healed (Revive). Return Value: the health given.", false },
                   "i:exec:,i:actor:Target,i:float:Amount,i:actor:Healer,o:exec:,o:float:Return Value" },
    [BPK_GET_HEALTH] = { { "Get Health", "Game|Damage", "The Target's health now (none: self; no Health component: 0).", true }, "i:actor:Target,o:float:Return Value" },
    [BPK_GET_HEALTH_PERCENT] = { { "Get Health Percent", "Game|Damage", "The Target's health over its Max Health, 0-1: a Progress Bar's Percent.", true }, "i:actor:Target,o:float:Return Value" },
    [BPK_GET_MAX_HEALTH] = { { "Get Max Health", "Game|Damage", "The Target's Max Health.", true }, "i:actor:Target,o:float:Return Value" },
    [BPK_GET_SHIELD] = { { "Get Shield", "Game|Damage", "The Target's shield now, its share of the Max Shield (0-1) and the Max Shield (0: it has none).", true },
                         "i:actor:Target,o:float:Return Value,o:float:Percent,o:float:Max Shield" },
    [BPK_SET_MAX_HEALTH] = { { "Set Max Health", "Game|Damage", "Changes the Target's Max Health; Fill Health: its health to it (else kept, at most the new max). A level up, a stronger boss phase.", false },
                             "i:exec:,i:actor:Target,i:float:Max Health,i:bool:Fill Health,o:exec:" },
    [BPK_SET_SHIELD] = { { "Set Shield", "Game|Damage", "Sets the Target's shield (an armor pickup); Max Shield above 0 changes the most it holds too.", false },
                         "i:exec:,i:actor:Target,i:float:Shield,i:float:Max Shield,o:exec:" },
    [BPK_IS_DEAD] = { { "Is Dead", "Game|Damage", "Whether the Target's health reached 0 (until Revive).", true }, "i:actor:Target,o:bool:Return Value" },
    [BPK_REVIVE] = { { "Revive", "Game|Damage", "Brings the Target back: Health (0: full), the shield full, its ragdoll stopped, the destroy called off.", false },
                     "i:exec:,i:actor:Target,i:float:Health,o:exec:" },
    [BPK_SET_INVINCIBLE] = { { "Set Invincible", "Game|Damage", "No damage on the Target while on (a dodge's frames, a cutscene, god mode).", false },
                             "i:exec:,i:actor:Target,i:bool:Invincible,o:exec:" },
    [BPK_SET_DAMAGE_MULT] = { { "Set Damage Multiplier", "Game|Damage", "How much of a Damage Type the Target takes: 0 immune, 0.5 half, 2 double. It covers the types under it (\"Damage.Fire\" covers \"Damage.Fire.Burning\"). Three types an actor.", false },
                              "i:exec:,i:actor:Target,i:name:Damage Type,i:float:Multiplier,o:exec:" },
    [BPK_EVENT_ANY_DAMAGE] = { { "Event AnyDamage", "Game|Damage", "Runs when this actor's Health takes damage (On Damaged, Take Damage): Damage taken (after its multiplier and the shield), its Damage Type, who did it (Instigated By) and with what (Damage Causer).", false },
                               "o:exec:,o:float:Damage,o:name:Damage Type,o:actor:Instigated By,o:actor:Damage Causer" },
    [BPK_EVENT_HEALED] = { { "Event On Healed", "Game|Damage", "Runs when this actor is healed (Heal): the Amount given and the Healer. Not for the regeneration.", false }, "o:exec:,o:float:Amount,o:actor:Healer" },
    [BPK_EVENT_DEATH] = { { "Event On Death", "Game|Damage", "Runs when this actor's health reaches 0: the Killer (the instigator), the Damage Type and the Damage Causer. The Health's Death then destroys the actor or makes it a ragdoll, or only this runs.", false },
                          "o:exec:,o:actor:Killer,o:name:Damage Type,o:actor:Damage Causer" },
    [BPK_EVENT_SHIELD_BROKEN] = { { "Event On Shield Broken", "Game|Damage", "Runs when the damage empties this actor's shield (its health takes the rest): who broke it.", false }, "o:exec:,o:actor:Instigated By" },
    [BPK_GET_MOUSE_POS] = { { "Get Mouse Position", "Input|Mouse", "Where the mouse is on the screen, or the first finger touching it, in pixels (x right, y down). Return Value false when there is none (a touch screen nobody touches).", true },
        "o:float:Location X,o:float:Location Y,o:bool:Return Value" },
    [BPK_AR_PLANES] = { { "Get AR Plane Count", "AR", "How many real surfaces ARCore found (each one an AR Plane actor that blocks: the Characters walk on the real floor).", true }, "o:int:Return Value" },
    [BPK_CLOTH_TEAR] = { { "Tear Cloth", "Physics|Cloth", "Cuts every Cloth's threads within Radius (cm) of Location: a hole that tears on as the cloth pulls (a blade, a bullet, an explosion).", false },
        "i:exec:,i:vector:Location,i:float:Radius,o:exec:" },
    [BPK_RAIN_SET] = { { "Set Rain Intensity", "Weather|Rain", "How hard it rains now (mm/h: 1 drizzle, 20 moderate, 50 heavy, 100 a storm; 0 stops it). The surfaces dry after their Dry Time.", false },
        "i:exec:,i:float:Intensity,o:exec:" },
    [BPK_RAIN_GET] = { { "Get Rain Intensity", "Weather|Rain", "How hard it rains now (mm/h; 0: dry).", true },
        "o:float:Return Value" },
    [BPK_RAIN_PROP_SET] = { { "Set Rain Property", "Weather|Rain", "Sets any field of the Rain (its name as in the Details: \"Wind Speed\", \"Gusts\", \"Drop Size\", \"Streak Length\", \"Splash Size\", \"Far Rain\", \"Wet Time\", \"Actor Dry Time\", \"Sound Volume\", \"Sound Style\" (0 Open Ground, 1 Roof, 2 Tin Roof, 3 Leaves, 4 Tent, 5 Distant), \"Muffle Under Roof\", \"Thunder\", \"Lightning\"...; spaces and case don't matter). Held in its range. False: no such field.", false },
        "i:exec:,i:name:Property,i:float:Value,o:exec:,o:bool:Return Value" },
    [BPK_RAIN_PROP_GET] = { { "Get Rain Property", "Weather|Rain", "Reads any field of the Rain by its name (as Set Rain Property). 0 with no Rain or no such field.", true },
        "i:name:Property,o:float:Return Value" },
    [BPK_RAIN_PRESET] = { { "Set Rain Preset", "Weather|Rain", "Applies a preset to the Rain, the look and the sound together: Drizzle, Light, Moderate, Heavy, Storm, Monsoon or Stylized. False: no such preset.", false },
        "i:exec:,i:name:Preset,o:exec:,o:bool:Return Value" },
    [BPK_RAIN_DROP_COLOR] = { { "Set Rain Drop Color", "Weather|Rain", "Tints the drops and the splashes (1, 1, 1: clear water lit by the sky; brighter than 1 glows).", false },
        "i:exec:,i:vector:Color,o:exec:" },
    [BPK_ACTOR_WETNESS] = { { "Get Actor Wetness", "Weather|Rain", "How wet a moving actor is (a Character, a Skeletal Mesh, a vehicle, a physics body near the camera): 0 dry .. 1 soaked; -1 when the rain doesn't follow it. Self with no Target.", true },
        "i:actor:Target,o:float:Return Value" },
    [BPK_SET_ACTOR_WETNESS] = { { "Set Actor Wetness", "Weather|Rain", "Sets how wet a moving actor is (0..1): a character out of a lake is soaked at once and dries over the Rain's Actor Dry Time. Self with no Target.", false },
        "i:exec:,i:actor:Target,i:float:Wetness,o:exec:" },
    [BPK_RAIN_SHADING] = { { "Set Rain Shading Style", "Weather|Rain", "How the wet world is shaded: Realistic, Cinematic or Stylized (sets the shading fields; change any after with Set Rain Property: \"Darkening\", \"Water Film\", \"Film Roughness\", \"Puddle Clarity\", \"Puddle Edge\", \"Surface Splashes\", \"Wall Drops\", \"Wall Drop Size\", \"Drop Light\", \"Drop Opacity\", \"Rain Mist\"). False: no such style.", false },
        "i:exec:,i:name:Style,o:exec:,o:bool:Return Value" },
    [BPK_SET_LIGHTING_LEVEL] = { { "Set Lighting Level Visible", "Game|Level Streaming", "Turns a Lighting Level (a sublevel marked Lighting in Window > Levels) on or off while the game runs: its lights and its baked lighting, added over the persistent level's and the other Lighting Levels' that are on. Any mix at once: day and a lamp-lit street, a storm over the night. Loads it when it isn't in. False: no such Lighting Level.", false },
        "i:exec:,i:name:Level Name,i:bool:Visible,o:exec:,o:bool:Return Value" },
    [BPK_GET_LIGHTING_LEVEL] = { { "Is Lighting Level Visible", "Game|Level Streaming", "True when a Lighting Level (Window > Levels) is on: its lights and its baked lighting shown.", true },
        "i:name:Level Name,o:bool:Return Value" },
    [BPK_SET_LIGHTING_BLEND] = { { "Set Lighting Level Blend", "Game|Level Streaming", "A Lighting Level (Window > Levels) at a weight while the game runs: 0 off, 1 as it was baked, between them dimmer, above 1 brighter. Its lights and its baked lighting follow at once on the GPU, so it can change every frame: drive it from a Timeline to fade a room's lamps in, make a sign flicker or a storm's lightning flash. Added over the persistent level's lighting and the other Lighting Levels'. A level not loaded comes in in the background. False: no such Lighting Level.", false },
        "i:exec:,i:name:Level Name,i:float:Blend,o:exec:,o:bool:Return Value" },
    [BPK_GET_LIGHTING_BLEND] = { { "Get Lighting Level Blend", "Game|Level Streaming", "A Lighting Level's weight now (0: off or no such level; 1: as baked).", true },
        "i:name:Level Name,o:float:Return Value" },
    [BPK_ACR_SET] = { { "Set Active Ragdoll", "Animation|Active Ragdoll", "Turns the Skeletal Mesh into an Active Ragdoll (on) or gives it back to the animation (off). As an Active Ragdoll every body is simulated and its muscles keep turning it toward the animation: it walks, idles and plays as animated, but a hit, a push or a fall moves it for real; pushed too hard it stumbles, then falls, and gets up by itself. Any skeleton: people, animals, creatures (Physics Asset needed). Target: the actor or its Skeletal Mesh.", false },
        "i:exec:,i:actor:Target,i:bool:Enabled,o:exec:" },
    [BPK_ACR_STRENGTH] = { { "Set Active Ragdoll Strength", "Animation|Active Ragdoll", "How strongly the muscles follow the animation: 0 limp, 1 normal, above 1 stiff. Bone Name empty: the whole body; else that bone and every bone below it (an arm going slack, a wounded leg). Blend Time: seconds to get there.", false },
        "i:exec:,i:actor:Target,i:string:Bone Name,i:float:Strength,i:float:Blend Time,o:exec:" },
    [BPK_ACR_PROFILE] = { { "Set Muscle Profile", "Animation|Active Ragdoll", "A set of strengths per part of the body: Default, Relaxed, Tense, Protective (arms held up), Drunk (loose, sways), Limp. False: no such profile.", false },
        "i:exec:,i:actor:Target,i:name:Profile,o:exec:,o:bool:Return Value" },
    [BPK_ACR_HIT] = { { "Apply Hit Reaction", "Animation|Active Ragdoll", "A hit on a bone: Impulse (cm/s) pushes its body, and its muscles (and those below it) drop to Weaken (0 limp .. 1 untouched), coming back over Recover Time seconds. A shot in the arm, a punch, a kick; strong enough it knocks the character down.", false },
        "i:exec:,i:actor:Target,i:string:Bone Name,i:vector:Impulse,i:float:Weaken,i:float:Recover Time,o:exec:" },
    [BPK_ACR_BALANCE] = { { "Get Balance State", "Animation|Active Ragdoll", "Where its balance is: 0 Balanced, 1 Stumbling, 2 Fallen, 3 Getting Up (-1: not an Active Ragdoll). Play a get-up sound or montage, stop the AI while it is down.", true },
        "i:actor:Target,o:int:Return Value" },
    [BPK_ACR_IS_BALANCED] = { { "Is Balanced", "Animation|Active Ragdoll", "True while the Active Ragdoll stands (not stumbling, down or getting up).", true },
        "i:actor:Target,o:bool:Return Value" },
    [BPK_ACR_GET_UP] = { { "Get Up", "Animation|Active Ragdoll", "A fallen Active Ragdoll gets up now (blended back into the animation where it lies), without waiting for Auto Get Up.", false },
        "i:exec:,i:actor:Target,o:exec:" },
    [BPK_SOFT_IMPULSE] = { { "Add Soft Body Impulse", "Physics|Soft Body", "Pushes a soft body's vertices within Radius (cm) of Location by Velocity (cm/s), fading toward the edge: a punch in a cushion, a kick to a jelly. Target: the soft body (none: self).", false },
        "i:exec:,i:actor:Target,i:vector:Location,i:float:Radius,i:vector:Velocity,o:exec:" },
    [BPK_SOFT_STIFFNESS] = { { "Set Soft Body Stiffness", "Physics|Soft Body", "Its Young's modulus now (kPa: jelly 3, cushion 12, foam 30, rubber 400): a cushion that hardens, a muscle that tenses.", false },
        "i:exec:,i:actor:Target,i:float:Stiffness,o:exec:" },
    [BPK_SOFT_PINNED] = { { "Set Soft Body Pinned", "Physics|Soft Body", "Holds it (on) or lets go (off) of what it is held by (its Held By band, the bone it follows): a cushion torn off the sofa falls free; on again, it snaps back.", false },
        "i:exec:,i:actor:Target,i:bool:Pinned,o:exec:" },
    [BPK_SOFT_PROP_SET] = { { "Set Soft Body Property", "Physics|Soft Body", "Sets any field of a soft body by its name in the Details (\"Stiffness (Young)\", \"Keep Volume\", \"Soft Damping\", \"Plasticity\", \"Dent Recovery\", \"Soft Density\", \"Friction\", \"Gravity Scale\", \"Hold Stiffness\", \"Push Physics Bodies\", \"Iterations\"...; spaces and case don't matter). False: no such field.", false },
        "i:exec:,i:actor:Target,i:name:Property,i:float:Value,o:exec:,o:bool:Return Value" },
    [BPK_ACR_AUTO_GET_UP] = { { "Set Auto Get Up", "Animation|Active Ragdoll", "Whether a fallen Active Ragdoll gets up by itself after lying still Delay seconds (on, 1.5 s by default). Off: it stays down until Get Up (dead, knocked out).", false },
        "i:exec:,i:actor:Target,i:bool:Enabled,i:float:Delay,o:exec:" },
    [BPK_PHYS_BLEND_BELOW] = { { "Set All Bodies Below Physics Blend Weight", "Animation|Physics", "How much of the pose the simulated bodies from a bone down give (0 the animation, 1 the physics), over Blend Time seconds: an arm let go limp while the rest plays, a ragdoll handed back to the animation part by part. Bone Name empty: every body.", false },
        "i:exec:,i:actor:Target,i:string:Bone Name,i:float:Weight,i:float:Blend Time,o:exec:" },
    [BPK_ACR_FACE_DOWN] = { { "Is Lying Face Down", "Animation|Active Ragdoll", "True while a fallen Active Ragdoll lies face down, false on its back (or not down): pick the get-up montage, a death pose, a sound. The Physics Asset's Get Up Montages are played by themselves.", true },
        "i:actor:Target,o:bool:Return Value" },
    [BPK_DYN_SET] = { { "Set Dynamic Bones Enabled", "Animation|Dynamic Bones", "Turns the Physics Asset's dynamic bones (earrings, hair, a tail, straps, a bag) on or off; off they keep the animation. Turn them off on far characters to save time.", false },
        "i:exec:,i:actor:Target,i:bool:Enabled,o:exec:" },
    [BPK_DYN_WIND] = { { "Set Dynamic Bones Wind", "Animation|Dynamic Bones", "The wind the dynamic bones feel (cm/s, world): a gust, a fan, a helicopter overhead. Each chain takes its Wind share of it.", false },
        "i:exec:,i:actor:Target,i:vector:Wind,o:exec:" },
    [BPK_DYN_RESET] = { { "Reset Dynamic Bones", "Animation|Dynamic Bones", "Starts the dynamic bones over where the animation has them: after a teleport, a cut, a respawn (else they would swing from where they were).", false },
        "i:exec:,i:actor:Target,o:exec:" },
    [BPK_ATTACH_SOCKET] = { { "Attach Actor To Socket", "Utilities|Transformation", "Hangs an actor on a socket (or a bone) of another actor's Skeletal Mesh: it follows the pose every frame - the animation, the ragdoll, the dynamic bones - at the socket's offset (Skeletal Mesh Editor > Skeleton) and Location / Rotation more. A weapon in the hand, a hat, a lantern on the belt. False: no Skeletal Mesh, no such socket or bone.", false },
        "i:exec:,i:actor:Target,i:actor:Parent,i:string:Socket Name,i:vector:Location,i:vector:Rotation,o:exec:,o:bool:Return Value" },
    [BPK_DETACH_SOCKET] = { { "Detach From Socket", "Utilities|Transformation", "Lets an actor go from the socket it hangs on: it stays where it is.", false },
        "i:exec:,i:actor:Target,o:exec:" },
    [BPK_SOCKET_LOCATION] = { { "Get Socket Location", "Utilities|Transformation", "Where a socket (or a bone) of an actor's Skeletal Mesh is now, in the world: a muzzle to shoot from, a hand to spawn an effect at.", true },
        "i:actor:Target,i:string:Socket Name,o:vector:Return Value" },
    [BPK_SOCKET_ROTATION] = { { "Get Socket Rotation", "Utilities|Transformation", "A socket's (or a bone's) rotation now, in the world (pitch, yaw, roll).", true },
        "i:actor:Target,i:string:Socket Name,o:vector:Return Value" },
    // (Q11) the toon and the outline in the game (toon_actor.h)
    [BPK_TOON_SET_PARAM] = { { "Set Toon Parameter", "Rendering|Toon", "Sets a number of the actor's Toon look by its name in the Material's Details (\"Bands\", \"Softness\", \"Shadow Threshold\", \"Specular\", \"Specular Size\", \"Rim\", \"Rim Width\", \"Hatching\", \"Ambient\", \"Outline\" 0/1, \"Outline Width\", \"Near Scale\", \"Far Scale\"...; English or Portuguese, spaces and case don't matter): a hit flash in more bands, a stunned enemy hatched, the outline thicker when selected. The actor gets its own instance (no bake). False: no such field or no material.", false },
        "i:exec:,i:actor:Target,i:name:Parameter,i:float:Value,o:exec:,o:bool:Return Value" },
    [BPK_TOON_SET_COLOR] = { { "Set Toon Color", "Rendering|Toon", "Sets a color of the actor's Toon look by its name (\"Shadow Color\", \"Receive Shadow Color\", \"Specular Color\", \"Rim Color\", \"Hatch Color\", \"Outline Color\"): a poisoned character's green shadows, a red outline on the target. False: no such color.", false },
        "i:exec:,i:actor:Target,i:name:Parameter,i:vector:Color,o:exec:,o:bool:Return Value" },
    [BPK_TOON_GET_PARAM] = { { "Get Toon Parameter", "Rendering|Toon", "A number of the actor's Toon look now, by its name (as Set Toon Parameter's).", true },
        "i:actor:Target,i:name:Parameter,o:float:Return Value" },
    [BPK_SET_ACTOR_OUTLINE] = { { "Set Actor Outline", "Rendering|Toon", "The actor's own outline over its material's: Enabled draws it in Color, Width px at 1440p (0: the material's width; the Auto Adaptive curve stays the material's); off hides it even when its material has one. A Skeletal Mesh's parts take it too: the item you can pick up, the enemy in your sights, the selected unit.", false },
        "i:exec:,i:actor:Target,i:bool:Enabled,i:vector:Color,i:float:Width,o:exec:" },
    [BPK_SET_LIGHT_TOON] = { { "Set Light Toon Mode", "Rendering|Toon", "How a light treats the toon: 0 as each material says (a Toon material in bands), 1 Force Toon (every material it lights in a hard cel step: a stylized moment), 2 Never (a Toon material lit smooth by it: a fill light).", false },
        "i:exec:,i:actor:Target,i:int:Mode,o:exec:" },
    [BPK_FP_SET_EXERTION] = { { "Set First Person Exertion", "Camera|First Person", "How tired the player is (0 rested .. 1 out of breath): the First Person Camera breathes deeper and quicker and its vignette closes in. Eased over a moment. Target: a pawn (none: the player).", false }, // (Q12)
        "i:exec:,i:actor:Target,i:float:Exertion,o:exec:" },
    [BPK_FP_SET_FEAR] = { { "Set First Person Fear", "Camera|First Person", "How afraid the player is (0 calm .. 1 terrified): quicker, shallower breaths, a small tremor, the vignette. Eased over a moment; a horror game sets it from its tension.", false }, // (Q12)
        "i:exec:,i:actor:Target,i:float:Fear,o:exec:" },
    [BPK_FP_GET_EXERTION] = { { "Get First Person Exertion", "Camera|First Person", "The exertion now (0..1, as eased).", true }, // (Q12)
        "i:actor:Target,o:float:Return Value" },
    [BPK_FP_GET_FEAR] = { { "Get First Person Fear", "Camera|First Person", "The fear now (0..1, as eased).", true }, // (Q12)
        "i:actor:Target,o:float:Return Value" },
    [BPK_FP_RECOIL] = { { "Add Camera Recoil", "Camera|First Person", "Kicks the First Person Camera: Pitch and Yaw (degrees: up, right) and Kick (cm back), coming back to rest by its Recoil Return (a shot, a punch taken). The control rotation stays: the view comes back on target.", false }, // (Q12)
        "i:exec:,i:actor:Target,i:float:Pitch,i:float:Yaw,i:float:Kick,o:exec:" },
    [BPK_FP_FOCUS_TARGET] = { { "Set Focus Target", "Camera|First Person", "Keeps an actor in focus (the autofocus follows it instead of the center of the view: a face in a conversation, a clue). None: back to the center.", false }, // (Q12)
        "i:exec:,i:actor:Target,i:actor:Focus Actor,o:exec:" },
    [BPK_FP_FOCUS_DISTANCE] = { { "Get Focus Distance", "Camera|First Person", "Where the autofocus is now (cm; 0: no autofocus).", true }, // (Q12)
        "i:actor:Target,o:float:Return Value" },
    [BPK_FP_PRESET] = { { "Set First Person Camera Preset", "Camera|First Person", "Gives the pawn's camera a preset's values: \"Realistic\", \"Cinematic\", \"Arcade\", \"Horror\". False: no such preset or no camera.", false }, // (Q12)
        "i:exec:,i:actor:Target,i:name:Preset,o:exec:,o:bool:Return Value" },
    [BPK_SHAKE_START] = { { "Start Camera Shake", "Camera|Camera Shake", "Plays a Camera Shake on the player's view (an asset's name, \"CS_Explosion\", or a built-in one: Explosion, Earthquake, Handheld, Hit, Gunshot, Footstep, Heartbeat, Dread) at Scale. Returns its handle for Stop Camera Shake.", false }, // (Q12)
        "i:exec:,i:name:Shake,i:float:Scale,o:exec:,o:int:Return Value" },
    [BPK_SHAKE_STOP] = { { "Stop Camera Shake", "Camera|Camera Shake", "Stops a shake by its handle: blending out, or at once.", false }, // (Q12)
        "i:exec:,i:int:Handle,i:bool:Immediately,o:exec:" },
    [BPK_SHAKE_STOP_ALL] = { { "Stop All Camera Shakes", "Camera|Camera Shake", "Stops every shake playing: blending out, or at once.", false }, // (Q12)
        "i:exec:,i:bool:Immediately,o:exec:" },
    [BPK_SHAKE_WORLD] = { { "Play World Camera Shake", "Camera|Camera Shake", "A shake from a place (an explosion, a giant's step): whole within Inner Radius of the Epicenter, fading to nothing at Outer Radius (Falloff: the curve's exponent), as far as the player's view is. Returns its handle (0: too far).", false }, // (Q12)
        "i:exec:,i:name:Shake,i:vector:Epicenter,i:float:Inner Radius,i:float:Outer Radius,i:float:Falloff,o:exec:,o:int:Return Value" },
    [BPK_HIDE_BONE] = { { "Hide Bone by Name", "Animation", "Hides a bone of the actor's Skeletal Mesh and the ones below it (shrunk to its root: skin and parts), or shows it again: a first-person body's head, a helmet's hair.", false }, // (Q12)
        "i:exec:,i:actor:Target,i:string:Bone Name,i:bool:Hidden,o:exec:" },
    // --- (Q13) the horror kit (terror.h) ---
    [BPK_SET_TENSION] = { { "Set Tension", "Rendering|Horror", "The Tension Director's target (0 calm .. 1 terror), reached in Blend Time seconds (-1: the Post Process Volume's Rise / Fall rates). It drives the grain, the vignette, the color, the letterbox, the field of view, a heartbeat pulse, the lamps' flicker and the tension's sound by the volume's curves (Details > Horror). Raise it as the monster nears, drop it in a safe room.", false },
        "i:exec:,i:float:Tension,i:float:Blend Time,o:exec:" },
    [BPK_ADD_TENSION] = { { "Add Tension", "Rendering|Horror", "Adds to the Tension Director's target (negative: calms), held in 0..1, reached in Blend Time seconds (-1: the volume's rates). A noise in the dark: +0.2; a locked door behind you: -0.3.", false },
        "i:exec:,i:float:Delta,i:float:Blend Time,o:exec:" },
    [BPK_GET_TENSION] = { { "Get Tension", "Rendering|Horror", "The Tension Director's value now (0..1, the smoothed one): drive your own music, AI or effects from it.", true },
        "o:float:Return Value" },
    [BPK_TRIGGER_FILTER] = { { "Trigger Horror Filter", "Rendering|Horror", "A filter as an event: up to Intensity in Fade In seconds, held Hold seconds, gone in Fade Out. Filters: Hallucination (coarse colored noise, red, posterized, the blacks crushed), Black & White Blown Out, White Flash, TV Static, VHS, Chromatic Aberration, Distortion. A jump scare: White Flash 1, 0, 0.05, 0.1. False: no such filter.", false },
        "i:exec:,i:name:Filter,i:float:Intensity,i:float:Fade In,i:float:Hold,i:float:Fade Out,o:exec:,o:bool:Return Value" },
    [BPK_SET_FILTER] = { { "Set Horror Filter", "Rendering|Horror", "Holds a filter at Intensity (0: off), faded over Fade seconds: VHS for a found-footage level, Distortion while poisoned. Stacks with the triggered ones (the strongest wins). False: no such filter.", false },
        "i:exec:,i:name:Filter,i:float:Intensity,i:float:Fade,o:exec:,o:bool:Return Value" },
    [BPK_CLEAR_FILTERS] = { { "Clear Horror Filters", "Rendering|Horror", "Every horror filter faded out over Fade seconds (the held ones and the events under way).", false },
        "i:exec:,i:float:Fade,o:exec:" },
    [BPK_GET_FILTER] = { { "Get Horror Filter", "Rendering|Horror", "A horror filter's intensity now (0..1): its event, its held value and the Sequencer's, the strongest.", true },
        "i:name:Filter,o:float:Return Value" },
    [BPK_SET_LETTERBOX] = { { "Set Letterbox", "Rendering|Horror", "Black bars closing to an aspect ratio over Blend Time seconds (2.39 scope, 2.2 the horror shorts', 1.85; 0: open again). For a cutscene, a scare, a memory.", false },
        "i:exec:,i:float:Aspect Ratio,i:float:Blend Time,o:exec:" },
    [BPK_DIP_BLACK] = { { "Dip to Black", "Rendering|Horror", "The picture fades to black in Fade In seconds, stays Hold, comes back in Fade Out: the cut between two blocks of the story, waking up after being caught.", false },
        "i:exec:,i:float:Fade In,i:float:Hold,i:float:Fade Out,o:exec:" },
    [BPK_DIP_WHITE] = { { "Dip to White", "Rendering|Horror", "The picture fades to white and back (Fade In, Hold, Fade Out): a flashback, a blast, a memory.", false },
        "i:exec:,i:float:Fade In,i:float:Hold,i:float:Fade Out,o:exec:" },
    [BPK_SCARE_RIG] = { { "Play Scare Rig", "Rendering|Horror", "A camera move on the player's view for Duration seconds (0: its own): Dolly Zoom (the lens widens while the camera pulls in: the room stretches behind the subject), Whip Pan (a snap aside and back), Dutch Angle (the horizon tilted). Intensity 1: the usual; more is stronger. False: no such rig.", false },
        "i:exec:,i:name:Rig,i:float:Intensity,i:float:Duration,o:exec:,o:bool:Return Value" },
    [BPK_SET_LIGHT_FLICKER] = { { "Set Light Flicker", "Rendering|Horror", "A light's flicker pattern: None, Candle, Faulty Bulb, Strobe, Lightning or Pulse, how deep (Amount 0..1) and how fast (Speed x). The pattern's sound (Details > Light Flicker) plays in sync with its strikes. False: not a light or no such pattern.", false },
        "i:exec:,i:actor:Target,i:name:Pattern,i:float:Amount,i:float:Speed,o:exec:,o:bool:Return Value" },
    [BPK_SET_LIGHT_CHANNELS] = { { "Set Lighting Channels", "Rendering|Horror", "Lighting Channels on an actor and its components: on a light, the meshes it lights (only those sharing a channel); on a mesh or a character, the lights that reach it. A hero light on channel 1 that lights only the player: Channel 0 off, Channel 1 on, on both.", false },
        "i:exec:,i:actor:Target,i:bool:Channel 0,i:bool:Channel 1,i:bool:Channel 2,o:exec:" },
    [BPK_SET_RIM_TARGET] = { { "Set Rim Light Target", "Rendering|Horror", "Makes a light a Rim Light behind an actor as the camera sees it (Distance behind, Height above): the silhouette's bright edge that cuts a character out of the dark. Target Actor empty: the player. Off: the light stays where it is.", false },
        "i:exec:,i:actor:Light,i:actor:Target Actor,i:bool:Enabled,i:float:Distance,i:float:Height,o:exec:" },
    // (Q14) splines and cables (spline.h): Target a Spline / Rail / Cable actor (none: self); World Space off: the actor's space
    [BPK_SPLINE_LENGTH] = { { "Get Spline Length", "Splines", "The curve's length (cm) along its points. A Cable: along its particles now.", true },
        "i:actor:Target,o:float:Return Value" },
    [BPK_SPLINE_LOC_AT_DIST] = { { "Get Location at Distance Along Spline", "Splines", "The place Distance cm along the curve from its first point (a closed loop wraps; an open one stops at its ends). Move a platform, a camera or a car along it by adding to the distance every tick.", true },
        "i:actor:Target,i:float:Distance,i:bool:World Space,o:vector:Return Value" },
    [BPK_SPLINE_DIR_AT_DIST] = { { "Get Direction at Distance Along Spline", "Splines", "The curve's direction (unit) Distance cm along it: where something following it faces.", true },
        "i:actor:Target,i:float:Distance,i:bool:World Space,o:vector:Return Value" },
    [BPK_SPLINE_ROT_AT_DIST] = { { "Get Rotation at Distance Along Spline", "Splines", "The curve's rotation (pitch, yaw, roll: its point roll included) Distance cm along it.", true },
        "i:actor:Target,i:float:Distance,i:bool:World Space,o:vector:Return Value" },
    [BPK_SPLINE_XFORM_AT_DIST] = { { "Get Transform at Distance Along Spline", "Splines", "The place, rotation and scale (the points' scale across the curve) Distance cm along it: Set Actor Transform with it rides the curve exactly.", true },
        "i:actor:Target,i:float:Distance,i:bool:World Space,o:vector:Location,o:vector:Rotation,o:vector:Scale" },
    [BPK_SPLINE_LOC_AT_KEY] = { { "Get Location at Spline Input Key", "Splines", "The place at an input key (point i at key i, 1.5 halfway between the 2nd and 3rd points along the curve).", true },
        "i:actor:Target,i:float:Key,i:bool:World Space,o:vector:Return Value" },
    [BPK_SPLINE_DIST_AT_KEY] = { { "Get Distance Along Spline at Spline Input Key", "Splines", "How far along the curve (cm) an input key is: a point i's distance at key i.", true },
        "i:actor:Target,i:float:Key,o:float:Return Value" },
    [BPK_SPLINE_CLOSEST_KEY] = { { "Find Input Key Closest to World Location", "Splines", "The input key of the curve's point nearest a world location (then Get Distance Along Spline at Spline Input Key: how far along it you are).", true },
        "i:actor:Target,i:vector:World Location,o:float:Return Value" },
    [BPK_SPLINE_CLOSEST_LOC] = { { "Find Location Closest to World Location", "Splines", "The curve's point nearest a world location (in the world): snap something to a path, a rail, a wire.", true },
        "i:actor:Target,i:vector:World Location,o:vector:Return Value" },
    [BPK_SPLINE_NUM_POINTS] = { { "Get Number of Spline Points", "Splines", "How many points the curve has (a Cable: its particles).", true },
        "i:actor:Target,o:int:Return Value" },
    [BPK_SPLINE_POINT_LOC] = { { "Get Location at Spline Point", "Splines", "A point's place (its index from 0).", true },
        "i:actor:Target,i:int:Index,i:bool:World Space,o:vector:Return Value" },
    [BPK_SPLINE_ADD_POINT] = { { "Add Spline Point", "Splines", "A point at the end of the curve (a Curve point: its tangents automatic). Its Spline Mesh and instances follow the same frame. Construction Scripts can build a curve this way.", false },
        "i:exec:,i:actor:Target,i:vector:Location,i:bool:World Space,o:exec:,o:bool:Return Value" },
    [BPK_SPLINE_SET_POINT] = { { "Set Location at Spline Point", "Splines", "Moves a point (its index from 0): the curve, its meshes and instances follow.", false },
        "i:exec:,i:actor:Target,i:int:Index,i:vector:Location,i:bool:World Space,o:exec:,o:bool:Return Value" },
    [BPK_SPLINE_REMOVE_POINT] = { { "Remove Spline Point", "Splines", "Removes a point (two always stay: a curve needs a segment).", false },
        "i:exec:,i:actor:Target,i:int:Index,o:exec:,o:bool:Return Value" },
    [BPK_SPLINE_CLEAR] = { { "Clear Spline Points", "Splines", "Removes every point (then Add Spline Point builds it again).", false },
        "i:exec:,i:actor:Target,o:exec:" },
    [BPK_SPLINE_SET_CLOSED] = { { "Set Closed Loop", "Splines", "Joins the last point back to the first (a loop: a race track, a patrol) or opens it.", false },
        "i:exec:,i:actor:Target,i:bool:Closed Loop,o:exec:" },
    [BPK_SPLINE_SET_TYPE] = { { "Set Spline Point Type", "Splines", "A point's type: \"Curve\" (automatic tangents), \"Curve Custom Tangent\", \"Linear\" (straight to the next) or \"Constant\" (a step).", false },
        "i:exec:,i:actor:Target,i:int:Index,i:name:Type,o:exec:" },
    [BPK_CABLE_ATTACH_END] = { { "Set Attach End To", "Splines|Cable", "Holds the cable's end on an actor (a socket or bone of its Skeletal Mesh when named): a rope tied to a hand, a hose to a car.", false },
        "i:exec:,i:actor:Target,i:actor:Actor,i:string:Socket Name,o:exec:,o:bool:Return Value" },
    [BPK_CABLE_ATTACH_START] = { { "Set Attach Start To", "Splines|Cable", "Holds the cable's start on an actor (and a socket or bone); none: the cable's own actor.", false },
        "i:exec:,i:actor:Target,i:actor:Actor,i:string:Socket Name,o:exec:,o:bool:Return Value" },
    [BPK_CABLE_DETACH] = { { "Detach Cable End", "Splines|Cable", "Lets an end go (End: the end, else the start): it falls and swings free, as a cut rope.", false },
        "i:exec:,i:actor:Target,i:bool:End,o:exec:" },
    [BPK_CABLE_SET_LENGTH] = { { "Set Cable Length", "Splines|Cable", "The cable's length (cm), where it hangs now: reel a winch in or out, lower a lamp.", false },
        "i:exec:,i:actor:Target,i:float:Length,o:exec:" },
    [BPK_CABLE_GET_LENGTH] = { { "Get Cable Length", "Splines|Cable", "The cable's rest length (cm; its particles' path: Get Spline Length).", true },
        "i:actor:Target,o:float:Return Value" },
    [BPK_CABLE_IMPULSE] = { { "Add Cable Impulse", "Splines|Cable", "Pushes the cable's particles within Radius of a location by a velocity (cm/s), fading to the edge: a hit, a gust, a character brushing past.", false },
        "i:exec:,i:actor:Target,i:vector:Location,i:vector:Velocity,i:float:Radius,o:exec:" },
    // --- (Q15) PCG (pcg.h) ---
    [BPK_PCG_GENERATE] = { { "Generate PCG", "PCG", "Runs the PCG component of the actor (a PCG Volume; Target empty: this actor) again: its graph inside its box, by its seed (a partitioned one: its cells near the player again). What it made before is replaced. For a Generate on Demand component, the first generation. False: no PCG component.", false },
        "i:exec:,i:actor:Target,o:exec:,o:bool:Return Value" },
    [BPK_PCG_CLEANUP] = { { "Cleanup PCG", "PCG", "Removes everything the PCG component made (meshes, actors, nested graphs) and keeps it empty until Generate PCG. False: no PCG component.", false },
        "i:exec:,i:actor:Target,o:exec:,o:bool:Return Value" },
    [BPK_PCG_SET_SEED] = { { "Set PCG Seed", "PCG", "The PCG component's seed: every random choice of its graph follows it (the same seed, the same result; another seed, another forest). What it made is made again with the new seed. A procedural level: a seed per run.", false },
        "i:exec:,i:actor:Target,i:int:Seed,o:exec:,o:bool:Return Value" },
    [BPK_PCG_SET_PARAM] = { { "Set PCG Parameter", "PCG", "Sets a parameter of the component's graph (Graph Parameters: Tree Density...) on this actor and makes it again. False: no PCG component, or 8 parameters set already.", false },
        "i:exec:,i:actor:Target,i:name:Name,i:float:Value,o:exec:,o:bool:Return Value" },
    [BPK_PCG_IS_GENERATED] = { { "Is PCG Generated", "PCG", "Whether the PCG component has finished making what its graph spawns (a partitioned one: its first cells). Count: the entities it made so far.", true },
        "i:actor:Target,o:bool:Return Value,o:int:Count" },
    // --- (Q16) everything editable in the game (runtime_lighting.h, props.h, cvars.h) ---
    [BPK_BUILD_LIGHTING] = { { "Build Lighting (Async)", "Rendering|Lighting", "The editor's Build Lighting run in the game, for a level made while playing (procedural rooms, a PCG graph, spawned Static actors): the lightmaps and the baked probe volume of every Static mesh. Mode: Loading Screen (the game waits behind the loading screen, the bake as fast as the GPU goes) or Progressive (the game plays on: a slice of the bake each frame inside Budget ms of the GPU, the dynamic GI lighting the level until the lightmaps are ready, then they show). Quality: Preview or Production. The first pin goes on at once; Completed when the bake ends (Event On Lighting Built runs too). Started false: no hardware ray tracing, no Static mesh, or a bake already running.", false },
                           "i:exec:,i:name:Mode,i:name:Quality,i:float:Budget ms,o:exec:,o:exec:Completed,o:bool:Started" },
    [BPK_LIGHTING_PROGRESS] = { { "Get Lighting Build Progress", "Rendering|Lighting", "The game's Build Lighting: Progress 0..1, Building while it runs, Built once its lightmaps show. For a loading bar of your own (Mode Progressive) or a message.", true },
                              "o:float:Progress,o:bool:Building,o:bool:Built" },
    [BPK_CANCEL_LIGHTING] = { { "Cancel Lighting Build", "Rendering|Lighting", "Stops the game's Build Lighting: the dynamic GI goes on lighting the level; Event On Lighting Built runs with Success false.", false }, "i:exec:,o:exec:" },
    [BPK_SAVE_LIGHTING] = { { "Save Built Lighting", "Rendering|Lighting", "Saves the lighting the game built under a name (Saved/SaveGames/<Name>.rlight): a procedural map made again from the same seed loads it back (Load Built Lighting) instead of baking. False: nothing built, or a bake still running.", false },
                          "i:exec:,i:string:Name,o:exec:,o:bool:Return Value" },
    [BPK_LOAD_LIGHTING] = { { "Load Built Lighting", "Rendering|Lighting", "Loads lighting saved by Save Built Lighting: the Static meshes found where they were baked (same place, same mesh) take their lightmaps at once. False: no such file.", false },
                          "i:exec:,i:string:Name,o:exec:,o:bool:Return Value" },
    [BPK_EVENT_LIGHTING_BUILT] = { { "Event On Lighting Built", "Rendering|Lighting", "Runs in every Blueprint when the game's Build Lighting ends: Success (false: cancelled or it could not start) and the Seconds it took. Hide your loading message, open the doors.", false },
                                 "o:exec:,o:bool:Success,o:float:Seconds" },
    [BPK_SET_PROPERTY] = { { "Set Property by Name", "Utilities|Properties", "Sets any field the Details panel shows, by its label, on the Target actor (empty: self) or the first of its components that has it: \"Intensity\", \"Attenuation Radius\", \"Max Walk Speed\", \"Light.Color\" (Section.Label when two components share a label; spaces and case don't matter). Value for a number or a checkbox, Vector for a vector or a color. False: no such property on it. Get Property by Name reads it; List Properties lists them.", false },
                         "i:exec:,i:actor:Target,i:name:Property,i:float:Value,i:vector:Vector,o:exec:,o:bool:Return Value" },
    [BPK_GET_PROPERTY] = { { "Get Property by Name", "Utilities|Properties", "Reads any field the Details panel shows, by its label, from the Target actor (empty: self) or its components: Value (a number; a checkbox 0 or 1), Vector (a vector or a color). Found false: no such property on it.", true },
                         "i:actor:Target,i:name:Property,o:float:Value,o:vector:Vector,o:bool:Found" },
    [BPK_SET_CVAR] = { { "Set Console Variable", "Utilities|Console", "Changes a render, quality, physics or audio setting by its console name, in the game as in the editor's Play: r.ScreenPercentage, sg.Scalability, r.GI.Probes, r.Reflections, r.Shadow.Virtual.Resolution, r.RenderStyle, p.GravityZ, p.SolverSubsteps, au.MasterVolume, au.Binaural... Text when given (\"75\", \"true\", \"Epic\"), else Value; kept in its range. False: no such variable. A play in the editor gives the editor's settings back when it stops.", false },
                     "i:exec:,i:name:Name,i:float:Value,i:string:Text,o:exec:,o:bool:Return Value" },
    [BPK_GET_CVAR] = { { "Get Console Variable", "Utilities|Console", "A console variable's value (Value; Text as written). Found false: no such variable.", true },
                     "i:name:Name,o:float:Value,o:string:Text,o:bool:Found" },
    [BPK_EXEC_CONSOLE] = { { "Execute Console Command", "Utilities|Console", "Execute Console Command: \"r.ScreenPercentage 75\" sets a console variable, \"r.ScreenPercentage\" reads it. Result: \"r.ScreenPercentage = 75\" (or why not). Return Value false: unknown.", false },
                         "i:exec:,i:string:Command,o:exec:,o:bool:Return Value,o:string:Result" },
    [BPK_LIST_PROPERTIES] = { { "List Properties", "Utilities|Properties", "The labels of every property Set Property by Name reaches on the Target actor (empty: self) and its components, as \"Section.Label\" in an array of strings (an options menu built from them, a debug print), and their Count.", false },
                            "i:exec:,i:actor:Target,o:exec:,o:string[]:Properties,o:int:Count" },
    // (Q25) VR (vr_pawn.h, notas-dev/xr.md)
    [BPK_VR_TELEPORT] = { { "VR Teleport", "VR", "Moves the VR player so the head lands over Location (cm, on a floor), facing Yaw (degrees) unless Keep Yaw. The play space and the pawn go together; Event On VR Teleported runs on the pawn.", false },
        "i:exec:,i:vector:Location,i:float:Yaw,i:bool:Keep Yaw,o:exec:" },
    [BPK_VR_SET_LOCOMOTION] = { { "Set VR Locomotion", "VR", "How the VR player moves and turns: Locomotion 0 Teleport, 1 Smooth, 2 both (the right stick's push teleports); Turn 0 Snap, 1 Smooth, 2 none; -1 keeps it. Project Settings > VR sets the start.", false },
        "i:exec:,i:int:Locomotion,i:int:Turn,o:exec:" },
    [BPK_VR_SET_VIGNETTE] = { { "Set VR Comfort Vignette", "VR", "The darkened edge while the VR player moves or turns (0: off, 1: strong): less motion sickness.", false },
        "i:exec:,i:float:Strength,o:exec:" },
    [BPK_VR_GRAB] = { { "VR Grab", "VR", "Puts Target in a hand (Hand: 0 left, 1 right) as its grip would: it follows the hand until VR Release or the grip opens (it need not be Grabbable).", false },
        "i:exec:,i:int:Hand,i:actor:Target,o:exec:,o:bool:Success" },
    [BPK_VR_RELEASE] = { { "VR Release", "VR", "The hand lets go of what it holds: thrown with the hand's speed (its physics again), or snapped into a Grab Socket near it.", false },
        "i:exec:,i:int:Hand,o:exec:" },
    [BPK_VR_GET_HELD] = { { "Get VR Held Actor", "VR", "What a hand holds (Hand: 0 left, 1 right); none: empty.", true }, "i:int:Hand,o:actor:Return Value" },
    [BPK_VR_LASER] = { { "Get VR Laser Hit", "VR", "Where the hand's laser ends: Hit (it met something within 10 m), the Location and the Interactable it points at (none: a wall, the floor, nothing).", true },
        "i:int:Hand,o:bool:Hit,o:vector:Location,o:actor:Actor" },
    [BPK_VR_HAND_TRACKING] = { { "Get VR Hand Tracking", "VR", "A tracked hand (the headset's cameras, no controller): Tracked, its Pinch (thumb and index touching: 1) and Grip (a fist: 1).", true },
        "i:int:Hand,o:bool:Tracked,o:float:Pinch,o:float:Grip" },
    [BPK_VR_SET_REFRESH] = { { "Set VR Refresh Rate", "VR", "Asks the headset's display for Hz (72, 90, 120 on a Quest). False: the headset doesn't offer it (or no headset).", false },
        "i:exec:,i:float:Hz,o:exec:,o:bool:Success" },
    [BPK_EVENT_VR_GRABBED] = { { "Event On VR Grabbed", "VR|Events", "Runs on an actor when a VR hand takes it: which Hand (0 left, 1 right).", false }, "o:exec:,o:int:Hand" },
    [BPK_EVENT_VR_RELEASED] = { { "Event On VR Released", "VR|Events", "Runs on an actor when a VR hand lets it go: the Hand, the Velocity it was thrown at (cm/s) and the Grab Socket it snapped into (none: thrown or dropped).", false },
        "o:exec:,o:int:Hand,o:vector:Velocity,o:actor:Socket" },
    [BPK_VR_FINGER_CURLS] = { { "Get VR Finger Curls", "VR", "A tracked hand's fingers one by one, 0 straight .. 1 curled: Thumb, Index, Middle, Ring, Little (Hand: 0 left, 1 right).", true },
        "i:int:Hand,o:float:Thumb,o:float:Index,o:float:Middle,o:float:Ring,o:float:Little" },
    [BPK_VR_HAND_GESTURE] = { { "Get VR Hand Gesture", "VR", "A tracked hand's shape: 0 None, 1 Open, 2 Fist, 3 Point, 4 Pinch, 5 Thumbs Up, 6 Peace; and its Name.", true },
        "i:int:Hand,o:int:Gesture,o:string:Name" },
    [BPK_VR_FINGER_TOUCH] = { { "Get VR Finger Touch", "VR", "How much a finger's tip touches the index finger (Finger: 0 thumb, 1 middle, 2 ring, 3 little): 1 touching, 0 apart.", true },
        "i:int:Hand,i:int:Finger,o:float:Return Value" },
    [BPK_VR_SET_HAND_COLLISION] = { { "Set VR Hand Collision", "VR", "What the VR hands collide with, in game: Physical Hands (the drawn hands stop at walls, floors and tables), With Objects (they stop at bodies with physics and Interactables too; off: the level only) and Push Bodies (the hands push the bodies with physics they touch). Project Settings > VR sets the start.", false },
        "i:exec:,i:bool:Physical Hands,i:bool:With Objects,i:bool:Push Bodies,o:exec:" },
    [BPK_EVENT_VR_TELEPORTED] = { { "Event On VR Teleported", "VR|Events", "Runs on the VR player's pawn after a teleport: the Location it landed on.", false }, "o:exec:,o:vector:Location" },
    // (Q25b) mixed reality (xr_mr.h, notas-dev/xr.md)
    [BPK_MR_SET_PASSTHROUGH] = { { "Set Passthrough Enabled", "VR|Mixed Reality", "Shows the real world through the headset's cameras (a Quest 3 / 3S / Pro; Project Settings > VR > Mixed Reality must be on when the game starts) or hides it.", false },
        "i:exec:,i:bool:Enabled,o:exec:" },
    [BPK_MR_IS_PASSTHROUGH] = { { "Is Passthrough Active", "VR|Mixed Reality", "True while the real world is seen (the passthrough running, or the simulated headset's).", true }, "o:bool:Return Value" },
    [BPK_MR_SET_OPACITY] = { { "Set Passthrough Opacity", "VR|Mixed Reality", "How much of the real world is seen (0: black, 1: all of it).", false },
        "i:exec:,i:float:Opacity,o:exec:" },
    [BPK_MR_SET_STYLE] = { { "Set Passthrough Style", "VR|Mixed Reality", "The real world's look: Style 0 color, 1 black & white, 2 one color (Tint by its brightness), 3 adjusted (Brightness -100..100, Contrast and Saturation: 1 as seen).", false },
        "i:exec:,i:int:Style,i:float:Brightness,i:float:Contrast,i:float:Saturation,i:vector:Tint,o:exec:" },
    [BPK_MR_SET_EDGE] = { { "Set Passthrough Edge Color", "VR|Mixed Reality", "Draws the real world's edges in Color (Alpha 0: none): a stylized look, a scanner.", false },
        "i:exec:,i:vector:Color,i:float:Alpha,o:exec:" },
    [BPK_MR_SET_BACKGROUND] = { { "Set Passthrough Background", "VR|Mixed Reality", "Show Room: the room in the level's sky's place (everything not drawn is the real world); false: the level's sky kept, the room only through Passthrough Windows.", false },
        "i:exec:,i:bool:Show Room,o:exec:" },
    [BPK_MR_SET_BLEND] = { { "Set Passthrough Blend", "VR|Mixed Reality", "The virtual things' opacity over the room: 1 solid, 0 only the room (a fade between VR and the real world).", false },
        "i:exec:,i:float:Virtual Opacity,o:exec:" },
    [BPK_MR_SET_WINDOW] = { { "Set Passthrough Window", "VR|Mixed Reality", "Target's bounds become a window to the real world: what is virtual inside and behind it isn't drawn (a portal in a virtual wall). 16 at most.", false },
        "i:exec:,i:actor:Target,i:bool:Enabled,o:exec:" },
    [BPK_MR_SET_OCCLUSION] = { { "Set Real World Occlusion", "VR|Mixed Reality", "Real things hiding the virtual ones behind them: Mode 0 none, 1 the room's planes and furniture, 2 the headset's real depth (the room without it).", false },
        "i:exec:,i:int:Mode,o:exec:" },
    [BPK_MR_LOAD_ROOM] = { { "Load Room", "VR|Mixed Reality", "Loads the headset's Space Setup again (the floor, walls, tables...): Event On Room Loaded runs when it is in. False: this headset has no Scene.", false },
        "i:exec:,o:exec:,o:bool:Success" },
    [BPK_MR_ROOM_SETUP] = { { "Request Room Setup", "VR|Mixed Reality", "Opens the headset's Space Setup so the player scans the room; Event On Room Loaded runs after it. False: not on this headset.", false },
        "i:exec:,o:exec:,o:bool:Success" },
    [BPK_MR_ROOM_PLANES] = { { "Get Room Planes", "VR|Mixed Reality", "The room's actors (\"MR Floor\", \"MR Wall 2\", \"MR Table\"...): one per plane and piece of furniture, colliding, never drawn. Get Room Plane Info tells each one's label and size.", false },
        "i:exec:,o:exec:,o:actor[]:Actors,o:int:Count" },
    [BPK_MR_PLANE_INFO] = { { "Get Room Plane Info", "VR|Mixed Reality", "A room actor's Label (Floor, Ceiling, Wall, Table, Couch, Bed, Storage, Screen...), its face's Center and Normal, its Size (cm). Valid false: not a room actor.", true },
        "i:actor:Target,o:bool:Valid,o:string:Label,o:vector:Center,o:vector:Normal,o:vector:Size" },
    [BPK_MR_TRACE] = { { "Line Trace Real World", "VR|Mixed Reality", "A line from Start to End against the real room (its planes, furniture and mesh): the nearest Hit, its Location, Normal, Label and room Actor. To put something on the real table.", true },
        "i:vector:Start,i:vector:End,o:bool:Hit,o:vector:Location,o:vector:Normal,o:string:Label,o:actor:Actor" },
    [BPK_LINE_TRACE] = { { "Line Trace By Channel", "Collision|Traces", "A line from Start to End: the first thing it hits. Return Value: it hit; Location and Impact Point where, Normal the surface's, Hit Actor, Distance (cm). Trace Channel: Visibility and Camera see everything that collides; World Static, World Dynamic, Pawn or Landscape only that kind. The actor running it is left out, and Actors to Ignore. Draw Debug Type: None, For One Frame, For Duration (Draw Time seconds) or Persistent (until Flush Persistent Debug Lines) - red to the hit, green after it, a point on the impact.", false },
        "i:exec:,i:vector:Start,i:vector:End,i:int(ETraceChannel):Trace Channel,i:bool:Trace Complex,i:actor[]:Actors to Ignore,i:int(EDrawDebugTrace):Draw Debug Type,i:float:Draw Time,o:exec:,o:bool:Return Value,o:vector:Location,o:vector:Impact Point,o:vector:Normal,o:actor:Hit Actor,o:float:Distance" },
    [BPK_SPHERE_TRACE] = { { "Sphere Trace By Channel", "Collision|Traces", "A sphere of Radius swept from Start to End: the first thing it touches. Location: the sphere's center there; Impact Point: the touched point. Exact (not sampled). Trace Channel: Visibility and Camera see everything that collides; World Static, World Dynamic, Pawn or Landscape only that kind. The actor running it is left out, and Actors to Ignore. Draw Debug Type: None, For One Frame, For Duration (Draw Time seconds) or Persistent (until Flush Persistent Debug Lines) - red to the hit, green after it, a point on the impact.", false },
        "i:exec:,i:vector:Start,i:vector:End,i:float:Radius,i:int(ETraceChannel):Trace Channel,i:bool:Trace Complex,i:actor[]:Actors to Ignore,i:int(EDrawDebugTrace):Draw Debug Type,i:float:Draw Time,o:exec:,o:bool:Return Value,o:vector:Location,o:vector:Impact Point,o:vector:Normal,o:actor:Hit Actor,o:float:Distance" },
    [BPK_BOX_TRACE] = { { "Box Trace By Channel", "Collision|Traces", "A box of Half Size (aligned to the world) swept from Start to End: the first thing it touches. Exact. Trace Channel: Visibility and Camera see everything that collides; World Static, World Dynamic, Pawn or Landscape only that kind. The actor running it is left out, and Actors to Ignore. Draw Debug Type: None, For One Frame, For Duration (Draw Time seconds) or Persistent (until Flush Persistent Debug Lines) - red to the hit, green after it, a point on the impact.", false },
        "i:exec:,i:vector:Start,i:vector:End,i:vector:Half Size,i:int(ETraceChannel):Trace Channel,i:bool:Trace Complex,i:actor[]:Actors to Ignore,i:int(EDrawDebugTrace):Draw Debug Type,i:float:Draw Time,o:exec:,o:bool:Return Value,o:vector:Location,o:vector:Impact Point,o:vector:Normal,o:actor:Hit Actor,o:float:Distance" },
    [BPK_CAPSULE_TRACE] = { { "Capsule Trace By Channel", "Collision|Traces", "An upright capsule (Radius, Half Height with the radius in it, as a Character's) swept from Start to End: the first thing it touches. Exact. Trace Channel: Visibility and Camera see everything that collides; World Static, World Dynamic, Pawn or Landscape only that kind. The actor running it is left out, and Actors to Ignore. Draw Debug Type: None, For One Frame, For Duration (Draw Time seconds) or Persistent (until Flush Persistent Debug Lines) - red to the hit, green after it, a point on the impact.", false },
        "i:exec:,i:vector:Start,i:vector:End,i:float:Radius,i:float:Half Height,i:int(ETraceChannel):Trace Channel,i:bool:Trace Complex,i:actor[]:Actors to Ignore,i:int(EDrawDebugTrace):Draw Debug Type,i:float:Draw Time,o:exec:,o:bool:Return Value,o:vector:Location,o:vector:Impact Point,o:vector:Normal,o:actor:Hit Actor,o:float:Distance" },
    [BPK_MULTI_LINE_TRACE] = { { "Multi Line Trace By Channel", "Collision|Traces", "A line from Start to End: every actor it passes through, nearest first (one hit each, 256 at most), in parallel arrays. Trace Channel: Visibility and Camera see everything that collides; World Static, World Dynamic, Pawn or Landscape only that kind. The actor running it is left out, and Actors to Ignore. Draw Debug Type: None, For One Frame, For Duration (Draw Time seconds) or Persistent (until Flush Persistent Debug Lines) - red to the hit, green after it, a point on the impact.", false },
        "i:exec:,i:vector:Start,i:vector:End,i:int(ETraceChannel):Trace Channel,i:bool:Trace Complex,i:actor[]:Actors to Ignore,i:int(EDrawDebugTrace):Draw Debug Type,i:float:Draw Time,o:exec:,o:bool:Return Value,o:actor[]:Hit Actors,o:vector[]:Locations,o:vector[]:Normals,o:float[]:Distances" },
    [BPK_MULTI_SPHERE_TRACE] = { { "Multi Sphere Trace By Channel", "Collision|Traces", "A sphere swept from Start to End: every actor it touches, nearest first. Trace Channel: Visibility and Camera see everything that collides; World Static, World Dynamic, Pawn or Landscape only that kind. The actor running it is left out, and Actors to Ignore. Draw Debug Type: None, For One Frame, For Duration (Draw Time seconds) or Persistent (until Flush Persistent Debug Lines) - red to the hit, green after it, a point on the impact.", false },
        "i:exec:,i:vector:Start,i:vector:End,i:float:Radius,i:int(ETraceChannel):Trace Channel,i:bool:Trace Complex,i:actor[]:Actors to Ignore,i:int(EDrawDebugTrace):Draw Debug Type,i:float:Draw Time,o:exec:,o:bool:Return Value,o:actor[]:Hit Actors,o:vector[]:Locations,o:vector[]:Normals,o:float[]:Distances" },
    [BPK_MULTI_BOX_TRACE] = { { "Multi Box Trace By Channel", "Collision|Traces", "A box swept from Start to End: every actor it touches, nearest first. Trace Channel: Visibility and Camera see everything that collides; World Static, World Dynamic, Pawn or Landscape only that kind. The actor running it is left out, and Actors to Ignore. Draw Debug Type: None, For One Frame, For Duration (Draw Time seconds) or Persistent (until Flush Persistent Debug Lines) - red to the hit, green after it, a point on the impact.", false },
        "i:exec:,i:vector:Start,i:vector:End,i:vector:Half Size,i:int(ETraceChannel):Trace Channel,i:bool:Trace Complex,i:actor[]:Actors to Ignore,i:int(EDrawDebugTrace):Draw Debug Type,i:float:Draw Time,o:exec:,o:bool:Return Value,o:actor[]:Hit Actors,o:vector[]:Locations,o:vector[]:Normals,o:float[]:Distances" },
    [BPK_MULTI_CAPSULE_TRACE] = { { "Multi Capsule Trace By Channel", "Collision|Traces", "An upright capsule swept from Start to End: every actor it touches, nearest first. Trace Channel: Visibility and Camera see everything that collides; World Static, World Dynamic, Pawn or Landscape only that kind. The actor running it is left out, and Actors to Ignore. Draw Debug Type: None, For One Frame, For Duration (Draw Time seconds) or Persistent (until Flush Persistent Debug Lines) - red to the hit, green after it, a point on the impact.", false },
        "i:exec:,i:vector:Start,i:vector:End,i:float:Radius,i:float:Half Height,i:int(ETraceChannel):Trace Channel,i:bool:Trace Complex,i:actor[]:Actors to Ignore,i:int(EDrawDebugTrace):Draw Debug Type,i:float:Draw Time,o:exec:,o:bool:Return Value,o:actor[]:Hit Actors,o:vector[]:Locations,o:vector[]:Normals,o:float[]:Distances" },
    [BPK_LINE_TRACE_BATCH] = { { "Line Trace Batch", "Collision|Traces", "Many lines at once (Starts[i] to Ends[i]), run in parallel on every core: hundreds of thousands a frame. Hits, Locations, Normals and Hit Actors in the same order (a miss: false, the end, no actor); Hit Count. Trace Channel: Visibility and Camera see everything that collides; World Static, World Dynamic, Pawn or Landscape only that kind. The actor running it is left out, and Actors to Ignore. Draw Debug Type: None, For One Frame, For Duration (Draw Time seconds) or Persistent (until Flush Persistent Debug Lines) - red to the hit, green after it, a point on the impact.", false },
        "i:exec:,i:vector[]:Starts,i:vector[]:Ends,i:int(ETraceChannel):Trace Channel,i:bool:Trace Complex,i:actor[]:Actors to Ignore,i:int(EDrawDebugTrace):Draw Debug Type,i:float:Draw Time,o:exec:,o:int:Hit Count,o:bool[]:Hits,o:vector[]:Locations,o:vector[]:Normals,o:actor[]:Hit Actors" },
    [BPK_SPHERE_TRACE_BATCH] = { { "Sphere Trace Batch", "Collision|Traces", "Many spheres of Radius swept at once (Starts[i] to Ends[i]) on every core. Trace Channel: Visibility and Camera see everything that collides; World Static, World Dynamic, Pawn or Landscape only that kind. The actor running it is left out, and Actors to Ignore. Draw Debug Type: None, For One Frame, For Duration (Draw Time seconds) or Persistent (until Flush Persistent Debug Lines) - red to the hit, green after it, a point on the impact.", false },
        "i:exec:,i:vector[]:Starts,i:vector[]:Ends,i:float:Radius,i:int(ETraceChannel):Trace Channel,i:bool:Trace Complex,i:actor[]:Actors to Ignore,i:int(EDrawDebugTrace):Draw Debug Type,i:float:Draw Time,o:exec:,o:int:Hit Count,o:bool[]:Hits,o:vector[]:Locations,o:vector[]:Normals,o:actor[]:Hit Actors" },
    [BPK_DRAW_DEBUG_LINE] = { { "Draw Debug Line", "Collision|Debug", "A line in the world for debugging. Duration: 0 this frame, seconds, or below 0 until Flush Persistent Debug Lines. Thickness in pixels.", false },
        "i:exec:,i:vector:Line Start,i:vector:Line End,i:color:Line Color,i:float:Duration,i:float:Thickness,o:exec:" },
    [BPK_DRAW_DEBUG_POINT] = { { "Draw Debug Point", "Collision|Debug", "A square point in the world (Size in pixels) for debugging. Duration as Draw Debug Line's.", false },
        "i:exec:,i:vector:Position,i:float:Size,i:color:Point Color,i:float:Duration,o:exec:" },
    [BPK_DRAW_DEBUG_BOX] = { { "Draw Debug Box", "Collision|Debug", "A box's 12 edges around Center (Extent: its half size) for debugging. Duration as Draw Debug Line's.", false },
        "i:exec:,i:vector:Center,i:vector:Extent,i:color:Line Color,i:float:Duration,i:float:Thickness,o:exec:" },
    [BPK_DRAW_DEBUG_SPHERE] = { { "Draw Debug Sphere", "Collision|Debug", "A sphere (three circles) for debugging. Duration as Draw Debug Line's.", false },
        "i:exec:,i:vector:Center,i:float:Radius,i:color:Line Color,i:float:Duration,i:float:Thickness,o:exec:" },
    [BPK_DRAW_DEBUG_CAPSULE] = { { "Draw Debug Capsule", "Collision|Debug", "An upright capsule (Half Height with the radius in it) for debugging. Duration as Draw Debug Line's.", false },
        "i:exec:,i:vector:Center,i:float:Half Height,i:float:Radius,i:color:Line Color,i:float:Duration,i:float:Thickness,o:exec:" },
    [BPK_FLUSH_DEBUG_LINES] = { { "Flush Persistent Debug Lines", "Collision|Debug", "Removes the debug lines, points and shapes drawn Persistent (and with a Duration below 0).", false },
        "i:exec:,o:exec:" },
    [BPK_SET_TRACE_COLORS] = { { "Set Debug Trace Colors", "Collision|Debug", "The colors the traces' Draw Debug Type uses: Trace Color to the hit (and the impact point), Trace Hit Color after it. red and green by default.", false },
        "i:exec:,i:color:Trace Color,i:color:Trace Hit Color,o:exec:" },
    [BPK_ASYNC_LINE_TRACE_BATCH] = { { "Async Line Trace Batch", "Collision|Traces", "Many lines (Starts[i] to Ends[i]) traced on background threads while the frame goes on: no wait now. Handle: give it to Get Async Trace Results next frame. Drawn (Draw Debug Type) the frame the results arrive.", false },
        "i:exec:,i:vector[]:Starts,i:vector[]:Ends,i:int(ETraceChannel):Trace Channel,i:bool:Trace Complex,i:actor[]:Actors to Ignore,i:int(EDrawDebugTrace):Draw Debug Type,i:float:Draw Time,o:exec:,o:int:Handle" },
    [BPK_ASYNC_SPHERE_TRACE_BATCH] = { { "Async Sphere Trace Batch", "Collision|Traces", "Many spheres of Radius swept on background threads while the frame goes on: no wait now. Handle: give it to Get Async Trace Results next frame.", false },
        "i:exec:,i:vector[]:Starts,i:vector[]:Ends,i:float:Radius,i:int(ETraceChannel):Trace Channel,i:bool:Trace Complex,i:actor[]:Actors to Ignore,i:int(EDrawDebugTrace):Draw Debug Type,i:float:Draw Time,o:exec:,o:int:Handle" },
    [BPK_GET_ASYNC_TRACE] = { { "Get Async Trace Results", "Collision|Traces", "An async batch's results, the frame after it was started (Ready true then; false before, and two frames later they are gone): Hit Count, and Hits, Locations, Normals, Hit Actors in the order of its Starts.", false },
        "i:exec:,i:int:Handle,o:exec:,o:bool:Ready,o:int:Hit Count,o:bool[]:Hits,o:vector[]:Locations,o:vector[]:Normals,o:actor[]:Hit Actors" },
    [BPK_ACR_WOUND] = { { "Apply Wound Reaction", "Animation|Active Ragdoll", "A wound at Location on a bone, Severity 0..1 (the impulse is Apply Hit Reaction's): a torso, arm or head wound, the other side's hand holds it; a leg wound, it limps (the knee bends, the pelvis drops; Get Limp for the walk). Past 0.7 the knees give, and on the ground it writhes a few seconds before it gets up.", false },
        "i:exec:,i:actor:Target,i:string:Bone Name,i:vector:Location,i:float:Severity,o:exec:" },
    [BPK_ACR_GET_LIMP] = { { "Get Limp", "Animation|Active Ragdoll", "How badly the Active Ragdoll limps now (0 none .. 1): from a leg wound, fading over 8-20 s. Feed the walk's injured blend or slow the character down.", true },
        "i:actor:Target,o:float:Return Value" },
    [BPK_ACR_IS_WRITHING] = { { "Is Writhing", "Animation|Active Ragdoll", "True while a gravely wounded Active Ragdoll writhes on the ground (before it gets up): a pain sound, a revive prompt.", true },
        "i:actor:Target,o:bool:Return Value" },
    [BPK_GET_TIME_OF_DAY] = { { "Get Time of Day", "City", "The game's clock: the hour of the day (0..24, 13.5 is half past one in the afternoon).", true },
        "o:float:Return Value" },
    [BPK_SET_TIME_OF_DAY] = { { "Set Time of Day", "City", "Sets the game's clock to Hour (0..24) and how fast it runs: Time Scale game seconds a real one (60: an hour of the game in a real minute; 0 keeps it). The Smart Objects open and close by it.", false },
        "i:exec:,i:float:Hour,i:float:Time Scale,o:exec:" },
    [BPK_REPORT_CRIME] = { { "Report Crime", "City", "A crime at Location seen within Radius (cm): the crowd there panics and the nearest one calls the police 2-4 s later (Get Police Calls).", false },
        "i:exec:,i:vector:Location,i:float:Radius,o:exec:" },
    [BPK_GET_POLICE_CALLS] = { { "Get Police Calls", "City", "How many calls the witnesses made to the police since the Play started: send a patrol, raise the wanted level.", true },
        "o:int:Return Value" },
    [BPK_IS_OPEN_NOW] = { { "Is Open Now", "City", "True when Target (an Interactable with Smart Object Slots) is open at the clock's hour.", true },
        "i:actor:Target,o:bool:Return Value" },
    [BPK_SET_LOCO_STYLE] = { { "Set Locomotion Style", "Animation|Motion Matching", "How Target moves: Normal, Injured (a limp), Tired, Drunk or Carrying. Its Motion Matching takes the clips with that word in their names when it has them, else a layer bends the body; it eases in (no pop). The active ragdoll's limp sets Injured by itself.", false },
        "i:exec:,i:actor:Target,i:name:Style,o:exec:,o:bool:Return Value" },
    [BPK_ADD_WARP_TARGET] = { { "Add or Update Warp Target", "Animation|Motion Warping", "Where Target's montage ends its Motion Warping window of this name: a place and a facing. Its root motion bends inside the window to arrive there: a vault onto a ledge, an attack that reaches its victim, a hand on a door handle.", false },
        "i:exec:,i:actor:Target,i:name:Warp Target Name,i:vector:Location,i:rotator:Rotation,o:exec:" },
    [BPK_REMOVE_WARP_TARGET] = { { "Remove Warp Target", "Animation|Motion Warping", "Forgets Target's Warp Target of this name (empty: all of Target's): its windows play the clip as it is.", false },
        "i:exec:,i:actor:Target,i:name:Warp Target Name,o:exec:" },
    [BPK_PLAY_PAIRED_MONTAGE] = { { "Play Paired Montage", "Animation|Montage", "Primary plays Montage and Partner its partner montage (set in the montage) on the same frame, at the same rate: Partner slides into the place the montage gives it from Primary (Align Time) and stays locked to it, even as Primary's root motion moves it. A takedown, a grab, a handshake, a finisher.", false },
        "i:exec:,i:actor:Primary,i:actor:Partner,i:name:Montage,i:float:Play Rate,o:exec:,o:bool:Return Value" },
    [BPK_TRY_TRAVERSAL] = { { "Try Traversal", "Animation|Traversal", "Looks at what is ahead of the Character (Target) with traces - its front, its top, its depth, the floor behind it, room for the body - and, when it can, vaults over it (low and thin, the floor below behind it), mantles onto it (low) or climbs onto it (high, up to Max Height cm), its hands on this ledge and its feet on this floor by Motion Warping. Max Distance: how far ahead it looks (cm, 0: 150). Return Value: it went; Action: None, Vault, Mantle or Climb; Height: the obstacle's. Draw Debug: the traces for 2 s. Usually on the jump key: Try Traversal, and Jump when it returns false.", false },
        "i:exec:,i:actor:Target,i:float:Max Distance,i:float:Max Height,i:bool:Draw Debug,o:exec:,o:bool:Return Value,o:name:Action,o:float:Height" },
    [BPK_FIND_PAIRED_MONTAGE] = { { "Find Paired Montage", "Animation|Montage", "Of the paired montages whose names start with Prefix (a set: AM_Takedown_Front, AM_Takedown_Back, AM_Takedown_Left...), the one whose partner place fits where Partner stands and faces now from Primary - within Max Distance (cm) and Max Angle (degrees; 0: no limit). Found false: none fits. Then Play Paired Montage with it.", true },
        "i:actor:Primary,i:actor:Partner,i:name:Prefix,i:float:Max Distance,i:float:Max Angle,o:bool:Found,o:name:Montage" },
    [BPK_CAN_PLAY_PAIRED] = { { "Can Play Paired Montage", "Animation|Montage", "Whether Partner stands within Max Distance (cm) and Max Angle (degrees) of its place in Montage from Primary and nothing is between them (a wall, a door): the check before a takedown or a handshake.", true },
        "i:actor:Primary,i:actor:Partner,i:name:Montage,i:float:Max Distance,i:float:Max Angle,o:bool:Return Value" },
    [BPK_MONTAGE_COMBO_INPUT] = { { "Montage Combo Input", "Animation|Montage", "The attack button while Target's montage plays: inside one of its Combo windows (or just before), kept - at the window's end the montage chains to the window's Next Section; without it the attack ends with its section. Return Value: it was taken.", false },
        "i:exec:,i:actor:Target,o:exec:,o:bool:Return Value" },
    [BPK_IN_MONTAGE_WINDOW] = { { "Is In Montage Window", "Animation|Montage", "Whether a window of this name is open in Target's montage now (a parry, an invulnerability, a cancel).", true },
        "i:actor:Target,i:name:Window,o:bool:Return Value" },
    [BPK_EVENT_MONTAGE_WINDOW] = { { "Event On Montage Window", "Animation|Events", "Runs when one of this actor's montage windows opens (Begin true) or closes (Begin false): its name.", false },
        "o:exec:,o:name:Window,o:bool:Begin" },
    [BPK_EVENT_MONTAGE_HIT] = { { "Event On Montage Hit", "Animation|Events", "Runs once for each actor a Hit window of this actor's montage touches (the sphere swept along its bone): who, where, which window. Apply Damage here.", false },
        "o:exec:,o:actor:Other Actor,o:vector:Location,o:name:Window" },
    [BPK_PLAY_HIT_REACTION] = { { "Play Hit Reaction", "Animation|Montage", "Target reacts to a hit from Hit Location: its spine and head thrown away from it and springing back (Strength: 0.3 a slap, 1 a punch, 2 a hammer), and when Montage Prefix names a set (AM_Hit_Front, AM_Hit_Back, AM_Hit_Left, AM_Hit_Right) the montage of that side plays. Return Value: the side (Front, Back, Left or Right).", false },
        "i:exec:,i:actor:Target,i:vector:Hit Location,i:float:Strength,i:name:Montage Prefix,o:exec:,o:name:Return Value" },
    [BPK_PRELOAD_LEVEL] = { { "Preload Level", "Game|Level Streaming", "Loads a level in the background without opening it: its file read in slices and its actors built apart (no BeginPlay, nothing drawn) at Load Speed (0.01 slow and light, e.g. behind a lobby; 1 as fast as it goes; 0: the Async Loading Speed). Open Level (by Name) of it later swaps it in at once, like a camera cut; not finished by then, the loading screen shows and it finishes at full speed.", false },
        "i:exec:,i:name:Level Name,i:float:Load Speed,o:exec:" },
    [BPK_SET_LEVEL_LOAD_SPEED] = { { "Set Level Load Speed", "Game|Level Streaming", "Changes a preloading level's Load Speed while it loads (0.01 .. 1): low while the lobby plays, high when everyone pressed Play. Level Name empty: every preload.", false },
        "i:exec:,i:name:Level Name,i:float:Load Speed,o:exec:" },
    [BPK_SET_ASYNC_LOAD_SPEED] = { { "Set Async Loading Speed", "Game|Level Streaming", "The speed of every background load (0.01 .. 1; 1, the default, as fast as it goes): the preloads left at their default speed and Load Stream Level's reads. Low keeps the frame smooth while something big loads behind.", false },
        "i:exec:,i:float:Load Speed,o:exec:" },
    [BPK_GET_PRELOAD_PROGRESS] = { { "Get Level Preload Progress", "Game|Level Streaming", "How far a preloading level is (0..1) and whether it is Ready (Open Level then swaps it in without a loading screen). Preloading: it was asked and hasn't failed.", true },
        "i:name:Level Name,o:float:Return Value,o:bool:Ready,o:bool:Preloading" },
    [BPK_CANCEL_PRELOAD] = { { "Cancel Level Preload", "Game|Level Streaming", "Stops preloading a level and frees what it read. Level Name empty: every preload.", false },
        "i:exec:,i:name:Level Name,o:exec:" },
    [BPK_ENTER_COVER] = { { "Enter Cover", "Character|Cover", "Takes Target (a Character) into cover against the nearest wall within Search Distance (cm, 0: 200): its back to it, glided there. In cover it moves only along the wall and stops at its ends; behind a low wall (under ~120 cm) it crouches. Return Value: a wall was found.", false },
        "i:exec:,i:actor:Target,i:float:Search Distance,o:exec:,o:bool:Return Value" },
    [BPK_EXIT_COVER] = { { "Exit Cover", "Character|Cover", "Target leaves cover: it moves freely again.", false }, "i:exec:,i:actor:Target,o:exec:" },
    [BPK_COVER_PEEK] = { { "Cover Peek", "Character|Cover", "While Peek is on, Target in cover leans out past the wall's end it stands at (to aim around it), or stands up over a low cover (to aim over it).", false },
        "i:exec:,i:actor:Target,i:bool:Peek,o:exec:" },
    [BPK_IS_IN_COVER] = { { "Is In Cover", "Character|Cover", "Whether Target is in cover; Low: behind a low wall (crouched); At Left Edge / At Right Edge: at an end of the wall (where it can peek).", true },
        "i:actor:Target,o:bool:Return Value,o:bool:Low,o:bool:At Left Edge,o:bool:At Right Edge" },
    [BPK_STOP_PAIRED_MONTAGE] = { { "Stop Paired Montage", "Animation|Montage", "Ends the paired montage Target is in (as the primary or the partner): both blend out.", false },
        "i:exec:,i:actor:Target,i:float:Blend Out,o:exec:" },
    [BPK_GET_LOCO_STYLE] = { { "Get Locomotion Style", "Animation|Motion Matching", "Target's locomotion style: 0 Normal, 1 Injured, 2 Tired, 3 Drunk, 4 Carrying (-1: no animation).", true },
        "i:actor:Target,o:int:Return Value" },
    [BPK_SET_SMART_OBJECT] = { { "Set Smart Object", "City", "Makes Target (an actor with an Interactable) a Smart Object for the crowds with City Routine: Slots at once (0: not one), open from Opens At to Closes At (hours; both 0: always).", false },
        "i:exec:,i:actor:Target,i:int:Slots,i:float:Opens At,i:float:Closes At,o:exec:" },
    [BPK_GET_VISITORS] = { { "Get Visitors", "City", "How many of the crowds are at Target (a Smart Object) or on their way now.", true },
        "i:actor:Target,o:int:Return Value" },
    [BPK_ADD_STORM] = { { "Add Storm", "Weather|Regional", "A storm at Location (a Rain actor with Regional Weather on): its Radius (cm), its peak Intensity (mm/h), for Duration seconds; it grows, holds and fades, drifting with the wind. Its index (-1: eight storms already).", false },
        "i:exec:,i:vector:Location,i:float:Radius,i:float:Intensity,i:float:Duration,o:exec:,o:int:Return Value" },
    [BPK_CLEAR_STORMS] = { { "Clear Storms", "Weather|Regional", "Every storm gone at once (the ground they wet dries by itself).", false },
        "i:exec:,o:exec:" },
    [BPK_GET_RAIN_AT] = { { "Get Rain At", "Weather|Regional", "The rain at Location now (mm/h): the storms' with Regional Weather on, else the Rain actor's intensity.", true },
        "i:vector:Location,o:float:Return Value" },
    [BPK_GET_WIND_AT] = { { "Get Wind At", "Weather|Regional", "The wind at Location now (m/s, x and y): the prevailing wind and the storms' gust fronts.", true },
        "i:vector:Location,o:vector:Return Value" },
    [BPK_GET_CLOUDS_AT] = { { "Get Cloudiness At", "Weather|Regional", "How overcast it is over Location (0 clear .. 1): the storms' clouds.", true },
        "i:vector:Location,o:float:Return Value" },
    [BPK_CROWD_ALARM] = { { "Crowd Alarm", "AI|Crowd", "A shot, an explosion at Location: the Crowd Spawners' agents within Radius (cm) hear it as a wave (15 m/s) and panic, running from it 6-12 s. At most 64 start a frame, the nearest first: a shot into thousands never wakes them all in one frame.", false },
        "i:exec:,i:vector:Location,i:float:Radius,o:exec:" },
    [BPK_EVENT_MR_ROOM_LOADED] = { { "Event On Room Loaded", "VR|Mixed Reality", "Runs in every Blueprint when the room (the headset's Space Setup, or the simulated one) is in: Count planes and pieces of furniture (0: none, Request Room Setup asks for one).", false },
        "o:exec:,o:int:Count" },
    [BPK_TIMELINE] = { { "Timeline", "Flow Control", "Timeline: curves played over time. Double-click it to edit its tracks (Float, Vector, Linear Color, Event) and keys, its Length, Loop and AutoPlay. Play / Play from Start / Reverse / Reverse from End start it, Stop stops it, Set New Time jumps to New Time. Update runs every frame it plays with each track's value; Finished when it reaches an end; an Event track's pin when the time passes its keys. Each actor keeps its own time.", false },
        "i:exec:Play,i:exec:Play from Start,i:exec:Stop,i:exec:Reverse,i:exec:Reverse from End,i:exec:Set New Time,i:float:New Time,o:exec:Update,o:exec:Finished,o:int:Direction" },
    [BPK_RAIN_PUDDLE_COLOR] = { { "Set Rain Puddle Color", "Weather|Rain", "Tints the puddles' water (what it lets through of the ground: 1, 1, 1 clear; darker or colored water).", false },
        "i:exec:,i:vector:Color,o:exec:" },
    [BPK_VEH_ENTER] = { { "Enter Vehicle", "Vehicles", "The player drives this vehicle (its pawn hides and waits; Exit Vehicle brings it back beside it). Starts the engine.", false },
        "i:exec:,i:actor:Target,o:exec:,o:bool:Success" },
    [BPK_VEH_EXIT] = { { "Exit Vehicle", "Vehicles", "The player leaves the vehicle it drives: back to its pawn, beside the vehicle.", false },
        "i:exec:,o:exec:,o:bool:Success" },
    [BPK_VEH_SPEED] = { { "Get Vehicle Speed", "Vehicles", "The vehicle's speed forward (km/h; negative backing up).", true },
        "i:actor:Target,o:float:Return Value" },
    [BPK_VEH_RPM] = { { "Get Engine RPM", "Vehicles", "The engine's revolutions per minute.", true }, "i:actor:Target,o:float:Return Value" },
    [BPK_VEH_GET_GEAR] = { { "Get Current Gear", "Vehicles", "The gear engaged (-1 reverse, 0 neutral, 1..).", true }, "i:actor:Target,o:int:Return Value" },
    [BPK_PS_IS_ACTIVE] = { { "Is System Active", "Effects|Particles", "True while a Particle System actor is emitting.", true },
        "i:actor:Target,o:bool:Return Value" },
    // Steam (steam.h): steam_api64.dll next to the game, the Steam client running; without it they do nothing
    [BPK_STEAM_AVAILABLE] = { { "Is Steam Available", "Steam", "True when Steam is up for this game (the DLL, the client, the App ID).", true }, "o:bool:Return Value" },
    [BPK_STEAM_NAME] = { { "Get Steam Name", "Steam", "The player's Steam name.", true }, "o:string:Return Value" },
    [BPK_STEAM_ID] = { { "Get Steam ID", "Steam", "The player's SteamID (as text).", true }, "o:string:Return Value" },
    [BPK_STEAM_UNLOCK] = { { "Unlock Achievement", "Steam|Achievements", "Unlocks a Steamworks achievement by its API name (stored at once).", false }, "i:exec:,i:string:Achievement,o:exec:" },
    [BPK_STEAM_CLEAR] = { { "Clear Achievement", "Steam|Achievements", "Locks an achievement again (testing).", false }, "i:exec:,i:string:Achievement,o:exec:" },
    [BPK_STEAM_IS_UNLOCKED] = { { "Is Achievement Unlocked", "Steam|Achievements", "True when the player has the achievement.", true }, "i:string:Achievement,o:bool:Return Value" },
    [BPK_STEAM_PROGRESS] = { { "Show Achievement Progress", "Steam|Achievements", "Steam shows the progress toward an achievement (\"3 / 10\").", false }, "i:exec:,i:string:Achievement,i:int:Current,i:int:Max,o:exec:" },
    [BPK_STEAM_SET_STAT] = { { "Set Stat", "Steam|Stats", "A Steamworks stat (Integer: an INT stat; else FLOAT). Store Stats sends them.", false }, "i:exec:,i:string:Stat,i:float:Value,i:bool:Integer,o:exec:" },
    [BPK_STEAM_GET_STAT] = { { "Get Stat", "Steam|Stats", "A Steamworks stat's value.", true }, "i:string:Stat,i:bool:Integer,o:float:Return Value" },
    [BPK_STEAM_STORE_STATS] = { { "Store Stats", "Steam|Stats", "Sends the stats and achievements changed to Steam.", false }, "i:exec:,o:exec:" },
    [BPK_STEAM_UPLOAD_SCORE] = { { "Upload Leaderboard Score", "Steam|Leaderboards", "A score to a leaderboard (made when missing: descending). Keep Best: only a better one replaces the player's.", false },
                                 "i:exec:,i:string:Leaderboard,i:int:Score,i:bool:Keep Best,o:exec:" },
    [BPK_STEAM_DOWNLOAD_SCORES] = { { "Download Leaderboard", "Steam|Leaderboards", "Fetches entries (Kind 0: the top, 1: around the player, 2: friends). Event On Leaderboard Downloaded, then Get Leaderboard Entry ...", false },
                                    "i:exec:,i:string:Leaderboard,i:int:Kind,i:int:Count,o:exec:" },
    [BPK_STEAM_SCORE_COUNT] = { { "Get Leaderboard Entry Count", "Steam|Leaderboards", "The entries the last download got.", true }, "o:int:Return Value" },
    [BPK_STEAM_SCORE_NAME] = { { "Get Leaderboard Entry Name", "Steam|Leaderboards", "An entry's player name.", true }, "i:int:Index,o:string:Return Value" },
    [BPK_STEAM_SCORE_VALUE] = { { "Get Leaderboard Entry Score", "Steam|Leaderboards", "An entry's score.", true }, "i:int:Index,o:int:Return Value" },
    [BPK_STEAM_SCORE_RANK] = { { "Get Leaderboard Entry Rank", "Steam|Leaderboards", "An entry's global rank.", true }, "i:int:Index,o:int:Return Value" },
    [BPK_STEAM_OVERLAY] = { { "Open Steam Overlay", "Steam", "Opens the Steam overlay on a page: friends, community, players, settings, achievements, stats.", false }, "i:exec:,i:string:Page,o:exec:" },
    [BPK_STEAM_INVITE] = { { "Invite Friends", "Steam", "The overlay's invite dialog for the current session (its Steam lobby).", false }, "i:exec:,o:exec:" },
    [BPK_STEAM_PRESENCE] = { { "Set Rich Presence", "Steam", "What friends see the player doing (key \"status\", or your Steamworks localization's keys).", false }, "i:exec:,i:string:Key,i:string:Value,o:exec:" },
    [BPK_STEAM_DLC] = { { "Is DLC Installed", "Steam", "True when the player owns and installed the DLC (its App ID).", true }, "i:int:App ID,o:bool:Return Value" },
    [BPK_STEAM_LANGUAGE] = { { "Get Steam Language", "Steam", "The game's language as chosen in Steam (\"english\", \"brazilian\"...).", true }, "o:string:Return Value" },
    [BPK_STEAM_FRIEND_COUNT] = { { "Get Friend Count", "Steam|Friends", "The player's Steam friends.", true }, "o:int:Return Value" },
    [BPK_STEAM_FRIEND_NAME] = { { "Get Friend Name", "Steam|Friends", "A friend's name.", true }, "i:int:Index,o:string:Return Value" },
    [BPK_STEAM_CLOUD_ENABLED] = { { "Is Steam Cloud Enabled", "Steam|Cloud", "True when Steam Cloud keeps this game's saves (the player's setting and the game's).", true }, "o:bool:Return Value" },
    [BPK_EVENT_STEAM_LEADERBOARD] = { { "Event On Leaderboard Downloaded", "Steam|Leaderboards", "Runs when Download Leaderboard got its entries.", false }, "o:exec:,o:int:Count" },
    [BPK_EVENT_STEAM_OVERLAY] = { { "Event On Steam Overlay", "Steam", "Runs when the Steam overlay opens or closes (pause the game).", false }, "o:exec:,o:bool:Shown" },
    // Saves (online.c)
    [BPK_SAVE_GAME] = { { "Save Game to Slot", "Game|Save Game", "Saves the variables (numbers, vectors, texts) of a Save Game object (Create Save Game Object) or of an actor (and its Inventory) in a slot (Saved/SaveGames, checked by a checksum), and in Steam Cloud when on (Project Settings > Online). The whole progress: Save Progress.", false },
                        "i:exec:,i:actor:Target,i:string:Slot Name,o:exec:,o:bool:Success" },
    [BPK_LOAD_GAME] = { { "Load Game from Slot", "Game|Save Game", "Gives the Save Game object or the actor back the variables of a slot (Steam Cloud's first when on). Success false: no slot, or a damaged file (refused). Older saves open too.", false }, "i:exec:,i:actor:Target,i:string:Slot Name,o:exec:,o:bool:Success" },
    [BPK_SAVE_EXISTS] = { { "Does Save Game Exist", "Game|Save Game", "True when the slot was saved.", true }, "i:string:Slot Name,o:bool:Return Value" },
    [BPK_DELETE_SAVE] = { { "Delete Game in Slot", "Game|Save Game", "Deletes a saved slot (and its Steam Cloud copy).", false }, "i:exec:,i:string:Slot Name,o:exec:" },
    [BPK_SHOW_MOUSE] = { { "Set Show Mouse Cursor", "User Interface", "Frees the mouse for the widgets' Buttons (the view stops turning) or captures it again.", false },
                         "i:exec:,i:bool:Show,o:exec:" },
    [BPK_REROUTE] = { { "Add Reroute Node", "Utilities", "A knot on a wire to organize the graph: wires pass through it (any type, exec too).", true }, "i:wild:,o:wild:" },
    [BPK_EVENT_BEGIN_OVERLAP] = { { "Event ActorBeginOverlap", "Collision", "Runs when another actor starts touching this actor's overlap shapes (Box / Sphere / Capsule Collision with Generate Overlap Events, a Trigger). In the Level Blueprint: of the actor named in Details.", false }, "o:exec:,o:actor:Other Actor" },
    [BPK_EVENT_END_OVERLAP] = { { "Event ActorEndOverlap", "Collision", "Runs when another actor stops touching this actor's overlap shapes. In the Level Blueprint: of the actor named in Details.", false }, "o:exec:,o:actor:Other Actor" },
    [BPK_EVENT_OVERLAPPING] = { { "Event ActorOverlapping", "Collision", "Runs every frame while another actor keeps touching this actor's overlap shapes. In the Level Blueprint: of the actor named in Details.", false }, "o:exec:,o:actor:Other Actor" },
    [BPK_IS_OVERLAPPING] = { { "Is Overlapping Actor", "Collision", "Whether Target touches Other now (Target's overlap shapes).", true }, "i:actor:Target,i:actor:Other,o:bool:" },
    [BPK_DISPATCH_CALL] = { { "Call", "Event Dispatchers", "Calls Target's Event Dispatcher: every event bound to it runs, with these inputs.", false }, "i:exec:,i:actor:Target,o:exec:" },
    [BPK_DISPATCH_BIND] = { { "Bind Event to", "Event Dispatchers", "Binds this Blueprint's event of the dispatcher to Target's: when Target calls it, the event runs here.", false }, "i:exec:,i:actor:Target,o:exec:" },
    [BPK_DISPATCH_UNBIND] = { { "Unbind Event from", "Event Dispatchers", "This Blueprint's event no longer runs when Target calls the dispatcher.", false }, "i:exec:,i:actor:Target,o:exec:" },
    [BPK_DISPATCH_UNBIND_ALL] = { { "Unbind all Events from", "Event Dispatchers", "No event runs any more when Target calls the dispatcher.", false }, "i:exec:,i:actor:Target,o:exec:" },
    [BPK_EVENT_DISPATCH] = { { "Event (Dispatcher)", "Event Dispatchers", "Runs when the dispatcher it is bound to (Bind Event to) is called; its outputs: the call's inputs.", false }, "o:exec:" },
    // Inventory and Items (inventory.h): the item is its Item Definition's name ("DA_Potion"); Target unconnected is self
    [BPK_INV_ADD] = { { "Add Item", "Inventory", "Puts Count of the item into the Target's Inventory: its stacks fill first (up to the item's Max Stack), then empty slots. Remainder: what didn't fit (no room or over Max Weight); Success: all of it went in.", false },
                      "i:exec:,i:actor:Target,i:name:Item,i:int:Count,o:exec:,o:bool:Success,o:int:Remainder" },
    [BPK_INV_REMOVE] = { { "Remove Item", "Inventory", "Takes up to Count of the item out of the Target's Inventory (from its last stacks). Removed: how many came out.", false },
                         "i:exec:,i:actor:Target,i:name:Item,i:int:Count,o:exec:,o:int:Removed" },
    [BPK_INV_HAS] = { { "Has Item", "Inventory", "Whether the Target's Inventory holds at least Count of the item (in its slots; the equipped: Get Equipped Item).", true },
                      "i:actor:Target,i:name:Item,i:int:Count,o:bool:Return Value" },
    [BPK_INV_COUNT] = { { "Get Item Count", "Inventory", "How many of the item the Target's Inventory holds (every stack of it).", true },
                        "i:actor:Target,i:name:Item,o:int:Return Value" },
    [BPK_INV_USE] = { { "Use Item", "Inventory", "Uses the item: Event On Item Used runs on the Target (heal, open, read...); a consumable (tagged Item.Consumable) spends one. False: it has none.", false },
                      "i:exec:,i:actor:Target,i:name:Item,o:exec:,o:bool:Success" },
    [BPK_INV_EQUIP] = { { "Equip Item", "Inventory", "Moves one of the item from the slots onto its equipment slot (its Equip Slot: Right Hand, Head...); its World Mesh hangs on the socket or bone of the Target's Skeletal Mesh. What was there goes back to the slots.", false },
                        "i:exec:,i:actor:Target,i:name:Item,o:exec:,o:bool:Success" },
    [BPK_INV_UNEQUIP] = { { "Unequip Item", "Inventory", "Takes off what is on the equipment slot (Right Hand, Left Hand, Head, Chest, Legs, Feet, Back, Accessory) back into the slots. False: nothing there or no room.", false },
                          "i:exec:,i:actor:Target,i:name:Slot Name,o:exec:,o:bool:Success" },
    [BPK_INV_DROP] = { { "Drop Item", "Inventory", "Takes Count of the item out and lays it in the world in front of the Target as a Pickup (taken again by walking onto it once the Target stepped away).", false },
                       "i:exec:,i:actor:Target,i:name:Item,i:int:Count,o:exec:,o:actor:Pickup" },
    [BPK_INV_GET_SLOT] = { { "Get Slot", "Inventory", "What a slot holds (0 the first): its item (None when empty) and how many.", true },
                           "i:actor:Target,i:int:Slot Index,o:name:Item,o:int:Count,o:bool:Is Empty" },
    [BPK_INV_MOVE] = { { "Move Item", "Inventory", "Moves a slot's stack onto another slot: the same item merges (up to its Max Stack), another swaps places.", false },
                       "i:exec:,i:actor:Target,i:int:From Slot,i:int:To Slot,o:exec:,o:bool:Success" },
    [BPK_INV_FIND_TAG] = { { "Find Item by Tag", "Inventory", "The first item of the Target's Inventory with the Gameplay Tag or one under it (\"Item.Weapon\" finds \"Item.Weapon.Sword\"). Found false: none.", true },
                           "i:actor:Target,i:name:Tag,o:name:Item,o:int:Slot Index,o:bool:Found" },
    [BPK_INV_SPLIT] = { { "Split Stack", "Inventory", "Moves Count of a slot's stack into the first empty slot (half a stack of arrows to trade).", false },
                        "i:exec:,i:actor:Target,i:int:Slot Index,i:int:Count,o:exec:,o:bool:Success" },
    [BPK_INV_SORT] = { { "Sort Inventory", "Inventory", "Merges the stacks of each item and orders the slots by name; the empty ones go last.", false },
                       "i:exec:,i:actor:Target,o:exec:" },
    [BPK_INV_GET_EQUIPPED] = { { "Get Equipped Item", "Inventory", "The item on an equipment slot (Right Hand, Head...); None when nothing.", true },
                               "i:actor:Target,i:name:Slot Name,o:name:Item,o:bool:Is Equipped" },
    [BPK_INV_WEIGHT] = { { "Get Inventory Weight", "Inventory", "How heavy the Target's items are (kg: the slots and the equipment) and the most it carries (0: no limit).", true },
                         "i:actor:Target,o:float:Weight,o:float:Max Weight" },
    [BPK_INV_ITEM_INFO] = { { "Get Item Info", "Inventory", "An Item Definition's values: what the UI shows and the rules use (more of its own: Get Data Asset Value).", true },
                            "i:name:Item,o:text:Display Name,o:text:Description,o:float:Weight,o:int:Max Stack,o:int:Value,o:name:Equip Slot" },
    [BPK_INV_PICK_UP] = { { "Pick Up Item", "Inventory", "The Pickup's item into the Target's Inventory (an AI that collects, an interact key of your own). The Pickup goes (or waits to come back); what didn't fit stays in it.", false },
                          "i:exec:,i:actor:Target,i:actor:Pickup,o:exec:,o:bool:Success" },
    [BPK_INV_TRANSFER] = { { "Transfer Item", "Inventory", "Moves a slot's stack from the Target's Inventory to another actor's (a chest, a shop, a body): onto To Slot, or where it fits when To Slot is -1.", false },
                           "i:exec:,i:actor:Target,i:int:From Slot,i:actor:To,i:int:To Slot,o:exec:,o:bool:Success" },
    [BPK_INV_SLOT_COUNT] = { { "Get Slot Count", "Inventory", "How many slots the Target's Inventory has (0: no Inventory): for a loop over Get Slot.", true },
                             "i:actor:Target,o:int:Return Value" },
    [BPK_EVENT_INV_CHANGED] = { { "Event On Inventory Changed", "Inventory|Events", "Runs once in a frame when this actor's Inventory changed (added, removed, moved, equipped, loaded): refresh a HUD.", false }, "o:exec:" },
    [BPK_EVENT_ITEM_USED] = { { "Event On Item Used", "Inventory|Events", "Runs when Use Item (or a right click in WBP_Inventory) uses one of this actor's items: what the item does.", false },
                              "o:exec:,o:name:Item" },
    [BPK_EVENT_ITEM_EQUIPPED] = { { "Event On Item Equipped", "Inventory|Events", "Runs when an item goes onto one of this actor's equipment slots (Equip Item).", false },
                                  "o:exec:,o:name:Item,o:name:Slot Name" },
    [BPK_EVENT_ITEM_UNEQUIPPED] = { { "Event On Item Unequipped", "Inventory|Events", "Runs when an item leaves one of this actor's equipment slots (Unequip Item, or another equipped there).", false },
                                    "o:exec:,o:name:Item,o:name:Slot Name" },
    // Pause and options (game_settings.h, notas-dev/pausa-opcoes.md)
    [BPK_SET_GAME_PAUSED] = { { "Set Game Paused", "Game", "Pauses or resumes the game: the world, physics, animation, particles, AI and the game's sounds stop; the widgets, the UI sounds and the actors with Tick When Paused go on. Return Value: whether it changed.", false },
                              "i:exec:,i:bool:Paused,o:exec:,o:bool:Return Value" },
    [BPK_IS_GAME_PAUSED] = { { "Is Game Paused", "Game", "True while the game is paused (Set Game Paused, the pause menu, the editor's Pause).", true }, "o:bool:Return Value" },
    [BPK_SET_TICK_PAUSED] = { { "Set Tickable When Paused", "Game", "Whether the Target actor (none: self) goes on ticking, its Delays and timers counting, its keys heard, while the game is paused (Details > Blueprint > Tick When Paused).", false },
                              "i:exec:,i:actor:Target,i:bool:Tickable When Paused,o:exec:" },
    [BPK_GET_GUS] = { { "Get Game User Setting", "Game|Settings", "One of the player's options by name: Scalability, Resolution, WindowMode, VSync, FrameRateLimit, ResolutionScale, MasterVolume, MusicVolume, SFXVolume, VoiceVolume, MouseSensitivity, InvertY, Language, Key:<Action> (Key:IA_Jump, Key:IA_Jump:Gamepad). Value: its number; Text: as text (\"1920x1080\", \"pt_BR\", \"Space\").", true },
                      "i:name:Setting,o:float:Value,o:string:Text" },
    [BPK_SET_GUS] = { { "Set Game User Setting", "Game|Settings", "Changes an option by name (Get Game User Setting has the names): Text when given (\"High\", \"1920x1080\", \"pt_BR\", a key's name), else Value. The volumes, keys, sensitivity, language and preset apply at once; the window's with Apply Settings. Return Value false: no such setting.", false },
                      "i:exec:,i:name:Setting,i:float:Value,i:string:Text,o:exec:,o:bool:Return Value" },
    [BPK_APPLY_GUS] = { { "Apply Settings", "Game|Settings", "Applies every option (the window's resolution and mode, VSync too) and saves them in GameUserSettings.ini: the next time the game opens they are read back.", false }, "i:exec:,o:exec:" },
    [BPK_LOAD_GUS] = { { "Load Settings", "Game|Settings", "Reads the saved options again (what was set since the last Apply Settings is dropped) and applies them.", false }, "i:exec:,o:exec:" },
    [BPK_DEFAULTS_GUS] = { { "Set Settings to Defaults", "Game|Settings", "Every option back to the project's defaults (the keys too), applied at once; saved with Apply Settings.", false }, "i:exec:,o:exec:" },
    [BPK_REMAP_KEY] = { { "Remap Action Key", "Input|Settings", "The player's key for an Enhanced Input action: the action's mapping of Key (empty: its first keyboard key) now reads New Key (empty: back to its own). Saved with Apply Settings.", false },
                        "i:exec:,i:name:Action,i:name:Key,i:name:New Key,o:exec:,o:bool:Return Value" },
    [BPK_GET_ACTION_KEY] = { { "Get Action Key", "Input|Settings", "The key an Enhanced Input action is on now (the player's or its mapping's): its first keyboard / mouse key, or its first gamepad button.", true },
                             "i:name:Action,i:bool:Gamepad,o:string:Return Value" },
    [BPK_OPEN_PAUSE_MENU] = { { "Open Pause Menu", "Game|Pause Menu", "Shows the pause menu (Project Settings > Pause Menu: WBP_PauseMenu by default), pauses the game and frees the cursor. Esc in the game (the Play: its key) and a gamepad's Start do it too.", false }, "i:exec:,o:exec:" },
    [BPK_CLOSE_PAUSE_MENU] = { { "Close Pause Menu", "Game|Pause Menu", "Closes the pause menu and its Options, resumes the game and gives the cursor back as it was.", false }, "i:exec:,o:exec:" },
    [BPK_MAIN_MENU] = { { "Return to Main Menu", "Game|Pause Menu", "Closes the pause menu and opens the Main Menu Map (Project Settings > Maps & Modes; none: the Game Default Map, else this level again).", false }, "i:exec:,o:exec:" },
    [BPK_QUIT_GAME] = { { "Quit Game", "Game", "Ends the game (the editor's Play: stops it).", false }, "i:exec:,o:exec:" },
    // Save and load the progress (save_progress.h, notas-dev/salvar-progresso.md)
    [BPK_CREATE_SAVE_OBJECT] = { { "Create Save Game Object", "Game|Save Game", "Makes an object of a Save Game class (a Blueprint whose parent is Save Game): its variables are what Save Game to Slot keeps. Cast it to the class to set and read them.", false },
                                 "i:exec:,i:class:Save Game Class,o:exec:,o:actor:Return Value" },
    [BPK_GET_PROGRESS_SAVE] = { { "Get Progress Save Game", "Game|Save Game", "The progress's Save Game object (Project Settings > Save Game Class, or the one Set Progress Save Game gave): it goes from level to level and into every Save Progress, and Load Progress gives its variables back. None: no class.", true },
                                "o:actor:Return Value" },
    [BPK_SET_PROGRESS_SAVE] = { { "Set Progress Save Game", "Game|Save Game", "Makes this Save Game object the progress's: kept between levels, saved by Save Progress.", false },
                                "i:exec:,i:actor:Save Game Object,o:exec:" },
    [BPK_SAVE_PROGRESS] = { { "Save Progress", "Game|Save Game", "Saves the whole progress in a slot: the level, the player (place, Health, Inventory), every Saveable actor, the destroyed ones, the Progress Save Game's variables, the checkpoint, the play time, the date and a picture of the screen. Display Name: what the slot list shows (empty: the level's name). Steam Cloud too when on.", false },
                            "i:exec:,i:string:Slot Name,i:string:Display Name,o:exec:,o:bool:Success" },
    [BPK_LOAD_PROGRESS] = { { "Load Progress", "Game|Save Game", "Opens the slot's level and puts back what was saved (before the level's BeginPlay; the player right after). Success false: no such slot, or a damaged file (refused, nothing changes).", false },
                            "i:exec:,i:string:Slot Name,o:exec:,o:bool:Success" },
    [BPK_NEW_GAME] = { { "New Game", "Game|Save Game", "Starts over: the play time, the checkpoint and the Progress Save Game cleared, then Level Name opened (empty: this level again). The slots stay.", false },
                       "i:exec:,i:name:Level Name,o:exec:" },
    [BPK_AUTO_SAVE] = { { "Auto Save", "Game|Save Game", "Saves the progress now into the next auto save slot (AutoSave_1, _2...: Project Settings > Auto Save Slots, in turn). Not while the player is dead. On events: a door opened, a boss beaten.", false },
                        "i:exec:,o:exec:,o:bool:Success" },
    [BPK_LIST_SAVE_SLOTS] = { { "List Save Slots", "Game|Save Game", "The saved slots, the newest first: the progress ones (Include All: the Save Game to Slot ones too). With Get Save Slot Info for a menu.", false },
                              "i:exec:,i:bool:Include All,o:exec:,o:string[]:Slot Names,o:int:Count" },
    [BPK_GET_SLOT_INFO] = { { "Get Save Slot Info", "Game|Save Game", "A slot's details from its header: the name shown, its level, when (\"2026-11-08 21:45\"), the play time (seconds and \"1:02:03\"), whether it is an auto save. Return Value false: missing or damaged.", true },
                            "i:string:Slot Name,o:bool:Return Value,o:string:Display Name,o:string:Level Name,o:string:Date Time,o:float:Play Time,o:string:Play Time Text,o:bool:Is Auto Save" },
    [BPK_GET_SLOT_NAME] = { { "Get Save Slot Name", "Game|Save Game", "The Index-th progress slot, the newest first (0: the newest; \"\" past the last).", true }, "i:int:Index,o:string:Return Value" },
    [BPK_GET_LATEST_SLOT] = { { "Get Latest Save Slot", "Game|Save Game", "The newest progress slot (Continue): auto saves and checkpoints count. Found false: none yet.", true }, "o:string:Return Value,o:bool:Found" },
    [BPK_GET_PLAY_TIME] = { { "Get Play Time", "Game|Save Game", "Seconds played in this progress (not while paused; a load brings its time back) and as text (\"1:02:03\").", true }, "o:float:Return Value,o:string:Text" },
    [BPK_GET_CURRENT_SLOT] = { { "Get Current Save Slot", "Game|Save Game", "The slot last saved or loaded (\"\": none yet).", true }, "o:string:Return Value" },
    [BPK_SET_AUTO_SAVE_INTERVAL] = { { "Set Auto Save Interval", "Game|Save Game", "Saves the progress every Seconds (0: no timed auto save), in place of Project Settings' interval until play stops.", false },
                                     "i:exec:,i:float:Seconds,o:exec:" },
    [BPK_RESPAWN_CHECKPOINT] = { { "Respawn at Checkpoint", "Game|Save Game", "Brings the player back to the last Checkpoint (none: where the level started it). Reload Save: the checkpoint's save is loaded (the world as it was then); else it is only moved there and Revived.", false },
                                 "i:exec:,i:actor:Player,i:bool:Reload Save,o:exec:,o:bool:Return Value" },
    [BPK_GET_CHECKPOINT] = { { "Get Last Checkpoint", "Game|Save Game", "The last Checkpoint the player reached (it is saved with the progress).", true }, "o:actor:Return Value,o:bool:Has Checkpoint" },
    [BPK_SET_CHECKPOINT] = { { "Set Checkpoint", "Game|Save Game", "Makes a Checkpoint actor the respawn point, as if the player reached it (no save).", false }, "i:exec:,i:actor:Checkpoint,o:exec:" },
    [BPK_OPEN_SAVE_MENU] = { { "Open Save Slots Menu", "Game|Save Game", "Shows WBP_SaveSlots (Content/UI): Continue, the slots with their pictures (Load, Delete), New Game; Save Mode: Save and Overwrite instead. The mouse is freed.", false },
                             "i:exec:,i:bool:Save Mode,o:exec:,o:bool:Return Value" },
    [BPK_EVENT_GAME_SAVED] = { { "Event On Game Saved", "Game|Save Game", "Runs in every Blueprint after the progress was saved (Save Progress, an auto save, a Checkpoint): the slot and whether it was automatic.", false },
                               "o:exec:,o:string:Slot Name,o:bool:Is Auto Save" },
    [BPK_EVENT_GAME_LOADED] = { { "Event On Game Loaded", "Game|Save Game", "Runs in every Blueprint once a Load Progress put the world back (after BeginPlay): the slot.", false }, "o:exec:,o:string:Slot Name" },
    [BPK_EVENT_CHECKPOINT] = { { "Event On Checkpoint Reached", "Game|Save Game", "Runs in every Blueprint when the player walks into a Checkpoint (the first time, or each time without Once).", false },
                               "o:exec:,o:actor:Checkpoint,o:actor:Player" },
    // Interaction (interaction.h, notas-dev/interacao.md)
    [BPK_IA_INTERACT] = { { "Interact", "Interaction", "The Target (an actor with an Interactor; none: self) uses Interactable (none: what it has in focus), as the player's key does: its condition checked, Event On Interact on it, a Door / Chest / Lever / Button does its part. Success false: disabled, used, locked.", false },
                          "i:exec:,i:actor:Target,i:actor:Interactable,o:exec:,o:bool:Success" },
    [BPK_IA_GET_FOCUSED] = { { "Get Focused Interactable", "Interaction", "What the Target's Interactor (none: self, else the player) has in focus now: the Interactable in view and in reach the prompt is about.", true },
                             "i:actor:Target,o:actor:Return Value,o:bool:Has Focus" },
    [BPK_IA_SET_ENABLED] = { { "Set Interaction Enabled", "Interaction", "Turns the Target's Interactable (none: self) on or off: off, it is neither focused nor used. Enabled again, a use-once one can be used again.", false },
                             "i:exec:,i:actor:Target,i:bool:Enabled,o:exec:" },
    [BPK_IA_SET_PROMPT] = { { "Set Prompt Text", "Interaction", "The text the prompt shows for the Target's Interactable (none: self): \"Talk to {0}\" ({0}: its Name). Empty: the device's verb (Open, Close, Pull, Press).", false },
                            "i:exec:,i:actor:Target,i:text:Prompt Text,o:exec:" },
    [BPK_IA_GET_PROMPT] = { { "Get Interaction Prompt", "Interaction", "The prompt of the Target's Interactor (none: self, else the player): its text (\"Open Door\", \"Locked (Red Key)\"), the key (the keyboard's or the gamepad's, the one used last), the hold's progress (0-1), whether something is in focus and whether it can be used now. WBP_InteractPrompt reads it.", true },
                            "i:actor:Target,o:text:Prompt,o:text:Key,o:float:Hold Progress,o:bool:Has Focus,o:bool:Can Interact" },
    [BPK_IA_DEVICE_SET] = { { "Set Device Active", "Interaction", "Opens or closes a Door or Chest, turns a Lever on or off, presses a Button (the Target; none: self), without its condition: a puzzle's script, a cinematic.", false },
                            "i:exec:,i:actor:Target,i:bool:Active,o:exec:" },
    [BPK_IA_DEVICE_IS] = { { "Is Device Active", "Interaction", "Whether the Target's Door or Chest is open, its Lever on, its Button in.", true }, "i:actor:Target,o:bool:Return Value" },
    [BPK_EVENT_IA_INTERACT] = { { "Event On Interact", "Interaction|Events", "Runs when this actor's Interactable is used (the key pressed or held, the Interact node): who used it.", false }, "o:exec:,o:actor:Instigator" },
    [BPK_EVENT_IA_FOCUS_BEGIN] = { { "Event On Focus Begin", "Interaction|Events", "Runs when an Interactor starts looking at this actor's Interactable (its prompt shows): light it, play a sound.", false }, "o:exec:,o:actor:Instigator" },
    [BPK_EVENT_IA_FOCUS_END] = { { "Event On Focus End", "Interaction|Events", "Runs when the Interactor looks away from this actor's Interactable (or walks out of reach).", false }, "o:exec:,o:actor:Instigator" },
    [BPK_EVENT_IA_HOLD] = { { "Event On Hold Progress", "Interaction|Events", "Runs every frame the key is held on this actor's Interactable (Hold Time above 0): the progress 0-1; 0 again when let go early.", false },
                            "o:exec:,o:float:Progress,o:actor:Instigator" },
    // Dialogue (dialogue.h, notas-dev/dialogo.md): a Dialogue asset run with a Dialogue Speaker; one conversation at a time
    [BPK_DLG_START] = { { "Start Dialogue", "Dialogue", "Starts a conversation with the Target (none: self): its Dialogue Speaker's Dialogue, or the one named in Dialogue (\"DLG_Merchant\"), with its name, camera and pause. Player: who talks (none: the possessed pawn). WBP_Dialogue shows it; false: no such Dialogue or nothing to say.", false },
                        "i:exec:,i:actor:Target,i:name:Dialogue,i:actor:Player,o:exec:,o:bool:Return Value" },
    [BPK_DLG_CHOOSE] = { { "Choose Option", "Dialogue", "Picks one of the Options shown (0 the first; the hidden ones don't count): its actions run, Event On Dialogue Choice, then the conversation goes on. False: no choice is shown.", false },
                         "i:exec:,i:int:Index,o:exec:,o:bool:Return Value" },
    [BPK_DLG_SKIP] = { { "Skip Line", "Dialogue", "The line appearing letter by letter shows whole; when it is whole, the conversation goes on (the player's click, Enter, Space, a gamepad's A).", false }, "i:exec:,o:exec:" },
    [BPK_DLG_END] = { { "End Dialogue", "Dialogue", "Ends the conversation now (Event On Dialogue End runs): the widget goes, the game resumes, the camera comes back.", false }, "i:exec:,o:exec:" },
    [BPK_DLG_IS_IN] = { { "Is In Dialogue", "Dialogue", "Whether a conversation runs, and with whom (its speaker).", true }, "o:bool:Return Value,o:actor:Speaker" },
    [BPK_DLG_SET_LANGUAGE] = { { "Set Dialogue Language", "Dialogue", "The language the conversations show: \"en\", \"pt\", \"es\" (or \"en_US\", \"pt_BR\", \"es_ES\"); empty: the game's (Options > Language; Spanish when the game follows a Spanish system).", false },
                               "i:exec:,i:name:Language,o:exec:" },
    [BPK_EVENT_DLG_LINE] = { { "Event On Dialogue Line", "Dialogue|Events", "Runs when a conversation shows a line: its Speaker (the NPC), the name shown, the Text in the language shown and its node (Line): subtitles of your own, a quest log, a sound.", false },
                             "o:exec:,o:actor:Speaker,o:string:Speaker Name,o:string:Text,o:int:Line" },
    [BPK_EVENT_DLG_CHOICE] = { { "Event On Dialogue Choice", "Dialogue|Events", "Runs when the player picks an Option: the Speaker, the Option's index among those shown and its Text.", false },
                               "o:exec:,o:actor:Speaker,o:int:Option,o:string:Text" },
    [BPK_EVENT_DLG_END] = { { "Event On Dialogue End", "Dialogue|Events", "Runs when a conversation ends (an End node, nothing more to say, End Dialogue): the Speaker and the Dialogue's name.", false },
                            "o:exec:,o:actor:Speaker,o:name:Dialogue" },
    // Quests (quest.h, notas-dev/missoes.md): a Quest asset in a Quest Log (the player's, or a Game State's)
    [BPK_QST_START] = { { "Start Quest", "Quest", "Starts the Quest (its asset's name, \"Q_LostAmulet\") in the Quest Log (none: the player's, else the first one, else the game's own): its first stage's objectives start counting, Event On Quest Started runs, the tracker shows it. False: no such quest, already active, or completed (and not Repeatable).", false },
                        "i:exec:,i:name:Quest,i:actor:Quest Log,o:exec:,o:bool:Return Value" },
    [BPK_QST_COMPLETE] = { { "Complete Quest", "Quest", "Completes an active Quest now, whatever its objectives: its rewards (items, tags, the Custom Event), Event On Quest Completed, then its Next Quest.", false },
                           "i:exec:,i:name:Quest,i:actor:Quest Log,o:exec:,o:bool:Return Value" },
    [BPK_QST_FAIL] = { { "Fail Quest", "Quest", "Fails an active Quest (Event On Quest Failed): its objectives stop. Start Quest can try it again.", false },
                       "i:exec:,i:name:Quest,i:actor:Quest Log,o:exec:,o:bool:Return Value" },
    [BPK_QST_ADVANCE] = { { "Advance Objective", "Quest", "Adds Amount (0: 1) to an objective of an active Quest (its id, \"find_key\", or its index, \"0\" the first): a Manual objective, or any other one by hand. Completed: true once it reached its count (its stage then moves on).", false },
                          "i:exec:,i:name:Quest,i:name:Objective,i:int:Amount,i:actor:Quest Log,o:exec:,o:bool:Completed" },
    [BPK_QST_SET_PROGRESS] = { { "Set Objective Progress", "Quest", "Sets an objective's progress (0 to its count) of an active Quest: Completed when it reaches the count.", false },
                               "i:exec:,i:name:Quest,i:name:Objective,i:int:Progress,i:actor:Quest Log,o:exec:,o:bool:Completed" },
    [BPK_QST_IS_ACTIVE] = { { "Is Quest Active", "Quest", "Whether the Quest is started and not yet completed nor failed.", true }, "i:name:Quest,i:actor:Quest Log,o:bool:Return Value" },
    [BPK_QST_IS_COMPLETED] = { { "Is Quest Completed", "Quest", "Whether the Quest was completed (it stays so: saved with the progress).", true }, "i:name:Quest,i:actor:Quest Log,o:bool:Return Value" },
    [BPK_QST_GET_STATE] = { { "Get Quest State", "Quest", "The Quest's state: 0 Not Started, 1 Active, 2 Completed, 3 Failed (and its name), and the stage it is on (0 the first).", true },
                            "i:name:Quest,i:actor:Quest Log,o:int:Return Value,o:name:State Name,o:int:Stage" },
    [BPK_QST_GET_PROGRESS] = { { "Get Objective Progress", "Quest", "An objective's progress and count (\"2\" of \"5\" wolves), whether it is completed and its text in the game's language.", true },
                               "i:name:Quest,i:name:Objective,i:actor:Quest Log,o:int:Progress,o:int:Count,o:bool:Completed,o:text:Text" },
    [BPK_QST_TRACK] = { { "Track Quest", "Quest", "The Quest the tracker (WBP_QuestTracker) and the markers show; empty: none. False: not an active quest.", false },
                        "i:exec:,i:name:Quest,i:actor:Quest Log,o:exec:,o:bool:Return Value" },
    [BPK_QST_GET_TRACKED] = { { "Get Tracked Quest", "Quest", "The quest the tracker shows (its name; empty: none) and its title in the game's language.", true },
                              "i:actor:Quest Log,o:name:Return Value,o:text:Title" },
    [BPK_EVENT_QST_STARTED] = { { "Event On Quest Started", "Quest|Events", "Runs in every Blueprint when a quest starts (Start Quest, a Quest Log's starting quests, a Next Quest): its name and the Quest Log's actor.", false },
                                "o:exec:,o:name:Quest,o:actor:Quest Log" },
    [BPK_EVENT_QST_OBJ_UPDATED] = { { "Event On Objective Updated", "Quest|Events", "Runs in every Blueprint when an objective's progress changes (a wolf killed, an herb picked): the quest, the objective's id and index, its progress and count.", false },
                                    "o:exec:,o:name:Quest,o:name:Objective,o:int:Index,o:int:Progress,o:int:Count" },
    [BPK_EVENT_QST_OBJ_COMPLETED] = { { "Event On Objective Completed", "Quest|Events", "Runs in every Blueprint when an objective reaches its count: the quest, the objective's id and index.", false },
                                      "o:exec:,o:name:Quest,o:name:Objective,o:int:Index" },
    [BPK_EVENT_QST_COMPLETED] = { { "Event On Quest Completed", "Quest|Events", "Runs in every Blueprint when a quest completes (after its rewards were given): its name and the Quest Log's actor.", false },
                                  "o:exec:,o:name:Quest,o:actor:Quest Log" },
    [BPK_EVENT_QST_FAILED] = { { "Event On Quest Failed", "Quest|Events", "Runs in every Blueprint when a quest fails (Fail Quest, its time limit): its name and the Quest Log's actor.", false },
                               "o:exec:,o:name:Quest,o:actor:Quest Log" },
    // Game Rules (game_rules.h, notas-dev/regras-de-jogo.md)
    [BPK_GR_GET_PHASE] = { { "Get Match Phase", "Game|Game Rules", "The match's phase: 0 Waiting, 1 Countdown, 2 Playing, 3 Round Over, 4 Game Over; its name; the seconds the phase has left (the countdown's 3, 2, 1); the round and how many.", true },
                           "o:int:Return Value,o:string:Phase Name,o:float:Phase Time Left,o:int:Round,o:int:Rounds" },
    [BPK_GR_SET_PHASE] = { { "Set Match Phase", "Game|Game Rules", "Goes to a phase now (0 Waiting, 1 Countdown, 2 Playing, 3 Round Over: the round lost when nobody won it, 4 Game Over). From Round Over, Playing starts the next round. On the server (or offline).", false },
                           "i:exec:,i:int:Phase,o:exec:,o:bool:Success" },
    [BPK_GR_START] = { { "Start Match", "Game|Game Rules", "Waiting -> the Countdown (none: Playing). Needed when the Game Rules' Auto Start is off (a lobby, a Press Start).", false }, "i:exec:,o:exec:,o:bool:Success" },
    [BPK_GR_END] = { { "End Match", "Game|Game Rules", "The manual objective: the match over now, won or lost, with a Reason (\"Objective Complete\", \"Escaped\"). Winner: a player (its team wins too; none: the local player's side). Game Over: WBP_MatchEnd and Event On Match Won / Lost.", false },
                     "i:exec:,i:bool:Won,i:string:Reason,i:actor:Winner,o:exec:,o:bool:Success" },
    [BPK_GR_ADD_SCORE] = { { "Add Score", "Game|Game Rules", "Points to the Target player (none, or self that is no player: the local player) and its team (negative takes away); Event On Score Changed; the Score Target checked. Kills score by themselves (Kill Points, the tags' points). On the server (or offline).", false },
                           "i:exec:,i:actor:Target,i:int:Points,o:exec:,o:int:New Score" },
    [BPK_GR_GET_SCORE] = { { "Get Score", "Game|Game Rules", "The Target player's score (none, or self that is no player: the local player), its kills, deaths and team (-1: none).", true }, "i:actor:Target,o:int:Return Value,o:int:Kills,o:int:Deaths,o:int:Team" },
    [BPK_GR_TEAM_SCORE] = { { "Get Team Score", "Game|Game Rules", "A team's score (0 the first; with one team, co-op: everyone's together) and the rounds it won.", true }, "i:int:Team,o:int:Return Value,o:int:Rounds Won" },
    [BPK_GR_TIME] = { { "Get Time Remaining", "Game|Game Rules", "The match clock: the seconds left before the limit (0: none), the seconds played this round, and the text the HUD shows (\"1:05\": what is left counting down, what was played counting up).", true },
                      "o:float:Return Value,o:float:Elapsed,o:string:Text" },
    [BPK_GR_SET_TIMER] = { { "Set Match Timer", "Game|Game Rules", "The match clock to Seconds (counting down: what is left; up: what was played), Paused or running (it also waits while the game is paused). Not Set Timer by Event: that one calls an event later.", false },
                           "i:exec:,i:float:Seconds,i:bool:Paused,o:exec:" },
    [BPK_GR_GET_LIVES] = { { "Get Lives", "Game|Game Rules", "The Target player's lives left (none, or self that is no player: the local player); Infinite when the Game Rules' Lives is 0; Out: no lives left (it waits for the next round).", true },
                           "i:actor:Target,o:int:Return Value,o:bool:Infinite,o:bool:Out" },
    [BPK_GR_ADD_LIVES] = { { "Add Lives", "Game|Game Rules", "Lives to the Target player (none, or self that is no player: the local player): an extra life pickup. A player that was out comes back (respawned after the delay).", false },
                           "i:exec:,i:actor:Target,i:int:Lives,o:exec:,o:int:Return Value" },
    [BPK_GR_RESPAWN] = { { "Respawn Player", "Game|Game Rules", "The Target player (none, or self that is no player: the local player) back now at its Player Start (or the last Checkpoint), revived and invincible for the Game Rules' seconds; Event On Player Respawned. Destroyed: a new one of its class, possessed.", false },
                         "i:exec:,i:actor:Target,o:exec:,o:bool:Success" },
    [BPK_GR_RESTART] = { { "Restart Match", "Game|Game Rules", "The match from the start: scores, lives, the clock, round 1, the players back at their starts (Waiting, then the countdown). Reload Level: the level opened again (the enemies back too).", false },
                         "i:exec:,i:bool:Reload Level,o:exec:" },
    [BPK_GR_RESULT] = { { "Get Match Result", "Game|Game Rules", "How the match (or the round) ended for the local player: Won, the Reason (\"Time Up\", \"Score Target Reached\", \"Enemies Eliminated\", \"Out of Lives\"...), the Winner (a player; none: a side) and Is Over (Game Over).", true },
                        "o:bool:Won,o:string:Reason,o:actor:Winner,o:bool:Is Over" },
    [BPK_EVENT_MATCH_PHASE] = { { "Event On Match Phase Changed", "Game|Game Rules|Events", "Runs in every Blueprint when the match changes phase (the first: Waiting): the new and the old (0 Waiting, 1 Countdown, 2 Playing, 3 Round Over, 4 Game Over) and the new one's name.", false },
                                "o:exec:,o:int:New Phase,o:int:Old Phase,o:string:Phase Name" },
    [BPK_EVENT_SCORE_CHANGED] = { { "Event On Score Changed", "Game|Game Rules|Events", "Runs in every Blueprint when a player's score changes (a kill, Add Score): the player, its score, by how much, its team and the team's score.", false },
                                  "o:exec:,o:actor:Player,o:int:Score,o:int:Delta,o:int:Team,o:int:Team Score" },
    [BPK_EVENT_MATCH_WON] = { { "Event On Match Won", "Game|Game Rules|Events", "Runs in every Blueprint at Game Over when the local player (or its team) won: the winner and why.", false }, "o:exec:,o:actor:Winner,o:string:Reason" },
    [BPK_EVENT_MATCH_LOST] = { { "Event On Match Lost", "Game|Game Rules|Events", "Runs in every Blueprint at Game Over when the local player lost (the time up, out of lives, another player's target): who won (none: nobody) and why.", false },
                               "o:exec:,o:actor:Winner,o:string:Reason" },
    [BPK_EVENT_ROUND_STARTED] = { { "Event On Round Started", "Game|Game Rules|Events", "Runs in every Blueprint when a round starts playing (after its countdown): its number (1 the first).", false }, "o:exec:,o:int:Round" },
    [BPK_EVENT_ROUND_ENDED] = { { "Event On Round Ended", "Game|Game Rules|Events", "Runs in every Blueprint when a round ends: its number, whether the local player's side won it, why.", false },
                                "o:exec:,o:int:Round,o:bool:Won,o:string:Reason" },
    [BPK_EVENT_PLAYER_RESPAWNED] = { { "Event On Player Respawned", "Game|Game Rules|Events", "Runs in every Blueprint when a player comes back (the respawn's delay over, Respawn Player): the pawn and its lives left.", false },
                                     "o:exec:,o:actor:Player,o:int:Lives Left" },
    [BPK_EVENT_TIMER_EXPIRED] = { { "Event On Timer Expired", "Game|Game Rules|Events", "Runs in every Blueprint when the match clock reaches its limit (before the Game Rules' Time Up does its part: a defeat, a victory, the most points).", false }, "o:exec:" },
    // Abilities and effects (ability.h, notas-dev/habilidades.md): an ability is its Gameplay Ability class's name
    // ("GA_Fireball"), an effect its Gameplay Effect's ("GE_Poison"); Target unconnected is self (in an ability: its owner)
    [BPK_ABIL_GIVE] = { { "Give Ability", "Abilities", "Grants the Gameplay Ability (its class: \"GA_Fireball\") to the Target's Ability System (made when it has none). Slot: its place in WBP_AbilityBar and its key (1-6). Already granted: its slot.", false },
                        "i:exec:,i:actor:Target,i:name:Ability,o:exec:,o:bool:Success,o:int:Slot" },
    [BPK_ABIL_REMOVE] = { { "Remove Ability", "Abilities", "Takes the ability away from the Target (cancelled first when it runs; the later slots move down).", false },
                          "i:exec:,i:actor:Target,i:name:Ability,o:exec:,o:bool:Success" },
    [BPK_ABIL_TRY] = { { "Try Activate Ability", "Abilities", "Activates the ability when it can: granted, not running, the owner alive and without its Blocked By Tags, a charge left (not on cooldown) and its Cost paid. Failure Reason: Not Granted, Active, Blocked, Cooldown, Cost, Dead.", false },
                       "i:exec:,i:actor:Target,i:name:Ability,o:exec:,o:bool:Success,o:name:Failure Reason" },
    [BPK_ABIL_TRY_TAG] = { { "Try Activate Ability by Tag", "Abilities", "Activates the first granted ability whose Ability Tags match the tag (\"Ability.Attack\" finds \"Ability.Attack.Fire\"). Ability: the one tried.", false },
                           "i:exec:,i:actor:Target,i:name:Tag,o:exec:,o:bool:Success,o:name:Ability" },
    [BPK_ABIL_CANCEL] = { { "Cancel Ability", "Abilities", "Stops the Target's running abilities named so, or with the tag (empty: all of them): Event On Ability Ended with Was Cancelled. Cancelled: how many.", false },
                          "i:exec:,i:actor:Target,i:name:Ability,o:exec:,o:int:Cancelled" },
    [BPK_ABIL_END] = { { "End Ability", "Abilities", "Ends the ability that runs this graph (in a Gameplay Ability; Target: an ability's object): Event On Ability Ended, its Activation Owned Tags go.", false },
                       "i:exec:,i:actor:Target,o:exec:" },
    [BPK_ABIL_OWNER] = { { "Get Ability Owner", "Abilities", "In a Gameplay Ability: the actor that has it and activated it.", true }, "i:actor:Target,o:actor:Return Value" },
    [BPK_ABIL_ON_COOLDOWN] = { { "Is Ability On Cooldown", "Abilities", "True while the ability has no charge left (waiting for its Cooldown).", true },
                               "i:actor:Target,i:name:Ability,o:bool:Return Value" },
    [BPK_ABIL_COOLDOWN] = { { "Get Cooldown Remaining", "Abilities", "Seconds until the next charge comes back (0: all there), the Cooldown, the charges left and the most it has.", true },
                            "i:actor:Target,i:name:Ability,o:float:Return Value,o:float:Duration,o:int:Charges,o:int:Max Charges" },
    [BPK_ABIL_IS_ACTIVE] = { { "Is Ability Active", "Abilities", "True while the ability runs (or casts).", true }, "i:actor:Target,i:name:Ability,o:bool:Return Value" },
    [BPK_GE_APPLY] = { { "Apply Gameplay Effect", "Abilities|Effects", "Applies the Gameplay Effect (\"GE_Poison\") to the Target (none: self): Instant changes the base values once; Has Duration and Infinite stay (their modifiers, Granted Tags, periodic Damage / Heal). Source: who did it (the damage's instigator; none: self). Stacks: how many now (0: not applied: its Required Tags).", false },
                       "i:exec:,i:actor:Target,i:name:Effect,i:actor:Source,o:exec:,o:bool:Success,o:int:Stacks" },
    [BPK_GE_REMOVE] = { { "Remove Gameplay Effect", "Abilities|Effects", "Removes the Target's active effects named so, or granting the tag (a cleanse: \"State.Poisoned\"). Stacks: how many to take off (0: all). Removed: the stacks taken.", false },
                        "i:exec:,i:actor:Target,i:name:Effect,i:int:Stacks,o:exec:,o:int:Removed" },
    [BPK_GE_HAS] = { { "Has Gameplay Effect", "Abilities|Effects", "Whether the Target has the effect (by name, or by a tag it grants), its stacks and the seconds left (-1: infinite).", true },
                     "i:actor:Target,i:name:Effect,o:bool:Return Value,o:int:Stacks,o:float:Time Remaining" },
    [BPK_ATTR_GET] = { { "Get Attribute", "Abilities|Attributes", "An attribute of the Target's Ability System (Mana, Stamina, Speed, Damage, Defense or your own): the current value (with the effects' modifiers) and the base.", true },
                       "i:actor:Target,i:name:Attribute,o:float:Return Value,o:float:Base Value,o:bool:Found" },
    [BPK_ATTR_SET] = { { "Set Attribute", "Abilities|Attributes", "Sets an attribute's base value (made when missing; X stays within 0 and MaxX when there is a MaxX). Speed is the Character's Max Walk Speed.", false },
                       "i:exec:,i:actor:Target,i:name:Attribute,i:float:Value,o:exec:" },
    [BPK_EVENT_ABIL_ACTIVATE] = { { "Event On Activate Ability", "Abilities|Events", "In a Gameplay Ability: runs when it is activated (after its Cast Time, its Cost paid). Owner: who uses it. Call End Ability when it is done.", false },
                                  "o:exec:,o:actor:Owner" },
    [BPK_EVENT_ABIL_ACTIVATED] = { { "Event On Ability Activated", "Abilities|Events", "Runs on the owner when one of its abilities acts.", false }, "o:exec:,o:name:Ability" },
    [BPK_EVENT_ABIL_ENDED] = { { "Event On Ability Ended", "Abilities|Events", "Runs on the owner (and in the ability) when one of its abilities ends; Was Cancelled: stopped (Cancel Ability, a stun in its cast, another's Cancel tags).", false },
                               "o:exec:,o:name:Ability,o:bool:Was Cancelled" },
    [BPK_EVENT_GE_APPLIED] = { { "Event On Effect Applied", "Abilities|Events", "Runs on the actor when a Gameplay Effect is applied to it: its name, its stacks now and the source.", false },
                               "o:exec:,o:name:Effect,o:int:Stacks,o:actor:Source" },
    [BPK_EVENT_GE_REMOVED] = { { "Event On Effect Removed", "Abilities|Events", "Runs on the actor when one of its effects ends (its time over, removed, cleansed).", false }, "o:exec:,o:name:Effect" },
    [BPK_EVENT_ATTR_CHANGED] = { { "Event On Attribute Changed", "Abilities|Events", "Runs on the actor when an attribute's current value changes (a cost, an effect, Set Attribute): refresh a HUD.", false },
                                 "o:exec:,o:name:Attribute,o:float:New Value,o:float:Old Value" },
    // The HUD (hud.h, notas-dev/hud.md): the pieces of WBP_GameHUD (or any widget that has them)
    // The render style (render.h; notas-dev/render.md "Estilos de render")
    [BPK_SET_RENDER_STYLE] = { { "Set Render Style", "Rendering", "Changes how the frame looks while the game runs: 0 Stylized (flat bands of light, stronger colors), 1 Realistic, 2 Photorealistic (AgX), 3 Video-realistic (AgX and the camera's lens and film). -1: back to the project's (Project Settings > Rendering). A flashback, a dream, a security camera.", false },
                               "i:exec:,i:int:Style,o:exec:" },
    [BPK_GET_RENDER_STYLE] = { { "Get Render Style", "Rendering", "The render style in use: its number (0 Stylized, 1 Realistic, 2 Photorealistic, 3 Video-realistic) and its name.", true },
                               "o:int:Style,o:string:Name" },
    [BPK_SPAWN_DECAL] = { { "Spawn Decal at Location", "Rendering|Decal", "Projects the Decal Material (Details) at a place, along Rotation's forward (X), over Decal Size (half the box: depth, width, height, in cm): a bullet hole, blood, a scorch mark. Life Span: seconds until it fades and goes (0: it stays). Returns the decal's actor.", false },
                          "i:exec:,i:vector:Location,i:rotator:Rotation,i:vector:Decal Size,i:float:Life Span,o:exec:,o:actor:Return Value" },
    [BPK_AUDIO_SET_PARAM] = { { "Set Float Parameter", "Audio|Components|Audio", "Sets an input of the Sound Graph the Audio Component plays (its name, as in the Sound Graph: \"RPM\", \"Intensity\"), while it plays: an engine's revs, the rain's strength, a music's tension. False: not playing a Sound Graph, or no such input.", false },
                              "i:exec:,i:actor:Target,i:name:In Name,i:float:In Float,o:exec:,o:bool:Return Value" },
    // a plugin's nodes (plugins.h, B14c): their pins come from the plugin (build_pins)
    [BPK_PLUGIN_PURE] = { { "Plugin Function", "Plugins", "A node a plugin adds (Edit > Plugins): a value computed by the plugin's native code.", true }, "" },
    [BPK_PLUGIN_CALL] = { { "Plugin Action", "Plugins", "A node a plugin adds (Edit > Plugins): the plugin's native code runs when the execution reaches it.", false }, "" },
    // World Partition (world_partition.h; notas-dev/mundos-grandes.md)
    [BPK_WP_SET_LAYER] = { { "Set Data Layer Runtime State", "World Partition", "Changes a Data Layer (its name, Window > World Partition) while the game runs: 0 Unloaded (its actors leave the world), 1 Loaded (in, hidden), 2 Activated (in and shown). A dungeon's interior, a town before and after the battle, an event's decorations. False: no such layer (or no World Partition).", false },
                           "i:exec:,i:name:Data Layer,i:int:State,o:exec:,o:bool:Return Value" },
    [BPK_WP_GET_LAYER] = { { "Get Data Layer Runtime State", "World Partition", "A Data Layer's state now: 0 Unloaded, 1 Loaded, 2 Activated (-1: no such layer).", true },
                           "i:name:Data Layer,o:int:State" },
    [BPK_WP_LOCATION_LOADED] = { { "Is Location Loaded", "World Partition", "True when the World Partition cell at Location is in the world (the player near it): spawn or teleport there only then, or its floor may still be missing. Always true without a World Partition.", true },
                                 "i:vector:Location,o:bool:Return Value" },
    [BPK_HUD_MESSAGE] = { { "Show HUD Message", "User Interface|HUD", "A message on the HUD's Message Feed (\"+1 Coin\", \"Quest Complete\") for Duration seconds (0: the feed's), in Color. The same text showing stacks (\"+1 Coin\" twice: \"+2 Coin\"); more than the feed shows wait their turn. False: the queue (32) is full.", false },
                          "i:exec:,i:text:Text,i:float:Duration,i:color:Color,o:exec:,o:bool:Return Value" },
    [BPK_HUD_ADD_COMPASS] = { { "Add Compass Marker", "User Interface|HUD", "Puts the Target actor on the HUD's Compass (and the Minimap): a diamond in Color where it is, its Label, its distance. The same actor's compass marker is replaced; it goes when the actor is destroyed.", false },
                              "i:exec:,i:actor:Target,i:text:Label,i:color:Color,i:bool:Show Distance,o:exec:,o:int:Marker Id" },
    [BPK_HUD_REMOVE_COMPASS] = { { "Remove Compass Marker", "User Interface|HUD", "Takes the Target actor off the HUD's Compass and Minimap.", false }, "i:exec:,i:actor:Target,o:exec:,o:bool:Return Value" },
    [BPK_HUD_ADD_SCREEN] = { { "Add Screen Marker", "User Interface|HUD", "A marker over the Target actor on the screen (Height Offset cm over its origin): a diamond, its Label, its distance; with Show Off Screen, out of the view it waits at the screen's edge with an arrow pointing to it. The same actor's is replaced.", false },
                             "i:exec:,i:actor:Target,i:text:Label,i:color:Color,i:float:Height Offset,i:bool:Show Off Screen,i:bool:Show Distance,o:exec:,o:int:Marker Id" },
    [BPK_HUD_REMOVE_SCREEN] = { { "Remove Screen Marker", "User Interface|HUD", "Takes the Target actor's screen marker away.", false }, "i:exec:,i:actor:Target,o:exec:,o:bool:Return Value" },
    [BPK_HUD_DAMAGE_NUMBER] = { { "Show Damage Number", "User Interface|HUD", "A floating number at Location (rising, fading): Amount; Critical bigger in its color with \"!\"; Heal in green with \"+\". The same numbers the Health shows (the HUD's Damage Numbers widget draws them).", false },
                                "i:exec:,i:vector:Location,i:float:Amount,i:bool:Critical,i:bool:Heal,o:exec:" },
    [BPK_HUD_SET_VISIBLE] = { { "Set HUD Visible", "User Interface|HUD", "Shows or hides the whole HUD (the HUD Layout's widget and every HUD piece): a cutscene of your own, a menu.", false }, "i:exec:,i:bool:Visible,o:exec:" },
    [BPK_HUD_BIND_BAR] = { { "Bind Attribute Bar", "User Interface|HUD", "The Attribute Bar named Bar Name (in Widget; none: the HUD on the screen) shows Actor's Attribute (none: the player): Health, Shield, or a float variable of its Blueprint (its max: Max<Name>). An enemy's bar, a boss's. False: no such bar.", false },
                         "i:exec:,i:actor:Widget,i:name:Bar Name,i:actor:Actor,i:name:Attribute,o:exec:,o:bool:Return Value" },
    [BPK_HUD_SET_SPREAD] = { { "Set Crosshair Spread", "User Interface|HUD", "How open the HUD's Crosshair is, 0 (closed) to 1 (Max Spread): it eases there. Firing, aiming down sights; the player's speed opens it too (HUD Layout).", false }, "i:exec:,i:float:Spread,o:exec:" },
    [BPK_HUD_PHOTO_MODE] = { { "Set Photo Mode", "User Interface|HUD", "The photo mode on or off: the HUD hides while it is on (HUD Layout > Hide in Photo Mode).", false }, "i:exec:,i:bool:On,o:exec:" },
    [BPK_HUD_IS_VISIBLE] = { { "Is HUD Visible", "User Interface|HUD", "Whether the HUD is on the screen now: not hidden by Set HUD Visible, the photo mode or a Sequence's camera.", true }, "o:bool:Return Value" },
    [BPK_GET_ALL_WITH_TAG] = { { "Get All Actors with Tag", "Utilities", "Every actor that has exactly this tag in its Gameplay Tags: \"Enemy\" finds the actors tagged Enemy, not Enemy.Boss. Slow with many actors: call it once (BeginPlay) and keep the array.", false },
                               "i:exec:,i:name:Tag,o:exec:,o:actor[]:Out Actors" },
    [BPK_GET_ALL_WITH_GTAG] = { { "Get All Actors with Gameplay Tag", "Gameplay Tags", "Every actor whose Gameplay Tags match: the tag or one under it (Has Matching: \"Enemy\" finds Enemy and Enemy.Boss); Exact Match: only the same name.", false },
                                "i:exec:,i:name:Gameplay Tag,i:bool:Exact Match,o:exec:,o:actor[]:Out Actors" },
};

uint32_t bp_node_kind_count(void) { return BPN_KIND_COUNT; }
const BpNodeDesc* bp_node_desc(uint32_t kind) { return kind < BPN_KIND_COUNT ? &g_nodes[kind].d : NULL; }

bool bp_kind_gameplay(uint32_t k) {
    if (k >= BPN_KIND_COUNT || !g_nodes[k].d.category) return false;
    if (k == BPK_APPLY_DAMAGE || k == BPK_APPLY_RADIAL_DAMAGE || k == BPK_EVENT_ANY_DAMAGE) return false; // (Unreal's own)
    if (k >= BPK_GET_PROGRESS_SAVE && k <= BPK_EVENT_CHECKPOINT) return true; // (Save Progress; Save Game to Slot is Unreal's)
    static const char* const cats[] = { "Inventory", "Abilities", "Quest", "Interaction", "Dialogue", "Game|Damage",
                                        "Game|Game Rules", "Game|Pause Menu", "User Interface|HUD" };
    const char* c = g_nodes[k].d.category;
    for (size_t i = 0; i < sizeof cats / sizeof cats[0]; i++) {
        size_t l = strlen(cats[i]);
        if (!strncmp(c, cats[i], l) && (c[l] == 0 || c[l] == '|')) return true;
    }
    return false;
}
uint32_t bp_kind_by_name(const char* name) {
    for (uint32_t k = 0; k < BPN_KIND_COUNT; k++)
        if (g_nodes[k].d.name && !strcmp(g_nodes[k].d.name, name)) return k;
    return BPN_KIND_COUNT;
}

// --- Assets ---

#define MAX_ASSETS 256
static struct {
    char root[512];
    BpAsset* assets[MAX_ASSETS];
    uint32_t n;
} A;
static bool s_scanning, s_scanned;
static uint32_t s_renamed[64][2], s_nrenamed; // old hash -> new hash
static int s_scan_pass;
static void scan_all(void);

void bp_init(const char* content_root) {
    snprintf(A.root, sizeof A.root, "%s", content_root ? content_root : "Content");
}

static void free_graph(BpGraph* g) {
    free(g->nodes);
    free(g->links);
    memset(g, 0, sizeof *g);
}
static void free_asset(BpAsset* a) {
    bp_jit_free(a);
    for (uint32_t i = 0; i < a->ngraphs; i++) free_graph(&a->graphs[i]);
    free(a->code);
    free(a->consts);
    free(a->init);
    free(a);
}
void bp_shutdown(void) {
    for (uint32_t i = 0; i < A.n; i++) free_asset(A.assets[i]);
    memset(&A, 0, sizeof A);
    s_scanned = false;
}

uint32_t bp_vpath_hash(const char* vpath) {
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) h = (h ^ (uint8_t)tolower((unsigned char)*c)) * 16777619u;
    return h ? h : 1u;
}

BpAssetKind bp_kind_of_path(const char* path, bool* is_bp) {
    const char* dot = strrchr(path, '.');
    *is_bp = false;
    if (!dot) return BPA_CLASS;
    for (int k = 0; k < BPA_COUNT; k++)
        if (!_stricmp(dot + 1, g_bp_asset_ext[k])) {
            *is_bp = true;
            return (BpAssetKind)k;
        }
    return BPA_CLASS;
}

static char* read_text(const char* path, size_t* len) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = n > 0 ? malloc((size_t)n + 1) : NULL; // a load, not a frame
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) buf[n] = 0, *len = (size_t)n;
    return buf;
}

// "/Content/A/B.rbp" -> "<root>/A/B.rbp"
static void disk_path(const char* vpath, char* out, size_t cap) {
    const char* rest = !strncmp(vpath, "/Content", 8) ? vpath + 8 : vpath;
    snprintf(out, cap, "%s%s", A.root, rest);
}

static BpGraph* add_graph(BpAsset* a, const char* name) {
    if (a->ngraphs >= 16) return NULL;
    BpGraph* g = &a->graphs[a->ngraphs++];
    memset(g, 0, sizeof *g);
    snprintf(g->name, sizeof g->name, "%s", name);
    g->zoom = 1.0f;
    g->next_id = 1;
    return g;
}

uint32_t bp_type_width(uint8_t type, const char* ref) {
    if (type == BPT_EXEC) return 0;
    if (type == BPT_TRANSFORM) return 3;
    if (type != BPT_STRUCT) return 1;
    const BpAsset* s = bp_find_name(BPA_STRUCT, ref);
    uint32_t w = 0;
    for (uint32_t i = 0; s && i < s->nfields; i++) w += bp_var_width(&s->fields[i]);
    return w ? w : 1;
}
uint32_t bp_var_width(const BpVar* v) { return v->type == BPT_EXEC ? 0 : v->cont ? 1 : bp_type_width(v->type, v->ref); }
uint32_t bp_pin_width(const BpPin* p) { return p->type == BPT_EXEC ? 0 : p->cont ? 1 : bp_type_width(p->type, p->ref); }

const BpGraph* bp_dispatcher(const BpAsset* a, const char* name, const char* cls) {
    const BpAsset* c = cls && cls[0] ? bp_find_name(BPA_CLASS, cls) : a;
    for (int depth = 0; c && depth < 16; depth++, c = bp_find_name(BPA_CLASS, c->parent))
        for (uint32_t g = 0; g < c->ngraphs; g++)
            if (c->graphs[g].gkind == BPG_DISPATCHER && !strcmp(c->graphs[g].name, name)) return &c->graphs[g];
    return NULL;
}

const BpGraph* bp_event_signature(const BpAsset* a, const char* name, const char* cls) {
    const BpAsset* c = cls && cls[0] && (!a || strcmp(cls, a->name)) ? bp_find_name(BPA_CLASS, cls) : a; // (its own name: itself, as edited)
    for (int depth = 0; c && depth < 16; depth++, c = bp_find_name(BPA_CLASS, c->parent))
        for (uint32_t g = 0; g < c->ngraphs; g++)
            if (c->graphs[g].gkind == BPG_EVENT && !strcmp(c->graphs[g].name, name)) return &c->graphs[g];
    return NULL;
}

BpGraph* bp_add_graph(BpAsset* a, const char* name, int gkind) {
    BpGraph* g = add_graph(a, name);
    if (!g) return NULL;
    g->gkind = (uint8_t)gkind;
    if (gkind == BPG_DISPATCHER || gkind == BPG_EVENT) { // only a signature (its inputs): no nodes
        a->dirty = true;
        return g;
    }
    if (gkind == BPG_MACRO) { // Unreal's: an exec in and out to start with
        g->nparams = g->nresults = 1;
        snprintf(g->params[0].name, BP_NAME, "In");
        snprintf(g->results[0].name, BP_NAME, "Out");
        g->params[0].type = g->results[0].type = BPT_EXEC;
        bp_add_node(a, g, BPK_TUNNEL_IN, 0, 0, name, NULL);
        bp_add_node(a, g, BPK_TUNNEL_OUT, 400, 0, name, NULL);
    } else {
        bp_add_node(a, g, BPK_FUNC_ENTRY, 0, 0, name, NULL);
        bp_add_node(a, g, BPK_FUNC_RETURN, 400, 0, name, NULL);
    }
    a->dirty = true;
    return g;
}

// A spec's type: "float", "vector[]" (an array), "int{}" (a set), "wild<>" (a map), "wild2" (a map's value), "string*2"
// (repeated: 2 + the pins added)
// (2026-11-19) the engine's own enumerations on int pins ("int(EName)": the pin is an int, the graph shows a drop-down)
static const struct { const char* name; const char* entries[8]; int n; } g_builtin_enums[] = {
    { "ETraceChannel", { "Visibility", "Camera", "World Static", "World Dynamic", "Pawn", "Landscape" }, 6 },
    { "EDrawDebugTrace", { "None", "For One Frame", "For Duration", "Persistent" }, 4 },
};
const char* const* bp_builtin_enum(const char* name, int* count) {
    for (size_t i = 0; name && name[0] && i < sizeof g_builtin_enums / sizeof g_builtin_enums[0]; i++)
        if (!strcmp(name, g_builtin_enums[i].name)) return *count = g_builtin_enums[i].n, g_builtin_enums[i].entries;
    return *count = 0, NULL;
}
static char g_spec_ref[BP_NAME]; // (parse_type's "(EName)")
static bool parse_type(const char* s, uint8_t* t, uint8_t* cont, int* repeat, uint8_t* wslot) {
    static const char* const n[] = { "exec", "bool", "int", "float", "vector", "rotator", "string", "actor", "struct", "enum",
                                     "byte", "int64", "name", "text", "color", "transform", "class", "object", "wild" };
    char base[32];
    size_t k = 0;
    g_spec_ref[0] = 0;
    while (s[k] && s[k] != '[' && s[k] != '{' && s[k] != '<' && s[k] != '*' && s[k] != '(' && k < sizeof base - 1) base[k] = s[k], k++;
    base[k] = 0;
    if (s[k] == '(') {
        size_t j = 0;
        for (k++; s[k] && s[k] != ')' && j < sizeof g_spec_ref - 1; k++) g_spec_ref[j++] = s[k];
        g_spec_ref[j] = 0;
        if (s[k] == ')') k++;
    }
    const char* rest = s + k;
    *cont = BPC_SINGLE, *repeat = 0, *wslot = 0;
    if (rest[0] == '[') *cont = BPC_ARRAY, rest += 2;
    else if (rest[0] == '{') *cont = BPC_SET, rest += 2;
    else if (rest[0] == '<') *cont = BPC_MAP, rest += 2;
    if (rest[0] == '*') *repeat = atoi(rest + 1) > 0 ? atoi(rest + 1) : 1;
    if (!strcmp(base, "wild2")) {
        *t = BPT_WILDCARD, *wslot = 2;
        return true;
    }
    for (int i = 0; i < (int)(sizeof n / sizeof n[0]); i++)
        if (!strcmp(base, n[i])) {
            *t = (uint8_t)i;
            if (i == BPT_WILDCARD) *wslot = 1;
            return true;
        }
    return false;
}

// The parts a split pin becomes (Unreal's Split Struct Pin): their names, types, refs; how many
static int split_parts(uint8_t type, const char* ref, const char** names, uint8_t* types, const char** refs) {
    static const char* const xyz[] = { "X", "Y", "Z" }, *pyr[] = { "Pitch", "Yaw", "Roll" }, *rgba[] = { "R", "G", "B", "A" },
                            *lrs[] = { "Location", "Rotation", "Scale" };
    switch (type) {
    case BPT_VECTOR: for (int i = 0; i < 3; i++) names[i] = xyz[i], types[i] = BPT_FLOAT, refs[i] = ""; return 3;
    case BPT_ROTATOR: for (int i = 0; i < 3; i++) names[i] = pyr[i], types[i] = BPT_FLOAT, refs[i] = ""; return 3;
    case BPT_COLOR: for (int i = 0; i < 4; i++) names[i] = rgba[i], types[i] = BPT_FLOAT, refs[i] = ""; return 4;
    case BPT_TRANSFORM:
        for (int i = 0; i < 3; i++) names[i] = lrs[i], types[i] = i == 1 ? BPT_ROTATOR : BPT_VECTOR, refs[i] = "";
        return 3;
    case BPT_STRUCT: {
        const BpAsset* s = bp_find_name(BPA_STRUCT, ref);
        int k = 0;
        for (uint32_t f = 0; s && f < s->nfields && k < 8; f++)
            if (!s->fields[f].cont) names[k] = s->fields[f].name, types[k] = s->fields[f].type, refs[k] = s->fields[f].ref, k++;
        return k;
    }
    default: return 0;
    }
}

static BpPin* push_pin(BpNode* n, int out, uint8_t type, const char* name, const char* ref) {
    if (n->npins >= BP_MAX_PINS) return NULL;
    BpPin* p = &n->pins[n->npins++];
    memset(p, 0, sizeof *p);
    p->out = (uint8_t)out;
    p->type = type;
    snprintf(p->name, sizeof p->name, "%s", name ? name : "");
    if (ref) snprintf(p->ref, sizeof p->ref, "%s", ref);
    return p;
}

// A whole pin, split into its parts when the node says so (n->split, by the pins' order: *base counts them)
static BpPin* add_pin(BpNode* n, int* base, int out, uint8_t type, uint8_t cont, const char* name, const char* ref) {
    int bi = (*base)++;
    if (bi < 16 && (n->split >> bi & 1u) && cont == BPC_SINGLE && bp_type_splittable(type)) {
        const char* names[8];
        const char* refs[8];
        uint8_t types[8];
        int k = split_parts(type, ref, names, types, refs);
        BpPin* first = NULL;
        for (int i = 0; i < k; i++) {
            char nm[BP_NAME * 2];
            if (name && name[0]) snprintf(nm, sizeof nm, "%s %s", name, names[i]);
            else snprintf(nm, sizeof nm, "%s", names[i]);
            BpPin* p = push_pin(n, out, types[i], nm, refs[i]);
            if (!p) break;
            p->sub = (uint8_t)(i + 1);
            p->ptype = type;
            snprintf(p->pname, sizeof p->pname, "%s", name ? name : "");
            snprintf(p->pref, sizeof p->pref, "%s", ref ? ref : "");
            if (!first) first = p;
        }
        if (first) return first;
    }
    BpPin* p = push_pin(n, out, type, name, ref);
    if (p) p->cont = cont;
    return p;
}

static BpPin* add_var_pin(BpNode* n, int* base, int out, const BpVar* v, const char* name) {
    BpPin* p = add_pin(n, base, out, v->type, v->cont, name ? name : v->name, v->ref);
    if (p && !p->sub) p->vtype = v->vtype;
    return p;
}

uint32_t bp_name_hash(const char* name) {
    uint32_t h = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)name; p && *p; p++) h = (h ^ *p) * 16777619u;
    return h ? h : 1u;
}

bool bp_var_overridable(const BpVar* v) {
    if (!v || v->cont) return false;
    switch (v->type) {
    case BPT_BOOL: case BPT_INT: case BPT_FLOAT: case BPT_VECTOR: case BPT_ROTATOR: case BPT_STRING: case BPT_ENUM:
    case BPT_BYTE: case BPT_INT64: case BPT_NAME: case BPT_TEXT: case BPT_COLOR:
        return true;
    default:
        return false;
    }
}

CBpOverride* bp_override_find(CBlueprint* b, const char* var) {
    uint32_t h = bp_name_hash(var);
    for (uint32_t i = 0; b && i < b->noverrides && i < BP_MAX_OVERRIDES; i++)
        if (b->ov[i].var == h) return &b->ov[i];
    return NULL;
}

bool bp_override_set(CBlueprint* b, const char* var, const float v[4], const char* text) {
    if (!b) return false;
    CBpOverride* o = bp_override_find(b, var);
    if (!o) {
        if (b->noverrides >= BP_MAX_OVERRIDES) return false;
        o = &b->ov[b->noverrides++];
        memset(o, 0, sizeof *o);
        o->var = bp_name_hash(var);
    }
    if (v) memcpy(o->v, v, sizeof o->v);
    if (text) snprintf(o->text, sizeof o->text, "%s", text);
    return true;
}

void bp_override_clear(CBlueprint* b, const char* var) {
    CBpOverride* o = bp_override_find(b, var);
    if (!o) return;
    uint32_t i = (uint32_t)(o - b->ov);
    memmove(&b->ov[i], &b->ov[i + 1], sizeof b->ov[0] * (b->noverrides - i - 1));
    b->noverrides--;
    memset(&b->ov[b->noverrides], 0, sizeof b->ov[0]);
}

int bp_var_index(const BpAsset* a, const char* name) {
    for (uint32_t i = 0; i < a->nvars; i++)
        if (!strcmp(a->vars[i].name, name)) return (int)i;
    return -1;
}

static const BpVar* find_param(const BpAsset* a, const char* name) {
    int v = bp_var_index(a, name);
    return v >= 0 ? &a->vars[v] : NULL;
}

// The graph (in an asset) a function / macro name is: its index, or -1
static int graph_named(const BpAsset* a, const char* name, int gkind) {
    for (uint32_t gi = a && a->kind == BPA_CLASS ? 1 : 0; a && gi < a->ngraphs; gi++) // (a class's graph 0: its Event Graph)
        if (a->graphs[gi].gkind == gkind && !strcmp(a->graphs[gi].name, name)) return (int)gi;
    return -1;
}

// A macro's graph: this class's (or a parent's), else a Macro Library's ("Library.Macro" in text2, or any library's)
const BpGraph* bp_find_macro(const BpAsset* a, const char* name, const char* lib) {
    for (int depth = 0; a && depth < 8; depth++) {
        int gi = graph_named(a, name, BPG_MACRO);
        if (gi >= 0) return &a->graphs[gi];
        a = bp_find_name(BPA_CLASS, a->parent);
    }
    for (uint32_t i = 0; i < bp_asset_count(); i++) {
        const BpAsset* l = bp_asset_at(i);
        if (l->kind != BPA_MACROLIB || (lib && lib[0] && strcmp(l->name, lib))) continue;
        int gi = graph_named(l, name, BPG_MACRO);
        if (gi >= 0) return &l->graphs[gi];
    }
    return NULL;
}

// An interface function's signature (the interface asset's graph of that name)
const BpGraph* bp_interface_function(const char* iface, const char* func) {
    const BpAsset* ia = iface && iface[0] ? bp_find_name(BPA_INTERFACE, iface) : NULL;
    int gi = graph_named(ia, func, BPG_FUNCTION);
    return gi >= 0 ? &ia->graphs[gi] : NULL;
}
// The interface (of those the class implements) that has the function
const char* bp_interface_of(const BpAsset* cls, const char* func) {
    for (int depth = 0; cls && depth < 8; depth++) {
        for (uint32_t i = 0; i < cls->ninterfaces; i++)
            if (bp_interface_function(cls->interfaces[i], func)) return cls->interfaces[i];
        cls = bp_find_name(BPA_CLASS, cls->parent);
    }
    return NULL;
}

// Builds a node's pins from its spec (and its text: the variable, struct, enum, function, data asset it names), its
// wildcards resolved, its split pins in parts, its added pins
static void build_pins(BpAsset* a, BpNode* n) {
    BpPin keep[BP_MAX_PINS];
    uint8_t nkeep = n->npins;
    memcpy(keep, n->pins, sizeof keep);
    n->npins = 0;
    int base = 0;
    const char* spec = g_nodes[n->kind].pins;
    char pspec[400];
    if (n->kind == BPK_PLUGIN_PURE || n->kind == BPK_PLUGIN_CALL) { // (B14c) the plugin's pins; not loaded: the node keeps its own
        int fi = plugin_fn_find(n->text);
        if (fi < 0 && nkeep) {
            n->npins = nkeep;
            return;
        }
        plugin_fn_spec(fi, n->kind == BPK_PLUGIN_CALL, pspec, sizeof pspec);
        spec = pspec;
    }
    char buf[400];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        int out = tok[0] == 'o';
        char* t = tok + 2;
        char* colon = strchr(t, ':');
        if (!colon) continue;
        *colon = 0;
        const char* pname = colon + 1;
        uint8_t type, cont, wslot;
        int repeat;
        if (!strcmp(t, "var")) { // the variable's type
            const BpVar* v = find_param(a, n->text);
            if (v) add_var_pin(n, &base, out, v, NULL);
            else add_pin(n, &base, out, BPT_FLOAT, 0, n->text, NULL);
        } else if (!strcmp(t, "field") || !strcmp(t, "struct")) {
            const BpAsset* s = bp_find_name(BPA_STRUCT, n->text);
            if (!strcmp(t, "struct")) add_pin(n, &base, out, BPT_STRUCT, 0, n->text, n->text);
            else if (s)
                for (uint32_t f = 0; f < s->nfields; f++) add_var_pin(n, &base, out, &s->fields[f], NULL);
        } else if (!strcmp(t, "entry")) {
            const BpAsset* e = bp_find_name(BPA_ENUM, n->text);
            if (e)
                for (uint32_t k = 0; k < e->nentries; k++) add_pin(n, &base, 1, BPT_EXEC, 0, e->entries[k], NULL);
        } else if (!strcmp(t, "data")) {
            const BpAsset* d = bp_find_name(BPA_DATA, n->text);
            int v = d ? bp_var_index(d, n->text2) : -1;
            if (v >= 0) add_var_pin(n, &base, out, &d->vars[v], n->text2);
            else add_pin(n, &base, out, BPT_FLOAT, 0, n->text2, NULL);
        } else if (!strcmp(t, "ivalue")) { // an Input Action's value: its type's (bool, float, a vector for 2D / 3D)
            const InputAction* ia = input_action_named(n->text);
            uint8_t vt = !ia ? BPT_FLOAT : ia->value_type == IAV_BOOL ? BPT_BOOL : ia->value_type == IAV_AXIS1D ? BPT_FLOAT : BPT_VECTOR;
            // (the action not loaded yet - the Blueprints read before the Input assets: the pin keeps its type; made a
            // float, its Vector wire to a Break Vector was dropped and the event read 0)
            for (uint8_t k = 0; !ia && k < nkeep; k++)
                if (keep[k].out && !strcmp(keep[k].name, pname)) vt = keep[k].type;
            add_pin(n, &base, out, vt, 0, pname, NULL);
        } else if (parse_type(t, &type, &cont, &repeat, &wslot)) {
            uint8_t vtype = 0;
            const char* ref = type == BPT_ENUM ? n->text : g_spec_ref[0] ? g_spec_ref : NULL; // (a built-in enum's name on an int)
            if (wslot == 1 && n->wtype) type = n->wtype, ref = n->wref, vtype = n->wtype2 ? n->wtype2 : BPT_WILDCARD;
            else if (wslot == 1 && cont == BPC_MAP) vtype = n->wtype2 ? n->wtype2 : BPT_WILDCARD;
            else if (wslot == 2 && n->wtype2) type = n->wtype2;
            int count = repeat ? repeat + n->extra : 1;
            for (int r = 0; r < count; r++) {
                char nm[BP_NAME];
                if (!repeat) snprintf(nm, sizeof nm, "%s", pname);
                else if (!strcmp(pname, "#")) snprintf(nm, sizeof nm, "%c", 'A' + r);
                else if (!pname[0] && type != BPT_EXEC) snprintf(nm, sizeof nm, "[%d]", r);
                else snprintf(nm, sizeof nm, "%s%d", pname, r);
                BpPin* p = add_pin(n, &base, out, type, cont, nm, ref);
                if (!p) break;
                if (!p->sub) p->vtype = vtype, p->wslot = wslot;
                if (n->kind == BPK_CAST && out && type == BPT_ACTOR) snprintf(p->name, sizeof p->name, "As %s", n->text), snprintf(p->ref, sizeof p->ref, "%s", n->text);
                if ((n->kind == BPK_SPAWN || n->kind == BPK_SPAWN_ASYNC) && out && type == BPT_ACTOR) {
                    // the Return Value is of the class picked in the Class pin (a literal) when there is one
                    for (uint8_t k = 0; k < nkeep; k++)
                        if (keep[k].type == BPT_CLASS && !keep[k].out) snprintf(p->ref, sizeof p->ref, "%s", keep[k].slit);
                }
            }
        }
    }
    if (n->kind == BPK_TIMELINE) { // (2026-11-16) its tracks: an Event's exec, the others' values (bp_timeline.h)
        const BpTimeline* tl = bp_timeline_find(a, n->text);
        static const uint8_t tt[TLT_COUNT] = { BPT_FLOAT, BPT_VECTOR, BPT_COLOR, BPT_EXEC };
        for (uint8_t i = 0; tl && i < tl->ntracks; i++) add_pin(n, &base, 1, tt[tl->tracks[i].type], 0, tl->tracks[i].name, NULL);
    }
    if (n->kind == BPK_EVENT_CUSTOM || n->kind == BPK_CALL_CUSTOM) { // a Custom Event's inputs: the event's outputs, the call's inputs
        if (n->kind == BPK_CALL_CUSTOM && n->text2[0]) add_pin(n, &base, 0, BPT_ACTOR, 0, "Target", n->text2); // (another class's event)
        const BpGraph* eg = bp_event_signature(a, n->text, n->kind == BPK_CALL_CUSTOM ? n->text2 : NULL);
        for (uint8_t i = 0; eg && i < eg->nparams; i++) add_var_pin(n, &base, n->kind == BPK_EVENT_CUSTOM, &eg->params[i], NULL);
    }
    if (n->kind == BPK_DISPATCH_CALL || n->kind == BPK_EVENT_DISPATCH) { // the dispatcher's signature: the call's inputs, the event's outputs
        const BpGraph* dg = bp_dispatcher(a, n->text, n->text2);
        for (uint8_t i = 0; dg && i < dg->nparams; i++) add_var_pin(n, &base, n->kind == BPK_EVENT_DISPATCH, &dg->params[i], NULL);
    }
    // a function's signature (entry: its parameters out; return: its results in; call: both)
    BpGraph* fg = NULL;
    for (uint32_t gi = a->kind == BPA_CLASS ? 1 : 0; gi < a->ngraphs; gi++) // (an interface's, a library's graphs are all functions)
        if (!strcmp(a->graphs[gi].name, n->text)) fg = &a->graphs[gi];
    if (fg && (n->kind == BPK_FUNC_ENTRY || n->kind == BPK_CALL_FUNCTION))
        for (uint8_t i = 0; i < fg->nparams; i++) add_var_pin(n, &base, n->kind == BPK_FUNC_ENTRY, &fg->params[i], NULL);
    if (fg && (n->kind == BPK_FUNC_RETURN || n->kind == BPK_CALL_FUNCTION))
        for (uint8_t i = 0; i < fg->nresults; i++) add_var_pin(n, &base, n->kind == BPK_CALL_FUNCTION, &fg->results[i], NULL);
    // a macro: its graph's inputs and outputs (exec ones too); its tunnels inside
    if (n->kind == BPK_MACRO || n->kind == BPK_TUNNEL_IN || n->kind == BPK_TUNNEL_OUT) {
        const BpGraph* mg = n->kind == BPK_MACRO ? bp_find_macro(a, n->text, n->text2) : NULL;
        if (!mg) // a tunnel: the macro graph it is in (its name in text)
            for (uint32_t gi = 1; gi < a->ngraphs; gi++)
                if (a->graphs[gi].gkind == BPG_MACRO && !strcmp(a->graphs[gi].name, n->text)) mg = &a->graphs[gi];
        if (mg && n->kind != BPK_TUNNEL_OUT)
            for (uint8_t i = 0; i < mg->nparams; i++) add_var_pin(n, &base, n->kind == BPK_TUNNEL_IN, &mg->params[i], NULL);
        if (mg && n->kind != BPK_TUNNEL_IN)
            for (uint8_t i = 0; i < mg->nresults; i++) add_var_pin(n, &base, n->kind == BPK_MACRO, &mg->results[i], NULL);
    }
    // an interface function: Call (Target, its inputs; its outputs), Event (its inputs out)
    if (n->kind == BPK_CALL_INTERFACE || n->kind == BPK_EVENT_INTERFACE) {
        const char* func = n->kind == BPK_CALL_INTERFACE ? n->text2 : n->text;
        const char* iface = n->kind == BPK_CALL_INTERFACE ? n->text : (n->text2[0] ? n->text2 : bp_interface_of(a, n->text));
        const BpGraph* ig = bp_interface_function(iface, func);
        for (uint8_t i = 0; ig && i < ig->nparams; i++) add_var_pin(n, &base, n->kind == BPK_EVENT_INTERFACE, &ig->params[i], NULL);
        for (uint8_t i = 0; ig && n->kind == BPK_CALL_INTERFACE && i < ig->nresults; i++) add_var_pin(n, &base, 1, &ig->results[i], NULL);
    }
    // (2026-11-10) the delegates, last (the saved wires to the pins before them keep their places): a Custom Event's out,
    // the Event in of the nodes that take one
    if (n->kind == BPK_EVENT_CUSTOM) {
        BpPin* d = add_pin(n, &base, 1, BPT_EXEC, 0, "Delegate", NULL);
        if (d) d->delegate = 1;
    } else if (n->kind == BPK_DISPATCH_BIND || n->kind == BPK_DISPATCH_UNBIND || n->kind == BPK_SET_TIMER || n->kind == BPK_CLEAR_TIMER) {
        BpPin* d = add_pin(n, &base, 0, BPT_EXEC, 0, "Event", NULL);
        if (d) d->delegate = 1;
    }
    // (2026-11-10) the asset's own pin, after them all (the saved wires keep their places): a Name, its literal the text
    const char* ap = bp_asset_pin(n->kind);
    if (ap) {
        BpPin* q = add_pin(n, &base, 0, BPT_NAME, 0, ap, NULL);
        if (q) q->asset = 1;
    }
    // (2026-11-10) Expose on Spawn: the chosen class's variables marked so, as inputs after them all (the saved wires
    // keep their places; build_pins runs again when the Class changes)
    if (n->kind == BPK_SPAWN || n->kind == BPK_SPAWN_ASYNC) {
        const char* cname = "";
        for (uint8_t k = 0; k < n->npins; k++)
            if (n->pins[k].type == BPT_CLASS && !n->pins[k].out) cname = n->pins[k].slit;
        for (uint8_t k = 0; !cname[0] && k < nkeep; k++)
            if (keep[k].type == BPT_CLASS && !keep[k].out) cname = keep[k].slit;
        const BpAsset* cls = cname[0] ? bp_find_name(BPA_CLASS, cname) : NULL;
        if (cname[0] && !cls) // (the class not loaded yet - the Blueprints read in any order: its pins stay as they were, values too)
            for (uint8_t k = 0; k < nkeep && n->npins < BP_MAX_PINS; k++)
                if (keep[k].spawnarg) n->pins[n->npins++] = keep[k];
        for (uint32_t vi = 0; cls && vi < cls->nvars; vi++) {
            const BpVar* v = &cls->vars[vi];
            if (!v->expose_spawn || !bp_var_overridable(v)) continue;
            BpPin* q = add_var_pin(n, &base, 0, v, NULL);
            if (q) q->spawnarg = 1;
        }
    }
    // the literals the user typed stay (by name)
    for (uint8_t i = 0; i < n->npins; i++)
        for (uint8_t k = 0; k < nkeep; k++)
            if (keep[k].out == n->pins[i].out && !strcmp(keep[k].name, n->pins[i].name) && (keep[k].type == n->pins[i].type || (keep[k].spawnarg && n->pins[i].spawnarg))) { // (an exposed pin: by name, its placeholder typed later)
                n->pins[i].lit = keep[k].lit;
                memcpy(n->pins[i].slit, keep[k].slit, sizeof n->pins[i].slit);
            }
    for (uint8_t i = 0; i < n->npins; i++) // (the asset pin's literal: the node's text, chosen in Details or typed on it)
        if (n->pins[i].asset) snprintf(n->pins[i].slit, sizeof n->pins[i].slit, "%s", n->text);
}

const char* bp_asset_pin(uint32_t kind) {
    switch (kind) {
    case BPK_PLAY_SEQUENCE: case BPK_STOP_SEQUENCE: case BPK_IS_SEQUENCE_PLAYING: return "Sequence";
    case BPK_PLAY_MONTAGE: return "Montage";
    case BPK_PLAY_ANIMATION: return "Animation";
    case BPK_RUN_BT: return "Behavior Tree";
    case BPK_BB_GET_FLOAT: case BPK_BB_GET_BOOL: case BPK_BB_GET_VECTOR: case BPK_BB_GET_ACTOR:
    case BPK_BB_SET_FLOAT: case BPK_BB_SET_BOOL: case BPK_BB_SET_VECTOR: case BPK_BB_SET_ACTOR: return "Key Name";
    case BPK_PLAY_SOUND_2D: case BPK_PLAY_SOUND_AT: case BPK_SPAWN_SOUND_2D: case BPK_SPAWN_SOUND_AT: case BPK_SPAWN_SOUND_ATTACHED:
    case BPK_AUDIO_SET_SOUND: return "Sound";
    case BPK_PUSH_SOUND_MIX: case BPK_POP_SOUND_MIX: case BPK_SET_BASE_SOUND_MIX: return "Sound Mix";
    case BPK_SET_CLASS_VOLUME: return "Sound Class";
    case BPK_PS_SPAWN: return "System";
    case BPK_SPAWN_DECAL: return "Decal Material";
    case BPK_WIDGET_SET_VALUE: case BPK_WIDGET_SET_ENABLED: case BPK_WIDGET_SET_TEXT: case BPK_WIDGET_SET_PERCENT:
    case BPK_WIDGET_SET_VISIBLE: case BPK_WIDGET_SET_COLOR: case BPK_WIDGET_GET_VALUE: case BPK_WIDGET_GET_TEXT: return "Widget Name";
    case BPK_PLAY_WIDGET_ANIM: case BPK_STOP_WIDGET_ANIM: return "Animation Name";
    case BPK_MAT_SCALAR: case BPK_MAT_VECTOR: return "Parameter Name";
    case BPK_ADD_MAPPING_CONTEXT: case BPK_REMOVE_MAPPING_CONTEXT: return "Mapping Context";
    default: return NULL;
    }
}

void bp_refresh_pins(BpAsset* a, BpGraph* g, BpNode* n) {
    // the node's links by their pins' names (the pins may move: a split, an added pin, a changed signature)
    enum { MAXL = 64 };
    struct { uint32_t li; char name[BP_NAME]; uint8_t side; } moved[MAXL];
    uint32_t nmoved = 0;
    for (uint32_t i = 0; i < g->nlinks && nmoved < MAXL; i++) {
        const BpLink* l = &g->links[i];
        if (l->from_node == n->id && l->from_pin < n->npins) {
            moved[nmoved].li = i, moved[nmoved].side = 0;
            snprintf(moved[nmoved++].name, BP_NAME, "%s", n->pins[l->from_pin].name);
        }
        if (l->to_node == n->id && l->to_pin < n->npins && nmoved < MAXL) {
            moved[nmoved].li = i, moved[nmoved].side = 1;
            snprintf(moved[nmoved++].name, BP_NAME, "%s", n->pins[l->to_pin].name);
        }
    }
    build_pins(a, n);
    for (uint32_t m = 0; m < nmoved; m++) {
        BpLink* l = &g->links[moved[m].li];
        uint8_t* pin = moved[m].side ? &l->to_pin : &l->from_pin;
        uint8_t out = moved[m].side ? 0 : 1;
        int found = -1;
        for (uint8_t p = 0; p < n->npins && found < 0; p++)
            if (n->pins[p].out == out && !strcmp(n->pins[p].name, moved[m].name)) found = p;
        *pin = found >= 0 ? (uint8_t)found : 0xFF; // gone: dropped below
    }
    // links to pins that no longer exist (or changed type) go
    for (uint32_t i = 0; i < g->nlinks;) {
        BpLink* l = &g->links[i];
        BpNode* f = bp_node(g, l->from_node);
        BpNode* t = bp_node(g, l->to_node);
        bool ok = f && t && l->from_pin < f->npins && l->to_pin < t->npins && bp_types_compatible(&f->pins[l->from_pin], &t->pins[l->to_pin]);
        if (!ok) g->links[i] = g->links[--g->nlinks];
        else i++;
    }
}

// Unreal's Split Struct Pin / Recombine: the pin (its index now) in parts, or the parts back into one pin
bool bp_split_pin(BpAsset* a, BpGraph* g, BpNode* n, uint8_t pin, bool split) {
    if (pin >= n->npins) return false;
    // the pin's index among the whole pins (n->split's bit)
    int bi = -1;
    for (uint8_t p = 0; p <= pin; p++)
        if (!n->pins[p].sub || n->pins[p].sub == 1) bi++;
    if (bi < 0 || bi >= 16) return false;
    BpPin whole = n->pins[pin];
    if (split) {
        if (whole.sub || whole.cont || !bp_type_splittable(whole.type)) return false;
        bp_unlink_pin(g, n->id, pin);
        n->split |= (uint16_t)(1u << bi);
        bp_refresh_pins(a, g, n);
        // the parts start with the whole's literal
        for (uint8_t p = pin, k = 0; p < n->npins && n->pins[p].sub; p++, k++) {
            BpPin* q = &n->pins[p];
            if (whole.type == BPT_VECTOR || whole.type == BPT_ROTATOR || whole.type == BPT_COLOR) q->lit.v[0] = whole.lit.v[k];
            else if (whole.type == BPT_TRANSFORM && k == 2) q->lit.v[0] = q->lit.v[1] = q->lit.v[2] = 1.0f;
        }
    } else {
        if (!whole.sub) return false;
        uint8_t first = pin;
        while (first > 0 && n->pins[first].sub > 1) first--;
        BpValue lit = { { 0 } };
        for (uint8_t p = first; p < n->npins && n->pins[p].sub && (p == first || n->pins[p].sub > 1); p++) {
            if (n->pins[p].sub <= 4) lit.v[n->pins[p].sub - 1] = n->pins[p].lit.v[0];
            bp_unlink_pin(g, n->id, p);
        }
        n->split &= (uint16_t)~(1u << bi);
        bp_refresh_pins(a, g, n);
        if (first < n->npins && (whole.ptype == BPT_VECTOR || whole.ptype == BPT_ROTATOR || whole.ptype == BPT_COLOR)) n->pins[first].lit = lit;
    }
    a->dirty = true;
    a->compiled = false;
    return true;
}

BpNode* bp_node(BpGraph* g, uint32_t id) {
    for (uint32_t i = 0; i < g->nnodes; i++)
        if (g->nodes[i].id == id) return &g->nodes[i];
    return NULL;
}

void bp_add_custom_event(BpAsset* a, const char* name) {
    if (!a->ngraphs) return;
    BpGraph* g = &a->graphs[0];
    float x = -400.0f, y = 0.0f;
    for (uint32_t i = 0; i < g->nnodes; i++) {
        if (g->nodes[i].kind == BPK_EVENT_CUSTOM && !strcmp(g->nodes[i].text, name)) return;
        if (g->nodes[i].y + 200.0f > y) y = g->nodes[i].y + 200.0f;
    }
    bp_add_node(a, g, BPK_EVENT_CUSTOM, x, y, name, NULL);
}

BpNode* bp_add_node(BpAsset* a, BpGraph* g, uint32_t kind, float x, float y, const char* text, const char* text2) {
    if (kind >= BPN_KIND_COUNT) return NULL;
    if (g->nnodes == g->cap_nodes) {
        uint32_t c = g->cap_nodes ? g->cap_nodes * 2 : 32;
        BpNode* nn = realloc(g->nodes, sizeof(BpNode) * c); // an edit, not a frame
        if (!nn) return NULL;
        g->nodes = nn;
        g->cap_nodes = c;
    }
    BpNode* n = &g->nodes[g->nnodes++];
    memset(n, 0, sizeof *n);
    n->id = g->next_id++;
    n->kind = (uint16_t)kind;
    n->x = x;
    n->y = y;
    if (text) snprintf(n->text, sizeof n->text, "%s", text);
    if (text2) snprintf(n->text2, sizeof n->text2, "%s", text2);
    build_pins(a, n);
    // sensible literals (Unreal's)
    for (uint8_t i = 0; i < n->npins; i++) {
        BpPin* p = &n->pins[i];
        if (kind == BPK_PRINT && p->type == BPT_STRING) snprintf(p->slit, sizeof p->slit, "Hello");
        if ((kind >= BPK_LINE_TRACE && kind <= BPK_SET_TRACE_COLORS) || kind == BPK_ASYNC_LINE_TRACE_BATCH || kind == BPK_ASYNC_SPHERE_TRACE_BATCH) { // (2026-11-19: Unreal's: 5 s, red and green, 1 px)
            if (!strcmp(p->name, "Draw Time")) p->lit.v[0] = 5.0f;
            if (!strcmp(p->name, "Radius")) p->lit.v[0] = kind == BPK_DRAW_DEBUG_SPHERE ? 50.0f : kind == BPK_DRAW_DEBUG_CAPSULE ? 34.0f : 20.0f;
            if (!strcmp(p->name, "Half Height")) p->lit.v[0] = 88.0f;
            if (!strcmp(p->name, "Half Size") || !strcmp(p->name, "Extent")) p->lit.v[0] = p->lit.v[1] = p->lit.v[2] = !strcmp(p->name, "Extent") ? 50.0f : 20.0f;
            if (!strcmp(p->name, "Thickness")) p->lit.v[0] = 1.0f;
            if (!strcmp(p->name, "Size")) p->lit.v[0] = 10.0f;
            if (p->type == BPT_COLOR) {
                bool green = !strcmp(p->name, "Trace Hit Color");
                p->lit.v[0] = green ? 0.0f : 1.0f, p->lit.v[1] = green ? 1.0f : 0.0f, p->lit.v[2] = 0.0f, p->lit.v[3] = 1.0f;
            }
        }
        if (kind == BPK_DELAY && p->type == BPT_FLOAT) p->lit.v[0] = 0.2f;
        if (kind == BPK_MOVE_INPUT && p->type == BPT_FLOAT) p->lit.v[0] = 1.0f;
        if (kind == BPK_RANDOM && !strcmp(p->name, "Max")) p->lit.v[0] = 1.0f;
        if (kind == BPK_FOR_LOOP && !strcmp(p->name, "Last Index")) p->lit.v[0] = 9.0f;
        if (kind == BPK_PLAY_WIDGET_ANIM && p->type != BPT_ACTOR) p->lit.v[0] = 1.0f; // (Unreal's: once, at normal speed)
        if (kind == BPK_PLAY_SEQUENCE && !strcmp(p->name, "Play Rate")) p->lit.v[0] = 1.0f;
        if (!strcmp(p->name, "Volume Multiplier") || !strcmp(p->name, "Pitch Multiplier") || !strcmp(p->name, "New Volume Multiplier") ||
            !strcmp(p->name, "New Pitch Multiplier") || !strcmp(p->name, "Fade Volume Level") ||
            (kind == BPK_SET_CLASS_VOLUME && p->type == BPT_FLOAT))
            p->lit.v[0] = kind == BPK_AUDIO_FADE_OUT ? 0.0f : 1.0f; // (Unreal's: 1; a fade out's level 0)
        if (!strcmp(p->name, "Fade In Duration") || !strcmp(p->name, "Fade Out Duration")) p->lit.v[0] = 1.0f;
        if ((kind == BPK_NET_RPC_SERVER || kind == BPK_NET_RPC_MULTICAST || kind == BPK_NET_RPC_OWNER) && !strcmp(p->name, "Reliable")) p->lit.v[0] = 1.0f;
        if (kind == BPK_NET_CREATE_SESSION && !strcmp(p->name, "Max Players")) p->lit.v[0] = 4.0f;
        if (kind == BPK_NET_CREATE_ADV && (!strcmp(p->name, "Max Players") || !strcmp(p->name, "Allow Join In Progress") ||
                                          !strcmp(p->name, "Allow Invites") || !strcmp(p->name, "Should Advertise")))
            p->lit.v[0] = !strcmp(p->name, "Max Players") ? 4.0f : 1.0f; // (Unreal's defaults)
        if (kind == BPK_NET_FIND_ADV && !strcmp(p->name, "Max Results")) p->lit.v[0] = 50.0f;
        if (kind == BPK_NET_FIND_ADV && (!strcmp(p->name, "Same Build Only") || !strcmp(p->name, "Exclude Full"))) p->lit.v[0] = 1.0f;
        if (kind == BPK_NET_UPDATE_SESSION && (!strcmp(p->name, "Should Advertise") || !strcmp(p->name, "Allow Join In Progress"))) p->lit.v[0] = 1.0f;
        if (kind == BPK_NET_UPDATE_SESSION && !strcmp(p->name, "Max Players")) p->lit.v[0] = 4.0f;
        if ((kind == BPK_NET_SET_JOINABLE && !strcmp(p->name, "Joinable")) || (kind == BPK_NET_AUTO_INVITES && !strcmp(p->name, "Accept"))) p->lit.v[0] = 1.0f;
        if (kind == BPK_NET_SPAWN && !strcmp(p->name, "Owner Player")) p->lit.v[0] = -1.0f;
        if (kind == BPK_STEAM_DOWNLOAD_SCORES && !strcmp(p->name, "Count")) p->lit.v[0] = 10.0f;
        if ((kind == BPK_STEAM_UPLOAD_SCORE && !strcmp(p->name, "Keep Best")) || (kind == BPK_NET_SET_REPLICATES && !strcmp(p->name, "Replicates"))) p->lit.v[0] = 1.0f;
        if ((kind == BPK_SET_SCALE) && p->type == BPT_VECTOR) p->lit.v[0] = p->lit.v[1] = p->lit.v[2] = 1.0f;
        if (kind == BPK_MAKE_TRANSFORM && !strcmp(p->name, "Scale")) p->lit.v[0] = p->lit.v[1] = p->lit.v[2] = 1.0f;
        if ((kind == BPK_MAKE_COLOR || kind == BPK_HSV_TO_RGB) && !strcmp(p->name, "A")) p->lit.v[0] = 1.0f;
        if (kind == BPK_HSV_TO_RGB && (!strcmp(p->name, "S") || !strcmp(p->name, "V"))) p->lit.v[0] = 1.0f;
        if (kind == BPK_RAND_INT && !strcmp(p->name, "Max")) p->lit.v[0] = 10.0f;
        if (kind == BPK_RAND_INT_RANGE && !strcmp(p->name, "Max")) p->lit.v[0] = 10.0f;
        if (kind == BPK_RAND_BOOL_W && p->type == BPT_FLOAT) p->lit.v[0] = 0.5f;
        if (kind == BPK_RAND_CONE && p->type == BPT_VECTOR) p->lit.v[0] = 1.0f;
        if (kind == BPK_RAND_CONE && p->type == BPT_FLOAT) p->lit.v[0] = 30.0f;
        if (kind == BPK_RAND_POINT_BOX && !strcmp(p->name, "Half Size")) p->lit.v[0] = p->lit.v[1] = p->lit.v[2] = 100.0f;
        if (kind == BPK_MAP_RANGE && (!strcmp(p->name, "In Range B") || !strcmp(p->name, "Out Range B"))) p->lit.v[0] = 1.0f;
        if (kind >= BPK_INV_ADD && kind <= BPK_INV_SLOT_COUNT && !strcmp(p->name, "Count")) p->lit.v[0] = 1.0f; // (Unreal's: one)
        if (kind == BPK_INV_TRANSFER && !strcmp(p->name, "To Slot")) p->lit.v[0] = -1.0f;
        if ((kind == BPK_VEQUAL || kind == BPK_NEARLY_EQUAL) && !strcmp(p->name, "Error Tolerance")) p->lit.v[0] = 0.0001f;
        if ((kind == BPK_FINTERP || kind == BPK_VINTERP || kind == BPK_RINTERP) && !strcmp(p->name, "Interp Speed")) p->lit.v[0] = 5.0f;
        if ((kind == BPK_DO_N && !strcmp(p->name, "N")) || ((kind == BPK_SET_TIMER || kind == BPK_RETRIGGER_DELAY) && p->type == BPT_FLOAT))
            p->lit.v[0] = 1.0f;
        if (kind == BPK_POW && !strcmp(p->name, "Exp")) p->lit.v[0] = 2.0f;
        if ((kind == BPK_APPLY_DAMAGE || kind == BPK_HEAL) && !strcmp(p->name, "Amount")) p->lit.v[0] = kind == BPK_HEAL ? 25.0f : 10.0f;
        if (kind == BPK_APPLY_RADIAL_DAMAGE && p->type == BPT_FLOAT)
            p->lit.v[0] = !strcmp(p->name, "Base Damage") ? 50.0f : !strcmp(p->name, "Damage Radius") ? 500.0f : !strcmp(p->name, "Falloff") ? 1.0f : 0.0f;
        if ((kind == BPK_SET_INVINCIBLE && p->type == BPT_BOOL) || (kind == BPK_SET_DAMAGE_MULT && p->type == BPT_FLOAT)) p->lit.v[0] = 1.0f;
        if (kind == BPK_SET_MAX_HEALTH && p->type == BPT_FLOAT) p->lit.v[0] = 100.0f;
        if ((kind == BPK_SAVE_PROGRESS || kind == BPK_LOAD_PROGRESS) && !strcmp(p->name, "Slot Name")) snprintf(p->slit, sizeof p->slit, "Slot1");
        if (kind == BPK_RESPAWN_CHECKPOINT && p->type == BPT_BOOL) p->lit.v[0] = 1.0f; // (Reload Save)
        if (kind == BPK_SET_AUTO_SAVE_INTERVAL && p->type == BPT_FLOAT) p->lit.v[0] = 300.0f;
        if ((kind == BPK_IA_SET_ENABLED || kind == BPK_IA_DEVICE_SET) && p->type == BPT_BOOL) p->lit.v[0] = 1.0f; // (Unreal's: on)
        if ((kind == BPK_GR_END && p->type == BPT_BOOL) || ((kind == BPK_GR_ADD_SCORE || kind == BPK_GR_ADD_LIVES) && p->type == BPT_INT)) p->lit.v[0] = 1.0f; // (won; 1 point, 1 life)
        if (kind == BPK_LOAD_STREAM && !strcmp(p->name, "Make Visible After Load")) p->lit.v[0] = 1.0f;
        if ((kind == BPK_HUD_MESSAGE || kind == BPK_HUD_ADD_COMPASS || kind == BPK_HUD_ADD_SCREEN) && p->type == BPT_COLOR) // (white; a marker's gold)
            p->lit.v[0] = 1.0f, p->lit.v[1] = kind == BPK_HUD_MESSAGE ? 1.0f : 0.82f, p->lit.v[2] = kind == BPK_HUD_MESSAGE ? 1.0f : 0.25f, p->lit.v[3] = 1.0f;
        if ((kind == BPK_HUD_ADD_COMPASS || kind == BPK_HUD_ADD_SCREEN || kind == BPK_HUD_SET_VISIBLE || kind == BPK_HUD_PHOTO_MODE || kind == BPK_SET_LIGHTING_LEVEL || kind == BPK_ACR_SET || kind == BPK_ACR_AUTO_GET_UP) && p->type == BPT_BOOL) p->lit.v[0] = 1.0f;
        if (kind == BPK_SET_LIGHTING_BLEND && p->type == BPT_FLOAT) p->lit.v[0] = 1.0f;
        if (kind == BPK_ACR_STRENGTH && p->type == BPT_FLOAT) p->lit.v[0] = !strcmp(p->name, "Strength") ? 1.0f : 0.3f;
        if (kind == BPK_ACR_HIT && p->type == BPT_FLOAT) p->lit.v[0] = !strcmp(p->name, "Weaken") ? 0.2f : 0.8f;
        if (kind == BPK_ACR_HIT && p->type == BPT_VECTOR) p->lit.v[0] = 400.0f; // (a push along X)
        if (kind == BPK_ACR_AUTO_GET_UP && p->type == BPT_FLOAT) p->lit.v[0] = 1.5f;
        if (kind == BPK_ACR_WOUND && p->type == BPT_FLOAT) p->lit.v[0] = 0.5f; // (Q27.5: Severity)
        if (kind == BPK_SET_TIME_OF_DAY && p->type == BPT_FLOAT) p->lit.v[0] = !strcmp(p->name, "Hour") ? 9.0f : 60.0f; // (Q27.10)
        if (kind == BPK_REPORT_CRIME && p->type == BPT_FLOAT) p->lit.v[0] = 2500.0f;
        if (kind == BPK_ADD_STORM && p->type == BPT_FLOAT) p->lit.v[0] = !strcmp(p->name, "Radius") ? 80000.0f : !strcmp(p->name, "Intensity") ? 40.0f : 900.0f; // (Q27.6)
        if ((kind == BPK_CONSTRAINT_LIN_DRIVE || kind == BPK_CONSTRAINT_ANG_DRIVE) && p->type == BPT_FLOAT && strncmp(p->name, "Max", 3))
            p->lit.v[0] = !strcmp(p->name, "Frequency") ? 2.0f : 1.0f; // (a 2 Hz spring, no bounce)
        if ((kind == BPK_PHYS_SET_GRAVITY || kind == BPK_PHYS_SET_CCD || kind == BPK_CONSTRAINT_LIN_DRIVE || kind == BPK_CONSTRAINT_ANG_DRIVE) &&
            p->type == BPT_BOOL && strcmp(p->name, "Velocity Drive"))
            p->lit.v[0] = 1.0f;
        if (kind == BPK_SOFT_IMPULSE && p->type == BPT_FLOAT) p->lit.v[0] = 30.0f; // (Q9: a hand's reach)
        if (kind == BPK_SOFT_IMPULSE && p->type == BPT_VECTOR && !strcmp(p->name, "Velocity")) p->lit.v[2] = -300.0f; // (a press down)
        if (kind == BPK_SOFT_STIFFNESS && p->type == BPT_FLOAT) p->lit.v[0] = 12.0f;
        if (kind == BPK_SOFT_PINNED && p->type == BPT_BOOL) p->lit.v[0] = 1.0f;
        if (kind == BPK_PHYS_BLEND_BELOW && p->type == BPT_FLOAT) p->lit.v[0] = !strcmp(p->name, "Weight") ? 0.0f : 0.3f; // (Q10)
        if (kind == BPK_DYN_SET && p->type == BPT_BOOL) p->lit.v[0] = 1.0f;
        if ((kind == BPK_SHAKE_START || kind == BPK_SHAKE_WORLD) && !strcmp(p->name, "Shake")) snprintf(p->slit, sizeof p->slit, "Explosion"); // (Q12)
        if ((kind == BPK_SHAKE_START && p->type == BPT_FLOAT) || (kind == BPK_SHAKE_WORLD && !strcmp(p->name, "Falloff")) || (kind == BPK_HIDE_BONE && p->type == BPT_BOOL)) p->lit.v[0] = 1.0f;
        if (kind == BPK_SHAKE_WORLD && p->type == BPT_FLOAT && strcmp(p->name, "Falloff")) p->lit.v[0] = !strcmp(p->name, "Inner Radius") ? 300.0f : 2000.0f;
        if (kind == BPK_FP_RECOIL && p->type == BPT_FLOAT) p->lit.v[0] = !strcmp(p->name, "Pitch") ? 1.5f : !strcmp(p->name, "Kick") ? 1.0f : 0.0f;
        if (kind == BPK_FP_PRESET && !strcmp(p->name, "Preset")) snprintf(p->slit, sizeof p->slit, "Realistic");
        if (kind == BPK_HIDE_BONE && !strcmp(p->name, "Bone Name")) snprintf(p->slit, sizeof p->slit, "head");
        if (kind == BPK_ACR_PROFILE && !strcmp(p->name, "Profile")) snprintf(p->slit, sizeof p->slit, "Default");
        if (kind == BPK_SET_LOCO_STYLE && !strcmp(p->name, "Style")) snprintf(p->slit, sizeof p->slit, "Injured"); // (Q27.11)
        if (kind == BPK_HUD_ADD_SCREEN && p->type == BPT_FLOAT) p->lit.v[0] = 120.0f; // (Height Offset: over a Character's head)
        if (kind == BPK_HUD_MESSAGE && p->type == BPT_FLOAT) p->lit.v[0] = 3.0f;
        if (kind == BPK_HUD_DAMAGE_NUMBER && p->type == BPT_FLOAT) p->lit.v[0] = 10.0f;
        if (kind == BPK_HUD_SET_SPREAD && p->type == BPT_FLOAT) p->lit.v[0] = 0.5f;
        if (kind == BPK_HUD_BIND_BAR && !strcmp(p->name, "Attribute")) snprintf(p->slit, sizeof p->slit, "Health");
        // (Q11) the toon's: a field named, an outline on and 3 px wide, a red one
        if ((kind == BPK_TOON_SET_PARAM || kind == BPK_TOON_GET_PARAM) && !strcmp(p->name, "Parameter")) snprintf(p->slit, sizeof p->slit, "Bands");
        if (kind == BPK_TOON_SET_PARAM && p->type == BPT_FLOAT) p->lit.v[0] = 3.0f;
        if (kind == BPK_TOON_SET_COLOR && !strcmp(p->name, "Parameter")) snprintf(p->slit, sizeof p->slit, "Shadow Color");
        if (kind == BPK_SET_ACTOR_OUTLINE && p->type == BPT_BOOL) p->lit.v[0] = 1.0f;
        if (kind == BPK_SET_ACTOR_OUTLINE && p->type == BPT_FLOAT) p->lit.v[0] = 3.0f;
        if (kind == BPK_SET_ACTOR_OUTLINE && p->type == BPT_VECTOR) p->lit.v[0] = 1.0f, p->lit.v[1] = 0.1f, p->lit.v[2] = 0.05f;
        // (Q13) the horror kit's defaults: a scare's flash, a held filter, the rigs, the letterbox, the dips, a faulty bulb
        if ((kind == BPK_TRIGGER_FILTER || kind == BPK_SET_FILTER || kind == BPK_GET_FILTER) && !strcmp(p->name, "Filter")) snprintf(p->slit, sizeof p->slit, "White Flash");
        if (kind == BPK_TRIGGER_FILTER && p->type == BPT_FLOAT) p->lit.v[0] = !strcmp(p->name, "Intensity") ? 1.0f : !strcmp(p->name, "Fade In") ? 0.0f : !strcmp(p->name, "Hold") ? 0.05f : 0.15f;
        if (kind == BPK_SET_FILTER && p->type == BPT_FLOAT) p->lit.v[0] = !strcmp(p->name, "Intensity") ? 1.0f : 0.5f;
        if (kind == BPK_CLEAR_FILTERS && p->type == BPT_FLOAT) p->lit.v[0] = 0.5f;
        if ((kind == BPK_SET_TENSION || kind == BPK_ADD_TENSION) && p->type == BPT_FLOAT) p->lit.v[0] = !strcmp(p->name, "Blend Time") ? -1.0f : kind == BPK_SET_TENSION ? 1.0f : 0.2f;
        if (kind == BPK_SET_LETTERBOX && p->type == BPT_FLOAT) p->lit.v[0] = !strcmp(p->name, "Aspect Ratio") ? 2.39f : 1.0f;
        if ((kind == BPK_DIP_BLACK || kind == BPK_DIP_WHITE) && p->type == BPT_FLOAT) p->lit.v[0] = !strcmp(p->name, "Hold") ? 0.3f : 0.5f;
        if (kind == BPK_SCARE_RIG && !strcmp(p->name, "Rig")) snprintf(p->slit, sizeof p->slit, "Dolly Zoom");
        if (kind == BPK_SCARE_RIG && p->type == BPT_FLOAT) p->lit.v[0] = !strcmp(p->name, "Intensity") ? 1.0f : 0.0f;
        if (kind == BPK_SET_LIGHT_FLICKER && !strcmp(p->name, "Pattern")) snprintf(p->slit, sizeof p->slit, "Faulty Bulb");
        if (kind == BPK_SET_LIGHT_FLICKER && p->type == BPT_FLOAT) p->lit.v[0] = 1.0f;
        if (kind == BPK_SET_LIGHT_CHANNELS && p->type == BPT_BOOL && !strcmp(p->name, "Channel 0")) p->lit.v[0] = 1.0f;
        if (kind == BPK_SET_RIM_TARGET && p->type == BPT_BOOL) p->lit.v[0] = 1.0f;
        if (kind == BPK_SET_RIM_TARGET && p->type == BPT_FLOAT) p->lit.v[0] = !strcmp(p->name, "Distance") ? 250.0f : 120.0f;
        if (kind >= BPK_SPLINE_LENGTH && kind <= BPK_CABLE_IMPULSE && p->type == BPT_BOOL) p->lit.v[0] = 1.0f; // (Q14: World Space, Closed Loop, End)
        if (kind == BPK_SPLINE_SET_TYPE && !strcmp(p->name, "Type")) snprintf(p->slit, sizeof p->slit, "Curve");
        if (kind == BPK_CABLE_SET_LENGTH && p->type == BPT_FLOAT) p->lit.v[0] = 500.0f;
        if (kind == BPK_CABLE_IMPULSE && p->type == BPT_FLOAT) p->lit.v[0] = 100.0f;
        if (kind == BPK_PCG_SET_PARAM && p->type == BPT_FLOAT) p->lit.v[0] = 1.0f; // (Q15)
        // (Q16) the bake while playing, in Preview; a light's intensity; the render's share
        if (kind == BPK_BUILD_LIGHTING && !strcmp(p->name, "Mode")) snprintf(p->slit, sizeof p->slit, "Progressive");
        if (kind == BPK_BUILD_LIGHTING && !strcmp(p->name, "Quality")) snprintf(p->slit, sizeof p->slit, "Preview");
        if ((kind == BPK_SET_PROPERTY || kind == BPK_GET_PROPERTY) && !strcmp(p->name, "Property")) snprintf(p->slit, sizeof p->slit, "Intensity");
        if ((kind == BPK_SET_CVAR || kind == BPK_GET_CVAR) && !strcmp(p->name, "Name")) snprintf(p->slit, sizeof p->slit, "r.ScreenPercentage");
        if (kind == BPK_SET_CVAR && p->type == BPT_FLOAT) p->lit.v[0] = 100.0f;
        if (kind == BPK_EXEC_CONSOLE && p->type == BPT_STRING && !p->out) snprintf(p->slit, sizeof p->slit, "r.ScreenPercentage 75");
        if ((kind == BPK_SAVE_LIGHTING || kind == BPK_LOAD_LIGHTING) && p->type == BPT_STRING) snprintf(p->slit, sizeof p->slit, "Seed1");
    }
    a->dirty = true;
    a->compiled = false;
    return n;
}

void bp_remove_node(BpGraph* g, uint32_t id) {
    for (uint32_t i = 0; i < g->nlinks;) {
        if (g->links[i].from_node == id || g->links[i].to_node == id) g->links[i] = g->links[--g->nlinks];
        else i++;
    }
    for (uint32_t i = 0; i < g->nnodes; i++)
        if (g->nodes[i].id == id) {
            g->nodes[i] = g->nodes[--g->nnodes];
            break;
        }
}

static bool vecish(uint8_t t) { return t == BPT_VECTOR || t == BPT_ROTATOR || t == BPT_COLOR; }

// One value's type into another's (a single value or a container's elements, same container)
static bool elem_compatible(uint8_t ft, const char* fref, uint8_t tt, const char* tref, bool convert) {
    if (ft == BPT_WILDCARD || tt == BPT_WILDCARD) return ft != tt; // the wildcard takes the other's type
    if (ft == tt) return (ft != BPT_STRUCT && ft != BPT_ENUM) || !strcmp(fref, tref);
    if (!convert) return false;
    if (bp_type_stringish(tt)) return true; // anything prints (converted)
    if (bp_type_numeric(ft) && bp_type_numeric(tt) && tt != BPT_ENUM) return true; // (Unreal adds the conversion node)
    if (vecish(ft) && vecish(tt)) return true;
    if ((ft == BPT_ACTOR && tt == BPT_OBJECT) || (ft == BPT_OBJECT && tt == BPT_ACTOR)) return true;
    return false;
}

bool bp_types_compatible(const BpPin* from, const BpPin* to) {
    if (!from->out || to->out) return false;
    if (from->type == BPT_EXEC || to->type == BPT_EXEC) return from->type == to->type;
    if (from->cont != to->cont) return false;
    if (from->cont == BPC_MAP && from->vtype != to->vtype && from->vtype != BPT_WILDCARD && to->vtype != BPT_WILDCARD) return false;
    return elem_compatible(from->type, from->ref, to->type, to->ref, from->cont == BPC_SINGLE);
}

// The asset a graph belongs to (wildcards resolve with its pins rebuilt)
static BpAsset* s_parsing;
static BpAsset* owner_of(const BpGraph* g) {
    if (s_parsing && g >= s_parsing->graphs && g < s_parsing->graphs + 16) return s_parsing;
    for (uint32_t i = 0; i < A.n; i++)
        if (g >= A.assets[i]->graphs && g < A.assets[i]->graphs + 16) return A.assets[i];
    return NULL;
}

// A wildcard pin wired to a typed one: the node takes that type (all its wildcard pins), Unreal's way
static void resolve_wild(BpGraph* g, BpNode* n, const BpPin* wp, const BpPin* other) {
    BpAsset* a = owner_of(g);
    if (!a) return;
    bool ch = false;
    if (wp->wslot == 1 && wp->type == BPT_WILDCARD && other->type != BPT_WILDCARD) {
        n->wtype = other->type, ch = true;
        snprintf(n->wref, sizeof n->wref, "%s", other->ref);
    }
    if (wp->cont == BPC_MAP && wp->vtype == BPT_WILDCARD && other->cont == BPC_MAP && other->vtype != BPT_WILDCARD) n->wtype2 = other->vtype, ch = true;
    if (wp->wslot == 2 && wp->type == BPT_WILDCARD && other->type != BPT_WILDCARD) n->wtype2 = other->type, ch = true;
    if (ch) bp_refresh_pins(a, g, n);
}

bool bp_pin_linked(const BpGraph* g, uint32_t node, uint8_t pin) {
    for (uint32_t i = 0; i < g->nlinks; i++)
        if ((g->links[i].from_node == node && g->links[i].from_pin == pin) || (g->links[i].to_node == node && g->links[i].to_pin == pin))
            return true;
    return false;
}

void bp_unlink_pin(BpGraph* g, uint32_t node, uint8_t pin) {
    for (uint32_t i = 0; i < g->nlinks;) {
        BpLink* l = &g->links[i];
        if ((l->from_node == node && l->from_pin == pin) || (l->to_node == node && l->to_pin == pin)) g->links[i] = g->links[--g->nlinks];
        else i++;
    }
}

bool bp_link(BpGraph* g, uint32_t from_node, uint8_t from_pin, uint32_t to_node, uint8_t to_pin) {
    BpNode* f = bp_node(g, from_node);
    BpNode* t = bp_node(g, to_node);
    if (!f || !t || from_node == to_node || from_pin >= f->npins || to_pin >= t->npins) return false;
    if (!f->pins[from_pin].out && t->pins[to_pin].out) { // given the other way round
        BpNode* tmp = f;
        f = t;
        t = tmp;
        uint8_t tp = from_pin;
        from_pin = to_pin;
        to_pin = tp;
    }
    // a Reroute node's untyped pin takes an exec wire too (Unreal's knots carry both)
    bool knot_exec = (f->kind == BPK_REROUTE && f->pins[from_pin].type == BPT_WILDCARD && t->pins[to_pin].type == BPT_EXEC) ||
                     (t->kind == BPK_REROUTE && t->pins[to_pin].type == BPT_WILDCARD && f->pins[from_pin].type == BPT_EXEC);
    if (f->pins[from_pin].delegate != t->pins[to_pin].delegate) return false; // (a delegate only into a delegate)
    if (f->pins[from_pin].delegate) knot_exec = false;
    if (!knot_exec && !bp_types_compatible(&f->pins[from_pin], &t->pins[to_pin])) return false;
    // wildcards take the other side's type (the node's pins rebuilt in the same places)
    BpPin fp = f->pins[from_pin], tp = t->pins[to_pin];
    if (fp.wslot) resolve_wild(g, f, &fp, &tp);
    if (tp.wslot) resolve_wild(g, t, &tp, &f->pins[from_pin]);
    // a data input takes one wire; an exec output leads to one place (Unreal's rules)
    // (a delegate: one event into each Event input, an event's delegate into many, as Unreal's)
    if (t->pins[to_pin].type != BPT_EXEC || t->pins[to_pin].delegate) bp_unlink_pin(g, t->id, to_pin);
    else bp_unlink_pin(g, f->id, from_pin);
    if (g->nlinks == g->cap_links) {
        uint32_t c = g->cap_links ? g->cap_links * 2 : 64;
        BpLink* nl = realloc(g->links, sizeof(BpLink) * c);
        if (!nl) return false;
        g->links = nl;
        g->cap_links = c;
    }
    g->links[g->nlinks++] = (BpLink){ f->id, t->id, from_pin, to_pin };
    return true;
}

void bp_add_function(BpAsset* a, const char* name) { bp_add_graph(a, name, BPG_FUNCTION); }

static BpVar* add_var(BpAsset* a, const char* name, uint8_t type) {
    if (a->nvars >= 64) return NULL;
    BpVar* v = &a->vars[a->nvars++];
    memset(v, 0, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", name);
    v->type = type;
    return v;
}

// The components a class's actors start with (spawn_actor's way): a Character gets a body (a cylinder) and its movement
static void default_components(BpAsset* a) {
    BpNative nat = bp_native_of(a);
    a->comp_mask = COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_BLUEPRINT);
    memset(a->comp_data, 0, sizeof a->comp_data);
    CTransform* t = (CTransform*)a->comp_data[COMP_TRANSFORM];
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    t->flags = MOBILITY_MOVABLE; // scripted actors move
    const BpAsset* parent = bp_find_name(BPA_CLASS, a->parent);
    if (parent && parent != a) { // a child starts with its parent's components
        a->comp_mask = parent->comp_mask;
        memcpy(a->comp_data, parent->comp_data, sizeof a->comp_data);
        memcpy(a->scs, parent->scs, sizeof a->scs);
        a->nscs = parent->nscs;
        return;
    }
    if (!bp_native_spatial(nat)) return; // a controller, a game state, a component...: no body
    a->comp_mask |= COMP_BIT(COMP_STATIC_MESH);
    CStaticMesh* m = (CStaticMesh*)a->comp_data[COMP_STATIC_MESH];
    m->mesh = nat == BPN_CHARACTER ? SHAPE_CYLINDER : SHAPE_CUBE;
    m->base_color[0] = 0.55f, m->base_color[1] = 0.6f, m->base_color[2] = 0.7f;
    m->roughness = 0.5f;
    m->cast_shadow = 1;
    if (nat == BPN_CHARACTER) {
        a->comp_mask |= COMP_BIT(COMP_CHARACTER);
        CCharacter* c = (CCharacter*)a->comp_data[COMP_CHARACTER];
        c->max_walk_speed = 600.0f;
        c->jump_z_velocity = 520.0f;
        c->gravity_scale = 1.0f;
        c->radius = 34.0f;
        c->half_height = 88.0f;
        c->camera_distance = 350.0f;
        t->scale[0] = t->scale[1] = c->radius * 2.0f / 100.0f; // the body: the capsule's size
        t->scale[2] = c->half_height * 2.0f / 100.0f;
        c->auto_possess = 1;
        // Unreal's Third Person template: the arrow, the camera boom (turned by the mouse) and the camera at its end
        a->nscs = 0;
        bp_scs_add(a, SCS_ARROW, BP_SCS_ROOT, "Arrow");
        int boom = bp_scs_add(a, SCS_SPRING_ARM, BP_SCS_ROOT, "CameraBoom");
        if (boom >= 0) {
            a->scs[boom].location[2] = c->half_height * 0.75f;
            ((CSpringArm*)a->scs[boom].data)->target_arm_length = c->camera_distance;
            bp_scs_add(a, SCS_CAMERA, boom, "FollowCamera");
        }
        // the Mannequin as its Mesh, animated by ABP_Mannequin (the engine makes both, anim.c): the cylinder goes
        const SkelMesh* sk = anim_mesh_name("SK_Mannequin");
        int mi = sk ? bp_scs_add(a, SCS_SKELETAL_MESH, BP_SCS_ROOT, "Mesh") : -1;
        if (mi >= 0) {
            a->scs[mi].location[2] = -c->half_height; // its feet at the capsule's bottom
            CSkeletalMesh* skm = (CSkeletalMesh*)a->scs[mi].data;
            skm->mesh = sk->hash;
            const BpAsset* abp = bp_find_name(BPA_CLASS, "ABP_Mannequin");
            skm->anim_class = abp ? abp->hash : 0;
            a->comp_mask &= ~COMP_BIT(COMP_STATIC_MESH);
        }
    }
}

BpAsset* bp_create(BpAssetKind kind, const char* folder_vpath, const char* name, const char* parent) {
    char vpath[600], disk[700];
    snprintf(vpath, sizeof vpath, "%s/%s.%s", folder_vpath, name, g_bp_asset_ext[kind]);
    disk_path(vpath, disk, sizeof disk);
    bool is_dir;
    if (re_file_stat(disk, NULL, NULL, &is_dir)) return NULL;
    BpAsset* a = calloc(1, sizeof *a); // an edit, not a frame
    if (!a) return NULL;
    a->kind = kind;
    snprintf(a->name, sizeof a->name, "%s", name);
    snprintf(a->path, sizeof a->path, "%s", disk);
    a->hash = bp_vpath_hash(vpath);
    snprintf(a->parent, sizeof a->parent, "%s", parent && parent[0] ? parent : "Actor");
    if (kind == BPA_CLASS) {
        add_graph(a, "EventGraph");
        default_components(a);
        if (!bp_find_name(BPA_CLASS, a->parent)) bp_template_graph(a); // a child keeps its parent's graph
        // Unreal's Construction Script: runs when an actor of the class is made (placed, spawned, play starts), before BeginPlay
        if (strcmp(a->parent, "Anim Instance") && strcmp(a->parent, "User Widget") && strcmp(a->parent, "Level Script Actor"))
            bp_add_graph(a, "ConstructionScript", BPG_FUNCTION);
        if (!strcmp(a->parent, "Gameplay Ability")) abil_class_defaults(a); // (ability.h: its class defaults, On Activate -> End)
    } else if (kind == BPA_STRUCT) {
        a->nfields = 2;
        snprintf(a->fields[0].name, BP_NAME, "Value");
        a->fields[0].type = BPT_FLOAT;
        snprintf(a->fields[1].name, BP_NAME, "Position");
        a->fields[1].type = BPT_VECTOR;
    } else if (kind == BPA_ENUM) {
        a->nentries = 3;
        snprintf(a->entries[0], BP_NAME, "NewEnumerator0");
        snprintf(a->entries[1], BP_NAME, "NewEnumerator1");
        snprintf(a->entries[2], BP_NAME, "NewEnumerator2");
    } else if (kind == BPA_INTERFACE) { // its functions are graphs (a signature: Unreal's interface graphs)
        a->nentries = 1;
        snprintf(a->entries[0], BP_NAME, "Interact");
        BpGraph* g = bp_add_graph(a, "Interact", BPG_FUNCTION);
        if (g) {
            g->nparams = 1;
            snprintf(g->params[0].name, BP_NAME, "Instigator");
            g->params[0].type = BPT_ACTOR;
            for (uint32_t i = 0; i < g->nnodes; i++) bp_refresh_pins(a, g, &g->nodes[i]);
        }
    } else if (kind == BPA_MACROLIB) {
        bp_add_graph(a, "NewMacro", BPG_MACRO);
    } else if (kind == BPA_DATA && inv_is_item_asset(a)) { // the ready-made Item Definition (inventory.h)
        inv_item_fields(a);
    } else if (kind == BPA_DATA && abil_is_effect_asset(a)) { // the ready-made Gameplay Effect (ability.h)
        abil_effect_fields(a);
    } else if (kind == BPA_DATA) {
        BpVar* v = add_var(a, "Damage", BPT_FLOAT);
        if (v) v->def.v[0] = 10.0f;
        v = add_var(a, "Display Name", BPT_STRING);
        if (v) snprintf(v->sdef, sizeof v->sdef, "Item");
    }
    if (A.n < MAX_ASSETS) A.assets[A.n++] = a;
    if (!bp_save(a)) return NULL;
    return a;
}

BpNative bp_native_of(const BpAsset* a) {
    for (int depth = 0; a && depth < 16; depth++) {
        for (int n = 0; n < BPN_COUNT; n++)
            if (!strcmp(a->parent, g_bp_native_names[n])) return (BpNative)n;
        a = bp_find_name(BPA_CLASS, a->parent);
    }
    return BPN_ACTOR;
}

bool bp_is_child_of(const BpAsset* a, const char* class_name) {
    for (int depth = 0; a && depth < 16; depth++) {
        if (!strcmp(a->name, class_name) || !strcmp(a->parent, class_name)) return true;
        a = bp_find_name(BPA_CLASS, a->parent);
    }
    return false;
}

// --- Files (JSON: readable, merges well; compiled code is never stored) ---

static void write_value(JsonWriter* w, const char* key, const BpValue* v) { jw_kfloats(w, key, v->v, 4); }

static void write_var(JsonWriter* w, const BpVar* v) {
    jw_obj_begin(w);
    jw_kstr(w, "name", v->name);
    jw_kint(w, "type", v->type);
    if (v->ref[0]) jw_kstr(w, "ref", v->ref);
    write_value(w, "def", &v->def);
    if (v->sdef[0]) jw_kstr(w, "sdef", v->sdef);
    if (v->editable) jw_kbool(w, "editable", true);
    if (v->save_game) jw_kbool(w, "save_game", true);
    if (v->expose_spawn) jw_kbool(w, "expose_on_spawn", true);
    if (v->rep) jw_kint(w, "rep", v->rep);
    if (v->category[0]) jw_kstr(w, "category", v->category);
    if (v->tooltip[0]) jw_kstr(w, "tooltip", v->tooltip);
    if (v->cont) jw_kint(w, "cont", v->cont);
    if (v->vtype) jw_kint(w, "vtype", v->vtype);
    if (v->type == BPT_TRANSFORM) jw_kfloats(w, "def3", v->def3[0].v, 8);
    if (v->nadef) {
        jw_key(w, "adef");
        jw_arr_begin(w);
        for (uint32_t i = 0; i < v->nadef; i++) {
            char buf[160];
            snprintf(buf, sizeof buf, "[%.9g,%.9g,%.9g,%.9g]", v->adef[i].v[0], v->adef[i].v[1], v->adef[i].v[2], v->adef[i].v[3]);
            jw_raw(w, buf);
        }
        jw_arr_end(w);
    }
    jw_obj_end(w);
}

static void read_var(const JsonValue* o, BpVar* v) {
    memset(v, 0, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", json_get_str(o, "name", "Var"));
    v->type = (uint8_t)json_get_int(o, "type", BPT_FLOAT);
    if (v->type >= BPT_COUNT) v->type = BPT_FLOAT;
    snprintf(v->ref, sizeof v->ref, "%s", json_get_str(o, "ref", ""));
    json_get_floats(o, "def", v->def.v, 4);
    snprintf(v->sdef, sizeof v->sdef, "%s", json_get_str(o, "sdef", ""));
    v->editable = json_get_bool(o, "editable", false);
    v->save_game = json_get_bool(o, "save_game", false);
    v->expose_spawn = json_get_bool(o, "expose_on_spawn", false);
    v->rep = (uint8_t)json_get_int(o, "rep", 0);
    snprintf(v->category, sizeof v->category, "%s", json_get_str(o, "category", ""));
    snprintf(v->tooltip, sizeof v->tooltip, "%s", json_get_str(o, "tooltip", ""));
    v->cont = (uint8_t)json_get_int(o, "cont", 0);
    if (v->cont >= BPC_COUNT) v->cont = 0;
    v->vtype = (uint8_t)json_get_int(o, "vtype", 0);
    if (v->vtype >= BPT_COUNT) v->vtype = 0;
    v->def3[1].v[0] = v->def3[1].v[1] = v->def3[1].v[2] = 1.0f; // (a transform's scale)
    json_get_floats(o, "def3", v->def3[0].v, 8);
    const JsonValue* ad = json_get(o, "adef");
    for (size_t i = 0; i < json_len(ad) && v->nadef < BP_MAX_ADEF; i++) {
        const JsonValue* e = json_at(ad, i);
        for (int k = 0; k < 4; k++) v->adef[v->nadef].v[k] = (float)json_num(json_at(e, (size_t)k), 0.0);
        v->nadef++;
    }
}

BpAsset* bp_new_class(const char* vpath, const char* disk_path_, const char* name, const char* parent) {
    BpAsset* a = calloc(1, sizeof *a); // (a script's class: adopted, or freed when its compile failed)
    if (!a) return NULL;
    a->kind = BPA_CLASS;
    snprintf(a->name, sizeof a->name, "%s", name);
    snprintf(a->path, sizeof a->path, "%s", disk_path_);
    a->hash = bp_vpath_hash(vpath);
    snprintf(a->parent, sizeof a->parent, "%s", parent && parent[0] ? parent : "Actor");
    add_graph(a, "EventGraph");
    default_components(a);
    a->generated = true;
    return a;
}
void bp_free_asset(BpAsset* a) {
    if (!a) return;
    bp_debug_forget(a), bp_debug_clear(a); // (its breakpoints go with it)
    free(a->op_src), free(a->dbg_nodes), free(a->dbg_sites), free(a->dbg_vars);
    bp_jit_free(a);
    for (uint32_t gi = 0; gi < a->ngraphs; gi++) free_graph(&a->graphs[gi]);
    bp_timelines_free(a);
    free(a->code), free(a->consts), free(a->init), free(a->arr), free(a);
}
int bp_add_var(BpAsset* a, const char* name, uint8_t type) {
    BpVar* v = add_var(a, name, type);
    return v ? (int)(v - a->vars) : -1;
}

BpAsset* bp_adopt(BpAsset* a) {
    if (!a) return NULL;
    for (uint32_t i = 0; i < A.n; i++)
        if (A.assets[i]->hash == a->hash) {
            BpAsset* old = A.assets[i];
            bp_jit_free(old);
            bp_debug_forget(old), bp_debug_forget(a); // (the debugger's patches: put back below, in the kept one)
            for (uint32_t gi = 0; gi < old->ngraphs; gi++) free_graph(&old->graphs[gi]);
            free(old->code), free(old->consts), free(old->init), free(old->arr);
            free(old->op_src), free(old->dbg_nodes), free(old->dbg_sites), free(old->dbg_vars);
            *old = *a;
            free(a);
            bp_debug_moved(a, old);
            if (old->compiled) bp_debug_compiled(old);
            return old;
        }
    if (A.n >= MAX_ASSETS) return NULL;
    A.assets[A.n++] = a;
    return a;
}

bool bp_save(BpAsset* a) {
    if (a && a->generated) return true; // (a script's class: the script is its file)
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1);
    jw_kstr(&w, "kind", g_bp_asset_ext[a->kind]);
    jw_kstr(&w, "name", a->name);
    jw_kstr(&w, "parent", a->parent);
    jw_key(&w, "vars");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < a->nvars; i++) write_var(&w, &a->vars[i]);
    jw_arr_end(&w);
    jw_key(&w, "fields");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < a->nfields; i++) write_var(&w, &a->fields[i]);
    jw_arr_end(&w);
    jw_key(&w, "entries");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < a->nentries; i++) jw_str(&w, a->entries[i]);
    jw_arr_end(&w);
    if (a->description[0]) jw_kstr(&w, "description", a->description);
    bool any_desc = false;
    for (uint32_t i = 0; i < a->nentries; i++) any_desc |= a->entry_desc[i][0] != 0;
    if (any_desc) { // (an Enumeration's entries' descriptions, in the entries' order)
        jw_key(&w, "entry_desc");
        jw_arr_begin(&w);
        for (uint32_t i = 0; i < a->nentries; i++) jw_str(&w, a->entry_desc[i]);
        jw_arr_end(&w);
    }
    jw_key(&w, "interfaces");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < a->ninterfaces; i++) jw_str(&w, a->interfaces[i]);
    jw_arr_end(&w);
    // the components: the mask and each one's bytes as hex
    char mask[24];
    snprintf(mask, sizeof mask, "%llu", (unsigned long long)a->comp_mask);
    jw_kstr(&w, "components", mask);
    jw_key(&w, "component_data");
    jw_obj_begin(&w);
    for (int c = 0; c < COMP_COUNT; c++) {
        if (!(a->comp_mask & COMP_BIT(c)) || c == COMP_NAME || c == COMP_BLUEPRINT) continue;
        char key[8], hex[2 * sizeof a->comp_data[0] + 2];
        snprintf(key, sizeof key, "%d", c);
        for (uint32_t b = 0; b < sizeof a->comp_data[0] && b * 2 + 2 < sizeof hex; b++) snprintf(hex + b * 2, 3, "%02x", a->comp_data[c][b]);
        jw_kstr(&w, key, hex);
    }
    jw_obj_end(&w);
    bp_timelines_write(&w, a); // (2026-11-16) its Timelines
    jw_key(&w, "scs"); // the component tree
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < a->nscs; i++) {
        const BpScsNode* sn = &a->scs[i];
        char hex[BP_SCS_DATA * 2 + 1];
        jw_obj_begin(&w);
        jw_kstr(&w, "name", sn->name);
        jw_kint(&w, "kind", sn->kind);
        jw_kint(&w, "parent", sn->parent);
        jw_kfloats(&w, "location", sn->location, 3);
        jw_kfloats(&w, "rotation", sn->rotation, 3);
        jw_kfloats(&w, "scale", sn->scale, 3);
        uint32_t nb = BP_SCS_DATA;
        while (nb > 96 && !sn->data[nb - 1]) nb--; // (trailing zeros left out: most components are small)
        for (uint32_t b = 0; b < nb; b++) snprintf(hex + b * 2, 3, "%02x", sn->data[b]);
        jw_kstr(&w, "data", hex);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    if (a->anim) { // an Animation Blueprint's Anim Graph (anim.c)
        jw_key(&w, "anim_graph");
        anim_graph_write(a->anim, &w);
    }
    if (a->ui) { // a Widget Blueprint's designer tree (ui_widget.c)
        jw_key(&w, "ui_tree");
        ui_tree_write(a->ui, &w);
        jw_key(&w, "ui_anims"); // (its Widget Animations: a key of their own, older files simply lack it)
        ui_anims_write(a->ui, &w);
    }
    jw_key(&w, "graphs");
    jw_arr_begin(&w);
    for (uint32_t gi = 0; gi < a->ngraphs; gi++) {
        const BpGraph* g = &a->graphs[gi];
        jw_obj_begin(&w);
        jw_kstr(&w, "name", g->name);
        jw_kint(&w, "next_id", g->next_id);
        jw_kfloats(&w, "view", (float[]){ g->pan[0], g->pan[1], g->zoom }, 3);
        if (g->gkind) jw_kint(&w, "gkind", g->gkind);
        if (g->iface[0]) jw_kstr(&w, "iface", g->iface);
        jw_key(&w, "params");
        jw_arr_begin(&w);
        for (uint8_t i = 0; i < g->nparams; i++) write_var(&w, &g->params[i]);
        jw_arr_end(&w);
        jw_key(&w, "results");
        jw_arr_begin(&w);
        for (uint8_t i = 0; i < g->nresults; i++) write_var(&w, &g->results[i]);
        jw_arr_end(&w);
        jw_key(&w, "nodes");
        jw_arr_begin(&w);
        for (uint32_t i = 0; i < g->nnodes; i++) {
            const BpNode* n = &g->nodes[i];
            jw_obj_begin(&w);
            jw_kint(&w, "id", n->id);
            jw_kstr(&w, "kind", g_nodes[n->kind].d.name);
            jw_kfloats(&w, "pos", (float[]){ n->x, n->y }, 2);
            if (n->text[0]) jw_kstr(&w, "text", n->text);
            if (n->text2[0]) jw_kstr(&w, "text2", n->text2);
            if (n->comment[0]) jw_kstr(&w, "comment", n->comment);
            if (n->wtype) jw_kint(&w, "wtype", n->wtype);
            if (n->wtype2) jw_kint(&w, "wtype2", n->wtype2);
            if (n->wref[0]) jw_kstr(&w, "wref", n->wref);
            if (n->split) jw_kint(&w, "split", n->split);
            jw_kint(&w, "extra", n->extra);
            if (n->net) jw_kint(&w, "net", n->net);
            jw_key(&w, "pins");
            jw_arr_begin(&w);
            for (uint8_t p = 0; p < n->npins; p++) {
                jw_obj_begin(&w);
                jw_kstr(&w, "name", n->pins[p].name);
                write_value(&w, "lit", &n->pins[p].lit);
                if (n->pins[p].slit[0]) jw_kstr(&w, "slit", n->pins[p].slit);
                jw_obj_end(&w);
            }
            jw_arr_end(&w);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_key(&w, "links");
        jw_arr_begin(&w);
        for (uint32_t i = 0; i < g->nlinks; i++) {
            const BpLink* l = &g->links[i];
            char buf[96];
            snprintf(buf, sizeof buf, "[%u,%u,%u,%u]", l->from_node, l->from_pin, l->to_node, l->to_pin);
            jw_raw(&w, buf);
        }
        jw_arr_end(&w);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    FILE* f = re_fopen(a->path, "wb");
    bool ok = f && fwrite(w.buf, 1, w.len, f) == w.len;
    if (f) fclose(f);
    jw_free(&w);
    if (ok) a->dirty = false;
    return ok;
}

static BpAsset* parse_asset(const char* text, size_t len, const char* disk, uint32_t hash) {
    JsonDoc doc;
    char err[128];
    if (!json_parse(text, len, &doc, err, sizeof err)) return NULL;
    const JsonValue* r = &doc.root;
    BpAsset* a = calloc(1, sizeof *a); // a load, not a frame
    if (!a) {
        json_free(&doc);
        return NULL;
    }
    snprintf(a->path, sizeof a->path, "%s", disk);
    a->hash = hash;
    s_parsing = a;
    const char* kind = json_get_str(r, "kind", "rbp");
    for (int k = 0; k < BPA_COUNT; k++)
        if (!strcmp(kind, g_bp_asset_ext[k])) a->kind = (BpAssetKind)k;
    snprintf(a->name, sizeof a->name, "%s", json_get_str(r, "name", "Blueprint"));
    snprintf(a->parent, sizeof a->parent, "%s", json_get_str(r, "parent", "Actor"));
    const JsonValue* arr = json_get(r, "vars");
    for (size_t i = 0; i < json_len(arr) && a->nvars < 64; i++) read_var(json_at(arr, i), &a->vars[a->nvars++]);
    arr = json_get(r, "fields");
    for (size_t i = 0; i < json_len(arr) && a->nfields < BP_MAX_FIELDS; i++) read_var(json_at(arr, i), &a->fields[a->nfields++]);
    arr = json_get(r, "entries");
    for (size_t i = 0; i < json_len(arr) && a->nentries < 32; i++)
        snprintf(a->entries[a->nentries++], BP_NAME, "%s", json_str(json_at(arr, i), ""));
    snprintf(a->description, sizeof a->description, "%s", json_get_str(r, "description", ""));
    arr = json_get(r, "entry_desc");
    for (size_t i = 0; i < json_len(arr) && i < 32; i++) snprintf(a->entry_desc[i], sizeof a->entry_desc[i], "%s", json_str(json_at(arr, i), ""));
    arr = json_get(r, "interfaces");
    for (size_t i = 0; i < json_len(arr) && a->ninterfaces < 8; i++)
        snprintf(a->interfaces[a->ninterfaces++], BP_NAME, "%s", json_str(json_at(arr, i), ""));
    a->comp_mask = strtoull(json_get_str(r, "components", "0"), NULL, 10);
    const JsonValue* cd = json_get(r, "component_data");
    for (int c = 0; c < COMP_COUNT; c++) {
        char key[8];
        snprintf(key, sizeof key, "%d", c);
        const char* hex = json_get_str(cd, key, NULL);
        for (uint32_t b = 0; hex && b < sizeof a->comp_data[0] && hex[b * 2] && hex[b * 2 + 1]; b++) {
            unsigned int x = 0;
            sscanf(hex + b * 2, "%2x", &x);
            a->comp_data[c][b] = (uint8_t)x;
        }
    }
    if (a->kind == BPA_CLASS) a->comp_mask |= COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_BLUEPRINT);
    bp_timelines_read(r, a); // (before the graphs: the Timeline nodes' pins)
    arr = json_get(r, "scs");
    a->nscs = 0;
    for (size_t i = 0; i < json_len(arr) && a->nscs < BP_MAX_SCS; i++) {
        const JsonValue* so = json_at(arr, i);
        BpScsNode* sn = &a->scs[a->nscs];
        memset(sn, 0, sizeof *sn);
        sn->kind = (uint16_t)json_get_int(so, "kind", 0);
        if (sn->kind >= SCS_KIND_COUNT) continue;
        snprintf(sn->name, sizeof sn->name, "%s", json_get_str(so, "name", "Component"));
        int par = json_get_int(so, "parent", BP_SCS_ROOT);
        sn->parent = (int16_t)(par >= 0 && par < (int)a->nscs ? par : BP_SCS_ROOT); // parents come first
        sn->scale[0] = sn->scale[1] = sn->scale[2] = 1.0f;
        json_get_floats(so, "location", sn->location, 3);
        json_get_floats(so, "rotation", sn->rotation, 3);
        json_get_floats(so, "scale", sn->scale, 3);
        const char* hex = json_get_str(so, "data", NULL);
        uint32_t nread = 0;
        for (uint32_t b = 0; hex && b < BP_SCS_DATA && hex[b * 2] && hex[b * 2 + 1]; b++, nread++) {
            unsigned int x = 0;
            sscanf(hex + b * 2, "%2x", &x);
            sn->data[b] = (uint8_t)x;
        }
        if (sn->kind == SCS_SKELETAL_MESH && nread <= 96) { // (saved while it didn't fit: its defaults were never set)
            CSkeletalMesh* sk = (CSkeletalMesh*)sn->data;
            if (sk->play_rate == 0.0f) sk->play_rate = 1.0f, sk->loop = 1;
            if (sk->physics_blend == 0.0f) sk->physics_blend = 1.0f;
            memset(sk->morph_weight, 0, sizeof sk->morph_weight), memset(sk->morph_set, 0, sizeof sk->morph_set);
        }
        a->nscs++;
    }
    const JsonValue* agj = json_get(r, "anim_graph");
    if (agj && !a->anim) a->anim = calloc(1, sizeof(AnimGraph)); // a load, not a frame
    if (agj && a->anim) anim_graph_read(a->anim, agj);
    const JsonValue* uij = json_get(r, "ui_tree");
    if (uij && !a->ui) a->ui = calloc(1, sizeof(UiTree)); // a load, not a frame
    if (uij && a->ui) ui_tree_read(a->ui, uij);
    if (uij && a->ui) ui_anims_read(a->ui, json_get(r, "ui_anims"));
    arr = json_get(r, "graphs");
    for (size_t gi = 0; gi < json_len(arr) && a->ngraphs < 16; gi++) {
        const JsonValue* go = json_at(arr, gi);
        BpGraph* g = add_graph(a, json_get_str(go, "name", "EventGraph"));
        float view[3] = { 0, 0, 1 };
        json_get_floats(go, "view", view, 3);
        g->pan[0] = view[0], g->pan[1] = view[1], g->zoom = view[2] > 0.1f ? view[2] : 1.0f;
        g->gkind = (uint8_t)json_get_int(go, "gkind", 0);
        snprintf(g->iface, sizeof g->iface, "%s", json_get_str(go, "iface", ""));
        const JsonValue* ps = json_get(go, "params");
        for (size_t i = 0; i < json_len(ps) && g->nparams < 6; i++) read_var(json_at(ps, i), &g->params[g->nparams++]);
        ps = json_get(go, "results");
        for (size_t i = 0; i < json_len(ps) && g->nresults < 4; i++) read_var(json_at(ps, i), &g->results[g->nresults++]);
    }
    // the nodes after every graph exists (a call's pins need the function's signature)
    for (size_t gi = 0; gi < json_len(arr) && gi < a->ngraphs; gi++) {
        const JsonValue* go = json_at(arr, gi);
        BpGraph* g = &a->graphs[gi];
        const JsonValue* nodes = json_get(go, "nodes");
        for (size_t i = 0; i < json_len(nodes); i++) {
            const JsonValue* no = json_at(nodes, i);
            uint32_t kind = bp_kind_by_name(json_get_str(no, "kind", ""));
            if (kind >= BPN_KIND_COUNT) continue;
            float pos[2] = { 0, 0 };
            json_get_floats(no, "pos", pos, 2);
            BpNode* n = bp_add_node(a, g, kind, pos[0], pos[1], json_get_str(no, "text", ""), json_get_str(no, "text2", ""));
            if (!n) continue;
            n->id = (uint32_t)json_get_int(no, "id", (int)n->id);
            snprintf(n->comment, sizeof n->comment, "%s", json_get_str(no, "comment", ""));
            n->wtype = (uint8_t)json_get_int(no, "wtype", 0);
            n->wtype2 = (uint8_t)json_get_int(no, "wtype2", 0);
            if (n->wtype >= BPT_COUNT) n->wtype = 0;
            if (n->wtype2 >= BPT_COUNT) n->wtype2 = 0;
            snprintf(n->wref, sizeof n->wref, "%s", json_get_str(no, "wref", ""));
            n->split = (uint16_t)json_get_int(no, "split", 0);
            // (a Sequence of the old files had three outputs: one added)
            n->extra = (uint8_t)json_get_int(no, "extra", kind == BPK_SEQUENCE ? 1 : 0);
            n->net = (uint8_t)json_get_int(no, "net", 0);
            if (n->wtype || n->split || n->extra) bp_refresh_pins(a, g, n);
            const JsonValue* pins = json_get(no, "pins");
            for (size_t p = 0; p < json_len(pins); p++) {
                const JsonValue* po = json_at(pins, p);
                const char* pname = json_get_str(po, "name", "");
                for (uint8_t k = 0; k < n->npins; k++)
                    if (!strcmp(n->pins[k].name, pname)) {
                        json_get_floats(po, "lit", n->pins[k].lit.v, 4);
                        snprintf(n->pins[k].slit, sizeof n->pins[k].slit, "%s", json_get_str(po, "slit", n->pins[k].slit));
                        break;
                    }
            }
            // (2026-11-10) Spawn Actor: its Class is known only now (a pin's literal): its Expose on Spawn pins made, and their
            // saved values given again (the class not loaded yet: placeholder pins keep them until it is). Before, the
            // values were dropped (no pin yet) and the pins came back later at 0
            if (kind == BPK_SPAWN || kind == BPK_SPAWN_ASYNC) {
                bp_refresh_pins(a, g, n);
                for (size_t p = 0; p < json_len(pins); p++) {
                    const JsonValue* po = json_at(pins, p);
                    const char* pname = json_get_str(po, "name", "");
                    int k = -1;
                    for (uint8_t q = 0; q < n->npins && k < 0; q++)
                        if (!n->pins[q].out && !strcmp(n->pins[q].name, pname)) k = q;
                    if (k < 0 && n->npins < BP_MAX_PINS && p >= 3) { // (past Exec, Class, Spawn Transform: an exposed variable's)
                        k = n->npins++;
                        memset(&n->pins[k], 0, sizeof n->pins[k]);
                        snprintf(n->pins[k].name, sizeof n->pins[k].name, "%s", pname);
                        n->pins[k].type = BPT_FLOAT, n->pins[k].spawnarg = 1;
                    }
                    if (k < 0 || !n->pins[k].spawnarg) continue;
                    json_get_floats(po, "lit", n->pins[k].lit.v, 4);
                    snprintf(n->pins[k].slit, sizeof n->pins[k].slit, "%s", json_get_str(po, "slit", n->pins[k].slit));
                }
            }
        }
        g->next_id = (uint32_t)json_get_int(go, "next_id", (int)g->next_id);
        for (uint32_t i = 0; i < g->nnodes; i++)
            if (g->nodes[i].id >= g->next_id) g->next_id = g->nodes[i].id + 1;
        const JsonValue* links = json_get(go, "links");
        for (size_t i = 0; i < json_len(links); i++) {
            const JsonValue* lo = json_at(links, i);
            if (json_len(lo) < 4) continue;
            if (!bp_link(g, (uint32_t)json_int(json_at(lo, 0), 0), (uint8_t)json_int(json_at(lo, 1), 0), (uint32_t)json_int(json_at(lo, 2), 0),
                         (uint8_t)json_int(json_at(lo, 3), 0)) &&
                s_scanning)
                a->relink = true; // (a pin from another asset not read yet: scan_all reads this one again)
        }
    }
    if (a->kind == BPA_CLASS && a->ngraphs == 0) add_graph(a, "EventGraph");
    if (a->kind == BPA_INTERFACE && a->ngraphs == 0) // an old interface (names only): its functions become graphs
        for (uint32_t i = 0; i < a->nentries; i++) bp_add_graph(a, a->entries[i], BPG_FUNCTION);
    if (a->kind == BPA_INTERFACE) { // (entries: the functions' names, for the code that lists them)
        a->nentries = 0;
        for (uint32_t i = 0; i < a->ngraphs && a->nentries < 32; i++) snprintf(a->entries[a->nentries++], BP_NAME, "%s", a->graphs[i].name);
    }
    s_parsing = NULL;
    json_free(&doc);
    a->dirty = false;
    return a;
}

BpAsset* bp_load(const char* vpath) {
    uint32_t h = bp_vpath_hash(vpath);
    for (uint32_t i = 0; i < A.n; i++)
        if (A.assets[i]->hash == h) return A.assets[i];
    if (!s_scanned && !s_scanning) { // what its pins name first
        scan_all();
        for (uint32_t i = 0; i < A.n; i++)
            if (A.assets[i]->hash == h) return A.assets[i];
    }
    char disk[700];
    disk_path(vpath, disk, sizeof disk);
    size_t len = 0;
    char* text = read_text(disk, &len);
    if (!text) return NULL;
    BpAsset* a = parse_asset(text, len, disk, h);
    free(text);
    if (!a || A.n >= MAX_ASSETS) {
        if (a) free_asset(a);
        return NULL;
    }
    A.assets[A.n++] = a;
    // the pins that depend on other assets (structs, enums, data assets, another class's dispatcher) once those are
    // loaded too: during the folder scan, after it (scan_all). Refreshed here, a class read before the one it binds to
    // (BP_TestDispListener before BP_TestDispSource: the file order) lost its Event's Amount pin and the wire on it
    if (!s_scanning) {
        for (uint32_t gi = 0; gi < a->ngraphs; gi++)
            for (uint32_t i = 0; i < a->graphs[gi].nnodes; i++) bp_refresh_pins(a, &a->graphs[gi], &a->graphs[gi].nodes[i]);
    }
    a->dirty = false;
    return a;
}

// Every asset of the Content folder loaded (a level's Blueprints, a name looked up): a recursive walk, once per need
typedef struct ScanCtx { const char* disk; const char* vpath; int depth; } ScanCtx;
static void scan(const char* disk, const char* vpath, int depth);
static void scan_entry(void* user, const ReDirEntry* e) {
    const ScanCtx* c = user;
    char d2[700], v2[700];
    snprintf(d2, sizeof d2, "%s/%s", c->disk, e->name);
    snprintf(v2, sizeof v2, "%s/%s", c->vpath, e->name);
    if (e->is_dir) {
        scan(d2, v2, c->depth + 1);
        return;
    }
    bool is_bp;
    BpAssetKind k = bp_kind_of_path(e->name, &is_bp);
    if (is_bp && (k == BPA_CLASS) == (s_scan_pass == 1)) bp_load(v2);
}
static void scan(const char* disk, const char* vpath, int depth) {
    if (depth > 8) return;
    ScanCtx c = { disk, vpath, depth };
    re_dir_walk(disk, scan_entry, &c);
}

// 0: structures, enumerations, interfaces, data assets (what classes' pins name); 1: classes
static void scan_all(void) {
    if (s_scanning) return;
    s_scanning = true;
    uint32_t first = A.n;
    for (s_scan_pass = 0; s_scan_pass < 2; s_scan_pass++) scan(A.root, "/Content", 0);
    s_scanning = false;
    s_scanned = true;
    for (uint32_t k = first; k < A.n; k++) { // a wire lost to the file order: the asset read again, everything in now
        BpAsset* a = A.assets[k];
        if (!a->relink) continue;
        size_t len = 0;
        char* text = read_text(a->path, &len);
        BpAsset* b = text ? parse_asset(text, len, a->path, a->hash) : NULL;
        free(text);
        if (!b) continue;
        for (uint32_t gi = 0; gi < a->ngraphs; gi++) free_graph(&a->graphs[gi]);
        bp_jit_free(a);
        free(a->code), free(a->consts), free(a->init);
        *a = *b; // (the same pointer: what holds the asset keeps it)
        free(b);
        a->relink = false;
    }
    for (uint32_t k = first; k < A.n; k++) { // (every asset is in: the pins that name another one, bp_load)
        BpAsset* a = A.assets[k];
        bool dirty = a->dirty;
        for (uint32_t gi = 0; gi < a->ngraphs; gi++)
            for (uint32_t i = 0; i < a->graphs[gi].nnodes; i++) bp_refresh_pins(a, &a->graphs[gi], &a->graphs[gi].nodes[i]);
        a->dirty = dirty;
    }
}

BpAsset* bp_find_hash(uint32_t hash) {
    for (uint32_t i = 0; i < s_nrenamed; i++) // an old name of a renamed asset
        if (s_renamed[i][0] == hash) hash = s_renamed[i][1];
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < A.n; i++)
            if (A.assets[i]->hash == hash) return A.assets[i];
        if (!pass && !s_scanned) scan_all();
    }
    return NULL;
}

uint32_t bp_asset_count(void) {
    if (!s_scanned) scan_all();
    return A.n;
}
BpAsset* bp_asset_at(uint32_t i) { return i < A.n ? A.assets[i] : NULL; }

void bp_renamed(const char* old_vpath, const char* new_vpath) {
    uint32_t oh = bp_vpath_hash(old_vpath);
    for (uint32_t i = 0; i < A.n; i++) {
        BpAsset* a = A.assets[i];
        if (a->hash != oh) continue;
        char old_name[BP_NAME];
        snprintf(old_name, sizeof old_name, "%s", a->name);
        const char* base = strrchr(new_vpath, '/');
        base = base ? base + 1 : new_vpath;
        const char* dot = strrchr(base, '.');
        int len = dot ? (int)(dot - base) : (int)strlen(base);
        snprintf(a->name, sizeof a->name, "%.*s", len, base);
        disk_path(new_vpath, a->path, sizeof a->path);
        a->hash = bp_vpath_hash(new_vpath);
        if (s_nrenamed < 64) s_renamed[s_nrenamed][0] = oh, s_renamed[s_nrenamed++][1] = a->hash;
        bp_save(a);
        // what named it by its old name: parents, casts, spawns, structs and enums in pins, interfaces, data assets
        for (uint32_t k = 0; k < A.n; k++) {
            BpAsset* o = A.assets[k];
            bool changed = false;
            if (!strcmp(o->parent, old_name)) snprintf(o->parent, sizeof o->parent, "%s", a->name), changed = true;
            for (uint32_t f = 0; f < o->ninterfaces; f++)
                if (!strcmp(o->interfaces[f], old_name)) snprintf(o->interfaces[f], BP_NAME, "%s", a->name), changed = true;
            for (uint32_t v = 0; v < o->nvars; v++)
                if (!strcmp(o->vars[v].ref, old_name)) snprintf(o->vars[v].ref, BP_NAME, "%s", a->name), changed = true;
            for (uint32_t g = 0; g < o->ngraphs; g++)
                for (uint32_t n = 0; n < o->graphs[g].nnodes; n++) {
                    BpNode* nd = &o->graphs[g].nodes[n];
                    if (nd->kind == BPK_VAR_GET || nd->kind == BPK_VAR_SET || nd->kind == BPK_CALL_FUNCTION || nd->kind == BPK_FUNC_ENTRY ||
                        nd->kind == BPK_FUNC_RETURN || nd->kind == BPK_EVENT_CUSTOM || nd->kind == BPK_CALL_CUSTOM || nd->kind == BPK_EVENT_KEY ||
                        nd->kind == BPK_AXIS || nd->kind == BPK_KEY_DOWN || nd->kind == BPK_EVENT_INTERFACE)
                        continue;
                    if (!strcmp(nd->text, old_name)) snprintf(nd->text, sizeof nd->text, "%s", a->name), changed = true;
                }
            if (changed) {
                o->compiled = false;
                bp_save(o);
            }
        }
        return;
    }
}

void bp_refresh_all(void) {
    for (uint32_t i = 0; i < A.n; i++) {
        BpAsset* a = A.assets[i];
        bool dirty = a->dirty;
        for (uint32_t g = 0; g < a->ngraphs; g++)
            for (uint32_t n = 0; n < a->graphs[g].nnodes; n++) bp_refresh_pins(a, &a->graphs[g], &a->graphs[g].nodes[n]);
        a->dirty = dirty;
        a->compiled = false;
    }
}

BpAsset* bp_find_name(BpAssetKind kind, const char* name) {
    if (!name || !name[0]) return NULL;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < A.n; i++)
            if (A.assets[i]->kind == kind && !strcmp(A.assets[i]->name, name)) return A.assets[i];
        if (!pass && !s_scanned) scan_all();
    }
    return NULL;
}

// --- Unreal's ready-made graph for a Character: walk with W/A/S/D along the view, look with the mouse, jump with Space
void bp_template_graph(BpAsset* a) {
    if (a->ngraphs == 0) return;
    BpGraph* g = &a->graphs[0];
    BpNode* bp = bp_add_node(a, g, BPK_EVENT_BEGIN_PLAY, -420, -260, NULL, NULL);
    BpNode* pr = bp_add_node(a, g, BPK_PRINT, -80, -260, NULL, NULL);
    snprintf(pr->pins[1].slit, sizeof pr->pins[1].slit, "%s", bp_native_of(a) == BPN_CHARACTER ? "Hello from the Character!" : "Hello!");
    bp_link(g, bp->id, 0, pr->id, 0);
    if (bp_native_of(a) != BPN_CHARACTER) return;
    BpNode* tick = bp_add_node(a, g, BPK_EVENT_TICK, -760, 0, NULL, NULL);
    BpNode* fwd = bp_add_node(a, g, BPK_CONTROL_FORWARD, -760, 140, NULL, NULL);
    BpNode* axf = bp_add_node(a, g, BPK_AXIS, -760, 220, "MoveForward", NULL);
    BpNode* mv1 = bp_add_node(a, g, BPK_MOVE_INPUT, -420, 40, NULL, NULL);
    BpNode* rgt = bp_add_node(a, g, BPK_CONTROL_RIGHT, -420, 220, NULL, NULL);
    BpNode* axr = bp_add_node(a, g, BPK_AXIS, -420, 300, "MoveRight", NULL);
    BpNode* mv2 = bp_add_node(a, g, BPK_MOVE_INPUT, -80, 40, NULL, NULL);
    BpNode* axt = bp_add_node(a, g, BPK_AXIS, -80, 220, "Turn", NULL);
    BpNode* yaw = bp_add_node(a, g, BPK_YAW_INPUT, 260, 40, NULL, NULL);
    BpNode* axl = bp_add_node(a, g, BPK_AXIS, 260, 220, "LookUp", NULL);
    BpNode* pit = bp_add_node(a, g, BPK_PITCH_INPUT, 560, 40, NULL, NULL);
    bp_link(g, tick->id, 0, mv1->id, 0);
    bp_link(g, fwd->id, 0, mv1->id, 1);
    bp_link(g, axf->id, 0, mv1->id, 2);
    bp_link(g, mv1->id, 3, mv2->id, 0);
    bp_link(g, rgt->id, 0, mv2->id, 1);
    bp_link(g, axr->id, 0, mv2->id, 2);
    bp_link(g, mv2->id, 3, yaw->id, 0);
    bp_link(g, axt->id, 0, yaw->id, 1);
    bp_link(g, yaw->id, 2, pit->id, 0);
    bp_link(g, axl->id, 0, pit->id, 1);
    BpNode* key = bp_add_node(a, g, BPK_EVENT_KEY, -760, 420, "Space", NULL);
    BpNode* jump = bp_add_node(a, g, BPK_JUMP, -420, 420, NULL, NULL);
    bp_link(g, key->id, 0, jump->id, 0);
    snprintf(tick->comment, sizeof tick->comment, "Walk with W/A/S/D, look with the mouse");
}
