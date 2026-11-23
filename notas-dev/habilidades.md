# Habilidades e efeitos (módulo 9 do R7)

Pedido (plano de melhorias, R7 módulo 9): o Gameplay Ability System da Unreal numa versão mais simples, feita do zero,
sobre as Gameplay Tags (D5) e a Vida (R7.1).

Código: `src/ability.h/.c` (definições, sistemas e efeitos em SoA, regras, eventos, registros do save, Debug Viewer,
WBP_AbilityBar, autoteste — no `re_render`), `src/ability_widget.c` (os widgets Ability Slot e Active Effects do UMG,
no editor e no jogo), `src/ability_ui.c` (Details), nós em `bp_internal.h`/`blueprint.c`/`bp_vm.c` (`abil_native` no
fim), eventos dos scripts em `script_lower.c`, o chunk `ABIL` em `save_progress.c`, Damage/Defense pelo
`health_set_damage_scale` do `health.c`, teste `src/ability_test.c`.

## Como funciona
- **Gameplay Ability** = classe Blueprint com pai nativo **Gameplay Ability** (`BPN_GAMEPLAY_ABILITY`; Content Browser >
  Adicionar > Habilidade de Jogo, ou Blueprint Class > Todas as Classes). Nasce com as variáveis da categoria Ability
  (os padrões da classe, como na Unreal): Display Name, Description, Icon, Cost Attribute (`Mana`), Cost, Cooldown,
  Charges, Cast Time, Ability Tags, Activation Owned Tags, Blocked By Tags (`State.Stunned`), Cancel Abilities With Tags,
  Self Effect — e o grafo **Event On Activate Ability (Owner) → End Ability**. Lidas por nome solto (sem espaço e caixa:
  `CostAttribute` serve), também nos pais e nas classes de script (`class X extends GameplayAbility`, `var Cooldown`).
- **Objeto da habilidade**: na primeira ativação de uma habilidade com Event On Activate Ability, um ator sem lugar da
  classe é criado para o dono (`bp_spawn`, como o Create Save Game Object); o Event On Activate Ability roda nele;
  **Get Ability Owner** devolve o dono; **End Ability** (Target vazio: self) termina. Sem esse evento: termina ao agir.
- **Gameplay Effect** = Data Asset com pai **Gameplay Effect** (Adicionar > Efeito de Jogo): Display Name, Description,
  Icon, Is Debuff, Duration Policy (`Instant`, `Has Duration`, `Infinite`), Duration, Period, Modifiers, Damage, Damage
  Type, Heal, Max Stacks, Refresh Duration, Granted Tags, Required Tags.
  - **Modifiers** em texto: `Speed x1.5, Defense +10, Mana -5, Damage =2, Armor /2` (até 4; `x`/`*` multiplica, `/`
    divide, `=` sobrescreve, `+`/`-` soma).
  - **Instant**: os modificadores no valor **base** uma vez (e o Damage/Heal). **Has Duration / Infinite** sem período:
    os modificadores no valor **atual** enquanto ativo: atual = (base + Σ somas × acúmulos) × Π mult^acúmulos, ou o
    último override. **Com Period**: roda na aplicação e a cada período (modificadores no base, Damage via
    `health_apply_damage` com o Source de instigador, Heal via `health_heal`) — uma regeneração é `Infinite`, Period
    0,5, `Mana +5`. Sem período mas com Damage/Heal: uma vez por aplicação.
  - **Acúmulo**: Max Stacks 0 = cada aplicação é um efeito; N ≥ 1 = um efeito com até N acúmulos (Refresh Duration
    reinicia o tempo). Um acúmulo novo não roda o período na hora.
  - **Required Tags**: só aplica num alvo com todas; **Granted Tags** ficam no alvo enquanto ativo (contadas: dois
    efeitos com `State.Burning`, a tag sai com o último; uma tag que o ator já tinha não sai).
- **Ability System** (`CAbilitySystem`, 120 bytes: cabe nos componentes da classe; Details > Adicionar > Sistema de
  Habilidades): Starting Attributes (`Mana 100, MaxMana 100`), até 6 Starting Abilities (as teclas 1-6 do jogador com
  Activate with Keys 1-6), até 4 Starting Effects, Show Ability Bar. Um efeito ou Give Ability num ator sem o
  componente cria um vazio.
