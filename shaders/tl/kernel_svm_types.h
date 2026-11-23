// Shader node types and the layout of their compiled instructions (SVM, after Cycles' "shader
// virtual machine"). Shared by the node editor data (nodes.c), the compiler (svm_compile.c) and
// the interpreter (kernel_svm.h), so the numbers here are the opcodes too.
//
// Program format: ints and floats (bit-cast) inside kfloats. A node instruction is
//   [type, prop0, prop1, prop2, prop3, in_0 .. in_{NIN-1}, out_0 .. out_{NOUT-1}]
// Closure nodes put their weight slot right after the props: [type, props(4), weight, in_0 ...].
// An input operand >= 0 is a stack slot; < 0 is minus the kfloats address of a constant.
// Vectors and colors take 3 consecutive floats. An output operand of -1 is not written.
#ifndef KERNEL_SVM_TYPES_H
#define KERNEL_SVM_TYPES_H

#define SVM_STACK_SIZE 255
#define SVM_PROPS 4
#define SVM_HEADER (1 + SVM_PROPS)

// --- Shaders -----------------------------------------------------------------------------
#define NODE_OUTPUT_MATERIAL 1
#define NODE_BSDF_PRINCIPLED 2
#define NODE_BSDF_DIFFUSE 3
#define NODE_BSDF_GLOSSY 4
#define NODE_BSDF_GLASS 5
#define NODE_BSDF_TRANSLUCENT 6
#define NODE_BSDF_TRANSPARENT 7
#define NODE_EMISSION 8
#define NODE_MIX_SHADER 9
#define NODE_ADD_SHADER 10
#define NODE_PRINCIPLED_VOLUME 11
#define NODE_VOLUME_ABSORPTION 12
#define NODE_VOLUME_SCATTER 13
#define NODE_BSDF_SHEEN 14
// --- Input -------------------------------------------------------------------------------
#define NODE_TEX_COORD 20
#define NODE_GEOMETRY 21
#define NODE_OBJECT_INFO 22
#define NODE_RGB 23
#define NODE_VALUE 24
#define NODE_FRESNEL 25
#define NODE_LAYER_WEIGHT 26
#define NODE_LIGHT_PATH 27
// --- Textures ----------------------------------------------------------------------------
#define NODE_TEX_NOISE 30
#define NODE_TEX_VORONOI 31
#define NODE_TEX_CHECKER 32
#define NODE_TEX_GRADIENT 33
#define NODE_TEX_WAVE 34
#define NODE_TEX_WHITE_NOISE 35
#define NODE_TEX_IMAGE 36
// --- Color -------------------------------------------------------------------------------
#define NODE_MIX 40
#define NODE_INVERT 41
#define NODE_HUE_SAT 42
#define NODE_BRIGHT_CONTRAST 43
#define NODE_GAMMA 44
// --- Converter ---------------------------------------------------------------------------
#define NODE_COLOR_RAMP 50
#define NODE_MATH 51
#define NODE_VECTOR_MATH 52
#define NODE_MAP_RANGE 53
#define NODE_CLAMP 54
#define NODE_SEPARATE_XYZ 55
#define NODE_COMBINE_XYZ 56
#define NODE_SEPARATE_COLOR 57
#define NODE_COMBINE_COLOR 58
#define NODE_RGB_TO_BW 59
#define NODE_BLACKBODY 60
// --- Vector ------------------------------------------------------------------------------
#define NODE_MAPPING 70
#define NODE_BUMP 71
#define NODE_NORMAL_MAP 72
#define NODE_VECTOR_ROTATE 73
#define NODE_DISPLACEMENT 74        // compiled as math (svm_compile.c), no instruction of its own
#define NODE_VECTOR_DISPLACEMENT 75 // idem
#define NODE_NORMAL 76              // idem
#define NODE_CURVES 77              // RGB / Vector / Float Curves (one instruction, p0: CURVES_*)
#define NODE_RGB_CURVE 78           // node types sharing NODE_CURVES
#define NODE_VECTOR_CURVE 79
#define NODE_FLOAT_CURVE 80
// --- Inputs compiled to the implicit data or to constants (no instruction of their own) ----------
#define NODE_UV_MAP 85
#define NODE_ATTRIBUTE 86
#define NODE_COLOR_ATTRIBUTE 87
#define NODE_AMBIENT_OCCLUSION 88
#define NODE_BEVEL 89
#define NODE_WIREFRAME 90
// --- Hair and particles (kernel_hair.h; the info nodes read the primitive's record, KINFO_*) ---------------------------
#define NODE_BSDF_HAIR_PRINCIPLED 91 // p0 model (HAIR_MODEL_*), p1 parametrization (HAIR_PARAM_*), p2 Random is linked
#define NODE_BSDF_HAIR 92            // p0 component (0 reflection, 1 transmission)
#define NODE_BSDF_HAIR_TOON 93       // Tlender's stylized hair
#define NODE_HAIR_INFO 94
#define NODE_PARTICLE_INFO 95
#define NODE_VOLUME_FIRE 96          // Tlender's fire / smoke / explosion volumes (the same instruction): p0 the color
#define NODE_VOLUME_SMOKE 97         // preset (SmokeFireColor), the kind from the opcode
#define NODE_VOLUME_EXPLOSION 98
#define NODE_VOLUME_INFO 99          // Blender's Volume Info: compiled as SVM_OP_VOLUME_ATTR
// --- Simulation materials (kernel_svm_sim.h, svm_sim_nodes.inc): 110-139 (100-107 are SVM_OP_*) ---------------------
#define NODE_TEX_BRICK 110      // Blender's Brick Texture; p0 offset frequency, p1 squash frequency
#define NODE_TEX_MAGIC 111      // Blender's Magic Texture; p0 depth
#define NODE_TEX_WEAVE 112      // p0 pattern (WEAVE_*)
#define NODE_TEX_CRACKS 113     // also the instruction of Stones and Grains: p0 CELLS_*
#define NODE_TEX_STONES 114     // compiled into NODE_TEX_CRACKS
#define NODE_TEX_GRAINS 115     // idem
#define NODE_TEX_OCEAN 116      // p0 tileable
#define NODE_TEX_FIBERS 117     // p0 axis
#define NODE_ATTRIBUTE_READ 118 // instruction of the Attribute node reading a mesh attribute: p0 attribute id, p1 is color
// Surface shaders made of the other instructions (no instruction of their own)
#define NODE_SIM_WATER 120
#define NODE_SIM_WATER_TOON 121
#define NODE_SIM_FABRIC 122
#define NODE_SIM_SOIL 123
#define NODE_SIM_MUD 124
#define NODE_SIM_SAND 125
#define NODE_SIM_SNOW 126
#define NODE_SIM_ICE 127
#define NODE_SIM_ROCK 128
#define NODE_SIM_BARK 129
#define NODE_SIM_LEAF 130
#define NODE_SIM_LAVA 131
#define NODE_SIM_WETNESS 132     // a converter: Color, Roughness, Coat
#define NODE_SIM_FIRST NODE_SIM_WATER
#define NODE_SIM_LAST_SHADER NODE_SIM_LAVA
// --- World (world.h, kernel/kernel_world.h): 150-159 ---------------------------------------------------------------
#define NODE_OUTPUT_WORLD 150    // Surface, Volume
#define NODE_BACKGROUND 151      // compiled as NODE_EMISSION (the same inputs: Color, Strength)
#define NODE_TEX_ENVIRONMENT 152 // p0 interpolation, p1 projection (ENV_*), p2 IMAGE_FLAG_*, p3 image index
// Wave 0.08 (S1): the Subsurface Scattering BSDF (reserved 200..219)
#define NODE_SUBSURFACE_SCATTERING 200 // p0 method (SSS_*)
// --- Wet and burnt layers (svm_layer_nodes.inc, nodes_layer.inc; made of the other instructions): 230-239 ------------
#define NODE_LAYER_WET 230   // Wet Layer: a wet film (or a wet material) over a shader, by the "wetness" attribute
#define NODE_LAYER_BURN 231  // Burnt: char, embers (blackbody) and the burnt-away part (transparent) over a shader
#define NODE_BURN_INFO 232   // Burn Info: the burn attributes (char, ember, burn, temperature, wetness)
#define NODE_LAYER_FIRST NODE_LAYER_WET
#define NODE_LAYER_LAST_SHADER NODE_LAYER_BURN
// --- Sky and rain (sky.h, rain.h; kernel/kernel_sky.h, kernel/kernel_rain.h): 240-259 ------------------------------
#define NODE_TEX_SKY 240         // Blender's Sky Texture; also the instruction of Tlender's Sky (p0 SKYF_*, p1 image, p2 clouds image)
#define NODE_SKY 241             // Tlender's Sky (time of day, moon, stars, clouds): compiled into NODE_TEX_SKY
#define NODE_RAIN_INFO 242       // Rain Info (kernel/kernel_rain.h): Normal -> Wetness, Puddle, Flow, Streaks, Exposed, Normal
#define NODE_WET_SURFACE 243     // Wet Surface: Color, Roughness, Normal, Porosity, Wetness -> the wet material's inputs
#define NODE_TEX_RAIN_RIPPLES 244 // Rain Ripples texture: Vector, Normal, Scale, Intensity, Strength -> Height, Normal
#define SVM_OP_RAIN_WET 245      // [op, attr, 0, 0, 0]: the surface's closure made wet (the rain's "wet every material") and
                                 // snowed on ("snow on every material"); attr: the "tl_rain" attribute (moving objects)
