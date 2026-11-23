# Ícones, tooltips e o estilo Órbita dos grafos (Q5, plano 0.43, pedido de 17/11/2026)

Parte da identidade própria da engine (Parte 9.3 do plano de melhorias, itens e e i): ícones que dizem a função,
tooltip com nome + descrição + atalho em todo controle, e os grafos no estilo "Órbita". Cores sempre por token do
tema (`theme.h`; o teste `theme_literals` cobra).

## Ícones (`src/ui_icons.c/.h`)
- **Gramática**: grade de 16, traço de 1,5 px que acompanha a escala (`t = max(1, 1.5 * s / 16)`), pontas redondas,
  **duotom**: o traço na cor passada (`col`, a do texto) e os corpos (`body_rect`, `body_circle`, `body_poly`,
  `body_tri`) preenchidos a 20% (`g_fill`, alfa 51 × alfa do traço) na **cor da família** do ícone.
- **Famílias** (`ui_icon_family_token`): Blueprint ciano (`TK_ASSET_BLUEPRINT`), Material verde, Malha laranja,
  Textura, Animação rosa, Áudio violeta, Efeitos âmbar (luzes, partículas, neblina), UI azul, Dados cinza-azulado,
  Nível branco, Pasta; ferramentas `TK_ACCENT`; os da engine (Play, Simular, Compilar, Build, Empacotar, Projeto)
  `TK_ACCENT2` (Ignição) com a órbita de motivo; estados `TK_WARN`/`TK_ERROR`/`TK_OK`.
- **199 ícones** (eram 100): todo tipo de asset (`ui_asset_icon(AssetKind)`, usado pelo Content Browser e pelo
  `asset_viewer`), ferramentas (copiar, colar, Git, travar, filtro, ordenar, gravar, chave, ímã, pincel, camadas...),
  estados (compilando = `ui_spinner`, erro, aviso, não salvo, OK) e as famílias de nó (Pura, Macro, Fluxo, Cast; o
  Evento virou um raio "Ignição"). `ui_icon_name()` dá o nome (tabela gerada do enum, `_Static_assert` na contagem).
- **Menus**: os 107 `igMenuItem_Bool/BoolPtr/igBeginMenu` sem ícone viraram `ui_menu_item/_ptr/ui_begin_menu` com um
  ícone da função; os títulos da barra de menu (File, Edit...) ficam só texto. Resta `igMenuItem` em `ui_widget_draw.c`
  (UI do jogo, sem `ui_icons.c` no executável do jogo) e em `editor.c:843` (já desenha o ícone do ator antes).
- **Content Browser**: o tipo é um **selo** colorido no canto superior esquerdo da miniatura (o Git fica no direito),
  não a faixa embaixo como na Unreal; o ícone do bloco em traço de texto + preenchimento da família.
- **Folha de ícones**: `realengine --icon-sheet --bench 2 --shot icon_sheet.png` desenha todos (16, 24 e 32 px, nome,
  cor da família) numa janela por cima do editor; teste `icon_sheet` (gera `build/icon_sheet.png`).

## Tooltip padrão (`src/ui_tips.c/.h`)
- O cartão: **ícone** + **nome em negrito** (fonte Inter SemiBold, `g_font_bold`) + **atalho** numa tecla à direita;
  embaixo a descrição (largura 24 em); no editor, "F1: mais no Assistente". Aparece ao lado (embaixo, ou à direita de
  um menu) depois de 0,55 s parado, nunca por cima.
- As partes: `ui_tip_item(icon, name, desc, shortcut)` para o último item (copia só quando o mouse está em cima);
  `ui_icon_button`, `ui_dropdown_button`, `ui_menu_item(_ptr)` e `ui_begin_menu` já passam o ícone e o atalho. O nome
  vem do rótulo visível (sem `##id`, sem os espaços do ícone do menu; um "(Ctrl+O)" no fim vira o atalho); o atalho
  também é achado na descrição ("(Ctrl+S)", "(F7)", "(W)"). `ui_item_tip(desc)` substitui um `igSetItemTooltip` de
  texto simples mantendo o cartão.
- **Linha do Details**: `ui_tip_row(label)` (chamado por `ui_tip_last_item`, `label_cell`, `PL()`) guarda o rótulo e o
  retângulo da linha a cada quadro (só ponteiro); um campo `##id` sem descrição própria usa a da linha.
- **F1** (`editor.c`): abre o Assistente buscando `ui_tip_hovered_name()` (o controle sob o mouse).
- O jogo também liga `ui_tips.c` (Debug Viewer): sem `ui_tips_editor()` não há ícone nem a linha do F1.
- **Ids na tabela**: sem `##` ("tb_open"); as 8 entradas antigas com `##` ("##lvlblend", do Q3) nunca casavam:
  `ui_tip_find` agora aceita as duas formas.
- **Descrições novas**: `src/ui_tips_table_q5.c` (551, en e pt), num arquivo próprio para não colidir com quem
  acrescenta no fim de `ui_tips_table.c`; `ui_tips.c` junta as duas tabelas (`tables_join`, até 2040).