- **Atributos**: base e atual por sistema (até 16). **Speed** é a Max Walk Speed do Character (criado sozinho num
  Character, escrito de volta a cada mudança); **Damage** multiplica o dano que o ator causa (padrão 1); **Defense**
  reduz o que ele leva: × 100 / (100 + Defense) (negativa aumenta: 2 − 100 / (100 − D)) — os dois pelo
  `health_set_damage_scale`, antes dos multiplicadores de tipo da Vida. `X` fica entre 0 e `MaxX` quando há `MaxX`.
- **Ativar** (Try Activate Ability; por tag; tecla; clique no WBP_AbilityBar): concedida? já rodando (Active)? dono
  morto pela Vida (Dead)? alguma Blocked By Tag (Blocked)? carga (Cooldown)? custo (Cost)? Então paga o custo (no base),
  gasta uma carga (a recarga corre: volta uma a cada Cooldown, até Charges), cancela as que rodam com as Cancel tags,
  dá as Activation Owned Tags e, depois do Cast Time (cancelado se uma Blocked By Tag aparecer durante), age: Self
  Effect, On Ability Activated, Event On Activate Ability.
- **Eventos** (no dono e nos Actor Components dele, `bp_fire_actor_event`): On Ability Activated (Ability), On Ability
  Ended (Ability, Was Cancelled; também no objeto da habilidade), On Effect Applied (Effect, Stacks, Source), On Effect
  Removed (Effect), On Attribute Changed (Attribute, New Value, Old Value). Ficam numa fila fixa (2048) e saem no fim da
  operação (o nó que a chamou) ou do quadro: nada de código do jogo no meio das varreduras. Nos scripts:
  `ActivateAbility`/`OnActivateAbility`, `OnAbilityActivated`, `OnAbilityEnded`, `OnEffectApplied`, `OnEffectRemoved`,
  `OnAttributeChanged`.
- **Memória (SoA, sem malloc no quadro)**: sistemas em arrays fixos (4096: atributos, concessões, tags contadas, lista
  de efeitos); efeitos num array SoA só (16384: sistema, definição, acúmulos, tempo, período, fonte) com uma lista
  duplamente ligada por sistema (remoção O(1): o último vai para o lugar). O `abil_tick` percorre os efeitos em
  sequência (tempo, períodos devidos numa lista, expirados removidos), depois os sistemas (recargas, conjurações,
  teclas), depois roda o que chama código do jogo (dano dos períodos, ativações), recalcula só os sistemas marcados e
  envia os eventos. Pausado (Set Game Paused): nada corre.

## Nós (15) e eventos (6)
Give Ability, Remove Ability, Try Activate Ability (Success, Failure Reason), Try Activate Ability by Tag, Cancel
Ability (nome ou tag; vazio: todas), End Ability, Get Ability Owner, Is Ability On Cooldown, Get Cooldown Remaining
(restante, duração, cargas, máximo), Is Ability Active, Apply Gameplay Effect (Target, Effect, Source → Success,
Stacks), Remove Gameplay Effect (nome ou tag concedida; Stacks 0: tudo), Has Gameplay Effect (Stacks, Time Remaining;
−1 infinito), Get Attribute (atual, base, Found), Set Attribute (o base). Target vazio = self; dentro de uma habilidade,
o dono. Eventos: Event On Activate Ability (Owner) e os cinco acima.

## Widgets
- **Ability Slot** (`UIW_ABILITY_SLOT`, paleta Abilities): o ator (vazio: o jogador) e o número do espaço; ícone (ou um
  ladrilho com as iniciais na cor do nome), recarga como varredura escura no sentido horário com os segundos quando sem
  carga, anel da conjuração, borda de destaque enquanto roda, azul quando falta custo, cargas embaixo à direita, a tecla
  em cima à esquerda; o clique ativa (lido contra os retângulos do quadro anterior, como a Grade de Inventário).
- **Active Effects** (`UIW_EFFECT_LIST`): uma linha por efeito: faixa verde (buff) ou vermelha (debuff), ícone, nome com
  `x3`, segundos e a barra do tempo (infinito: sem tempo).
