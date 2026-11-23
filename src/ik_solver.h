// Copied from the Tlender (src/ik_solver.h).
// The IK solver of Blender's "Standard" IK (the legacy iTaSC-less solver of the IK constraint): a Jacobian solver with
// selectively damped least squares, one segment per bone with the degrees of freedom its IK locks leave (spherical,
// swing, elbow, revolute) plus a translation along Y when the bone may stretch, IK limits and stiffness, position and
// rotation goals, and the pole target as a rotation of the whole chain before solving. Written from Blender's
// documented behavior and checked against Blender's own poses (tools/ik_ref.py, tests/test_ik.c). No UI, no Scene.
//
// Everything is in the tree's space: the origin at the root bone's head, the axes of its parent's pose (rotation only).
#pragma once
#include <stdbool.h>

enum { IKS_X = 1, IKS_Y = 2, IKS_Z = 4, IKS_TRANS_Y = 8 };

typedef struct IkBone {
    int parent;              // index of the parent bone in the array (parents first), -1 for the root
    unsigned dof;            // IKS_X/Y/Z: free rotation axes (not locked); IKS_TRANS_Y: the bone stretches
    unsigned limit;          // IKS_X/Y/Z: rotation limits on
    double min[3], max[3];   // the limits, radians
    double stiffness[3];     // 0..1 per rotation axis
    double stretch_stiffness; // 1 - ik_stretch^2 (Blender squares the bone's IK Stretch)
    double start[3];         // the head from the parent's tail, in the parent's frame (the root: 0)
    double rest[3][3];       // rest rotation relative to the parent's (Blender's bone_mat), rows
    double basis[3][3];      // the pose rotation relative to rest: rest^T * parent^T * pose
    double length;           // with the pose's Y scale
    // results
    double basis_change[3][3]; // the solved basis = input basis * basis_change
    double trans_change[3];    // the translation added along the bone (stretch), in its frame
} IkBone;

typedef struct IkGoal {
    int tip;                 // the bone whose tail reaches the goal
    bool use_pos, use_rot;
    double pos[3];
    double rot[3][3];
    double weight, orient_weight;
    bool use_pole;           // only with use_pos
    double pole[3];
    double pole_angle;       // radians
} IkGoal;

#define IK_SOLVER_MAX_BONES 255
#define IK_SOLVER_MAX_GOALS 16

// Solves the tree in place (basis_change, trans_change). False when there is nothing to solve.
bool ik_solve(IkBone* bones, int n, const IkGoal* goals, int ngoals, int max_iterations);
