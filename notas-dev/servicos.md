# Relatórios de crash, bug reports e atualizador

Copiados do Tlender e adaptados (prefixo `re_`, nomes, pastas): `crashlog.c`, `report_upload.c`, `updater.c`,
`update_install.c`, `updater_main.c` (vira `realengine_updater.exe`), `net_http.c` (WinHTTP), `re_crypto.c` (SHA-256 e
ECDSA P-256 via BCrypt), `zip_read.c` + `re_zlib.c` (inflate/deflate), `io_json.c`, `endpoints.c`. Integração em
`src/services.c`.

## Crash
- `services_early_init`: handler instalado antes da janela, stdout/stderr no log (`<exe>/crashes/../logs/realengine.log`).
- Relatório Markdown + minidump em `<exe>/crashes/RealEngine_crash_<data>.txt/.dmp`, pilhas simbolizadas com o `.pdb`,
  estado do editor (nível, atores, PIE, modo, viewport, passos de desfazer) e **salvamento de emergência do nível**
  (`..._level.rlevel`; não durante o PIE: o mundo do jogo não é trabalho do usuário).
- Erros fatais do Vulkan (`vk_check`) e `IM_ASSERT` (cimconfig.h) viram relatório. Na abertura seguinte, a barra
  de status e o Output Log avisam do crash anterior.
- Teste: `REALENGINE_CRASH_TEST=acesso|divisao|abort|pilha|heap|imgui|gpu...` (medido: relatório + .dmp + nível).

## Envio de bug reports, crashes e sugestões (ligado em 2026-10-31)
- O mesmo Worker e o mesmo repositório do Tlender (`tlender-reports`): `endpoints.ini [reports] report_url` aponta
  para ele, o pedido leva `produto = realengine` e o texto começa com "- Produto: Real Engine" (`product_body`), então o
  visualizador de reports (`../reports-viewer`) os separa mesmo com o Worker antigo. A fila (`report_upload.c`, copiada
  do Tlender) fica em `%APPDATA%\RealEngine\outbox` e é enviada numa thread (`services.c`: `Q`, `kick_flush`); sem
  internet fica para a próxima abertura; o resultado vai para a barra de status.
- Help > Report a Bug: título e descrição, salvos com o contexto do editor em `<exe>/bugReport/` e, com "Enviar aos
  desenvolvedores" (ligado), enviados. Help > Send a Suggestion: nome (opcional), título e ideia. Crash: na abertura
  seguinte a janela "Enviar o Relatório de Crash?" manda o `.txt` e o `.dmp` (o nível de emergência não vai).
- Testado (2026-10-31): sugestão pela janela do editor e um crash real com o `.dmp` chegaram como issues (#21, #20).

## Travamento ao sair da tela de carregamento (0.17, 2026-11-04)
Relato: "crash quando a splash screen tá finalizando e vai abrir o editor". Não era crash: nenhum relatório, e o Log de
Eventos do Windows tinha dois `AppHangB1` (a janela parou de responder e foi fechada pelo usuário), os dois logo depois
de abrir um projeto pelo Project Browser (MyProject; um projeto novo do template RPG), na escala Épico. Não reproduziu
aqui (segundo plano, visível, `--browser-open`, Épico): sem a pilha, duas medidas:
- **Materiais na tela de carregamento**: o `mat_load` só lê os materiais do projeto na primeira busca (`mat_find_hash`),
  que acontecia no primeiro quadro do editor, depois do `mat_bake_pending` da abertura: no MyProject, 4,2 s de editor
  parado enquanto a tela de carregamento sumia (16 materiais de 2048, 0,2-1,2 s cada). `editor_open_project` chama
  `mat_count()` (lê todos) antes do `mat_bake_pending`: o mesmo trabalho com a barra andando; com o vigia em 2 s,
  nenhuma parada na abertura.
- **Vigia de travamento** (`crashlog.c`: `crash_watchdog_start`, `crash_heartbeat`): o laço principal e cada quadro da
  tela de carregamento batem; parada a thread principal 6 s, uma thread a suspende só para ler os registradores e a
  pilha, escreve onde ela está no log (aviso "Travamento") e em `crashes/RealEngine_travamento_<data>.txt` (versão,
  memória, pilha simbolizada, últimas 80 linhas do log), outra vez se ainda estiver parada em 4x, e deixa seguir. Só
  fora dos testes (`REALENGINE_WATCHDOG=<s>` liga neles). Teste `hang_watchdog` (`--hang-test`: um quadro parado 7,5 s;
  o relatório tem de nomear a função parada). Custo: uma troca atômica por quadro.
- `--browser-open <rproject>`: o Project Browser na tela e o projeto aberto 90 quadros depois pelo mesmo caminho do
  clique (`editor_open_project_later`, entre quadros, com a tela de carregamento).
- 0.18: o vigia pegou outro travamento (`RealEngine_travamento_2026-11-04_21-06-07.txt`, a pilha no driver
  dentro de `build_msaa_pipelines`): a primeira malha Masked recompilava as pipelines na thread principal. Agora elas
  são compiladas numa thread (render.md, "Pipelines Masked em segundo plano").

## Intro: crash com multi-viewport (corrigido em 2026-10-31)
A 0.8 publicada fechava ao abrir (IM_ASSERT "Forgot to call UpdatePlatformWindows()"): o multi-viewport está sempre
ligado (`main.c`) e os laços da intro e da tela de carregamento (`brand.c`) não chamavam `igUpdatePlatformWindows()`
depois do `igRender()`. `end_platform_frame` nos dois.

## Atualizador
- **Limite da API (2026-11-25, bug-fixer)**: a consulta ia só à API (`releases/latest`), 60 pedidos por hora por
  endereço; no limite (403/429) não havia atualização, e o teste `update_download` da suíte ficava vermelho. Agora, como
  no Tlender: o relay dos relatórios (`[reports] report_url` + `/releases?repo=<releases_repo>`: o Worker pergunta com o
  token dele e guarda a resposta), depois a API (`/releases?per_page=30`: a mais nova sem rascunho, pre-release nem
  beta), depois o feed público `github.com/<repo>/releases.atom` (o pacote nomeado pelo `*` do padrão, os endereços
  web). `update_relay_url`, `update_parse_atom` (updater.c), o app e o `--update-test`. Testes: `update_parse` (offline:
  a lista e o feed) e `update_download` (pelo relay; SKIP com o motivo se todos os caminhos esbarrarem no limite).
- **504 do GitHub (2026-11-08)**: o caminho web dos arquivos (`github.com/<repo>/releases/download/...`) respondeu
  504 Gateway Time-out por horas enquanto a API (`api.github.com/.../releases/assets/<id>`, `Accept:
  application/octet-stream`) baixava normal. Desde a 0.24 o pacote e o `.sig` vêm pela API primeiro, depois o caminho
  web; cada um com 3 tentativas em erro de rede, 5xx ou 429. Teste `update_download` (`--update-test`, rótulo
  `network`): baixa a Latest de verdade, confere a assinatura e descompacta (0.23: 140 MB em 15,5 s). Versões até a 0.23
  usam só o caminho web: com o 504, baixar pela página da release.
- `[update]` em `data/branding/endpoints.ini` (embutido): `NathanLourencoPeixoto/realengine-releases`,
  pacote `RealEngine-*-win64.zip`, a mesma chave pública do Tlender (a privada fica em
  `C:\Users\Player\.tlender-release\release_signing.key`, fora do repo).
- Ao abrir: checagem numa thread (pula com `REALENGINE_NO_UPDATE_CHECK=1`, que o `--bench` liga). Achou versão
  nova (nunca beta, rascunho ou pre-release): janela com as notas + selo verde na barra de status. "Baixar e
  Instalar" baixa numa thread, confere a assinatura, descompacta; ao fechar o editor o `realengine_updater.exe`
  copia por cima (guardando os antigos até dar certo) e reabre. Help > Check for Updates faz a checagem à mão.

## Releases
- `tools/make_dist.ps1 [-Beta] -SignKey C:\Users\Player\.tlender-release\release_signing.key`: build Release em
  `build-release`, pasta + zip em `dist/` (exe, pdb, updater, runtime do VC, LEIA-ME), `.sig` e `.sha256`.
- **0.2** publicada em 2026-10-30 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.2),
  2,3 MB; o repositório de releases ficou público (regra do CLAUDE.md: privado até a primeira release).
