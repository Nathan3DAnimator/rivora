// --assistant-test (the test assistant, notas-dev/assistente.md): the base read from the packed folder (as the online
// fetch reads GitHub's), the answers to questions in Portuguese and English (with a typo, an error message pasted, a
// function's name, nonsense), the time of a question, the offline copy written and removed, the window drawn.
#include <stdio.h>
#include <string.h>

#include "assistant.h"
#include "assistant_ui.h"
#include "async.h"
#include "bp_editor.h"
#include "editor.h"
#include "i18n.h"
#include "platform.h"
#include "script.h"
#include <stdlib.h>

static struct {
    int phase, frames;
    bool ok, offline_seen, removed;
    double query_ms;
    int answered;
} T = { 0, 0, true };

// The questions as people ask them: who never used an engine, slang, English and Spanish, a professional, code. want:
// the answer(s) accepted ("a|b"), within the first `within`
typedef struct Case { const char* q; const char* want; int within; bool weak; } Case;
static const Case g_cases[] = {
    // formal
    { "como faço o personagem pular duas vezes", "double-jump", 3, false },
    { "empacotar o jogo", "package-game", 1, false },
    { "importar do tlender", "import-tlender", 3, false },
    { "inimigo que persegue o jogador", "enemy-chase", 3, false },
    { "Qual é a diferença entre material e textura?", "material-vs-texture", 2, false },
    { "Como salvar o progresso do jogador?", "save-load", 3, false },
    // the beginner
    { "nunca usei uma engine, por onde começo?", "start-here|first-game", 2, false },
    { "o que é um ator", "actor-component", 2, false },
    { "colocar uma luz", "lights|place-actors", 3, false },
    { "perdi a camera nao acho mais nada", "viewport-navigation", 3, false },
    { "o jogador nasce no lugar errado", "game-framework", 3, false },
    // informal, slang, typos
    { "mano como faço o bagulho ficar girando", "rotate-object", 3, false },
    { "pakage the game", "package-game", 3, false },
    { "o jogo ta travando mt, fps la embaixo", "performance|performance-budget|fps-frame", 3, false },
    { "o editor travou carregando", "background-tasks", 3, false },
    { "plataforma que sobe e desce", "moving-platform", 3, false },
    // English and Spanish
    { "how do I add a light", "lights", 3, false },
    { "how do i make my dude double jump lol", "double-jump", 3, false },
    { "quiero que el personaje salte dos veces", "double-jump", 3, false },
    // the professional
    { "como funciona o ECS da engine", "ecs-architecture", 2, false },
    { "replicar variavel no multiplayer com RPC", "replication-pro|multiplayer", 2, false },
    { "Cast To outro blueprint", "blueprint-communication", 3, false },
    { "JIT do bytecode", "jit-native", 2, false },
    { "virtual shadow maps PCSS", "shadows-pro", 2, false },
    // code
    { "PlaySound2D", "PlaySound2D", 3, false },                     // (a function: the API)
    { "IsKeyDown", "IsKeyDown|keyboard-input", 2, false },
    { "def on_begin_overlap(self, other)", "event-concept|coin-pickup|teleport|score-counter|python-guide|health-damage|fog-change", 3, false },
    { "'velocidade' isn't declared here", "error-not-declared", 1, false }, // (the compiler's message pasted)
    { "for i in 0..10", "real-language|arrays-loops", 3, false },
    // the areas added with the rule "every new feature goes into the Assistant" (coverage.txt)
    { "como fazer uma caixa quebrar em pedaços", "fracture", 2, false },
    { "sessao com senha e filtro de regiao no multiplayer", "advanced-sessions", 2, false },
    { "advanced sessions ban player", "advanced-sessions", 3, false },
    { "meu jogo ta lento, como vejo o que ta pesando", "debug-viewer|performance", 3, false },
    { "how do I make glass", "glass-translucent", 2, false },
    { "trocar o idioma pra ingles", "language", 2, false },
    { "colocar um video na tela de carregamento", "loading-screen", 2, false },
    { "o foot ik nao acha o pe do meu personagem do mixamo", "rig-detection", 2, false }, // (2026-11-23)
    { "orelha de coelho balancando com fisica", "physics-auto-fit", 2, false },
    { "carregar o mapa no lobby sem travar", "level-preload", 2, false },
    { "posso vender meu jogo feito na engine", "open-source-license", 2, false }, // (2026-11-23)
    { "como pular a animacao de abertura do editor", "editor-intro", 2, false }, // (2026-11-23)
    { "tem algum jogo de exemplo pronto", "demo-games", 3, false },
    { "o verniz coat do material nao faz nada", "material-pins-no-effect", 2, false },
    { "como usar a cor de vertice no material", "vertex-color-detail", 2, false },
    { "como colocar sangue na parede", "decals", 2, false },
    { "relevo de pedras no material sem poligonos", "parallax", 2, false },
    { "parallax occlusion displacement height map", "parallax", 1, false },
    { "bullet hole decal from a line trace", "decals", 2, false },
    { "detail normal texture for close up pores", "vertex-color-detail", 2, false },
    { "fotorrealista com ruido granulado na iluminacao", "rt-denoiser", 2, false },
    { "rough reflections are noisy, how does the denoiser work", "rt-denoiser", 2, false },
    { "ray tracing usando muita memoria de video", "rt-clusters", 2, false },
    { "does the engine support rtx mega geometry clusters", "rt-clusters", 1, false },
    { "minha placa nao tem rtx, a iluminacao global funciona?", "gi-software-rt", 2, false },
    { "global illumination on android without ray tracing", "gi-software-rt", 2, false },
    { "montanha longe fica sem luz indireta", "far-probes", 2, false },
    { "distant buildings get no bounce light", "far-probes", 2, false },
    { "pele do rosto parece plastico", "digital-humans", 2, false },
    { "ear should glow red with a light behind", "digital-humans", 2, false },
    { "como fazer olho realista com iris", "digital-humans", 2, false },
    { "como colocar cabelo de verdade em fios no personagem", "hair-groom", 2, false },
    { "realistic hair strands instead of cards", "hair-groom", 2, false },
    { "como fazer uma fogueira com fumaca", "fire", 2, false },
    { "como fazer um rio com correnteza", "rivers-swimming", 2, false },
    { "my character should swim in the lake", "rivers-swimming", 2, false },
    { "objeto flutuando na agua", "rivers-swimming", 2, false },
    { "neblina so dentro de uma sala ou caixa", "sky-fog", 3, false },
    { "importar cenario escaneado pelo celular", "gaussian-splat", 2, false },
    { "how to show a 3dgs scan in the level", "gaussian-splat", 2, false },
    { "importar fumaca vdb do blender", "volumes-vdb", 2, false },
    { "tirar sombra assada da textura escaneada", "delight", 2, false },
    { "mundo aberto grande carregando por distancia", "world-partition", 3, false },
    { "como fazer um fio pendurado entre dois postes", "splines-cables", 2, false }, // (Q14)
    { "spline mesh road along a curve", "splines-cables", 2, false },
    { "mover um objeto seguindo a spline", "splines-cables", 3, false }, // (move-object first: a fair answer too)
    { "camera balancando com os passos em primeira pessoa", "first-person-camera", 2, false }, // (Q12)
    { "camera shake explosion first person recoil", "first-person-camera", 2, false },
    { "autofoco da camera e respiracao quando cansado", "first-person-camera", 2, false },
    { "how to make far away meshes cheaper with hlod", "world-partition", 2, false },
    { "ligar e desligar camadas de atores no jogo", "world-partition", 3, false },
    { "usar minha propria fonte ttf no menu", "ui-fonts-lists", 2, false },
    { "list view with thousands of items", "ui-fonts-lists", 2, false },
    { "texto com uma palavra colorida e negrito", "ui-fonts-lists", 3, false },
    { "som do motor do carro mudando com a rotacao", "metasounds", 2, false },
    { "procedural rain sound that gets stronger", "metasounds", 2, false },
    { "como fazer um sintetizador com oscilador e filtro", "metasounds", 2, false },
    { "exportar a cinematica como video", "cinematics-render", 2, false },
    { "record my gameplay into a sequence", "cinematics-render", 2, false },
    { "escurecer a tela no fim da cutscene", "cinematics-render", 3, false },
    { "rastro atras da espada quando ataca", "particle-ribbons", 2, false },
    { "ribbon trail behind a projectile", "particle-ribbons", 2, false },
    { "pintar grama e terra no terreno", "landscape-paint", 2, false },
    { "inimigo nao reage quando leva dano", "ai-damage-avoidance", 2, false }, // (Q24)
    { "enemies walk through each other", "ai-damage-avoidance", 2, false },
    { "paint layers on the landscape", "landscape-paint", 2, false },
    { "testar multiplayer com duas janelas no editor", "online-prediction", 2, false },
    { "client side prediction and reconciliation", "online-prediction", 2, false },
    { "como usar git no editor", "source-control", 2, false },
    { "objeto parece flutuar sem sombra no pe", "contact-shadows", 2, false },
    { "screen space contact shadows", "contact-shadows", 2, false },
    { "como usar um arquivo cube de lut", "color-grading-lut", 2, false },
    { "camera com iso e velocidade do obturador", "cine-camera", 2, false },
    { "jogo em hdr na tela hdr", "hdr10-output", 2, false },
    { "reflexo no chao molhado sem ray tracing", "ssr-reflections", 2, false },
    { "ligar o compute assincrono", "async-compute", 2, false },
    { "async compute queue", "async-compute", 2, false },
    { "screen space reflections on medium", "ssr-reflections", 2, false },
    { "hdr10 output paper white", "hdr10-output", 2, false },
    { "cine camera bokeh blades", "cine-camera", 2, false },
    { "color grading lut from davinci resolve", "color-grading-lut", 2, false },
    { "commit my assets with git lfs", "source-control", 2, false },
    { "gravar o desempenho para achar uma travada", "trace-insights", 2, false },
    { "unreal insights timeline profiler", "trace-insights", 2, false },
    { "como criar um plugin com nos novos", "plugins", 2, false },
    { "enable a plugin dll", "plugins", 2, false },
    { "simulated fire like niagara fluids", "fire", 2, false },
    { "a engine tem restir para muitas luzes", "many-lights", 2, false },
    { "VK_ERROR_DEVICE_LOST in vkWaitSemaphores", "gpu-crash", 1, false }, // (the crash report's message pasted)
    { "luz atravessa a parede", "scalability|shadows-pro|lights|gi-fallback", 3, false }, // (Q27.12: the fallback GI's lamps never light through a wall)
    { "sombra da luz pontual sem ray tracing", "shadows-pro|scalability", 3, false },
    { "como marcar o inimigo como atordoado com tag", "gameplay-tags", 2, false },
    { "valor diferente em cada ator no details", "variable-concept", 3, false },
    { "personagem nao sobe na plataforma que se move", "character-physics", 2, false },
    { "porta com dobradica que abre com fisica", "physics-constraint", 2, false },
    { "como fazer a ia pular um buraco", "navmesh", 2, false },
    { "ai can't find a path around the wall", "navmesh|enemy-chase", 3, false },
    { "como fazer o rosto sorrir com blend shape", "morph-targets", 2, false },
    { "inimigo com vida e escudo que morre com dano de fogo", "health-damage", 2, false },
    { "grenade radial damage falloff", "health-damage", 3, false },
    { "como fazer uma mochila com itens que empilham", "inventory", 2, false },
    { "equip a sword in the right hand", "inventory", 3, false },
    { "como salvar o progresso com checkpoint e auto save", "save-progress", 2, false },
    { "save slots with a thumbnail and continue button", "save-progress", 3, false },
    { "porta trancada que abre com a chave apertando E", "interaction", 2, false },
    { "press E to open a chest and hold to interact", "interaction", 2, false },
    { "como fazer um npc conversar com escolhas", "dialogue", 2, false },
    { "dialogue option hidden unless the player has a quest tag", "dialogue", 3, false },
    { "como fazer uma missão com objetivos e marcador no hud", "quests", 3, false },
    { "kill 5 wolves quest with a reward and a tracker", "quests", 3, false },
    { "como espalhar uma floresta no landscape com um grafo procedural", "pcg", 2, false }, // (Q15)
    { "pcg scatter rocks along a spline with self pruning and a seed", "pcg", 3, false },
    { "generar un bosque procedural por celdas del world partition", "pcg|world-partition", 3, false },
    { "como usar texturas udim 1001 1002 no personagem", "udim", 2, false }, // (Q18)
    { "import a udim folder from substance painter", "udim", 3, false },
    { "pause menu hitches the first time the options open", "general-optimization", 3, false }, // (Q22)
    { "como ver o canal alfa e os mips de uma textura", "texture-viewer", 3, false }, // (Q23)
    { "hdr texture doesn't open, exposure of an hdri", "texture-viewer", 3, false },
    { "pintei a textura no photoshop e a miniatura nao atualiza", "texture-viewer", 3, false },
    { "como fazer uma partida com cronometro, pontos e vidas", "game-rules", 2, false },
    { "win the match when all enemies are dead, best of 3 rounds", "game-rules", 3, false },
    { "como fazer um feitiço que gasta mana e tem recarga", "abilities", 2, false },
    { "poison debuff that stacks and deals damage over time", "abilities", 3, false },
    { "como mostrar a barra de vida e uma bussola com marcador no hud", "hud", 2, false },
    { "floating damage numbers and an arrow at the screen edge to the objective", "hud", 3, false },
    { "como dividir os 10 ms do quadro entre sombras e GI", "performance-budget", 2, false },
    { "frame budget slices over their goal", "performance-budget|debug-viewer", 2, false },
    { "modo desempenho do upscaling e resolucao dinamica", "tsr-upscaling", 2, false },
    { "dlss quality mode", "tsr-upscaling", 3, false },
    { "como ligar o dlss no projeto", "vendor-upscalers", 2, false },
    { "fsr greyed out in the upscaler list", "vendor-upscalers", 2, false },
    { "qual upscaler usar xess ou fsr", "vendor-upscalers", 2, false },
    { "como fazer pintura de carro com verniz", "material-pins-no-effect", 3, false },
    { "brushed metal anisotropic highlight", "material-pins-no-effect", 3, false },
    { "mudar o estilo de render para video realista", "render-styles", 2, false },
    { "agx tonemapper stylized look", "render-styles", 2, false },
    { "xyzzy qwfp zzkv", NULL, 0, true },                           // (nonsense: offered as a suggestion)
};
// (2026-11-25) a query as printed: its "_" as spaces, so a pasted error ("VK_ERROR_DEVICE_LOST") never matches the
// suite's failure pattern (FAIL|VUID|DEVICE_LOST) by itself
static const char* shown(const char* q) {
    static char b[256];
    snprintf(b, sizeof b, "%s", q);
    for (char* c = b; *c; c++)
        if (*c == '_') *c = ' ';
    return b;
}

