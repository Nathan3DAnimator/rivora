# Missões e objetivos (módulo 7 do R7)

Código: `src/quest.h/.c` (asset `.rquest`, regras, Quest Logs, eventos, save, WBP_QuestTracker, Debug Viewer, autoteste —
no `re_render`), `src/quest_hud.c` (os marcadores na vista, cimgui: editor, jogo e Android), `src/quest_editor.c` (editor
do asset e Details do Quest Log), `src/quest_test.c` (teste jogado), nós em `bp_internal.h`/`blueprint.c`/`bp_vm.c`,
eventos dos scripts em `script_lower.c`, chunk no `save_progress.c`. Feito do zero e modular: não usa o quest log da demo
de RPG; Vida, Inventário, Interação e Diálogo (módulos 1, 2, 5 e 6) entram pelos ganchos que já existiam no `bp_vm.c`.

## Como funciona
- **Asset Quest** (`.rquest`, JSON; Content Browser > Adicionar > Missão, vazia ou o **Exemplo do Amuleto Perdido**):
  título e descrição em inglês (o msgid), português e espanhol (vazio: o inglês, e em português a tabela da engine), uma
  categoria, até 8 **etapas** (uma depois da outra, ou **Etapas ao Mesmo Tempo**; cada uma pode ter os **Objetivos em
  Ordem** e um texto) e até 16 **objetivos** (na ordem das etapas). Repetível; **Falha Depois de** X s.
- **Objetivos**: **Ir Até** (o alvo — nome do ator ou uma Gameplay Tag nele — com Raio > 0: a distância; Raio 0: dentro
  da Colisão do ator, caixa girada pelo yaw / esfera / cápsula; sem Colisão: 2 m), **Interagir** (o On Interact do
  Interagível, por nome ou tag; só o uso do dono do log conta), **Coletar** N (o `inv_count` do Inventário do jogador,
  lido quando a revisão do inventário muda, quando um objetivo termina e 2× por segundo; **Entregar os Itens** os tira ao
  completar), **Matar** N (o On Death da Vida: o morto casa por nome ou tag hierárquica; a morte do próprio jogador não
  conta; **Só as Mortes do Jogador**), **Falar** (o fim de um Diálogo: quem fala casa por nome/tag, ou o nome do Diálogo
  é o alvo) e **Manual** (Advance Objective / Set Objective Progress). Cada um: **ID** (os nós e eventos o chamam por ele;
  um número também serve), **Opcional**, **Tag da Condição** (com Não), **Ator do Marcador** (vazio: o alvo; `-`: nenhum).
- **Regra**: um objetivo se move só quando a missão está ativa, a etapa dele está ativa, ele não terminou, os anteriores
  obrigatórios da etapa terminaram (em ordem) e a condição passa. Uma etapa termina quando os obrigatórios terminam (os
  opcionais dela param de contar); a última etapa (ou todas, em paralelo) completa a missão.
- **Recompensas** (na ordem): os itens entregues saem, os itens da recompensa entram no Inventário de quem a missão é
  (o pawn do log, ou o jogador), as tags, o evento **On Quest Completed**, o **Custom Event** da recompensa (com o nome da
  missão como entrada) no ator do log, no jogador e no **Alvo do Evento**, e a **Próxima Missão** começa (o mesmo feito
  não conta para ela).
- **Quest Log** (`CQuestLog`, 28 bytes; Details > Adicionar e componentes da classe Blueprint): **Missões Iniciais** (4,
  começam no BeginPlay se ainda não estão no log), **Mostrar o Rastreador**, **Mostrar Marcadores**, **Rastrear Missões
  Novas**, **Distância do Marcador**, **Widget do Rastreador**. O conteúdo vive no `quest.c` (8 logs × 256 missões, sem
  malloc): o log padrão é o do pawn possuído com Quest Log, senão o primeiro Quest Log (um Game State), senão o do próprio
  jogo (sem componente: rastreador e marcadores ligados). Os logs **passam de nível para nível** (o dono é achado de novo
  pelo nome) e somem quando o Play para (`progress_session_end`) ou no New Game.
- **Eventos** (fila de 512, enviados quando a mudança termina — um evento pode iniciar ou completar outra missão sem
  bagunçar a que está sendo mudada) para todos os Blueprints que os têm: On Quest Started (Quest, Quest Log), On Objective
  Updated (Quest, Objective, Index, Progress, Count), On Objective Completed (Quest, Objective, Index), On Quest Completed
  / Failed (Quest, Quest Log). Nos scripts: `OnQuestStarted`, `OnObjectiveUpdated`, `OnObjectiveCompleted`,
  `OnQuestCompleted`, `OnQuestFailed`; os nós entram sozinhos na API (`StartQuest("Q_X")`, `AdvanceObjective(...)`...).
- **Nós** (categoria Quest; Quest Log desligado = o log padrão): Start Quest, Complete Quest, Fail Quest, Advance
  Objective (Amount 0 = 1), Set Objective Progress, Is Quest Active, Is Quest Completed, Get Quest State (0..3, o nome, a
  etapa), Get Objective Progress (progresso, contagem, completo, texto no idioma do jogo), Track Quest (vazio: nenhuma),
  Get Tracked Quest. Os que mudam o jogo não rodam no Construction Script do editor.
- **WBP_QuestTracker** (Content/UI, criado ao adicionar um Quest Log, uma Missão, ou no Play com o rastreador): uma
  coluna no canto superior direito com linhas que têm fundo próprio (TitleRow/Title, StageRow/Stage,
  ObjectiveNRow/ObjectiveN, 8 linhas): linha escondida colapsa, então o painel tem a altura do que mostra. O módulo
  escreve os textos só quando o log muda (revisão), o idioma muda ou a cada 32 quadros (uma tag de condição). Feito: verde
  claro; opcional: cinza com "(Opcional)". Um WBP do usuário com os mesmos nomes funciona.
