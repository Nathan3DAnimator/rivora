# Editor (etapa 1)

## O que foi feito
- **Build**: CMake (VS 2022, x64, C11, `/arch:AVX2`). `re_core` (ECS, memória, sistemas, Win32) separado do
  executável `realengine` (janela WIN32 com `main`; log em `%APPDATA%\RealEngine\realengine.log` quando não há stdout).
  `external/glfw` e `external/cimgui` copiados do Tlender (só o necessário: sem generator/docs/exemplos).
  O imgui do Tlender tem dois ganchos (faixas de número do Blender, expressões digitadas): o `cimconfig.h` da Engine
  os declara e `src/imgui_hooks.c` responde de forma neutra (comportamento do ImGui normal).
- **Vulkan 1.3** (`vkctx.c`, adaptado do Tlender): dynamic rendering (sem render pass/framebuffer),
  synchronization2 (`vkCmdPipelineBarrier2`, `vkQueueSubmit2`), **um timeline semaphore** contando os frames (sem
  fences), um command pool por frame em voo (reset do pool inteiro), timestamps de GPU por frame. Liga também
  descriptor indexing, buffer device address, multiDrawIndirect, BC (para o bindless/GPU-driven da etapa 2).
  Present: sem vsync por padrão (mailbox, senão immediate); `--vsync` ou Edit > Editor Preferences > VSync.
- **Win32** (`platform_win32.c`, `platform_files.c`, prefixo `re_`): UTF-8, diálogos nativos, threads, atômicos,
  relógio, `re_parallel_for`. Saíram o gancho de caneta/touch e os diálogos de tipos do Tlender.
- **Memória** (`mem.c`): reservas de endereço (`VirtualAlloc`) com commit conforme enche — arrays crescem no lugar,
  ponteiros estáveis, zero `malloc` no frame. Arena por frame (`g_frame_arena`, alinhada a 64) para listas da UI.
- **ECS** (`ecs.c`): handle de 32 bits (índice 20 + geração 12), arquétipos por máscara de componentes, uma coluna
  contígua por componente, remoção por swap com o último, `ecs_add/remove` move entre arquétipos, `ecs_clone`.
  **Limite: 64 componentes** (máscara `uint64_t`, `COMP_BIT`). O B1 (World Partition) chegou a 64 (09/11); um
  deslocamento por 64 é indefinido em C (o x64 trata como 0) e zerou as máscaras dos Blueprints e recusou os níveis
  salvos (`mask >> ncomp`). Agora `COMP_ALL` e um `_Static_assert(COMP_COUNT <= 64)`: o 65º componente precisa antes de
  máscaras de duas palavras (ou campos novos em componentes que já existem).
  Componentes: `Name`, `Transform` (cm, Z para cima, pitch/yaw/roll em graus como a Unreal), `Rotating Movement`.
  Sistema `system_rotating` (roda só no PIE).
- **Layout da Unreal**: menu File/Edit/Window/Tools/Build/Select/Actor/Help; toolbar (Save, Q/W/E/R, + Add,
  Play / Pause-Stop-Eject centralizados); viewport; Outliner (busca, lista em ordem estável de slot com
  `ImGuiListClipper`, seleção, renomear com duplo clique/F2, menu de contexto, apagar); Details (nome, + Add
  componente, Transform com campos X/Y/Z coloridos, Rotating Movement removível, info da entidade/arquétipo);
  divisórias arrastáveis; Content Drawer (Ctrl+Space, some ao perder o foco, lista a pasta `Content`); barra de status
  com FPS, ms do frame, CPU e GPU (cor: verde ≥ 350, amarelo ≥ 200).
- **Viewport provisório** (até o renderer): grid, eixos, atores como caixas em wireframe com eixos no selecionado,
  clique seleciona, botão direito + WASD/QE voa (roda = velocidade), botão do meio arrasta, roda aproxima, F foca,
  G modo jogo, F11 tela cheia.
- **PIE**: Alt+P entra (snapshot por memcpy de slots, free list, entidades e colunas), Esc sai (restaura; entidades
  criadas no play somem, apagadas voltam com o mesmo handle), F8 possuir/ejetar, Shift+F1 libera o mouse (clique no
  viewport recaptura), Pause pausa. Câmera do editor volta ao sair.
- **Testes** (`ctest -C Release`): `ecs` (handles, gerações, mudança de arquétipo, snapshot) e `editor_fps`
  (`realengine --bench 3 --min-fps 1000`: falha abaixo de 1000 fps). Tools > Measure Frame Time mede 3 s no editor.

## Medidas (RTX 3060, 2560x1417, Release, mailbox, 2026-10-30)
| O quê | Medida |
|---|---|
| Editor vazio (3 atores) | **~4500 fps**: 0,222 ms/frame, CPU 0,148 ms, GPU 0,145 ms (pior frame ~0,8–1,1 ms) |
| Idem com a camada de validação | ~3500 fps, nenhum aviso |
| Snapshot / restore do PIE (3 atores) | 0,057 ms / 0,001 ms |
| Criar 1M entidades (Transform + Rotating) | 21 ms |
| `system_rotating` sobre 1M | 3,5 ms/frame (limite de banda: ~84 MB lidos+escritos) |
| Snapshot / restore de 1M (61 MB) | 8,7 ms / 4,2 ms |

## Atualização: layout completo, gizmos, Desfazer, níveis (2026-10-30)
- **Atores da Unreal** (toolbar "+ Add", Actor > Add, "+" do Outliner, Content Drawer): Empty Actor, Player Start,
  Static Mesh (Cube, Sphere, Cylinder, Cone, Plane), Directional/Point/Spot/Rect/Sky Light, Camera Actor. Renderer em
  [render.md](render.md). PIE nasce no Player Start (altura dos olhos); uma câmera com "Auto Activate for Player"
  vira a vista do jogador.
- **Gizmo** (W mover com eixos e planos, E girar com anéis, R escalar com eixos e centro uniforme; Espaço alterna,
  Q seleciona), em espaço de mundo como o padrão da Unreal, sempre por cima. **Snapping** na toolbar do viewport
  (grade 1–500 cm, ângulo 5–90°, escala 0,0625–1). Clique seleciona por raio contra a caixa orientada da malha;
  ícones de luzes/câmeras/player start por distância na tela. End: encosta no chão. H: esconde no editor.
- **Toolbar do viewport** (como a da Unreal 5): menu (Game View, Stats, exposição, FOV), Perspective, modo de
  visualização (Unreal + Blender, ver render.md), Show (grade, ícones, stats), ferramentas, snaps, velocidade da
  câmera.
- **Outliner**: olho (esconder), ícone por tipo, tipo da Unreal ("Static Mesh Actor", "Point Light"...).
  **Details**: cabeçalho com ícone, nome e "+ Add"; seções Transform (com "reset" amarelo e Mobility
  Static/Stationary/Movable), Static Mesh, Materials, Light (campos por tipo), Camera, Rotating Movement, Rendering.
- **Desfazer/Refazer** (Ctrl+Z/Y, 100 passos): cada passo é o mundo inteiro serializado (`ecs_serialize`, poucos KB
  num nível de editor); um arrasto ou uma edição de campo vira um passo só, com os valores de antes.
- **Níveis** `.rlevel` (File > New/Open/Save/Save As, Ctrl+N/O/S/Shift+S; padrão em `Content/Maps`): cabeçalho com
  a câmera do editor + o mesmo blob, gravado em `.tmp` e trocado (nunca meio arquivo). Título com `*` quando há
  mudança. Blob versionado com os tamanhos dos componentes: mudou o layout, sobe a versão e migra.
- **Output Log** (barra de status), **Settings > Engine Scalability** (Low/Medium/High/Epic: MSAA 1/2/4/8; GI e
  sombras entram nos presets quando existirem), VSync.
- Teste `editor_fps` agora com o nível padrão renderizado: falha abaixo de 350 fps (o ideal do lema). Medida:
  ~1500 fps (0,67 ms).

## Ícones (2026-10-30)
Todos desenhados com a draw list do ImGui (`ui_icons.c`, grade de 16 unidades): nítidos em qualquer DPI, sem fonte
nem textura, alguns vértices cada.
- **Modo de visualização** (botão e menu, como a Unreal): uma esfera sombreada do jeito que cada modo mostra a cena:
  Iluminado (esfera iluminada com brilho), Sem Luz (disco chapado), Aramado (meridianos), Só Iluminação (esfera
  cinza e a luz), Normal do Mundo (fatias RGB), Sólido (cinza fosco), Sólido Aleatório (fatias coloridas), Textura
  (xadrez), Rugosidade (meia brilhante, meia fosca), Metálico (cromo), Path Tracing (raio quicando), Só GI (esfera
  acesa pelo chão). `ICON_VM_LIT + view_mode`.
- **Barra do viewport**: Perspectiva, modo de visualização e Mostrar viraram botões com ícone, nome e ▾
  (`ui_dropdown_button`). Perspectiva abre um menu de verdade (antes não fazia nada), com o campo de visão.
- **Menus** (Arquivo, Editar, Janela, Ferramentas, Compilar, Selecionar, Ator, Ajuda, opções do viewport, Mostrar):
  ícone em cada item (`ui_menu_item`, `ui_menu_item_ptr`, `ui_begin_menu`: o rótulo ganha espaços à esquerda e o
  ícone é desenhado nesse espaço; cinza quando o item está desabilitado).
