# Orçamento do quadro (`frame_budget.c`)

Pedido (plano de melhorias, 4.4b e R11): o vídeo-realismo no Épico a 1440p cabe em **10 ms** na RTX 3060 (acima de
100 fps). O orçamento existia só no papel; agora cada fatia é medida, aparece no Debug Viewer e um teste acusa a
regressão.

## Fatias
GPU (as metas do R11 para 10 ms; com outro orçamento, proporcionais):

| Fatia | Meta | O que entra (timestamps do render, com o detalhe ligado) |
|---|---|---|
| Geometria e materiais | 3,0 | o upload da cena + o cull + as malhas do passe principal (opacas e translúcidas: o R11 dá 2 + 1) |
| Luz direta e sombras | 1,5 | o passe de sombra: sombras virtuais do sol, mapas das luzes locais |
| GI e reflexos | 3,0 | cache de superfície, sondas, traços |
| Volumes, água e céu | 1,0 | atmosfera, clima, céu/grade e o resolve |
| Câmera, cor e pós | 0,8 | máscara de seleção, exposição, DOF, motion blur, bloom |
| Reconstrução e UI | 0,7 | a composição: TSR (ou o upscaler) e a interface |

CPU (cada sistema com a sua medida; o quadro é o mais lento dos dois): render 2,0; Blueprints 1,5; física (corpos,
veículos, tecido) 1,5; animação 1,0; IA (BT, navegação, multidões) 1,0; partículas 0,5; módulos de jogo (vida,
inventário, interação, diálogo, missões, regras, habilidades, HUD) 0,5.

Sem o detalhe ligado (Debug Viewer fechado e sem `--budget`), o passe principal não é separado: cena + principal
contam como geometria e a fatia de volumes fica em 0. `fb_begin` liga o detalhe (`render_set_detail`).

## Uso
- **F10 > Frame Budget:** as fatias de agora contra a meta (marcadas "(over)"), o total da GPU contra os 10 ms.
- **`--bench N --budget <ms>`:** mede, imprime `Budget <cena>: ...` com média e pior quadro de cada fatia e falha (FAIL)
  quando a GPU ou a CPU média passa do orçamento. **`--budget-strict`:** falha também quando uma fatia passa da meta.
- Testes: `budget_house` (Épico, estrito), `budget_rpg` (Oakvale jogado, só o total), `budget_heavy` (18 ms por
  enquanto: uma guarda contra regressão até os upscalers baixarem a resolução interna).

## Medidas (RTX 3060, 2560×1417, Épico, 5 s)
| Cena | fps | GPU | Geometria | Sombras | GI | Volumes | Pós | Reconstrução | CPU |
|---|---|---|---|---|---|---|---|---|---|
| house | 319 | 3,05 | 1,17 | 0,00 | 0,79 | 0,75 | 0,13 | 0,18 | 0,81 |
| Oakvale (RPG, Play) | 155 | 6,25 | 2,35 | 0,13 | 1,71 | **1,63** | 0,16 | 0,24 | 1,70 |
| heavy | 63 | 15,66 | **11,79** | — | 2,70 | 0,82 | 0,15 | 0,18 | 1,24 |
- Oakvale cabe nos 10 ms, mas o céu e as nuvens passam da fatia de 1 ms (o próximo a baratear).
- A heavy (2,1 M triângulos, 600 k cartões Masked) não cabe: a geometria sozinha é 11,8 ms. O R11 prevê resolução interna
  de 960p a 1080p por TSR ou DLSS: é o item seguinte (upscalers).
- O custo do próprio orçamento: os 6 timestamps a mais do detalhe e a soma das fatias, abaixo de 0,01 ms.

## Novo sistema
Um sistema de CPU novo entra na fatia que for dele em `fb_sample` (a medida do último quadro, em ms). Uma fatia nova
de GPU vem de timestamps do render: some no grupo certo para que o total das fatias continue igual ao da GPU.