// wave 0.10 (a high number of the range: the sky's session of the wave may add some after 245)
#define NODE_TEX_RAIN_DROPS 250  // Rain Drops texture: Normal, Size, Density, Running, Speed, Trail, Strength, Time, Factor
                                 // -> Height, Drops, Trail, Wetness, Normal
#define NIN_RAIN_INFO 1
#define NOUT_RAIN_INFO 6
#define NIN_WET_SURFACE 5
#define NOUT_WET_SURFACE 8
#define NIN_TEX_RAIN_RIPPLES 5
#define NOUT_TEX_RAIN_RIPPLES 2
#define NIN_TEX_RAIN_DROPS 9
#define NOUT_TEX_RAIN_DROPS 5
// wave 0.11 (high numbers of the range: the sky and storm session may add some after 245). The rain nodes above take
// the "tl_rain" attribute's id in p3 (a moving object's own wetness and drops).
#define NODE_WET_SHADER 255      // Wet: Shader, Factor, Porosity, Wet Roughness, Water Film, Drops, Drop Size -> Shader, Mask
                                 // p0 the "tl_rain" attribute, p1 by the rain. As a closure: [op, weight, mark, the 6 values]
#define NODE_SNOW_SHADER 256     // Snow: Shader, Factor, Color, Angle -> Shader, Mask (p0, p1 as the Wet's)
                                 // As a closure: [op, weight, mark, factor, color, angle]
