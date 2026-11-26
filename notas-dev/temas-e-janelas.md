# Temas (cara própria) e janelas modulares

Pedido do Nathan (17/11/2026, plano 0.43, itens Q1 e Q2): identidade visual própria (nada do cinza+azul da Unreal),
10 temas e um Editor de Tema; janelas modulares como as da Unreal, com os alvos de encaixe visíveis, Níveis ao lado do
Organizador, layouts salvos, e o X da janela maximizada fechando só a janela (fechava a engine).

## Q1: temas por tokens
- **`theme.h`**: 118 tokens de cor e 15 métricas. Grupos: superfícies (`bg_deep`, `bg_app`, `bg_panel`, `bg_raised`,
  `bg_field`, `bg_control`, `bg_hover`, `bg_selected_dim`, `border`, `border_strong`), texto (`text`, `text_strong`,
  `text_dim`, `text_disabled`, `text_on_accent`), acentos (`accent`, `accent_strong`, `accent_dim`, `accent2`), estados
  (`ok`, `warn`, `error`, `info`), a **rampa neutra** `n0`-`n15`, **11 matizes x 3 tons** (`red_d`, `red`, `red_l`...),
  pinos de Blueprint por família, famílias de nó (`node_*` e `node_title`), famílias de asset, grafos/prévias/encaixe.
  Métricas: raios (janela, painel, controle, popup, aba, nó), espaço entre painéis, margem, espaçamento, borda,
  durações (rápida, normal, surgimento) e tamanhos de fonte.
- **Uso**: `TH(TK_X)` (ImU32 opaco), `THA(TK_X, alfa)`, `THV`/`THVA` (ImVec4), `TMV(TM_X)`. A tabela é resolvida uma
  vez por troca de tema; ler uma cor é uma leitura de array (**1,0 ns**). Trocar de tema: **0,0004 ms** + o estilo do
  ImGui **0,0003 ms**, entre quadros (`main.c`: `theme_style_dirty()` antes do `igNewFrame`). Sem custo de fps.
- **Sobre a cena 3D**: um arquivo que define `THEME_SCENE` antes de incluir `theme.h` (`editor_viewport.c`,
  `bp_preview.c`, `asset_viewer.c`, `editor_view2.c`) lê a tabela "de cena": num tema claro, as cinzas e matizes de um
  tema escuro, porque o texto sobre um render precisa do contraste de sempre.
