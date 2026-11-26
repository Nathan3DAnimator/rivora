# Marca: ícone, intro e tela de carregamento (`src/brand.c`)

## Arquivos (`data/branding/`)
- `icon.ico` (16, 24, 32, 48, 64, 128, 256 px, PNG dentro) e `icon.png` (256): desde 24/11/2026 o **R azul da abertura**
  (o "R" com a órbita do `logo.png` como máscara, no degradê do Azul Real: claro em cima, #2B6FE3 no meio, fundo
  embaixo; fundo transparente com sombra suave), gerado por `python tools/make_icon.py` (Pillow + numpy). Antes era o
  R prateado sobre preto de outubro, que ficou nos exes depois da troca de nome (o pedido do usuário: "o ícone não
  mudou"). O `.rc` (`realengine.rc.in`, `GLFW_ICON`) vai em **todos os exes**: `rivora.exe`, `rivora_game.exe`,
  `rivora_updater.exe`, `rivora_mcp.exe` e o lançador `realengine.exe` (o atualizador e a ponte MCP não tinham ícone).
  O GLFW registra a classe das janelas com o `GLFW_ICON` do exe: a janela do editor, as janelas soltas do ImGui
  (viewports) e a do jogo pegam o mesmo ícone sem `glfwSetWindowIcon`; a abertura em camada carregava o id 1 (nada)
  e agora carrega `GLFW_ICON`. A marca da barra de menu (`brand_mark`) já era o R tingido com o acento do tema.
  Não há atalho nem instalador no código (o pacote é um zip; o atalho antigo aponta para o `realengine.exe`, que tem
  o ícone). O Windows guarda os ícones em cache: um atalho fixado pode mostrar o antigo até o cache renovar.
- Teste `exe_icon` (área editor): `tools/check_exe_icon.py` lê o `GLFW_ICON` de cada exe (LoadLibraryEx como dados,
  o grupo e os RT_ICON) e compara byte a byte com o `icon.ico`, tamanho por tamanho.
- `logo.png` (512²): o R para a intro. `splash.jpg`: a tela do usuário **sem os textos e a barra pintados** (os cantos,
  "Create • Render • Play", status, barra, rótulos, copyright): a engine os desenha ao vivo e traduzidos.
- Gerados por um script PowerShell (System.Drawing; não há Python na máquina): recortes, ICO com PNGs, e o fundo
  limpo por região (interpolação vertical no preto; clone da faixa de cima ou de baixo onde há textura).
- Embutidos no executável (`tools/embed_bin.cmake` → `build/generated/brand_*.inc`), decodificados pelo WIC
  (`mat_load_image_rgba_mem`).

## Intro (3,4 s; clique, Esc, Espaço ou Enter pulam)
Pontos se juntam numa esfera girando → um icosaedro em arame (a geometria) → uma luz varre e revela o R polido (o
render) → brilho → "REAL ENGINE" com o espaçamento das letras fechando → "CRIAR • RENDERIZAR • JOGAR" → fade. Antes do
Project Browser (e da splash) no editor e no jogo empacotado (Project Settings > Packaging > Show the Engine Intro).

## Tela de carregamento
- Editor: na partida (enquanto o renderer e o editor iniciam) e ao abrir um projeto (o Project Browser agenda a
  abertura, `editor_open_project_later`, e o `main.c` a faz entre quadros; cada etapa do `editor_open_project`
  chama `g_load_progress`). Jogo: enquanto os shaders, o nível e a iluminação carregam.
- Animação: zoom lento do fundo, poeira, luz cruzando o logo a cada 4 s, barra suavizada pelo tempo com brilho na
  ponta e um reflexo correndo, porcentagem; versão e textos traduzidos.
- **Do projeto**: Project Settings > Packaging > Loading Screen: imagem (PNG, JPEG, BMP, TGA) ou **vídeo** (MP4, WMV,
  MOV, AVI, MKV, WebM; Media Foundation, em loop, reduzido a ~1920 de largura). Um arquivo de fora é copiado para
  `Content/Splash` e vai com o jogo empacotado. Com imagem/vídeo próprio só a barra e o status ficam por cima.
- Testes e linha de comando: `--no-intro`, `--intro` (força com flags de teste), `--splash-hold N` (segura a tela N
  segundos), `--splash-file <arquivo>`. Nos testes automáticos (`--bench`, `--bp-test`...) nada disso aparece.

## Medidas
- Splash/intro: só ImGui (quadros completos sem cena); o editor vazio segue a 474 fps depois de aberto.
- Vídeo 4500×3000 (o `oobe-intro.mp4` do Windows): decodificado e reduzido para 1500×1000 por quadro.

## Carregar antes de mostrar o editor (2026-11-02)
Os materiais sujos são assados na tela de carregamento (`mat_bake_pending`, "Assando os materiais... <nome>"): com a
Delight o editor abria e travava ~3 s (11 materiais 2048², 0,2-0,7 s cada). A tela some com um fade de 0,45 s sobre os
primeiros quadros do editor (`brand_splash_fade`).

## Abertura transparente como a do Blender (2026-11-23)
Pedido: "a primeira animação mais semelhante com a do Blender, fundo transparente, bem smooth; as cores do R azul".
- `src/brand_intro_win.inc` (incluído pelo `brand.c` no Windows): janela **em camada** (WS_EX_LAYERED +
  `UpdateLayeredWindow`, alfa por pixel pré-multiplicado) composta na CPU por uma **thread própria**. O `main.c` chama
  `brand_intro_start` logo depois do `glfwInit`: a abertura toca **enquanto** o Vulkan e o ImGui iniciam, com a janela
  do editor criada escondida; `brand_run_intro` só espera ela acabar e o `main.c` mostra o editor maximizado e focado.
- Animação (~2,6 s, 60 quadros por segundo por timer de alta resolução): o R (máscara do `logo.png`, como o
  `brand_mark`) cresce com um leve passar-e-voltar, gradiente do azul do Azul Real (#2B6FE3, mais claro no topo, mais
  fundo embaixo), sombra suave e brilho azul atrás; uma faixa de luz cruza o R; "Real Engine" (Sora SemiBold, a fonte do
  título da barra de menu, carregada da memória por `AddFontMemResourceEx` via `theme_title_font`) e a versão sobem
  com uma linha azul entre eles; tudo some no fim. Clique, Esc, Espaço ou Enter: some em 0,22 s.
- O jogo empacotado segue com a intro antiga dentro da janela (`brand_run_intro` sem a thread).
- A barra da tela de carregamento passou para o mesmo azul (degradê do fundo para a ponta, brilho azul).
- Testes: `intro_window` (`--intro --bench 2`: imprime os quadros e o custo). `REALENGINE_INTRO_SHOTS=<prefixo>`
  grava 6 quadros em PNG (metade sobre fundo escuro, metade sobre claro) para conferir o visual. Fundo: no
  `REALENGINE_BACKGROUND` a janela fica fora das telas; no `REALENGINE_MONITOR` centra naquele monitor, sem foco.
- Medida: 620 × 480 (100% de DPI), **3,7 ms por quadro** compostos (pior 4,75 ms) numa thread à parte, 153 quadros em
  2,6 s; a partida do editor não espera mais os 3,4 s da intro antiga (ela corre junto da inicialização).
