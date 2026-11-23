// Gaussian Splatting (A9.5d; splat.c, notas-dev/render.md "Gaussian Splatting"): scenes scanned as millions of 3D
// Gaussians (3DGS .ply, .splat). Each Splat actor's Gaussians in one buffer (64 bytes each: its center and opacity, its
// covariance in the actor's space, its color); every frame sorted back to front by a counting sort on a 16-bit depth key
// (splat_sort.comp) and drawn as screen quads (splat.vert / splat.frag), each its Gaussian projected (EWA, Zwicker et
// al. 2001, as Kerbl et al. 2023), blended over the scene.
#ifndef SPLAT_GLSL
#define SPLAT_GLSL

#define SPLAT_BINS 65536u
#define SPLAT_ACTOR_SHIFT 26u // (a sorted entry: the actor << 26 | its Gaussian)

struct SplatGpu {
    vec4 pos_op;     // center (actor space, cm), opacity
    vec4 cov0;       // covariance xx, xy, xz, yy (cm^2)
    vec4 cov1;       // yz, zz, the color (rgba8 as the float's bits), -
    vec4 extra;      // (spare: the next SH bands one day)
};
// The actors' table (splat.c SplatActorGpu): its rows (actor -> world), its Gaussians' first and count, its look
struct SplatActor {
    vec4 rows[3];
    uint first, count, mode, pad;   // mode: 0 lit by the scene (the color an albedo), 1 emissive (the color times nits)
    vec4 look;                       // x: brightness (nits, emissive), y: the opacity's scale, zw: -
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer SplatBuf { SplatGpu s[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer SplatActorBuf { SplatActor a[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) buffer SplatUint { uint v[]; };
// The frame's working buffers (splat.c SplatFrameGpu, at pc.instances): the data, the actors, the keys, the values,
// the sorted values, the histogram (SPLAT_BINS) then the counters (visible, 4 draw arguments)
struct SplatFrame {
    uvec2 data, actors, keys, vals, sorted, hist;
    uint nactors, total, pad0, pad1;
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer SplatFrameBuf { SplatFrame f; };

vec3 splat_world(SplatActor A, vec3 p) { return vec3(dot(A.rows[0], vec4(p, 1.0)), dot(A.rows[1], vec4(p, 1.0)), dot(A.rows[2], vec4(p, 1.0))); }
#endif