#define SVM_OP_RAIN_MASK 257     // [op, attr, kind (0 wet, 1 snow), by the rain, 0, the node's values, out]: its Mask alone
#define SVM_OP_CLOSURE_MARK 258  // [op, 0, 0, 0, 0, out]: the closure's lobe count now (where a Wet / Snow node's start)
#define NIN_WET_SHADER 7
#define NOUT_WET_SHADER 2
#define NIN_SNOW_SHADER 4
#define NOUT_SNOW_SHADER 2
#define NODE_TYPE_COUNT 260
#define NIN_TEX_ENVIRONMENT 1    // Vector (unconnected: the position, the ray direction in the world)
#define NOUT_TEX_ENVIRONMENT 2   // Color, Alpha
#define ENV_EQUIRECTANGULAR 0    // Blender's projection values
#define ENV_MIRROR_BALL 1

// Instruction layouts of the simulation textures
#define NIN_TEX_BRICK 12   // Vector, Color1, Color2, Mortar, Scale, Mortar Size, Mortar Smooth, Bias, Brick Width,
                           // Row Height, Offset, Squash (Blender's node properties offset and squash as inputs)
#define NOUT_TEX_BRICK 2   // Color, Fac
#define NIN_TEX_MAGIC 3    // Vector, Scale, Distortion
#define NOUT_TEX_MAGIC 2   // Color, Fac
#define NIN_TEX_WEAVE 7    // Vector, Scale, Thread Width, Warp Color, Weft Color, Fibers, Randomness
#define NOUT_TEX_WEAVE 3   // Color, Height, Warp
#define NIN_TEX_CELLS 7    // Vector, Scale, Width, Distortion, Detail, Randomness, Roundness
#define NOUT_TEX_CELLS 5   // Gap, Height, Random, Distance, Random vector
#define NIN_TEX_OCEAN 7    // Vector, Scale, Direction, Spread, Choppiness, Waves, Time
#define NOUT_TEX_OCEAN 2   // Height, Foam
#define NIN_TEX_FIBERS 6   // Vector, Scale, Stretch, Detail, Roughness, Distortion
#define NOUT_TEX_FIBERS 2  // Fac, Ridges
#define NOUT_ATTRIBUTE_READ 4 // Color, Vector, Fac, Alpha (as the Attribute node)
#define WEAVE_PLAIN 0
#define WEAVE_TWILL 1
#define WEAVE_SATIN 2
#define WEAVE_BASKET 3
#define WEAVE_PATTERNS 4
#define CELLS_CRACKS 0
#define CELLS_STONES 1
#define CELLS_GRAINS 2
#define CELLS_MODES 3

