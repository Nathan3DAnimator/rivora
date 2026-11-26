# Testes: a suíte

A **suíte** (item 22, 25/11/2026) é a única cobrada antes de publicar. São 142 testes em 22 áreas do que importa ao
usuário, ~40 min com a fase de janelas um teste por vez. Os outros ~240 testes registrados no `ctest` são os de
**regressão** (rótulo `regressao`): cada bug corrigido tem o seu, e eles rodam pelo nome (`ctest -R nome`), mas não
são a suíte. O kit rápido, as 63 áreas por nota e o `-Completa` antigos saíram.

## Como rodar (`tools/suite.ps1`)
| Comando | O quê |
|---|---|
| `tools/suite.ps1 -Visivel` | a suíte inteira, janelas no 2º monitor (o padrão do usuário) |
| `tools/suite.ps1` | a mesma, escondida (`REALENGINE_BACKGROUND=1`) |
| `tools/suite.ps1 -Area render,fisica` | só essas áreas |
| `tools/suite.ps1 -Falhas` | só o que falhou na última execução (`<build>/Testing/suite_failed.txt`) |
| `tools/suite.ps1 -Listar` | as áreas e os testes de cada uma |
| `tools/suite.ps1 -GravarReferencias` | regrava as fotos de `tests/data/ref` (conferir a olho antes de commitar) |
| `tools/suite.ps1 -Resumo` | o resumo de novo, a partir dos logs da última execução (nada roda) |
| `ctest --test-dir build -C Release -R "^(a|b)$"` | um teste de regressão pelo nome |
| `ctest --test-dir build -C Release -j 8` | todos os ~380 registrados (a trava `gpu` serializa as janelas) |

Opções: `-Build build-release`, `-Jobs 8` (a fase sem janela), `-Monitor 2`, `-Forcar`.

O que ela faz, em ordem:
0. **Fecha o que sobrou do build.** Um `rivora*` deste build vivo de antes (um teste que não fechou o que abriu) é
   fechado e avisado; no fim, de novo, e aí reprova a rodada (25/11: a Prévia em VR aberta pelo `ui_sweep` comeu a GPU
   de duas suítes, 72 falhas falsas de fps).
1. **Confere a máquina.** OVRServer, ffmpeg ou um editor/jogo de fora da build abertos dividem a GPU e dão falhas
   falsas de fps. Ela lista o que está aberto e para (exit 3); `-Forcar` segue e avisa. Nunca fecha processos do usuário.
2. **Fase sem janela** (rótulo `cpu`) em paralelo, depois a **fase com janela**, um teste por vez (`-j 1`).
3. **Resumo numa tela.** Cada área PASSOU/FALHOU, os testes passados/total, os segundos; abaixo de cada falha, a
   1ª linha `FAIL`/`VUID`/`[Vulkan]` dela; no fim o tempo e o log (`<build>/Testing/suite.log`, a junção de
   `suite_cpu.log` e `suite_gpu.log`). Exit 0 só com tudo passando.