- Custo: dentro do ruído (editor com GI desligada 1052 fps, antes 1067–1076).
## Gizmos (2026-10-30, copiados e adaptados do Tlender: `manip.c` / `ui.c`)
- Alças do tamanho da tela em volta do pivô (100 px por unidade): **Mover** setas, quadrados de plano e a bola do
  centro (livre no plano da vista); **Girar** a metade da frente de cada anel, o anel de fora (em volta do eixo da
  vista) e a pizza do ângulo varrido; **Escalar** linhas com caixas, quadrados de plano, centro e anel de fora
  (uniforme). Alça clareia sob o mouse; somem quando o eixo aponta para a câmera.
- **Bug corrigido**: escalar no X escalava no Y — as alças apontavam nos eixos do mundo e mudavam a escala local.
  A escala agora é sempre local (como a Unreal). **World / Local** (globo / cubo na barra do viewport, Ctrl+`) para
  mover e girar. Durante o arraste o gizmo é o do início (pivô e eixos fixos).

## Seleção múltipla (2026-10-30)
- `ed->sel` (até 262 144) + `sel_mark` por slot; `ed->selected` é o primário (o último: Details e pivô do gizmo).
  Código antigo que só muda `selected` vira seleção de um (`editor_sync_selection` a cada frame).
- Viewport: clique seleciona (Shift adiciona, Ctrl alterna); **arrastar no vazio = caixa** (pivôs dentro; Shift
  adiciona, Ctrl remove). Outliner: Ctrl+clique, Shift+clique (intervalo), arrastar sobre as linhas; linhas azuis.
- Atalhos: **Ctrl+A** tudo, **Ctrl+I** inverte, **Esc** nada, **Ctrl+C/X/V** copiar/recortar/colar (no mesmo
  lugar, como a Unreal; nomes novos), **Ctrl+D** duplica, **Shift+D** duplica e a cópia segue o mouse no chão
  (clique esquerdo posiciona, direito/Esc cancela), **Del** apaga todos. O gizmo move/gira/escala o grupo em volta
  do pivô do primário.
- Contorno: o mask tem um id por objeto e profundidade própria (o mais perto vence): cada selecionado tem o seu
  contorno, inclusive dentro da silhueta de outro (antes um chão selecionado escondia o contorno do resto).

## O que falta
- Details editando vários atores de uma vez; câmeras ortográficas (Top/Front/Side).
- Projeto/pasta de conteúdo de verdade (hoje `Content` ao lado do executável).

## UI animada (2026-10-31)
No imgui copiado (`external/cimgui/imgui`, trechos marcados `[Real Engine]`), para valer em toda janela sem mexer nos
editores:
- **Fade ao abrir**: `Begin()` empilha um alfa que vai de 0 a 1 (smoothstep) — janelas 0,14 s, popups/menus/tooltips
  0,09 s; janelas filhas herdam o do pai; `End()` desempilha. **Nunca chega a 0** (mínimo 0,001, abaixo de 1/255):
  o ImGui pula a janela com alfa 0, e o popup de um combo pulado dispara `IM_ASSERT(0)` em `BeginComboPopup` — os
  crashes da 0.9 (2º combo aberto em qualquer janela: Blueprint, Anim Graph, IA). O fade começa no `Appearing` do
  próprio frame (depois do docking), não no do frame anterior. Teste `ui_popups` (ImGui sem janela: combo e popup
  reabertos 3 vezes, visíveis em todo frame, nenhum assert).
- **Só aparece pronta** (sugestão #28, 2026-11-04): uma janela (não popup/menu/tooltip) fica invisível (alfa 0,001) nos
  2 primeiros quadros (as abas acopladas assentam) e enquanto os quadros forem engasgo de carga (> 50 ms: assets, prévias,
  materiais assando), no máximo 1 s; só então começa o fade. Uma aba acoplada nunca aparece antes da janela que a hospeda
  (`FadeInStart` = máximo dos dois): o editor de Blueprint aparece inteiro de uma vez, sem os botões antes do resto.
- **Fade ao fechar** pelo X: o botão marca `FadeOutStart`; a janela fica 0,12 s sem entrada (`NoInputs`) enquanto o alfa
  cai, e só então `*p_open` vira false. (Fechar pelo Esc do código do editor continua imediato.)
- **Listas abrindo**: o conteúdo de um nó de árvore aberto há menos de 0,18 s entra com o alfa subindo
  (`TreeFadeDepthMask` por profundidade; `TreePop` desempilha). Outliner, árvore do esqueleto, Content Browser.
- **Hover suave**: botões, setas, fechar, imagem, radio e cabeçalhos de árvore passam da cor normal à de hover em 0,12 s
  (`RE_HoverColor`, pelo `HoveredIdTimer`).
- **Ícones animados** (`ui_icons.c`): `ui_spinner` (arco girando que "respira": bake de luz na barra de status,
  miniaturas carregando), `ui_pulse_dot` (ponto com anel crescendo: alterações não salvas em âmbar, Play em verde),
  `ui_icon_anim` (gira / pulsa / "pop" com overshoot / quica qualquer ícone transformando seus vértices: a mensagem nova
  da barra de status entra com o ícone saltando e some com fade nos últimos 0,6 s).
- **Tema**: janelas flutuantes e popups com cantos de 6 px, controles 4 px, barra de rolagem arredondada e mais fina (os
  painéis fixos continuam retos: empilham rounding 0).
- **Custo**: um PushStyleVar por janela só durante o fade; os ícones animados são poucos vértices; sem diferença
  mensurável no editor (628 fps com gaveta e visualizador abertos).

## Bug corrigido: regiões de cópia sobrepostas (2026-10-31)
`scene_prepare` (render.c) podia mandar o mesmo slot duas vezes num quadro (o log de mudanças e o conjunto dinâmico),
gerando regiões sobrepostas no mesmo `vkCmdCopyBuffer` (VUID-vkCmdCopyBuffer-pRegions-12482, intermitente no teste
`ai`). Agora cada slot guarda o quadro e o índice em que foi preparado (`sent_frame`/`sent_at`): o segundo envio
sobrescreve o primeiro. Também: `weather_shutdown` destruía 5 das 7 imagens (as nuvens na tela) e o `brand_shutdown`
nunca era chamado (texturas da marca vazavam ao sair).

## Docking e janelas em outros monitores (2026-10-31)
- O Dear ImGui vendorizado passou para o ramo **docking 1.92.9** (cimgui tag `1.92.9dock`, as fontes oficiais), com os
  trechos `[Real Engine]` reaplicados (fade de janelas/popups, hover animado, árvores; um trecho refeito à mão no
  CollapseButton, que mudou no ramo docking). `cimconfig.h` continua o nosso (asserts → relatório de crash, ganchos dos
  campos).
- `ImGuiConfigFlags_DockingEnable | ViewportsEnable` no editor: qualquer janela (editor de Blueprint, materiais, IA,
  visualizadores) arrastada para fora vira uma janela do sistema, em outro monitor; o backend Vulkan cria a swapchain
  dela no mesmo formato (cores iguais). O retângulo da cena desconta a posição da janela principal (com multi-viewport as
  coordenadas do ImGui são as da área de trabalho).
- **Editor de Blueprint / Anim Blueprint / Widget Blueprint**: as abas (Viewport ou Designer, AnimGraph, Event Graph,
  funções) viraram janelas num DockSpace do editor: fechar (x), reordenar, dividir lado a lado, arrastar para fora
  (flutua, vai para outro monitor) e de volta. Cada janela de grafo desenha o seu grafo; os painéis esquerdo/direito
  seguem o que tem foco. O menu das abas (reabrir as fechadas) foi para a barra de ferramentas.
- Medido: editor vazio 451 fps (CPU 0,79 ms) com docking + multi-viewport ligados; sem custo visível.

## Mais perto da Unreal 5 (2026-11-01)
- **Posicionar Atores** (Janela > Posicionar Atores, ou Adicionar > Painel Posicionar Atores): o painel da esquerda da
  Unreal, por categoria (Básico, Luzes, Formas, Efeitos Visuais, Volumes, Cinemática, Prototipagem, Mundo) com busca;
  clique coloca na frente da câmera, formas arrastam para o viewport. `--show place` para fotos.
- **Arquivo**: Salvar Tudo (Ctrl+Shift+S, como na Unreal; antes era Salvar Como), Abrir Asset (Ctrl+P: busca por nome
  em todo o Content), Importar para o Nível. O Salvar Tudo do Content Browser passou a funcionar.
- **Compilar**: Compilar Todos os Níveis, Compilar Só a Iluminação e Qualidade da Iluminação (Prévia / Produção).
- **Ator > Visibilidade**: Ocultar Selecionados (H), Mostrar Todos (Ctrl+H).
- **Barra**: seletor de **Modos** (Seleção / Paisagem / Folhagem, Shift+1 / 2 / 3: o modo Paisagem seleciona a paisagem com o
  pincel ligado) e **Plataformas** (Windows > Empacotar Projeto, Configurações de Empacotamento) depois do Play.

## UI enxuta, descrições em tudo e editores como os da Unreal (2026-11-01)
- **Tema** (`apply_theme`, main.c): Segoe UI 15 px (antes 16), espaçamentos menores (FramePadding 6,3; ItemSpacing 6,4),
  cantos de 2–3 px, bordas finas, abas com a linha azul da Unreal 5 em cima da selecionada (`TabBarOverlineSize`),
  barras de rolagem sem fundo, separadores e alças de redimensionar em azul só no hover. Dicas próprias dos widgets
  esperam 0,5 s (antes 0,15 s).
- **Descrições** (`ui_tips.c`, `ui_tips_table.c`): todo botão, item de menu, caixa, campo e aba diz o que faz. O ImGui
  vendorizado ganhou `RE_LABEL` (trechos `[Real Engine]`): cada widget com rótulo o entrega ao `ItemAdd`, que, com o mouse
  em cima, chama `re_imgui_hover_label` (`imgui_hooks.c` guarda em `g_ui_hover`). `ui_tips_frame()` (fim do quadro)
  procura o texto pelo msgid em inglês (rótulo traduzido → `i18n_msgid`; botão só de ícone → o `##id`) e, depois de
  0,55 s parado, mostra **ao lado, nunca por cima**: embaixo do controle (em cima perto da borda de baixo), à direita do
  menu (à esquerda perto da borda). As dicas próprias (`igSetItemTooltip`) foram para o mesmo lugar e quebram linha
  (30 em). Nome desenhado como texto (linha dos Details): `ui_tip_last_item`. Novo widget = uma linha `T(...)` na tabela
  (inglês e português). Teste `ui_tips`: tabela sem repetição e com as duas línguas, busca, e ImGui sem janela (a
  descrição não aparece na hora, aparece depois, embaixo do botão, e não aparece sobre a dica própria).
- **Details como a Unreal**: `PL(TR("Intensity"))` desenha o nome numa coluna à esquerda (40%, cortado nela, com a
  descrição) e o campo ocupa a direita; ~260 campos dos editores de Blueprint, Anim, Motion Matching, IA, Material,
  Input e Widget. Campo depois de `SameLine` ou dentro de tabela continua no layout do ImGui.
- **Structure** (Unreal): abas Structure (Description, + Add Variable, linhas nome / tipo com o ponto da cor do pino /
  contêiner, ▲▼ e lixeira; aberta, a Tooltip) e **Default Values** — os padrões agora valem: a variável de uma estrutura
  começa com eles (`bp_vm.c`). **Enumeration**: Description, + Add Enumerator, tabela # / Display Name / Description /
  ▲▼ / lixeira (a ordem é o valor). **Data Asset**: tabela Name / Type / Value. `description` e `entry_desc` no JSON
  (opcionais: arquivos antigos abrem iguais).
- **Blueprint Interface**: painel "My Blueprint", sem a frase sobre o grafo, janela grande como a da classe. Bug
  corrigido: a 1ª função de uma interface (e de uma biblioteca de macros) não mostrava as entradas e saídas nos nós
  (a busca da assinatura começava no grafo 1, que só nas classes é o Event Graph).
- Traduções: ~100 textos que estavam em inglês no português (IA, animação, Enhanced Input, materiais).
- Medida (RTX 3060, 2560x1417, editor com o nível padrão): 463 fps, CPU 0,72 ms (antes ~420 fps, CPU ~1,0 ms na barra
  de status: dentro do ruído). A descrição custa uma cópia do rótulo quando o mouse muda de controle e uma busca em hash.
- Teste `blueprints` falhou 2 vezes seguidas (IA_Move: escala z) logo depois de várias aberturas manuais do editor e
  passou 6 vezes depois, com e sem estas mudanças: intermitente, causa não encontrada.

## Grafos, Blend Space, triggers, câmera e jogos de demonstração (2026-11-01)
- **Reroute em todos os grafos**: Blueprint (`BPK_REROUTE`, "Add Reroute Node": pino curinga que aceita qualquer tipo, até
  execução; o compilador junta os fios na cópia do grafo, junto com as macros — `expand_macros`), Anim Graph (`AN_REROUTE`,
  categoria Utilities, um nó pequeno) e materiais (`MAT_NODE_REROUTE` = o `NodeReroute` do Blender, seguido por
  `follow_link` na compilação, carrega até shader).
- **Seleção por caixa e vários nós**: Anim Graph (nós), máquina de estados (estados) e materiais (Ctrl alterna, Shift
  adiciona, arrastar no vazio = caixa; movem e apagam juntos). O Blueprint já tinha.
- **Transições arrastando** (Unreal): na máquina de estados a borda do estado acende; arrastar dela até outro estado cria
  a transição (também do Any State).
- **Blend Space Editor** (janela própria, como a da Unreal): duplo clique no nó/estado de Blend Space (ou "Open Blend
  Space Editor"): Axis Settings (variável, mínimo, máximo, divisões da grade; `axis[6]` no JSON, opcional), as amostras
  numa grade grande (arrastar com encaixe na grade, Alt livre, botão direito apaga), Asset Browser à direita (arrastar
  animações para a grade, duplo clique adiciona no ponto de prévia), o ponto de prévia (mouse; Shift segura) com o peso
  de cada amostra. `--blend-space <ABP>` abre (fotos).
- **Triggers** (Unreal): Trigger Box / Sphere / Capsule em Posicionar Atores > Volumes (contorno laranja, seção Shape nos
  Details). Eventos `Event ActorBeginOverlap`, `Event ActorOverlapping` (todo quadro enquanto encosta) e
  `Event ActorEndOverlap` com Other Actor, e `Is Overlapping Actor`. Sensores: Box/Sphere/Capsule Collision com Generate
  Overlap Events (do ator ou de um Trigger); corpos: Character, rigid body, projétil, formas e malhas de Blueprint,
  Skeletal Mesh, IA (as malhas estáticas do nível não, como o padrão da Unreal). Pares de atores do quadro contra os do
  anterior; os dois atores recebem; o evento com um nome nos Details é o do Level Blueprint para aquele ator. Nada roda
  sem sensor. Teste: `--bp-test` (o personagem atravessa um Trigger Box: Begin e End).
- **Câmera**: a Camera Component do Blueprint ganhou o que a Camera Actor tem (Auto Activate, Camera Motion Blur) e as
  duas ganharam **Post Process** próprio (14 ajustes: exposição, bloom, motion blur, profundidade de campo, vinheta,
  cor...), aplicado por cima dos volumes enquanto é a câmera do jogador (`CCamera` cresceu no fim: níveis antigos abrem).
- **Jogos de demonstração** (Project Browser > Jogos; `demo_games.inc`): RPG (vila, moedas, mercador que troca poção por
  ouro com E, bandidos com IA, espada, poções), Terror (casa escura à noite, neblina, exposição travada, lanterna F,
  chaves e portas trancadas, monstro com IA que caça pela visão e pelo barulho, saída) e Luta (ringue, rival com IA, soco,
  chute, defesa, vida, nocaute). A lógica fica no Event Graph do personagem (eventos de sobreposição + Get Display Name).
  Medidas (RTX 3060, 1440p, Alto): RPG ~200 fps, Luta ~213, Terror ~170 (GPU 5,8 ms: neblina volumétrica e a sombra da
  lanterna em salas fechadas — abaixo do lema; caminho: escalabilidade Média no jogo). Testes `demo_rpg`, `demo_horror`,
  `demo_fighting`.
- **Atualização (2026-11-01, "tudo que falta")**: Event Dispatchers e Construction Script (ver blueprints.md); modo
  **Blackboard** no editor de Behavior Tree (botões Behavior Tree / Blackboard: chaves, tipo, onde a árvore usa cada
  uma, renomear acompanha os nós; `--bt-blackboard`); **janela da regra de transição** (duplo clique na seta ou "Open
  Transition Rule": as condições como nós → AND → Result: Can Enter Transition, e as configurações de blend;
  `--transition-rule <ABP>`); **HUD** dos jogos de demonstração (Widget Blueprint no canto inferior esquerdo: barras de
  vida, ouro, poções, chaves, vida do rival; atualizado por um timer de 0,1 s). O terror usa escalabilidade Média no
  jogo (~300 fps; em Alto a GI por raios em salas fechadas custa ~2,5 ms e fica em ~160 fps). Seletores de variável,
  animação, osso e chave com o nome à esquerda.

## Prévias de assets na GPU, modos de visualização (2026-11-03, relatório #53, sugestões #39, #63, #71)
Os viewports dos editores de asset deixaram de ser desenhados na CPU (`bp_preview.c`, cor por vértice, sem textura) e
passaram a ser **renderizados pela GPU** (`src/render_preview.c`, `shaders/preview.*`), cada um numa imagem própria:
- As malhas saem dos mesmos buffers de vértices/índices da cena, com os **materiais assados** (as texturas que o passe
  principal lê: base, normal/rugosidade/metálico, emissão, Masked), 4x MSAA, um estúdio (key light com **mapa de sombra**
  2048 PCF, fill, rim, ambiente com reflexo — split sum de Karis) e o chão com a grade, que some no fundo. ACES igual ao antigo.
- **Modos** (botão no canto como o "Lit" da Unreal): Iluminado, Sem Iluminação, Wireframe, Iluminado com Wireframe,
  Só Iluminação, Normal do Mundo, Rugosidade, Metálico. Os overlays (ossos, gizmo, ajudantes) continuam por cima no ImGui.
- Onde: Skeletal Mesh / Animação / Montage, Motion Matching, **Static Mesh** (com "Material da Prévia": o material do
  slot achado pelo nome `SM_<objeto>_<material>`, ou escolhido), **Viewport do Blueprint**, **Viewport do Anim Blueprint**
  (o Skeletal Mesh tocado pelo grafo, `anim_preview_pose`, com o *Anim Preview Editor*: os valores das variáveis Float/
  Integer/Boolean, Pausar, Recomeçar e o estado atual), **prévia no canto do AnimGraph** e **prévia 3D no editor de Blend
  Space** (amostras sincronizadas pela fase e misturadas pelos pesos no ponto de prévia). Tudo em tempo real (materiais
  animados pelo tempo, animações tocando).
- Pedido enquanto a UI é montada (`render_preview`: desenhos copiados, sem malloc), gravado por `render_preview_record`
  depois da cena (main.c). Pipelines criados na primeira prévia (o jogo empacotado nunca paga). Alvos por janela (até 8,
  tamanho arredondado a 64 px: redimensionar não recria a cada quadro); imagens antigas liberadas alguns quadros depois.
  As miniaturas do Content Browser e do Project Browser continuam na CPU. `REALENGINE_PREVIEW_CPU=1` volta ao desenho antigo
  (medidas de antes).

Medidas (RTX 3060, 2560x1417, Release, cópia do projeto FirstPerson):
| Situação | FPS | CPU | GPU | Prévia (timestamps) |
|---|---|---|---|---|
| Editor sem janela de asset | 293 | 1,80 ms | 3,09 ms | — |
| Skeletal Mesh "Delight" (243 partes, texturas) — antes (CPU) | 7-12 | 83-86 ms | — | — |
| Skeletal Mesh "Delight" — GPU | 261 | 2,64 ms | 3,44 ms | 0,34 ms |
| Viewport do Anim Blueprint (Mannequin, 28 partes) | — | — | — | 0,16 ms |
| Editor de Blend Space (prévia de 500x200) | 258 | 2,31 ms | 3,27 ms | 0,07 ms |
- Os picos de ~280 ms ao abrir um Blueprint ou um personagem são a carga/bake dos materiais e animações (uma vez), iguais
  com a prévia antiga.

## `--scalability` não fica salvo (2026-11-04)
A escalabilidade da linha de comando valia também para o `editor.ini` (salvo ao fechar): depois de um teste com
`--scalability 3` (os de flicker do RPG), todos os seguintes rodavam no Épico, MSAA 8x (multidão 126 → 60 fps só dentro
da suíte). Agora `Editor.saved_scalability` guarda a preferência de antes e é ela que vai para o arquivo.

## Posicionar Atores mais claro (sugestão #41, 2026-11-04)
Relato: o painel "bem feio", com mistura visual. Agora como o Place Actors da Unreal 5: busca no topo; as categorias numa
coluna própria à esquerda (ícone + nome; só ícones quando o painel é estreito, com o nome na dica), a atual com a barra
laranja; à direita o título da categoria com uma linha embaixo e as classes como **cartões** (caixa arredondada, o ícone
num quadrado escuro, o nome e o tipo — Luz, Malha Estática, Volume, Protótipo, Ator — em cinza), mais claros sob o
mouse. **Usados Recentemente** (os últimos 8 posicionados) primeiro e **Todas as Classes** no fim (sem repetir uma classe
de duas categorias). Buscando, todas as que batem agrupadas sob o título da categoria. Folhagem entrou em Mundo.
Custo: o mesmo do painel antigo (algumas dezenas de retângulos e textos; nada por quadro fora dele).

## Vistas ortográficas no viewport principal (sugestão #66, 2026-11-04)
O viewport principal tem **Perspectiva, Topo, Baixo, Esquerda, Direita, Frente e Trás** (menu da câmera na barra do
viewport e os atalhos da Unreal: Alt+G, Alt+J, Alt+H, Alt+K; com Shift o lado oposto). Ao trocar, a vista ortográfica olha
para onde a câmera olhava (o ponto do chão); botão direito ou do meio desloca, a roda dá zoom no ponto do mouse, F centra
no selecionado, "Meia Altura Ortográfica" no menu. `--ortho top|front|...` para capturas e testes.
- **Renderer** (`RenderView.ortho_half`, `render.c`): matriz ortográfica com Z reverso linear de 5 km atrás a 5 km na
  frente do ponto olhado (que vira `camera_pos`: GI, sombras e luzes ficam em volta dele). Planos do cull com o lado de
  longe; LOD por pixels por cm (`cull_params.x`) e Hi-Z com a profundidade linear (`cull.comp`); vetor de visão
  constante (`mesh.frag`); `view_origin`/`view_ray` em `common.glsl`; path tracing com um raio por pixel paralelo.
  Sem neblina, nuvens, perspectiva aérea, profundidade de campo nem motion blur na vista ortográfica (como a da Unreal).
- **Fundo**: cinza neutro com o brilho de um cartão cinza 18% sob o sol e o céu: a exposição automática fica a mesma da
  perspectiva mesmo com o chão de lado (com um cinza fixo ela estourava os objetos).
- **Grade** no plano voltado para a vista (XY no Topo, XZ na Frente, YZ no Lado), eixos coloridos, sem fade; linhas de 1 m
  somem ao afastar e as de 10 m/100 m assumem.
- **Editor**: raios do mouse com origem no pixel (`View.ro`) para seleção, gizmo, pincéis de Landscape/Foliage e soltar
  assets; gizmo do mesmo tamanho em qualquer zoom. A base vem da própria câmera (pitch -90 / yaw 0 / 90): uma base
  espelhada invertia o winding e mostrava o lado de dentro das malhas.
- **Medidas** (RTX 3060, 2560x1417, nível padrão): Perspectiva 640-700 fps; Frente 667-713 (GPU 1,03 ms); Esquerda 729;
  Topo 402-508 (GPU 1,9 ms: o chão iluminado ocupa a tela toda, sem céu). Testes `editor_ortho` (Topo) e
  `editor_ortho_front` com `--validation`, > 250 fps.

## Combos com busca e prévia (sugestão #60, 2026-11-04)
- **Busca em todo combo** (trecho `[Real Engine]` em `imgui_widgets.cpp`, sem mexer nos editores): um combo cuja lista teve
  8 linhas ou mais ganha um campo de busca no topo (com o foco ao abrir; Enter escolhe a primeira que passa) e as linhas
  num filho embaixo (o campo fica quando a lista rola). `Selectable()` pula as linhas cujo rótulo não contém o texto (sem
  diferenciar maiúsculas, só a parte visível) e conta todas (a escolha do quadro seguinte); o texto zera ao reabrir. O
  "Buscar..." vem traduzido (`g_re_search_hint`, posto pelo editor a cada quadro). Teste `ui_popups`: campo depois do
  primeiro quadro, filtro "UBE" → Cube e Tube, Enter escolhe Cube e fecha, reaberto limpo.
- **Miniatura em cada linha de asset** (`asset_selectable` em `asset_viewer.c`): a miniatura do Content Browser à
  esquerda (malhas, Skeletal Meshes, animações, materiais, texturas; as formas básicas por `#mesh/<id>`) e, sob o
  mouse, uma prévia grande ao lado; a animação toca (`asset_thumb_hover`). Nos combos de Static Mesh, Material,
  Skeletal Mesh, animação e Foliage do Details, do editor de animação/Anim Graph, dos nós de Blueprint e do Static Mesh
  Editor. Custo: as miniaturas já existentes (64 KB cada, feitas uma vez); só o item sob o mouse anima.

## Menu do botão direito nos gráficos (2026-11-07, pedido do usuário)
Pedido: "as funções do botão direito referentes ao gráfico clicado, só as que ele suporta, com histórico das 2 últimas
e 2 recomendadas que conectam com o que ele está fazendo, em todos os gráficos".
- `graph_menu.c/h` (comum a todos): por tipo de gráfico (`GM_BP_EVENT`, `GM_BP_FUNCTION`, `GM_BP_MACRO`,
  `GM_BP_CONSTRUCTION`, `GM_ANIM_GRAPH`, `GM_MATERIAL`, `GM_BEHAVIOR_TREE`, `GM_SOUND_CUE`, `GM_PARTICLES`) as
  últimas usadas e os pares aprendidos ("depois de A, o usuário pôs B": 8 pontos por vez) somados a uma tabela de
  pares comuns (os hábitos da Unreal: BeginPlay → Print String / Delay, Get Actor Location → Set Actor Location,
  Image Texture → Principled BSDF, Sequence → Move To...). Guardado em `%APPDATA%\RealEngine\graph_menu.txt`
  (`REALENGINE_GRAPH_MENU_FILE` troca o arquivo: os testes).
- O contexto: o nó do fio puxado, senão o selecionado, senão o último colocado nesse gráfico.
- Blueprints: cada tipo de nó classificado uma vez (um nó feito à parte): evento (saída de exec sem entrada) e latente
  (a dica diz "latent"). Função e Construction Script: sem eventos nem latentes; macro: sem eventos; nós do Animation
  Blueprint só nele. Puxando um fio, Recentes e Recomendados só com o que encaixa. A seção antiga "Usados
  Recentemente" (todos) virou as 2 de cima.
- AnimGraph (o nó do fio / o selecionado), Material Editor (o selecionado), Behavior Tree (o pai do novo nó; na raiz
  só composites) e Sound Cue (o selecionado) com as mesmas duas seções no topo.
- Testes: `graph_menu` (aprender, ordenar, a tabela, salvar e ler; todo gráfico tem sugestões iniciais) e
  `bp_graph_menu` (BP_TestActor: Event Graph 534 ações com eventos e Delay, depois de BeginPlay recomenda Print String
  e Delay e coloca a primeira; numa função 495, sem eventos nem latentes).
- Custo: o menu aberto calcula as recomendações a cada quadro (alguns milhares de comparações de texto, < 0,05 ms).

## Ícones dinâmicos (2026-11-08, pedido do usuário)
- O ícone é o próprio ator: malha importada → miniatura do modelo; Skeletal Mesh → o modelo na pose de repouso (T pose),
  no Organizador e no viewport (cartão arredondado, desenhado mesmo "atrás" da própria malha). `editor_actor_thumb`
  usa as miniaturas do Content Browser (`asset_thumb`, rasterizadas uma vez, 128 px, em cache).
- Atores de malha importada passaram a ter ícone no viewport (`icons_update` aceita `COMP_STATIC_MESH` fora das formas
  básicas); as formas básicas continuam sem ícone.
- **Mostrar Ícone** por ator (`TRANSFORM_NO_ICON`, bit 4 do `CTransform.flags`, salvo com o nível): Detalhes > Transform
  e Ator > Visibilidade > Esconder/Mostrar Ícones dos Selecionados; desligado no dono esconde os das partes.

## Controle de versão com Git (B14a, 2026-11-09)
`src/source_control.c/.h`: o Git do PC (como o plugin de Git da Unreal), por `re_run_capture` (CreatePipe, sem janela).
- `sc_init(projeto)`: `git rev-parse --show-toplevel`; sem Git ou fora de repositório tudo responde "desligado".
- `git status --porcelain -z` numa thread a cada 5 s (`sc_refresh`); `sc_state("/Content/...")` dá o estado do asset e o
  Content Browser desenha a bolinha (verde novo, amarela modificado, vermelha apagado).
- Arquivo > Controle de Versão: Enviar Conteúdo (lista com caixas, mensagem; `git add` + `git commit -F` com a mensagem num
  arquivo temporário, numa thread), Atualizar Status, Rastrear Binários com Git LFS (`git lfs track` das extensões da
  engine no `.gitattributes`).
- Teste `source_control` (`--sc-test`): repositório temporário com autor próprio (não usa o git config do usuário): novo,
  enviado, alterado, log.
- (sobras, 09/11) Pull (`--ff-only`), Push, Lock/Unlock (`git lfs lock`, botão direito no asset), Diff por arquivo na janela
  (`git diff`, leitura) por um executor genérico na thread; o teste ganhou um remoto bare: diff, pull de um commit de outro
  clone e push conferido no remoto.
- Falta: diff visual de binários (meshes, texturas), mesclar no pull, a lista de locks dos outros nos tiles.

## Plugins (B14c, 2026-11-09)
DLLs nativas como os plugins da Unreal. Público: `src/plugin_api.h` (só C; copiado para `SDK/` ao lado do editor).
- Pasta `Plugins/<Nome>/` com `plugin.json` (FriendlyName, VersionName, Description, Category, CreatedBy,
  EnabledByDefault, Module) e `<Module>.dll`; no projeto ou ao lado da engine (o do projeto ganha se tiver o mesmo nome).
  Ativo por projeto em `Config/Plugins.ini`; mudança vale na próxima abertura. `REALENGINE_PLUGINS_ENABLE=A,B` força (testes).
- `re_plugin_init(api)`: `add_function` (nó de Blueprint puro ou chamável, até 8 entradas e 4 saídas de
  float/int/bool/vector/rotator/color), `add_tick`, `add_menu_item` (Ferramentas > Plugins), `log`, `status`,
  `time_seconds`, `actor_count`, `in_editor`. A tabela só cresce (version/size).
- Blueprints: `BPK_PLUGIN_PURE`/`BPK_PLUGIN_CALL`, o texto do nó é o nome da função; os pinos vêm de `plugin_fn_spec` no
  `build_pins` (plugin ausente: o nó guarda os pinos que tinha). Compila com todas as entradas em `OP_NPARAM` e o nome como
  string; a VM resolve o nome por um cache de 64 entradas (endereço da string) e chama `plugin_fn_call`. Paleta: categoria
  "Plugins|...". Scripts: `ScriptApi.fixed` (o nome); "Smooth Step" vira `SmoothStep(...)`; a lista da API é refeita
  quando entram funções de plugin.
- Segurança: init, funções, ticks e menus sob `__try/__except`; um crash desliga o plugin ("crashed in X") e o editor segue.
- Ordem: editor_open_project chama `plugins_init(projeto)` antes do `bp_init`; sem projeto, main.c chama
  `plugins_init(NULL)` (só os da engine); o jogo antes do `bp_init`. Empacotar copia os carregados para `Plugins/`.
- Editar > Plugins (`plugins_ui.c`): busca, caixa, versão, autor, estado, nós; Novo Plugin... gera `plugin.json`,
  `Source/<Nome>.c` (nó Remap), `plugin_api.h` e `build.bat` (acha o Visual Studio pelo vswhere; `pushd` na pasta do
  Installer porque `cmd /c` tira as aspas de caminhos com parênteses).
- ExamplePlugin (`plugins/ExamplePlugin`, desligado de fábrica): Smooth Step, Spring Toward, Random Point in Box, Count
  Calls (chamável com estado), Example Ticks, tick e menu; Crash Test só com `REALENGINE_PLUGIN_CRASH_TEST`.
- Testes: `plugins` (script Real chama os nós em Play: 0,5 / 10 / 7 / ponto na caixa; 1328 ticks; crash isolado) e
  `plugin_template` (Novo Plugin, build.bat em ~2,5 s, carrega, Remap(5) = 50).
- Medidas: carga do ExamplePlugin 0,8 ms (21 ms a frio); chamada de nó ≈ uma busca no cache + a função.
- Falta: plugins com conteúdo (assets em Plugins/<Nome>/Content), hot reload, plugins no Android (.so), pinos de string/ator.

## Painel de plugins e plugins embutidos (2026-11-25, item 18)
- Editar > Plugins como o da Unreal (`plugins_ui.c`): barra lateral (Todos, Embutidos, Projeto, as categorias com a
  contagem), cartões com o ícone duotom na cor da categoria, nome, versão, autor, descrição, Ativado, estado ("precisa
  reiniciar" só para DLL, erro, nós no hover), busca, Novo Plugin..., Abrir Pasta. `plugin.json` ganhou `Icon` (o nome
  de um ícone da engine, `ui_icon_name`: "BRUSH"; vazio: o da categoria); o modelo do Novo Plugin já traz `Icon`.
- `plugins_add_builtin` (plugins.c): um plugin embutido genérico (sem DLL, liga e desliga na hora, `Config/Plugins.ini`,
  senão o padrão dele); sem projeto aberto a mudança vale só nesta execução. PLUGINS_MAX 64 -> 96.
- O kit (`plugin_kit.h/.c` + `plugin_kit_<categoria>.c`): 32 ferramentas do editor, cada uma uma `KitPlugin` (janela em
  Ferramentas > Plugins > categoria, sobreposição na viewport, tick, `off` que solta a memória, auto-teste). Ligados de
  fábrica: Snap to Ground, Align and Distribute, Camera Bookmarks, Selection Sets, Command Palette.
  - Artista: Batch Rename, Color Palette, Material Swap, Mesh Audit. Level Design: Scatter, Snap to Ground, Align and
    Distribute, Array, Measure. Animação: Animation Retime, Animation Audit, Motion Trails. Gameplay: Debug Cheats (God
    Mode, Fast Walk, Low Gravity, Heal, Slow Motion: `kit_time_scale` no `dt` do `editor_frame` em Play), Spawn Palette,
    Teleporter, Tag Finder. Produtividade: Camera Bookmarks (Ctrl+Shift+0..9), Actor Notes, Selection Sets, Command Palette
    (Ctrl+Shift+P), Asset References, Unused Assets (Lixeira, com confirmação). Render: Screenshot (F9, a viewport pelo
    `vkctx_request_grab`), Lighting Audit, Scene Stats, Light Radius. Áudio: Sound Audit, Sound Map. IA: AI Overview, Nav
    Path Tester. UI: Safe Frames. Online: Net Stats.
  - Referências entre assets (Asset References, Unused Assets): um worker (`async_run`) lê cada arquivo do Content e acha os
    hashes de 4 bytes (`bp_vpath_hash`, o que os componentes e assets binários guardam) e as palavras com o nome dos outros
    assets (texto, caminhos); o nível aberto conta pelos bytes dos componentes dos atores. Um asset carregado só pelo nome
    num script aparece como sem uso: o aviso fica na janela e na confirmação.
  - Ganchos fora do kit: `editor_duplicate_actor` (a cópia do Ctrl+D sem undo nem seleção), `kit_menu`/`kit_frame` no
    editor.c, `kit_time_scale`. A memória dos plugins é contada (`kit_alloc`, `kit_account`): desligado, nada fica.
- Teste `plugins` (REALENGINE_KIT_WINDOWS=1): cada um ligado, o auto-teste, desligado sem vazar; depois todos ligados com as
  janelas abertas durante o Play e desligados no fim (0 bytes). `plugin_template` confere Category e Icon do modelo.
- Falta: ícone em imagem (PNG) no plugin.json, as ferramentas por API de plugin (DLL) em vez de embutidas, atalhos
  configuráveis.

## Catálogo de classes, busca e arrastar (2026-11-10, sugestões "organização" e "pesquisa")
- Um catálogo só (`place_catalog`, `editor.c`) para o + Adicionar, Ator > Adicionar, o + do Organizador e o Place
  Actors: 12 categorias como na Unreal 5, cada classe uma vez; modelos (partículas, veículos, tecido, mundo
  procedural, céu, interativos) em submenu próprio. O menu abre com a busca ativa (nome em pt ou en).
- Qualquer classe arrasta para o viewport (`RE_PAYLOAD_PLACE`, `editor_place_drop`: onde o raio do mouse toca o
  chão, grade de 10 cm). Colocados Recentemente compartilhado entre menu e painel.
- Content Browser > Adicionar: "Material" básico e o submenu "Material" avançado tinham o mesmo ID (o aviso do
  ImGui); cada seção tem seu escopo de ID agora.

## Simulate (2026-11-10)
- Alt+S, menu ☰ do viewport ou a setinha do Play: Play já ejetado (`editor_simulate`), a câmera do editor, "Simulando"
  no aviso; Esc para e restaura o snapshot.

## Gameplay Framework como plugin embutido (2026-11-10)
- Os módulos R7 (Vida, Inventário, Interação, Pausa e Opções, Salvar Progresso, Diálogo, Missões, Regras, Habilidades,
  HUD) são o plugin `GameplayFramework` na lista do Editar > Plugins (`PluginInfo.builtin`, sem DLL; ligado por padrão;
  `Config/Plugins.ini` como os outros, lido também ao lado do exe no jogo empacotado, que leva o arquivo).
- Liga e desliga na hora (`plugins_gameplay_enabled`). Desligado: `bp_kind_gameplay` tira os nós do menu do grafo,
  `BP_COMP_GAMEPLAY` os componentes (Blueprint e Details), o catálogo de classes some com Pickup, Checkpoint, Game
  Rules e Interativos, o Content Browser com Diálogo e Missão; no Play não há Interactor nem prompt, HUD, barra de
  habilidades, rastreador de missões nem a tecla do menu de pausa. O código continua na engine: um nível que já usa
  os componentes segue funcionando (como um plugin da Unreal desligado depois de usado, só que sem quebrar).

## Estilos dos grafos (2026-11-10)
- `graph_style.c/.h`: 5 estilos de nó (Padrão, Plano, Minimalista, Vidro, Neon) e 5 de fio (Curva, Reto, Circuito com
  ângulos retos, Brilho, Fluxo com traços andando), escolhidos separados em Preferências do Editor > Estilo dos Grafos
  por miniaturas (`graph_style_preview`: dois nós e dois fios desenhados com o par). Salvos no editor.ini
  (`graph_node_style`, `graph_link_style`). O Blueprint desenha nós e fios por eles (`graph_node_frame`, `graph_wire`,
  `graph_wire_dist` para clicar no fio); Material, AnimGraph, Sound Cue e Diálogo, os fios. `--show prefs:graph` abre a página.

## Painéis redimensionáveis, gizmo nos pontos da spline, plataforma do empacotamento (2026-11-10)
- `ui_splitter` (ui_icons.c): divisória arrastável entre painéis (duplo clique: padrão) nos editores de Material,
  Blueprint (colunas e Components), Animação, Sound Cue, Diálogo e Partículas; janelas redimensionam por qualquer borda
  (`ConfigWindowsResizeFromEdges`, 6 px de folga); a Gaveta de Conteúdo pela borda de cima; o Output Log pelas bordas.
- Pontos da spline (rio, trilho): clique num ponto mostra 3 setas (X, Y, Z do mundo) que movem o ponto só naquele eixo.
- Empacotar Jogo: cartões de plataforma (Windows, Android APK, Android Google Play), salvos em `package_platform` no
  `.rproject`; sem escolha, abre no Android se o projeto tem nome de pacote ou controles de toque.

## Viewport de asset compartilhado (Q7, 2026-11-17, pedido do usuário)
Pedido: "várias janelas 3D dos editores não mostram o modelo; o Physics Asset/Ragdoll e o IK Rig mostram só o esqueleto;
as janelas são simples demais". Agora todo editor com 3D usa **um viewport de asset só** (`src/asset_viewport.c/.h`),
como o viewport dos editores de asset da Unreal:
- **O modelo sempre aparece**, desenhado pela GPU (`render_preview`: materiais, texturas, sombra, 4x MSAA). Onde: Static
  Mesh, Skeletal Mesh, **Esqueleto**, Animação, Montage, Blend Space, Anim Blueprint (Viewport e canto do AnimGraph),
  Motion Matching, **Physics Asset** e **IK Rig** (antes só o esqueleto em linhas), **Material** (a esfera na CPU virou o
  material de verdade numa malha de prévia: esfera, cubo, cilindro, cone, plano ou qualquer Static Mesh), **Partículas**
  (o chão, o céu, a luz e a câmera do viewport; as partículas por cima), **Veículo** (as vistas 2D de cima/lado viraram 3D:
  a carroceria ou a caixa, rodas, boias, asas e rotores) e **Tecido** (Janela > Simulação de Tecido: a peça selecionada em
  repouso). Groom não tem editor próprio (é um componente): fica para quando tiver.
- **Modos de visualização** (13): Iluminado, Sem Iluminação, Wireframe, Iluminado com Wireframe, Só Iluminação, Normal do
  Mundo, Rugosidade, Metálico e os novos **Albedo**, **Oclusão Ambiente** (passe de profundidade sem anexo: cada fragmento
  deixa a profundidade num buffer por `atomicMin`; 12 pontos do hemisfério projetados nele — precisa de
  `fragmentStoresAndAtomics`, ligado no `vkctx.c`; sem ele cai para Só Iluminação), **UV** (xadrez pelas coordenadas e o
  layout do canal 0 desenhado no canto), **Cores por LOD** (branco, vermelho, verde, azul como a LOD Coloration da Unreal) e
  **Overdraw** (todas as superfícies somadas sem teste de profundidade sobre o preto).
- **LOD**: Automático (a mesma escolha do `cull.comp`, `render_mesh_lod_for`: erro abaixo de 1 px nas importadas, raio na
  tela nas formas) ou forçado no menu LOD (com os triângulos de cada um). `PreviewDraw.lod` + `render_mesh_lod_range`.
- **Mostrar**: ossos (nenhum / selecionados / todos; losangos como os da Unreal, clique seleciona), nomes, eixos; sockets;
  corpos de física (cápsulas, esferas e caixas em 3D, clique seleciona) e juntas; chão, grade; layout de UV; estatísticas
  (triângulos, malhas, LOD, GPU e CPU).
- **Iluminação**: Estúdio ou **Céu Aberto** (céu procedural atrás e nos reflexos, o sol onde está a luz); a luz principal
  girada por **L + arrastar** ou pelos campos (direção, altura, intensidade); "Seguir a Câmera" volta ao estúdio.
- **Câmera**: esquerdo orbita, meio move, roda aproxima, **direito + WASD/QE voa** (roda durante o voo: velocidade),
  Alt + direito aproxima, **F enquadra**, campo de visão e velocidade no menu Câmera.
- **Abas no canto superior direito** (Modelo / Esqueleto / Animação / Física / IK Rig): a mesma janela (`###asset_persona`)
  troca o que edita sem fechar — da animação para o modelo ou o esqueleto, e para o Physics Asset / IK Rig da malha (o
  editor de IK Rig/Physics assume a mesma janela; `anim_persona_switch`). A aba Física cria um Physics Asset pelos ossos
  quando a malha não tem (como o Create Physics Asset da Unreal). O IK Rig só aparece quando existe.
- **Sockets** (`AnimSocket` no `.rskm`, chave `sockets`, pelo nome do osso): Adicionar Socket (botão na aba Esqueleto ou
  clique direito num osso da árvore), renomear, trocar de osso, posição e rotação relativas, **mover com o gizmo** (as
  três setas nos eixos do mundo; `avp_socket_move_world` converte para o espaço do osso), **malha de prévia** (só nos
  editores; `has_preview`: o cubo é a malha 0). Na árvore, os sockets ficam embaixo dos ossos. `anim_bone_entity` aceita o
  nome de um socket (vale o osso dele). Limite: o deslocamento do socket ainda não entra no anexo do jogo (o item vai no
  osso); fica para o Q10.
- `render_preview`: novos modos e flags (`PREVIEW_SKY`, `PREVIEW_LIGHT_WORLD`), leitura da imagem para o teste
  (`render_preview_readback/read`), o carimbo de tempo inicial com `ALL_COMMANDS` (no topo do pipe ele media também o fim
  do passe da cena: 0,1 ms aparecia como 0,8). `INDEX_CAP` do i18n dobrou (16384): a tabela passou de 4096 entradas e as
  novas não eram achadas.
- Teste `asset_viewport` (área editor): abre os 13 editores com um asset de exemplo (os que o projeto não tem são criados
  em `/Content/Q7Test` e apagados no fim), lê a imagem da GPU em cada um dos 13 modos e conta os pixels do modelo contra o
  fundo (chão, grade e céu desligados; os ossos são do ImGui, nunca entram); cria um socket na mão do Mannequin com um cubo
  de prévia (os pixels crescem), move 10 cm (o lugar acompanha), salva numa cópia e relê; mede o custo. Folha de contato
  `asset_viewport_test.png`; `REALENGINE_AVP_DUMP=1` grava cada imagem; `REALENGINE_AVP_KEEP=1` mantém o `Q7Test`.

Medidas (RTX 3060, 2560x1417, Release, escondido, teste `asset_viewport`):
| Situação | CPU (viewports) | GPU (prévias, timestamps) |
|---|---|---|
| 1 editor (Skeletal Mesh, Mannequin, 28 partes) | 0,03-0,05 ms | 0,11-0,31 ms |
| 3 editores (+ Static Mesh e Material) | 0,06-0,07 ms | 0,26-1,24 ms |
- Faixa de 5 rodadas do teste (a GPU variou entre rodadas; as 3 últimas ficaram em ~0,29 e ~1,23 ms). O teste falha acima
  de 3 ms de GPU ou 1,5 ms de CPU com 3 editores.
- O AO adiciona um passe de profundidade só quando o modo AO está ativo; nenhum custo fora das janelas abertas.

## Travadas ao abrir: pipelines em lote nos núcleos (2026-11-19, pedido do usuário)
"Em vários momentos a engine freeza... ao abrir o Project Browser ou o editor, volta uns 30 s depois." Todos os
relatórios de travamento desde a 0.18 (`Documentos/RealEngine/crashes/RealEngine_travamento_*`) tinham a thread
principal no compilador da NVIDIA (`nvgpucomp64.dll`), um pipeline depois do outro: `build_msaa_pipelines` e
`ext_pipelines_ensure` no primeiro quadro, com o cache frio (a primeira abertura depois de uma atualização, ou depois de
uma abertura fechada travada: o `pipelines.bin` só era salvo na abertura e na saída).
- **Lotes** (`render.c`, `PIPE_BATCH`): o construtor roda duas vezes. Na primeira, `pipeline()` só grava a descrição
  (o SPIR-V de cada shader, pela tabela de `shader_named`); os gravados compilam em threads, um por núcleo, enquanto a
  thread principal bombeia as mensagens da janela (`render_set_wait_pump(glfwPollEvents)`) e alimenta o vigia; na
  segunda, cada `pipeline()` pega o feito para a mesma descrição (+ `pipe_state()` e o layout). O cache é salvo logo
  depois de um lote que compilou (> 250 ms). Vale para os MSAA, os RE_EXT e os fixos.
- **Pré-compilação** (`render_prewarm`, logo depois do `editor_init`): o conjunto do passe principal para o MSAA do
  editor gravado e compilado em segundo plano (núcleos − 2) durante a intro, a tela de carregamento e o Project Browser
  (que não renderiza a cena); o primeiro lote pega os prontos (`pw_join`). Abrindo direto um projeto, a tela de
  carregamento mostra "Compilando os shaders: n de N" (`wait_shaders_splash`) em vez do editor parado.
- **Medidas** (i7-11700F 8 núcleos, RTX 3060, cache do driver vazio por `__GL_SHADER_DISK_CACHE_PATH` + o nosso
  desligado por `REALENGINE_PIPELINE_CACHE=off`): 79 pipelines, 88 s de compilação somada. Antes: o primeiro quadro
  parado ~80 s ("não respondendo"; os relatórios: 30 s com parte do cache do driver). Lote: 9,2 s com a janela
  respondendo. Lote + pré-compilação: quadro mais longo **93 ms**; a tela de carregamento conta 9,2 s (8 núcleos: 80 s
  de trabalho ÷ 8). Cache quente: 74 ms de compilação, quadro mais longo 52 ms, 492 fps no editor vazio (igual).
- **Testes** (área editor): `start_pipelines` (sem o nosso cache: a pré-compilação aproveitada e nenhum quadro da
  partida > 400 ms, `REALENGINE_MAX_START_FRAME_MS`) e `start_pipelines_validation` (o mesmo sob as validation layers:
  os pipelines pegos batem com os seus passes, nenhum VUID).
- Falta: os compute pipelines do `render_init` (41, ~6,6 s frios) ainda compilam em série atrás da tela de abertura.

## Gizmos como os do Tlender, com o controle para o dev (2026-11-23)
Pedido do usuário: "gizmos e manipuladores iguais aos do Tlender, dando controle para o dev". O gizmo já era uma cópia do
desenho do `manip.c` do Tlender; agora vem também a camada de controle dele (`editor_viewport.c`, o bloco "The gizmo").
- **O arrasto vira um delta** (`GD`: a translação, o giro em volta de um eixo ou o trackball, a escala nos eixos do
  gizmo) aplicado a cada ator selecionado a partir de onde ele começou (`gizmo_apply`), não mais escrito no ativo e
  copiado aos outros. Assim vêm o **Ponto de Pivô** (Ator Ativo, Ponto Mediano, Centro da Caixa, Origens Individuais;
  **Só Posições**) e a **Orientação** (Mundo, Local, Vista; Ctrl+` Mundo/Local; o Mundo da escala é o Local, como na
  Unreal). Os dois ficam na barra da viewport (globo e alvo, com menu) e no `editor.ini`.
- **Teclas no arrasto** (as do Tlender): X/Y/Z prendem no eixo, Shift+X/Y/Z no plano; de novo, a outra orientação; de
  novo, solta. **Números digitados** (Tab o próximo, - inverte, Backspace), **Ctrl** inverte o encaixe, **Shift** é um
  décimo do movimento (um mouse virtual: sem salto ao apertar), **+/-** o tamanho. **Botão direito / Esc** cancela: os
  atores voltam e o passo de desfazer sai (`editor_undo_drop`); o desfazer só é gravado no primeiro movimento (um clique
  parado não cria passo). Shift ao pegar uma seta: o plano dos outros dois eixos. Os valores aparecem junto ao mouse e o
  eixo preso como linha pelo pivô. Enquanto o gizmo arrasta, os atalhos do editor ficam com ele (o `1`-`9` não troca o
  modo de visualização, o `Esc` não desmarca); o botão direito que cancelou não começa a voar até ser solto.
- **Os fallbacks do Tlender**: seta apontada para a câmera arrasta pela direção dela na tela, plano de perfil pelo plano
  da vista, anel de perfil pela tangente; o giro acumula voltas. Encaixe: Mundo na grade do mundo (como antes, como a
  Unreal); Local e Vista em passos ao longo dos eixos do gizmo (antes o encaixe absoluto puxava o ator para fora do eixo
  local). Escala pela razão das distâncias na tela (a do Tlender). **Trackball** (girar: dentro dos anéis).
- **Rotação invertida** (relato do usuário): o trackball copiado do Tlender girava ao contrário — com os eixos daqui
  (X à frente, Y à direita, Z para cima) o giro positivo em volta de "cima" leva a face da frente para a esquerda; os
  sinais eram os do Blender. Agora a face virada para a câmera acompanha o mouse. O teste confere cada anel (Z, X, o de
  fora: o ponto agarrado fica a < 1 px do mouse) e o trackball (a face vai para a direita e para baixo); com o sinal
  antigo ele falha (−29,7, −19,5 px).
- **Cursor do mouse** (o do Tlender, pedido do usuário): sobre um cabo, quatro setas (mover), a mão (girar) ou setas
  diagonais (escalar); arrastando, o cursor do sistema some e o de transformação é desenhado no mouse (quatro setas; um
  arco em volta do pivô; um anel de setas no trackball; setas para fora do pivô), branco com contorno escuro. O teste
  confere o cursor sobre o cabo e escondido no arrasto.
- **Vista ortográfica**: o tamanho pelo `ppc`; antes um ator na frente do alvo da vista sumia o gizmo (`z < 5`).
- **Tamanho do Gizmo** (Preferências do Editor > Viewport, 40-300 px, × a escala da UI).
- Teste `gizmo_ui` (`src/gizmo_test.inc`, mouse e teclas injetados): seta X move só em X com um passo de desfazer; clique
  parado sem passo; Y no meio troca o eixo; "250" = 250 cm; botão direito volta sem passo; girar "90" em volta da mediana
  de dois atores; escalar "2" na caixa X; os anéis e o trackball girando para o lado do mouse; o gizmo na vista de cima. Artigo `gizmo-control` no Assistente.
- Fora (ainda do Tlender): ferramenta Transformar (as três juntas), G/R/S modal sem gizmo, snapping a vértice/face,
  cursor 3D, orientações Normal/Gimbal/Parent.

## Editores de nós sem atraso (2026-11-23)
"Os nodes têm muito delay ao arrastar, tanto eles como os links; tudo que tem node está lento."
- **Um quadro atrás**: o Material Editor (e os grafos de Animação, IA, Diálogo e Sound Cue) moviam o nó **depois** de
  desenhar: nó e fios apareciam onde estavam no quadro anterior. Agora a posição vem antes do desenho (no Material
  Editor pela posição absoluta desde o clique, como o Blueprint já fazia). Medido no `mated_ui`: o nó desenhado com o
  mouse em 8 de 8 quadros do arrasto.
- **Janela destacada a 30 fps**: "Usar Menos CPU em Segundo Plano" dormia 30 ms por quadro quando a janela principal
  perdia o foco — também quando o foco estava num editor destacado para outra janela. Agora conta o foco de qualquer
  janela do editor (`editor_any_window_focused`).
- **Zoom**: a fonte dos grafos em pixels inteiros (cada tamanho novo assava glifos e esperava a GPU).
- Um bug achado pelo teste: o pino de saída de um nó em cima da caixa de outro desenhado depois puxava o link do nó de
  baixo (`hover_out_node`).

## Organizar Nós e arrastar vários assets (2026-11-23, pedidos do usuário)
- **Organizar Nós** (`graph_layout.c`, comum a todos): layout em camadas (o de Sugiyama, pequeno). Colunas pelo maior
  caminho até as pontas do grafo (fluxo: cada nó uma coluna à esquerda do mais distante que ele alimenta) ou desde a
  raiz (árvore); a ordem de cada coluna pelo baricentro dos vizinhos já colocados; empilhado sem sobrepor; ancorado onde o
  grafo estava; empates mantêm a ordem antiga (os filhos de uma Behavior Tree rodam da esquerda para a direita); laços
  cortados onde fecham. **Q** e item no menu do botão direito em Material, Blueprint (menu do nó), Anim Graph, Sound
  Cue, Diálogo (árvore para a direita a partir do Start) e PCG; a Behavior Tree usa o Auto Arrange dela. Com 2+ nós
  selecionados, só eles. Teste `graph_layout` (fluxo de 8 embaralhado: sem sobreposição, todo fio para a direita; só a
  seleção; árvore na ordem; laço).
- **Arrastar vários assets** (`content.c`): apertar um item já selecionado com outros mantém a seleção (antes o clique,
  no mouse down, reduzia a seleção a ele e só ele era arrastado); soltar sem arrastar deixa só ele. A prévia mostra
  "X e mais N"; `content_drag_paths` dá a lista a quem recebe o arraste (pastas já moviam a seleção; o Material Editor
  agora liga as texturas, ver materiais.md).

## Static Mesh: material salvo e sockets (2026-11-23, pedido do usuário)
- **Material da malha**: o combo do visualizador (antes "Material de Prévia") ganhou "Salvar como Material da Malha":
  grava o `.<malha>.mat` (o que o arraste para o nível usa) e avisa o editor (`asset_viewer_on_mesh_material`), que
  passa os atores do nível com essa malha e o material antigo (ou nenhum) para o novo, com desfazer.
- **Sockets** (`anim_meshsock.inc`, no anim.c): os mesmos `AnimSocket` (osso 0: o espaço da malha) num arquivo oculto
  `.<malha>.sockets` (JSON), em cache. O visualizador monta um esqueleto de um osso com eles e usa o painel e o gizmo
  dos sockets de Skeletal Mesh (com um osso só o painel esconde o osso e as dicas sobre ossos); salva na hora.
  `anim_socket_world` acha os de uma Static Mesh quando o ator não tem esqueleto (Get Socket Location/Rotation), e
  `anim_attach_actor_to_socket` prende atores neles (lista própria, colocada a cada quadro depois da lógica).
- Teste `mesh_sockets` (depois de importar o OBJ de teste: 2 sockets gravados e lidos, o "Handle" num ator girado 90° e
  escalado 2, um ator preso ao "Top" com deslocamento, o material salvo passando aos 2 atores do nível que tinham o
  antigo); o painel conferido por print.

## Destaque ao arrastar material e rolagem pela borda (2026-11-23, pedidos do usuário)
- **Viewport**: o contorno e a dica "Aplicar X em Y" ao arrastar um material nunca apareciam: a condição esperava o
  payload não estar aceito, e a própria viewport o aceita enquanto está sob o mouse. Agora aparece para material,
  Material Instance e textura; uma textura solta numa malha usa o `M_<textura>` ao lado (ou o cria pelo
  `content_make(CM_MATERIAL)`, que já liga as irmãs pelo nome) e aplica.
- **Rolagem pela borda** (`gv_edge_scroll` no graph_view.h): arrastando nó ou fio a menos de 36 px da borda (ou além),
  a vista rola até ~2100 px/s (pela distância, por tempo); nos editores de arraste absoluto (Material, Blueprint) o
  ponto de partida do arraste anda junto, nos de arraste por delta (Anim Graph, Sound Cue, Diálogo, PCG, BT) o nó soma o
  mesmo; o nó fica sob o mouse. Teste: passo 4 do `mated_ui` (nó segurado na borda: a vista rolou e o nó a 0,0 px do
  mouse).

## Visualizadores com o Details da Unreal (2026-11-24, item 8)
Pedido: "os visualizadores com as opções da Unreal (LOD, Nanite, redução de triângulos, rig, textura, malha), UIs mais cheias".
- Busca no topo e seções que recolhem em todo visualizador (`av_details_search` / `av_section` / `av_row` em
  `src/asset_viewer_details.inc`, declarados no `asset_viewer.h`): Static Mesh, Textura, Skeletal Mesh, Esqueleto,
  Animação (`src/anim_editor_details.inc`) e Sound Wave (`audio_editor.c`). A busca casa o título traduzido, o inglês e
  palavras-chave de cada seção; buscando, as seções aparecem abertas.
- **Static Mesh**: Detalhes do Asset (caminho, triângulos, vértices, tamanho, canais de UV, cores, disco), Slots de
  Material, **Configurações de LOD** (número 1-4, % dos triângulos do LOD 0 por LOD, mínimo; Aplicar / LODs Automáticos:
  `asset_mesh_rebuild` refaz do LOD 0 com `mesh_build_indexed_lods` e grava o `.rmesh` (a tabela de LODs que já existia:
  sem mudar o formato), relê as malhas (`render_meshes_reload`) e guarda os valores em `.<malha>.rlod`), **Configurações
  de Build** (Recalcular Normais pelo ângulo de suavização: faces no mesmo lugar dentro do ângulo, por área; Aplicar
  Escala), **Estatísticas** (triângulos, %, vértices e KB por LOD, erro em cm), Sockets, Prévia.
- **Textura**: Textura (importada/exibida, formatos, mips, Tamanho do Recurso), Configurações (mais o **LOD Bias** =
  o Tamanho Máximo por mips: `asset_tex_set`), Canais, Ações (Reimportar), Visualização.
- **Skeletal Mesh**: Detalhes (ossos, sockets, skins, partes, morph targets, triângulos, arquivo de origem), LODs das
  skins (`mesh_skinned_lods_opts`: no LOD 0 dos vértices, sem cache; mínimo 2: com 1 a carga refaz os automáticos),
  Materiais, pesos padrão dos Morph Targets (salvos no .rskm), Física e IK (abrir/criar), Osso Selecionado; **Buscar
  Ossos** na árvore. **Esqueleto**: ossos, sockets, rig. **Animação**: duração, quadros, taxa, trilhas, notifies, curvas,
  memória, Repetir. **Sound Wave**: Informações (duração, taxa, canais, quadros, formato, memória, pico, RMS), Som,
  Configurações.
- Fora (o que a engine não faz por asset não aparece): Nanite por malha (os clusters/DAG são automáticos nas malhas
  grandes, no render.c), colisão simples/complexa por asset, screen size por LOD (o cull escolhe pelo erro na tela), mip
  gen e endereçamento X/Y separados (o material lê um Wrap), lightmap UV gerada (só a que a importação fez). Clusters de
  Skeletal Mesh: entram no painel quando o item 4 os trouxer.
- **Medido** (teste `asset_viewport`, estágio de opções, RTX 3060): refazer 3 LODs de uma grade de 8192 triângulos
  **37 ms** (8192 / 4096 / 1638 = 50% / 20%), 1 LOD, escala x2 (100 -> 200 cm) com normais refeitas; textura BC7 ->
  sem compressão e LOD Bias 2 (64 -> 16) relidos do `.rtex`. O custo dos editores abertos segue (3 editores: CPU 0,074 ms,
  GPU 0,557 ms). Fotos antes/depois: side-session-notes do Lead (`visualizador-*-antes-depois.png`).

## Destaque da seleção em todo ator (item 17, 2026-11-25)
- Lista do mask (`render.c`, antes das luzes): o selecionado com malha e, por um bitset dos selecionados, tudo o que pende
  dele por `CAttach` (sobe pelo `parent` e pelo `owner` até 8 níveis): partes e pele da Skeletal Mesh (a pose animada:
  `mesh.vert` passe 2 lê os vértices dobrados), componentes de Blueprint, veículos, blocos do Landscape. Cada draw leva
  o id do ator no `pad` do push (2 + o índice do ator; 1 é o destaque do jogo): uma borda só em volta do ator inteiro.
- `mask.frag`: corta pelo alfa do material Masked (o corte simples do `masked.frag`, UDIM incluído).
- Sem seleção o passe do mask não roda (`R.mask_idle`: ele fica limpo e em leitura; refeito ao redimensionar).
- `composite.frag`: a borda medida antes do retorno do HDR10 e aplicada em nits (antes sumia no HDR10).
- `editor_viewport.c` `draw_selected_shape(ed, dl, v, e)`: para todos os selecionados (até 256), não só o primário; sem
  malha nem forma própria, a colisão (caixa, esfera, cápsula), a cápsula do Player Start ou uma caixa de 25 cm (Empty).
  Vale no G (modo jogo).
- Teste `selection_outline` (`selection_test.c`, `--scene selection --game-view`, validado): cubo 29 281 px, ator com 2
  componentes 2 draws / 16 598 px, pele em dois momentos 42 190 e 32 658 px (silhuetas diferentes), luz e Empty com forma,
  nada selecionado 0 px; o passe do mask 0,022 ms (0,030 ms a 1440p). Sem cobertura no teste: Masked (não há material
  Masked pronto nos dados), multidão, Groom, partículas e água (pipelines próprios: o item 17 parte 2).- Parte 2 (25/11): o cabelo do ator selecionado no mask (`mask_hair.frag` + `hair.vert`, `groom_draw_mask`); a multidão
  selecionada (um Crowd Spawner): um draw por parte com os agentes como instâncias (o passo nparts e o byte do id no
  `pass` do push, bit 31: `mesh.vert`/`mask.frag`), cada parte no LOD mais grosso com menos de meio pixel de erro na
  distância da multidão (30 Mannequins a 15 m: 840 draws no LOD 0 custavam 0,200 ms, instanciados 0,202 - eram os
  vértices -, com o LOD 0,035 ms). Só Skeletal Mesh por partes: uma pele não faz multidão (a VAT é pendência); água e
  partículas com a caixa laranja do `draw_selected_shape` (a água pelo plano dela). A cadeia de peças subida até 64
  níveis (era 8: um rig fundo perdia peças). `selection_outline`: cadeia de 13 níveis (12 peças), borda por peça, a
  multidão de 30 agentes. Flags de teste: `--select-all`, `--select-name <prefixo>` (no 30º quadro).

## Processos filhos e o 1º quadro do Play (2026-11-25, bug-fixer)
- `re_process_start` (platform_win32.c) põe cada filho num job object com `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`: ele é
  criado suspenso, entra no job e só então segue, e morre com o editor (fim, crash, o timeout de um teste). A Prévia em
  VR (`rivora_game.exe --vr`) aberta pelo `ui_sweep` sobrevivia e segurou a GPU de duas suítes. Teste
  `child_process_cleanup` (`--vr-preview`, o editor sai, nenhum `rivora_game.exe` do build fica).
- O 1º quadro do Play: os controles perguntados atrás da tela de abertura (a 1ª consulta do GLFW, ~110 ms, caía nele
  pelo Start do menu de pausa) e as escolhas do Upscaler num thread (`ups_vendor_warm_async`): o Play de 1284 ms na
  varredura, depois 188, agora 42-44 ms (teste `play_hitch`, limite 100 ms). Escopos de trace no `editor_play`
  ("PIE: ...") e no começo do quadro ("Gamepad", "NavMesh", "Background work (done)"...).
