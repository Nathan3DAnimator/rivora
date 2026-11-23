// (Q20, notas-dev/gi-estavel.md) The descriptions of what Q20 shows in the editor: Project Settings > Rendering's
// temporal stability options (a file of its own so the branches don't collide).
#include "ui_tips.h"

#define T(label, en, pt) { label, { en, pt } }

const UiTip g_ui_tips_q20[] = {
    T("Reject Uncovered History", "Where a moving actor uncovers the scene behind it (a character walking past a wall), the temporal upscaler (TSR) drops the history of what left and starts from this frame: no trail behind it. Found by last frame's depth (copied while an actor moves, ~0.03 ms). Console: r.TSR.Disocclusion.", "Onde um ator em movimento descobre a cena atrás dele (um personagem passando na frente de uma parede), o upscaler temporal (TSR) descarta o histórico do que saiu e começa deste quadro: sem rastro atrás dele. Achado pela profundidade do quadro anterior (copiada enquanto um ator se move, ~0,03 ms). Console: r.TSR.Disocclusion."),
    T("Denoiser History Fix", "Photorealistic and Video-realistic: a pixel the denoiser just started (uncovered behind a moving actor) takes the light of its neighbors on the same surface that hold many frames, instead of its own noisy one or two: the trail behind a moving actor stops boiling. Like ReBLUR's history fix. Console: r.Denoiser.HistoryFix.", "Fotorrealista e Vídeo-realista: um pixel que o denoiser acabou de começar (descoberto atrás de um ator em movimento) pega a luz dos vizinhos na mesma superfície que já têm muitos quadros, em vez dos seus um ou dois quadros ruidosos: o rastro atrás de um ator em movimento para de ferver. Como o history fix do ReBLUR. Console: r.Denoiser.HistoryFix."),
};
const int g_ui_tip_q20_count = (int)(sizeof g_ui_tips_q20 / sizeof g_ui_tips_q20[0]);