- Testado de ponta a ponta: uma build "0.1" achou a 0.2, mostrou as notas e o selo, baixou e conferiu a assinatura.
- **0.3** publicada em 2026-10-30 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.3),
  2,5 MB: GI híbrida, Build Lighting, luz física (lux/cd/EV100), céu atmosférico, sombras por raio. Pacote testado
  (522 fps no nível padrão).
- **0.4** publicada em 2026-10-30 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.4),
  3,4 MB: Blueprints, GI estável e instantânea (cache de superfície), céu com nuvens e neblina volumétrica, gizmos e
  seleção múltipla, relevância, oclusão automática; o pacote passa a levar o `realengine_game.exe` (Package Game).
  Pacote testado: 541 fps no nível padrão, teste de Blueprints passando.
  A pasta de trabalho é `%LOCALAPPDATA%\Real Engine\update` (com espaço: mantida para as versões se acharem).
- **0.7** publicada em 2026-10-31 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.7),
  4,8 MB, a "Latest"; o pacote baixado do GitHub passou no `realengine_updater --verify` com a chave pública. GI
  corrigida, Debug Viewer, materiais por nodes do Tlender, Skeletal Mesh, animação, Anim Blueprints e Montages,
  importar `.tlend`, componentes da Unreal, física, IA (Behavior Tree e Real Brain), Project Browser com templates,
  Project Settings, Editor Preferences. Pacote testado extraído numa pasta limpa: 517 fps no nível padrão; um projeto
  Terceira Pessoa criado, jogado (509 fps) e empacotado; o jogo roda a 348 fps em Épico (MSAA 8x, 2560x1440).
- **0.8** publicada em 2026-10-31 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.8),
  7,1 MB, a "Latest"; o pacote baixado do GitHub passou no `realengine_updater --verify`. Importar OBJ/glTF/FBX (luzes,
  câmeras, morphs), Auto Rig, docking e multi-janela, Viewport 2, path tracing jogável, Post Process Volume, Enhanced
  Input, Level Blueprint, UMG, Blend Space 2D, Motion Matching, Mannequins e criaturas, IA em esquadrão, multidões,
  Landscape, prototipagem. Pacote extraído numa pasta limpa: 485 fps no nível padrão; projeto Terceira Pessoa jogado
  a 456 fps (RTX 3060, 2560x1417).
- **0.9** publicada em 2026-10-31 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.9),
  7,1 MB, a "Latest"; o pacote baixado do GitHub passou no `realengine_updater --verify`. Envio de bug reports, crashes
  e sugestões; o crash da intro da 0.8 corrigido. Pacote extraído numa pasta limpa: abre e passa pela intro sem crash.
- **0.10** publicada em 2026-11-01 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.10),
  8,1 MB, a "Latest"; o pacote baixado do GitHub passou no `realengine_updater --verify`. AnimGraph como o da Unreal,
  editor de Motion Matching, animações refeitas (IK, rolagem do pé, pêndulo), prévia animada no Content Browser,
  Posicionar Atores/modos/Abrir Asset/Salvar Tudo, renomear ao criar, splash de no mínimo 1,5 s, Delight igual ao
  Tlender (Masked/dois lados, TGA), crashes #22-24 corrigidos (issues fechadas). Pacote extraído numa pasta limpa roda
  o `--bench` sem erros.
- **0.11** publicada em 2026-11-01 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.11),
  8,2 MB, a "Latest"; o pacote baixado do GitHub passou no `realengine_updater --verify`. Motion Matching com poucas
  animações (starts, stops, trocas de velocidade e direções gerados no estilo dos clipes, pés presos, movimento no
  limite do corpo, Distance Matching), pacote de Exemplos de Motion Matching. Pacote extraído numa pasta limpa: 476 fps
  no nível padrão, `--scene mm_examples` passando. Suíte: 51/52 em cada rodada (`game_blueprints` 267-269 fps < 270 já
  falhava antes desta versão; `crowd` oscilou uma vez em 1505 cm e passou nas 3 repetições).
- **0.12** publicada em 2026-11-01 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.12),
  8,2 MB, a "Latest"; o pacote baixado do GitHub passou no `realengine_updater --verify`. Motion Matching com clipes
  autorais primeiro (root motion de qualquer clipe), takes longos como banco único (espelhados), giros pelo clipe,
  trocas por inercialização, Salvar como Animação; exemplos LongTake e Authored. Pacote extraído numa pasta limpa:
  475 fps no nível padrão, `mm_longtake` e `mm_authored` passando. Suíte 53/54 (`game_blueprints` 260 fps < 270, limitado
  pela GPU, já falhava antes).
- **0.14** publicada em 2026-11-01 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.14),
  8,4 MB, a "Latest"; o pacote baixado do GitHub passou no `realengine_updater --verify` (a 0.13 foi só build local: a
  0.14 a inclui). UI enxuta com descrições em todo controle, Details da Unreal, editores de Structure/Enum, Reroute,
  seleção por caixa, Blend Space Editor, triggers, câmera com pós-processamento, Event Dispatchers, Construction Script,
  modo Blackboard, regra de transição em janela, jogos de demonstração (RPG, Terror, Luta). Suíte 58/58; editor
  empacotado 636 fps no nível padrão.

## Relatórios da 0.14 (issues #25-#68, 2026-11-02)
Os 3 crashes da pasta `realengine/crashes` são da 0.9/0.10 e já estavam corrigidos (fae31a6, 9f66195). Feito agora:
- **#52** duplicar a Delight: a cópia ficava sem malha (o `ecs_clone` copiava `CSkeletalMesh.built`, então as partes
  nunca eram feitas). `built`/`inst` zerados no clone; um Skeletal Mesh salvo sem partes as refaz no frame seguinte
  (conserta os níveis já salvos: no MyProject do usuário, 280 → 528 entidades, as 2 cópias com as 124 partes).
- **#38** "Abrir Editor de Skeletal Mesh": abre pela própria malha (antes refazia "/Content/..." do caminho em disco).
- **#47** importar não coloca mais atores no nível: janela **Import Options** (como a da Unreal) para .tlend/OBJ/glTF/FBX,
  "Colocar no Level" desligado por padrão (a linha de comando `--import` continua colocando: os testes contam atores).
- **#26** WASD/mouse no Terror: um segundo `Event Tick` (o do barulho da corrida) substituía o do template (movimento).
  O compilador agora roda todos os BeginPlay/Tick/EndPlay repetidos da mesma classe, em ordem (stub com PUSHRET/JMP);
  a classe filha continua substituindo o da mãe. Conserta projetos já gerados.
- **#33** o rival nascia dentro do ringue (empurrado para fora): nasce em cima dele.
- **#30** moeda/porta destruída deixava a caixa de colisão (as caixas eram feitas só no Play): refeitas no tick seguinte
  a um Destroy Actor. Giro de 360°: o yaw não era normalizado e `fmodf` de negativo ia pelo lado longo; `remainderf`
  (personagem, IA, inclinação da animação, multidão).
- **#42** câmera: Camera Lag ligado no template de terceira pessoa e nos jogos de demonstração; na primeira pessoa a
  altura da vista suaviza nos degraus. (Lag no template base fez o `play_flicker` passar de 0,07% para 0,73%: ficou
  como na Unreal, desligado, e ligado só nos templates.)
- **#27** chão "flicando" no Terror: o piso das salas/corredores (protótipos) ficava no mesmo plano do chão: 1 cm acima.
- **#35/#36** IA: posição do alvo atualizada todo frame enquanto vê e por 1,5 s depois de perder de vista; o alvo visto
  há pouco é "sentido" até 4,5 m fora do cone; último trecho do caminho vai direto ao alvo; repath 0,5 → 0,25 s.