- **Marcadores** (`quest_tick` faz a lista dos objetivos ativos da missão rastreada com Ator do Marcador; `quest_hud.c`
  desenha): losango dourado (opcional: prata) sobre o ator (+ meia altura do Character ou 120 cm), distância em m do
  jogador embaixo; fora da vista ou atrás, preso à borda da tela (margem 46 px) com uma seta para o lado certo (atrás: de
  lado). Desenhado depois dos widgets no PIE (`editor_viewport.c`) e no jogo (`game_main.c`, que abre o quadro do ImGui
  quando há marcador).
- **Idiomas**: o do Diálogo (`dlg_language`: Set Dialogue Language ou o do jogo; espanhol pelo sistema). Sem texto, o
  objetivo é montado do tipo e do alvo ("Colete Erva", "Defeat Enemy.Wolf"...).

## Save
- Registro **`#Quests`** no formato do fluxo de variáveis (`RESAV001` + registros): por log um ou mais registros de até
  4080 bytes (magic `QST1`, versão 1, flags padrão / primeiro, nº de missões, rastreada, nome do dono) e por missão hash,
  estado, etapa, nº de objetivos, feitos, etapas feitas, tempo e o progresso (16 bits) de cada objetivo. 200 missões com
  3 objetivos = 6156 bytes.
- **Save Progress**: chunk **QSTL** (ver `notas-dev/salvar-progresso.md`; desconhecido para um save antigo: pulado). O
  Load Progress lê no `apply_level` (antes do BeginPlay; as Missões Iniciais já no log não recomeçam); save sem QSTL =
  nenhuma missão. **Save Game to Slot** acrescenta o registro ao fluxo do ator; o Load o lê de volta.
- A leitura confere tudo (tamanhos, estado, etapa < etapas, feitos só dos objetivos que existem); missão cujo asset sumiu
  é descartada.

## Medidas (RTX 3060, 2560×1417, Release, validação; `--quest-test`)
| O quê | Custo |
|---|---|
| `quest_tick` no Play com 201 missões ativas (601 objetivos: 200 Ir Até, 200 Matar, 201 Manual) + rastreador + marcador | 0,036–0,045 ms/quadro |
| Autoteste (mundo próprio, sem widgets): `quest_tick` com 200 missões / 600 objetivos | 0,006–0,010 ms/quadro |
| Start Quest de 200 missões (assets já carregados, 200 eventos no Blueprint) | 0,23–0,55 ms (≈1–3 µs cada) |
| Uma morte passando por 200 objetivos Matar (com 200 On Objective Updated no Blueprint) | 0,05–0,13 ms |
| Salvar o registro de 200 missões | 3–7 µs |
| Sem nenhuma missão | o `quest_tick` percorre 8 logs vazios (< 1 µs) |
| Cena do teste (PIE, 2560×1417) | 254–442 fps |
Antes/depois: sem missões o custo por quadro é zero na prática; os ganchos (morte, interação, fim de diálogo) só
percorrem os logs quando há missão ativa.

## Teste (`ctest -R quest`, projeto novo `test_projects/QST`, `REALENGINE_SAVE_DIR=build/quest_test/SaveGames`)
`quest_selftest`: em ordem (o baú não conta antes do poço), Ir Até no raio, Interagir pela tag, Matar pela tag
hierárquica (urso e a morte do jogador não contam), Advance Objective, etapa que passa sem o opcional, condição por tag
(`Rep.Friend` cobre `Rep.Friend.Close`), Falar pelo nome do Diálogo, registro do save (limpo e lido), recompensas (tag,
Custom Event, próxima missão rastreada), não repetível, tempo limite, recomeçar a falhada, Fail Quest, paralelo, Track
Quest, 200 missões cronometradas (limites: tick < 0,2 ms, morte < 0,5 ms). Depois jogado: o Quest Log do herói inicia a
missão principal (On Quest Started no Blueprint), WBP_QuestTracker (título, etapa, só o primeiro objetivo), marcador no
Trigger Box a 20 m, ervas pegas antes contam quando o Coletar começa (2/3), a Porta usada, os lobos por Apply Damage,
Get Objective Progress / Is Quest Active pelo Blueprint, Save Progress e Save Game to Slot, Load Progress (o nível aberto
de novo: etapa 2 com 1 lobo), Load Game from Slot, Falar com a anciã (Dialogue Speaker), o ritual pelo Advance Objective
do Blueprint, recompensas (5 moedas, a tag, o Custom Event no Alvo do Evento, ervas entregues), a próxima missão com o
marcador, uma missão que falha pelo tempo, o rastreador em português; 200 missões a mais e a medida (`--min-fps 200`).

## Limites / próximos passos
- 16 objetivos por missão, 8 etapas, 8 logs, 256 missões por log; o rastreador mostra 8 objetivos.
- Ir Até com Raio 0 usa a Colisão do próprio ator (a de um componente filho de um Blueprint não).
- Interagir conta usos (o mesmo ator duas vezes conta duas, a menos que o Interagível seja de uso único).
- Missões não são replicadas no online (o servidor decide e replica o que precisar).
- `COMP_COUNT` está em 51 de 64 (máscara de 64 bits): os próximos componentes precisam ser contados.
- Os componentes Inventário e Dialogue Speaker não aparecem na árvore de componentes do editor de Blueprint (as listas
  `order[]` do `bp_editor.c`); o Quest Log foi posto nelas.