// --- Instructions that are not nodes ---------------------------------------------------------
#define SVM_OP_END 0
#define SVM_OP_CONVERT 100     // [op, kind, 0, 0, 0, in, out]
#define SVM_OP_BUMP_OFFSET 101 // [op, axis, 0, 0, 0]: position offset for the next nodes (0 = none, 1 = +T, 2 = +B)
#define SVM_OP_NORMAL 102      // [op, 0, 0, 0, 0, out]: shading normal (unconnected Normal inputs)
#define SVM_OP_GENERATED 103   // [op, 0, 0, 0, 0, out]: generated coordinates (unconnected texture Vectors)
#define SVM_OP_MIX_WEIGHT 104  // [op, 0, 0, 0, 0, weight, fac, out_a, out_b]: Mix Shader branch weights
#define SVM_OP_TANGENT 105     // [op, 0, 0, 0, 0, out]: surface tangent (unconnected Tangent inputs)
#define SVM_OP_UV 106        // [op, 0, 0, 0, 0, out]: UV map coordinates (unconnected Image Texture Vectors)
#define SVM_OP_SET_NORMAL 107 // [op, 0, 0, 0, 0, in]: the shading normal from here on (displacement as bump)
#define SVM_OP_VOLUME_ATTR 108 // [op, id, 0, 0, 0, out color, out fac]: a volume object's grid (KGRID_ID_* or a name's id) at P

#define SVM_CONVERT_COLOR_TO_FLOAT 0 // luminance
#define SVM_CONVERT_VECTOR_TO_FLOAT 1 // average
#define SVM_CONVERT_FLOAT_TO_VECTOR 2 // splat

// Inputs and outputs of each node instruction (order of the sockets in nodes.c).
#define NIN_OUTPUT_MATERIAL 4      // Surface, Volume, Displacement (as bump), Thickness (EEVEE: not used)
#define NIN_BSDF_PRINCIPLED 30
#define NIN_BSDF_DIFFUSE 3         // Color, Roughness, Normal
#define NIN_BSDF_GLOSSY 6          // Color, Roughness, Anisotropy, Rotation, Normal, Tangent
#define NIN_BSDF_GLASS 4           // Color, Roughness, IOR, Normal
#define NIN_BSDF_TRANSLUCENT 2     // Color, Normal
#define NIN_BSDF_TRANSPARENT 1     // Color
#define NIN_EMISSION 2             // Color, Strength
#define NIN_MIX_SHADER 3           // Fac, Shader, Shader
#define NIN_ADD_SHADER 2
#define NIN_PRINCIPLED_VOLUME 9
#define NIN_VOLUME_ABSORPTION 2    // Color, Density
#define NIN_VOLUME_SCATTER 3       // Color, Density, Anisotropy
#define NIN_BSDF_SHEEN 3           // Color, Roughness, Normal
#define NOUT_TEX_COORD 7           // Generated, Normal, UV, Object, Camera, Window, Reflection
#define NOUT_GEOMETRY 6            // Position, Normal, Tangent, True Normal, Incoming, Backfacing
#define NOUT_OBJECT_INFO 4         // Location, Color, Object Index, Random
#define NIN_FRESNEL 2              // IOR, Normal
#define NIN_LAYER_WEIGHT 2         // Blend, Normal
#define NOUT_LAYER_WEIGHT 2        // Fresnel, Facing
#define NOUT_LIGHT_PATH 13
#define NIN_TEX_NOISE 9            // Vector, W, Scale, Detail, Roughness, Lacunarity, Offset, Gain, Distortion; p0 dims, p1 NOISE_*, p2 normalize
#define NOUT_TEX_NOISE 2           // Fac, Color
#define NIN_TEX_VORONOI 3          // Vector, Scale, Randomness
#define NOUT_TEX_VORONOI 3         // Distance, Color, Position
#define NIN_TEX_CHECKER 4          // Vector, Color1, Color2, Scale
#define NOUT_TEX_CHECKER 2         // Color, Fac
#define NIN_TEX_GRADIENT 1
#define NOUT_TEX_GRADIENT 2        // Color, Fac
#define NIN_TEX_WAVE 7             // Vector, Scale, Distortion, Detail, Detail Scale, Detail Roughness, Phase Offset
#define NOUT_TEX_WAVE 2            // Color, Fac
#define NIN_TEX_WHITE_NOISE 1
#define NOUT_TEX_WHITE_NOISE 2     // Value, Color
#define NIN_TEX_IMAGE 1            // Vector (props: interpolation, extension, IMAGE_FLAG_*, image index)
#define NOUT_TEX_IMAGE 2           // Color, Alpha
#define NIN_MIX 7                  // Factor, A/B float, A/B vector, A/B color
#define NOUT_MIX 3                 // Result float, vector, color
#define NIN_INVERT 2               // Fac, Color
#define NIN_HUE_SAT 5              // Hue, Saturation, Value, Fac, Color
#define NIN_BRIGHT_CONTRAST 3      // Color, Bright, Contrast
#define NIN_GAMMA 2                // Color, Gamma
#define NOUT_COLOR_RAMP 2          // Color, Alpha
#define NIN_MATH 3
#define NIN_VECTOR_MATH 4          // Vector x3, Scale
#define NOUT_VECTOR_MATH 2         // Vector, Value
#define NIN_MAP_RANGE 6            // Value, From Min, From Max, To Min, To Max, Steps
#define NIN_CLAMP 3                // Value, Min, Max
#define NOUT_SEPARATE 3
#define NIN_COMBINE 3
#define NIN_MAPPING 4              // Vector, Location, Rotation, Scale
#define NIN_BUMP 4                 // Strength, Distance, Height, Normal (+ heights at the two offsets)
#define NIN_NORMAL_MAP 2           // Strength, Color
#define NIN_VECTOR_ROTATE 5        // Vector, Center, Axis, Angle, Rotation
#define NIN_CURVES 2               // Fac, Color / Vector / Value (+ the table after the output)
#define NIN_BSDF_HAIR_PRINCIPLED 17 // Color, Melanin, Melanin Redness, Tint, Absorption, Aspect Ratio, Roughness, Radial
                                    // Roughness, Coat, IOR, Offset, Random Color, Random Roughness, Random, R, TT, TRT
