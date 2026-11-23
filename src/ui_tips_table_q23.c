// (Q23, notas-dev/auditoria-artista.md) The descriptions of what Q23 adds to the editor: the Texture viewer's mips and a
// .hdr's exposure, the Montage editor's Remove Section (a file of its own so the branches don't collide).
#include "ui_tips.h"

#define T(label, en, pt) { label, { en, pt } }

const UiTip g_ui_tips_q23[] = {
    T("Mip Level", "Shows the texture at a smaller mip: the image box-filtered to that size, as the GPU samples it far away (blurrier, thin lines fading). Its size beside the number.", "Mostra a textura num mip menor: a imagem filtrada para esse tamanho, como a GPU a lê de longe (mais borrada, linhas finas sumindo). O tamanho ao lado do número."),
    T("HDR Exposure", "Brightens or darkens a .hdr image to see its lights and shadows (stops: +1 doubles). Only the view: a material reads the image at 0 EV.", "Clareia ou escurece uma imagem .hdr para ver as luzes e as sombras dela (stops: +1 dobra). Só a visualização: o material lê a imagem em 0 EV."),
    T("Remove Section", "Removes this section from the montage (the first one stays).", "Remove esta seção do montage (a primeira fica)."),
};
const int g_ui_tip_q23_count = (int)(sizeof g_ui_tips_q23 / sizeof g_ui_tips_q23[0]);
