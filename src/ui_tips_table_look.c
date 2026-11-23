// (Q26, notas-dev/temas-e-janelas.md) The descriptions of what the Azul Real look added to the editor: the Modes panel,
// Platform Support, the status bar's new buttons and readouts, the toolbar's groups, the new look's offer (a file of its
// own so the branches don't collide).
#include "ui_tips.h"

#define T(label, en, pt) { label, { en, pt } }

const UiTip g_ui_tips_look[] = {
    T("Modes", "editor modes: Selection, Landscape (sculpt and paint the ground) and Foliage (paint plants), and the active mode's tools. Shift+1 / Shift+2 / Shift+3.", "Os modos do editor: Seleção, Paisagem (esculpir e pintar o chão) e Folhagem (pintar plantas), e as ferramentas do modo ativo. Shift+1 / Shift+2 / Shift+3."),
    T("Platform Support", "Each target platform (PC, Android, VR) and what it has and lacks to package: the SDKs found, the JDK, the OpenXR runtime, the package folder, the upload key; the devices connected (Android by adb). It may be closed (Window > Platform Support).", "Cada plataforma alvo (PC, Android, VR) e o que ela tem e o que falta para empacotar: os SDKs encontrados, o JDK, o runtime OpenXR, a pasta do pacote, a chave de upload; os aparelhos conectados (Android pelo adb). Pode ser fechado (Janela > Suporte a Plataformas)."),
    T("Profiler", "The Debug Viewer (F10): the cost of each system and each object, the frame budget and the Trace (like Insights).", "O Debug Viewer (F10): o custo de cada sistema e de cada objeto, o orçamento do quadro e o Trace (como Insights)."),
    T("Device Manager", "Platform Support: the devices connected (the Android phones and headsets by adb, the VR runtime) and each target's readiness.", "Suporte a Plataformas: os aparelhos conectados (celulares e headsets Android pelo adb, o runtime de VR) e o que cada alvo tem pronto."),
    T("Shader Compile", "Pipelines and materials still compiling in the background. The editor never waits for them: the scene draws with what is ready.", "Pipelines e materiais ainda compilando em segundo plano. O editor nunca espera por eles: a cena é desenhada com o que já está pronto."),
    T("Derived Data", "The size of the derived data cache on this PC (the compiled pipelines, the processed meshes and textures). Click: open its folder.", "O tamanho do cache de dados derivados neste PC (os pipelines compilados, as malhas e texturas processadas). Clique: abre a pasta."),
    T("Select Mode", "Editor modes: Selection, Landscape, Foliage (Shift+1 / Shift+2 / Shift+3).", "Modos do editor: Seleção, Paisagem, Folhagem (Shift+1 / Shift+2 / Shift+3)."),
    T("Keep It", "Keeps the new look; Edit > Editor Preferences > Appearance changes the theme any time.", "Mantém o visual novo; Editar > Preferências do Editor > Aparência troca o tema a qualquer momento."),
    T("Go Back to My Previous Look", "Your previous theme and panel layout, as they were (the layout also stays in Window > Layout).", "O seu tema e o layout de painéis de antes, como estavam (o layout também fica em Janela > Layout)."),
    T("Try It", "Switches to Azul Real and the new default layout; you can go back right after.", "Troca para o Azul Real e o novo layout padrão; dá para voltar logo depois."),
    T("No, Thanks", "Keeps your theme and layout; Azul Real stays in Editor Preferences > Appearance.", "Mantém o seu tema e layout; o Azul Real fica em Preferências do Editor > Aparência."),
    T("Package for", "Packages the project for the chosen target, in the background (the card at the bottom right).", "Empacota o projeto para o alvo escolhido, em segundo plano (o cartão no canto inferior direito)."),
    T("ps_package", "Packages the project for the chosen target, in the background (the card at the bottom right).", "Empacota o projeto para o alvo escolhido, em segundo plano (o cartão no canto inferior direito)."),
    T("Search Classes...", "Type part of a class's name (in English or in the language shown): every category's matches are listed.", "Digite parte do nome de uma classe (em inglês ou no idioma mostrado): as que combinam aparecem em cada categoria."),
};
const int g_ui_tip_look_count = (int)(sizeof g_ui_tips_look / sizeof g_ui_tips_look[0]);