#define NIN_BSDF_HAIR 5             // Color, Offset, RoughnessU, RoughnessV, Tangent
#define NIN_BSDF_HAIR_TOON 16       // Root Color, Tip Color, Gradient, Bands, Softness, Normal, Tangent, Highlight (color,
                                    // shift, width), Secondary (color, shift, width), Rim (color, strength, width)
#define NOUT_HAIR_INFO 8            // Is Strand, Intercept, Length, Thickness, Tangent Normal, Random, Root UV, Surface Color
#define NOUT_PARTICLE_INFO 9        // Index, Random, Age, Lifetime, Location, Size, Velocity, Angular Velocity, Color (Tlender)
#define NIN_VOLUME_FIRE 12          // Density, Smoke Color, Anisotropy, Absorption Color, Flame Strength, Flame Tint,
                                    // Temperature Min, Temperature Max (K), Smoke Glow, Smoke Shadow, Smoke Ambient,
                                    // Smoke From Heat
#define NIN_SUBSURFACE_SCATTERING 7 // Color, Scale, Radius, IOR, Roughness, Anisotropy, Normal
#ifndef SSS_RANDOM_WALK // Subsurface methods (also in kernel_types.h, for the closures): the prop index, 0 = Blender's default
#define SSS_RANDOM_WALK 0
#define SSS_RANDOM_WALK_SKIN 1
#define SSS_BURLEY 2
#endif


// Principled Hair BSDF (Blender's enum values)
#define HAIR_MODEL_CHIANG 0
#define HAIR_MODEL_HUANG 1
#define HAIR_PARAM_COLOR 0      // Direct Coloring
#define HAIR_PARAM_MELANIN 1    // Melanin Concentration
#define HAIR_PARAM_ABSORPTION 2 // Absorption Coefficient

// Normal Map / Displacement spaces (Blender's SHD_SPACE_*)
#define NMAP_TANGENT 0
#define NMAP_OBJECT 1
#define NMAP_WORLD 2
#define NMAP_BLENDER_OBJECT 3
#define NMAP_BLENDER_WORLD 4

// Vector Rotate types (Blender's NODE_VECTOR_ROTATE_TYPE_*)
#define VROT_AXIS 0
#define VROT_X 1
#define VROT_Y 2
#define VROT_Z 3
#define VROT_EULER_XYZ 4