- **Os 10 temas** estão no `theme.c` no próprio formato `.rtheme` (um parser só): Órbita Escuro (padrão: grafite
  azulado #0B0E13/#11151C, acento ciano #3CC6F0, Ignição #FF9A3D, a paleta da Parte 9 do plano antigo), Grafite,
  Meia-noite (OLED), Claro, Alto Contraste (borda 2 px, acento amarelo), Brasa, Floresta, Neon, Sépia e Daltônico
  seguro (Okabe-Ito). Cada tema dá ~25 cores; o resto é **derivado** (`theme_resolve`): a rampa passa pelas superfícies
  e textos nos valores que as cores antigas tinham; um tema claro inverte a rampa e os tons (o tom "escuro" vira um
  pastel de preenchimento, o "claro" vira tinta escura); pinos, cabeçalhos e famílias seguem a matiz da referência do
  Órbita; cabeçalho de nó garante 4,5:1 com `node_title`; matiz média 3:1 sobre o painel.
- **`.rtheme`**: texto. `rtheme 1`, `name`, `base` (o embutido de partida: o que o arquivo não dá vem dele), `light`,
  `hue_sat`, `hue_light`, `palette = okabe`, `<token> = #RRGGBB[AA]`, `<métrica> = número`. Os do usuário ficam em
  `%APPDATA%\RealEngine\Themes`; o tema escolhido vai no `editor.ini` (`theme=`). `--theme <nome>` vale só para uma
  execução (prints, testes).
- **Editor de Tema** (`theme_editor.c`, Editar > Preferências do Editor > **Aparência**): cartões dos temas (um clique
  aplica no quadro seguinte), Editar uma cópia (cada cor ao vivo; "Derivar" devolve uma cor ao valor calculado),
  Duplicar, Importar/Exportar `.rtheme`, Apagar (só os do usuário), tema claro, saturação/luminosidade das matizes,
  Okabe-Ito, tamanhos e animações, a linha de **contraste** ao vivo e uma prévia (botão, caixa, campo, linha
  selecionada, estados, dois nós com pinos). `--show prefs:appearance` abre a página.
- **Conversão** (`tools/theme_convert.py`, uma vez): as ~990 cores fixas de 38 arquivos viraram tokens. Cinza → rampa
  pelo brilho; o azul da Unreal (#0070E0 e vizinhos) → `accent*`; texto colorido (`igTextColored`) → estados;
  o resto → matiz e tom. Tabelas `static` viraram arrays de tokens (`TH(tabela[i])` no uso); pinos (`type_color`),
  cabeçalhos (`header_color`) e famílias de asset (`content.c`) receberam tokens semânticos. Cores que são dado (a
  média de duas cores de vértice, o preto da intro em `brand.c`) ficam com `// theme: literal (motivo)`.
- **Animações**: as durações do imgui corrigido viraram variáveis (`RE_FadeInSeconds`, `RE_FadeQuickSeconds`,
  `RE_FadeOutSeconds`, `RE_HoverEaseSeconds`) que o tema preenche.
- **Fontes** (`data/fonts`, OFL, `tools/embed_bin.cmake`): Inter Regular/SemiBold (interface, `g_font_ui`,
  `g_font_bold`), Sora SemiBold (títulos, `g_font_title`), JetBrains Mono (código e números, `g_font_mono` =
  `g_code_font`). Instanciadas das variáveis do google/fonts e cortadas ao latim: 208 KB no executável. Segoe UI só
  como reserva (outras escritas). Não leem mais `C:\Windows\Fonts` para o texto principal.

## Q2: janelas modulares (`editor_dock.c`)
- O editor de nível era um layout fixo de filhos (`##editor`). Agora o espaço entre a barra de ferramentas e a de
  status é um **DockSpace** (`RealEngine.LevelEditor.Dock`, id fixo: os layouts o nomeiam). Painéis: Posicionar
  Atores/Modos (`###re_place`), Organizador (`###re_outliner`), Níveis (`###re_levels`), Detalhes (`###re_details`),
  Navegador de Conteúdo acoplado (`###Content Browser`). O `panel_header` some num painel acoplado (a aba já o nomeia).
- **Viewport**: fica sobre o **nó central vazio** (`PassthruCentralNode | NoDockingOverCentralNode`). Com uma janela
  acoplada no nó central o ImGui pinta o fundo do DockSpace inteiro e a cena some; vazio, ele deixa um buraco e a cena
  aparece. A janela do viewport é posta no retângulo do nó central a cada quadro (`NoDocking`, `NoBackground`).
- **Layout padrão** (`build_default`): Posicionar Atores à esquerda (14%), coluna direita (19%) com Organizador e
  **Níveis numa aba ao lado** em cima (42%) e Detalhes embaixo, o Navegador de Conteúdo acoplado embaixo do viewport
  (33%). Níveis abre por padrão; o Organizador fica na frente.
- **Alvos ao arrastar** (`dock_frame_end`): o ImGui só mostra os 5 alvos do painel sob o mouse; agora aparecem os de
  todos os painéis (mesma geometria do `DockNodeCalcDropRectsAndTestMousePos`), as 4 bordas externas e um alvo em
  cada **vão entre dois painéis** (soltar ali cria um nó entre os dois, da altura ou largura da divisão inteira:
  `DockContextQueueDock` no segundo filho, lado externo). Painel reaberto sem lugar volta ao lado do viewport.
- **Layouts** (Janela > Layout): Salvar Layout Como (Este projeto → `<projeto>/Config/Layouts/<nome>.rlayout`; Todos
  os meus projetos → `%APPDATA%\RealEngine\Layouts`), a lista dos salvos, Layout Padrão. `.rlayout` = seção
  `[RealEngine][Panels]` (painéis abertos) + o ini do ImGui (docking e janelas), aplicado entre quadros
  (`dock_between_frames`). O último layout é salvo ao fechar (`Saved/Layout.rlayout` do projeto e `Last.rlayout` do
  usuário) e volta ao abrir ou ao trocar de projeto. Execuções automáticas nunca leem nem gravam.
- **Crash ao fechar (0.43 e 0.44, relatórios #143-146, corrigido em 2026-11-20).** Todo fechamento normal do editor
  quebrava: o `main.c` destruía o contexto do ImGui e só depois chamava `editor_shutdown`, cujo salvamento do layout
  (`igSaveIniSettingsToMemory`) escrevia num contexto nulo. A suíte não via porque as execuções automáticas não gravam o
  layout. Agora o `main.c` salva o layout antes de desligar o ImGui, e `dock_layout_autosave` não roda sem contexto.
  Teste `editor_close`: `--quit-after N` fecha a janela como pelo X ("Não Salvar"), sem nada automático, com um
  `APPDATA` próprio; passa só se o `Last.rlayout` foi gravado. Sem a correção, ele dá SegFault no mesmo ponto.

## O X da janela maximizada fechava a engine (causa e conserto)
- **Causa**: `main.c` faz `glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE)` para abrir o editor maximizado, e a dica nunca
  era desfeita. As dicas do GLFW valem para toda janela criada depois, então **cada janela arrastada para fora do
  editor (viewport do ImGui) nascia maximizada**; o ImGui a mantinha no tamanho dela, e o X que aparecia no canto da
  tela era o da janela principal.
- **Conserto**: a dica volta a `GLFW_FALSE` logo depois de criar a principal; uma janela própria maximizada pelo
  sistema (Win+Cima, menu da barra de tarefas) passa a ocupar a área de trabalho do monitor (o X dela fica no canto);
  e um fechamento que chega à principal com outra janela da engine na frente fecha aquela (`dock_route_main_close`).
- **`cimgui.h`**: os campos `[Real Engine]` do imgui corrigido (`FadeInStart`... no `ImGuiWindow`,
  `TreeFadeDepthMask` no `ImGuiWindowTempData`) nunca foram espelhados no header C: todo campo do `ImGuiWindow` depois
  deles era lido deslocado (o `ui_tips` lia `Hidden` errado; o código de dock caía). Espelhados; o teste `theme`
  compara `sizeof`/`offsetof` do C com os do C++ (`RE_SizeofImGuiWindow`...).
- **`windows.h`** define `small` e `near`: não use esses nomes em arquivos que o incluem.

## Testes (área `editor`)
- `theme` (`re_test_theme`, sem janela): os 10 temas, nomes do plano, todos os tokens resolvidos, contraste, os temas
  diferentes entre si, rampa invertida no claro, estilo do ImGui pelos tokens, `.rtheme` ida e volta e com `base`,
  editar/cancelar/salvar/duplicar/exportar/importar/apagar numa pasta própria, structs do C = C++, custo.
- `theme_literals` (`tools/theme_literals.py`): falha se um arquivo de UI de `src/` ganhar cor literal fora do
  `theme.c` (mesma detecção do conversor; exceções: `brand.c` e `// theme: literal (motivo)`).
- `theme_contrast` (`tools/theme_contrast.py` → `re_test_theme --contrast`): WCAG nos 10 temas (texto 4,5:1 nas
  superfícies, ícones/acentos/estados/pinos 3:1, título de nó 4,5:1). `--user` confere os temas do usuário.
- `dock_targets` (`--dock-test`, `dock_test.c`): com o mouse do próprio teste (fila de entrada do ImGui) arrasta uma
  janela pela barra de título para os 11 alvos (centro e 4 lados de Detalhes, 4 bordas externas, o vão vertical e o
  horizontal) e confere onde ela acoplou; depois, já janela própria e maximizada, fecha pelo X, pelo sistema
  (WM_CLOSE) e por um fechamento que chegou à principal: a janela fecha e a engine segue. Em segundo plano a janela
  própria fica fora da tela; visível, ela é maximizada de verdade e o teste confere que nasceu não maximizada e que
  ocupou a área de trabalho. `REALENGINE_DOCK_DEBUG=1` mostra cada passo (com o foco do sistema e o mouse antes de
  cada arraste).
- **Por que o `dock_targets` falhou na suíte da 0.45 (20/11)**: o encaixe e o layout novo estavam certos; eram duas
  causas no teste. (1) Os testes faziam a checagem de atualização (o comentário dizia que não; só o `--bench` a
  pulava): um build mais velho que a release publicada (o `RE_VERSION_STRING` velho no cache do build principal, a
  base de um A/B) abria a janela Atualizações no meio e os 12 estágios falhavam. Mesmo executável: 15/15 com
  `REALENGINE_NO_UPDATE_CHECK=1`, 12 falhas sem. Agora todo ctest recebe a variável (`tools/suite_areas.cmake`).
  (2) Falhas soltas de um alvo ou do clique duplo quando uma janela da engine pegava o foco do Windows: o backend
  GLFW punha o cursor real nos quadros de espera e uma perda de foco (um clique em outro programa, a janela de outra
  sessão) soltava os botões do ImGui no meio do arraste. Agora o teste é o dono da fila de entrada enquanto roda
  (`drop_os_input`: mouse, teclado e foco do sistema descartados antes dos eventos dele). Prova: uma perda de foco
  injetada no meio de cada arraste derrubava os 11 alvos sem o filtro e nenhum com ele. Custo: nenhum fora do teste.

## Medidas (RTX 3060, 2560x1417, `REALENGINE_BACKGROUND=1`)
Antes = `main` a3bd783 (tema fixo da Unreal, layout fixo), depois = este branch (Órbita Escuro, DockSpace). Melhor de 3
execuções de `--bench 5`, GPU livre:

| Cena | Antes | Depois |
|---|---|---|
| Editor, nível padrão | 526 fps, 1,902 ms/quadro, CPU 0,948 ms | 522 fps, 1,914 ms/quadro, CPU 0,952 ms |
| Editor + Preferências do Editor abertas | 518 fps, 1,930 ms, CPU 1,039 ms | 517 fps, 1,936 ms, CPU 0,971 ms |
| `materials` (`--validation`, 2 s) | 432 fps, 2,314 ms | 438 fps, 2,285 ms |
| `render_style_photoreal` | 272 fps, 3,674 ms | 277 fps, 3,610 ms |

Diferenças dentro do ruído entre execuções (±0,03 ms). Ler uma cor: 1,0 ns; trocar de tema: 0,0004 ms + estilo do
ImGui 0,0003 ms (uma vez, entre quadros). Os alvos de encaixe só são desenhados enquanto uma janela é arrastada.
Fontes no executável: 208 KB. Suíte: área `editor` 16/16 (49 s); kit rápido 69/72 com o editor do Q4 aberto na GPU
(`materials` 175 fps e `render_style_photoreal` 108 fps por GPU dividida: com a GPU livre, 438 e 277 fps; o `i18n`
acusou 3 textos repetidos, já tirados).

## Para o merge
- `cimgui.h` e `imgui.cpp`/`imgui_widgets.cpp` mudaram: rebuild completo.
- Conflitos prováveis: `main.c` (fontes, tema, dock, teste), `editor.c` (layout central, menu Janela, prefs `theme=`,
  cores em tokens), `editor_levels.c` (só o título `###re_levels`), `content.c` (cores e o modo painel do
  `content_draw`), `i18n_table.c` (bloco "Q1 and Q2" no fim), `CMakeLists.txt`, `tools/suite_areas.cmake`,
  `assistant/coverage.txt`. Cor nova vinda de outro branch: `python tools/theme_convert.py --apply <arquivo>` e depois
  o teste `theme_literals`.

## Clique duplo na barra de título maximiza e restaura (19/11, pedido do usuário)
- `editor_dock.c` `dock_title_double_click` (no `dock_frame_end`): clique duplo na barra de título (ou na barra de abas de
  um nó solto) de uma janela solta, fora do X. Janela própria (fora do editor): a área de trabalho do monitor dela; sobre o
  editor: a área do dock space (menu e toolbar à vista). O lugar de antes fica em `g_zoom[16]`; outro clique duplo devolve.
  Maximizada pelo sistema (Win+Seta): o clique duplo restaura (`glfwRestoreWindow`). O arrasto que o 2º clique começaria é
  cancelado (senão a janela voltava para baixo do mouse). Popups, menus, a janela do editor e as presas não entram; a
  janela principal é a do sistema (o Windows já faz isso na barra dela).
- Teste: estágio novo do `dock_targets` (`--dock-test`): "double-click on the title: maximized, then back" (maior que o
  dobro, depois o mesmo retângulo ±2 px). Custo: um teste de retângulo por quadro só quando há clique duplo.

## Q26: o visual Azul Real (19/11/2026, pedido do usuário: as duas referências `referencia_1.png` e `referencia_2.png`)
O usuário gerou duas imagens (editor de nível e editor de Blueprint) e pediu que a engine fosse assim por padrão. Tudo o
que aparece nelas virou função de verdade ou ficou de fora (lista no fim); nada de botão morto.

### Tema "Azul Real" (o padrão novo)
- 11º tema embutido (`theme.c`, `THEME_DEFAULT = 10`, `THEME_DEFAULT_NAME`): azul-marinho #0B0F15 (app), #10151D
  (painel), #161D27 (cabeçalhos/popups), campos #0A0E14, bordas finas #1D2531/#2A3442; um só acento saturado #2B6FE3
  (abas ativas, seleção, botões principais, a pílula da versão), texto sobre o acento branco (4,7:1), Play verde
  (`accent2` #34C46B); cabeçalhos de nó vermelho (eventos #A3262E), azul (funções #1F4F9E), verde (puras #1E6B3E);
  pastas laranja (#E9A23B); cantos 4-6 px, 4 px entre os cartões.
- O Órbita Escuro continua no índice 0 (é a referência das famílias dos outros temas: `orbita()`); os 10 temas antigos
  ficam todos. `theme_init` escolhe `THEME_DEFAULT`; apagar o tema atual ou cancelar a edição volta para ele.
- Aba ativa de qualquer tema: o painel tingido 30% pelo acento (`ImGuiCol_TabSelected`), como nas referências.
- Barra de título do Windows na cor do tema (`editor_look.c` `title_bar_colors`): DWM `CAPTION_COLOR`/`TEXT_COLOR`/
  `BORDER_COLOR` (Windows 11; no 10 o modo escuro), refeito quando o tema muda. Os botões de janela continuam os do
  sistema (snap, arrastar entre monitores, o teste `dock_targets` com a janela maximizada): a imagem mostra os botões na
  mesma faixa, aqui a faixa do sistema fica na cor do menu.

### Quem já usava a engine (o menos surpreendente)
- `editor.ini` ganha `look=2` e `target=`. Um `editor.ini` sem `look` (de antes da 0.44) **com o tema padrão antigo**
  (Órbita Escuro) recebe o Azul Real e o layout novo uma vez; o layout anterior vira um layout do usuário
  (`%APPDATA%\RealEngine\Layouts\Antes do Azul Real.rlayout`, em Janela > Layout) e um cartão no topo oferece
  **Manter** ou **Voltar ao Meu Visual Anterior** (tema e layout de antes, na hora). Com **outro tema escolhido**, nada
  muda: o cartão só oferece **Experimentar** / **Não, Obrigado**. Usuário novo: o padrão direto. Execução automática
  (`g_editor_prefs_readonly`): nada disso.
- Um `.rlayout` antigo (sem as chaves `modes`/`platforms`) abre com esses dois painéis fechados.

### Layout padrão (`editor_dock.c` `build_default`, Janela > Layout > Layout Padrão)
- Embaixo de tudo uma faixa de 33%: Navegador de Conteúdo acoplado (Registro de Saída numa aba ao lado) e Suporte a
  Plataformas na ponta direita (15,5%). Em cima: Modos + Posicionar Atores à esquerda (14,5%), o viewport (o nó central),
  Organizador (Níveis ao lado) sobre Detalhes à direita (23,5% do resto, 60/40).
- `DockPanel` ganhou `DP_MODES`, `DP_LOG`, `DP_PLATFORMS` (no fim: layouts antigos continuam valendo). O Registro de
  Saída virou painel acoplável (era uma janela fixa no canto); a chave `log=` dos layouts é a mesma.
- As abas da frente (Organizador, Posicionar Atores, Navegador de Conteúdo): o ImGui devolve a aba à janela com foco
  todo quadro (`DockNodeUpdateTabBar`, "Apply NavWindow focus back") e a última a aparecer (Níveis) ficava na frente;
  agora o foco é limpo depois de 8 quadros com as abas certas.
- Execuções automáticas continuam com o conjunto mínimo de antes (Organizador, Níveis, Detalhes): os testes de fps e de
  clique não mudam. `--layout-default` abre o padrão inteiro (prints, testes).
- Um Blueprint (classe, interface, biblioteca) abre por cima do viewport, com os painéis do editor em volta (a
  referência 2), nos primeiros 4 quadros acompanhando o nó central; depois é uma janela solta como antes.

### Barra de menu (`look_menu_brand`, `look_menu_readouts`)
- Mais alta (fonte + 2 × 0,55 fonte): a marca (o logo como máscara branca 64 × 64 feita do `logo.png` pela luminância,
  `brand_mark()`, pintada no acento com o topo mais claro), "Real Engine" em Sora e a versão (`RE_VERSION_STRING`) no
  acento; os menus; à direita as barras de sinal (verde ≥ 120 fps, amarelo ≥ 55, vermelho) com "FPS: x | Quadro: y ms"
  (dica: CPU, GPU e o modo de apresentação; clique: Debug Viewer) e os alvos **PC | Android | VR**.
- **Plataforma alvo**: o clique escolhe o alvo (salvo em `target=`); o menu Plataformas mostra o dele primeiro e, com VR,
  o Play abre o VR Preview. Cada alvo tem o ponto de prontidão (verde/amarelo/vermelho) e a dica com a lista.

### Barra de ferramentas
- Modo (Select Mode ▾), grupos ícone + ▾ (Salvar/Abrir, Adicionar, Blueprints, Cinemáticas), Play / Pausa / Parar /
  Ejetar no centro (cinza enquanto não agem) e as opções do Play, Plataformas ▾ (Windows, Android APK e AAB, Meta Quest,
  VR Preview, Suporte a Plataformas), Configurações ▾ à direita. Botões mais altos (FramePadding 8 × 5).

### Barra de status (`look_status_left`, `look_status_right`)
- Esquerda: Gaveta de Conteúdo (acoplado: traz a aba para a frente; Ctrl+Space também), Registro de Saída, Estatísticas
  (o overlay do viewport), Profiler (Debug Viewer), Gerenciador de Aparelhos (o Suporte a Plataformas com os aparelhos).
- Direita: **Pronto** / Jogando / Compilando (spinner); **Compilação de Shaders: N** = `render_pipelines_pending_count()`
  (as pipelines de raio Masked e as do denoiser sendo feitas nas threads delas, 12 cada) + `mat_compiling_count()` (os
  materiais na fila do bake); **Dados Derivados: X** = o tamanho de `%LOCALAPPDATA%\RealEngine` (pipelines.bin, malhas
  processadas, impostores...) medido por uma thread ao abrir e a cada 60 s (clique: abre a pasta); a pílula **Real
  Engine 0.xx** (Sobre).

### Suporte a Plataformas (`look_platforms_panel`, Janela > Suporte a Plataformas, pode ser fechado)
- Três cartões (PC, Android, VR) com o estado; as conferências do alvo, refeitas a cada 5 s (algumas consultas de
  arquivo, < 0,1 ms): PC (GPU e Vulkan 1.3, ray tracing por hardware, pasta do pacote), Android (o `package_apk.ps1`,
  o SDK por `ANDROID_HOME` ou `%LOCALAPPDATA%\Android\Sdk` com build-tools e `platforms\android-*`, o JDK por `JAVA_HOME`
  ou `C:\Program Files\{Microsoft,Java,Eclipse Adoptium}\jdk*`, a `librealengine_game.so` pronta ou o NDK, a pasta do
  pacote, a chave de upload), VR (o runtime OpenXR do registro `HKLM\SOFTWARE\Khronos\OpenXR\1` com o nome do manifesto,
  as plataformas VR do projeto, o Quest pelo Android). As mesmas regras do `android/package_apk.ps1`.
- Botões só onde há o que fazer: Configurações (Empacotamento ou Projeto), Abrir (a pasta), **Empacotar para ...** (os
  empacotadores de sempre, em segundo plano), VR Preview. **Aparelhos**: Atualizar roda `adb devices -l` numa thread
  (modelo, serial, estado); no VR, o runtime.

### Painéis
- **Modos** (novo, ao lado de Posicionar Atores): Seleção / Paisagem / Folhagem como linhas (Shift+1/2/3) e as
  ferramentas do modo ativo (`mode_panel`); entrar num modo traz a aba para a frente. Antes o painel de Posicionar Atores
  virava o de modo.
- **Posicionar Atores**: busca no topo e categorias que abrem e fecham; **Básico** aberto (Ator, Início do Jogador, as
  luzes Pontual/Spot/Retangular/Céu, Cubo, Esfera, Trigger Box/Sphere, Câmera; cada um também na sua categoria),
  Recentes quando há; a contagem de cada categoria; clique posiciona, arrastar leva ao viewport (como antes).
- **Organizador**: a coluna Tipo de um ator de Blueprint é o link azul **Editar BP_...** (abre a classe).
- **Detalhes**: "+ Adicionar" no acento (botão principal), o aviso de nada selecionado centrado.
- **Navegador de Conteúdo**: "+ Adicionar" no acento; pastas como pastas laranja sólidas (`ui_folder_solid`) sem
  cartão, só o nome centrado; assets num cartão escuro com a **faixa da cor da família** embaixo da imagem (no lugar do
  ponto do Q5, como nas referências).
- **Viewport**: os botões em pílulas escuras mais redondas; o botão de layout liga o Viewport 2.
- Ícones novos no estilo duotom: `ICON_MONITOR` (PC), `ICON_ANDROID`, `ICON_SIGNAL` (202 ícones; `icon_sheet`).

### O que ficou de fora (sem função real por trás)
- O lema "Create. Play. Anywhere." e o cartão de marketing "One Engine. All Platforms" (o Suporte a Plataformas mostra o
  estado real no lugar); "Multi-Platform Build" (empacotar tudo de uma vez: os empacotadores escrevem na mesma pasta, ficam
  um por vez no menu); as abas **World Settings** e **Message Log** (não há essas funções); Favoritos e Coleções na árvore
  do Navegador de Conteúdo; a busca nos Detalhes; Character e Pawn soltos em Básico (vêm de Blueprint Classes); os botões
  de janela desenhados pela engine (ficam os do sistema, na cor do tema).

### Testes
- `theme` (11 temas, o padrão é o Azul Real, o Órbita continua o 0), `theme_contrast` (11 temas, 0 pares ruins),
  `theme_literals`, `tips_audit` (tabela nova `ui_tips_table_look.c`), `ui_tips`, `i18n` (89 textos novos em pt),
  `icon_sheet`, `dock_targets` (o layout novo, todos os alvos), `editor_fps`, `assistant` (artigo `platform-support`;
  `themes` e `window-layouts` atualizados).
- `look_migration` (novo, área `editor`; `--look-test <pasta>`): `%APPDATA%` numa pasta própria com um `editor.ini` da
  0.43 no Órbita Escuro e o último layout; confere o Azul Real, o layout padrão inteiro, o layout anterior guardado
  ("Antes do Azul Real.rlayout"), o cartão, e o "voltar" (o Órbita de novo). Nunca toca o `%APPDATA%` do usuário.
- Suíte: área `editor` + `assistente` + `idiomas` 23/23 (2 min 50 s).

### Medidas (RTX 3060, 2560x1417, `REALENGINE_BACKGROUND=1`, `--bench 5`, nível padrão, A/B intercalado)
Antes = `realengine.exe` de `ef431f1` (Órbita, layout de antes), depois = este branch. Durante as medidas outro worktree
rodava a engine na mesma GPU (variação de ±1 ms entre execuções): o par sem outra engine aberta é o que vale.

| Execução | Antes | Depois |
|---|---|---|
| Par sem outra engine na GPU | 442 fps, 2,264 ms, CPU 1,628 ms | 525 fps, 1,905 ms, CPU 1,486 ms |
| Pares com a GPU dividida | 485 / 302 fps (CPU 1,24 / 1,67 ms) | 358 / 407 fps (CPU 1,74 / 1,98 ms) |
| Depois: layout mínimo dos testes × layout padrão inteiro (`--layout-default`), 3 pares | 506 / 526 / 294 fps (CPU 1,14 / 0,95 / 1,38 ms) | 592 / 683 / 381 fps (CPU 1,07 / 0,99 / 1,40 ms) |

Sem custo mensurável de CPU: as barras de menu e de status são algumas dezenas de quads e textos por quadro; as
conferências das plataformas, a cada 5 s (consultas de arquivo); o tamanho do cache e o `adb`, em threads. O layout
inteiro é até mais rápido (o viewport menor tem menos pixels). O editor vazio ficou como antes (a meta de > 1000 fps é a
da cena vazia; o nível padrão com GI está em ~500 fps nos dois).

## Seleção no azul do acento (2026-11-23)
Pedido: "usar o azul do R como base de todo o estilo". A seleção dos nós no Material Editor, PCG, Diálogo e Behavior
Tree, o widget selecionado no Designer do UMG e o estilo de grafo atual nas Preferências passaram de laranja para
`TK_ACCENT` (o azul #2B6FE3 do Azul Real; em outro tema, o acento dele). O contorno de seleção na viewport 3D segue
laranja (convenção da Unreal, contrasta com a interface azul).

## Item 9: a janela Editores e as janelas soltas como as da Unreal (2026-11-25, pedido do usuário)
Antes: cada editor de asset era uma janela ImGui solta e única por tipo (um Material só: abrir outro trocava o primeiro);
arrastada para fora virava uma janela do GLFW **sem dono**: um clique no editor a jogava para trás dele (o Material do
outro lado da tela "sumia"), cada uma tinha botão na barra de tarefas, e o swapchain dela pedia MAILBOX (o FIXME do
backend: metade do fps com uma 2ª janela).
- **Janelas com dono** (`ui_viewports.c`): o `Platform_CreateWindow` do backend embrulhado em `src/` (o `external/`
  intocado) faz `SetWindowLongPtrW(GWLP_HWNDPARENT, <janela principal>)` antes de mostrar a janela; `io.ConfigViewports`
  `NoTaskBarIcon` ligado, `NoDefaultParent` desligado, `ConfigDpiScaleFonts`/`Viewports` (o monitor de outro DPI).
  `NoAutoMerge` ficou **desligado**: ligado, toda janela solta virava janela do sistema mesmo sobre o editor e os
  arrastos do dock_test não acoplavam em nada (11 alvos falharam); sobre o editor ela é desenhada dentro dele (acima
  dele de qualquer jeito). O `Renderer_CreateWindow` embrulhado escolhe o present das secundárias
  (`REALENGINE_VP_PRESENT=fifo|mailbox|immediate`; refaz o swapchain uma vez se mudar). **Padrão: mailbox**, medido:
  FIFO prendia o editor inteiro na taxa do monitor (437 -> 165 fps com 2 janelas soltas), mailbox 458 -> 391. Elas
  continuam em SDR (`SRGB_NONLINEAR`, o pedido do backend).
- **Janela Editores** (`editor_host.c`): uma janela com um DockSpace (`rivora_editors_dockspace`) desenhada antes dos
  editores (editor.c) enquanto tem documento; cada editor chama `edhost_doc(&flag)` antes do `igBegin` (a classe
  comum `RivoraAssetEditor`, `DockingAllowUnclassed`) e, ao abrir, entra nela como aba (`SetNextWindowDockID`). Entram
  o Grafo (Blueprint), o Material, o visualizador, o Persona e as Partículas; Sequencer, Diálogo, Missão etc. seguem
  janelas próprias. A primeira vez ela ocupa o retângulo do viewport (o visual do Q26).
- **Material com várias instâncias** (`mat_editor.c`): `MatEd g_med[4]`, `E` é a que desenha (`g_me`); cada uma com a
  câmera do preview, a chave da GPU (`BP_PV_KEY_MATERIAL` e `0x230 + slot`) e os caches que eram `static` de função
  (seleção, UDIM, quadros animados). O visualizador e o Persona seguem únicos: o preview deles é o `AssetViewport`
  compartilhado (multi-instância = mais alvos do `render_preview`, `PV_MAX` 8).
  **Limitação conhecida**: um visualizador (Static Mesh/Textura) e um Persona de cada vez. Cada um fica ao lado dos
  outros tipos na janela Editores, mas abrir uma segunda malha troca a que estava aberta.
- **Fechar**: a troca de projeto e o fechar usam o `editor_docs.c` (bug-fixer); o layout do projeto
  (`Saved/Layout.rlayout`) ganhou `[RealEngine][Documents]` com `open=/Content/...` (Grafos, Materiais, visualizador)
  e reabre os que ainda existem; o layout é salvo antes da troca. Layouts com nome não levam documentos.
- O Grafo de classe não usa mais o lugar do Q26 quando vai para a janela Editores (`SetNextWindowPos` desacopla a janela
  no ImGui: `w->placed = 4`); a própria Editores toma aquele lugar.
- **Teste**: `dock_targets` estendido (`dock_test_docs.c`): Grafo + Material abas de uma janela; 3 janelas próprias
  (a Editores, o Material solto, a do teste) com `GW_OWNER` = a principal e o present pedido; a principal levantada
  (`SetWindowPos HWND_TOP` sem ativar) e elas continuam acima; fps com 0 e com 3; troca de projeto com 0 documentos,
  0 janelas e 0 erros de validação (os "imagem não adquirida" de cada janela solta vinham do próprio backend do ImGui:
  `ImGui_ImplVulkanH_CreateOrResizeWindow` fazia a transição das imagens sem acquire; consertado pelo bug-fixer com a
  marca [Real Engine] em imgui_impl_vulkan.cpp); o editor fecha com 2 janelas soltas e nenhuma sobra (`dock_test_after_shutdown`).
  `--open-asset "a;b"` abre vários (as fotos).
- **Medidas** (RTX 3060, 2560x1417, 2º monitor, `dock_targets`): sem janela solta 469-597 fps (1,7-2,1 ms); com 3
  janelas soltas (mailbox) 393 fps (2,54 ms): +0,4-0,8 ms, o swapchain e o desenho de cada uma. FIFO: 165 fps.