## As 22 áreas (`tools/suite_areas.cmake`; rótulos `suite` e `s_<área>`)
| Área | Testes |
|---|---|
| inicio-e-projetos | start_pipelines, intro_window, exe_icon, editor_close, project_third_person/first_person/top_down/quality |
| editor | ui_sweep + ui_sweep_clean, dock_targets, ui_popups, ui_tips, theme, tips_audit, editor_fps, selection_outline, gizmo_ui, asset_picker, asset_viewport, suite_areas |
| importar | import_obj, import_fbx_rig, import_fbx_lights, import_gltf, import_glb, import_tlend_q19, import_tlend_hair, import_rasset, import_folder |
| render | render_ref_epico/alto/baixo/rt, gi_house, spec_leak, render_style_photoreal, budget_house, heavy_mesh, rt_clusters, tess_seams, vista_pop, hair, translucent, ghost_realistic, upscaler_dlss/fsr/xess, particles_scene, rain_scene, sequencer |
| materiais | materials, mat_live_param, material_lobes, decals, texture_streaming, udim_seams, mat_pins |
| animacao-characters | anim_math, anim_foot_ik, traversal, mm_startstop, morph_targets, skin_5000, ikphys_selftest, active_ragdoll, char_physics |
| fisica | constraints, physics, phys_stack, phys_ccd, phys_joints, phys_crates, softbody_rigid, cloth_mesh, vehicle_selftest, vehicles_scene |
| grafos-scripts | blueprints, blueprints_slow, graph_menu, bp_debugger, gameplay_tags, scripts (Rivora Script, C#, Python, C, C++) |
| ia-navegacao | ai, navmesh, ai_squad, ai_senses, crowd, traffic_queue |
| audio | audio_mixer, audio_play |
| ui-do-jogo-localizacao | umg, hud, pause_options, pause_options_reload, pause_apply_cinematic, loc, i18n, i18n_audit |
| sistemas-de-jogo | inventory, interaction, dialogue, quest, rules, ability, health |
| niveis-streaming | levels, levels_panel, preload, world_partition, wp_fast, save_progress |
| multiplayer | net_protocol, net_session, pie_multiplayer, net_session_advanced |
| empacotar | game_package, game_fps, game_first_run, game_blueprints, game_package_icon, game_icon_runs, mobile_selftest, touch_selftest, android_package |
| vr-ar | vr_sim, mr_sim, ar_sim |
| atualizador-assinatura | update_sign, update_download (a release real: precisa de internet) |
| mcp, plugins, assistente | mcp; plugins, plugin_template; assistant_pack, assistant |
| troca-de-projeto | project_switch_clean, project_switch |
| memoria-travadas | scalability_memory (a memória da GPU por VK_EXT_memory_budget; os bytes privados só informam), place_volume_hitch, play_hitch, place_rain_hitch, open_asset_hitch, assistant_hitch, facility_leak, hang_watchdog, child_process_cleanup |

## Os testes feitos para a suíte
- **render_ref_epico / alto / baixo / rt** (`tools/test_render_ref.cmake` + `tools/compare_png.py`):
  - A casa (`--scene house`, a câmera da cena) em Épico, Alto, Baixo e Fotorrealista (RT).
  - A foto do fim do `--bench` é reduzida a 640 px de largura e comparada a `tests/data/ref/house_<preset>.png`.
  - Medidas: a média do erro (a maior diferença de canal por pixel, 0..255) e a % de pixels acima de 24, só no recorte da viewport
    (sem os painéis e o FPS do editor; a cena com `--game-view`). Limites: erro médio 0,8 e 0,5% dos pixels. De uma
    execução para outra o erro fica entre 0,00 e 0,40; Épico contra Alto dá 1,29, então a troca é pega. Ao falhar, grava `<foto>_diff.png` ao lado.
  - Cada um cobra a meta de fps do preset (200; RT 100).
  - As referências se regravam com `-GravarReferencias` (`REALENGINE_UPDATE_REFS=1`), só nesta GPU e conferidas a olho.
- **android_package** (`tools/test_android_package.cmake`):
  - `--package-android` pela linha de comando; o APK tem de existir.
  - Com um aparelho no `adb devices`: instala, abre (o pacote e a atividade lidos pelo `aapt`) e confere que o
    processo segue vivo 8 s depois.
  - Sem SDK: SKIP. Sem aparelho: só o empacotamento.
- **update_sign** (`tools/test_update_sign.cmake`):
  - Offline, pelo `rivora_updater --keygen/--sign/--verify`, com duas chaves de teste geradas no build e apagadas no fim.
  - O pacote assinado passa; verificado contra outra chave, é recusado; com um byte trocado, é recusado.
  - Nunca usa a chave de release.
- **ui_sweep_clean** (`tools/ui_sweep_report.py --check`, depois do `ui_sweep` pelo fixture `ui_sweep_ran`):
  - Reprova com qualquer bug do relatório: crash, assert, erro de validação, quadro > 400 ms, memória que fica,
    tradução que falta.
  - O `ui_sweep` sozinho só reprova quando não termina.

## Teste novo
Registrado no CMake, ele é `regressao` (roda pelo nome). Só entra na suíte (`re_area` em `tools/suite_areas.cmake`) se
cobrir um caminho da área que ela ainda não cobre; em `RE_CPU` se não abre janela. A checagem `suite_areas` falha se
um nome da suíte não for um teste registrado (os opcionais, que dependem de SDK ou Python, ficam em `RE_OPTIONAL`) ou
se uma área ficar vazia.

## Histórico
- Antes do item 22 (10/11 a 25/11): 63 áreas por nota e um kit `quick` de ~70 testes em ~4 min (`-Areas`, `-Completa`).
  A suíte completa tinha 202 testes em ~20 min em 10/11 e 367 em 25/11.

## Varredura da interface (`--ui-sweep`, item 21, 2026-11-25)
Um robô que percorre o editor inteiro por entrada injetada (`src/ui_sweep.c`), para a varredura de bugs:
- **Como enxerga**: o `ItemAdd` do ImGui oferece cada item com rótulo (o gancho do "Mostrar onde" do Assistente,
  `GREHighlight`, agora também `g_ui_sweep_item_hook` em `imgui_hooks.c`): rótulo como aparece, retângulo, janela raiz,
  desativado. Um controle é um caminho de cliques a partir do editor limpo (`Arquivo > Salvar Tudo`, `[clique direito:
  viewport] > ...`); cada episódio reseta (Esc, Cancelar de um modal), refaz o caminho pelo mouse injetado e observa 14
  quadros. Menus e popups que abriram viram controles novos; uma janela que abriu (painel, Project Settings, Editor
  Preferences, Plugins, o editor do asset que o Criar do Content Browser fez) dá os botões e abas dela (um nível, até 48).
  Limpeza: Esc, os documentos fechados sem salvar (`editor_docs_discard/finish`), o X das janelas novas, o layout padrão.
- **Play**: 3 s de jogo (o HUD), o menu de pausa (`pause_menu_key`) e cada botão dele e das páginas que abrir
  (`ui_test_buttons`, `ui_test_click`), menos sair/menu principal.
- **Por controle**: crash (o `ui_sweep_running.txt` nomeia o controle que estava rodando), erros de validação (a última
  mensagem: `g_vk_last_validation`), assert do ImGui (`g_ui_sweep_assert_hook` no `crashlog.c`: anotado e segue),
  texto sem tradução (`i18n_missing_count`), quadro > 400 ms, +256 MB que ficam depois de fechar.
- **Nunca clica** (`g_never`, pt/en/es): apagar, sair, fechar/abrir/novo projeto, publicar, enviar, relatório, Git,
  empacotar, URL, site, doação, atualizar, idioma, tela cheia, Steam/online, Play (a parte de Play é à parte), gravar,
  dispositivos, baixar, limpar, redefinir. Diálogos do sistema, Explorer e navegador não abrem
  (`g_platform_synthetic_input`: arquivo, pasta, mostrar no Explorer).
- **Rodar**: `ctest -R ui_sweep` (área editor; `REALENGINE_BACKGROUND=1`, projeto `test_projects/Sweep` do template
  Terceira Pessoa, `--lang pt`). `REALENGINE_SWEEP_MAX` (1500 controles) e `REALENGINE_SWEEP_SECONDS` (420) limitam.
  O teste passa quando a execução termina; os achados vão para `build/ui_sweep/ui_sweep.jsonl`, e
  `python tools/ui_sweep_report.py build/ui_sweep` gera `ui_sweep.md` (todos) e `ui_sweep_bugs.md` (um por linha: o
  controle, o erro, o arquivo provável pelo rótulo -> msgid -> src/).
- **Armadilhas achadas ao montar** (cada uma custou uma execução):
  - a navegação por teclado do editor está desligada: o Esc não fecha menus; a limpeza fecha pelo ImGui
    (`igClosePopupToLevel`) e clica Cancelar/Fechar/Não/OK num diálogo;
  - um menu que o hover já abriu (a barra ativa, um submenu) fecha com o clique: o robô só clica quando o hover não
    abriu nada (`popup_count`);
  - o "Mostrar onde" do Assistente zerava o `GREHighlight` no meio do quadro: tudo desenhado depois (Project
    Settings, Preferências) sumia; durante a varredura ele não zera;
  - em segundo plano o "Usar menos CPU" dormia 30 ms por quadro (desligado na varredura);
  - um ator que o controle adiciona é desfeito (`editor_undo`); uma janela de alternância (Janela > X) fecha pelo mesmo
    caminho; as janelas novas contam só contra a foto tirada no começo do episódio;
  - os itens de um popup são achados pelo rótulo em qualquer popup (o id de um menu de clique direito muda);
  - botões só de ícone entram pelo id (`#tb_save`).
- **Faltas de tradução no relatório**: um TR de um texto já traduzido não erra na tela (o TR devolve o texto que não
  conhece), só faz ruído; o relatório o descarta, e também os ids ("tb_save"). Fica o que é bug: o texto inglês sem
  entrada, e o **literal em português passado ao TR** (o texto achado como `TR("...")` no src/: aparece em português no
  editor em inglês e espanhol). `--all-misses` mostra tudo.
- **Processos filhos**: o "Prévia em VR" abria um `rivora_game.exe` que sobrevivia à varredura e comia a GPU das suítes
  seguintes (76 falhas falsas de fps). Agora: a Prévia em VR e "novo processo" ficam na exclusão; a limpeza de cada
  controle fecha os processos filhos dele (Toolhelp, pai = o editor); no fim, um filho vivo imprime `FAIL: UI sweep` e
  reprova o teste. O editor prende os filhos num job object `KILL_ON_JOB_CLOSE` (`re_process_start`, bug-fixer).
- **Estado que fica entre controles**: um cabeçalho do Debug Viewer deixado aberto empurra os outros para fora da vista,
  uma lista fica rolada, e o que está fora da vista não passa pelo `ItemAdd`. O robô rola a janela e as subjanelas dela
  de cima a baixo (13 passos) antes de dar o item por não alcançado; o registro guarda o estado da janela. A escala
  volta ao que era depois de cada controle.
- **O Debug Viewer aberto cobria a barra de menus**: o menu seguinte não abria (os 13 não alcançados dele); a limpeza o
  fecha (`dv_set_open(false)`). Uma janela que um controle deixou aberta é fechada refazendo só a parte de menu do
  caminho que a abriu (Preferências > uma aba: a janela ficava, e o próximo "Preferências..." a fechava).
- **Estado que um controle deixa e o próximo herda** (cada um dava não alcançados em série; a limpeza ou o reset de
  cada controle o desfaz): o Abrir Asset é um modal reaberto enquanto `show_open_asset` está ligado, e os itens de menu
  das Preferências e do Project Settings são alternâncias (`show_prefs`, `show_project_settings`): as três flags
  desligadas a cada reset. Uma página, modo ou seção escolhido por um controle (Preferências > Aparência, Behavior Tree
  > Blackboard, Sound Wave > Som): o item não achado nem rolando faz o robô clicar os outros controles da janela, um a
  um (8 no máximo), até ele aparecer. O VSync e o "Usar menos CPU" ligados por um controle prendiam todos os seguintes
  em 60 fps (a varredura de 150 s ia a 470 s): os dois voltam a cada controle. O canvas de um grafo reaberto precisa de
  um clique esquerdo antes do direito (o foco). Se a linha do Abrir Asset não aparece, o asset abre pelo caminho do
  Content Browser (`content_open_asset`; contado no resumo).
- **Seleção e projeto**: o que só aparece com algo selecionado é alcançado pela busca na janela (até 24 controles,
  pulando os rótulos repetidos como "#src"); o clique direito do grafo tenta 6 pontos (um nó ligado: Desfazer Todas as
  Ligações); o do Content Browser vai ao canto vazio do painel; a linha do Abrir Asset no meio de um caminho abre o
  asset pela API. Sem projeto aberto (o `--new-project` falhou: a pasta sobrou) o robô reprova no começo; o teste exige
  a fixture `projects` (bug-fixer).
- **Medida final (2026-11-25, template Terceira Pessoa, RTX 3060, segundo plano)**: 867 controles em 396 s (limite
  padrão 600 s), 1 não alcançado (o campo Nome de uma Entrada no MetaSound MS_Wind: só com aquele nó selecionado),
  `ui_sweep_clean: OK (869 controls, no problem)`. Quadros lentos que oscilam entre execuções (trocar a escala para
  Cinemática 5,1 s, Opções da pausa 2,3 s, Abrir Asset 1,0 s) ficaram com o bug-fixer.
- **Ignorados no relatório**: dados de assets de teste (`*Test*.r*` no caminho) e os nomes padrão Param1/Result1.
  `--check` (bug-fixer): sai com 1 e imprime `FAIL: ui_sweep_clean` enquanto houver bug (teste `ui_sweep_clean`).
- Os padrões `TR(cond ? TR(a) : TR(b))` (11 lugares) viraram `(cond ? TR(a) : TR(b))`; o "Ponto N" do Trilho é montado
  já traduzido (`label_cell_text`, editor.c).
- **Medidas** (RTX 3060, segundo plano, validação ligada, template Terceira Pessoa): 607 controles em 240 s
  (~0,4 s cada), 52 não alcançados (contexto que muda: o clique direito no Organizador cai em outra linha), 39 bugs
  na lista depois do filtro (textos já traduzidos e ids pedidos ao TR pelas dicas não contam).

## Mensagens da validação reprovam qualquer teste (2026-11-25, bug-fixer)
- Uma regra só, no fim do `CMakeLists.txt` (antes do `suite_areas.cmake`): todo teste ganha mais uma
  `FAIL_REGULAR_EXPRESSION` `VUID|\[Vulkan\]` (a propriedade é uma lista: qualquer uma reprova; a do próprio teste
  fica). O `vkctx.c` imprime cada aviso/erro da camada como `[Vulkan] ...` (só com `--validation`), e nem todos trazem
  "VUID" (os das janelas soltas não traziam: o `dock_targets` passava com 10). 44 testes com `--validation` não tinham
  regex de falha nenhuma. No CMake o colchete vai como `\\[` (`"\["` vira `[`, uma classe de caracteres).
- Testes novos da varredura da interface: `blueprints_slow` (o bp-test a 4 fps: `REALENGINE_SLOW_FRAME_MS=250`),
  `place_volume_hitch` e `play_hitch` (`--place-hitch-test "<classe>"|Play <ms>`: o pior quadro depois de posicionar
  ou de apertar Play), `pause_apply_cinematic` (Apply sem mudar nada com o view em Cinemática),
  `scalability_memory` (`--scalability-mem-test`: Médio e Cinemática três vezes, os bytes privados).
- O `--bp-test` confere um jogo que rodou os segundos do `--bench` em tempo de jogo, não de parede (cada quadro conta
  no máximo 0,1 s: abaixo de 10 fps, 2 s de parede eram 0,4 s de jogo; espera no máximo 60 s a mais).

## A 1ª rodada da suíte nova (25/11, RTX 3060, OVRServer aberto: `-Forcar`) e o que ela achou
- 143 testes, 132 passaram. Corrigidos depois:
  - `blueprints` (2 de 2 na suíte, sozinho passava): os testes de rede também rodam `--bp-test` e deixam as classes
    de `Content/BpTest` conhecidas; o setup do seguinte apaga os arquivos, e o Empacotar Jogo falhava em copiar um
    Blueprint registrado cujo arquivo não existia mais. Agora ele fica de fora (editor.c, "left out (its file was
    deleted)"); o mesmo acontecia com quem apagasse um Blueprint no Content Browser e empacotasse. Teste
    `blueprints_after_net` (o fixture `net_left_bp` roda o `net_session_advanced` antes).
  - `wp_fast` (instável aqui e na base): o critério de 2× a mediana contava qualquer quadro de ~6,5 ms a 300 fps
    (0,37 a 0,70% com a GPU dividida; o streaming no pior quadro, 0,5 ms). Agora conta como engasgo o quadro acima de
    2× a mediana e de 1/60 s (no máximo 0,2%), e o streaming no pior quadro em até 2 ms; 3 de 3 com 0 engasgos.
  - `android_package` e `render_ref_*`: as pastas do `--save` e do `--shot` criadas pelos scripts.
  - `render_ref_baixo` ficou fora, pendente: a casa no Baixo sai quase preta (a GI de fallback não ilumina), bug do
    render aberto pelo Lead; a referência será gravada depois do conserto.
- Ficaram de outros: `ui_sweep` (o `nvngx_update.exe` da NVIDIA, aberto pelo NGX, contado como processo que sobrou),
  `selection_outline` (item 17), `assistant` (a busca "VK_ERROR_DEVICE_LOST").

## Medidas de fps escondidas (2026-11-23, valem para a suíte nova)
Com `-Visivel` o teste roda no 2º monitor, mas a **janela da medida** de um `--bench` com `--min-fps` sai da tela
(`re_test_window_offscreen`, `platform_win32.c`: o lugar do modo escondido, mesmo tamanho e mesma swapchain; 0,25 s para
assentar; de volta ao monitor depois). Log: "Visible suite: the fps measure off the screens". Vale para o editor
(`main.c`) e o jogo (`game_main.c`). As metas não mudaram: foram feitas escondidas.
- **Por quê:** uma janela mostrada num monitor custa **9-12% da GPU** na composição do Windows, em todo teste e na
  mesma proporção do quadro (não é custo da engine). Medido com a GPU livre (RTX 3060, 2560×1417, 3 monitores de
  165/144/60 Hz): `many_lights` 69 visível × 78 escondido, `rain_clear_drops` 173 × 199, `demo_horror` 176 × 195,
  `demo_fighting` 172 × 194. A GPU mínima é igual nos dois (4,73 × 4,64 ms na chuva), clock e potência também (~1980 MHz,
  no limite de 170 W). Só os passes de dentro do quadro esticam (main 3,0 → 4,3 ms, GI 0,8 → 1,25), e só no DWM: ele
  usa 1,3% da GPU.
- **O que não é:** o modo de apresentação (mailbox e immediate iguais: 176-182 × 164-181), a janela maior que o monitor
  (1600×900 dentro dele: 294-296 × 323-324 escondida), o monitor de 144 Hz (o de 60 Hz perde o mesmo). Uma faixa de 112
  px à mostra perde metade (185-190).
- **Depois:** visível com a medida fora da tela = escondido (rain 184/183 × 185/183 na mesma hora; parado no monitor
  167-171).
- **A máquina também pesa:** o editor instalado aberto (99% da GPU, no limite de potência), outra sessão rodando
  `realengine.exe --bench` (uma medida caiu a 8 fps) e um vídeo tocando no Chrome (3-10% do 3D mais o decodificador:
  rain escondido 183-197 em vez de 196-204). `rain_clear_drops` (Épico, meta 200) não tem folga nem com a GPU livre:
  196-204 escondido, GPU mínima 4,64-4,73 ms (211-215 fps), a média 6% acima.
