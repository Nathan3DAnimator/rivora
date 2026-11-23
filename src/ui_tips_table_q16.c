// (Q16, notas-dev/em-jogo.md) The descriptions of what Q16 shows in the editor: the game's Build Lighting card in Play,
// the Blueprint nodes' pins and the console variables' names a menu of options lists (a file of its own so the
// branches don't collide).
#include "ui_tips.h"

#define T(label, en, pt) { label, { en, pt } }

const UiTip g_ui_tips_q16[] = {
    T("Building lighting (game)", "A Build Lighting (Async) asked by a Blueprint while playing: the editor's bake of the Static meshes (lightmaps, the baked probe volume) in the game's mode. Cancel stops it: the dynamic GI goes on lighting the level.", "Um Build Lighting (Async) pedido por um Blueprint durante o Play: o bake do editor das malhas Static (lightmaps, o volume de sondas) no modo do jogo. Cancelar para: a GI dinâmica segue iluminando o nível."),
    T("Loading Screen", "Build Lighting (Async)'s mode: the game waits behind the project's loading screen while the bake runs as fast as the GPU goes (30 ms of it a frame).", "Modo do Build Lighting (Async): o jogo espera atrás da tela de carregamento do projeto enquanto o bake corre o mais rápido que a GPU deixa (30 ms dela por quadro)."),
    T("Progressive", "Build Lighting (Async)'s mode: the game plays on; a slice of the bake each frame inside Budget ms of the GPU (1 ms by default), the dynamic GI lighting the level until the lightmaps are ready.", "Modo do Build Lighting (Async): o jogo continua; uma fatia do bake por quadro dentro de Budget ms da GPU (1 ms por padrão), a GI dinâmica iluminando o nível até os lightmaps ficarem prontos."),
    T("Budget ms", "The GPU's milliseconds a frame the game's Build Lighting may take (0: the mode's own: 1 ms while playing, 30 ms behind the loading screen). Measured by its own timestamps: the slice follows the GPU.", "Os milissegundos de GPU por quadro que o Build Lighting do jogo pode usar (0: o do modo: 1 ms jogando, 30 ms atrás da tela de carregamento). Medido pelos próprios timestamps: a fatia acompanha a GPU."),
    T("Property", "A Details field's label (\"Intensity\", \"Max Walk Speed\", \"Light.Color\"): Set / Get Property by Name reach every field the Details panel edits.", "O rótulo de um campo do Details (\"Intensity\", \"Max Walk Speed\", \"Light.Color\"): Set / Get Property by Name alcançam todo campo que o Details edita."),
    T("Console Variable", "A setting by its console name (r.ScreenPercentage, r.GI.Probes, p.GravityZ, au.MasterVolume): render, quality, physics and audio, in the game as in Play.", "Um ajuste pelo nome do console (r.ScreenPercentage, r.GI.Probes, p.GravityZ, au.MasterVolume): render, qualidade, física e áudio, no jogo como no Play."),
};
const int g_ui_tip_q16_count = (int)(sizeof g_ui_tips_q16 / sizeof g_ui_tips_q16[0]);
