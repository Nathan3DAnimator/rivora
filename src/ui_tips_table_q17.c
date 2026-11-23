// (Q17, notas-dev/tessellation.md) The descriptions of what Q17 shows in the editor: the material's Nanite section
// (Enable Tessellation, Displacement Scaling) and the Debug Viewer's row (a file of its own so the branches don't collide).
#include "ui_tips.h"

#define T(label, en, pt) { label, { en, pt } }

const UiTip g_ui_tips_q17[] = {
    T("Enable Tessellation", "Geometry Tessellation: the material's Displacement moves the surface for real. Each cluster's triangles are subdivided on the GPU by their size on screen (Epic: an edge every 2 px) and pushed along the normal by the height: the silhouette, the shadows and the contact change, with no crack between neighbors. Needs a mesh with a cluster LOD (16 k triangles or more); smaller ones move their vertices only. Low: the vertices displaced, nothing subdivided.", "Geometry Tessellation: o Deslocamento do material move a superfície de verdade. Os triângulos de cada cluster são subdivididos na GPU pelo tamanho na tela (Épico: uma aresta a cada 2 px) e empurrados ao longo da normal pela altura: a silhueta, as sombras e o contato mudam, sem rachadura entre vizinhos. Precisa de uma malha com LOD de clusters (16 mil triângulos ou mais); as menores só movem os vértices. Baixo: os vértices deslocados, nada subdividido."),
    T("Displacement Magnitude", "How far the height's whole range (0 to 1) moves the surface, in cm of the mesh: the actor's scale scales it. Negative digs in where it would rise.", "Quanto a faixa inteira da altura (0 a 1) move a superfície, em cm da malha: a escala do ator também multiplica. Negativo afunda onde subiria."),
    T("Displacement Center", "The height that stays in place (Blender's Midlevel): 0.5 sinks the dark half and raises the light one; 0 only raises, 1 only sinks.", "A altura que fica no lugar: 0,5 afunda a metade escura e levanta a clara; 0 só levanta, 1 só afunda."),
    T("Tessellation", "Geometry Tessellation's cost this frame: the dicing (tess.comp, GPU ms) and the micro triangles drawn from the base ones. Past the preset's budget the micro edges grow (the scale).", "O custo da Geometry Tessellation neste quadro: a subdivisão (tess.comp, ms de GPU) e os microtriângulos desenhados a partir dos triângulos base. Passando do orçamento do preset as microarestas crescem (a escala)."),
};
const int g_ui_tip_q17_count = (int)(sizeof g_ui_tips_q17 / sizeof g_ui_tips_q17[0]);
