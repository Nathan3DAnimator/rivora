# O jogo empacotado (`game_main.c`, Build > Package Game)

## O que é
- **`realengine_game.exe`**: o runtime do jogo, sem editor nem ImGui (780 KB). Carrega o nível e a iluminação
  compilada (`.rlight`), roda os sistemas do jogo (`systems_update`) e renderiza na tela inteira com o mesmo renderer
  do editor (biblioteca `re_render`: render, GI, luzes, contexto Vulkan, `level.c`), sem nada do editor: sem UI, sem
  ícones de atores, sem seleção, sem grade.
- **`game.ini`** ao lado do executável: `title`, `level` (relativo ao executável), `scalability` (0–3, o mesmo preset do
  editor: `render_scalability`), `fullscreen` (tela cheia sem borda no monitor principal, F11 alterna), `vsync`.
- Câmera: a câmera do nível com Auto Activate; senão um espectador no Player Start (mouse olha, WASD/Q/E move, Shift
  mais rápido, roda do mouse a velocidade), como o Play in Editor. Esc fecha. Os controles do jogo de verdade vêm com
  os Blueprints.
- Linha de comando: `--level <arquivo>`, `--windowed`, `--scalability <0-3>`, `--bench <s>` `[--min-fps <n>]` (mesma medida
  do editor), `--validation`. Crash: o mesmo relatório do editor (pasta `crashes/` ao lado, ou `%APPDATA%`).

## Empacotar (Compilar > Empacotar Jogo...)
Salva o nível (pede um caminho se ainda não tiver), pede a pasta de destino e cria `<destino>/<Nível>/`:
`<Nível>.exe` (o `realengine_game.exe` renomeado), `Content/<Nível>.rlevel` e `.rlight`, `game.ini` (o preset de escala
atual do editor) e as DLLs do runtime C quando o editor as tem ao lado (o pacote do editor tem). Abre a pasta no
Explorer. Empacotar de novo na mesma pasta substitui os arquivos (e apaga o `.rlight` antigo se o nível não tem mais
luz compilada). `realengine --save X.rlevel --package <pasta>` faz o mesmo sem diálogos (os testes).

## Código compartilhado
- `level.c`: ler/gravar `.rlevel` (antes dentro do `editor.c`), o caminho do `.rlight`, a base da câmera, a câmera
  com Auto Activate e o Player Start. `EdCamera` é o `LevelCamera`.
- `render_scalability` (render.c): os presets Baixo/Médio/Alto/Épico (MSAA e GI), usados pelo editor e pelo jogo.

## Medidas (RTX 3060, driver 617.42, Alto, MSAA 4x, sem vsync)
| Cena | Jogo (2560×1440, tela cheia) | Editor (viewport ~2100×1350 + UI) |
|---|---|---|
| Nível padrão | 491 fps · CPU 0,27–0,29 ms · GPU 1,96 ms | 471 fps · CPU 0,42–0,44 ms · GPU 2,05 ms |
| 1 M objetos (nível salvo) | 162–168 fps · CPU 0,34–0,41 ms · GPU 5,9–6,1 ms | 177–178 fps · CPU 0,54–0,59 ms · GPU 5,5 ms |

O jogo tira ~35% da CPU do frame (sem ImGui nem editor) e rende ~30% mais pixels (a tela toda); na cena de 1 M
objetos a câmera também é outra (a do Player Start). Em cenas pesadas a GPU manda: para 350 fps nelas o próximo passo
é renderizar abaixo da resolução nativa e reconstruir (TAA com upsampling; DLSS onde houver).

Testes: `game_package` (o editor salva o nível padrão e empacota) e `game_fps` (o jogo do pacote, com a validação,
acima de 350 fps; 495 medidos).

## A primeira execução (cache de pipelines)
O driver da NVIDIA guarda os shaders compilados **por executável** (caminho e nome): um jogo recém-empacotado é um `.exe`
que ele nunca viu e compilava os 22 pipelines de novo. Medido (RTX 3060, nível padrão): **4,6 s até o primeiro quadro**
(4,2 s em `vkCreate*Pipelines`); o mesmo `.exe` na 2ª vez 0,42 s (o cache do driver); renomeado, frio de novo (4,65 s).
- Agora o motor tem o seu: `VkPipelineCache` em `%LOCALAPPDATA%\RealEngine\pipelines.bin` (`vkctx.c`), lido no início
  (só se o cabeçalho for desta GPU e deste driver: vendor, device, `pipelineCacheUUID`), gravado depois do renderer
  (um crash depois não perde) e ao sair, se cresceu; gravado ao lado e movido por cima (dois programas ao mesmo tempo).
  Editor, ImGui e todos os jogos usam o mesmo arquivo (mesmos shaders, ~1,1 MB): **um jogo empacotado nesta máquina já
  abre quente: 0,35–0,44 s, 22 pipelines em 6–60 ms**. Acima de 64 MB (shaders velhos acumulados) recomeça vazio.
  `REALENGINE_PIPELINE_CACHE=<arquivo>` usa outro (os testes).
