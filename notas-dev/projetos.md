# Projetos: Project Browser, Project Settings, Editor Preferences

## O projeto
Uma pasta com `<Nome>.rproject` (JSON, `project.c`) e `Content/` (os assets, `Content/Maps/` os níveis). Toda chave é
opcional: um projeto antigo abre com os padrões novos. Sem projeto (os testes, a pasta ao lado do executável) valem os
padrões e o `Content` do executável.

`editor_open_project` (editor.c) troca **todos** os sistemas para o `Content` do projeto: Content Browser, Blueprints
(`bp_shutdown` + `bp_init`), materiais (+ os da engine, quando faltam), animação (Mannequin), IA (exemplos), malhas
importadas (`render_set_content_root` + `render_meshes_reload`); limpa o Desfazer e abre o **Editor Startup Map**.

Os projetos recentes ficam em `%APPDATA%/RealEngine/projects.txt` (mais novo primeiro). Execuções automáticas
(`--bench`) não mexem na lista.

## Project Browser (project_ui.c)
Janela antes do editor, como a da Unreal: aparece quando o editor abre sem argumentos e a preferência "Mostrar o
Navegador de Projetos ao iniciar" está ligada; também em Arquivo > Novo/Abrir Projeto.
- **Projetos Recentes**: tiles com miniatura, duplo clique abre, botão direito remove da lista / mostra no Explorer,
  "Procurar..." abre qualquer `.rproject`.
- **Jogos** (templates): Em Branco, Terceira Pessoa, Primeira Pessoa, Visão de Cima, Playground de Física; descrição,
  Predefinição de Qualidade e Ray Tracing (vão para o projeto), local e nome (regra da Unreal: letra primeiro, letras,
  dígitos e `_`, até 20).
- **Miniaturas**: sem render target nem textura. Ao salvar um nível do projeto, `project_save_thumbnail` grava em
  `<projeto>/Saved/Thumbnail.rthumb` as até 160 maiores formas básicas do nível (forma, eixos, origem, cor, rugosidade,
  metal: 76 bytes cada, ≤ 12 KB); o browser as desenha com `bp_preview_shapes` (o mesmo desenho do Viewport das
  Blueprints). O enquadramento ignora paredes enormes e o chão é cortado ao que está em cima dele. Os templates têm
  miniaturas feitas no código.
- Enquanto o browser cobre o editor, a cena não é renderizada (`render_view.w = 0`).

## Templates (`editor_build_template`)
| Template | O que cria |
|---|---|
| Em Branco | chão com M_Grid, sol, céu, Player Start |
| Terceira Pessoa | percurso (plataforma, rampa, degraus, parede, caixas com física) + `BP_ThirdPersonCharacter` (o Character padrão: Spring Arm 350, câmera, Mannequin) |
| Primeira Pessoa | percurso + `BP_FirstPersonCharacter` (braço de 0 cm nos olhos, sem corpo, arma presa à câmera) + `BP_Projectile` (esfera emissiva, Projectile Movement 3500 cm/s, quica). Botão esquerdo → Spawn Actor from Class na posição dos olhos + 90 cm à frente, com **Get Control Rotation** (nó novo) |
| Visão de Cima | percurso + `BP_TopDownCharacter` (braço de 1400 cm a -55°, sem girar com o mouse: os nós de Yaw/Pitch saem do grafo) + 3 guardas com o Real Brain `Brain_Guard` |
| Realidade Aumentada | chão (o real no AR), `BP_ARObject` e `BP_ARPlacer` (tocar no chão real põe uma caixa: Event Touch → AR Line Trace → Spawn); Project Settings > Android com AR e oclusão (notas-dev/xr.md) |
| Playground de Física | a cena de física (pirâmide, torre, bolas na rampa, caixa que quebra) + Character em terceira pessoa |

O template salva o nível como `Content/Maps/<X>Map.rlevel`, que vira Editor Startup Map e Game Default Map, e põe a
classe como **Default Pawn** do projeto.

## Project Settings (Editar > Configurações do Projeto)
Categorias como na Unreal (salva no `.rproject` a cada mudança):
- Projeto: **Descrição** (nome, empresa, versão, descrição), **Mapas e Modos** (Editor Startup Map, Game Default Map com
  "Usar o Atual"; GameMode, Default Pawn, Player Controller, HUD, Game State, Player State, Game Instance — combos com as
  classes Blueprint filhas de cada classe nativa), **Empacotamento** (pasta de saída, incluir todos os assets).
- Motor: **Física** (gravidade Z, máximo de subpassos de 120 Hz), **Entrada** (teclas de MoveForward/MoveRight/Jump por
  captura de tecla, sensibilidade e inverter Y do mouse), **Renderização** (escalabilidade do jogo, ray tracing,
  iluminação estática, exposição automática e compensação).
- Plataformas: **Windows** (título, resolução, tela cheia, VSync, capturar o mouse).

Quem usa: `physics.c` (gravidade, subpassos), `editor.c`/`game_main.c` (teclas, mouse), `project_begin_play` (classes do
Game Mode e o Default Pawn no Player Start, se o nível não deu um pawn ao jogador), o empacotamento (`game.ini` e
`Game.rproject` no pacote).

## Editor Preferences (Editar > Preferências do Editor; `%APPDATA%/RealEngine/editor.ini`)
- Geral: idioma, escala da interface (`FontScaleMain`), tamanho das miniaturas, salvamento automático (minutos), mostrar o
  Navegador de Projetos ao iniciar, **confirmar antes de sair com mudanças não salvas** (diálogo Salvar e Sair / Não
  Salvar / Cancelar, ligado por padrão).
