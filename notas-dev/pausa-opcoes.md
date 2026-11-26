# Pausa e opções (módulo 3 do R7)

Código: `src/game_settings.h/.c` (as opções, o `.ini`, os remapeamentos, a pausa), `src/pause_menu.c` (os dois Widget
Blueprints prontos e o menu em execução), nós em `bp_internal.h`/`blueprint.c`/`bp_vm.c`, ligação e navegação no UMG
(`ui_widget.c`, `ui_widget_draw.c`), teste `src/pause_test.c`. Feito do zero, modular: nada preso a um jogo.

## Pausa (Set Game Paused da Unreal)
- `game_paused()`: o PIE (`editor_frame`) e o jogo (`game_main.c`) não chamam `systems_update`, `ai_update`,
  `physics_update`, `vehicle_update`, `anim_update`, `crowd_update`; partículas com dt 0; `audio_pause_game` (os sons de
  UI continuam). O botão Pause do editor segue parando tudo, como antes.
- `bp_tick` pausado: só as instâncias com `paused_tick` rodam Tick, Delays, timers, eventos do Enhanced Input e Key Events
  (`bp_key_event`); sem movimento de Character/projétil, Sequencer, componentes e overlaps. Widgets (User Widget) sempre
  têm `paused_tick` (como o UMG da Unreal).
- **Tick When Paused** por ator: `CBlueprint.flags & BP_FLAG_TICK_WHEN_PAUSED` (era o `pad`: níveis antigos = 0), no
  Details > Blueprint e pelo nó Set Tickable When Paused.
- `bp_begin_play`/`bp_end_play` despausam e fecham o menu (um Play nunca começa pausado).

## Game User Settings
- `GameUserSettings` (`gus()`): preset (`render_scalability`), resolução, modo de janela (Fullscreen exclusivo, Windowed
  Fullscreen sem borda, Windowed), VSync, limite de fps, escala da resolução (% do `tsr_scale` do preset), volumes
  Master/Music/SFX/Voice, sensibilidade (× a do projeto), Inverter Y, idioma (-1: o do sistema), até 64 remapeamentos.
- Por nome (`gus_get`/`gus_set`): Scalability, Resolution ("1920x1080"), WindowMode, VSync, FrameRateLimit,
  ResolutionScale, MasterVolume, MusicVolume, SFXVolume, VoiceVolume, MouseSensitivity, InvertY, Language ("pt_BR"),
  `Key:<Ação>[:<Tecla>|:Gamepad]`. O `.ini` usa os mesmos nomes (lido de volta por `gus_set`).
- Arquivo: jogo empacotado `%LOCALAPPDATA%/<jogo>/Saved/Config/GameUserSettings.ini` (Android: `Saved/Config`, a pasta
  do app); Play do editor `<projeto>/Saved/Config/GameUserSettings.ini`; `REALENGINE_SETTINGS_FILE` vence (os testes).
  O jogo lê antes de criar a janela (janela, VSync, preset, idioma da tela de carregamento); `--bench` não lê (medida
  igual sempre), a não ser com a variável.
- Aplicar: `gus_apply_live` (volumes, teclas, sensibilidade, idioma, preset, escala, limite) na hora; `gus_apply`
  (Apply Settings) também a janela e o VSync, e salva. O host (`GusHost`) aplica o que é dele: no jogo
  `game_apply_video` (preset só quando muda: o perfil do celular e o VR ficam), no PIE o preset vale até o Stop (o do
  editor volta, nunca gravado no `editor.ini`), sem janela/VSync.
- Áudio: Master por `audio_set_master_volume`; Music/SFX/Voice por `audio_set_user_class_volume` (multiplica as
  classes da engine na cadeia, `class_eval`; zero custo com todos em 1).
- Controles: `input.c` lê cada mapeamento por `gus_remapped(ação, tecla)` (1 comparação quando nada foi remapeado); o
  WASD do `pawn_input`/`game_input` passa por `gus_remapped_code("IA_Move", ...)`; a sensibilidade e o Inverter Y valem
  para o mouse e o analógico direito.
- Idioma: no jogo `i18n_set_language`; no PIE só os widgets (`gus_tr` = `tr_lang` no idioma do jogo). Os textos dos
  widgets ao vivo passam por `gus_tr` (um texto que está na tabela aparece traduzido; os ligados a variável/opção não).

