# Regras de jogo (Game Mode de regras, módulo 8 do R7)

Código: `src/game_rules.h/.c` (o estado da partida, as fases, pontos, vitória e derrota, vidas e renascer, o cronômetro, a
rede, o chunk do save, o Debug Viewer, o autoteste — no `re_render`), `src/rules_widgets.c` (WBP_MatchHUD e WBP_MatchEnd,
`re_render`), `src/rules_ui.c` (Details), componente `CGameRules` (128 bytes) em `components.h`, nós em
`bp_internal.h`/`blueprint.c`/`bp_vm.c`, eventos dos scripts em `script_lower.c`, teste `src/rules_test.c`. Feito do
zero, sem nada das demos (luta, terror, RPG têm o próprio modo em `demo_play.c`). Vida (R7.1), pausa (R7.3) e
Checkpoint/save (R7.4) entram pelas funções deles.

## Como funciona
- **Onde fica**: o ator pronto **Game Rules** (Place Actors, menu rápido > Jogabilidade, `add:GameRules` do Assistente),
  **Details > Adicionar > Game Rules** em qualquer ator, ou numa classe Blueprint (Componentes > + Adicionar): um
  `BP_GameMode` com ele em Project Settings > Game Mode. Uma partida por Play: o primeiro componente achado. O Game Mode
  do projeto nasce depois do BeginPlay (`project_begin_play`), então a busca é no `rules_tick` (um teste de máscara por
  arquétipo enquanto não há); a configuração é relida a cada quadro (o Details e os Blueprints mudam no Play).
- **Fases**: Aguardando (Começar Sozinho após o Tempo de Espera; desligado espera **Start Match**) → Contagem (3-2-1; 0:
  direto) → Jogando (o cronômetro corre, os abates contam) → Fim de Rodada (Tempo do Fim de Rodada) → a contagem da
  próxima rodada ou Fim de Jogo. **Set Match Phase** vai direto (Fim de Rodada sem resultado = rodada perdida; de Fim de
  Rodada, Jogando começa a próxima).
- **Rodadas**: melhor de N: vence a partida quem passa da metade (ou, na última, quem ganhou mais; empate = derrota).
  Cada rodada nova: cronômetro, resultado e eliminação zerados, vidas cheias, os jogadores de volta (Zerar Pontos a Cada
  Rodada opcional).
- **Times** (`teams`): 0 cada um por si (o alvo é de cada jogador), 1 um time cooperativo (os pontos dos jogadores somam;
  um bot que pontua joga sozinho, time -1: um inimigo que mata o jogador não pontua para os jogadores), 2-4 times em
  rodízio pelo slot.
- **Pontos**: o `health_event` do `bp_vm.c` chama `rules_on_death` antes do On Death do ator: quem matou ganha Pontos por
  Abate, ou os da primeira **Tag da Vítima** que casa (`gt_has`, hierárquica). Só em Jogando. **Add Score** (negativo
  tira) → On Score Changed e o alvo conferido na hora.
- **Vitória e derrota** (o resultado é do ponto de vista do jogador local: ganhou o slot dele ou o time dele):
  Pontuação Alvo; Eliminação (todos os atores com a Tag a Eliminar mortos ou destruídos — só depois de ter visto algum
  vivo; conferido na hora de uma morte e a cada 0,25 s para os criados e destruídos); Sem Vidas / Por Mortes (o jogador
  sai; todos os jogadores fora = derrota; com vários jogadores, o último de pé ou o último time vence); Quando o Tempo
  Acabar (nada, derrota, vitória por sobreviver, mais pontos com empate = derrota); **End Match** (Won, Reason, Winner):
  o objetivo manual, direto para o Fim de Jogo.
- **Vidas e renascer**: uma morte tira uma vida; com Renascer, depois do Atraso o jogador volta no **Player Start mais
  perto de onde ele começou** (vários jogadores: cada um no seu lado) ou, com No Último Checkpoint e um alcançado, por
  `progress_respawn` (sem recarregar o save); revivido (`health_revive`), velocidade zerada, invencível pelos segundos
  (o Invencível da própria Vida volta depois). Destruído (Ao Morrer: Destruir) → um novo da classe dele, possuído se era o
  jogador. Um Checkpoint com Respawn on Death que trouxe o jogador antes: o das regras não repete. **Add Lives** traz de
  volta um jogador que tinha saído. O começo de cada rodada também dá a invencibilidade (proteção de spawn).
- **Cronômetro**: decrescente a partir do Tempo Limite da Rodada (ou crescente, o limite opcional); não anda com
  `game_paused()` (Set Game Paused, o menu de pausa), nem com **Set Match Timer** (Paused). Set Match Timer: decrescente =
  o que falta; crescente = o que passou. No limite: On Timer Expired, depois a regra do tempo.
- **Eventos** (em todo Blueprint, por `bp_fire_event`; nos scripts `OnMatchPhaseChanged`, `OnScoreChanged`,
  `OnMatchWon`/`OnVictory`, `OnMatchLost`/`OnDefeat`, `OnRoundStarted`, `OnRoundEnded`, `OnPlayerRespawned`,
  `OnTimerExpired`/`OnTimeUp`): o primeiro On Match Phase Changed é o Aguardando do começo. Ordem no fim: Phase Changed,
  Round Ended, Won/Lost.