- **#25** nodes/fios piscando no zoom (Blueprint, Behavior Tree, AnimGraph): o zoom era aplicado no meio do frame.
- **#58** Widget Blueprint sem painel Components (como na Unreal).
- **#50/#56** FPS despencando perto da Delight: o "occlusion auto" media 28 quadros *sem* o pré-passe a cada 3 s; com
  dezenas de cartões de cabelo isso é 46 ms/quadro (engasgo periódico). Agora mede com, depois sem, aborta o "sem" assim
  que fica 1,5× pior e só remede em 20 s quando o pré-passe ganha com folga. Medido (Épico, 3 Delights na tela):
  18,1 → 15,9 ms por quadro. O custo restante é o cabelo com MSAA 8x (follow-up: LOD/alpha to coverage).
- **#67/#54/#62** neblina iluminando lugares fechados: luz do céu na neblina volumétrica com 2 raios de visibilidade
  por froxel; a neblina analítica escurece pela razão GI/céu da superfície; o reflexo do céu borra com a rugosidade (a
  parede fosca mostrava as nuvens). Túnel do MyProject: brilho médio da cena 5230 → 1637 nits, GPU 7,56 → 7,33 ms.
- **#49** sombra suave em grade: anéis da espiral deslocados por pixel além da rotação (grão fino em vez de grade);
  4× raios só na penumbra custava 11 ms num personagem de perto (medido, descartado).
- Sugestões feitas: **#46** materiais assados na tela de carregamento (a Delight travava ~3 s depois de abrir) e fade da
  tela sobre o editor; **#40** atalhos no Material Editor (1, 3, T, M, A, L, U + clique; tecla com fio arrastado cria e
  conecta); **#28** soltar fio no corpo do node conecta no pino compatível, duplo clique no fio cria Reroute, B/S/D
  com fio arrastado; **#57** menu do Content Browser como o da Unreal 5 (Create Basic Asset / Advanced por categoria).
- **Pendentes** (grandes, para sessões próprias): #53/#39/#63 prévias e visualizadores com o renderer real (textura,
  modos de visualização, Realtime), #32 reflexos de personagens/estruturas no chão (SSR/RT para superfícies ásperas),
  #55 luz indireta só de perto, #42 landscape preto/sumindo, #48 IK/piscar da animação importada da Delight, #68 modo
  Landscape (heightmap, modo de Foliage), #45 modos Qualidade/Desempenho, Sequencer, path tracing em jogo, #59/#61 UMG
  completo, #60 combos com busca e prévia, #64 Construction Script ao vivo no viewport, #66 vistas ortográficas no
  viewport principal (o Viewport 2 já tem), #65 mapa de calor de custo no 3D, #51 streaming de texturas, #44 nuvens,
  #37 opções de empacotamento num lugar só (ícone), #41 painel Posicionar Atores, #29/#31/#34/#43 demos com modelos e
  mecânicas de verdade.
- **Segunda rodada (2026-11-02, issues #69-#72)**: #69 criar pasta no Outliner escondia todos os atores fora dela (só
  `!tree` os listava); #70 o breadcrumb "AnimGraph" ficava sob o botão invisível do canvas e não recebia o clique (agora
  `SetNextItemAllowOverlap`, seta de voltar, Alt+Esquerda e o botão "voltar" do mouse). Relatórios revisados apagados do
  `tlender-reports` e 39 issues fechadas (bugs corrigidos, sugestões em `../Sugestões para analise/2026-11-02 RE ...`,
  crashes já corrigidos, inclusive a #20 da 0.8). Investigado e **não** corrigido: #30/#27 o "piscar" é o ruído do cache
  de superfície (0,45% no RPG parado, igual à 0.14; céu/nuvens e objetos; não é o vento das nuvens), #42 landscape
  (charts novos começam sem história e o cache realoca ao andar: suspeita, sem reprodução andando). Ficam para sessões
  próprias: #30/#27/#42 (cache de GI), #48 (IK/animação da Delight), #55/#32 (GI de longe, reflexos), #53 (prévias com o
  renderer), #68 (modo Landscape).
- **0.15** publicada em 2026-11-04 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.15),
  9,6 MB, a "Latest"; o pacote baixado do GitHub passou no `realengine_updater --verify`. Todos os relatórios da 0.14
  corrigidos (issues #25-#72 fechadas): Delight (cópia, IK, sombra do cabelo), Import Options, Landscape por heightmap e
  Foliage, prévias com o renderer, GI de toda a cena, neblina em lugar fechado, reflexo do chão, atalhos da Unreal,
  Blueprints sem perder fios ao abrir. Meta de FPS em 250. Suíte **64/64 em 3 rodadas seguidas**; o editor do pacote
  extraído numa pasta limpa faz 643 fps no nível padrão.
- **0.16_b** (beta) publicada em 2026-11-04 como pre-release (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.16_b),
  127 MB (agora com `DemoContent`, os assets CC0 das demos); o atualizador da 0.15 não a oferece (betas são ignoradas),
  a 0.15 segue como "Latest". O pacote baixado do GitHub passou no `realengine_updater --verify <chave em hex>`.
  Todas as sugestões da 0.14 (streaming de texturas, modos Desempenho/Qualidade, Sequencer, UMG 5.8, nuvens, demos).
- **0.17** publicada em 2026-11-04 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.17),
  127 MB, a "Latest" (o atualizador da 0.15 a oferece); o pacote baixado do GitHub passou no `realengine_updater --verify
  <chave em hex> <zip>` (a chave primeiro). Tudo da 0.16_b mais GI sem manchas, sombras virtuais do sol, nuvens do
  Tlender, online com Steam (notas-dev/online.md) e a meta de 200 fps. Suíte 76/76; o editor do pacote extraído numa pasta
  limpa faz 565 fps no nível padrão e passa o `--net-test`.
- **0.18** publicada em 2026-11-04 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.18),
  127 MB, a "Latest"; o pacote baixado do GitHub passou no `realengine_updater --verify`. O travamento ao abrir um
  projeto da 0.17 (materiais na tela de carregamento) e o vigia de travamento (seção acima). Suíte 77/77; o editor do
  pacote extraído numa pasta limpa faz 567 fps no nível padrão e passa o `--hang-test` (pilha com o `.pdb` do pacote).
- **0.19** publicada em 2026-11-05 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.19),
  127 MB, a "Latest"; o pacote baixado do GitHub passou no `realengine_updater --verify`. Relatórios da 0.18: FPS com
  cabelo/roupa Masked (#113, render.md), partes do Skeletal Mesh seguindo o gizmo (#114, animacao.md) e as pipelines
  Masked em segundo plano (o travamento da 0.18). Suíte 78/78; o editor do pacote extraído numa pasta limpa faz 539 fps
  no nível padrão, passa o `--sk-move-test` e troca as pipelines Masked em 165 ms (cache quente).
- **0.20** publicada em 2026-11-05 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.20),
  127 MB, a "Latest". O pacote baixado do GitHub passou no `realengine_updater --verify`.
  - O que entrou (render.md): a sombra das luzes de área filtrada (o pontilhado do vídeo do #113; a regra de 4 raios
    da 0.19 desfeita), o corte Masked como cobertura por amostra (cabelo sem serrilhado) e o resolve MSAA ponderado
    pelo brilho.
  - O editor do pacote extraído numa pasta limpa passou no `--sk-move-test` e no `shadow_filter` com validação.
  - Mínimo da GPU no nível padrão igual ao da 0.19 (1,65–1,81 × 1,71 ms). O fps oscilou com o Blender aberto.
- **0.21** publicada em 2026-11-06 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.21),
  128 MB, a "Latest". O pacote baixado do GitHub passou no `realengine_updater --verify`.
  - O que entrou (render.md, materiais.md, luz.md, gi.md):
    - modelos pesados: as fases 0 a 5 (malhas soldadas com LODs, clusters, Masked rápido com mips que preservam a
      cobertura, Épico em MSAA 4×, Translucent com OIT, BC7 na GPU);
    - o sol híbrido mapa + raio nos personagens;
    - o cache da GI sem realocações à toa.
  - Suíte 81/81.
  - O pacote extraído numa pasta limpa:
    - nível padrão a 574 fps (mínimo da GPU 1,69 ms);
    - cenas `glass` e `heavy` com validação sem erro;
    - `heavy` com RT em 26 ms, com 163 MB de texturas.

