#version 460
// (2026-11-25, item 17) a selected actor's hair in the selection's mask (hair.vert's ribbons, groom.c groom_draw_mask): its
// actor's id from the push's pad (mask.frag's hash), so the hair joins the actor's one outline
layout(location = 0) out float o_mask;
layout(push_constant) uniform Push { uvec2 frame; uvec2 vertices; uvec2 instances; uint pass; uint pad; } pc;

void main() {
    uint h = pc.pad * 2654435761u;
    o_mask = float(1u + (h >> 24) % 254u) / 255.0;
}