static bool wanted(const char* want, const char* id) {
    char w[200];
    snprintf(w, sizeof w, "%s", want);
    for (char* t = strtok(w, "|"); t; t = strtok(NULL, "|"))
        if (!strcmp(t, id)) return true;
    return false;
}

// every recipe's code (```real / ```cs / ```py / ```c) compiled as a script: the articles teach code that works
static void check_recipes(void) {
    int compiled = 0, failed = 0;
    for (int i = 0; i < assistant_doc_count(); i++) {
        const AsDoc* d = assistant_doc(i);
        if (d->src != AS_SRC_ARTICLE) continue;
        const char* p = d->body[0];
        while ((p = strstr(p, "```")) != NULL) {
            p += 3;
            ScriptLang lang = !strncmp(p, "real", 4) ? SL_REAL : !strncmp(p, "cs", 2) ? SL_CSHARP : !strncmp(p, "py", 2) ? SL_PYTHON
                            : !strncmp(p, "c\n", 2) ? SL_C : SL_COUNT;
            const char* body = strchr(p, '\n');
            const char* end = body ? strstr(body + 1, "```") : NULL;
            if (!body || !end) break;
            if (lang != SL_COUNT && strstr(body, "class ")) { // (whole classes: the snippets of a line or two aren't)
                ScriptFile* f = calloc(1, sizeof *f);
                if (!f) return;
                f->lang = lang;
                snprintf(f->vpath, sizeof f->vpath, "/Content/__assistant_%s.real", d->id); // (one per article: its variants declare the same structures)
                const char* cn = strstr(body, "class ") + 6;
                size_t k = 0;
                while (cn[k] && cn[k] != ' ' && cn[k] != '(' && cn[k] != ':' && cn[k] != '\n' && k + 1 < sizeof f->name) k++;
                snprintf(f->name, sizeof f->name, "%.*s", (int)k, cn);
                bool ok = script_compile_text(f, body + 1, (size_t)(end - body - 1), false);
                if (ok) compiled++;
                else {
                    failed++;
                    printf("[RealEngine] FAIL: Assistant: the recipe of '%s' (%s) doesn't compile\n", d->id, g_script_lang_names[lang]);
                    for (uint32_t q = 0; q < f->ndiags; q++) printf("[RealEngine]   %s(%d:%d): %s\n", f->name, f->diags[q].line, f->diags[q].col, f->diags[q].text);
                }
                free(f->syms);
                free(f);
            }
            p = end + 3;
        }
    }
    printf("[RealEngine] Assistant: %d recipes' code compiled, %d failed\n", compiled, failed);
    if (failed || compiled < 20) T.ok = false;
}

