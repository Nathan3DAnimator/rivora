#version 460
// The selected meshes' silhouettes, for the outline in composite.frag: an id per object (1..255, from its slot), the
// nearest one where they overlap (the pass's own depth), so each selected object gets its own outline
// The game's highlight (interaction.h, the push's pad 1): the top id (255), which composite.frag outlines in its own color
layout(location = 2) flat in uint i_instance;
layout(location = 0) out float o_mask;
layout(push_constant) uniform Push { uvec2 frame; uvec2 vertices; uvec2 instances; uint pass; uint pad; } pc;

void main() {
    uint h = i_instance * 2654435761u;
    o_mask = pc.pad == 1u ? 1.0 : float(1u + (h >> 24) % 254u) / 255.0;
}