- **0.22** publicada em 2026-11-07 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.22),
  129 MB, a "Latest". O pacote baixado do GitHub passou no `realengine_updater --verify` (o primeiro argumento é a
  chave pública em hex, `pubkey` de `endpoints.ini`).
  - O que entrou: otimização geral (TSR, muitas luzes com sombra, LOD de clusters em DAG, fases 0-5), multidão sem
    rastros, corte de câmera do TSR corrigido, async (o editor nunca trava), programação nas 4 linguagens com JIT,
    Assistente (F1), áudio com surround, casa assombrada.
  - Ao mesmo tempo foi criado o repositório público `NathanLourencoPeixoto/realengine-assistant` com a base (127
    artigos).
  - Suíte 89/89 (três vezes, a última depois do merge final).
  - O pacote extraído numa pasta limpa:
    - nível padrão a 694 fps;
    - `crowd2000` com validação sem erro;
    - `--assistant-test` sem as variáveis de ambiente: a base veio do GitHub, 36/36 respostas.
- **0.23_b** (beta) publicada em 2026-11-07 como pre-release
  (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.23_b), 129 MB, de main e2ae1f4; a 0.22
  segue como "Latest" (o atualizador não oferece betas). O pacote baixado do GitHub passou no `--verify` e no sha256. Flicker da Delight corrigido (sondas de GI no mesmo slot) e sessões avançadas. Assinatura OK no
  `realengine_updater --verify`; o editor do pacote extraído numa pasta limpa: 592 fps no nível padrão (300 s, mínimo
  da GPU 1,37 ms).
  - **Atenção:** um `build-release` novo não tem `DemoContent`. O alvo `demo_assets` precisa rodar antes do
    `make_dist.ps1`, senão o pacote sai com 11,7 MB e sem as demos.

- **0.23** publicada em 2026-11-07 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.23),
  134 MB, a "Latest", de main 9e3da34. O pacote baixado do GitHub passou no `realengine_updater --verify` e no sha256.
  - O que entrou: exportar para Android (APK e AAB para a Google Play), jogo liso no celular, controles de toque,
    multiplayer contra lag e perda, partículas, veículos, mundos procedurais, IK Rig e Physics Asset, menu dos
    gráficos, sessões avançadas, flick e vestido da Delight.
  - O pacote leva `android/` (script, Java, manifesto, `prebuilt/` com a `.so` e o `classes.dex`, `logo.png`). O teste
    de fumaça do pacote extraído achou o `logo.png` faltando (o empacotador falhava sem ícone de projeto): corrigido antes
    de publicar.
  - Suíte 106/106. O pacote extraído numa pasta limpa: nível padrão a 623 fps com validação; um projeto novo empacotado
    para Android pelo próprio pacote (APK de 4,5 MB).
  - Base do Assistente publicada junto (`realengine-assistant`, 144 artigos).

- **0.24** publicada em 2026-11-08 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.24),
  135,6 MB, a "Latest", de main 4f6fcc6 (+ o commit da versão). O pacote baixado do GitHub passou no
  `realengine_updater --verify` e no sha256.
  - O que entrou: tecido em roupa importada (Criar Tecido a partir da Seleção), auto-colisão e sombra do tecido, AR com
    ARCore (oclusão, toque, template Realidade Aumentada), VR por OpenXR, chuva/água/oceano/lagos com cáusticas, neve com
    pegadas, veículos a partir do modelo, ícones dinâmicos, editor de partículas; Android abrindo no Adreno (S23),
    atualizador pela API (504).
  - Antes do pacote: `android/prebuilt` refeito (`package_apk.ps1 -BuildNative`, a `.so` de 12:55 era anterior à
    oclusão e ao toque) e o `build-android` com `RE_VERSION_STRING=0.24` (o cache guardava 0.23; no PowerShell o
    `-DRE_VERSION_STRING=0.24` virou "0": passar pelo bash); `demo_assets` antes do `make_dist.ps1`.
  - Suíte 121/121. O pacote extraído numa pasta limpa: nível padrão a 599 fps com validação; autotestes do tecido em
    malha e da ARCore ok.
  - Base do Assistente publicada junto (`realengine-assistant`, 149 artigos; o clone precisa de `user.name`/`user.email`
    do repositório para o commit).

- **0.25** publicada em 2026-11-10 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.25),
  238 MB, a "Latest" (entraram os runtimes de DLSS, FSR e XeSS: 141 MB descompactados); o pacote baixado do GitHub passou no
  `realengine_updater --verify`. O plano de melhorias das Fases 0 a 6 e as sobras: módulos de jogo (R7), física e
  NavMesh, estilos de render, upscalers, GTAO, sombras de contato, SSR, LUT, câmera de cinema, HDR10, pele, olhos,
  cabelo, fogo, World Partition, MetaSounds, Movie Render Queue, predição online, Git, Trace, plugins, async compute
  opcional. Suíte 195/199 com o PC livre (particles_5m, many_lights, spec_leak_nort e crowd_20000 no limite de FPS da
  3060; a versão das 14:57 mede igual); o editor do pacote extraído numa pasta limpa faz 471 fps no nível padrão sem
  erros de validação. O runtime Android (android/prebuilt) recompilado.
- **0.26** publicada em 2026-11-10 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.26),
  238 MB, a "Latest", de main 0d265ec (+ o commit da versão); o pacote baixado do GitHub passou no sha256 e no
  `realengine_updater --verify`. Relatório do usuário de 2026-11-10: jogador fora do chão (Plane com escala uniforme),
  Reimportar (vestido da Delight), barras na tela (grão), Estilizado controlável, Simular / Tempo Real, catálogo de
  classes com busca e arrastar, replicação dos Blueprints como na Unreal, menu do grafo em caixas. Suíte 198/199 antes
  (ikphys_ragdoll oscilou e passa sozinho); a release saiu sem rodar a suíte de novo, a pedido do usuário. O editor do
  pacote: 544 fps no nível padrão, sem erros de validação. Runtime Android recompilado (`build-android` com
  `RE_VERSION_STRING=0.26` pelo bash). Base do Assistente publicada junto (`realengine-assistant`, 201 artigos, 27aea92).
- **0.27** publicada em 2026-11-10 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.27),
  238 MB, a "Latest", de main 38c2b75 (+ o commit da versão); o pacote baixado do GitHub passou no sha256 e no
  `realengine_updater --verify`. Vestido da Delight (shape key presa na faixa), seleção por triângulo, variáveis do
  Blueprint arrastando e apagando, Grão de Filme e Denoise, Gameplay Framework como plugin embutido. Suíte não rodada
  inteira (o editor do usuário ocupava a GPU: só falhas de FPS nos testes dos módulos); o editor do pacote: 515 fps,
  sem erros de validação. Runtime Android recompilado (0.27). Base do Assistente publicada junto.
- **0.28** publicada em 2026-11-10 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.28),
  238 MB, a "Latest", de main 70d4237 (+ o commit da versão); sha256 e `--verify` do pacote baixado ok. Sombras sem rastro
  (luzes pontuais), arrastar nós sem atraso, delegate dos eventos, alças no viewport (splines, prototipagem, portas),
  estilos dos grafos. Testes das áreas tocadas passaram; o editor do pacote: 523 fps, sem erros de validação. Runtime
  Android recompilado (0.28). Base do Assistente publicada junto.
- **0.29** publicada em 2026-11-10 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.29),
  245 MB, a "Latest", de main ff3bfb1 (+ o commit da versão); sha256 e `--verify` do pacote baixado ok. Pinos de asset
  nos Blueprints, painéis redimensionáveis, Delight nos editores, chuva com vento, goteiras e escoamento, Opções do
  Estilo (ruído das sombras, lente), lóbulos do Principled em Masked/Two Sided, plataforma no Empacotar. Suíte inteira
  passou (202 de 202, sequencial); o editor do pacote: 507 fps, sem erros de validação. Runtime Android recompilado
  (0.29, 32,9 MB: as variantes novas dos shaders). Base do Assistente publicada junto.