- O jogo imprime `Start: first frame N ms after launch; P pipelines in M ms (pipeline cache: warm|cold)` e os quadros dos
  2 primeiros segundos; `--max-start-ms <n>` falha acima disso. O `--bench` agora mede a janela inteira mesmo quando o
  primeiro quadro atrasa (antes encolhia: 0,79 s em vez de 2 s na execução fria).
- Teste `game_first_run` (`tools/test_first_run.cmake`): empacota um jogo com nome inédito (o driver nunca viu o `.exe`),
  com um cache só do teste que o editor preenche; o jogo, com a validação, tem de mostrar o 1º quadro em menos de
  1,5 s e passar de 350 fps desde o início. Roda também uma cópia fria (outro nome, cache vazio) só para registrar.
  Medido: **quente 444 ms, 498 fps; frio 4572 ms**.
- No PC de um jogador a primeira execução ainda compila (o cache é desta GPU/driver); as seguintes abrem quentes.

## Falta
- Upsampling temporal (resolução de renderização < tela) e DLSS; preset de escala no menu do jogo.
- Vários níveis e um nível inicial escolhido no pacote; ícone do executável; empacotar os assets `.rasset` quando
  existirem.
- Compilação "Shipping" (LTO, sem os timestamps de medida).

## Meta de 250 fps e primeira execução num projeto limpo (2026-11-04)
- Os testes cobram no máximo 250 fps (RealEngine.md); cenas pesadas (multidões, luzes em escala) mantêm a própria meta.
- `game_first_run` empacota um projeto em branco (`--new-project .../Blank --template 0`): o `Content` da build tem o que
  os outros testes deixam (89 malhas, 17 MB) e o jogo carregava tudo (1,56–1,65 s > 1,5 s). Agora 437 KB, 1,0–1,1 s.
  `--new-project` abre o projeto na hora também com `--save` / `--package` (antes só com `--import`).
- Frio (sem o cache de pipelines, PC de um jogador na 1ª vez): **~28,5 s, 37 pipelines** — a 0.14 dá o mesmo hoje (driver
  617.42); a nota de 4,6 s acima era de 22 pipelines. Próximo passo: compilar os pipelines em paralelo / sob demanda.

## Package Game: tudo num lugar só (sugestão #37, 2026-11-04)
Relato: faltavam opções (ícone etc.) e estava espalhado. Agora **Compilar > Empacotar Jogo** e **Plataformas > Empacotar
Projeto** abrem a janela **Package Game** (`project_ui.c`), com cabeçalho (o ícone como vai ficar, o nome, a versão, a
configuração) e as seções: **Jogo** (Nome do Jogo = nome do .exe, vazio = o do nível; Projeto, Empresa, Versão, Descrição,
Direitos Autorais), **Ícone**, **Níveis** (Nível Inicial = Game Default Map; todos os níveis de Content vão junto;
incluir todos os assets), **Janela** (título, resolução, tela cheia, VSync, capturar o mouse), **Gráficos** (escala,
ray tracing), **Tela de Carregamento** (imagem/vídeo, intro), **Compilação** (Desenvolvimento / Distribuição, pasta de
saída) e o botão azul **Empacotar Jogo** com o resultado. Os mesmos valores do Project Settings (ficam no `.rproject`) e
o `--package` usa todos.
- **Ícone e versão no .exe** (`package_res.c`): `BeginUpdateResource` na cópia do `realengine_game.exe`. O ícone da
  engine (grupo `GLFW_ICON`, na língua em que foi compilado) sai e entra o do jogo com o mesmo nome: Explorer, barra de
  tarefas e a janela (o GLFW carrega `GLFW_ICON`) mostram o do jogo. Um `.ico` vai como está; uma imagem (.png, .jpg,
  .bmp, .tga) vira 256, 64, 48, 32 e 16 px de 32 bits (filtro de caixa ponderado pelo alfa, encaixada no quadrado com
  margem transparente). A versão (`VS_VERSIONINFO`): Empresa, Produto, Descrição, Versão (também nos números fixos),
  Direitos Autorais, nome do arquivo. A imagem escolhida é copiada para `Content/Branding`.