// (2026-11-25) asked in a Spanish editor: the articles' Spanish sections and the controls' Spanish in the index
static const Case g_cases_es[] = {
    { "como cambio el idioma del editor a español", "language", 3, false },
    { "plugins integrados del editor para dispersar objetos", "editor-plugins-kit", 3, false },
    { "traducir el juego a otros idiomas", "game-localization", 3, false },
    { "cargar el mapa en el lobby sin trabas", "level-preload", 3, false },
};
static void check_es(void) {
    int keep = i18n_language();
    i18n_set_language(LANG_ES);
    assistant_relang();
    for (size_t i = 0; i < sizeof g_cases_es / sizeof g_cases_es[0]; i++) {
        const Case* c = &g_cases_es[i];
        AsHit hits[8];
        bool weak = false;
        int n = assistant_search(c->q, hits, 8, &weak), at = -1;
        for (int k = 0; k < n && at < 0; k++)
            if (assistant_doc(hits[k].doc) && wanted(c->want, assistant_doc(hits[k].doc)->id)) at = k;
        bool good = at >= 0 && at < c->within;
        printf("[RealEngine] Assistant (es): \"%s\" -> %s (wanted at %d)%s\n", shown(c->q), n ? assistant_doc(hits[0].doc)->id : "-", at + 1, good ? "" : "  WRONG");
        if (!good) T.ok = false;
        else T.answered++;
    }
    i18n_set_language(keep);
    assistant_relang();
}