- **0.30** publicada em 2026-11-10 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.30),
  245,4 MB, a "Latest", de main 27f4ca2 (+ o commit da versão 26bd897); o pacote baixado do GitHub passou no sha256 e no
  `realengine_updater --verify` (assinatura OK). Sequencer no layout da Unreal (curvas, interpolação, Auto Key, Criar
  Câmera, desfazer), variáveis de Blueprint com referências de asset e Expose on Spawn, chuva (sem rastro seco, pingos
  com o movimento, goteiras visíveis, quantidade e velocidade), Water Body Exclusion, prototipagem por spline, malhas
  importadas com o material, upscaling no editor, crash ao Empacotar corrigido, personagem visível nos Blueprints,
  nuvens ao virar a câmera. Suíte inteira com o PC em uso: as falhas foram de fps/tempo (GPU dividida); repetidas,
  sobraram 7 do mesmo tipo, e um A/B com a 0.29 compilada do código e o mesmo conteúdo mediu GPU mínima igual (spec_leak
  2,05-2,16 x 2,13 ms; editor vazio 1,915 x 1,915 ms). O editor do pacote extraído numa pasta limpa: 431 fps com
  validação, sem erros. Runtime Android recompilado (0.30, 32,9 MB). Base do Assistente publicada junto
  (`realengine-assistant` bf4f875, 202 artigos).
- **0.31** publicada em 2026-11-14 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.31),
  244,3 MB, a "Latest", de main 193bad3 (+ o commit da versão a1a813b); o pacote baixado do GitHub passou no sha256 e no
  `realengine_updater --verify` (assinatura OK). Estudo de desempenho (notas-dev/desempenho.md): LODs de Skeletal Mesh e
  BLAS no LOD 1, checkerboard do denoiser, raios da GI em compute (`den_trace.comp`), cópia do SSR só sem reflexo por
  raio, GPU por passe (`--gpu-passes`, Debug Viewer > GPU Passes). MyProject, Épico 1440p nativo: Realista 80 → 90 fps,
  Vídeo-realista 45 → 65. Suíte das áreas tocadas + kit rápido (126 testes): com a GPU livre passaram; o resto rodou com
  a GPU ocupada pelas sessões do Tlender e falhou só por FPS (nenhum erro de validação); `pause_options` falha também na
  0.30. O editor do pacote extraído numa pasta limpa: 439 fps com validação, sem erros. Runtime Android recompilado (0.31,
  31,8 MB). Base do Assistente publicada junto (`realengine-assistant` 1207524, 203 artigos).
- **0.32** publicada em 2026-11-14 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.32),
  244,4 MB, a "Latest", de main 5bd6591 (+ o commit da versão 4436ca7); o pacote baixado do GitHub passou no sha256 e no
  `realengine_updater --verify` (assinatura OK). Chuva no chão com a câmera alta (agua.md), Resolução Dinâmica sem zerar
  TSR/denoiser/nuvens (upscaling.md), VRS sem artefatos (render.md), passes do denoiser no `--gpu-passes`. Testes das
  áreas tocadas passaram com a GPU livre (água 7/7; upscale, budget_heavy_dynres, play_flicker, house_walk, denoiser_*,
  vrs, gi_house, spec_leak_epic com e sem VRS); `pause_options` falha também na 0.30. O editor do pacote numa pasta
  limpa: 505 fps no nível padrão e 336 fps na cena de chuva, com validação, sem erros. Runtime Android recompilado (0.32,
  31,8 MB). Base do Assistente publicada junto (`realengine-assistant` c83d1de).
- **0.34** publicada em 2026-11-15 (https://github.com/NathanLourencoPeixoto/realengine-releases/releases/tag/v0.34),
  248,2 MB, a "Latest", de main 9ec751d (+ o commit da versão 676cfac); leva também o que só tinha ido na beta 0.33_b
  (chuva vista do alto, atores molhando devagar, design/som/trovões da chuva, ReSTIR GI espacial). O pacote baixado do
  GitHub passou no sha256 e no `realengine_updater --verify` (assinatura OK). Chuva realista (película d'água, poças,
  respingos, gotas e filetes nas paredes, gotas iluminadas, névoa, goteiras de tempestade), estilos de shading da
  chuva, água na lente simulada, marcas molhadas (agua.md). Suíte das áreas tocadas + kit rápido: 87/87. O editor do
  pacote numa pasta limpa, com validação e sem erros: 520 fps no nível padrão, 297 fps na cena de chuva. Runtime
  Android recompilado (0.34, 34,2 MB). Base do Assistente publicada junto (`realengine-assistant` 648ae1f, 205 artigos).
  O GitHub agora responde pela conta `Nathan3DAnimator` (os endereços antigos redirecionam).
- **0.35** publicada em 2026-11-15 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.35),
  248,2 MB, a "Latest", de main 5c5099e (+ o commit da versão 7befdcd): correções da chuva da 0.34 (flicker roxo pela
  película sem filtro, goteiras sumindo ao andar/girar, véu de tempestade num personagem Static; agua.md). O pacote
  baixado do GitHub passou no sha256 e no `--verify` (assinatura OK). O editor do pacote numa pasta limpa, com validação
  e sem erros (237 fps no nível padrão e 131 na chuva com a GPU 42% ocupada por outro editor aberto; a suíte da água e
  do render também falhou só por FPS nessa hora). Runtime Android recompilado (0.35, 34,2 MB). Base do Assistente sem
  mudança desde a 0.34 (`realengine-assistant` 648ae1f).
- **0.36** publicada em 2026-11-15 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.36),
  248,4 MB, a "Latest", de main 3d61eb8 (+ o commit da versão f4f001a): goteiras só em beiradas com telhado e água,
  pingos dos personagens decaindo fora da chuva, caminho do reflexo pela película estável (agua.md). O pacote baixado
  do GitHub passou no sha256 e no `--verify`. O editor do pacote numa pasta limpa, com validação e sem erros (449 fps no
  nível padrão e 254 na chuva, com a GPU 39% ocupada por outro editor). Área `agua` 9/9. Runtime Android recompilado
  (0.36, 34,4 MB). Base do Assistente sem mudança (`realengine-assistant` 648ae1f).
- **0.37** publicada em 2026-11-16 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.37),
  248,4 MB, a "Latest", de main 550cf82 (+ o commit da versão 462410c): escoamento em fios só onde a água chega,
  quedas d'água pela vazão real, sombra de chuva menor, lente sem cores, molhado menos escuro, sem os pingos caindo dos
  atores, gotas sem cintilar na resolução adaptativa (agua.md). O pacote baixado do GitHub passou no sha256 e no
  `--verify`. O editor do pacote numa pasta limpa, com validação e sem erros, com a GPU livre: 523 fps no nível padrão
  e 292 na chuva. Áreas agua, editor e assistente 23/23. Runtime Android recompilado (0.37, 34,3 MB). Base do Assistente
  publicada junto (`realengine-assistant` 82b01aa).
- **0.38** publicada em 2026-11-16 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.38),
  248,5 MB, a "Latest", de main d220f38 (+ o commit da versão dad1c04): a chuva pela física real (espectro de
  Marshall-Palmer, velocidade terminal por gota, riscos do obturador, anéis densos nas poças) e a tela sem piscar com
  personagem secando (agua.md). O pacote baixado do GitHub passou no sha256 e no `--verify`. O editor do pacote numa pasta
  limpa, com validação e sem erros, com a GPU livre: 529 fps no nível padrão e 287 na chuva. Área `agua` 9/9. Runtime
  Android recompilado (0.38). Base do Assistente publicada junto (`realengine-assistant` 28c9080).
- **0.39** publicada em 2026-11-16 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.39),
  248,6 MB, a "Latest", de main ff09c83 (+ o commit da versão 3d00515): subníveis (Janela > Níveis, como o painel Levels
  da Unreal) e Lighting Levels empilháveis, a soma dos visíveis, vários ao mesmo tempo (niveis.md). O pacote baixado do
  GitHub passou no sha256 e no `--verify`. O editor do pacote numa pasta limpa, com validação e sem erros: 504 fps no
  nível padrão. Suíte completa: 198/207 de primeira; 7 das falhas eram só fps com a GPU disputada e passaram de novo;
  ficaram `shader_spill` (o passe principal derrama 96 bytes) e `budget_heavy_dynres` (10,5 ms > 10), que já vinham da
  0.38 (os shaders do passe principal são os mesmos). Runtime Android recompilado (0.39). Base do Assistente publicada
  junto (`realengine-assistant` 206381d, 206 artigos).