- **WBP_AbilityBar** (`abil_bar_asset`, Content/UI, criado quando falta: ao adicionar o componente, pelo menu Adicionar >
  Interface do Usuário, ou no Play com Show Ability Bar): moldura embaixo no centro com 6 Ability Slots e os Active
  Effects no canto de cima à esquerda. Sem grafo: os widgets leem o Ability System. O usuário copia e muda.

## Save
Chunk **ABIL** no `.sav` do Save Progress (versão própria 1; um save sem ele abre igual, e um com versão maior é
ignorado): por ator com chave (os Saveables do nível com Ability System e o jogador, `ABIL_PLAYER_KEY`), os atributos
pelo nome (base), os efeitos ativos (hash, tempo restante, período restante, acúmulos) e as habilidades (hash, cargas,
recarga). No load os do nível voltam no `apply_level` (antes do BeginPlay) e o do jogador no `apply_late`; um efeito
que saiu do projeto é pulado. Instantâneos não ficam ativos (o efeito deles já está no base). Os atores criados no jogo
(sem chave) ainda não levam o registro.

## Debug Viewer
Lista **Abilities and Effects**: o `abil_tick`, sistemas, efeitos ativos, habilidades rodando, aplicações, execuções de
período, ativações e falhas desde o Play, e cada sistema com efeitos, habilidades e os dois primeiros atributos.

## Medidas (2026-11-09, RTX 3060, 2560×1417, Release)
| Caso | Resultado |
|---|---|
| 1000 atores × 5 efeitos (dano periódico 0,5 s, Speed x1.5, Defense +100, regeneração periódica, atordoado com tag), 600 quadros | **0,016–0,027 ms/quadro** (pior quadro 0,25–0,53 ms, os períodos coincidindo: 1000 danos pela Vida) |
| Aplicar os 5000 efeitos | 3,9–7,4 ms no total, 0,8–1,5 µs cada |
| `abil_tick` no teste jogado (2 sistemas) | 0,0006 ms |
| Teste `ability` (editor com validação, a barra na tela) | 315 fps (125 com a GPU dividida com outras sessões) |

## Teste `ability` (`--ability-test`)
1. `abil_selftest` (sem a VM): custo e recarga (2 s, 1 s depois falta 1 s, Mana 100 → 70 → 40 → 10, recusado pelo
   custo), cargas (duas esquivas, a terceira em recarga, uma volta por segundo), bloqueio por tag (o atordoamento dá
   `State.Stunned`, bloqueia, sai com o efeito), conjuração (0,5 s; interrompida pelo atordoamento; depois age),
   veneno periódico pela Vida (10 na hora, 10 por segundo, acúmulos 2, 3, 3; 30 por segundo com 3; some com a tag),
   Speed x1.5 no Character (600 → 900 → 600), Defense 100 (20 → 10), Damage com 2 acúmulos, Instant no base,
   regeneração presa no MaxMana, cancelamento por tag, ativar por tag, o registro do save de volta, 1000 × 5 medidos.
2. Jogado: GA_Fireball (o grafo aplica GE_Poison no BP_AbilDummy e chama End Ability), GA_Haste (Self Effect GE_Haste),
   BP_AbilHero com o Ability System (Show Ability Bar): Try Activate duas vezes (Success; Failure Reason Cooldown),
   atordoado → Blocked, o clique no primeiro espaço do WBP_AbilityBar lança de novo (veneno com 2 acúmulos), Haste →
   450 de velocidade, o veneno pela Vida, Save Progress → tudo mudado → Load Progress (abre o nível de novo): Mana,
   o Haste com o tempo, a velocidade, a recarga do Haste, os acúmulos do veneno e a tag de volta; os eventos contados.

## Limites / próximos passos
- Sem replicação online (o servidor aplica e replica o que o jogo precisar, como na Vida).
- Sem "magnitude por nível", curvas ou atributos calculados por fórmula; os modificadores são números fixos.
- O Cast Time não toca animação sozinho (use o Event On Activate Ability ou um Montage no On Ability Activated).
- Teclas fixas 1-6 para o jogador; o Enhanced Input ainda não chama habilidades sozinho (use Try Activate Ability).
- O objeto da habilidade é um por dono e habilidade (não instanciado por ativação).