// Curves: p0 the kind, p1 table size, p2 extrapolate (0/1); the table (p1 rows of 3 floats over [min, max]) follows
// the output, preceded by min and max. A row is the curves of R, G, B (RGB), X, Y, Z (Vector) or the value (Float).
#define CURVES_RGB 0
#define CURVES_VECTOR 1
#define CURVES_FLOAT 2
#define CURVES_TABLE_SIZE 128
#define CURVES_MAX_POINTS 16       // control points of all the curves of a node (Node.ramp)

// Property values (Blender's enum values where Blender has them).
#define DIST_MULTI_GGX 0
#define DIST_GGX 1

#define MIX_DATA_FLOAT 0
#define MIX_DATA_VECTOR 1
#define MIX_DATA_COLOR 2

#define BLEND_MIX 0
#define BLEND_ADD 1
#define BLEND_MUL 2
#define BLEND_SUB 3
#define BLEND_SCREEN 4
#define BLEND_DIV 5
#define BLEND_DIFF 6
#define BLEND_DARK 7
#define BLEND_LIGHT 8
#define BLEND_OVERLAY 9
#define BLEND_DODGE 10
#define BLEND_BURN 11
#define BLEND_HUE 12
#define BLEND_SAT 13
#define BLEND_VAL 14
#define BLEND_COLOR 15
#define BLEND_SOFT 16
#define BLEND_LINEAR 17
#define BLEND_EXCLUSION 18
#define BLEND_COUNT 19

#define MATH_ADD 0
#define MATH_SUBTRACT 1
#define MATH_MULTIPLY 2
#define MATH_DIVIDE 3
#define MATH_SINE 4
#define MATH_COSINE 5
#define MATH_TANGENT 6
#define MATH_ARCSINE 7
#define MATH_ARCCOSINE 8
#define MATH_ARCTANGENT 9
#define MATH_POWER 10
#define MATH_LOGARITHM 11
#define MATH_MINIMUM 12
#define MATH_MAXIMUM 13
#define MATH_ROUND 14
#define MATH_LESS_THAN 15
#define MATH_GREATER_THAN 16
#define MATH_MODULO 17
#define MATH_ABSOLUTE 18
#define MATH_ARCTAN2 19
#define MATH_FLOOR 20
#define MATH_CEIL 21
#define MATH_FRACTION 22
#define MATH_SQRT 23
#define MATH_INV_SQRT 24
#define MATH_SIGN 25
#define MATH_EXPONENT 26
#define MATH_RADIANS 27
#define MATH_DEGREES 28
#define MATH_SINH 29
#define MATH_COSH 30
#define MATH_TANH 31
#define MATH_TRUNC 32
#define MATH_SNAP 33
#define MATH_WRAP 34
#define MATH_COMPARE 35
#define MATH_MULTIPLY_ADD 36
#define MATH_PINGPONG 37
#define MATH_SMOOTH_MIN 38
#define MATH_SMOOTH_MAX 39
#define MATH_FLOORED_MODULO 40
#define MATH_COUNT 41

#define VMATH_ADD 0
#define VMATH_SUBTRACT 1
#define VMATH_MULTIPLY 2
#define VMATH_DIVIDE 3
#define VMATH_CROSS 4
#define VMATH_PROJECT 5
#define VMATH_REFLECT 6
#define VMATH_DOT 7
#define VMATH_DISTANCE 8
#define VMATH_LENGTH 9
#define VMATH_SCALE 10
#define VMATH_NORMALIZE 11
#define VMATH_SNAP 12
#define VMATH_FLOOR 13
#define VMATH_CEIL 14
#define VMATH_MODULO 15
#define VMATH_FRACTION 16
#define VMATH_ABSOLUTE 17
#define VMATH_MINIMUM 18
#define VMATH_MAXIMUM 19
#define VMATH_WRAP 20
#define VMATH_SINE 21
#define VMATH_COSINE 22
#define VMATH_TANGENT 23
#define VMATH_REFRACT 24
#define VMATH_FACEFORWARD 25
#define VMATH_MULTIPLY_ADD 26
#define VMATH_COUNT 27

#define GRADIENT_LINEAR 0
#define GRADIENT_QUADRATIC 1
#define GRADIENT_EASING 2
#define GRADIENT_DIAGONAL 3
#define GRADIENT_SPHERICAL 4
#define GRADIENT_QUADRATIC_SPHERE 5
#define GRADIENT_RADIAL 6

#define WAVE_BANDS 0
#define WAVE_RINGS 1
#define WAVE_PROFILE_SIN 0
#define WAVE_PROFILE_SAW 1
#define WAVE_PROFILE_TRI 2