## Aviso da origem (2026-11-04)
O Worker grava no cabeçalho de cada report o estado/UF e o país, além de coordenadas aproximadas (~100 km), tirados da
geolocalização da Cloudflare pelo IP. O IP e a cidade não ficam guardados. As três janelas de envio (bug, sugestão,
crash) avisam isso: "Junto com o envio vão o estado e o país de onde você está (aproximados, sem o IP nem a cidade)."
A engine não manda nada novo. Detalhes em `../notas-dev/visualizador-de-reports.md` ("Origem dos reports").

## Onde a GPU parou (2026-11-08, plano de melhorias B14e)
Os crashes #115 e #116 (device lost no tecido, antes do 0a114b7) só diziam "VK_ERROR_DEVICE_LOST em vkWaitSemaphores".
Agora o relatório diz o passe.
- **Marcadores** (`vkctx.c`, `gpu_mark(cmd, "nome")`): com `VK_AMD_buffer_marker` (o driver da NVIDIA tem), cada passe
  do quadro escreve o seu número num buffer host-visible duas vezes: ao começar (TOP_OF_PIPE) e quando tudo antes dele
  terminou (ALL_COMMANDS). O nome é guardado só por ponteiro (literal), por slot de quadro em voo. Marcados: início e fim
  do quadro, céu, skin, BLAS do tecido, partículas, cena/TLAS, luzes, AR, clima, água, tecido, GI (sondas, cache,
  oclusores), Build Lighting, sombras virtuais, passe principal, translúcidos, exposição, pós, composição, UI.
- **Device lost** (`vk_check`): `vkctx_gpu_diagnosis` lê os marcadores; a mensagem do relatório ganha "A GPU parou no
  quadro N em "passe"" e o log as linhas completas (último iniciado, último com tudo antes terminado, os passes em
  andamento). Com `VK_EXT_device_fault` (também ligada) vêm o endereço e o tipo da falha que o driver der.
- **Espera limitada**: o driver da NVIDIA reinicia o canal depois de uma falha de página **sem devolver**
  VK_ERROR_DEVICE_LOST (medido: o evento 153 aparece e o `vkWaitSemaphores` com UINT64_MAX nunca volta; o editor ficava
  travado). As esperas pelo quadro (`wait_frame`) agora desistem em 10 s e contam como device lost, com os marcadores.
- **Teste real** (`REALENGINE_GPU_FAULT_TEST=1`: a partir do quadro 400 a sonda de cintilação escreve num endereço sem
  memória): o relatório saiu com "A GPU parou no quadro 402 em "exposicao"", o passe certo. Não entra na suíte (reinicia
  o canal da GPU).
- **Teste `gpu_marks`** (`--check-gpu-marks`, cena cloth com validação): depois do bench e de `vkDeviceWaitIdle`, o
  último quadro tem os 26 passes marcados, o último iniciado é "quadro terminado" e o diagnóstico não acusa nada.
- **Custo**: house 1440p, 3 rodadas alternadas: GPU 1,833 ms com, 1,829 sem (ruído). `REALENGINE_NO_GPU_MARKERS=1`
  desliga as duas extensões.
- **0.40_b** e **0.41_b** (betas) publicadas em 2026-11-16 como pre-release (Lighting Level Blend e Timeline; Ragdoll Ativo).
- **0.42** publicada em 2026-11-16 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.42), 246,5 MB,
  a "Latest", de main 18b575f: refração, cabelo dinâmico e do Tlender/OBJ, Wet Surface (molhado e suor), crash da GPU no
  passe skin ao adicionar subnível (#135-137: paleta de ossos realocada no meio do quadro), textura arrastada para o grafo
  de materiais (#124). Suíte completa 211/211 antes das correções e 122/122 nas áreas mexidas depois; o pacote passou no
  `--verify` e no sha256 baixado do GitHub; editor do pacote a 484 fps no nível padrão, validação limpa. Runtime Android
  com `RE_VERSION_STRING=0.42`. Base do Assistente publicada (209 artigos).
- **0.43** publicada em 2026-11-19 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.43), 268,7 MB,
  a "Latest", de main 64625c1 (+ o commit da versão 2c0f007): o plano 0.43 inteiro (Q1-Q25 em 8 lotes: temas e janelas,
  ícones, seletor de assets, viewport de asset, física profissional, soft body, ragdoll realista que levanta, Toon,
  câmera em primeira pessoa, kit de terror, splines, PCG, tudo em jogo, Nanite Tessellation, UDIM, Tlender completo, GI
  estável, visão distante, VR no Quest e realidade mista, auditorias). Suíte completa no 2º monitor 290/297 com a CPU a
  94% (um `tlender_tests` de outra sessão): as falhas eram de fps; `ikphys_ragdoll` (ragdoll que não para) falha igual na
  base 867ac95 sob essa carga (3 de 5). O pacote baixado do GitHub passou no sha256 e no `--verify` (a chave pública vai
  em hex na linha de comando). Editor do pacote numa pasta limpa: 283 fps com validação, sem erros; sem validação, em
  pares com o pacote da 0.42: 533/466 contra 545/450 fps. Runtime Android recompilado (0.43: `.so` de 54,9 MB com o
  OpenXR/MR, `classes.dex` novo do Q25b; o `RE_VERSION_STRING` trocado no `CMakeCache.txt` do `build-android`). Base do
  Assistente publicada (`realengine-assistant` 59ef6be, 243 artigos).
- **0.44** publicada em 2026-11-20 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.44), 312,7 MB,
  a "Latest", de main 0239fa9 (+ o commit da versão 45fe3cb): travadas ao abrir (pipelines em lote nos núcleos), traces
  como os da Unreal (200 mil por quadro, Trace Complex, assíncronos, desenho de debug na GPU), visual Azul Real (Q26), cena
  `metro` (feixes de sol, trem, arcos, entulho, hera; neblina nos espelhos: `shader_spill` agora aceita 64 bytes, main
  +0,03 ms). Suíte completa no 2º monitor antes do ajuste do spill: 281/307, as falhas de fps com um `tlender_tests` de
  outra sessão usando a GPU (o `demo_metro` passou sozinho a 159 fps e caiu a 34 com ele a 62%). O pacote passou no
  `--verify` e o sha256 do GitHub bate; editor do pacote numa pasta limpa a 507 fps com validação, sem erros. Runtime
  Android recompilado (`.so` de 55,5 MB, `RE_VERSION_STRING=0.44`). Base do Assistente publicada (`realengine-assistant`
  1d1a5ab, 246 artigos). Subchats Q25, Q25b e os dois do Tlender (mocap) arquivados: todos já na main.
- **0.45_b** (beta, pre-release) publicada em 2026-11-20 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.45_b),
  312,8 MB, de main fe0e647 (a 0.44 segue a "Latest"): o crash ao fechar o editor (#143-146, teste `editor_close`) e a
  mão base do VR (FBX para modelador e rigger, mãos com esqueleto; notas-dev/xr.md). O pacote passou no `--verify`, o
  sha256 do GitHub bate, o editor do pacote abre a 508 fps com validação e fecha limpo (`--quit-after`). Runtime Android
  recompilado (`RE_VERSION_STRING=0.45_b`). Base do Assistente publicada (247 artigos).
- **0.45** publicada em 2026-11-20 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.45), 312,8 MB,
  a "Latest", de main 1568c42: pulos no Motion Matching (root motion vertical: a altura do take move a cápsula, Jump pede
  a decolagem da animação; exemplo JumpTake, teste `mm_jump`) mais o que veio na 0.45_b (crash ao fechar, mão base do VR).
  Suíte completa no 2º monitor: 279/313, com o Blender e o `tlender_tests` de outras sessões usando a máquina; 29 das
  falhas eram de fps/tempo. As outras: `dock_targets` e `toon` (0,603% > 0,600%) falham igual na base 7e0689a;
  `spec_leak_epic`, `stable_pan` e `stable_pan_photoreal` passaram sozinhos (o brilho de 2,6 a 3,6 em 3 rodadas); `async`
  falha igual na base sob essa carga (a fase 0 trava ~6 s gravando as malhas pesadas e a medida acaba antes). O pacote
  passou no `--verify`, o sha256 do GitHub bate; editor do pacote numa pasta limpa a 447 fps com validação, sem erros,
  fecha limpo (`--quit-after`), `mm_jump` passa nele (460 fps). Runtime Android recompilado (`.so` de 55,6 MB,
  `RE_VERSION_STRING=0.45` no cache do `build-android`; o `classes.dex` da beta, sem mudança no Java). Base do Assistente
  publicada (`realengine-assistant` 67eb69b, 248 artigos).
  - `dock_targets` falha desde antes desta release (12 alvos do encaixe com cliques injetados): investigar à parte.
