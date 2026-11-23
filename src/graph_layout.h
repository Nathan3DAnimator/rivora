// (2026-11-23) Organize Nodes (graph_layout.c): the layered layout every node editor shares
#pragma once
#include <stdbool.h>

typedef struct GlNode { float x, y, w, h; } GlNode; // a node's box in the graph's units (laid out in place)
typedef struct GlEdge { int from, to; } GlEdge;    // a link: from the node whose output feeds the node `to` (a tree: parent -> child)
// a flow toward its ends on the right (materials, Blueprints...); a tree down from its root (Behavior Trees); a tree to
// the right from its root (dialogues)
enum { GL_FLOW_RIGHT = 0, GL_TREE_DOWN = 1, GL_TREE_RIGHT = 2 };
// nodes laid out (only: the ones to move, NULL all); false when there was nothing to do
bool graph_layout(GlNode* nodes, int count, const GlEdge* edges, int nedges, const bool* only, int mode, float gap_col, float gap_row);
