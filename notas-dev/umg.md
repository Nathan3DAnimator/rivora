# UMG — UI do jogo (Widget Blueprints)

Código: `src/ui_widget.h/.c` (dados, JSON, execução — sem ImGui, no `re_render`), `src/ui_widget_draw.c` (layout,
desenho, cliques, Designer — no editor e no jogo), nós em `bp_internal.h`/`blueprint.c`/`bp_vm.c`, imagens por
`brand_ui_image` (`brand.c`), teste `src/umg_test.c`.

## Como funciona
- Um **Blueprint Class com pai "User Widget"** é um Widget Blueprint: abre na aba **Designer** (no lugar do Viewport)
  com Paleta, Hierarquia, prévia 16:9 (clique seleciona, arrastar move, o canto redimensiona, Del apaga) e os Detalhes
  do widget. A árvore (`UiTree`, até 64 widgets) vai no `.rbp` em `"ui_tree"`.
- Widgets: **Canvas Panel** (raiz; filhos por âncoras/posição/tamanho/alinhamento, presets de âncora como a Unreal,
  âncoras esticadas = margens), **Text**, **Image** (textura do Content, arrastável do Content Browser), **Button**,
  **Progress Bar**, **Vertical/Horizontal Box** (filhos empilhados), **Border**. Tamanhos em pixels a 1080p
  (escalados pela altura da tela).
- **Ligações**: o texto de um Text e o percentual de uma Progress Bar podem ser ligados a uma variável do widget (lida
  a cada quadro: `bp_instance_var_text`).
- **Nós**: Create Widget (a instância é uma entidade com o Blueprint; Event BeginPlay = Construct da Unreal), Add to
  Viewport (ZOrder), Remove from Parent, Set Text (Widget), Set Percent, Set Widget Visible, Set Color and Opacity (o
  widget pelo nome, escolhido nos Detalhes do nó; Target vazio = self), Set Show Mouse Cursor.
- **Botões**: hover/press com cor; o clique chama o Custom Event `OnClicked_<nome>` do widget — o botão
  "+ Ao Clicar (evento)" nos Detalhes cria esse evento no Event Graph.
- Desenho por ImGui (draw list) por cima da vista: no PIE dentro do viewport, no jogo empacotado na background draw
  list (o Debug Viewer fica por cima). Com Show Mouse Cursor o cursor sai e a câmera para de girar.

## Medidas (RTX 3060, 2560x1417)
| Cena | Resultado |
|---|---|
| `--umg-test` (8 widgets: painel, título, texto ligado, barra ligada, botão) | desenho 0,005 ms/quadro; 288-314 fps com validação |

Teste `umg`: o widget na tela, "Score" ligado mostra "7", o clique no StartButton roda o evento (título "Clicked!").

## Limites / próximos passos
- Sem fontes próprias (a fonte do editor, escalada; negrito simulado); imagens carregadas na primeira vez que aparecem
  (até 32 na sessão). Sem Rich Text, List View nem Named Slot. A rotação (Angle) gira Imagens; nos outros só anima o valor.

## Widgets como os da Unreal 5.8 (sugestões #59 / #61, 2026-11-04)
- **Paleta da Unreal** por categoria, com busca: Comuns (Borda, Botão, Caixa de Seleção, Imagem, Barra de Progresso,
  Controle Deslizante, Texto, Espaçador, Indicador de Carregamento, Indicador Circular), Entrada (Caixa de Combinação
  (Texto), Texto Editável, Caixa Numérica), Painel (Canvas, Caixa Horizontal/Vertical, Sobreposição, Caixa de Rolagem,
  Caixa de Tamanho, Grade Uniforme, Caixa com Quebra). Até 128 widgets por árvore. Os tipos antigos guardam os números
  (arquivos antigos abrem iguais: um Botão antigo ganha os cantos e o contorno que tinha, um Texto a sombra).
- **Slot** como o da Unreal: no Canvas âncoras/posição/alinhamento; nas caixas a **Regra de Tamanho** (Auto ou Fill com
  peso), margem interna do slot (E C D B) e alinhamento horizontal/vertical (Fill, Esquerda/Topo, Centro, Direita/Baixo);
  na Grade Uniforme a ordem dá a célula; na Caixa com Quebra a linha quebra quando não cabe. **Mover para Cima/Baixo** na
  ordem dos irmãos. Prévia nas resoluções 1920×1080, 1280×720, 2560×1440, 21:9, 9:16 e 4:3.
- **Aparência**: cantos arredondados, contorno (largura e cor), cores Normal/Sob o Mouse/Pressionado do Botão (sem
  opacidade: o Normal mais claro/escuro), Texto com negrito, sombra e quebra de linha, Imagem com cantos e rotação.
- **Comportamento**: Visibilidade (Visível, Recolhido = sem espaço, Oculto = com espaço, Sem Clique ele e os filhos, Sem
  Clique só ele), Está Habilitado (cinza, sem entrada), Texto da Dica (aparece a 0,6 s sob o mouse), Opacidade;
  **Transformação de Desenho** (translação, escala pelo centro, ângulo) — os filhos vão junto.
- **Entrada**: o Controle Deslizante segue o mouse (passo e faixa), a Caixa Numérica muda arrastando, a Caixa de Seleção
  alterna, o Texto Editável recebe o que é digitado (Backspace, Enter confirma, Esc sai; cursor piscando), a Caixa de
  Combinação abre a lista por cima, a roda rola a Caixa de Rolagem (barra à direita, conteúdo recortado).