- **0.46** publicada em 2026-11-23 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.46), 314,3 MB,
  a "Latest", de main 3d56f9d: o plano Q27 (tecnologia do GTA 6: multidão em pânico, trânsito, cabelo em massa, batidas
  de carro, ferimentos do ragdoll ativo, clima regional e poças persistentes, streaming preditivo, retrorrefletor e
  flocos metálicos, variedade de NPCs, rotinas da cidade, estilos de locomoção, GI de fallback sem raios), texturas
  nítidas a distância (TSR, mips Kaiser, M_Grid) e a importação #164 (.rasset, peças presas a osso). Suíte completa no
  2º monitor: 319/328; corrigidos os 2 que eram desta versão (05a3270), os de fps e o Timeline passaram ao repetir,
  `spec_leak` (29-42 nits contra 30) já falhava antes (48,6 no log das 00:01). O pacote passou no `--verify`, o sha256
  do GitHub bate; editor do pacote numa pasta limpa a 543 fps com validação, sem erros, fecha limpo; `city_routine` passa
  nele. Runtime Android recompilado (`.so` de 56,9 MB, `RE_VERSION_STRING=0.46`). Base do Assistente publicada
  (`realengine-assistant` 1dccc5d, 256 artigos).
- **0.47** publicada em 2026-11-23 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.47), 314,3 MB,
  a "Latest", de main b4c0b70: a importação da Capsula (67f02fe: UDIM de um tile assado da fonte cheia, não da cópia de
  64², e Relative Parenting no .tlend). Suíte completa no 2º monitor: 323/329. `dock_targets` e `spec_leak` já falhavam
  antes; `many_lights` (69 < 70), `rain_clear_drops` (170 < 200), `demo_horror` (176 < 180) e `demo_fighting` (150-160 <
  180) são de fps e dão os mesmos números com o código de antes da correção (4d7c62c), medidos na mesma hora: o estado
  da máquina, não desta versão (investigar à parte). Pacote: `--verify` e sha256 do baixado do GitHub ok; editor do pacote
  numa pasta limpa a 538 fps com validação, sem erros, e a Capsula importa nítida e no lugar do .rasset. Runtime Android
  recompilado (`.so` de 56,9 MB, `RE_VERSION_STRING=0.47` pelo bash: pelo PowerShell o `0.47` vira `0`). Base do
  Assistente publicada (`realengine-assistant` 0a775bd, 256 artigos).
- **0.48** publicada em 2026-11-23 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.48), 314,4 MB,
  a "Latest", de main e6edc4b: gizmos com o controle do Tlender (pivô, orientação, X/Y/Z e números no arrasto, cancelar
  sem desfazer, trackball com a rotação no sentido do mouse, o cursor do Tlender), editores de nós sem atraso e soltar
  link no vazio, Detalhes do Material Editor reorganizados, materiais emissivos iluminam (Emissive Light / Emissive
  Shadows). Sem a suíte completa (pedido do usuário): a das áreas mexidas (editor, materiais, luz, gi) deu 133/141 —
  `dock_targets`, `spec_leak`, `many_lights` e `demo_fighting` já falhavam, `blueprints` passou ao repetir,
  `render_style_photoreal` e `budget_house` caíram pela sombra da luz emissiva e voltaram a passar com ela opcional,
  `assistant` passou depois do artigo reescrito; depois disso só os testes ligados (gizmo_ui, mated_ui, emissive_light,
  materials, render_style_photoreal, budget_house, assistant, tips_audit, i18n). Pacote: `--verify` e sha256 do baixado
  ok; editor do pacote numa pasta limpa sem erros de validação e o `gizmo_ui` passando nele. Runtime Android recompilado
  (`RE_VERSION_STRING=0.48`). Base do Assistente publicada (`realengine-assistant` 8914683, 259 artigos).
- **0.49** publicada em 2026-11-23 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.49), 314,8 MB,
  a "Latest", de main 2863e61: dinâmicas de animação para gameplay (root motion de montages, Motion Warping, montages
  pareados com Find/Can Play, traversal Vault/Mantle/Climb, Orientation/Stride Warping, janelas de combo e acerto, Play
  Hit Reaction, cobertura em terceira pessoa), texturas achadas nas pastas em volta, pasta inteira importada, nomenclatura
  das texturas no Principled, Organizar Nós em todos os grafos, rolagem na borda, configurações da textura, material e
  sockets do Static Mesh, destaque do alvo ao soltar material, abertura transparente como a do Blender e o Project
  Browser no azul do R. Suíte completa no 2º monitor: 334/344; as 10 falhas repetiram com -Falhas. `dock_targets`,
  `spec_leak`, `many_lights`, `rain_clear_drops`, `demo_horror` e `demo_fighting` já falhavam na 0.47/0.48;
  `render_style_photoreal` (192-199 < 200), `budget_house` (0,72-0,75 ms > 0,70) e `wp_fast` falharam igual com o
  executável da 0.48 na mesma hora (a GPU com 22% de uso de outro programa); `translucent` (143 < 150) é de fps.
  Pacote: sha256 e `--verify` do baixado do GitHub ok; editor do pacote numa pasta limpa com validação sem erros (441 fps)
  e a abertura nova (3,9 ms por quadro). Runtime Android recompilado (`RE_VERSION_STRING=0.49`, `.so` de 57,1 MB) e o
  pacote refeito com ele. Base do Assistente publicada (`realengine-assistant` 2ff6862, 273 artigos).

