// (Q18, notas-dev/udim.md) The descriptions of what Q18 shows in the editor: the Image Texture's UDIM button and the
// Content Browser's UDIM import (a file of its own so the branches don't collide).
#include "ui_tips.h"

#define T(label, en, pt) { label, { en, pt } }

const UiTip g_ui_tips_q18[] = {
    T("Use as UDIM", "This image is one tile of a UDIM set (<name>.1001.png with others beside it): read the whole set, each tile of the UV from its own image (1001 at 0..1, 1002 to its right, 1011 above). The material is baked per tile; each tile streams on its own.", "Esta imagem é um tile de um conjunto UDIM (<nome>.1001.png com outros ao lado): lê o conjunto inteiro, cada tile do UV da sua própria imagem (1001 em 0..1, 1002 à direita, 1011 acima). O material é assado por tile; cada tile faz streaming sozinho."),
    T("Import UDIM Folder...", "A folder of UDIM tiles (<name>.1001.png, <name>.1002.png...) into one texture asset (.rudim) here: each tile compressed to BC7 (BC5 for normal maps) with its mips, read straight from the disk. Dropping one tile from Explorer does the same.", "Uma pasta de tiles UDIM (<nome>.1001.png, <nome>.1002.png...) vira um asset de textura (.rudim) aqui: cada tile comprimido em BC7 (BC5 nos normal maps) com seus mips, lido direto do disco. Soltar um tile do Explorer faz o mesmo."),
};
const int g_ui_tip_q18_count = (int)(sizeof g_ui_tips_q18 / sizeof g_ui_tips_q18[0]);