## UMG
- **Game User Setting** nos Detalhes de Slider, Spin Box, Check Box, Combo Box (as opções e a escolha vêm da opção),
  Text (o valor como texto: "75%", "Alto", "Espaço"), Progress Bar e o widget novo **Input Key Selector** (o da Unreal:
  clicado ou Enter/A espera a próxima tecla, botão do mouse ou do controle; Esc/Start cancela; OnKeySelected). Lidos
  quando `gus_serial()` muda; escritos no evento do widget (`bind_write`) e aplicados na hora.
- Teclado e controle (com o cursor fora, um menu): setas, D-pad e analógico esquerdo vão ao controle mais perto naquela
  direção (`nav_move`); esquerda/direita mudam Slider, Spin Box e Combo Box; Enter/Espaço/A clicam, alternam, avançam a
  escolha ou fazem o Key Selector esperar; a Caixa de Rolagem acompanha. O contorno azul só aparece depois de uma tecla.
- O widget da frente com algo clicável sob o mouse fica com o mouse (o fundo escurecido do menu protege o que está atrás).
- `ui_back`: Esc/P/B mandam o Custom Event **OnBack** ao widget da frente (as Opções voltam ao menu).

## Os widgets prontos (Content/UI, feitos quando faltam: o Play, Empacotar)
- **WBP_PauseMenu**: Continuar (Close Pause Menu), Opções (Create Widget WBP_Options + Add to Viewport Z 110), Menu
  Principal (Return to Main Menu: Main Menu Map, senão Game Default Map, senão o nível de novo), Sair (Quit Game).
- **WBP_Options**: abas Gráficos, Áudio, Controles (uma linha por mapeamento de tecla do projeto, teclado e controle,
  até 10), Idioma; Restaurar Padrões, Aplicar, Voltar (e OnBack). Grafo de nós comuns (Set Widget Visible por aba).
- Abrir: Esc no jogo (Project Settings > Maps & Modes > Pause Menu; desligado, o Esc fecha o jogo como antes), a tecla
  do Play (P; o Esc para o Play), o Start do controle (`pause_menu_poll_gamepad`), o nó Open Pause Menu.

## Nós (14)
Set Game Paused, Is Game Paused, Set Tickable When Paused, Get/Set Game User Setting, Apply Settings, Load Settings,
Set Settings to Defaults, Remap Action Key, Get Action Key, Open/Close Pause Menu, Return to Main Menu, Quit Game
(scripts: `SetGamePaused(true)`, `GetGameUserSetting("MusicVolume")`...). Artigo `pause-options` no Assistente.

## Medidas (RTX 3060, 2560x1417, com validação)
| Caso | Resultado |
|---|---|
| Passo do jogo no PIE (`--pause-test`), rodando | 0,012 ms |
| O mesmo pausado (só widgets e Tick When Paused) | 0,009 ms |
| WBP_Options aberto (~110 widgets, 16 ligados) desenhado | 0,034-0,046 ms |
| `gus_remapped` sem remapeamento | 1 comparação por mapeamento |
| Teste `pause_options` sozinho na GPU | 651 fps (o mínimo do teste: 200) |

Teste `pause_options`: o menu abre (tecla do Play), Baixo + Enter abre as Opções, o mundo parado (caixa com física e
um Tick parados, o ator Tick When Paused andando), aba Áudio, preset Baixo aplicado à vista, Jump em F pelo Key
Selector, volume 0,35 e Inverter Y, Aplicar grava; a tecla volta das Opções e fecha o menu; F dispara IA_Jump e Espaço
não. `pause_options_reload` (outro processo): as mesmas opções lidas ao abrir o Play e aplicadas.

## Limites / próximos passos
- Resolução e modo de janela não mudam a janela do editor (só no jogo empacotado). Sem "manter estas configurações?"
  com contagem regressiva; sem detecção de conflito de teclas (duas ações na mesma tecla).
- O Key Selector não grava eixos (WASD de IA_Move vai tecla por tecla; o analógico não se remapeia).
- Idiomas: os que o `i18n` tem (inglês, português).

## Apply e o preset do view no Play (2026-11-25, bug-fixer)
- No Play o preset salvo das Game User Settings não é posto no view ao começar (o editor segue com o seu). O Apply
  chamava `apply_video` sempre, e o `pie_apply_video` trocava o view para o preset salvo mesmo sem nada mudado nas
  Opções: com o editor em Cinemática, cada Apply refazia os pipelines de MSAA (um quadro de 403 ms na varredura).
  Agora só troca quando o preset das configurações mudou desde o último aplicado (`g_pie_gus_scalability`, editor.c).
  Teste `pause_apply_cinematic` (e o passo novo do `pause_options`: Apply sem mudar nada, o preset fica).