## Teste "nenhum controle sem tooltip" (`tools/tips_audit.py`, teste `tips_audit`)
Varre `src/*.c` atrás de toda chamada de widget com rótulo literal (botões, menus, caixas, campos, abas, combos,
`ui_icon_button`, os rótulos de linha `label_cell`/`row_*`) e exige o rótulo (ou o `##id`) na tabela, com a mesma
busca do editor. Um campo `##id` até 5 linhas depois de um rótulo de linha herda a descrição dela. Fora: rótulos com
`%` (dados), a UI do jogo (`ui_widget_draw.c`, `hud_widget.c`...) e linhas marcadas `tips: none (<motivo>)` (os
canvases: viewport, grafos, prévias, blocos que desenham o próprio nome). Hoje: 1903 rótulos, todos com descrição.
Ids genéricos usados em vários lugares com sentidos diferentes foram renomeados (`##var_container`, `##pin_bool`,
`##disp_param`, `##bb_key`, `##bb_type`, `##curve_val`).

## Estilo Órbita dos grafos (`src/graph_style.c/.h`)
- `GS_NODE_ORBIT` (nó) e `GS_LINK_GRADIENT` (fio) são o padrão (`GS_*_DEFAULT_STYLE`); os 5 estilos antigos ficam na
  galeria de Preferências do Editor > Estilo do Grafo ("Default" virou **Classic**). `editor.ini` grava
  `graph_node_style2`/`graph_link_style2`; um ini antigo com o primeiro estilo (o único padrão de antes) passa para o
  Órbita (`graph_set_styles_ini`), uma escolha diferente fica.
- **Nó** (`graph_node_frame`, caso `GS_NODE_ORBIT`): cartão de cantos `TM_RADIUS_NODE` (8 px), corpo `TK_BG_RAISED`,
  cabeçalho em degradê da cor da família (`header_grad`), linha de luz no topo, halo de 3 camadas no selecionado
  (`TK_ACCENT`) e no erro (`TK_ERROR`) + selo `!` (`graph_error_badge`).
- **Famílias** no Blueprint (`bp_editor.c`): Evento `TK_NODE_EVENT` com o raio, Função azul, Pura verde-água com o
  losango, **Latente** violeta com o relógio (`node_latent`: Delay, Retriggerable Delay, Timeline, Spawn assíncrono ou
  um pino de execução Completed/Finished), Macro, Fluxo, Cast, Variável na cor do tipo.
- **Pinos** (`graph_pin`): valor círculo, Array barra de três, Set anel duplo, Map dois círculos ligados (chave e
  valor), Class anel vazado, Soft Reference tracejado (pronto, sem tipo no Blueprint ainda), execução seta, delegate
  quadrado; preenchido quando ligado, vazado em `TK_BG_FIELD` quando não.
- **Fio** (`graph_wire2`): faixa única com 4 vértices por ponto (degradê da cor da saída para a da entrada, borda de
  1 px suave), sob uma linha escura que destaca o fio de qualquer fundo.
- **Fundo** (`graph_background`): pontos a cada 16 unidades (32, 64... quando apertariam), maiores a cada 128; acima
  do zoom 1 a grade de 128 aparece em linhas. Quads direto na draw list (máx. 60 mil). `gv_grid` (Behavior Tree,
  Sound Cue, Diálogo) e o editor de Material usam o mesmo.
- **Cartões dos outros grafos** (`graph_card`): Behavior Tree, Sound Cue (faixa por tipo + ícone), Diálogo (ícone por
  tipo de fala), Material (faixa da categoria + ícone de material, pinos `graph_pin`, fio em degradê saída→entrada) e o
  cabeçalho dos emissores de partículas. No estilo Clássico cada editor mantém o desenho de antes.

## Pendente / limites
- Sequencer: ícones dos menus, as dicas e a chave selecionada no acento com halo; as trilhas ainda não ganharam o visual de cartão.
- Campos de valor do Details e do Editor de Código: herdam o tema do Q1 (tokens de campo e foco); não houve mudança
  própria aqui além das dicas.
- O cartão padrão não cobre os `igSetItemTooltip` com conteúdo próprio (prévias de asset, valores ao vivo): esses
  continuam com o tooltip deles (o ImGui avisa `re_imgui_tooltip_begun` e a descrição não aparece por cima).

## Medidas (RTX 3060, 2560x1417, editor vazio, `--bench 5`)
- Antes (main 3447370, executável da `merge-lote1`), máquina livre: 501 fps, CPU 1,169 ms, GPU 1,976 ms.
- A/B intercalado (antes, depois, 3 pares, com a suíte de outro branch dividindo a GPU): antes 389/256/465 fps (CPU
  1,13/1,56/1,03 ms), depois 450/352/443 fps (CPU 1,30/1,12/1,35 ms): médias 370 × 415 fps e 1,24 × 1,25 ms de CPU,
  dentro do ruído. Sem custo mensurável.
- `budget_house` (Épico, fatia "Reconstruction and UI" da GPU, 3 pares intercalados): antes 0,634/0,627/0,665 ms,
  depois 0,634/0,641/0,653 ms (médias 0,642 × 0,643 ms; limite 0,70).
- O que custa: um ícone duotom desenha o corpo duas vezes (preenchimento + contorno); o tooltip só copia texto no
  quadro em que o item está sob o mouse; a linha do Details guarda um ponteiro por linha; os pontos do fundo do grafo
  são quads direto na draw list (~7 mil num grafo de 1440p em zoom 1).
- Cuidado ao medir: uma pasta de build antiga guarda `RE_VERSION_STRING` no cache (aqui era 0.24): o editor abre um
  aviso de versão e o `dock_targets` falha em tudo. `cmake -U RE_VERSION_STRING build` resolve.