- Viewport: velocidade da câmera, campo de visão, sensibilidade e Y invertido, passos de encaixe, grade/ícones/stats.
- Desempenho: usar menos CPU em segundo plano (dorme 30 ms por quadro sem foco), limite de quadros, VSync,
  escalabilidade do editor. Os dois primeiros não valem durante uma medida (`--bench`, Ferramentas > Medir;
  `Editor.measuring`): o `editor.ini` é um só para todos os worktrees e um teste cuja janela perdia o foco para outra
  dormia 30 ms por quadro (`preview_anim_bp` 29-40 fps, "CPU 35 ms", em vez de ~500; 2026-11-04).
- Jogar: começar possuído ou ejetado.
- Atalhos de Teclado: a lista dos atalhos do editor.

## Linha de comando
`--project X.rproject`, `--new-project <pasta>\<Nome> --template N` (0 Em Branco … 4 Física), `--project-browser`,
`--show prefs|settings|games` (screenshots).

## Medidas (RTX 3060, 2560x1417, mailbox, jogando)
| Template | fps | CPU | GPU |
|---|---|---|---|
| Em Branco | 816 | 0.52 ms | 1.15 ms |
| Terceira Pessoa | 506 | 0.71 ms | 1.90 ms |
| Primeira Pessoa | 576 | 0.64 ms | 1.66 ms |
| Visão de Cima (3 guardas com IA) | 318 | 0.80 ms | 3.06 ms |
| Playground de Física | 395 | 0.87 ms | 2.46 ms |

Criar um projeto (pastas, assets padrão, classes, nível salvo) leva menos de 1 s. As janelas não custam nada fechadas;
o browser aberto não renderiza a cena.

Testes: `project_third_person`, `project_first_person` (com validação, ≥ 300 fps), `project_top_down` (≥ 200 fps),
depois de `projects_clean` (fixture que apaga `build/test_projects`).

O pacote do jogo agora leva também `.rskm .ranim .rmontage .rbt .rbrain` e as outras extensões de asset; antes o
`SK_Mannequin.rskm` ficava de fora e o Character do jogo aparecia sem corpo. Medida do teste `game_blueprints`
(A/B com o commit anterior, mesmo nível): 393 → 343 fps, GPU 2,47 → 2,83 ms (o Mannequin perto da câmera, main pass
1,86 → 2,2 ms). O limite do teste passou de 350 para 300 fps, como os outros testes com Character e validação.

## Pendências
- Empacotar não limpa a pasta de saída: um asset apagado do projeto continua no pacote antigo (apagar
  `<saída>/<Jogo>/Content` antes de copiar, com confirmação).
- `play_flicker` já falhava antes deste trabalho (~320 fps contra o limite de 350, medido no commit anterior também).
- Miniatura dos recentes só com as formas básicas (as malhas importadas pertencem ao renderer do projeto aberto).
- Mais categorias de template (Veículo, Realidade Virtual) e "Starter Content".

## Modos de renderização: Desempenho e Qualidade (sugestão #45, 2026-11-04)
- **Onde se escolhe**: ao criar o projeto, no Project Browser ("Rendering Mode", ao lado da predefinição). Depois, em
  Project Settings > Rendering e em Package Game > Graphics. Fica no `.rproject` (`render_mode`, `game_path_tracing`).
- **Desempenho**: o caminho de sempre, 250+ fps, a base para VR e PCs fracos. A predefinição do jogo vai até Épico.
- **Qualidade**: usa a escala nova **Cinemática** (nível 4, `render_scalability`), acima do Épico.
  - Sombras de área com 16 raios.
  - Cache de superfície a 7,5 cm com 16 raios e 256 mil texels por quadro.
  - Reflexos traçados até rugosidade 0,8.
  - Ao criar, o editor também passa a Cinemática, e a Cinemática aparece em todos os menus de escala.
- **Path Tracing no Jogo** (só em Qualidade, com ray tracing): o `game.ini` recebe `path_tracing=1` e o jogo usa como
  vista o path tracer, que já reprojeta o histórico e faz denoise (`pathtrace.comp`). Sem ray tracing, o jogo usa Lit.
- **Upsampling temporal tipo TSR**: não entrou nesta etapa. A engine não tem TAA nem jitter, e fazer isso direito
  (jitter na projeção, histórico, clamp de vizinhança, render em resolução menor) é uma área própria com teste de
  flicker. O denoise temporal do path tracer cobre o item 5 da sugestão.
- **Testes**:
  - `project_quality`: Third Person criado em Qualidade, jogado, > 90 fps (230 medidos).
  - `game_path_tracing`: o jogo do `game_pkg` com `--path-tracing`, > 60 fps (186 medidos).

| RTX 3060, 2560×1440, Third Person | fps | GPU |
|---|---|---|
| Desempenho (Épico do editor) | 330 | 1,9 ms |
| Qualidade (Cinemática) | 230 | 4,2 ms |
| Jogo empacotado: Lit / path tracing | 408 / 186 | 1,7 / 4,9 ms |

## Project Browser no azul do R (2026-11-23)
- Cabeçalho numa faixa do `accent_dim` ao fundo: o R da barra de menu (`brand_mark` tingido do acento, metade de cima
  mais clara), "Real Engine" e a versão no acento, a dica da página ("Abra um projeto recente..." / "Escolha um modelo...").
- Coluna da esquerda com `nav_item` (ícone, barra azul e fundo `accent_dim` na página aberta) e mais larga (15,5 em).
- Cartões (`tile`): cantos arredondados, sombra em 3 camadas, sobem 2 px sob o mouse com borda azul fraca, o
  selecionado com borda e halo azuis (antes laranja), prévia sobre degradê escuro, nome centrado.
- Abrir/Criar em azul (`primary_push`: acento, texto sobre acento, cantos arredondados).