- **Eventos** (botões "+" nos Detalhes, como os verdes da Unreal): OnClicked/OnPressed/OnReleased/OnHovered/OnUnhovered
  (Botão), OnValueChanged (Deslizante, Numérica), OnCheckStateChanged, OnTextChanged/OnTextCommitted, OnSelectionChanged:
  o Custom Event `<Evento>_<nome>` no Event Graph, enviados depois do desenho.
- **Animações de widget** (a Timeline do UMG): painel embaixo da prévia: + Animação, nome, duração; trilhas
  `widget · propriedade` (Opacidade, Translação X/Y, Escala X/Y, Ângulo, Cor R/G/B/A) com chaves em losango (clique
  seleciona, arrastar move no tempo, Del apaga, botão direito na trilha a apaga); régua para escolher o tempo, Play/Stop
  na prévia (uma cópia: a árvore guarda o autoral), **Key** marca as trilhas do widget selecionado com os valores dos
  Detalhes. Interpolação suave entre as chaves. Salvas no `.rbp` em `"ui_anims"`.
- **Nós novos**: Get Value (Widget), Set Value (Widget), Get Text (Widget), Set Is Enabled, Play Widget Animation (Num
  Loops To Play: 1 por padrão, 0 = para sempre; Playback Speed), Stop Widget Animation. (O nome "Play Animation" já era
  do Skeletal Mesh: os nós são gravados pelo nome, dois iguais trocavam um pelo outro ao abrir o arquivo.)
- Correção: o Construct (BeginPlay) de um widget roda dentro do Create Widget, antes de a instância saber a entidade:
  agora os nós dele já o encontram (Play Animation no Construct funcionava só depois).
- **Medidas** (RTX 3060, 1440p): o teste com 39 widgets (os 8 antigos + painel de opções com os 6 de entrada, Caixa de
  Rolagem com 8 linhas, Grade com 8, indicador circular, a animação Pulse) desenha em 0,022-0,039 ms; 410-430 fps com
  validação. Teste `umg` cobre também Set Value (0,9), a opção da Combo (High) e a escala do título pulsando.

## Fontes, Rich Text e listas (B7, 09/11)
- **Fontes do projeto**: `UiWidget.font` (um .ttf/.otf: `/Content/Fonts/...` ou um caminho), `italic`, `outline` e
  `outline_col`. Cada (arquivo, negrito, itálico) entra uma vez no atlas do ImGui (até 32). O ImGui 1.92 gera os glifos de
  cada tamanho quando aparecem: a letra fica nítida em qualquer tamanho, que é o que o MSDF daria numa UI 2D; o MSDF só
  faria falta para texto no mundo 3D (fica nas sobras). Negrito/itálico pelos arquivos da família (`-Bold`, `Bold`,
  `_Bold`, o `b` do Windows; `-Italic`/`i`; `-BoldItalic`/`z`; `Nome-Regular` tem a família `Nome`); sem eles, o negrito
  desenha duas vezes e o itálico inclina os vértices do texto (12°, pela linha de base de cada linha).
- **Contorno**: 4 cópias deslocadas até 2 px, 8 acima. Com 8 sempre, o teste com a validação do Vulkan caía de ~440 para
  ~190 fps (a validação pesa por cópia); sem a validação não mudava.
- **Rich Text Block** (`UIW_RICH_TEXT`): marcas `<b>`, `<i>`, `<color=#RRGGBB[AA]>` (sRGB), `<size=N>`, `<outline>`, com
  pilha de 16; layout palavra a palavra com quebra e quebras de linha.
- **List View / Tile View** (`UIW_LIST_VIEW`, `UIW_TILE_VIEW`): itens com `|` (como a Combo Box). Set Text maior que o
  texto do widget vai para um depósito à parte (64 listas, liberadas no fim do Play). Só as linhas/tiles visíveis são
  percorridas e desenhadas; barra de rolagem; roda do mouse; clique = OnItemClicked e OnSelectionChanged; Get/Set Value =
  índice (-1 nenhum), Get Text = item. Tile View por Columns.
- A máscara de tipos do seletor de widget (`ui_widget_combo`) passou a 64 bits: os tipos já passavam de 32.
- **Medidas**: o teste `umg` (41 widgets: Rich Text com 3 fontes do Windows, lista de 1000 itens) desenha em 0,058-0,073
  ms com 9 linhas desenhadas (antes 0,022-0,039 ms sem esses widgets); 440 fps com validação.
- **Falta**: MSDF para texto 3D, Named Slot, entrada de lista como widget (o Entry Widget da Unreal), String Tables.

## Widgets de toque
Virtual Joystick, Touch Button e Touch Look Area (vários dedos ao mesmo tempo; o mouse no PC) e o WBP_TouchControls padrão: [android.md](android.md).

## Opções do jogador e teclado/controle
Widget ligado a uma Game User Setting (Detalhes), o **Input Key Selector**, a navegação por teclado e controle nos menus e
o OnBack: [pausa-opcoes.md](pausa-opcoes.md).

## Peças de HUD
Barra de Atributo, Bússola, Mensagens, Números de Dano, Mira, Marcadores na Tela, Direção do Dano e Minimapa (Paleta > HUD) e o componente HUD Layout: [hud.md](hud.md).