- **game.ini** ganhou `width`/`height` (a janela fora da tela cheia, dentro do monitor), `shipping` (sem o Debug Viewer
  no F10) e `version`; `level` aponta para o Nível Inicial onde ele está em Content.
- Custo: só ao empacotar (o ícone de 5 tamanhos e a versão: alguns ms). Testes `game_package_icon` (logo.png → "icon of 5
  images") e `game_icon_runs` (o jogo com os recursos novos abre e roda).

## Android
Compilar > Empacotar para Android: o mesmo pacote virado APK (arm64, Vulkan 1.3): [android.md](android.md).

## Shipping: o pacote único e o LTO (2026-11-09, plano de melhorias B15a)
- **`Content.rpak`** (`src/rpak.c`): no Shipping do PC (ou `REALENGINE_PACK=1`), ao fim do Empacotar, a pasta `Content`
  do pacote vira um arquivo: cada arquivo comprimido por deflate (o zlib da engine; guardado cru quando não economiza
  10%), índice ordenado (caminho com a caixa original, comparado sem caixa), FNV-1a 64 do índice e dos dados conferidos
  ao montar (um byte mudado: não abre). A pasta solta é apagada. O jogo monta `<exe>/Content.rpak` como `<exe>/Content`
  e as chamadas de arquivo da plataforma respondem por ele: `re_map_file` (descomprime na memória), `re_fopen` de leitura
  (num arquivo temporário que o sistema apaga ao fechar), `re_file_stat` e `re_dir_walk` (pastas pelo índice). O resto
  (saves, configurações) segue no disco. Testes `game_package_rpak` e `game_rpak_runs` (o jogo do pacote único, com a
  validação). Fora do Android (o APK já é um zip).
- **LTO:** `-DREALENGINE_LTO=ON` liga /GL + /LTCG em `re_core`, `re_render`, no editor e no jogo (o build da release).
- **Medidas** (RTX 3060, 1440p): o pacote de teste, 299 arquivos, 23,5 -> 12,0 MB em 1,6 s ao empacotar; o jogo do
  pacote único 382-396 fps (o mesmo do solto). LTO: o jogo 23,30 -> 23,45 MB (mais inlining), 382/396/393 fps contra
  372/395/393 sem ele, CPU 0,62-0,68 ms nos dois: nessa cena (limitada pela GPU) não muda nada que se meça; fica opcional.
- **Falta:** zstd (a engine só tem o descompressor do Tlender: deflate por ora), assinatura do pacote (hoje um hash de
  integridade), PGO, ler sem descomprimir o arquivo inteiro (blocos).

## Saída HDR10 (2026-11-09, sobras da Fase 2, A9.4)
- Project Settings > Platforms: HDR Output (HDR10), Paper White, Peak Brightness -> `game.ini` `hdr=1`, `hdr_paper`,
  `hdr_peak` (`REALENGINE_HDR=1` força, nos testes).
- vkctx: a instância pede `VK_EXT_swapchain_colorspace` quando existe (`hdr_possible`); `want_hdr` + recriar a swapchain
  escolhe A2B10G10R10 em `HDR10_ST2084` (`hdr`). O jogo liga isso depois da intro e da tela de carregamento (desenhadas
  por ImGui direto na swapchain SDR), só com a swapchain B8G8R8A8 (o formato do pipeline do ImGui).
- A UI vai para um alvo próprio B8G8R8A8 (`render_hdr_ui_begin/end`, criado no `render_scene` antes de algo ligar o set;
  binding 81); o composite (pipelines refeitos quando o formato da swapchain muda) põe a imagem no Paper White, abre os
  claros além do branco até o pico (pela luminância antes do tone mapping), aplica o Fade, coloca a UI por cima (sRGB
  pré-multiplicado -> linear x paper white), converte BT.709 -> BT.2020 e codifica em PQ.
- Teste `game_hdr10` (o pacote rpak com `REALENGINE_HDR=1`: "HDR10 output: on" ou "unavailable", sem erros). Nesta
  máquina a janela do jogo não recebe HDR10 (o HDR do Windows desligado): o caminho PQ não foi visto numa tela HDR.
- Falta: metadados HDR (VK_EXT_hdr_metadata), HDR no editor, scRGB, calibração no menu de opções.

## Blueprint apagado e o Empacotar Jogo (2026-11-25, bug-fixer)
- Um Blueprint cujo arquivo foi apagado (Content Browser, lixeira ou fora do editor) continua registrado na memória; o
  pacote o deixa de fora ("Package: ... left out (its file was deleted)") em vez de falhar com "não foi possível
  copiar". Teste `blueprints_after_net`.