// Noise Texture types (Blender / Cycles NodeNoiseType values) and dimensions (prop index = dims - 1)
#define NOISE_MULTIFRACTAL 0
#define NOISE_FBM 1
#define NOISE_HYBRID_MULTIFRACTAL 2
#define NOISE_RIDGED_MULTIFRACTAL 3
#define NOISE_HETERO_TERRAIN 4

#define VORONOI_F1 0
#define VORONOI_F2 1
#define VORONOI_EUCLIDEAN 0
#define VORONOI_MANHATTAN 1
#define VORONOI_CHEBYCHEV 2

#define RAMP_LINEAR 0
#define RAMP_EASE 1
#define RAMP_CONSTANT 2
#define RAMP_BSPLINE 3   // through 4 stops, as Blender's BKE_colorband_evaluate
#define RAMP_CARDINAL 4
#define RAMP_MAX_STOPS 16

#define MAP_RANGE_LINEAR 0
#define MAP_RANGE_STEPPED 1
#define MAP_RANGE_SMOOTHSTEP 2
#define MAP_RANGE_SMOOTHERSTEP 3

#define CLAMP_MINMAX 0
#define CLAMP_RANGE 1

#define COLOR_MODE_RGB 0
#define COLOR_MODE_HSV 1
#define COLOR_MODE_HSL 2

#define MAPPING_POINT 0
#define MAPPING_TEXTURE 1
#define MAPPING_VECTOR 2
#define MAPPING_NORMAL 3

// Image Texture (Blender's enum values).
#define IMAGE_INTERP_LINEAR 0
#define IMAGE_INTERP_CLOSEST 1
#define IMAGE_EXT_REPEAT 0
#define IMAGE_EXT_EXTEND 1
#define IMAGE_EXT_CLIP 2
#define IMAGE_EXT_MIRROR 3
#define IMAGE_COLORSPACE_SRGB 0
#define IMAGE_COLORSPACE_NONCOLOR 1
#define IMAGE_ALPHA_STRAIGHT 0   // the image's alpha mode (Blender's Image.alpha_mode)
#define IMAGE_ALPHA_PREMUL 1
#define IMAGE_ALPHA_CHANNEL_PACKED 2
#define IMAGE_ALPHA_NONE 3
#define IMAGE_FLAG_SRGB 1        // texels are sRGB-encoded: decode after interpolation (as Cycles)
#define IMAGE_FLAG_UNASSOCIATE 2 // the Alpha output is used: Color is divided by alpha

// Node groups (specialization constant KERNEL_NODE_GROUPS, constant_id 5): the pipeline of a scene carries only the
// interpreter cases of the groups its programs use (svm_compile.c ORs svm_op_group of every instruction), so the
// driver compiles a fraction of the interpreter. The ops without a group are always there. A case whose group is off
// stops the program (pc = -1): a missing bit shows as a wrong image, never as a hang.
#define SVMG_PRINCIPLED 0x0001u // Principled BSDF
#define SVMG_BSDF 0x0002u       // Diffuse, Glossy, Glass, Translucent, Transparent, Sheen
#define SVMG_HAIR 0x0004u       // the hair BSDFs
#define SVMG_VOLUME 0x0008u     // the volume closures and the volume objects' grids
#define SVMG_INPUT 0x0010u      // Geometry, Object / Hair / Particle Info, Fresnel, Layer Weight, Light Path
#define SVMG_NOISE 0x0020u      // Noise and Wave textures
#define SVMG_VORONOI 0x0040u    // Voronoi
#define SVMG_TEX 0x0080u        // Checker, Gradient, White Noise
#define SVMG_IMAGE 0x0100u      // Image Texture
#define SVMG_COLOR 0x0200u      // Mix, Invert, Hue/Saturation, Bright/Contrast, Gamma, Separate/Combine Color, RGB to BW, Blackbody
#define SVMG_RAMP 0x0400u       // Color Ramp
#define SVMG_MATH 0x0800u       // Math, Vector Math, Map Range, Clamp, Separate/Combine XYZ
#define SVMG_VECTOR 0x1000u     // Mapping, Bump, Normal Map, Vector Rotate
#define SVMG_CURVES 0x2000u     // RGB / Vector / Float Curves
#define SVMG_SIMTEX 0x4000u     // the textures of the simulation materials (Brick, Magic, Weave, Cracks, Ocean, Fibers)
#define SVMG_ATTR 0x8000u       // mesh attributes and the rain (kernel_rain.h)
// Shading sites (render.c from the materials, not from the instructions): the interpreter is inlined at every place
// that shades a material, and these two places only need it when some program can reach them. Off, they shade the
// materials without a program only (shader_eval_surface_site); a scene with a plain opaque material compiles one copy
// of the interpreter instead of three.
#define SVMG_SHADOW 0x10000u // shadow rays shade a material with a surface program (KMAT_FLAG_SHADOW_PASS: alpha, no surface)
#define SVMG_EMIT 0x20000u   // a material with a surface program is emissive (its triangles are lights: light_triangle_emission)
#define SVMG_SKY 0x40000u    // the Sky Texture and Tlender's Sky (kernel_sky.h, kernel_storm.h: an image node's scene doesn't pay for it)
#define SVMG_OPS 0x4FFFFu    // the groups of the instructions
#define SVMG_ALL 0x7FFFFu