- **0.50 — Rivora Engine** publicada em 2026-11-23 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.50),
  a "Latest", 314,2 MB, com o pacote em dois nomes (`Rivora-0.50-win64.zip` e `RealEngine-0.50-win64.zip`, mesmos bytes e
  assinatura): o nome novo, código aberto (MIT, avisos, privacidade, doação, sem marcas da Epic), detecção de rig e física
  pela malha, Preload Level com velocidade. Suíte completa no 2º monitor: 331/347; `update_download` só passa com a 0.50
  publicada (procura `Rivora-*`); `spec_leak_nort`, `ghost_realistic`, `render_style_video`, `async`, `upscaler_xess` e
  `crowd_20000` passaram ao repetir sozinhos; as outras 9 são as conhecidas (fps e luz, iguais com a 0.48/0.49).
  **Ponte conferida antes de publicar**: a 0.49 extraída numa pasta temporária, o `realengine_updater.exe` dela com
  `--install` sobre a 0.50: copiou `rivora*.exe`, trocou o `realengine.exe` pelo lançador (93 KB) e o lançador abriu o
  `rivora.exe` da pasta. **Depois de publicar**, pela internet: o `realengine.exe --update-test` da 0.49 achou
  `RealEngine-0.50-win64.zip`, baixou, assinatura ok, desempacotou (61,6 s); o `rivora.exe --update-test` da 0.50 achou
  `Rivora-0.50-win64.zip` (37,1 s). Runtime Android recompilado (0.50, 57,3 MB). Código publicado no `rivora` (commit "Rivora Engine
  0.50", e-mail no-reply), base do Assistente (`realengine-assistant` 0dff5e0, 278 artigos).
- **0.51** publicada em 2026-11-23 (https://github.com/Nathan3DAnimator/realengine-releases/releases/tag/v0.51), a "Latest",
  314,8 MB nos dois nomes (`Rivora-` e `RealEngine-0.51-win64.zip`, assinatura conferida nos dois): o MCP (a IA controla o
  editor; `rivora_mcp.exe` no pacote), os valores editados direto nos nós do Material Editor, o report #180 (o ator sumia
  ao mexer no material) e a tela de carregamento com o nome novo. Suíte completa parada em 233/348 a pedido do usuário
  ("só o que deu problema e os testes do MCP"): as 12 falhas repetidas sozinhas passaram, menos `dock_targets` (falha
  desde a 0.47); `blueprints` falhava pela pasta `build/test_game/bp` antiga (apagada: passa); os de fps eram o editor
  instalado aberto na GPU. Testes `mcp`, `mat_live_edit`, `mated_ui`, `scripts`, `tips_audit`, `i18n`, `assistant*`
  passando. O editor do pacote rodou o `--mcp-test` numa pasta limpa (ponte stdio inclusa). Depois de publicar, o
  `rivora.exe --update-test` da 0.50 achou `Rivora-0.51-win64.zip`, baixou, assinatura ok, desempacotou (39,3 s). Runtime
  Android recompilado (0.51, 57,4 MB). Código no `rivora` (fa755ef, no-reply), base do Assistente (`realengine-assistant`
  8df6da3, 280 artigos).
## Código aberto (2026-11-23)
Pedido: "deixar a engine pronta para distribuição open source gratuita, no meu site só com um local de doação".
- **Licença MIT** (`LICENSE`, Nathan Lourenço Peixoto). Os jogos são do usuário, sem royalties.
- **`THIRD_PARTY_NOTICES.md`** gerado por `tools/make_notices.py` dos próprios arquivos de licença (GLFW zlib, Dear
  ImGui e cimgui MIT, meshoptimizer MIT, as fontes OFL, os assets CC0 das demos; DLSS/FSR/XeSS, Steamworks e o runtime do
  VC descritos). Rodar de novo ao trocar uma biblioteca de `external/` ou uma fonte.
- **`PRIVACY.md`** (pt/en): sem telemetria; a internet só para o atualizador (API pública do GitHub), a base do
  Assistente e os relatórios que o usuário confirma (o rascunho já mostrava o que vai e tirava nome de usuário, pasta
  pessoal, nome do computador e e-mails). O Worker já tem limite por IP (12 por hora com o KV `RATE`).
- **`README.md`** (pt/en): o que é, como compilar, avisos (sem afiliação com a Epic), site.
- **Pacotes**: `make_dist.ps1` põe LICENSE, THIRD_PARTY_NOTICES.md, PRIVACY.md e README.md ao lado do editor; o
  Empacotar Jogo põe `Rivora_LICENSE.txt` e `THIRD_PARTY_NOTICES.md` ao lado de todo jogo (de uma build, da raiz do
  repositório).
- **Doação**: Ajuda > Apoiar a Real Engine (Doar) e o botão no Sobre abrem `[support] donate_url` do `endpoints.ini`
  (desde 24/11/2026 https://nathan3dstudio.com/#doar, o domínio próprio do site; o endereço workers.dev antigo continua no ar para as instalações antigas; vazio esconde o item). O Sobre diz a licença, "sem afiliação com a Epic" e abre os
  avisos.
- **Marcas da Epic fora da interface**: MetaSound virou **Sound Graph** ("Grafo de Som"; a pasta de dados
  `/Content/Audio/MetaSounds` ficou, é dado dos projetos), Nanite Tessellation virou **Geometry Tessellation** (os
  `r.Nanite.*` viraram `r.Tessellation.*`, os nomes antigos ainda respondem no console e no game.ini), sem "(like
  Lumen/Nanite)", e as 163 comparações "Unreal's X" / "o X da Unreal" das dicas e descrições saíram (o recurso continua
  com o nome comum: World Partition, Behavior Tree, Spline...). Ficam de propósito os nomes de sistemas de rig na
  detecção (UE Mannequin / MetaHuman: compatibilidade) e as comparações nos artigos do Assistente e nas notas
  (documentação). Feito por um script sobre todos os textos de `src/` (as chaves de tradução mudam junto) e revisado à
  mão: frases que perderam informação junto com a comparação foram refeitas.
- **Repositórios** (2026-11-23): o de desenvolvimento, com todo o histórico, é o privado `Nathan3DAnimator/realengine-dev`
  (antes `realengine`; o GitHub redireciona o nome antigo). O público `Nathan3DAnimator/realengine` recebe o código de
  cada release por `tools/publish_source.ps1`: os arquivos do HEAD (`git archive`) numa cópia em `..\realengine-public`,
  sem o `CLAUDE.md`, com `Users\Player` trocado por `Users\Player` (mesmo tamanho: o FBX de teste continua válido), um
  commit "Real Engine <versão>" com o e-mail no-reply da conta, enviado. Rodar depois de cada release publicada.
- **Blender**: varredura do código (cabeçalhos GPL, "Blender Foundation", funções do código do Blender): nada GPL. As
  texturas procedurais dos nós seguem o kernel do Cycles (Apache 2.0): creditado no THIRD_PARTY_NOTICES.md com a licença
  (`data/licenses/Apache-2.0.txt`), assim como as declarações do OpenXR.
- **Falta (do usuário)**: busca de marca do nome "Real Engine" (ou um nome novo).

## Real Engine -> Rivora Engine (2026-11-23)
Pedido: o nome novo (o "Real Engine" lembrava "Unreal" e "Reality Engine"; "Rivora" é inventado: marca mais forte).
- **O que o usuário vê**: 97 textos de `src/` (títulos, menus, Sobre, intro, Project Browser, crash, atualizador; as
  chaves de tradução mudam junto), "© 2026 Rivora Engine" no splash (sem "todos os direitos reservados": é MIT), pasta
  padrão de projetos novos `Documentos\Rivora Projects`, recurso de versão do .exe, README/PRIVACY/avisos, artigos.
- **Executáveis**: os alvos do CMake mantêm os nomes (os testes `COMMAND realengine` rodam o `rivora.exe`), os arquivos
  mudam por `OUTPUT_NAME`: `rivora.exe`, `rivora_game.exe`, `rivora_updater.exe`. O alvo `realengine_legacy`
  (`src/legacy_launcher.c`, 93 KB) gera o `realengine.exe` que só abre o `rivora.exe` com os mesmos argumentos: os
  atalhos das instalações antigas e o atualizador da 0.49 (que exige o `realengine.exe` no pacote e o abre ao fim).
- **Pasta do usuário**: `%APPDATA%\Rivora` e `%LOCALAPPDATA%\Rivora`; na primeira vez o `%APPDATA%\RealEngine` é
  copiado (preferências, layouts, temas, base offline do Assistente, outbox; o cache local começa do zero).
- **Pacote**: `make_dist.ps1` gera `Rivora-<versão>-win64.zip` e uma cópia `RealEngine-<versão>-win64.zip` (mesmos bytes,
  a mesma assinatura; o `.sha256` com o outro nome): a 0.49 procura `RealEngine-*`, a 0.50 em diante `Rivora-*`
  (`endpoints.ini [update] asset`). O atualizador aceita tags `rivora`/`realengine`.
- **Ficam de propósito** (dados e compatibilidade): o formato `"RealEngine Material"`, os nós `RealEngine*`, a API dos
  scripts (`using RealEngine;`, `from realengine import *`, `realengine.h`), o id Android padrão `com.realengine.*`
  (mudar trocaria o id de apps publicados), `librealengine_game.so`, as variáveis `REALENGINE_*`, o prefixo `[RealEngine]`
  dos logs (os testes), o `produto = realengine` e "- Produto: Real Engine" dos relatórios (Worker e visualizador), os
  repositórios de releases e do Assistente (os clientes antigos os leem), a pasta `RealEngine/` e o `RealEngine.md`.
- **Repositórios**: `rivora-dev` (privado, histórico) e `rivora` (público, `publish_source.ps1`).
- **Testes**: os 21 ligados (Assistente, dicas, traduções, empacotar, layouts antigos, intro, projetos) passam; o
  `realengine.exe` abre o `rivora.exe` com os argumentos (`--preload-test` gravou o arquivo).
