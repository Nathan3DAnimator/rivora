// (Q11, notas-dev/toon.md) The descriptions of the Toon shading model's, the outline's and the toon lights' controls
// (the same form as ui_tips_table.c; a file of its own so the branches don't collide).
#include "ui_tips.h"

#define T(label, en, pt) { label, { en, pt } }

const UiTip g_ui_tips_q11[] = {
    T("Toon Bands", "How many flat steps the light makes on the surface (2: lit and shadow, the cel look; 0 or 1: no steps, the ramp alone).", "Quantos degraus chapados a luz faz na superfície (2: luz e sombra, o visual cel; 0 ou 1: sem degraus, só a rampa)."),
    T("Toon Softness", "How soft each band's edge is, as a share of a band (0: a hard line; 0.3: a short gradient).", "O quanto a borda de cada faixa é suave, numa fração da faixa (0: uma linha dura; 0,3: um degradê curto)."),
    T("Shadow Threshold", "Where the shadow side starts: 0.5 at the surface's terminator, higher gives more shadow, lower more light.", "Onde começa o lado da sombra: 0,5 no terminador da superfície, mais alto dá mais sombra, mais baixo mais luz."),
    T("Shadow Color", "The color the dark side tends to (times the base color): a cool purple, a warm red for skin.", "A cor para onde vai o lado escuro (vezes a cor base): um roxo frio, um vermelho quente para pele."),
    T("Received Shadow Color", "The color where another object hides the light (the cast shadows), apart from the dark side's.", "A cor onde outro objeto esconde a luz (as sombras projetadas), separada da do lado escuro."),
    T("Ramp Points", "How many points the tone ramp has (1 to 4): the light's level from dark (left) to lit (right) read through them.", "Quantos pontos a rampa de tons tem (1 a 4): o nível da luz do escuro (esquerda) ao iluminado (direita) lido por eles."),
    T("Point Position", "Where this ramp point sits on the light's level (0 the dark side, 1 fully lit).", "Onde este ponto da rampa fica no nível da luz (0 o lado escuro, 1 totalmente iluminado)."),
    T("Point Color", "The tone at this point (white keeps the base color, darker or tinted shades it).", "O tom neste ponto (branco mantém a cor base, mais escuro ou colorido sombreia)."),
    T("Ramp Image", "An image read from left (dark) to right (lit) instead of the points: a painted ramp. Empty: the points.", "Uma imagem lida da esquerda (escuro) para a direita (iluminado) no lugar dos pontos: uma rampa pintada. Vazio: os pontos."),
    T("Highlight Color", "The stylized highlight's color: a hard spot where the light reflects toward the eye.", "A cor do brilho estilizado: um ponto duro onde a luz reflete para o olho."),
    T("Highlight Strength", "How bright the stylized highlight is (0: none).", "O quanto o brilho estilizado é forte (0: nenhum)."),
    T("Highlight Size", "How big the stylized highlight's spot is.", "O tamanho do ponto do brilho estilizado."),
    T("Highlight Hardness", "How sharp the highlight's edge is (1: a hard cut, 0: soft).", "O quanto a borda do brilho é nítida (1: um corte duro, 0: suave)."),
    T("Rim Light Color", "The rim of light's color: the grazing edge on the side a light reaches.", "A cor da luz de borda: a borda rasante no lado que uma luz alcança."),
    T("Rim Light Strength", "How bright the rim of light is (0: none).", "O quanto a luz de borda é forte (0: nenhuma)."),
    T("Rim Light Width", "How far in from the silhouette the rim of light reaches.", "Até onde, a partir da silhueta, a luz de borda entra."),
    T("Rim Light Softness", "How soft the rim's inner edge is.", "O quanto a borda interna da luz de borda é suave."),
    T("Hatching", "Hatching on the screen where the light is low: lines across the shadow, crossed where it is darker (0: none).", "Hachura na tela onde a luz é baixa: linhas cruzando a sombra, cruzadas onde é mais escuro (0: nenhuma)."),
    T("Hatch Color", "The hatching lines' color (times what is under them).", "A cor das linhas da hachura (vezes o que está embaixo)."),
    T("Hatch Spacing", "The distance between the hatching lines, in pixels at 1440p (steady on the screen, also under TSR).", "A distância entre as linhas da hachura, em pixels a 1440p (firmes na tela, também com TSR)."),
    T("Hatch Angle", "The hatching lines' angle on the screen (degrees); the crossed ones at 90 to it.", "O ângulo das linhas da hachura na tela (graus); as cruzadas a 90 dele."),
    T("Hatch Threshold", "Below which tone of light the hatching starts (0.5: only the dark side).", "Abaixo de qual tom de luz a hachura começa (0,5: só o lado escuro)."),
    T("Toon Ambient", "How much of the indirect light (the sky, the GI) a toon surface takes: lower keeps the bands flat and readable.", "Quanto da luz indireta (céu, GI) uma superfície toon recebe: menos mantém as faixas chapadas e legíveis."),
    T("Draw Outline", "Draws an outline around the actors with this material (any shading model): Auto Adaptive, its width by the distance.", "Desenha um contorno em volta dos atores com este material (qualquer modelo de sombreamento): Auto Adaptativo, a largura pela distância."),
    T("Line Color", "The outline's color as the screen shows it (no light on it).", "A cor do contorno como a tela mostra (sem luz nele)."),
    T("Line Width", "The outline's width in pixels at 1440p (other resolutions keep the same look); the curve below scales it by the distance.", "A largura do contorno em pixels a 1440p (outras resoluções mantêm a aparência); a curva abaixo a escala pela distância."),
    T("Auto Adaptive", "The width follows the distance along the silhouette itself: thin near the camera, thicker far away (the curve). Off: the same width everywhere.", "A largura segue a distância ao longo da própria silhueta: fina perto da câmera, mais grossa longe (a curva). Desligado: a mesma largura em todo lugar."),
    T("Near Distance", "Up to this distance (m) the outline keeps its Near Width Scale.", "Até esta distância (m) o contorno mantém a Escala Perto."),
    T("Far Distance", "From this distance (m) on the outline has its Far Width Scale.", "A partir desta distância (m) o contorno tem a Escala Longe."),
    T("Near Width Scale", "The width's multiplier near the camera (under 1: thinner up close).", "O multiplicador da largura perto da câmera (abaixo de 1: mais fino de perto)."),
    T("Far Width Scale", "The width's multiplier far away (over 1: thicker far, the shapes stay readable).", "O multiplicador da largura longe (acima de 1: mais grosso longe, as formas continuam legíveis)."),
    T("Width Curve", "How the width goes from near to far: 1 straight, over 1 stays thin longer, under 1 thickens sooner.", "Como a largura vai de perto a longe: 1 reto, acima de 1 fica fino por mais tempo, abaixo de 1 engrossa antes."),
    T("Depth Sensitivity", "How big a jump of depth (a share of the distance) draws a line inside the actor (an arm in front of the body). Lower: more lines.", "Que salto de profundidade (uma fração da distância) desenha uma linha dentro do ator (um braço na frente do corpo). Menor: mais linhas."),
    T("Crease Angle", "Lines where the surface folds more than this angle (a box's edges). 0: only the silhouettes.", "Linhas onde a superfície dobra mais que este ângulo (as arestas de uma caixa). 0: só as silhuetas."),
    T("Override Toon and Outline", "This instance has its own Toon and Outline values (else its parent material's).", "Esta instância tem seus próprios valores de Toon e Contorno (senão os do material pai)."),
    T("Actor Outline", "This actor's own outline over its material's: As the material, On (its color and width below) or Off.", "O contorno próprio deste ator sobre o do material: Como o material, Ligado (cor e largura abaixo) ou Desligado."),
    T("Actor Outline Color", "This actor's outline color (the screen's).", "A cor do contorno deste ator (a da tela)."),
    T("Actor Outline Width", "This actor's outline width in px at 1440p (0: its material's); the distance curve stays its material's.", "A largura do contorno deste ator em px a 1440p (0: a do material); a curva da distância continua a do material."),
    T("Toon Light", "How this light treats the toon: As the material (a Toon material in bands), Force Toon (every material in a hard cel step) or Never Toon (lit smooth: a fill light).", "Como esta luz trata o toon: Como o material (um material Toon em faixas), Forçar Toon (todo material num degrau cel duro) ou Nunca Toon (luz suave: uma luz de preenchimento)."),
};
const int g_ui_tip_q11_count = (int)(sizeof g_ui_tips_q11 / sizeof g_ui_tips_q11[0]);