#ifndef KERNEL_GLSL
// The group of an instruction (0: always compiled in). Must match the SVM_GROUP of its case in kernel_svm.h.
static inline unsigned svm_op_group(int op) {
    switch (op) {
    case NODE_BSDF_PRINCIPLED: return SVMG_PRINCIPLED;
    case NODE_BSDF_DIFFUSE: case NODE_BSDF_GLOSSY: case NODE_BSDF_GLASS: case NODE_BSDF_TRANSLUCENT:
    case NODE_BSDF_TRANSPARENT: case NODE_BSDF_SHEEN: case NODE_SUBSURFACE_SCATTERING: return SVMG_BSDF;
    case NODE_BSDF_HAIR_PRINCIPLED: case NODE_BSDF_HAIR: case NODE_BSDF_HAIR_TOON: return SVMG_HAIR;
    case NODE_PRINCIPLED_VOLUME: case NODE_VOLUME_ABSORPTION: case NODE_VOLUME_SCATTER: case NODE_VOLUME_FIRE:
    case NODE_VOLUME_SMOKE: case NODE_VOLUME_EXPLOSION: case SVM_OP_VOLUME_ATTR: return SVMG_VOLUME;
    case NODE_GEOMETRY: case NODE_OBJECT_INFO: case NODE_HAIR_INFO: case NODE_PARTICLE_INFO: case NODE_FRESNEL:
    case NODE_LAYER_WEIGHT: case NODE_LIGHT_PATH: return SVMG_INPUT;
    case NODE_TEX_NOISE: case NODE_TEX_WAVE: return SVMG_NOISE;
    case NODE_TEX_VORONOI: return SVMG_VORONOI;
    case NODE_TEX_CHECKER: case NODE_TEX_GRADIENT: case NODE_TEX_WHITE_NOISE: return SVMG_TEX;
    case NODE_TEX_IMAGE: case NODE_TEX_ENVIRONMENT: return SVMG_IMAGE;
    case NODE_TEX_SKY: return SVMG_SKY;
    case NODE_MIX: case NODE_INVERT: case NODE_HUE_SAT: case NODE_BRIGHT_CONTRAST: case NODE_GAMMA:
    case NODE_SEPARATE_COLOR: case NODE_COMBINE_COLOR: case NODE_RGB_TO_BW: case NODE_BLACKBODY: return SVMG_COLOR;
    case NODE_COLOR_RAMP: return SVMG_RAMP;
    case NODE_MATH: case NODE_VECTOR_MATH: case NODE_MAP_RANGE: case NODE_CLAMP: case NODE_SEPARATE_XYZ:
    case NODE_COMBINE_XYZ: return SVMG_MATH;
    case NODE_MAPPING: case NODE_BUMP: case NODE_NORMAL_MAP: case NODE_VECTOR_ROTATE: return SVMG_VECTOR;
    case NODE_CURVES: return SVMG_CURVES;
    case NODE_TEX_BRICK: case NODE_TEX_MAGIC: case NODE_TEX_WEAVE: case NODE_TEX_CRACKS: case NODE_TEX_OCEAN:
    case NODE_TEX_FIBERS: return SVMG_SIMTEX;
    case NODE_ATTRIBUTE_READ: case NODE_RAIN_INFO: case NODE_WET_SURFACE: case NODE_TEX_RAIN_RIPPLES: case SVM_OP_RAIN_WET:
    case NODE_TEX_RAIN_DROPS: case NODE_WET_SHADER: case NODE_SNOW_SHADER: case SVM_OP_RAIN_MASK: case SVM_OP_CLOSURE_MARK:
        return SVMG_ATTR; // mesh attributes and the rain's maps (data of the scene at the point)
    default: return 0;
    }
}
#endif

#endif