static void check_cases(void) {
    for (size_t i = 0; i < sizeof g_cases / sizeof g_cases[0]; i++) {
        const Case* c = &g_cases[i];
        AsHit hits[8];
        bool weak = false;
        int n = assistant_search(c->q, hits, 8, &weak);
        int at = -1;
        for (int k = 0; c->want && k < n; k++) {
            const AsDoc* d = assistant_doc(hits[k].doc);
            if (d && wanted(c->want, d->id)) {
                at = k;
                break;
            }
        }
        const AsDoc* top = n ? assistant_doc(hits[0].doc) : NULL;
        bool good = c->weak ? weak : (at >= 0 && at < c->within);
        printf("[RealEngine] Assistant: \"%s\" -> %s (%s%s)%s\n", shown(c->q), top ? top->id : "-", c->want ? "wanted at " : "weak expected",
               c->want ? (at >= 0 ? (char[2]){ (char)('1' + at), 0 } : "none") : "", good ? "" : "  WRONG");
        if (!good) { // (what came before it: the articles to tell apart)
            T.ok = false;
            for (int k = 0; k < n && k < 6; k++) {
                const AsDoc* d = assistant_doc(hits[k].doc);
                printf("[RealEngine] Assistant:     %d. %s\n", k + 1, d ? d->id : "-");
            }
        } else T.answered++;
    }
    check_es();
    // the time of a question (the whole base: articles, functions, controls)
    double t0 = re_time_seconds();
    AsHit hits[24];
    bool weak;
    int ncases = (int)(sizeof g_cases / sizeof g_cases[0]);
    for (int r = 0; r < 200; r++) assistant_search(g_cases[r % ncases].q, hits, 24, &weak);
    T.query_ms = (re_time_seconds() - t0) * 1000.0 / 200.0;
}

