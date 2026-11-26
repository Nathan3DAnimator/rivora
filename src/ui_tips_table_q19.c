// (Q19, notas-dev/importar-tlender.md) The descriptions of what Q19 shows in the editor: Window > Import Report (the
// conversion report of the last import) and its controls (a file of its own so the branches don't collide).
#include "ui_tips.h"

#define T(label, en, pt) { label, { en, pt } }

const UiTip g_ui_tips_q19[] = {
    T("Import Report", "The last import's conversion report (.tlend, OBJ, glTF, FBX): what came in the same, what was converted into an Engine equivalent, what was baked (on the GPU into the material's textures, or into curves) and what was lost, each with its reason. Opens by itself when an import loses something; also written to the project's Saved/ImportReports folder.", "O relatório de conversão da última importação (.tlend, OBJ, glTF, FBX): o que entrou igual, o que virou um equivalente da Engine, o que foi assado (na GPU nas texturas do material, ou em curvas) e o que se perdeu, cada um com o motivo. Abre sozinho quando uma importação perde algo; também gravado na pasta Saved/ImportReports do projeto."),
    T("Show Report File", "Shows the report file (the same list, in the language of the editor) selected in the Explorer, in the project's Saved/ImportReports folder.", "Mostra o arquivo do relatório (a mesma lista, no idioma do editor) selecionado no Explorer, na pasta Saved/ImportReports do projeto."),
    T("imprep_search", "Filters the rows by text: the item (a modifier, node, image, driver), the object or material it belongs to, or the reason.", "Filtra as linhas pelo texto: o item (um modificador, nó, imagem, driver), o objeto ou material a que pertence, ou o motivo."),
    T("imprep_k0", "Shows what came in the same: an Engine node, modifier or rig part that is the Tlender's.", "Mostra o que entrou igual: um nó, modificador ou parte do rig da Engine que é a do Tlender."),
    T("imprep_k1", "Shows what was converted into an Engine equivalent (a Metallic BSDF into a Principled, a Mirror applied to the mesh, a Cloth modifier into the Cloth component...).", "Mostra o que virou um equivalente da Engine (um Metallic BSDF num Principled, um Mirror aplicado na malha, um modificador Cloth no componente de Tecido...)."),
    T("imprep_k2", "Shows what was baked: nodes the Engine evaluates only in the GPU bake of the material's textures, images the Tlender baked, drivers baked into the animations' curves.", "Mostra o que foi assado: nós que a Engine avalia só no bake da GPU das texturas do material, imagens que o Tlender assou, drivers assados nas curvas das animações."),
    T("imprep_k3", "Shows what was lost and why (a modifier the Engine can't apply, a volume shader, a missing image), with what the Tlender's export needs to bring it.", "Mostra o que se perdeu e por quê (um modificador que a Engine não aplica, um shader de volume, uma imagem que falta), com o que a exportação do Tlender precisa para trazer."),
};
const int g_ui_tip_q19_count = (int)(sizeof g_ui_tips_q19 / sizeof g_ui_tips_q19[0]);