- **Nós** (Game|Game Rules): Get/Set Match Phase, Start Match, End Match, Add Score, Get Score (+ Kills, Deaths, Team),
  Get Team Score (+ Rounds Won), Get Time Remaining (+ Elapsed, Text "1:05"), **Set Match Timer** (o nome "Set Timer" já é
  o Set Timer by Event da Unreal), Get Lives (+ Infinite, Out), Add Lives, Respawn Player, Restart Match (Reload Level:
  o nível aberto de novo), Get Match Result (Won, Reason, Winner, Is Over). O jogador é o pino **Target** (nos scripts o
  self da chamada: `AddScore(25)`, `bot.AddScore(25)`); desligado ele lê o self (como todo ator nos nós): um self que não é jogador (o widget, o Game Mode) vira o jogador local (`gr_player` no `bp_vm.c`); o
  mesmo no Winner do End Match.
- **Widgets** (Content/UI, feitos quando faltam: ao pôr o ator, ao adicionar o componente, ao começar o Play com
  Mostrar): **WBP_MatchHUD** (faixa no alto: tempo, pontos, vidas — some com infinitas —, rodada só com mais de uma; a
  contagem grande no meio; "Fim de Rodada"), **WBP_MatchEnd** (Vitória ou Derrota, o motivo, os pontos, Jogar de Novo =
  Restart Match, Menu Principal = Return to Main Menu; o cursor aparece e volta no Restart). Grafos de nós comuns (Event
  Tick → Get Time Remaining / Get Score / Get Lives / Get Match Phase / Get Match Result → Set Text / Set Widget Visible):
  o usuário abre e muda. Os textos (fases, motivos) passam pelo `gus_tr`: aparecem traduzidos.

## Online
O servidor (ou o jogo sozinho) decide. A cada tick de rede do servidor, quando `rules_net_serial()` mudou e uma vez por
segundo (o cronômetro), vai a mensagem **M_RULES** confiável para todos (e a um jogador que entra: o próximo tick): fase,
rodada, tempo da fase, cronômetro e limite, resultado, pontos e rodadas dos times, e cada slot (jogador da rede, time,
pontos, abates, mortes, vidas, renascendo) — **87 bytes** com 2 jogadores. O cliente (`rules_net_read`) mapeia os slots
pelo `online_player_pawn`, anda o cronômetro sozinho entre as mensagens e dispara os mesmos eventos pelo que mudou (fase,
pontos, renascer, tempo esgotado, o fim). No cliente Add Score, Start Match, End Match... não fazem nada (como o Game Mode
da Unreal, que só existe no servidor). Os pawns têm slots por `online_player_pawn` no servidor.

## Save
Chunk **RULE** no Save Progress (`save_progress.c`: depois do DLGM): versão 1, fase, rodada, tempos, resultado, times e o
slot do jogador local (pontos, abates, mortes, vidas, fora, renascendo) — **69 bytes**. Lido com limites; um chunk cortado
lê o que tem (uma linha no log). No load o nível abre antes; o chunk espera a partida achar o componente (primeiro quadro)
e entra por cima, com o Phase Changed da fase salva. Versão maior que a nossa: a partida começa do zero.

## Medidas (2026-11-09, RTX 3060, 2560×1417, Release com validação)
| Caso | Resultado |
|---|---|
| Autoteste: 1000 mortes (dano da Vida + as regras: pontos por tag, eventos) | 0,04–0,08 ms |
| `rules_tick` jogando, 303 atores com a Tag a Eliminar (a busca a cada 0,25 s) | **0,0006–0,0010 ms/quadro** (pior quadro 0,05 ms) |
| Sem Game Rules no nível | um teste de máscara por arquétipo |
| Mensagem de rede / chunk do save | 87 / 69 bytes |
| Teste `rules` (editor com validação, HUD, 303 atores marcados) | 376–523 fps |
O Debug Viewer (F10) tem a lista **Game Rules**: o tempo do quadro, a fase, a rodada, o relógio, quantos atores marcados
vivos, e cada jogador com pontos, time, vidas e mortes.

## Teste `rules` (`--rules-test`, projeto novo RULES, `--bench 10 --min-fps 200`)
1. `rules_selftest` (mundo sem VM): as fases e seus tempos (Aguardando 1 s, a contagem em 1 s, Jogando, Round Started),
   vencer por pontos (abate 10, chefe pela tag 40, Score Changed, o time, Fim de Rodada, Fim de Jogo, Won), perder por
   tempo (Restart, sem contagem, 4 s, pausado não anda, Timer Expired, "Time Up"), vidas (2, invencível no começo,
   morto: o renascer espera 1,5 s, no Player Start com o yaw, invencível 1 s, Add Lives, sem vidas = derrota), um inimigo
   que mata pontua sozinho, eliminação por tag em melhor de 3 (Zerar Pontos, Set Match Phase, End Match), o chunk do save
   de ida e volta (e cortado), a mensagem de rede lida como cliente (estado e eventos; o cliente não decide), 1000 mortes
   com orçamento.
2. Jogado: BP_GameMode com o Game Rules nos padrões da classe e os 8 eventos contados em variáveis; a contagem no HUD,
   Jogando, o cronômetro parado pelo Set Game Paused, um abate (10 no HUD), o jogador morto (1 vida no HUD) e de volta
   no Player Start, o alvo (Fim de Rodada, Fim de Jogo, Vitória no WBP_MatchEnd, o motivo, o cursor), Jogar de Novo
   (pontos 0, vidas 2), Set Match Timer 1 s → Timer Expired → Derrota; a medida com 303 marcados (< 0,05 ms).

## Limites / próximos passos
- Online: o renascer de um pawn remoto move o pawn no servidor, mas o pawn de um cliente tem a autoridade dele (o
  servidor corrige o salto pela validação de velocidade); um renascer "do servidor" com teleporte confiável ainda não.
  O teste de rede é o de ida e volta da mensagem no autoteste (sem dois processos).
- Na contagem os jogadores podem andar (sem travar a entrada).
- Os times são por rodízio de slot; não há Set Team nem escolha de time.
- A eliminação olha atores com Gameplay Tags (os da Crowd/multidão não entram).