void assistant_test_frame(Editor* ed) {
    (void)ed;
    T.frames++;
    switch (T.phase) {
    case 0: // the online fetch (REALENGINE_ASSISTANT_URL: the packed folder)
        if (T.frames < 3) return;
        assistant_init();
        assistant_fetch(false);
        T.phase = 1;
        return;
    case 1:
        if (assistant_state() == AS_FETCHING) return;
        if (assistant_state() != AS_ONLINE || assistant_article_count() < 100) {
            printf("[RealEngine] FAIL: Assistant: the base wasn't fetched (%d articles): %s\n", assistant_article_count(), assistant_error());
            T.ok = false;
            T.phase = 5;
            return;
        }
        check_cases();
        check_recipes();
        assistant_ui_open("pular duas vezes"); // (the window drawn through the measure)
        assistant_fetch(true);                // the offline copy
        T.phase = 2;
        return;
    case 2:
        if (assistant_state() == AS_FETCHING) return;
        T.offline_seen = assistant_state() == AS_OFFLINE && assistant_offline_present();
        assistant_remove_offline();
        T.removed = !assistant_offline_present();
        T.phase = 5;
        return;
    default: return;
    }
}

bool assistant_test_done(void) { return T.phase == 5; }

bool assistant_test_check(Editor* ed) {
    (void)ed;
    AsStats s = assistant_stats();
    int per[3] = { 0, 0, 0 };
    for (int i = 0; i < assistant_doc_count(); i++) per[assistant_doc(i)->src]++;
    printf("[RealEngine] Assistant: the sources: %d articles, %d engine functions, %d controls' descriptions\n", per[0], per[1], per[2]);
    if (per[1] < 100) printf("[RealEngine] FAIL: Assistant: the engine's functions aren't in the base\n"), T.ok = false;
    printf("[RealEngine] Assistant: %d articles (%u KB, version %s), %u documents, %u terms, %u words; built in %.1f ms; "
           "a question %.3f ms; %d of %d answered right; offline copy %s, removed %s\n",
           assistant_article_count(), assistant_base_bytes() / 1024u, assistant_version(), s.docs, s.terms, s.vocab, s.build_ms, T.query_ms,
           T.answered, (int)(sizeof g_cases / sizeof g_cases[0] + sizeof g_cases_es / sizeof g_cases_es[0]), T.offline_seen ? "written" : "MISSING", T.removed ? "yes" : "NO");
    bool ok = T.ok && T.phase == 5 && T.offline_seen && T.removed;
    if (T.query_ms > 2.0) printf("[RealEngine] FAIL: Assistant: a question took %.3f ms (2 at most)\n", T.query_ms), ok = false;
    if (!T.offline_seen || !T.removed) printf("[RealEngine] FAIL: Assistant: the offline copy wasn't written or removed\n");
    if (ok) printf("[RealEngine] Assistant OK\n");
    else printf("[RealEngine] FAIL: Assistant\n");
    return ok;
}
